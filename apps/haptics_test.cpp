// Spidy's body haptics on bHaptics devices, without the game: whether the
// bHaptics Player and the devices are there, which motor is where, and every
// pattern the game plays, each named with what it should feel like. It plays
// through the game's own path (body_haptics::Mixer, bhaptics::Output).
//
//   spidy_haptics_test [check] [map] [effects] [stream]   (none: all four)
//     --only NAME        effects: just the one named (web_shot, swing, ...)
//     --step             map: wait for Enter after each place
//     --level N          strength in percent (100)
//     --dry              no Player: the frames as text, 0 (.) to 9
//     --app-id ID --api-key KEY
//                        the bHaptics app, if not this build's (also the
//                        SPIDY_BHAPTICS_APP_ID and SPIDY_BHAPTICS_API_KEY variables)
#include "spidy/bhaptics_output.hpp"
#include "spidy/body_haptics.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
using namespace spidy;
using namespace spidy::body_haptics;
namespace {
constexpr float pi = 3.14159265f;
// Where the patterns go: the devices, or the console.
class Sink {
  public:
    virtual ~Sink() = default;
    virtual void post(const Event&) = 0;
    virtual void set(const State&) = 0;
    virtual void hold(const Frame&, float seconds) = 0;
    virtual void wait(double seconds) = 0;
};
class Devices final : public Sink {
  public:
    explicit Devices(bhaptics::Output& out) : out_(out) {}
    void post(const Event& e) override {
        out_.post(e);
    }
    void set(const State& s) override {
        state_ = s;
        out_.set(s);
    }
    void hold(const Frame& f, float seconds) override {
        out_.hold(f, seconds);
    }
    // The state again and again while it waits, as the game's frame loop
    // gives it every frame: it lasts 300 ms.
    void wait(double seconds) override {
        using clock = std::chrono::steady_clock;
        const auto end = clock::now() + std::chrono::duration_cast<clock::duration>(
                                            std::chrono::duration<double>(seconds));
        for (auto now = clock::now(); now < end; now = clock::now()) {
            out_.set(state_);
            std::this_thread::sleep_for(std::min<clock::duration>(end - now, std::chrono::milliseconds(20)));
        }
    }

  private:
    bhaptics::Output& out_;
    State state_;
};
// The frames as the output thread would make them, a line for each tick with
// a motor on: the vest's front and back (four rows, the wearer's left first),
// the left and right sleeve, the visor.
class Console final : public Sink {
  public:
    explicit Console(int level) : level_(level) {}
    void post(const Event& e) override {
        mixer_.post(e, now_);
    }
    void set(const State& s) override {
        mixer_.set(s);
    }
    void hold(const Frame& f, float seconds) override {
        mixer_.hold(f, now_, seconds);
    }
    void wait(double seconds) override {
        for (double end = now_ + seconds; now_ < end - 1e-9; now_ += .04) {
            const auto f = mixer_.render(now_);
            bool any = false;
            for (unsigned d = 0; d < deviceCount; ++d)
                any = any || f.any(static_cast<Device>(d));
            if (any)
                std::printf("%7.0f ms  %s\n", (now_ - since_) * 1000, line(f).c_str());
        }
    }
    void mark() {
        since_ = now_;
    }

  private:
    // A motor as the output thread sends it, in tenths rounded up (1: 3-10%,
    // 2: 11-20% ... 9: over 80%); '.' off, or under bhaptics::minimumLevel.
    char digit(float v) const {
        const auto percent = static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * static_cast<float>(level_)));
        return percent < bhaptics::minimumLevel ? '.' : static_cast<char>('0' + std::clamp((percent + 9) / 10, 1, 9));
    }
    std::string line(const Frame& f) const {
        std::string out;
        for (const bool back : {false, true}) {
            out += back ? " B[" : "F[";
            for (unsigned row = 0; row < 4; ++row) {
                for (unsigned side = 0; side < 4; ++side)
                    out += digit(f.at(Device::vest, vestMotor(back, row, side)));
                out += row < 3 ? "|" : "]";
            }
        }
        for (const auto& [label, device] : {std::pair{" L[", Device::leftArm}, std::pair{" R[", Device::rightArm}}) {
            out += label;
            for (unsigned m = 0; m < 3; ++m)
                out += digit(f.at(device, m));
            out += ']';
        }
        out += " H[";
        for (unsigned i = 0; i < 4; ++i)
            out += digit(f.at(Device::head, visorMotor(i)));
        return out + ']';
    }
    Mixer mixer_;
    double now_{}, since_{};
    int level_;
};

State live() {
    State s;
    s.live = true;
    return s;
}
Event at(Kind kind, int hand = -1, float strength = 1, float azimuth = 0, float height = .4f) {
    Event e;
    e.kind = kind;
    e.hand = static_cast<int8_t>(hand);
    e.strength = strength;
    e.azimuth = azimuth;
    e.height = height;
    return e;
}
// Changes the state over `seconds`, `apply` given 0..1 along the way.
void sweep(Sink& sink, double seconds, const std::function<void(State&, float)>& apply) {
    const int steps = static_cast<int>(seconds / .04);
    for (int i = 0; i <= steps; ++i) {
        State s = live();
        apply(s, static_cast<float>(i) / static_cast<float>(steps));
        sink.set(s);
        sink.wait(.04);
    }
    sink.set(live());
}

struct Demo {
    const char* name;
    const char* feel;
    std::function<void(Sink&)> play;
};
const std::vector<Demo>& demos() {
    static const std::vector<Demo> all = {
        {"web_shot", "a light tick in your LEFT forearm, then your RIGHT",
         [](Sink& s) {
             s.post(at(Kind::webShot, 0));
             s.wait(.5);
             s.post(at(Kind::webShot, 1));
             s.wait(.5);
         }},
        {"web_attach", "a web takes hold: a tick in the arm, then that shoulder (left, then right)",
         [](Sink& s) {
             s.post(at(Kind::webAttach, 0));
             s.wait(.6);
             s.post(at(Kind::webAttach, 1));
             s.wait(.6);
         }},
        {"swing", "a swing on the right hand: the pull in the arm and shoulder grows from nothing to the bottom "
                  "of the swing and eases off; the web lets go, and a faint wind meets your chest in the flight",
         [](Sink& s) {
             s.post(at(Kind::webAttach, 1));
             s.wait(.2);
             sweep(s, 2.4, [](State& st, float u) {
                 st.webTension[1] = 10 + 38 * std::sin(u * pi);
                 st.airborne = true;
                 st.velocity = {0, -6 * std::cos(u * pi), 18 + 16 * std::sin(u * pi)};
             });
             s.post(at(Kind::webRelease, 1));
             sweep(s, 1.6, [](State& st, float) {
                 st.airborne = true;
                 st.velocity = {0, 4, 32};
             });
             s.wait(.4);
         }},
        {"zip", "yanked along a web ahead of you: the right arm, a spot on your chest, then your face",
         [](Sink& s) {
             s.post(at(Kind::zip, 1));
             s.wait(.6);
         }},
        {"wind", "a fall: a faint wind round your waist, growing; then flying ahead: on your chest and face",
         [](Sink& s) {
             sweep(s, 2.5, [](State& st, float u) {
                 st.airborne = true;
                 st.velocity = {0, -(10 + 40 * u), 0};
             });
             sweep(s, 2.5, [](State& st, float u) {
                 st.airborne = true;
                 st.velocity = {0, 0, 10 + 40 * u};
             });
             s.wait(.3);
         }},
        {"landing", "a soft landing (a jump), then a hard one (a fall from a roof): from the waist up",
         [](Sink& s) {
             s.post(at(Kind::landing, -1, .2f));
             s.wait(.8);
             s.post(at(Kind::landing, -1, 1));
             s.wait(.8);
         }},
        {"walls", "onto a wall ahead: a spot on your chest and both arms; then onto one on your RIGHT: your right "
                  "side; then a jump off it",
         [](Sink& s) {
             s.post(at(Kind::wallLand));
             s.wait(.8);
             s.post(at(Kind::wallLand, -1, 1, pi / 2));
             s.wait(.8);
             s.post(at(Kind::wallJump, -1, 1, pi / 2));
             s.wait(.8);
         }},
        {"leap", "a charged leap: building round the waist and in the arms, then up and away",
         [](Sink& s) {
             s.post(at(Kind::leapCharge));
             sweep(s, 1.5, [](State& st, float u) { st.charge = u; });
             s.post(at(Kind::leap));
             s.wait(.8);
         }},
        {"punches", "a light jab (left), a firm punch (right), a heavy blow (right), a finisher (whole body)",
         [](Sink& s) {
             s.post(at(Kind::punch, 0, .2f));
             s.wait(.6);
             s.post(at(Kind::punch, 1, .9f));
             s.wait(.6);
             s.post(at(Kind::heavyBlow, 1));
             s.wait(.8);
             s.post(at(Kind::finisher, 1));
             s.wait(1);
         }},
        {"spider_sense", "Spider-Sense: a tingle in the visor and a spot at your shoulders from your LEFT, your "
                         "RIGHT, BEHIND you; then one to dodge (harder, longer) from AHEAD",
         [](Sink& s) {
             for (const float az : {-pi / 2, pi / 2, pi}) {
                 s.post(at(Kind::spiderSense, -1, 1, az));
                 s.wait(.9);
             }
             s.post(at(Kind::senseDodge, -1, 1, 0));
             s.wait(1);
         }},
        {"hits", "hit from AHEAD, your LEFT, BEHIND, your RIGHT, then a heavy one from ahead (visor and arms too)",
         [](Sink& s) {
             for (const float az : {0.f, -pi / 2, pi, pi / 2}) {
                 s.post(at(Kind::hit, -1, .25f, az, .4f));
                 s.wait(.8);
             }
             s.post(at(Kind::hit, -1, 1, 0, .4f));
             s.wait(.8);
         }},
        {"heal", "warmth spreading out from the middle of your chest, then round your back",
         [](Sink& s) {
             s.post(at(Kind::heal));
             s.wait(1.2);
         }},
        {"slow_motion", "slow motion: a deep pulse, a slow heartbeat left of your chest's middle, a short release",
         [](Sink& s) {
             s.post(at(Kind::slowStart));
             sweep(s, 3.5, [](State& st, float) { st.slowMotion = true; });
             s.post(at(Kind::slowStop));
             s.wait(.5);
         }},
        {"low_health", "low health: the heart racing",
         [](Sink& s) {
             sweep(s, 3.5, [](State& st, float) { st.health = .15f; });
             s.wait(.3);
         }},
        {"grab", "the web catches a thug (left), yanks him, holds him pulling, then you throw him",
         [](Sink& s) {
             s.post(at(Kind::grabCaught, 0));
             s.wait(.4);
             s.post(at(Kind::grabYanked, 0));
             s.wait(.4);
             s.post(at(Kind::grabHeld, 0));
             sweep(s, 1.5, [](State& st, float u) { st.grabTension[0] = .3f + .7f * u; });
             s.post(at(Kind::grabLetGo, 0));
             s.wait(.5);
         }},
        {"fight", "a web pull, a thug pulled in in the air, a web strike's rush, a counter, a knockout",
         [](Sink& s) {
             s.post(at(Kind::webPull));
             s.wait(.8);
             s.post(at(Kind::airPull));
             s.wait(.6);
             s.post(at(Kind::strikeRush));
             s.wait(.7);
             s.post(at(Kind::counter));
             s.wait(.6);
             s.post(at(Kind::knockout));
             s.wait(.7);
         }},
        {"dodges", "a dodge to your left, then a perfect dodge to your right: a whoosh across",
         [](Sink& s) {
             s.post(at(Kind::dodge, -1, 1, -1));
             s.wait(.6);
             s.post(at(Kind::perfectDodge, -1, 1, 1));
             s.wait(.7);
         }},
        {"interface", "calibrated (the whole body twice), a scan (across the visor), the gadget wheel (left arm), "
                      "the phone (right arm)",
         [](Sink& s) {
             s.post(at(Kind::calibrated));
             s.wait(1);
             s.post(at(Kind::scan));
             s.wait(.6);
             s.post(at(Kind::gadget, 0, .3f));
             s.wait(.3);
             s.post(at(Kind::gadget, 0, .15f));
             s.wait(.3);
             s.post(at(Kind::gadget, 0, .5f));
             s.wait(.5);
             s.post(at(Kind::phone, 1));
             s.wait(.6);
         }},
    };
    return all;
}

// One place at a time: what should be felt where.
void map(Sink& sink, bool step, bool dry) {
    struct Place {
        std::string what;
        Frame frame;
    };
    std::vector<Place> places;
    const float strength = .6f;
    for (const bool back : {false, true})
        for (const unsigned rows : {0u, 2u})
            for (const unsigned sides : {0u, 2u}) {
                Place p;
                p.what = std::string("vest ") + (back ? "BACK" : "FRONT") + ", " + (rows ? "lower" : "upper") + " " +
                         (sides ? "RIGHT" : "LEFT") + " (your " + (sides ? "right" : "left") + ")";
                for (unsigned row = rows; row < rows + 2; ++row)
                    for (unsigned side = sides; side < sides + 2; ++side)
                        p.frame.at(Device::vest, vestMotor(back, row, side)) = strength;
                places.push_back(p);
            }
    for (const unsigned side : {0u, 1u, 2u, 3u}) {
        Place p;
        p.what = "vest, one column front and back, " + std::to_string(side + 1) + " of 4 from your LEFT";
        for (unsigned row = 0; row < 4; ++row)
            for (const bool back : {false, true})
                p.frame.at(Device::vest, vestMotor(back, row, side)) = strength;
        places.push_back(p);
    }
    for (const unsigned hand : {0u, 1u}) {
        Place whole;
        whole.what = std::string(hand ? "RIGHT" : "LEFT") + " sleeve, all three motors";
        for (unsigned m = 0; m < 3; ++m)
            whole.frame.at(arm(hand), m) = strength;
        places.push_back(whole);
        for (unsigned m = 0; m < 3; ++m) {
            Place one;
            one.what = std::string(hand ? "RIGHT" : "LEFT") + " sleeve, motor " + std::to_string(m) +
                       " alone (where on the arm?)";
            one.frame.at(arm(hand), m) = strength;
            places.push_back(one);
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        Place p;
        p.what = "visor, motor " + std::to_string(i + 1) + " of 4 from your LEFT temple";
        p.frame.at(Device::head, visorMotor(i)) = strength;
        places.push_back(p);
    }
    sink.set(live());
    for (const auto& p : places) {
        std::cout << "  " << p.what << std::endl;
        sink.hold(p.frame, .9f);
        sink.wait(1.5);
        if (step && !dry) {
            std::cout << "    (Enter for the next)" << std::flush;
            std::string ignored;
            std::getline(std::cin, ignored);
        }
    }
}
// A steady frame, then a slow ramp: whether frames sent a tick at a time feel
// like one steady buzz, or pulse.
void stream(Sink& sink) {
    sink.set(live());
    Frame whole;
    for (unsigned m = 0; m < 32; ++m)
        whole.at(Device::vest, m) = .4f;
    for (unsigned d = 1; d < deviceCount; ++d)
        for (unsigned m = 0; m < motorCount(static_cast<Device>(d)); ++m)
            whole.at(static_cast<Device>(d), m) = .4f;
    std::cout << "  every motor at 40% for 3 seconds: one steady buzz? (a pulse or a flicker: tell Spidy's maker)"
              << std::endl;
    sink.hold(whole, 3);
    sink.wait(3.4);
    std::cout << "  the whole vest from 10% up to 100% and down again over 4 seconds: smooth?" << std::endl;
    for (int i = 0; i <= 40; ++i) {
        const float u = static_cast<float>(i) / 40;
        Frame f;
        for (unsigned m = 0; m < 32; ++m)
            f.at(Device::vest, m) = .1f + .9f * std::sin(u * pi);
        sink.hold(f, .1f);
        sink.wait(.1);
    }
    sink.wait(.3);
}
const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}
} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> sections;
    std::string only, appId = bhaptics::builtAppId(), apiKey = bhaptics::builtApiKey();
    bool dry = false, step = false;
    int level = 100;
    if (const char* v = env("SPIDY_BHAPTICS_APP_ID"))
        appId = v;
    if (const char* v = env("SPIDY_BHAPTICS_API_KEY"))
        apiKey = v;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool value = i + 1 < argc;
        if (a == "--dry")
            dry = true;
        else if (a == "--step")
            step = true;
        else if (a == "--only" && value)
            only = argv[++i];
        else if (a == "--level" && value)
            level = std::clamp(std::atoi(argv[++i]), 0, 100);
        else if (a == "--app-id" && value)
            appId = argv[++i];
        else if (a == "--api-key" && value)
            apiKey = argv[++i];
        else if (a == "check" || a == "map" || a == "effects" || a == "stream")
            sections.push_back(a);
        else {
            std::cerr << "usage: spidy_haptics_test [check] [map] [effects] [stream] [--only NAME] [--step] "
                         "[--level N] [--dry] [--app-id ID --api-key KEY]\neffects:";
            for (const auto& d : demos())
                std::cerr << ' ' << d.name;
            std::cerr << '\n';
            return 2;
        }
    }
    if (sections.empty())
        sections = {"check", "map", "effects", "stream"};
    const auto wants = [&](const char* s) { return std::find(sections.begin(), sections.end(), s) != sections.end(); };

    bhaptics::Output out;
    Console console(level);
    Devices devices(out);
    Sink& sink = dry ? static_cast<Sink&>(console) : devices;
    int result = 0;
    if (!dry) {
        bhaptics::Config config;
        config.library = bhaptics::libraryBeside();
        config.appId = appId;
        config.apiKey = apiKey;
        std::wcout << L"bHaptics library: " << config.library << std::endl;
        std::cout << "bHaptics app: " << (appId.empty() ? "none - give --app-id and --api-key" : appId)
                  << (apiKey.empty() ? ", no API key" : ", API key set") << std::endl;
        out.start(config);
        out.level(level);
        out.set(live());
        // The Player connects in the background: up to fifteen seconds, then
        // a moment more for the devices to be asked for.
        bhaptics::Status shown = bhaptics::Status::off;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        for (;;) {
            const auto c = out.counters();
            if (c.status != shown) {
                shown = c.status;
                std::cout << "status: " << bhaptics::name(shown) << std::endl;
            }
            const bool done = c.status == bhaptics::Status::playing || c.status == bhaptics::Status::noLibrary ||
                              c.status == bhaptics::Status::noApp || c.status == bhaptics::Status::noPlayer ||
                              c.status == bhaptics::Status::failed || c.status == bhaptics::Status::rejected;
            if (done || std::chrono::steady_clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto c = out.counters();
        std::cout << "devices on: " << bhaptics::deviceNames(c.devices) << std::endl;
        switch (c.status) {
        case bhaptics::Status::noLibrary:
            std::cout << "bhaptics_library.dll is missing beside this program, or is not the bHaptics SDK's." << std::endl;
            return 2;
        case bhaptics::Status::noApp:
            std::cout << "This build has no bHaptics app: give --app-id and --api-key (the Developer Portal app's "
                         "Settings)." << std::endl;
            return 2;
        case bhaptics::Status::noPlayer:
            std::cout << "The bHaptics Player is not installed: bhaptics.com/software/player" << std::endl;
            return 1;
        case bhaptics::Status::failed:
            std::cout << "The bHaptics SDK would not start." << std::endl;
            return 1;
        case bhaptics::Status::rejected:
            std::cout << "The bHaptics Player did not take this app: give the workspace ID and API key from the "
                         "Developer Portal (not the app's own ID), and deploy the app with at least one event."
                      << std::endl;
            return 1;
        case bhaptics::Status::waiting:
            std::cout << "No answer from the bHaptics Player: is it running? Is the app deployed on the Developer "
                         "Portal, with at least one event?" << std::endl;
            return 1;
        case bhaptics::Status::connected:
            std::cout << "Connected, but no device is on: pair them in the bHaptics Player." << std::endl;
            result = 1;
            break;
        default:
            break;
        }
        if (wants("check")) {
            const auto info = out.deviceInfo();
            std::cout << "the Player's devices: " << (info.empty() ? "(nothing)" : info) << std::endl;
        }
    }
    auto* shown = dry ? &console : nullptr;
    if (wants("map")) {
        std::cout << "\nMAP: each place for a moment, at 60%. Say where you feel it, if it is not there." << std::endl;
        if (shown)
            shown->mark();
        map(sink, step, dry);
    }
    if (wants("effects")) {
        std::cout << "\nEFFECTS:" << std::endl;
        bool found = only.empty();
        for (const auto& d : demos()) {
            if (!only.empty() && only != d.name)
                continue;
            found = true;
            std::cout << "  " << d.name << ": " << d.feel << std::endl;
            if (shown)
                shown->mark();
            sink.set(live());
            d.play(sink);
            sink.wait(.4);
        }
        if (!found) {
            std::cerr << "no effect " << only << '\n';
            return 2;
        }
    }
    if (wants("stream")) {
        std::cout << "\nSTREAM:" << std::endl;
        if (shown)
            shown->mark();
        stream(sink);
    }
    if (!dry) {
        const auto c = out.counters();
        std::cout << "\nframes sent " << c.frames << ", refused " << c.refused << ", devices seen "
                  << bhaptics::deviceNames(c.devicesSeen) << std::endl;
        result = c.refused ? 1 : result;
        out.stop();
    }
    return result;
}

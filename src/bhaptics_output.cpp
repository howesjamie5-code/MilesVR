// Body haptics on bHaptics devices (bhaptics_output.hpp). The SDK's header
// only names its functions: they come from the library, loaded at run time.
#define BHAPTICS_DECLSPEC
#include <bhaptics/bhaptics.h>

#include "spidy/bhaptics_output.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <vector>
#include <windows.h>

#ifndef SPIDY_BHAPTICS_APP_ID
#define SPIDY_BHAPTICS_APP_ID ""
#endif
#ifndef SPIDY_BHAPTICS_API_KEY
#define SPIDY_BHAPTICS_API_KEY ""
#endif

namespace spidy::bhaptics {
using body_haptics::Device;
namespace {
// The SDK's functions Spidy calls, from the library.
struct Api {
    decltype(&::registryAndInit) registryAndInit{};
    decltype(&::wsIsConnected) wsIsConnected{};
    decltype(&::wsClose) wsClose{};
    decltype(&::bHapticsShutdown) shutdown{};
    decltype(&::playDot) playDot{};
    decltype(&::stop) stop{};
    decltype(&::stopAll) stopAll{};
    decltype(&::isbHapticsConnected) isbHapticsConnected{};
    decltype(&::isPlayerInstalled) isPlayerInstalled{};
    decltype(&::getDeviceInfoJson) getDeviceInfoJson{};
    decltype(&::getHapticMappingsJson) getHapticMappingsJson{};
    bool load(HMODULE m) {
        bool all = true;
        const auto get = [&](auto& f, const char* name) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(GetProcAddress(m, name));
            all = all && f;
        };
        get(registryAndInit, "registryAndInit");
        get(wsIsConnected, "wsIsConnected");
        get(wsClose, "wsClose");
        get(shutdown, "bHapticsShutdown");
        get(playDot, "playDot");
        get(stop, "stop");
        get(stopAll, "stopAll");
        get(isbHapticsConnected, "isbHapticsConnected");
        get(isPlayerInstalled, "isPlayerInstalled");
        get(getDeviceInfoJson, "getDeviceInfoJson");
        get(getHapticMappingsJson, "getHapticMappingsJson");
        return all;
    }
};
// Each device's frames play under a request id of its own ("SP" and the device).
constexpr int32_t streamId(Device d) {
    return 0x5350 + static_cast<int32_t>(d);
}
// The positions of the devices the Player calls connected, as bits 1 <<
// position, from its device list (getDeviceInfoJson: an object for each, with
// its "position" and "connected"). The SDK's own isbHapticsConnected said false
// for every position on October 20, while the list had a TactSuit Pro (0) and
// both TactSleeves (1, 2) connected and playDot was taken for each.
uint32_t connectedPositions(const char* json) {
    if (!json)
        return 0;
    const std::string_view text(json);
    // An object's value for a key: from its first character.
    const auto value = [](std::string_view object, std::string_view key) -> std::string_view {
        const size_t at = object.find(key);
        if (at == std::string_view::npos)
            return {};
        const size_t colon = object.find(':', at + key.size());
        const size_t start = colon == std::string_view::npos ? colon : object.find_first_not_of(" \t\r\n", colon + 1);
        return start == std::string_view::npos ? std::string_view{} : object.substr(start);
    };
    uint32_t bits{};
    for (size_t open = text.find('{'); open != std::string_view::npos; open = text.find('{', open + 1)) {
        const size_t close = text.find('}', open);
        if (close == std::string_view::npos)
            break;
        const auto object = text.substr(open, close - open);
        const auto position = value(object, "\"position\"");
        if (value(object, "\"connected\"").substr(0, 4) == "true" && !position.empty() && position[0] >= '0' &&
            position[0] <= '3' && (position.size() < 2 || position[1] < '0' || position[1] > '9'))
            bits |= 1u << (position[0] - '0');
        open = close;
    }
    return bits;
}
// Events waiting for the thread at most: it takes them twenty-five times a second.
constexpr size_t mostWaiting = 256;
// How often the devices on are asked for again while connected (ticks).
constexpr uint64_t devicePoll = 25;
// How long stop() waits for the thread to finish.
constexpr auto stopWait = std::chrono::seconds(3);
// A state lasts this long unless given again: the frame loop sends one every
// frame, and one that stops (the headset taken off) leaves nothing playing.
constexpr auto stateLease = std::chrono::milliseconds(300);
} // namespace

struct Output::Shared {
    Config config;
    std::mutex lock;
    std::condition_variable wake;
    bool stopping{}, finished{};
    Api api;
    bool loaded{}; // the api is there, and the SDK not shut down
    std::vector<body_haptics::Event> events;
    struct Held {
        body_haptics::Frame frame;
        float seconds{};
    };
    std::vector<Held> holds;
    body_haptics::State state;
    std::chrono::steady_clock::time_point stateAt{};
    int level = 100;
    Counters counters;
    void status(Status s) {
        std::lock_guard guard(lock);
        counters.status = s;
    }
    void run();
};

const char* name(Status s) {
    switch (s) {
    case Status::off:
        return "off";
    case Status::starting:
        return "starting";
    case Status::noLibrary:
        return "no_library";
    case Status::noApp:
        return "no_app";
    case Status::noPlayer:
        return "no_player";
    case Status::waiting:
        return "waiting";
    case Status::connected:
        return "connected";
    case Status::playing:
        return "playing";
    case Status::failed:
        return "failed";
    case Status::rejected:
        return "rejected";
    }
    return "unknown";
}
const char* builtAppId() {
    return SPIDY_BHAPTICS_APP_ID;
}
const char* builtApiKey() {
    return SPIDY_BHAPTICS_API_KEY;
}
std::wstring libraryBeside() {
    HMODULE self{};
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&libraryBeside), &self))
        return {};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return {};
    std::wstring folder(path, n);
    folder.resize(folder.find_last_of(L"\\/") + 1);
    return folder + L"bhaptics_library.dll";
}

std::string deviceNames(uint32_t devices) {
    constexpr const char* names[] = {"TactSuit", "left TactSleeve", "right TactSleeve", "TactVisor"};
    std::vector<const char*> on;
    for (unsigned d = 0; d < body_haptics::deviceCount; ++d)
        if (devices >> d & 1)
            on.push_back(names[d]);
    if ((devices & 6) == 6) {
        // Both sleeves are a pair.
        on.erase(std::remove(on.begin(), on.end(), names[2]), on.end());
        *std::find(on.begin(), on.end(), names[1]) = "both TactSleeves";
    }
    std::string text;
    for (size_t i = 0; i < on.size(); ++i)
        text += (i == 0 ? "" : i + 1 == on.size() ? " and " : ", ") + std::string(on[i]);
    return text.empty() ? "none" : text;
}

void Output::start(const Config& config) {
    if (shared_)
        return;
    shared_ = std::make_shared<Shared>();
    shared_->config = config;
    shared_->config.tickMs = std::clamp(config.tickMs, 10, 200);
    shared_->config.frameMs = std::clamp(config.frameMs, shared_->config.tickMs, 1000);
    if (config.appId.empty() || config.apiKey.empty()) {
        shared_->counters.status = Status::noApp;
        shared_->finished = true;
        return;
    }
    shared_->counters.status = Status::starting;
    thread_ = std::thread([shared = shared_] { shared->run(); });
}
void Output::stop() {
    if (!shared_)
        return;
    std::unique_lock guard(shared_->lock);
    shared_->stopping = true;
    shared_->wake.notify_all();
    const bool finished = shared_->wake.wait_for(guard, stopWait, [&] { return shared_->finished; });
    guard.unlock();
    if (thread_.joinable()) {
        if (finished)
            thread_.join();
        else
            thread_.detach(); // it holds the shared state, and the library stays loaded
    }
}
bool Output::started() const {
    return shared_ != nullptr;
}
void Output::level(int percent) {
    if (!shared_)
        return;
    std::lock_guard guard(shared_->lock);
    shared_->level = std::clamp(percent, 0, 100);
    shared_->counters.level = static_cast<uint32_t>(shared_->level);
}
void Output::post(const body_haptics::Event& e) {
    if (!shared_ || static_cast<unsigned>(e.kind) >= body_haptics::kindCount)
        return;
    std::lock_guard guard(shared_->lock);
    ++shared_->counters.events;
    ++shared_->counters.kinds[static_cast<unsigned>(e.kind)];
    if (!shared_->finished && shared_->events.size() < mostWaiting)
        shared_->events.push_back(e);
}
void Output::set(const body_haptics::State& s) {
    if (!shared_)
        return;
    std::lock_guard guard(shared_->lock);
    shared_->state = s;
    shared_->stateAt = std::chrono::steady_clock::now();
}
void Output::hold(const body_haptics::Frame& f, float seconds) {
    if (!shared_)
        return;
    std::lock_guard guard(shared_->lock);
    if (!shared_->finished && shared_->holds.size() < mostWaiting)
        shared_->holds.push_back({f, seconds});
}
Counters Output::counters() const {
    if (!shared_)
        return {};
    std::lock_guard guard(shared_->lock);
    return shared_->counters;
}
std::string Output::deviceInfo() {
    if (!shared_)
        return {};
    std::lock_guard guard(shared_->lock);
    // The SDK takes calls from any thread, one at a time.
    if (!shared_->loaded || shared_->stopping || !shared_->api.wsIsConnected())
        return {};
    const char* text = shared_->api.getDeviceInfoJson();
    return text ? text : "";
}

void Output::Shared::run() {
    using namespace std::chrono;
    struct Finish {
        Shared& s;
        ~Finish() {
            std::lock_guard guard(s.lock);
            s.finished = true;
            s.loaded = false;
            s.events.clear();
            s.holds.clear();
            s.wake.notify_all();
        }
    } finish{*this};
    const HMODULE module = LoadLibraryExW(config.library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Api sdk;
    if (!module || !sdk.load(module)) {
        status(Status::noLibrary);
        return;
    }
    {
        std::lock_guard guard(lock);
        api = sdk;
        loaded = true;
    }
    // Shut down on every way out once the SDK may have started: it has
    // threads of its own. Not under the lock, which the headset's frame loop
    // takes every frame: the SDK joins its worker here.
    struct Shutdown {
        Shared& s;
        ~Shutdown() {
            {
                std::lock_guard guard(s.lock);
                s.loaded = false;
            }
            s.api.shutdown();
        }
    } shutdown{*this};
    if (!sdk.isPlayerInstalled()) {
        status(Status::noPlayer);
        return;
    }
    // It connects in the background, and keeps reconnecting: a Player
    // started later, or started again, is found by itself.
    if (!sdk.registryAndInit(config.apiKey.c_str(), config.appId.c_str(), "")) {
        status(Status::failed);
        return;
    }
    status(Status::waiting);
    body_haptics::Mixer mixer;
    const auto origin = steady_clock::now();
    auto next = origin;
    std::vector<body_haptics::Event> posted;
    std::vector<Held> held;
    std::array<bool, body_haptics::deviceCount> sending{};
    uint32_t devices{};
    bool connected{};
    // Whether the Player took the app: it sends the app's event list as it
    // does (within a fifth of a second, October 20), and nothing for an app
    // its server does not know.
    bool accepted{};
    auto connectedAt = origin;
    for (uint64_t tick = 0;; ++tick) {
        body_haptics::State now;
        int percent{};
        {
            std::unique_lock guard(lock);
            if (wake.wait_until(guard, next, [this] { return stopping; }))
                break;
            posted.swap(events);
            held.swap(holds);
            now = state;
            now.live = now.live && steady_clock::now() - stateAt < stateLease;
            percent = level;
        }
        const double seconds = duration<double>(steady_clock::now() - origin).count();
        mixer.set(now);
        for (const auto& e : posted)
            mixer.post(e, seconds);
        for (const auto& h : held)
            mixer.hold(h.frame, seconds, h.seconds);
        posted.clear();
        held.clear();
        const auto frame = mixer.render(seconds);
        const bool connecting = sdk.wsIsConnected();
        if (connecting && !connected)
            connectedAt = steady_clock::now();
        if (!connecting) {
            accepted = false;
        } else if (!accepted && (!connected || tick % 5 == 0)) {
            const char* list = sdk.getHapticMappingsJson();
            accepted = list && std::strlen(list) > 2; // "[]" or nothing: no list
        }
        // The Player lists its devices a second or so after it connects: asked
        // for five times a second until there are some.
        if (connecting && (!connected || tick % devicePoll == 0 || (!devices && tick % 5 == 0))) {
            devices = connectedPositions(sdk.getDeviceInfoJson());
            for (unsigned d = 0; d < body_haptics::deviceCount; ++d)
                if (sdk.isbHapticsConnected(body_haptics::sdkPosition(static_cast<Device>(d))))
                    devices |= 1u << d;
        }
        if (!connecting)
            devices = 0;
        uint64_t sent{}, refused{};
        std::array<int32_t, body_haptics::deviceCount> peaks{};
        for (unsigned d = 0; d < body_haptics::deviceCount; ++d) {
            const auto device = static_cast<Device>(d);
            const unsigned motors = body_haptics::motorCount(device);
            int32_t values[body_haptics::maxMotors]{};
            int32_t peak{};
            for (unsigned m = 0; m < motors; ++m) {
                const auto v = static_cast<int32_t>(std::lround(std::clamp(frame.at(device, m), 0.f, 1.f) * percent));
                values[m] = v >= minimumLevel ? v : 0;
                peak = std::max(peak, values[m]);
            }
            const bool on = connecting && (devices >> d & 1) && peak > 0;
            // The last frame goes before the next plays, and at once when the
            // device goes quiet (a menu, a pause), not when it runs out.
            if (sending[d] && connecting)
                sdk.stop(streamId(device));
            if (on) {
                if (sdk.playDot(streamId(device), body_haptics::sdkPosition(device), config.frameMs, values,
                                static_cast<int32_t>(motors)) < 0) {
                    ++refused;
                } else {
                    ++sent;
                    peaks[d] = peak;
                }
            }
            sending[d] = on;
        }
        {
            std::lock_guard guard(lock);
            counters.connects += connecting && !connected;
            counters.devices = devices;
            counters.devicesSeen |= devices;
            counters.frames += sent;
            counters.refused += refused;
            counters.liveTicks += now.live && connecting;
            for (unsigned d = 0; d < body_haptics::deviceCount; ++d)
                if (peaks[d] > 0) {
                    ++counters.onTicks[d];
                    counters.peakSum[d] += static_cast<uint32_t>(peaks[d]);
                }
            // Connected without a device only once the Player has had time to list them.
            const auto since = steady_clock::now() - connectedAt;
            counters.status = !connecting                                       ? Status::waiting
                              : !accepted && since > std::chrono::seconds(5)    ? Status::rejected
                              : devices                                         ? Status::playing
                              : since > std::chrono::seconds(3)                 ? Status::connected
                                                                                : Status::waiting;
        }
        connected = connecting;
        // Never a burst to catch up: a late tick is followed by a whole one.
        next += milliseconds(config.tickMs);
        if (const auto t = steady_clock::now(); next < t)
            next = t;
    }
    if (connected)
        sdk.stopAll();
    sdk.wsClose();
}
} // namespace spidy::bhaptics

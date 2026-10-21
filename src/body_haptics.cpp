#include "spidy/body_haptics.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace spidy::body_haptics {
namespace {
constexpr float pi = 3.14159265f;
// Patterns playing at once; past it the oldest goes.
constexpr size_t mostPlaying = 48;
float clamp01(float v) {
    return std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f;
}
// Two patterns on one motor add up without passing full strength.
void mix(float& motor, float v) {
    motor = 1 - (1 - motor) * (1 - clamp01(v));
}
// A pulse: up over `attack`, held for `hold`, down over `release` (seconds); 0 outside it.
// The devices get a frame every 40 ms, so a part shorter than that may fall
// between two: the patterns hold at least that long.
float envelope(float t, float attack, float hold, float release) {
    if (!(t >= 0))
        return 0;
    if (t < attack)
        return t / attack;
    t -= attack;
    if (t < hold)
        return 1;
    t -= hold;
    return t < release ? 1 - t / release : 0;
}
// A motor's random number for a step of time; 0..1.
float noise(unsigned motor, int64_t step) {
    uint32_t x = static_cast<uint32_t>(step) * 0x9e3779b1u ^ (motor + 1) * 0x85ebca77u;
    x ^= x >> 15;
    x *= 0x2c1b3c6du;
    x ^= x >> 12;
    x *= 0x297a2d39u;
    x ^= x >> 15;
    return static_cast<float>(x & 0xffff) / 65535.f;
}
// A flutter: each motor its own, changing twenty times a second; 0..1.
float flutter(unsigned motor, double now) {
    return noise(motor, static_cast<int64_t>(std::floor(now * 20)));
}
// A swell: each motor its own, gliding from one value to the next four times
// a second; 0..1. The wind's: a flutter on every motor at once was a buzz.
float swell(unsigned motor, double now) {
    const double x = now * 4;
    const double step = std::floor(x);
    const float f = static_cast<float>(x - step), s = f * f * (3 - 2 * f);
    const auto i = static_cast<int64_t>(step);
    return noise(motor + 64, i) * (1 - s) + noise(motor + 64, i + 1) * s;
}
// An angle brought to -pi..pi.
float wrap(float a) {
    a = std::fmod(a + pi, 2 * pi);
    return (a < 0 ? a + 2 * pi : a) - pi;
}
// The vest's motor's side, 0 the wearer's left to 3 the right, and its row.
unsigned sideOf(unsigned motor) {
    const unsigned column = motor % 4;
    return (motor >= 16 ? backMirrored : frontMirrored) ? 3 - column : column;
}
unsigned rowOf(unsigned motor) {
    return motor % 16 / 4;
}
// Draws patterns into a frame, every motor mixed (mix above).
struct Paint {
    Frame& f;
    void vest(unsigned motor, float v) {
        mix(f.at(Device::vest, motor), v);
    }
    void vest(bool back, unsigned row, unsigned side, float v) {
        vest(vestMotor(back, row, side), v);
    }
    // Every motor of rows `top` to `bottom`, the front's at `front` and the back's at `back`.
    void rows(unsigned top, unsigned bottom, float front, float back) {
        for (unsigned row = top; row <= bottom; ++row)
            for (unsigned side = 0; side < 4; ++side) {
                vest(false, row, side, front);
                vest(true, row, side, back);
            }
    }
    void vestAll(float v) {
        rows(0, 3, v, v);
    }
    // The middle of the chest, and of the back.
    void chest(float v) {
        for (unsigned row = 1; row <= 2; ++row)
            for (unsigned side = 1; side <= 2; ++side)
                vest(false, row, side, v);
    }
    void midBack(float v) {
        for (unsigned row = 1; row <= 2; ++row)
            for (unsigned side = 1; side <= 2; ++side)
                vest(true, row, side, v);
    }
    // The shoulder on a hand's side, front and back, the outer motors most.
    void shoulder(unsigned hand, float v) {
        const unsigned outer = hand ? 3 : 0, inner = hand ? 2 : 1;
        vest(false, 0, outer, v);
        vest(true, 0, outer, v);
        vest(false, 0, inner, v * .5f);
        vest(true, 0, inner, v * .5f);
    }
    void armMotor(unsigned hand, unsigned motor, float v) {
        mix(f.at(body_haptics::arm(hand), motor), v);
    }
    void arm(unsigned hand, float v) {
        for (unsigned m = 0; m < 3; ++m)
            armMotor(hand, m, v);
    }
    void arms(float v) {
        arm(0, v);
        arm(1, v);
    }
    void visor(unsigned fromLeft, float v) {
        mix(f.at(Device::head, visorMotor(fromLeft)), v);
    }
    void visorAll(float v) {
        for (unsigned i = 0; i < 4; ++i)
            visor(i, v);
    }
    // A spot where something comes from `azimuth`, `height` down the body:
    // the motor nearest it at `v`, the ones round it less over `across`
    // radians round the body and `down` of its height, the rest not at all.
    // Whatever the direction, the nearest motor gets the whole `v`. Until the
    // second build of October 20 it spread over half the vest (0.9, 0.45), a
    // side's blow weaker than a frontal one.
    void toward(float azimuth, float height, float v, float across = .5f, float down = .3f) {
        std::array<float, 32> near{};
        float most = 0;
        for (unsigned m = 0; m < 32; ++m) {
            const float a = wrap(vestAzimuth(m) - azimuth) / across, h = (vestHeight(m) - height) / down;
            near[m] = std::exp(-(a * a + h * h));
            most = std::max(most, near[m]);
        }
        if (!(most > 0))
            return;
        for (unsigned m = 0; m < 32; ++m)
            if (const float n = near[m] / most; n >= .2f)
                vest(m, v * n);
    }
};
constexpr const char* names[] = {"web_shot",      "web_attach", "web_taken",   "web_release", "zip",
                                 "grab_caught",   "grab_yanked", "grab_held",  "grab_let_go", "air_pull",
                                 "web_pull",      "wall_land",  "wall_jump",   "landing",     "leap_charge",
                                 "leap",          "punch",      "heavy_blow",  "finisher",    "strike_rush",
                                 "knockout",      "counter",    "spider_sense", "sense_dodge", "hit",
                                 "heal",          "slow_start", "slow_stop",   "perfect_dodge", "dodge",
                                 "calibrated",    "scan",       "gadget",      "phone",       "wall_grip"};
static_assert(std::size(names) == kindCount);
constexpr float durations[] = {.16f, .22f, .14f, .1f,  .3f,  .14f, .22f, .11f, .14f, .2f,  .36f, .22f,
                               .28f, .3f,  .1f,  .4f,  .24f, .3f,  .5f,  .32f, .3f,  .2f,  .45f, .6f,
                               .3f,  .8f,  .5f,  .15f, .32f, .3f,  .55f, .3f,  .1f,  .25f, .12f};
static_assert(std::size(durations) == kindCount);
// Whether `groups` (State::groups) plays the group.
bool plays(uint32_t groups, Group g) {
    return g == Group::always || (groups >> static_cast<unsigned>(g) & 1);
}

void pattern(Paint& p, const Event& e, float t) {
    const float s = e.strength;
    const unsigned hand = e.hand == 1 ? 1u : 0u;
    const bool handed = e.hand == 0 || e.hand == 1;
    // The arm (the shoulder) of the event's hand, else both.
    const auto ownArm = [&](float v) {
        if (handed)
            p.arm(hand, v);
        else
            p.arms(v);
    };
    const auto ownShoulder = [&](float v) {
        p.shoulder(hand, v);
        if (!handed)
            p.shoulder(1 - hand, v);
    };
    switch (e.kind) {
    case Kind::webShot:
        ownArm(s * .3f * envelope(t, .005f, .05f, .06f));
        break;
    case Kind::webAttach:
        // The web takes hold: a tick in the arm, then its shoulder.
        ownArm(s * .32f * envelope(t, .005f, .05f, .1f));
        ownShoulder(s * .16f * envelope(t - .03f, .01f, .05f, .1f));
        break;
    case Kind::webTaken:
        ownArm(s * .22f * envelope(t, .005f, .04f, .08f));
        break;
    case Kind::webRelease:
        ownArm(s * .12f * envelope(t, .005f, .04f, .05f));
        break;
    case Kind::zip: {
        // Yanked along the web: the arm, then the body on the side the web
        // pulls toward, and the face.
        ownArm(s * .4f * envelope(t, .005f, .08f, .12f));
        const float v = s * .28f * envelope(t - .03f, .01f, .08f, .1f);
        if (e.placed)
            p.toward(e.azimuth, e.height, v);
        else
            p.chest(v);
        p.visorAll(s * .15f * envelope(t - .08f, .01f, .05f, .08f));
        break;
    }
    case Kind::grabCaught:
        ownArm(s * .22f * envelope(t, .005f, .05f, .08f));
        break;
    case Kind::grabYanked:
        ownArm(s * .4f * envelope(t, .005f, .08f, .12f));
        ownShoulder(s * .18f * envelope(t - .02f, .01f, .08f, .1f));
        break;
    case Kind::grabHeld:
        ownArm(s * .16f * envelope(t, .005f, .04f, .06f));
        break;
    case Kind::grabLetGo:
        ownArm(s * .26f * envelope(t, .005f, .05f, .08f));
        break;
    case Kind::airPull:
        p.arms(s * .22f * envelope(t, .005f, .06f, .12f));
        p.chest(s * .18f * envelope(t - .02f, .005f, .06f, .1f));
        break;
    case Kind::webPull: {
        p.arms(s * .45f * envelope(t, .01f, .15f, .15f));
        const float v = envelope(t - .03f, .02f, .15f, .15f);
        p.rows(0, 1, s * .25f * v, s * .12f * v);
        break;
    }
    case Kind::wallLand: {
        // The body meets the wall on its side, the hands on it.
        const float v = s * .32f * envelope(t, .005f, .05f, .15f);
        if (e.placed)
            p.toward(e.azimuth, e.height, v, .6f, .35f);
        else
            p.chest(v);
        p.arms(s * .22f * envelope(t, .005f, .04f, .1f));
        break;
    }
    case Kind::wallJump: {
        // Pushed off it: on the side it was, and the arms.
        const float v = s * .3f * envelope(t, .005f, .05f, .1f);
        if (e.placed)
            p.toward(e.azimuth, e.height, v, .6f, .35f);
        else
            p.midBack(v);
        p.arms(s * .28f * envelope(t, .005f, .05f, .1f));
        break;
    }
    case Kind::landing:
        // From the waist up: the waist first and hardest, the shoulders last and least.
        for (unsigned row = 0; row < 4; ++row) {
            const float near = static_cast<float>(row + 1) / 4;
            const float v = s * (.12f + .45f * near) *
                            envelope(t - static_cast<float>(3 - row) * .03f, .005f, .05f, .12f);
            p.rows(row, row, v, v);
        }
        if (s > .6f)
            p.visorAll((s - .6f) * .6f * envelope(t - .06f, .005f, .03f, .08f));
        p.arms(s * .15f * envelope(t - .04f, .005f, .04f, .08f));
        break;
    case Kind::leapCharge: {
        const float v = s * .15f * envelope(t, .01f, .04f, .05f);
        p.rows(2, 3, v, v);
        break;
    }
    case Kind::leap:
        // Up and away: from the waist to the shoulders, front and back.
        for (unsigned row = 0; row < 4; ++row) {
            const float v = s * .4f * envelope(t - static_cast<float>(3 - row) * .06f, .005f, .06f, .08f);
            p.rows(row, row, v, v);
        }
        p.arms(s * .25f * envelope(t, .005f, .08f, .1f));
        p.visorAll(s * .2f * envelope(t - .2f, .01f, .05f, .1f));
        break;
    case Kind::punch: {
        // As hard and as long as the blow; it comes back through the shoulder.
        const float kick = .06f + .08f * s;
        ownArm((.25f + .3f * s) * envelope(t, .005f, kick, .06f));
        ownShoulder(.15f * s * envelope(t - .02f, .005f, kick, .06f));
        break;
    }
    case Kind::heavyBlow:
        ownArm(.6f * envelope(t, .005f, .18f, .08f));
        if (handed)
            p.arm(1 - hand, .25f * envelope(t, .005f, .1f, .06f));
        p.rows(0, 3, .25f * s * envelope(t - .02f, .005f, .1f, .1f), 0);
        break;
    case Kind::finisher:
        p.arms(.75f * envelope(t, .005f, .3f, .12f));
        p.vestAll(.5f * s * envelope(t, .005f, .15f, .25f));
        p.visorAll(.35f * s * envelope(t, .005f, .1f, .15f));
        break;
    case Kind::strikeRush: {
        // Flying at him: the air on the front, fluttering.
        const float v = s * .18f * envelope(t, .03f, .2f, .08f);
        for (unsigned m = 0; m < 16; ++m)
            p.vest(m, v * (.6f + .4f * flutter(m, t)));
        p.arms(s * .18f * envelope(t, .005f, .05f, .05f));
        break;
    }
    case Kind::knockout: {
        const float v = s * (envelope(t, .005f, .07f, .04f) + envelope(t - .14f, .005f, .07f, .04f));
        p.arms(.45f * v);
        p.chest(.3f * v);
        break;
    }
    case Kind::counter:
        p.arms(s * .45f * envelope(t, .005f, .08f, .06f));
        p.chest(s * .35f * envelope(t, .005f, .06f, .08f));
        p.visorAll(s * .25f * envelope(t, .005f, .05f, .06f));
        break;
    case Kind::spiderSense:
    case Kind::senseDodge: {
        // The tingle: the visor's motors two by two, a dozen times a second,
        // strongest on the side the attack comes from; and a spot of the vest
        // toward it, at the shoulders. One to dodge tingles longer and harder.
        const bool dodge = e.kind == Kind::senseDodge;
        const float level = s * (dodge ? .55f : .4f) * envelope(t, .01f, dodge ? .45f : .3f, .1f);
        const bool odd = static_cast<int64_t>(std::floor(t * 12.5f)) & 1;
        const float side = std::sin(e.azimuth);
        for (unsigned i = 0; i < 4; ++i) {
            const float across = (static_cast<float>(i) - 1.5f) / 1.5f;
            const float lean = clamp01(.55f + .45f * side * across);
            p.visor(i, level * lean * (((i & 1) != 0) == odd ? 1.f : .25f));
        }
        if (e.placed)
            p.toward(e.azimuth, .15f, level * (dodge ? .8f : .65f));
        break;
    }
    case Kind::hit: {
        // Where the blow came from, as hard as the health it took; a heavy one
        // rings in the head. From nowhere known: the chest and the back.
        const float v = (.3f + .45f * s) * envelope(t, .005f, .06f, .18f);
        if (e.placed) {
            p.toward(e.azimuth, e.height, v);
        } else {
            p.chest(v);
            p.midBack(.7f * v);
        }
        if (s > .35f) {
            p.visorAll(.3f * envelope(t, .005f, .04f, .1f));
            p.arms(.15f * envelope(t, .005f, .04f, .08f));
        }
        break;
    }
    case Kind::heal:
        // Warmth spreading out from the middle of the chest, then round the back.
        for (unsigned m = 0; m < 32; ++m) {
            const float r = std::hypot(static_cast<float>(m % 4) - 1.5f, (static_cast<float>(rowOf(m)) - 1.5f) * 1.2f);
            const float at = (m >= 16 ? .2f : 0.f) + r * .12f;
            p.vest(m, s * .22f * envelope(t - at, .05f, .08f, .15f));
        }
        break;
    case Kind::slowStart:
        p.vestAll(s * .25f * envelope(t, .01f, .08f, .35f));
        p.visorAll(s * .2f * envelope(t, .01f, .05f, .2f));
        break;
    case Kind::slowStop:
        p.vestAll(s * .12f * envelope(t, .005f, .05f, .08f));
        break;
    case Kind::perfectDodge:
    case Kind::dodge: {
        // A whoosh across the body, from the side the azimuth names.
        const float v = s * (e.kind == Kind::perfectDodge ? .35f : .2f);
        for (unsigned step = 0; step < 4; ++step) {
            const unsigned side = e.azimuth > 0 ? 3 - step : step;
            const float w = v * envelope(t - static_cast<float>(step) * .05f, .01f, .05f, .06f);
            for (unsigned row = 0; row < 4; ++row) {
                p.vest(false, row, side, w);
                p.vest(true, row, side, w);
            }
        }
        break;
    }
    case Kind::calibrated: {
        // Suited up: the whole body twice.
        const float v = s * .3f * (envelope(t, .005f, .1f, .05f) + envelope(t - .25f, .005f, .1f, .05f));
        p.vestAll(v);
        p.arms(v);
        p.visorAll(v * .7f);
        break;
    }
    case Kind::scan:
        for (unsigned i = 0; i < 4; ++i)
            p.visor(i, s * .3f * envelope(t - static_cast<float>(i) * .05f, .01f, .06f, .06f));
        break;
    case Kind::gadget:
        ownArm(s * .6f * envelope(t, .005f, .04f, .04f));
        break;
    case Kind::phone:
        ownArm(s * .18f * (envelope(t, .005f, .06f, .02f) + envelope(t - .12f, .005f, .06f, .02f)));
        break;
    case Kind::wallGrip:
        ownArm(s * .2f * envelope(t, .005f, .04f, .06f));
        break;
    }
}
} // namespace

bool Frame::any(Device d) const {
    const auto& m = motors[static_cast<unsigned>(d)];
    return std::any_of(m.begin(), m.begin() + motorCount(d), [](float v) { return v > 0; });
}

float vestAzimuth(unsigned motor) {
    // The inner columns either side of the zip and the spine, the outer ones
    // round toward the flanks.
    const float across = static_cast<float>(sideOf(motor)) - 1.5f;
    const float out = std::abs(across) > 1 ? .95f : .3f;
    return (across < 0 ? -1.f : 1.f) * (motor < 16 ? out : pi - out);
}
float vestHeight(unsigned motor) {
    return static_cast<float>(rowOf(motor)) / 3;
}
const char* name(Kind k) {
    const auto i = static_cast<unsigned>(k);
    return i < kindCount ? names[i] : "unknown";
}
float duration(Kind k) {
    const auto i = static_cast<unsigned>(k);
    return i < kindCount ? durations[i] : 0.f;
}
Group group(Kind k) {
    switch (k) {
    case Kind::webShot:
    case Kind::webAttach:
    case Kind::webTaken:
    case Kind::webRelease:
    case Kind::zip:
    case Kind::grabCaught:
    case Kind::grabYanked:
    case Kind::grabHeld:
    case Kind::grabLetGo:
    case Kind::airPull:
    case Kind::webPull:
        return Group::webs;
    case Kind::wallLand:
    case Kind::wallJump:
    case Kind::landing:
    case Kind::leapCharge:
    case Kind::leap:
    case Kind::wallGrip:
        return Group::movement;
    case Kind::punch:
    case Kind::heavyBlow:
    case Kind::finisher:
    case Kind::strikeRush:
    case Kind::knockout:
    case Kind::counter:
    case Kind::spiderSense:
    case Kind::senseDodge:
    case Kind::hit:
    case Kind::heal:
    case Kind::slowStart:
    case Kind::slowStop:
    case Kind::perfectDodge:
    case Kind::dodge:
        return Group::combat;
    case Kind::calibrated:
    case Kind::scan:
    case Kind::gadget:
    case Kind::phone:
        break;
    }
    return Group::always;
}

float Body::azimuth(Vec3 world) const {
    const float x = dot(world, right), z = dot(world, ahead);
    return std::isfinite(x) && std::isfinite(z) && x * x + z * z > 1e-8f ? std::atan2(x, z) : 0.f;
}
float Body::height(Vec3 world) const {
    const float flat = std::hypot(dot(world, right), dot(world, ahead));
    const float rise = std::atan2(dot(world, up), flat);
    return std::isfinite(rise) ? std::clamp(.4f - rise / 1.5f, 0.f, 1.f) : .4f;
}
namespace {
// Where a head looks across the floor, as a unit vector: looking straight
// down, the top of the head points ahead; straight up, behind. Zero for none.
Vec3 levelAhead(const Quat& head) {
    const Vec3 forward = head.rotate({0, 0, -1}), top = head.rotate({0, 1, 0});
    Vec3 ahead{forward.x, 0, forward.z};
    if (!(length(ahead) >= .2f))
        ahead = forward.y < 0 ? Vec3{top.x, 0, top.z} : Vec3{-top.x, 0, -top.z};
    ahead = normalized(ahead);
    return finite(ahead) && length(ahead) > .5f ? ahead : Vec3{};
}
} // namespace
Body body(const Quat& head) {
    const Vec3 ahead = levelAhead(head);
    if (length(ahead) < .5f)
        return {};
    return {cross(ahead, {0, 1, 0}), {0, 1, 0}, ahead};
}
Body Facing::update(const Quat& head, const Quat& toWorld, float seconds) {
    seconds = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .25f) : 0.f;
    if (const Vec3 ahead = levelAhead(head); length(ahead) > .5f) {
        const float looks = std::atan2(ahead.x, -ahead.z);
        if (!set_) {
            yaw_ = looks;
            set_ = true;
        } else {
            const float off = wrap(looks - yaw_);
            if (std::abs(off) > deadZone)
                yaw_ = wrap(yaw_ + std::copysign(std::min(std::abs(off) - deadZone, speed * seconds), off));
            yaw_ = wrap(yaw_ + wrap(looks - yaw_) * (1 - std::exp(-drift * seconds)));
        }
    }
    const Vec3 ahead{std::sin(yaw_), 0, -std::cos(yaw_)}, right{std::cos(yaw_), 0, std::sin(yaw_)};
    Body b{toWorld.rotate(right), toWorld.rotate({0, 1, 0}), toWorld.rotate(ahead)};
    if (!finite(b.right) || !finite(b.up) || !finite(b.ahead) || length(b.ahead) < .5f)
        return {right, {0, 1, 0}, ahead};
    return b;
}

void Mixer::post(const Event& e, double now) {
    if (!state_.live || static_cast<unsigned>(e.kind) >= kindCount || !std::isfinite(now) ||
        !plays(state_.groups, group(e.kind)))
        return;
    Event clean = e;
    clean.strength = clamp01(e.strength);
    clean.placed = e.placed && std::isfinite(e.azimuth) && std::isfinite(e.height);
    clean.azimuth = std::isfinite(e.azimuth) ? wrap(e.azimuth) : 0.f;
    clean.height = std::isfinite(e.height) ? clamp01(e.height) : .4f;
    if (clean.hand != 0 && clean.hand != 1)
        clean.hand = -1;
    if (active_.size() >= mostPlaying)
        active_.erase(active_.begin());
    active_.push_back({clean, now});
}
void Mixer::set(const State& s) {
    state_ = s;
    if (!finite(state_.velocity))
        state_.velocity = {};
    for (unsigned h = 0; h < 2; ++h) {
        state_.webTension[h] = std::isfinite(s.webTension[h]) ? std::max(s.webTension[h], 0.f) : 0.f;
        state_.grabTension[h] = clamp01(s.grabTension[h]);
    }
    state_.charge = clamp01(s.charge);
    state_.health = std::isfinite(s.health) ? std::clamp(s.health, 0.f, 1.f) : 1.f;
}
void Mixer::hold(const Frame& f, double now, float seconds) {
    if (!state_.live || !std::isfinite(now) || !(seconds > 0))
        return;
    if (held_.size() >= mostPlaying)
        held_.erase(held_.begin());
    held_.push_back({f, now + seconds});
}
Frame Mixer::render(double now) {
    Frame f;
    if (!std::isfinite(now))
        return f;
    const float dt = last_ < 0 ? 0.f : std::clamp(static_cast<float>(now - last_), 0.f, .25f);
    last_ = std::max(last_, now);
    std::erase_if(active_, [&](const Active& a) { return now - a.start > duration(a.e.kind); });
    std::erase_if(held_, [&](const Held& h) { return now >= h.until; });
    if (!state_.live) {
        active_.clear();
        held_.clear();
        webFelt_ = grabFelt_ = {};
        windFelt_ = 0;
        return f;
    }
    // A group switched off stops at once, what it was playing with it.
    const auto on = [&](Group g) { return plays(state_.groups, g); };
    std::erase_if(active_, [&](const Active& a) { return !on(group(a.e.kind)); });
    if (!on(Group::webs))
        webFelt_ = grabFelt_ = {};
    if (!on(Group::wind))
        windFelt_ = 0;
    Paint p{f};
    // The webs' pull, smoothed over an eighth of a second: the swing's step
    // to step changes would buzz. It grows from nothing: a web hanging the
    // player at real gravity pulls about 10 m/s^2 (barely felt, 2%), a
    // swing's middle 20 (8%), the bottom of a fast swing 40 (23%) and 50 and
    // more 30% (October 20's swings: 9, 20 and 41 m/s^2 at the tenth, half
    // and nine tenths of their attached time). Until then any web pulling
    // over 2 m/s^2 played 12% at once, and 36% in the middle of a swing.
    const float follow = 1 - std::exp(-dt / .12f);
    bool hanging = false;
    for (unsigned h = 0; h < 2; ++h) {
        hanging = hanging || state_.webTension[h] > 2;
        const float pull = on(Group::webs) ? clamp01((state_.webTension[h] - 6) / 44) : 0.f;
        const float web = .3f * std::pow(pull, 1.1f);
        webFelt_[h] += (web - webFelt_[h]) * follow;
        grabFelt_[h] += ((on(Group::webs) ? .3f * state_.grabTension[h] : 0.f) - grabFelt_[h]) * follow;
        if (webFelt_[h] > .005f) {
            p.arm(h, webFelt_[h]);
            p.shoulder(h, webFelt_[h] * .5f);
        }
        if (grabFelt_[h] > .005f)
            p.arm(h, grabFelt_[h]);
    }
    // The wind, in free flight only: in the air, off any wall, no web
    // holding the player. From 15 m/s (nothing) to 40 (16%), on the motors
    // that face where he flies, the nearer they face it the more; in a fall
    // coming up from below, the waist most; and on the face. Smoothed over a
    // quarter of a second, so a web taking hold lets it die away. On October
    // 20 it blew from 12 m/s on half the vest, also on the walls and on a web,
    // and was on 94% of the time in the air (the swing's speed: 31 m/s at the
    // middle).
    const float speed = length(state_.velocity);
    const bool free = on(Group::wind) && state_.airborne && !state_.onWall && !hanging;
    windFelt_ += ((free ? .16f * clamp01((speed - 15) / 25) : 0.f) - windFelt_) * (1 - std::exp(-dt / .25f));
    if (windFelt_ > .005f && speed > 1) {
        const Vec3 way = state_.velocity / speed;
        const float rising = std::max(0.f, -way.y);
        for (unsigned m = 0; m < 32; ++m) {
            // The motor's facing: round the body, the shoulders' row tipped up a little and the waist's down.
            const float az = vestAzimuth(m), tip = .25f - .5f * vestHeight(m);
            const Vec3 n{std::sin(az) * std::cos(tip), std::sin(tip), std::cos(az) * std::cos(tip)};
            const float into = std::min(1.f, std::max(0.f, dot(n, way)) + rising * .6f * vestHeight(m));
            if (into > .1f)
                p.vest(m, windFelt_ * into * into * (.7f + .3f * swell(m, now)));
        }
        if (const float face = std::max(0.f, way.z) * .8f + rising * .3f; face > .1f)
            for (unsigned i = 0; i < 4; ++i)
                p.visor(i, windFelt_ * face * face * (.7f + .3f * swell(40 + i, now)));
    }
    // A charged leap building in the legs' end of the vest, and in the arms.
    if (state_.charge > 0 && on(Group::movement)) {
        const float wobble = .75f + .25f * std::sin(2 * pi * 9 * static_cast<float>(std::fmod(now, 1.0)));
        const float v = (.06f + .2f * state_.charge) * wobble;
        p.rows(2, 3, v, v);
        p.arms((.04f + .12f * state_.charge) * wobble);
    }
    // The heart, left of the chest's middle: racing below 30% of the health,
    // slow and calm in slow motion.
    const bool low = state_.health > 0 && state_.health < .3f;
    if ((low || state_.slowMotion) && on(Group::combat)) {
        const float bpm = low ? 80 + (.3f - state_.health) * 200 : 55.f;
        const float strength = low ? .18f + (.3f - state_.health) * .6f : .14f;
        const float t = static_cast<float>(std::fmod(now, 60.0 / bpm));
        const float beat = strength * (envelope(t, .02f, .07f, .05f) + .7f * envelope(t - .2f, .02f, .06f, .05f));
        p.vest(false, 1, 1, beat);
        p.vest(false, 1, 0, .6f * beat);
        p.vest(false, 2, 1, .5f * beat);
    }
    for (const auto& a : active_)
        pattern(p, a.e, static_cast<float>(now - a.start));
    for (const auto& h : held_)
        for (unsigned d = 0; d < deviceCount; ++d)
            for (unsigned m = 0; m < motorCount(static_cast<Device>(d)); ++m)
                mix(f.motors[d][m], h.frame.motors[d][m]);
    return f;
}

float LandingWatch::update(float seconds, bool airborne, float downSpeed) {
    seconds = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
    downSpeed = std::isfinite(downSpeed) ? downSpeed : 0.f;
    float landed = 0;
    if (airborne) {
        airSeconds_ += seconds;
        // The speed it came down at in the flight's last moments: held, and let
        // go of over a fifth of a second.
        fastest_ = std::max(downSpeed, fastest_ * std::exp(-seconds / .2f));
    } else if (airborne_ && airSeconds_ >= .25f && fastest_ > 4) {
        // A hop of a moment is no flight; a jump lands softly, a fall from a roof hard.
        landed = .15f + .85f * clamp01((fastest_ - 4) / 24);
    }
    if (!airborne)
        fastest_ = airSeconds_ = 0;
    airborne_ = airborne;
    return landed;
}

float HealthWatch::update(float hitPoints) {
    if (!std::isfinite(hitPoints) || hitPoints < 0 || hitPoints > 1e6f)
        return 0;
    most_ = std::max(most_, hitPoints);
    float lost = 0;
    if (last_ >= 0 && most_ > 0 && last_ - hitPoints > .005f * most_)
        lost = (last_ - hitPoints) / most_;
    last_ = hitPoints;
    return lost;
}
float HealthWatch::fraction() const {
    return last_ >= 0 && most_ > 0 ? std::clamp(last_ / most_, 0.f, 1.f) : 1.f;
}
} // namespace spidy::body_haptics

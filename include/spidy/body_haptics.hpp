#pragma once
#include "math.hpp"
#include <array>
#include <cstdint>
#include <vector>

// Body haptics: what happens to Spider-Man felt on a bHaptics TactSuit (the
// vest), a TactSleeve on each forearm and a TactVisor on the headset - webs
// taking hold, the swing's pull in the arm that holds it, the wind of a fall,
// landings, blows given and taken, and Spider-Sense tingling in the head.
//
// The game's XR worker posts what happened (Event) and, every frame, what is
// going on (State); the mixer turns both into each motor's strength for one
// moment (Frame), which bhaptics_output.hpp sends to the bHaptics Player. Every
// pattern is made here, from code: nothing has to be designed or deployed in
// bHaptics' own portal. Pure and deterministic, so the core tests play it.
//
// Strengths are the share of a motor's full strength (the SDK's 100) at the
// player's 100%. The first session on the devkit (October 20) had most events
// at 50-100% and the swing's wind on 94% of the time in the air: "way too
// agressive, directionality didnt really feel like anything, felt like 0 to
// 100 always". Since then a pattern is a spot of a few motors where it can be,
// what goes on all the time (the web's pull, the wind) stays under a third and
// grows with what causes it from nothing, and events play at 10-45% but for
// the heaviest blows.
namespace spidy::body_haptics {
// The devices, as the bHaptics SDK places them (playDot's `position`: 0 the
// vest, 1 and 2 the left and right forearm, 3 the head).
enum class Device : uint8_t { vest, leftArm, rightArm, head };
inline constexpr unsigned deviceCount = 4, maxMotors = 32;
inline constexpr unsigned motorCount(Device d) {
    return d == Device::vest ? 32u : d == Device::head ? 4u : 3u;
}
inline constexpr int32_t sdkPosition(Device d) {
    return static_cast<int32_t>(d);
}
inline constexpr Device arm(unsigned hand) {
    return hand ? Device::rightArm : Device::leftArm;
}
// Each motor's strength, 0..1, by device and the device's own motor index.
struct Frame {
    std::array<std::array<float, maxMotors>, deviceCount> motors{};
    float& at(Device d, unsigned motor) {
        return motors[static_cast<unsigned>(d)][motor];
    }
    float at(Device d, unsigned motor) const {
        return motors[static_cast<unsigned>(d)][motor];
    }
    // Whether any motor of the device is on.
    bool any(Device d) const;
};

// The TactSuit Pro's motors (bHaptics' motor index, docs.bhaptics.com/sdk/
// further/motor): 0-15 the front, 16-31 the back, each a grid of four rows
// (0 the shoulders, 3 the waist) by four columns, row by row. On both sides
// the diagram puts column 0 at the wearer's left: the front seen from ahead
// (0 by the chest's control dial) and the back seen from behind. If the suit
// says otherwise in the hand, these two flip a side.
inline constexpr bool frontMirrored = false, backMirrored = false;
// The motor on `back` (else the front), `row` 0-3 down from the shoulders,
// `side` 0-3 across from the wearer's left to the right.
inline constexpr unsigned vestMotor(bool back, unsigned row, unsigned side) {
    const unsigned column = (back ? backMirrored : frontMirrored) ? 3 - side : side;
    return (back ? 16u : 0u) + row * 4 + column;
}
// Where a vest motor sits: around the body, radians from straight ahead,
// positive to the wearer's right (the back's middle at +-pi); down the body,
// 0 at the shoulders to 1 at the waist.
float vestAzimuth(unsigned motor);
float vestHeight(unsigned motor);
// The TactVisor's four motors run across the forehead; whether from the
// wearer's left (as assumed) or right is to be felt.
inline constexpr bool visorMirrored = false;
inline constexpr unsigned visorMotor(unsigned fromLeft) {
    return visorMirrored ? 3 - fromLeft : fromLeft;
}

// What happened. Each makes a pattern of its own, a fraction of a second long.
enum class Kind : uint8_t {
    webShot,      // a web ball left the wrist (hand)
    webAttach,    // a shot web took hold (hand)
    webTaken,     // a hand took hold of the other hand's web (hand)
    webRelease,   // a web let go of (hand)
    zip,          // a web zip (hand; azimuth, height: toward the web's end)
    grabCaught,   // the web caught a prop or a thug (hand)
    grabYanked,   // ... and yanked it (hand)
    grabHeld,     // ... and holds it at the hand (hand)
    grabLetGo,    // ... and threw it or let it go (hand)
    airPull,      // a thug the web brought in arrived within reach, in the air
    webPull,      // both hands yanked a door or a grate (the game's LB + RB)
    wallLand,     // onto a wall (azimuth, height: where the wall is)
    wallJump,     // off a wall with a jump (azimuth, height: where the wall is)
    landing,      // feet down after a fall (strength: how hard)
    leapCharge,   // a charged leap began to build
    leap,         // a charged leap let go of
    punch,        // a fist landed (hand, strength)
    heavyBlow,    // a heavy or called blow landed (hand)
    finisher,     // a finisher landed (hand)
    strikeRush,   // a web strike flies the player at a thug
    knockout,     // the player knocked a thug out
    counter,      // a Spider-Sense counter landed
    spiderSense,  // the game warned of an attack (azimuth: where it comes from)
    senseDodge,   // ... one to dodge, not counter
    hit,          // the player was hit (azimuth, height, strength: how much of his health)
    heal,         // the player healed
    slowStart,    // slow motion began
    slowStop,     // slow motion ended
    perfectDodge, // the game slowed time for a dodge in time
    dodge,        // a dodge (B; azimuth: the side it goes to)
    calibrated,   // the T-pose calibration finished
    scan,         // the hand at the temple: the game's scan
    gadget,       // the gadget wheel: opened, a place lit, one chosen (hand, strength)
    phone,        // the phone came out (hand)
    wallGrip,     // a hand took hold of the wall to climb (hand; HAND WALL CLIMBING)
};
inline constexpr unsigned kindCount = static_cast<unsigned>(Kind::wallGrip) + 1;
// Its name, for the session report and the test tool ("web_shot", ...).
const char* name(Kind);
// What the tab's switches play or silence as one (WEB, WIND, COMBAT and
// MOVEMENT HAPTICS): webs (shots, holds, the swing's pull, zips, grabs), the
// wind, combat (blows given and taken, Spider-Sense, dodges, heals, slow
// motion and the heart) and movement (landings, walls, leaps). The few
// confirmations of the interface (calibrated, a scan, the gadget wheel, the
// phone) play whenever body haptics do.
enum class Group : uint8_t { webs, wind, combat, movement, always };
inline constexpr uint32_t allGroups = 0xf;
Group group(Kind);

struct Event {
    Kind kind{};
    int8_t hand = -1;    // 0 the left, 1 the right; -1 neither
    float strength = 1;  // 0..1
    float azimuth = 0;   // where it comes from around the body: radians from ahead, + to the right
    float height = .4f;  // and how high: 0 the shoulders to 1 the waist
    // Whether azimuth and height say anything: false plays the event's own
    // place (a hit: the chest and the back at once).
    bool placed = true;
};

// What goes on while it lasts, given every frame.
struct State {
    // VR play: off, everything stops (menus, cutscenes, the game screen) and
    // events are let go of.
    bool live{};
    // The player's velocity in the frame of his body (Body below): x to the
    // right, y up, z ahead; m/s. In free flight the wind blows against it.
    Vec3 velocity{};
    bool airborne{};
    // Held on a wall or a ceiling (Spidy's own wall, the game's crawl): no
    // wind there, however fast the player runs along it.
    bool onWall{};
    // Each arm's web as it pulls the body (the swing's tension, m/s^2; 0 for
    // no web or a slack one), and a grab's pull at the hand (0..1).
    std::array<float, 2> webTension{}, grabTension{};
    // A charged leap building (0..1), slow motion on, and the player's health
    // (0..1 of the most seen; 1 where it is unknown).
    float charge{};
    bool slowMotion{};
    float health = 1;
    // The groups on (bit 1 << Group): one off plays none of its events, and
    // what goes on in it stops at once.
    uint32_t groups = allGroups;
};

// The frame of the player's body for the vest: its right, up and ahead in
// the world, and a world direction or velocity in it (x right, y up, z ahead).
struct Body {
    Vec3 right{1, 0, 0}, up{0, 1, 0}, ahead{0, 0, -1};
    Vec3 local(Vec3 world) const {
        return {dot(world, right), dot(world, up), dot(world, ahead)};
    }
    // Where a world direction comes from around the body (Event::azimuth).
    float azimuth(Vec3 world) const;
    // How high on the body a blow from a world direction lands (Event::height).
    float height(Vec3 world) const;
};
// Level, facing where the head looks.
Body body(const Quat& head);
// Where the torso faces, which the vest does: it is not tracked. It faces
// where the head does until the head turns further than a look over the
// shoulder (deadZone), then turns with it at up to `speed` radians a second,
// and drifts after it by `drift` of the way a second meanwhile - as
// body_ik.hpp turns Spider-Man's body. Until the second build of October 20
// the vest faced wherever the head looked, so a look around turned the wind
// and every blow round the body with it.
class Facing {
  public:
    static constexpr float deadZone = .6f, speed = 6, drift = .5f;
    // head: the headset's orientation in the room (the tracking space, +y
    // up); toWorld: the room's turn into the world (snap and smooth turns, a
    // wall stood on, a flip); seconds: since the last call. The torso turns
    // in the room, so a turn of the room turns it at once.
    Body update(const Quat& head, const Quat& toWorld, float seconds);
    void reset() {
        *this = {};
    }

  private:
    float yaw_{};
    bool set_{};
};

class Mixer {
  public:
    // Plays `e` from `now` (seconds on a steady clock).
    void post(const Event& e, double now);
    void set(const State& s);
    // Plays `f` as it is for `seconds` from `now`, over everything else (the
    // test tool's motor by motor map).
    void hold(const Frame& f, double now, float seconds);
    // Every motor at `now`, which only goes forward: 0..1 each, before the
    // player's level. Not live: all off.
    Frame render(double now);
    // Patterns playing, for the tests.
    size_t playing() const {
        return active_.size();
    }

  private:
    struct Active {
        Event e;
        double start{};
    };
    std::vector<Active> active_;
    struct Held {
        Frame frame;
        double until{};
    };
    std::vector<Held> held_;
    State state_;
    // The swing's and the grab's pulls as the arms feel them, and the wind,
    // smoothed.
    std::array<float, 2> webFelt_{}, grabFelt_{};
    float windFelt_{};
    double last_ = -1;
};
// How long each pattern plays, seconds.
float duration(Kind);

// The end of a flight, from the swing's samples: how hard the feet came down
// (0..1; 0 for none, or one too soft to feel).
class LandingWatch {
  public:
    // seconds: since the last call. airborne: the player is in the air now;
    // downSpeed: how fast he comes down (m/s, the world's).
    float update(float seconds, bool airborne, float downSpeed);

  private:
    bool airborne_{};
    float fastest_{}, airSeconds_{};
};

// Hit points lost, from the hero's health each frame.
class HealthWatch {
  public:
    // hitPoints: the hero's now, or NaN where they cannot be read. The share
    // of the most seen lost since the last reading (0: none).
    float update(float hitPoints);
    // The health now, 0..1 of the most seen (1 while unknown).
    float fraction() const;
    // A new player (a respawn, another hero): what he had is no measure.
    void reset() {
        *this = {};
    }

  private:
    float last_ = -1, most_{};
};
} // namespace spidy::body_haptics

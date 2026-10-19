#pragma once
#include "math.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

// Miles's venom punch (MilesVR). With the venom meter full, the right
// controller shoots no web: its web button (the grip) held charges the right
// fist with venom; charged, a right punch at the marked enemy is a venom blow
// (game_punch) and spends the meter. The user's design, October 19, after
// the right thumbstick's click of 0.1.8 and 0.1.9 (a stick held in is half
// of the pad layer's chord, and in 0.1.9 it latched the pad layer instead).
namespace spidy::venom_punch {
// The meter: a float from 0 to fullMeter at +meterOffset in the player's
// Hero::HeroMeleeManager (its vtable, MilesMorales.exe 4.630.0.0). Found
// October 18 from a recording of the training sim: 100 at the start of a
// fight, down to 15.25 at a heal, back up 6.5 a landed hit to 100, down to
// 41.75 at the next heal (reports\venom-probe-20261018-234812.csv).
constexpr uintptr_t meleeManagerVtable = 0x3e122a8, meterOffset = 0x28c;
constexpr float fullMeter = 100;
// The most a meter read is believed to hold: Miles gets two more bars later
// in the story (until 0.1.21 a meter past 200 read as no meter at all).
constexpr float mostMeter = 1000;
// Within this long of an enemy near, the player is in a fight: the right
// stick's click is no LB + RB there (the game's web throw of an object).
constexpr float fightCalmSeconds = 3;
inline bool full(float meter) {
    return std::isfinite(meter) && meter >= fullMeter - .01f;
}
// What a blow of the player's adds to the meter: the game fills it 6.5 a hit
// of its own melee and nothing for Spidy's blows (0.1.14: the meter rose only
// with the game's hits). The user, October 19: a hit should charge it "a
// reasonable amount not to quick but not too slow" - ten plain hits fill it,
// a hard blow (heavy, launch, juggle, slam) gives half again, the flow's
// finisher twice. The venom punch itself gives nothing.
constexpr float hitGain = 10, hardGain = 15, finisherGain = 20;
inline float gain(bool hard, bool finisher) {
    return finisher ? finisherGain : hard ? hardGain : hitGain;
}
// The meter after `amount` more, never past full. A meter the game holds
// above fullMeter (later in the story Miles has two more bars) is left as it
// is: a blow must not cut it back to one bar.
inline float filled(float meter, float amount) {
    if (!std::isfinite(meter) || meter < 0)
        meter = 0;
    if (meter >= fullMeter)
        return meter;
    return std::min(fullMeter, meter + std::max(0.f, amount));
}

// Miles's bars (0.1.22, from the user's 100% save, October 19): the meter
// is bar one of up to mostBars, each a value and its most, side by side - the
// value at the meter's address, its most 4 bytes on, the next bar 8 bytes on.
// A save early in the story has one bar (the others' most is 0); the 100%
// save had three of 100. The game fills them in order: its hits went into
// the second once the first was full (the user: "punching a guy wouldnt fill
// the 2nd or 3rd bar up only the first" - 0.1.21 wrote the first alone).
constexpr unsigned mostBars = 3;
struct Bars {
    float value[mostBars]{}, most[mostBars]{};
    unsigned count = 1;
};
// The bars from the words at the meter (value, most, value, most, ...). A
// first bar without a believable most is one of fullMeter.
inline Bars bars(const float (&words)[mostBars * 2]) {
    Bars b;
    b.count = 0;
    for (unsigned i = 0; i < mostBars; ++i) {
        const float value = words[i * 2], most = words[i * 2 + 1];
        const bool real = std::isfinite(most) && most > 1 && most <= mostMeter && std::isfinite(value) &&
                          value >= 0 && value <= most + .01f;
        if (!real) {
            if (i == 0) {
                b.value[0] = std::isfinite(value) ? std::clamp(value, 0.f, fullMeter) : 0.f;
                b.most[0] = fullMeter;
                b.count = 1;
            }
            break;
        }
        b.value[i] = value;
        b.most[i] = most;
        b.count = i + 1;
    }
    return b;
}
inline float total(const Bars& b) {
    float t = 0;
    for (unsigned i = 0; i < b.count; ++i)
        t += b.value[i];
    return t;
}
inline float capacity(const Bars& b) {
    float t = 0;
    for (unsigned i = 0; i < b.count; ++i)
        t += b.most[i];
    return t;
}
// The bars holding `amount` in all, filled in order.
inline Bars holding(Bars b, float amount) {
    float left = std::isfinite(amount) ? std::clamp(amount, 0.f, capacity(b)) : 0.f;
    for (unsigned i = 0; i < b.count; ++i) {
        b.value[i] = std::min(left, b.most[i]);
        left -= b.value[i];
    }
    return b;
}
// A blow's gain, into the bars in order; a venom move's cost, one bar (the
// first's worth) off the whole.
inline Bars gained(const Bars& b, float amount) {
    return holding(b, total(b) + std::max(0.f, amount));
}
inline Bars paid(const Bars& b) {
    return holding(b, total(b) - b.most[0]);
}

// The Mega Venom Blast (0.1.23, the user: "this depletes 3 bars"): ready
// when Miles has all three bars and all are full; it spends them all.
inline bool megaReady(const Bars& b) {
    return b.count >= mostBars && total(b) >= capacity(b) - .01f;
}
inline Bars paidMega(const Bars& b) {
    return holding(b, 0);
}

// The meter after a venom move: one bar spent (the user, October 19: the
// punch and the smash "should use only one bar"). Until Miles has more bars
// that is the whole meter; one of fullMeter's worth comes off a meter above
// it. (The thumbstick's let-go empties it all: with a bar left the meter
// would still be full and the hand's webs still off.)
inline float spent(float meter) {
    if (!std::isfinite(meter) || meter <= fullMeter)
        return 0;
    return meter - fullMeter;
}

// The web button pressed past `on` and let go below `off`.
constexpr float gripOn = .6f, gripOff = .35f;
// The right web button, read as the venom charge.
class Charge {
  public:
    // Held this long, the fist is charged; let go charged, it stays charged
    // this long for the punch; the glow fades over the last fadeSeconds.
    static constexpr float chargeSeconds = .35f, keepSeconds = 1.5f, fadeSeconds = .3f;
    struct State {
        float glow{};   // 0..1: how bright the fist's venom shows
        bool armed{};   // the next blow of the right fist is venom
        bool charged{}; // it became armed at this update (a rumble)
    };
    // seconds: since the previous update. play: VR play, the right fist free
    // to punch. full: the meter is full. grip: the right web button, 0..1.
    State update(float seconds, bool play, bool full, float grip) {
        const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
        State out;
        held_ = std::isfinite(grip) && grip >= (held_ ? gripOff : gripOn);
        const bool held = held_;
        if (!play || !full) {
            charge_ = keep_ = 0;
            return out;
        }
        if (held && !spent_) {
            const bool was = charge_ >= 1;
            charge_ = std::min(1.f, charge_ + dt / chargeSeconds);
            out.charged = charge_ >= 1 && !was;
            if (charge_ >= 1)
                keep_ = keepSeconds;
        } else if (!held) {
            spent_ = false;
            if (charge_ >= 1 && keep_ > 0)
                keep_ = std::max(0.f, keep_ - dt);
            if (charge_ < 1 || keep_ <= 0)
                charge_ = keep_ = 0;
        }
        out.armed = charge_ >= 1;
        out.glow = out.armed && !held ? std::min(1.f, keep_ / fadeSeconds) : charge_;
        return out;
    }
    // The venom blow landed: the charge is gone, and a grip still held
    // charges nothing more until it is let go.
    void spent() {
        charge_ = keep_ = 0;
        spent_ = true;
    }

  private:
    float charge_{}, keep_{};
    bool held_{}, spent_{};
};

// The venom hand's buttons while the venom has it (MilesVR 0.1.49). The web
// button is the hand's webs alone: 0.1.42-0.1.48 made its tap a web ball, and
// the user, October 20: "seperate web balls from the web swinging button ...
// it keeps interfering with swinging while in combat and interferes with camo"
// (a quick web in a fight was a tap, and the web ball an attack). The venom
// button shares: while the hand's web holds it reels; otherwise a tap (let go
// within tapSeconds) is a web ball, a press of pulseSeconds as it is let go,
// and a hold charges the fist. Until the venom has the hand nothing of this
// runs: the venom button is the swing's own, a web ball the moment it is
// pulled.
class VenomButtons {
  public:
    static constexpr float tapSeconds = .25f, pulseSeconds = .1f;
    static_assert(tapSeconds < Charge::chargeSeconds);
    struct Out {
        float web{};    // the swing's web button: shoot and hold a web
        float reel{};   // the swing's other: reel a held web, else a web ball
        float charge{}; // the venom charge's button (Charge::update's grip)
    };
    // seconds: since the previous update. venom and other: the venom button
    // and the web button, 0..1. webHeld: the hand's web holds.
    Out update(float seconds, float venom, float other, bool webHeld) {
        const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
        const auto unit = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f; };
        const float v = unit(venom);
        Out out;
        out.web = unit(other);
        if (webHeld) {
            reset();
            out.reel = v;
            return out;
        }
        const bool down = v >= (held_ ? gripOff : gripOn);
        if (down)
            time_ += dt;
        else {
            if (held_ && time_ < tapSeconds) {
                pulse_ = pulseSeconds;
                ++taps_;
            }
            time_ = 0;
        }
        held_ = down;
        out.charge = down ? v : 0.f;
        out.reel = pulse_ > 0 ? 1.f : 0.f;
        pulse_ = std::max(0.f, pulse_ - dt);
        return out;
    }
    void reset() {
        held_ = false;
        time_ = pulse_ = 0;
    }
    // Web balls the taps sent.
    uint32_t taps() const {
        return taps_;
    }

  private:
    bool held_{};
    float time_{}, pulse_{};
    uint32_t taps_{};
};

// The venom smash (MilesVR 0.1.20, the user's design, October 19): the
// charged venom fist punched DOWN - in the air (no web holding him) Miles
// drops to the ground and the smash goes off as he lands; standing, it goes
// off where he is. A venom blast hits every enemy within smashRadius and
// throws him up and away, and the meter is spent. Any other charged punch is
// still the venom punch.
//
// A punch down: the fist, relative to the player, at least smashSpeed and
// within smashCone of straight down (its downward share at least
// smashSteep, the cosine).
constexpr float smashSpeed = 2.6f, smashSteep = .82f;
inline bool smashing(Vec3 fist) {
    const float speed = length(fist);
    return std::isfinite(speed) && speed >= smashSpeed && fist.y <= -smashSteep * speed;
}
// The blast: every enemy whose feet are within smashRadius across and
// smashHeight up or down of where it goes off. Its damage is a venom
// punch's at the middle, smashEdge of that at the edge; he flies away from
// the middle at smashThrow and up at smashLift.
constexpr float smashRadius = 5, smashHeight = 3, smashEdge = .6f, smashThrow = 12, smashLift = 7;
inline bool inBlast(Vec3 centre, Vec3 feet) {
    const Vec3 d = feet - centre;
    return std::hypot(d.x, d.z) <= smashRadius && std::fabs(d.y) <= smashHeight;
}
inline float blastShare(Vec3 centre, Vec3 feet) {
    const Vec3 d = feet - centre;
    const float across = std::clamp(std::hypot(d.x, d.z) / smashRadius, 0.f, 1.f);
    return 1 - (1 - smashEdge) * across;
}
// Away from the middle across the ground (any way for one on top of it), and up.
inline Vec3 blastFlight(Vec3 centre, Vec3 feet) {
    Vec3 away{feet.x - centre.x, 0, feet.z - centre.z};
    const float across = length(away);
    away = across > .05f ? away / across : Vec3{1, 0, 0};
    return away * smashThrow + Vec3{0, smashLift, 0};
}
// The Mega Venom Blast: the venom fist charged and all three bars full, then
// both arms stretched out wide (the user, October 19: "i charge with grip
// then stretch both arms out to trigger it"). Out wide: the fists at least
// spreadSpan apart across, each within spreadHeight of the head's height (a
// T, not arms hanging down or held up), held spreadSeconds. A blast of
// megaRadius and megaHeight, twice the smash's damage, thrown farther.
constexpr float spreadSpan = 1.3f, spreadHeight = .6f, spreadSeconds = .1f, megaRadius = 10, megaHeight = 5,
                megaDamage = 2, megaThrow = 16, megaLift = 9;
// The arms out wide: each fist's place relative to the head, in the world's axes.
inline bool spread(Vec3 left, Vec3 right) {
    const Vec3 across = right - left;
    return finite(left) && finite(right) && std::hypot(across.x, across.z) >= spreadSpan &&
           std::fabs(left.y) <= spreadHeight && std::fabs(right.y) <= spreadHeight;
}
class Spread {
  public:
    // True once the arms have been out wide spreadSeconds; again only after
    // they came in.
    bool update(float seconds, bool wide) {
        const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
        if (!wide) {
            held_ = 0;
            fired_ = false;
            return false;
        }
        held_ += dt;
        if (fired_ || held_ < spreadSeconds)
            return false;
        fired_ = true;
        return true;
    }

  private:
    float held_{};
    bool fired_{};
};
inline bool inMega(Vec3 centre, Vec3 feet) {
    const Vec3 d = feet - centre;
    return std::hypot(d.x, d.z) <= megaRadius && std::fabs(d.y) <= megaHeight;
}
inline float megaShare(Vec3 centre, Vec3 feet) {
    const Vec3 d = feet - centre;
    const float across = std::clamp(std::hypot(d.x, d.z) / megaRadius, 0.f, 1.f);
    return megaDamage * (1 - (1 - smashEdge) * across);
}
inline Vec3 megaFlight(Vec3 centre, Vec3 feet) {
    const Vec3 f = blastFlight(centre, feet);
    return Vec3{f.x, 0, f.z} * (megaThrow / smashThrow) + Vec3{0, megaLift, 0};
}
// The Venom Jump (0.1.33, the user's choices, October 20): the charged venom
// fist punched straight UP while standing. Every enemy within the smash's
// blast is a venom blow of jumpDamage the smash's, thrown up at jumpLift (and
// a little away), and Miles goes up with them: jumpHeight over jumpSeconds,
// then he hangs there jumpHang for the blows on them. One bar.
constexpr float jumpDamage = .7f, jumpThrow = 2, jumpLift = 9, jumpHeight = 3.5f, jumpSeconds = .3f,
                jumpHang = 1.2f;
inline bool jumping(Vec3 fist) {
    const float speed = length(fist);
    return std::isfinite(speed) && speed >= smashSpeed && fist.y >= smashSteep * speed;
}
inline float jumpShare(Vec3 centre, Vec3 feet) {
    return jumpDamage * blastShare(centre, feet);
}
inline Vec3 jumpFlight(Vec3 centre, Vec3 feet) {
    const Vec3 f = blastFlight(centre, feet);
    return Vec3{f.x, 0, f.z} * (jumpThrow / smashThrow) + Vec3{0, jumpLift, 0};
}
// The game prompting for one of its own venom moves (the user, October 19:
// story moments "such as powering stuff up" must go on with the venom punch,
// the smash and the blast). Miles Morales's venom moves are its LB with a
// face button (Venom Punch, Smash, Jump, Dash) and both sticks pressed (the
// Mega Venom Blast); LB with RB is its web throw, not venom. XInput's bits,
// as prompt_buttons has them.
inline bool venomPrompt(uint16_t buttons) {
    constexpr uint16_t lb = 0x100, rb = 0x200, faces = 0x1000 | 0x2000 | 0x4000 | 0x8000, l3 = 0x40, r3 = 0x80;
    return ((buttons & lb) && !(buttons & rb) && (buttons & faces)) || ((buttons & l3) && (buttons & r3));
}
// A venom gesture made for such a prompt, the venom hand's web button held:
// the fist punched out or down (at least smashSpeed, relative to the head),
// or both arms out wide. The prompt's own buttons go to the game for it.
inline bool promptGesture(Vec3 fistVelocity, Vec3 left, Vec3 right) {
    const float speed = length(fistVelocity);
    return (std::isfinite(speed) && speed >= smashSpeed) || spread(left, right);
}
// The drop: from the air straight down at dropSpeed until he stands, the
// smash then; one that finds no ground within dropSeconds is called off.
class SmashDrop {
  public:
    static constexpr float dropSpeed = 20, dropSeconds = 2;
    struct Step {
        bool drive{};    // the player moves at `velocity` this step
        Vec3 velocity{};
        bool blast{};    // the smash goes off at `at`, this step
        bool calledOff{};
        Vec3 at{};
    };
    bool active() const {
        return dropping_ || ready_;
    }
    // The punch down: on the ground the smash goes off at the next step, in
    // the air he drops first.
    void start(Vec3 feet, bool grounded) {
        dropping_ = !grounded;
        ready_ = grounded;
        at_ = feet;
        elapsed_ = 0;
    }
    Step step(float seconds, Vec3 feet, bool grounded) {
        Step out;
        if (ready_ || (dropping_ && grounded)) {
            out.blast = true;
            out.at = ready_ ? at_ : feet;
            dropping_ = ready_ = false;
            return out;
        }
        if (!dropping_)
            return out;
        elapsed_ += std::isfinite(seconds) ? std::max(0.f, seconds) : 0.f;
        if (elapsed_ >= dropSeconds) {
            dropping_ = false;
            out.calledOff = true;
            return out;
        }
        out.drive = true;
        out.velocity = {0, -dropSpeed, 0};
        return out;
    }
    void cancel() {
        dropping_ = ready_ = false;
    }

  private:
    bool dropping_{}, ready_{};
    Vec3 at_{};
    float elapsed_{};
};
} // namespace spidy::venom_punch

#pragma once
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
// The meter after `amount` more, never past full.
inline float filled(float meter, float amount) {
    if (!std::isfinite(meter) || meter < 0)
        meter = 0;
    return std::min(fullMeter, meter + std::max(0.f, amount));
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
} // namespace spidy::venom_punch

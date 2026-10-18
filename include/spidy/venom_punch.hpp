#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Miles's venom punch (MilesVR). With the venom meter full, the right
// thumbstick's click held charges the right fist with venom; charged, the
// next blow that fist lands (a punch, or the web strike it opens at the
// marked enemy) is a venom blow and spends the meter. While the meter is
// full the stick's click is no LB + RB (the web pull of doors and grates):
// the user's design, October 18, so the two never clash. Below full it is
// the LB + RB it was.
namespace spidy::venom_punch {
// The meter: a float from 0 to fullMeter at +meterOffset in the player's
// Hero::HeroMeleeManager (its vtable, MilesMorales.exe 4.630.0.0). Found
// October 18 from a recording of the training sim: 100 at the start of a
// fight, down to 15.25 at a heal, back up 6.5 a landed hit to 100, down to
// 41.75 at the next heal (reports\venom-probe-20261018-234812.csv).
constexpr uintptr_t meleeManagerVtable = 0x3e122a8, meterOffset = 0x28c;
constexpr float fullMeter = 100;
// Within this long of an enemy near, the player is in a fight: the click is
// no LB + RB there (the game's web throw), full meter or not.
constexpr float fightCalmSeconds = 3;
inline bool full(float meter) {
    return std::isfinite(meter) && meter >= fullMeter - .01f;
}

// The right stick's click, read as the venom charge or as the pull.
class Charge {
  public:
    // Held this long, the fist is charged; let go charged, it stays charged
    // this long for the punch; the glow fades over the last fadeSeconds.
    static constexpr float chargeSeconds = .35f, keepSeconds = 1.5f, fadeSeconds = .3f;
    struct State {
        float glow{};   // 0..1: how bright the fist's venom shows
        bool armed{};   // the next blow of the right fist is venom
        bool charged{}; // it became armed at this update (a rumble)
        bool pull{};    // the click goes on as the pull's (PullPress's `mine`)
    };
    // seconds: since the previous update. play: VR play, the right fist free
    // to punch. full: the meter is full. held: the right stick's click.
    // other: the left stick's click (both together are the pad layer's
    // chord: no charge from it).
    State update(float seconds, bool play, bool full, bool held, bool other) {
        const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
        State out;
        out.pull = held && !full;
        if (!held)
            chord_ = false;
        chord_ = chord_ || (held && other);
        if (!play || !full || chord_) {
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
    // The venom blow landed: the charge is gone, and a click still held
    // charges nothing more until it is let go.
    void spent() {
        charge_ = keep_ = 0;
        spent_ = true;
    }

  private:
    float charge_{}, keep_{};
    bool chord_{}, spent_{};
};
} // namespace spidy::venom_punch

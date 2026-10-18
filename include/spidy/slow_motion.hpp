#pragma once
#include <cstdint>

// Slow motion as Blade & Sorcery has it: a click of the left thumbstick slows
// the game's time, another click ends it. Focus (the meter on the left wrist)
// drains while it lasts, in real time, ends it when it runs out, and refills
// once it has ended. Time eases in and out along an S-curve in log time, so
// the world decelerates and recovers without a jolt; the headset and the
// controllers keep real time throughout. game_time.hpp slows the game.
namespace spidy {
struct SlowMotionTuning {
    // The game's time at full slow motion, per real second.
    float scale = .3f;
    // Real seconds easing into it and back out.
    float enterSeconds = .4f, exitSeconds = .55f;
    // Real seconds a full meter lasts, and refilling takes from empty to full;
    // refilling starts this long after slow motion ends.
    float drainSeconds = 12, refillSeconds = 12, refillDelay = 1.2f;
    // A press with less focus than this is refused.
    float minimum = .15f;
};
// What the headset shows of it (an eye image's share: NativeEyeFrame).
struct SlowMotionView {
    float blend{};     // 0..1: how far into slow motion, eased (the image's recolouring)
    float focus = 1;   // 0..1: the meter
    float meter{};     // 0..1: the meter's opacity (it shows while focus is spent)
    float ripple = -1; // 0..1: a ring crossing the view after a start or an end; below 0 none
    float warning{};   // 0..1: a red flash fading after a refused press or an empty meter
    bool entering{};   // the ring follows a start (else an end)
    bool active{};     // slow motion is on (easing in or holding)
};
class SlowMotion {
  public:
    // A ring crosses the view this long after a start or an end; the meter
    // flashes red this long after a refusal or running empty; a full meter
    // stays this long after it filled or a refusal, fading in and out.
    static constexpr float rippleSeconds = .5f, warningSeconds = .6f, meterHold = 1.5f;
    static constexpr float meterIn = .15f, meterOut = .5f;
    enum class Event : std::uint8_t {
        none,
        started,     // a press started it
        stopped,     // a press ended it
        emptied,     // focus ran out
        refused,     // a press with too little focus
        interrupted, // play stopped (a menu, the game screen, the flat screen)
    };
    explicit SlowMotion(const SlowMotionTuning& tuning = {});
    // One headset frame: `seconds` of real time since the last one, `pressed`
    // the button was pressed (StickPress), `allowed` while the player plays in
    // VR. Returns what happened in it.
    Event update(float seconds, bool pressed, bool allowed);
    bool active() const {
        return active_;
    }
    // 0..1: how far into slow motion, along a smoother step.
    float blend() const;
    // The game's time per real second now: 1, down to tuning.scale.
    float timeScale() const;
    float focus() const {
        return focus_;
    }
    SlowMotionView view() const;
    const SlowMotionTuning& tuning() const {
        return tuning_;
    }
    // A full meter in real time, as a session starts.
    void reset();

  private:
    SlowMotionTuning tuning_;
    bool active_{};
    float progress_{}; // 0..1 linear in time toward active_
    float focus_ = 1, meter_{};
    // Real seconds since slow motion ended (refill delay), since the last
    // start or end, since a warning, and since the meter was last needed.
    float idle_{}, sinceChange_ = 1e9f, sinceWarning_ = 1e9f, sinceNeeded_ = 1e9f;
};
// Moments: the game's time slows by itself for an instant where a fight or
// a swing deserves a look, and eases back. They cost no focus and need no
// press; the look is slow motion's own, fainter (view()).
enum class Moment : std::uint8_t {
    blow,    // a finisher, an uppercut that lifts him, a slam
    called,  // a called blow (web_strike.hpp's Cues)
    airPull, // a thug the web brought to a player in the air arrives within reach
    dodge,   // a dodge the game itself slowed time for: its perfect dodge
    // An enemy the player's blow knocked out: a light one, so that he knows
    // this one is done. (The user, October 12: "its not clear when they're
    // dead (maybe a light slow motion or visual)".)
    knockout,
};
inline constexpr unsigned momentKinds = 5;
struct MomentTuning {
    // The game's time in a moment, per real second (a knockout's: `light`);
    // real seconds easing into it and back out.
    float scale = .3f, light = .55f, in = .07f, out = .35f;
    // Real seconds each kind holds (Moment's order).
    // A thug brought to a player in the air: long enough to land a blow.
    float hold[momentKinds] = {.3f, .35f, 1.2f, .6f, .2f};
    // No moment starts this soon after the last one ended: a flurry of blows
    // is not one long slow motion. A dodge's starts all the same.
    float rest = 1.5f;
};
class SlowMoments {
  public:
    explicit SlowMoments(const MomentTuning& tuning = {});
    // Starts one, or with one going keeps it up for as long as this one
    // holds. False when the rest after the last refuses it.
    bool start(Moment kind);
    // One headset frame, `seconds` (real) long; ends at once without play.
    void update(float seconds, bool allowed);
    // The game's time per real second now: 1, down to the tuning's scale.
    float timeScale() const;
    // 0..1: how far into the moment, eased.
    float blend() const;
    bool active() const {
        return left_ > 0 || level_ > 0;
    }
    // Real seconds since the latest start; large before any.
    float sinceStart() const {
        return sinceStart_;
    }
    std::uint32_t count(Moment kind) const {
        return counts_[static_cast<unsigned>(kind) % momentKinds];
    }
    // Slow motion's view with the moment in it: the stronger look of the two,
    // and a ring across the view as a moment starts (none for a light one,
    // whose look is fainter too).
    SlowMotionView view(SlowMotionView manual) const;
    void reset();

  private:
    MomentTuning tuning_;
    float left_{}, level_{}, sinceEnd_ = 1e9f, sinceStart_ = 1e9f;
    // The time scale the moment going asks for: the deepest of those begun in it.
    float scale_ = 1;
    std::uint32_t counts_[momentKinds]{};
};
// A press of one thumbstick's click that is not half of clicking both (until
// October 12 that switched between VR and the flat screen; now it is neither
// stick's press): it counts once the stick has
// been held chordSeconds without the other, or when it is let go sooner.
// While unfocused nothing counts, and a stick held through it must be let go
// before it counts again.
class StickPress {
  public:
    static constexpr float chordSeconds = .12f;
    bool update(float seconds, bool focused, bool mine, bool other);

  private:
    bool down_{}, decided_{}, chord_{};
    float held_{};
};
} // namespace spidy

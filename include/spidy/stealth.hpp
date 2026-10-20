#pragma once
#include "math.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

// Stealth takedowns (MilesVR 0.1.66, the user's plan, October 20): behind an
// enemy, reach out and grab him with the grip - the hand on his back or
// shoulder - then yank toward you. That presses the game's own takedown
// button (X, its Square; Y until 0.1.69, which is the game's web strike
// takedown from a perch and never a ground one), so the game does its
// takedown and decides whether one is on
// (an enemy who saw you gets none). The mod cannot read whether he saw you:
// it marks the nearest enemy you stand behind within reach, out of a fight.
namespace spidy::stealth {
// Within `reach` across and `rise` up or down of his feet, and behind him: the
// player's place from him at least `behindCos` (a cosine) against the way he
// faces.
constexpr float reach = 1.5f, rise = 1.f, behindCos = .35f; // reach 1.8 until 0.1.68
// A hand within `onBack` of his middle is on his back.
constexpr float onBack = .6f;
// A perch takedown (MilesVR 0.1.70, the user: "i want the perch silent
// takedown implemented"): the player on his feet at least `perchBelow`
// above an enemy and within `perchReach` of him across, a hand pointing at
// him within `perchAim` (a cosine): the game's web strike takedown, its Y.
constexpr float perchBelow = 1.5f, perchReach = 15, perchAim = .97f;
// A hand from `from` along `way` (unit) points at `target` within perchAim.
inline bool aimsAt(Vec3 from, Vec3 way, Vec3 target) {
    const Vec3 to = target - from;
    const float d = length(to);
    return d > .1f && std::isfinite(d) && finite(way) && dot(way, to / d) >= perchAim;
}
// Venom::takedownHands: a bit per hand pointing at him, and this bit for a
// perch takedown (the game's Y; without it a ground one, its X).
constexpr unsigned perchBit = 0x100;
// No mark within `calmSeconds` of a blow of the player's: in a fight Y is the
// game's web strike.
constexpr float calmSeconds = 5;
inline bool behind(Vec3 enemyFeet, Vec3 enemyForward, Vec3 playerFeet) {
    const Vec3 away{playerFeet.x - enemyFeet.x, 0, playerFeet.z - enemyFeet.z};
    const Vec3 facing{enemyForward.x, 0, enemyForward.z};
    const float a = length(away), f = length(facing);
    if (!(a > .05f) || !(f > .1f) || !std::isfinite(a) || !std::isfinite(f))
        return false;
    return dot(away / a, facing / f) <= -behindCos;
}
// One hand's grab and yank. The grip goes down while a takedown is marked:
// the hand is the takedown's until the grip is let go (`hold`: its web is not
// shot, so the web grab neither catches nor flings him). Within `window` of
// the grab, the hand pulled toward the head at `yankSpeed` or faster fires
// it, once. (0.1.66 wanted the hand on his back, within onBack of his middle,
// and a yank of 1.2 m/s: the user, trying it, "they would just keep seeing me"
// while that was lined up - one takedown in a session. Since 0.1.67 any hand,
// anywhere, while the mark shows, and a gentler yank.)
class Takedown {
  public:
    static constexpr float gripOn = .6f, gripOff = .35f, yankSpeed = .9f, window = .8f;
    struct Out {
        bool hold{}; // the hand's grip is the takedown's
        bool fire{}; // the yank: press the game's takedown now
        // The hand went away from the head instead, a push toward him
        // (MilesVR 0.1.71: from a perch the web strike takedown, the game's
        // green Triangle, where the yank is its stealth webbing takedown, the
        // purple Square).
        bool push{};
    };
    // seconds: since the previous update. onBack: a takedown is marked (the
    // name from 0.1.66, when the hand had to be on his back). grip: its grip,
    // 0..1. hand: the hand relative to the head, metres (any axes, the same
    // each update).
    Out update(float seconds, bool onBack, float grip, Vec3 hand) {
        const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
        const bool down = std::isfinite(grip) && grip >= (down_ ? gripOff : gripOn);
        const bool press = down && !down_;
        down_ = down;
        Out out;
        Vec3 velocity{};
        if (finite(hand) && seen_ && dt > 0)
            velocity = (hand - before_) / dt;
        before_ = hand;
        seen_ = finite(hand);
        speed_ = speed_ * .5f + velocity * .5f;
        if (press && onBack) {
            grabbing_ = true;
            fired_ = false;
            since_ = 0;
        }
        if (!down)
            grabbing_ = false;
        if (!grabbing_)
            return out;
        out.hold = true;
        since_ += dt;
        const float toward = length(hand) > .05f ? dot(speed_, hand * (-1.f / length(hand))) : 0.f;
        if (!fired_ && since_ <= window && (toward >= yankSpeed || -toward >= yankSpeed)) {
            fired_ = true;
            out.fire = true;
            out.push = toward < 0;
            ++fires_;
        }
        return out;
    }
    uint32_t fires() const {
        return fires_;
    }

  private:
    bool down_{}, grabbing_{}, fired_{}, seen_{};
    Vec3 before_{}, speed_{};
    float since_{};
    uint32_t fires_{};
};
} // namespace spidy::stealth

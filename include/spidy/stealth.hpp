#pragma once
#include "math.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

// Stealth takedowns, Batman: Arkham Shadow's way with Spider-Man's finish
// (MilesVR 0.1.73; the user, October 20: "scrap this stealth system i want a
// batman arkham shadow style" - "but i also still want it to be less violent
// as its spiderman" - "pushing forward toward him to start the takedown").
//
// Ground, the web wrap: behind an enemy who has not seen you, squeeze both
// grips and push both hands forward toward him: the game's own takedown (its
// X) plays out on the body - the hero walked in and turned to his back - while
// the view stays the headset's (0.1.75; 0.1.73's own webbing hits sprayed webs
// and stuck him to the ground).
//
// Perch, the web hang: from a ledge or lamp post above him, point both hands
// at him, squeeze both grips and pull up hand over hand: the game's own
// stealth web takedown (its X, the purple Square) leaves him hanging.
//
// No prompts: a soft buzz and a faint blue glow on the hands when a takedown
// is on. The mod cannot read whether he saw you; the game's webbing and its
// takedown decide.
namespace spidy::stealth {
// The ground's: within `reach` across and `rise` up or down of his feet, and
// behind him - the player's place from him at least `behindCos` (a cosine)
// against the way he faces.
// 0.1.75 (the user: "the radius needs to be increased"): 3 m, the hero walked
// in the rest of the way as the game's takedown goes in.
constexpr float reach = 3.f, rise = 1.f, behindCos = .35f;
// No takedown within `calmSeconds` of a blow of the player's.
constexpr float calmSeconds = 5;
inline bool behind(Vec3 enemyFeet, Vec3 enemyForward, Vec3 playerFeet) {
    const Vec3 away{playerFeet.x - enemyFeet.x, 0, playerFeet.z - enemyFeet.z};
    const Vec3 facing{enemyForward.x, 0, enemyForward.z};
    const float a = length(away), f = length(facing);
    if (!(a > .05f) || !(f > .1f) || !std::isfinite(a) || !std::isfinite(f))
        return false;
    return dot(away / a, facing / f) <= -behindCos;
}
// The perch's: the player on his feet at least `perchBelow` above an enemy
// within `perchReach` across, a hand pointing at him within `perchAim` (a
// cosine).
constexpr float perchBelow = 1.5f, perchReach = 15, perchAim = .97f;
inline bool aimsAt(Vec3 from, Vec3 way, Vec3 target) {
    const Vec3 to = target - from;
    const float d = length(to);
    return d > .1f && std::isfinite(d) && finite(way) && dot(way, to / d) >= perchAim;
}
// Venom::takedownHands: a bit per hand pointing at him from a perch, and
// perchBit for a perch takedown (without it, a ground one).
constexpr unsigned perchBit = 0x100;

// The grips, with the hysteresis a held grip has.
constexpr float gripOn = .6f, gripOff = .35f;
// A hand's speed along the line from the head through it, m/s: positive
// away from the head (a push), negative toward it (a pull), smoothed.
class Reach {
  public:
    float update(float seconds, Vec3 hand) {
        const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
        Vec3 velocity{};
        if (finite(hand) && seen_ && dt > 0)
            velocity = (hand - before_) / dt;
        before_ = hand;
        seen_ = finite(hand);
        speed_ = speed_ * .5f + velocity * .5f;
        const float d = length(hand);
        return d > .05f && std::isfinite(d) ? dot(speed_, hand / d) : 0.f;
    }

  private:
    Vec3 before_{}, speed_{};
    bool seen_{};
};

// The ground's web wrap. `ready`: a takedown is on (behind him, close
// enough). Both grips down and both hands pushed away from the head at
// pushSpeed within pushWindow of each other starts it; held for `seconds`
// with both grips it is done, a webbing hit every hitEvery meanwhile. A grip
// let go first loses it. While a takedown is on, a grip pressed is held back
// from the swing for up to `pending` (the push shoots no web), and held from
// the start until both grips are let go after it.
class WebWrap {
  public:
    static constexpr float seconds = 1.5f, hitEvery = .25f, pushSpeed = .8f, pushWindow = .4f, pending = .6f;
    struct Out {
        bool hold{};  // the grips are the takedown's: their webs are not shot
        bool start{}; // the push: the wrap begins
        bool hit{};   // a webbing hit now
        bool done{};  // held long enough: he is webbed up
        bool lost{};  // a grip let go first
        float progress{};
    };
    Out update(float dt, bool ready, float left, float right, Vec3 leftHand, Vec3 rightHand) {
        const float t = std::isfinite(dt) ? std::clamp(dt, 0.f, .1f) : 0.f;
        const float grips[2]{left, right};
        const float away[2]{reach_[0].update(t, leftHand), reach_[1].update(t, rightHand)};
        bool any{}, both = true;
        for (int i = 0; i < 2; ++i) {
            const bool down = std::isfinite(grips[i]) && grips[i] >= (down_[i] ? gripOff : gripOn);
            heldFor_[i] = down ? heldFor_[i] + t : 0.f;
            down_[i] = down;
            pushedAgo_[i] = down && away[i] >= pushSpeed ? 0.f : std::min(pushedAgo_[i] + t, 10.f);
            any = any || down;
            both = both && down;
        }
        Out out;
        if (spent_) {
            out.hold = any;
            spent_ = any;
            return out;
        }
        if (wrapping_) {
            if (!both) {
                wrapping_ = false;
                out.lost = true;
                spent_ = any;
                out.hold = any;
                return out;
            }
            out.hold = true;
            time_ += t;
            out.progress = std::min(1.f, time_ / seconds);
            if (time_ >= nextHit_) {
                out.hit = true;
                nextHit_ += hitEvery;
            }
            if (time_ >= seconds) {
                out.done = true;
                wrapping_ = false;
                spent_ = true;
            }
            return out;
        }
        if (ready && both && pushedAgo_[0] <= pushWindow && pushedAgo_[1] <= pushWindow) {
            wrapping_ = true;
            time_ = 0;
            nextHit_ = 0;
            out.start = out.hold = true;
            return out;
        }
        out.hold = ready && any && std::max(heldFor_[0], heldFor_[1]) <= pending;
        return out;
    }
    bool wrapping() const {
        return wrapping_;
    }

  private:
    Reach reach_[2];
    bool down_[2]{}, wrapping_{}, spent_{};
    float heldFor_[2]{}, pushedAgo_[2]{10, 10}, time_{}, nextHit_{};
};

// The perch's web hang. `ready`: both hands point at an enemy below. Both
// grips down there, then each hand pulled toward the head at pullSpeed - hand
// over hand, the two pulls within pullWindow - fires it, once. The grips are
// held back from the swing from the moment both go down there until both are
// let go.
class WebHang {
  public:
    static constexpr float pullSpeed = .9f, pullWindow = 2.f;
    struct Out {
        bool hold{};
        bool fire{};
    };
    Out update(float dt, bool ready, float left, float right, Vec3 leftHand, Vec3 rightHand) {
        const float t = std::isfinite(dt) ? std::clamp(dt, 0.f, .1f) : 0.f;
        const float grips[2]{left, right};
        const float away[2]{reach_[0].update(t, leftHand), reach_[1].update(t, rightHand)};
        bool any{}, both = true;
        for (int i = 0; i < 2; ++i) {
            const bool down = std::isfinite(grips[i]) && grips[i] >= (down_[i] ? gripOff : gripOn);
            down_[i] = down;
            any = any || down;
            both = both && down;
        }
        Out out;
        if (!armed_ && ready && both && !spent_) {
            armed_ = true;
            pulled_[0] = pulled_[1] = false;
            since_ = 0;
        }
        if (armed_) {
            since_ += t;
            for (int i = 0; i < 2; ++i)
                pulled_[i] = pulled_[i] || -away[i] >= pullSpeed;
            if (pulled_[0] && pulled_[1]) {
                out.fire = true;
                armed_ = false;
                spent_ = true;
            } else if (!both || since_ > pullWindow) {
                armed_ = false;
                spent_ = any;
            }
        }
        if (spent_ && !any)
            spent_ = false;
        out.hold = armed_ || spent_;
        return out;
    }

  private:
    Reach reach_[2];
    bool down_[2]{}, armed_{}, spent_{}, pulled_[2]{};
    float since_{};
};
} // namespace spidy::stealth

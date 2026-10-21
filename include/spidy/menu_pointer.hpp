#pragma once
// MilesVR: the Touch controllers as a mouse on the game screen. While a menu is on the screen in the
// headset, a hand pointed at it puts the game's mouse cursor where it points (once its trigger has woken
// the pointer: see Pointer); its trigger is the left
// mouse button and its stick, pushed up or down, the wheel. The game draws its own cursor in its
// menus, so the player sees where they point. The other controls stay the Xbox controller's.
#include "math.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace spidy::menu_pointer {

// The screen as xr_session::presentation shows it: a quad at `pose`, facing its +z, `width` metres
// wide and width / aspect high.
struct Screen {
    Pose pose{};
    float width{}, aspect{16.f / 9};
};

// Where a ray from `from` along its -z meets the front of the screen: (0, 0) its top left, (1, 1)
// its bottom right. None behind the hand, from behind the screen or off its edges.
inline std::optional<std::array<float, 2>> hit(const Screen& s, const Pose& from) {
    if (!(s.width > 0) || !(s.aspect > 0) || !finite(s.pose.position) || !finite(from.position))
        return std::nullopt;
    const Vec3 normal = s.pose.orientation.rotate({0, 0, 1});
    const Vec3 right = s.pose.orientation.rotate({1, 0, 0}), up = s.pose.orientation.rotate({0, 1, 0});
    const Vec3 direction = from.orientation.rotate({0, 0, -1});
    const float facing = dot(direction, normal);
    if (!(facing < -1e-4f))
        return std::nullopt; // parallel to it, or pointing at its back
    const float t = dot(s.pose.position - from.position, normal) / facing;
    if (!(t > 0) || t > 50)
        return std::nullopt;
    const Vec3 at = from.position + direction * t - s.pose.position;
    const float height = s.width / s.aspect;
    const float u = dot(at, right) / s.width + .5f, v = .5f - dot(at, up) / height;
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || u > 1 || v < 0 || v > 1)
        return std::nullopt;
    return std::array<float, 2>{u, v};
}

struct Hand {
    bool valid{};
    Pose ray{};
    float trigger{}, stickY{};
};

// What the frame does with the mouse, and which of the hands' controls it took from the Xbox
// controller: the pointing hand's trigger and stick's y.
struct Output {
    bool pointing{};
    unsigned hand{};
    std::array<float, 2> at{};
    bool press{}, release{};
    int wheel{}; // notches, + away from the player (scroll up)
    std::array<bool, 2> tookTrigger{}, tookStick{};
};

// MilesVR 0.1.78: the pointer SLEEPS until it is wanted. The game turns its button prompts to the
// keyboard's whenever the mouse moves and back to the controller's on a pad press, so a hand that only
// happened to point at the screen (controllers raised to the headset) made them flicker and took the
// trigger and stick from the menus. Asleep it moves nothing and takes nothing. A trigger pulled while
// pointing at the screen wakes it, and that pull clicks; any other control used (a face button, a grip,
// a stick pushed sideways or clicked, the other hand's stick), or no hand on the screen, puts it back to
// sleep. Awake, a hand's small shake does not move the cursor.
class Pointer {
  public:
    // The trigger presses past `pressAt` and lets go below `releaseAt`; a stick pushed past
    // `wheelAt` scrolls a notch, and another every `repeatSeconds` it stays there. Awake, the cursor
    // follows the hand once it is `steady` (of the screen's width) from where it was last put.
    static constexpr float pressAt = .6f, releaseAt = .35f, wheelAt = .55f, repeatSeconds = .12f, steady = .004f;

    // `otherInput`: a control the pointer does not use was used this frame (see above).
    Output update(bool enabled, const Screen& screen, const std::array<Hand, 2>& hands, float seconds,
                  bool otherInput = false) {
        Output out;
        std::array<std::optional<std::array<float, 2>>, 2> hits{};
        if (enabled)
            for (unsigned i = 0; i < 2; ++i)
                if (hands[i].valid)
                    hits[i] = hit(screen, hands[i].ray);
        std::array<bool, 2> pulled{};
        for (unsigned i = 0; i < 2; ++i) {
            const float trigger = std::isfinite(hands[i].trigger) ? hands[i].trigger : 0;
            pulled[i] = trigger >= pressAt && lastTrigger_[i] < pressAt;
            lastTrigger_[i] = trigger;
        }
        if (!awake_) {
            // A trigger pulled at the screen wakes it, on that hand, and clicks.
            for (unsigned i : {hand_, 1 - hand_})
                if (hits[i] && pulled[i] && !otherInput) {
                    awake_ = true;
                    hand_ = i;
                    break;
                }
            if (!awake_)
                return out;
            moved_ = true;
        } else {
            // The hand that pointed last keeps the cursor while it points; else the other, if it does.
            if (!hits[hand_] && hits[1 - hand_])
                hand_ = 1 - hand_;
            // A trigger pulled on the other hand, pointing too, takes the cursor.
            const unsigned other = 1 - hand_;
            if (hits[other] && !held_ && pulled[other]) {
                hand_ = other;
                moved_ = true;
            }
            const float otherStick =
                std::isfinite(hands[other].stickY) ? std::abs(hands[other].stickY) : 0;
            if (!hits[hand_] || (otherInput && !held_) || (otherStick >= wheelAt && !held_)) {
                // Off the screen, or the controller wanted: a held button lets go, so a drag never
                // sticks, and it sleeps.
                out.release = held_;
                held_ = awake_ = false;
                repeat_ = 0;
                return out;
            }
        }
        out.pointing = true;
        out.hand = hand_;
        const auto at = *hits[hand_];
        // Only a real move, or a drag, moves the cursor: a still hand's shake would turn the prompts.
        if (moved_ || held_ || std::abs(at[0] - put_[0]) > steady ||
            std::abs(at[1] - put_[1]) * screen.aspect > steady)
            put_ = at;
        moved_ = false;
        out.at = put_;
        out.tookTrigger[hand_] = out.tookStick[hand_] = true;
        const float trigger = lastTrigger_[hand_];
        if (!held_ && trigger >= pressAt)
            out.press = held_ = true;
        else if (held_ && trigger < releaseAt) {
            out.release = true;
            held_ = false;
        }
        const float y = std::isfinite(hands[hand_].stickY) ? std::clamp(hands[hand_].stickY, -1.f, 1.f) : 0;
        if (std::abs(y) < wheelAt) {
            repeat_ = 0;
        } else {
            repeat_ -= seconds;
            if (repeat_ <= 0) {
                out.wheel = y > 0 ? 1 : -1;
                repeat_ = repeatSeconds;
            }
        }
        return out;
    }
    bool held() const {
        return held_;
    }
    bool awake() const {
        return awake_;
    }

  private:
    unsigned hand_ = 1; // the right hand first
    bool held_{}, awake_{}, moved_{};
    float repeat_{};
    std::array<float, 2> lastTrigger_{}, put_{};
};

} // namespace spidy::menu_pointer

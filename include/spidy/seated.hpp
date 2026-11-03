#pragma once
#include "tracking.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

// Seated play and the player's own height (the SPIDY VR tab's SEATED and
// HEIGHT; the user, October 23: "add proper height calibration or override for
// seated mode"; players on the Discord, October 14: "options like in bonelab
// where you measure yourself and then enter your measurements instead of
// tposing", and "like blade and sorcery where the game will auto measure you
// ... but you can edit the results").
//
// HEIGHT is the player's height, or AUTO. Set, it sizes Spider-Man's body in
// place of the T-pose calibration's eye height or the headset's running one
// (native_body), and it is the height SEATED raises a seated player to.
//
// SEATED: a seated player's headset is half a metre lower than their eyes are
// standing, and everything Spidy places from the tracking space - the game's
// camera, the body under it, the hands and the webs from them - came out that
// much too low: Spider-Man crouched, his eyes at a standing man's chest, and
// a T-pose calibration took the seated eyes for standing ones. Seated, the
// tracking space is raised by the difference between the standing eye height
// and the seated one, which is measured over the first second of play and
// again after every recenter of the headset. Only what is placed in the game's
// world rises: the headset's own poses, which the runtime reprojects the
// images with, stay as tracked, and the gestures measured from the head (a
// hand at the temple or the chest, a back pocket) are where they were.
namespace spidy::seated {
// A player's eyes stand this share of their height above the floor.
inline constexpr float eyeShare = .936f;
// The standing eye height when nothing gives the player's own: a 1.76 m
// player's.
inline constexpr float defaultEye = 1.65f;
// HEIGHT's range, centimetres; 0 is AUTO.
inline constexpr int minHeightCm = 120, maxHeightCm = 220;
// A T-pose calibration's eye height below this is a seated player's (one made
// sitting before SEATED raised them), not a standing one to raise them to.
inline constexpr float lowestStandingEye = 1.35f;
inline float eyeHeight(int heightCm) {
    return static_cast<float>(heightCm) * eyeShare / 100;
}
// The standing eye height, metres: HEIGHT's when set, else a T-pose
// calibration's made standing, else `fallback`.
inline float standingEye(int heightCm, float calibrated, float fallback) {
    if (heightCm >= minHeightCm && heightCm <= maxHeightCm)
        return eyeHeight(heightCm);
    if (std::isfinite(calibrated) && calibrated >= lowestStandingEye)
        return calibrated;
    return fallback;
}
// How far a seated player's tracking space is raised.
class Lift {
  public:
    // Seconds of play a seated eye height is measured over; the most the
    // space is raised, metres; how long a new lift takes to come in.
    static constexpr float measureSeconds = 1, maxLift = 1.2f, easeSeconds = .3f;
    // seconds: since the previous update. on: SEATED. play: tracked VR play.
    // headY: the headset's height above the floor, tracked. recentered: the
    // headset was recentered this frame. standing: the eye height to raise
    // the player to (standingEye). Returns the lift, metres.
    float update(float seconds, bool on, bool play, float headY, bool recentered, float standing) {
        const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
        if (!on) {
            // Back on the floor, and measured again when SEATED comes on.
            measuring_ = true;
            sum_ = time_ = seatedEye_ = lift_ = 0;
            count_ = 0;
            return lift_;
        }
        if (recentered) {
            measuring_ = true;
            sum_ = time_ = 0;
            count_ = 0;
        }
        if (measuring_ && play && std::isfinite(headY) && headY > .3f && headY < 2.5f) {
            sum_ += headY;
            ++count_;
            time_ += dt;
            if (time_ >= measureSeconds) {
                seatedEye_ = sum_ / static_cast<float>(count_);
                measuring_ = false;
                ++measures_;
            }
        }
        const float wanted =
            seatedEye_ > 0 && std::isfinite(standing) ? std::clamp(standing - seatedEye_, 0.f, maxLift) : 0.f;
        lift_ += (wanted - lift_) * std::min(1.f, dt / easeSeconds);
        if (std::abs(wanted - lift_) < .001f)
            lift_ = wanted;
        return lift_;
    }
    float lift() const {
        return lift_;
    }
    // The seated eye height last measured, metres; 0 before one.
    float seatedEye() const {
        return seatedEye_;
    }
    // Seated eye heights measured so far.
    uint32_t measures() const {
        return measures_;
    }

  private:
    bool measuring_ = true;
    float sum_{}, time_{}, seatedEye_{}, lift_{};
    uint32_t count_{}, measures_{};
};
// A frame's head, eyes and hands raised by `lift`, for what places them in the
// game's world. The frame the headset's layer is submitted with keeps its own.
inline XrFrame raised(XrFrame f, float lift) {
    if (!(lift > 0) || !std::isfinite(lift))
        return f;
    f.head.position.y += lift;
    f.gaze.position.y += lift;
    for (auto& eye : f.eyes)
        eye.pose.position.y += lift;
    for (auto& hand : f.hands) {
        hand.grip.position.y += lift;
        hand.aim.position.y += lift;
    }
    return f;
}
} // namespace spidy::seated

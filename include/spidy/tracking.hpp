#pragma once
#include "math.hpp"
#include <cstdint>

namespace spidy {
struct TrackedHand {
    Pose aim, grip;
    bool valid{};
    float trigger{}, squeeze{}, stickX{}, stickY{};
    bool stickClick{};
};
struct EyeFov {
    float left{}, right{}, down{}, up{};
};
struct TrackedEye {
    Pose pose{};
    EyeFov fov{};
    unsigned width{}, height{};
};
// Face and menu buttons of the VR controllers (XrFrame::buttons). On Touch
// controllers A and B are on the right, X, Y and the menu button on the left.
// The Steam Frame's have all five on the right, and on the left a View button
// and a D-pad, which no other controller has (controller_layout.hpp).
enum ControllerButton : uint32_t {
    buttonA = 1,
    buttonB = 2,
    buttonX = 4,
    buttonY = 8,
    buttonMenu = 16,
    buttonView = 32,
    buttonDpadUp = 64,
    buttonDpadDown = 128,
    buttonDpadLeft = 256,
    buttonDpadRight = 512,
};
struct XrFrame {
    Pose head{};
    std::array<TrackedHand, 2> hands{};
    std::array<TrackedEye, 2> eyes{};
    std::int64_t predictedDisplayTime{};
    float seconds = 1.f / 90;
    bool focused{}, valid{}, jump{}, reset{}, recentered{};
    uint32_t buttons{};
};
inline bool validTrackedPose(Pose p) {
    const auto q = p.orientation;
    const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return finite(p.position) && std::isfinite(n) && std::abs(n - 1) < .001f;
}
// Spider-Man's web shooter on the hand a controller's OpenXR grip pose holds:
// on the palm's side of the wrist the player's body draws there
// (body::Config::wristFromGrip has that wrist 9 cm from the grip point toward
// the forearm, the grip's +y, and 2 cm toward the back of the hand), looking
// where the hand points - from the wrist out past the knuckles, the grip's -y
// - with the thumb's side up, the grip's -z. The grip's +x is normal to the
// palm and to the right on both hands: out of a left palm, into a right one.
// hand: 0 the left, 1 the right.
//
// Webs leave from here, and web balls, picks and pointing aim along it. Until
// October 15 they aimed along the controller's aim pose, which Meta's runtime
// (and Virtual Desktop's, calibrated to it) turns 30 degrees from the hand
// toward the thumb: with the palm down, as a web is shot, 30 degrees inward
// of where the hand pointed ("it feels off to the side a bit").
inline Pose webShooter(Pose grip, unsigned hand) {
    const float palm = hand == 0 ? .008f : -.008f;
    return compose(grip, {{palm, .08f, 0}, {-.70710678f, 0, 0, .70710678f}});
}
} // namespace spidy

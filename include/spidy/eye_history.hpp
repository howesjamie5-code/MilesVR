#pragma once
// SM2 VR (port of SpidyVR to Marvel's Spider-Man 2), GPLv3; SpidyVR by Ilya Mezerowsky.
//
// Eye history (live.txt "history"): each new eye pair is blended with the one
// before it, turned to the new pair's view, so the city's fine detail (window
// grids, railings, far edges) that crawls and shimmers from one picture to the
// next settles. No game address or game setting is involved: it is a pass over
// the eye images the bridge already stages (native_eye_gpu.cpp).
//
// The previous picture is turned, not moved: a pixel's direction in the new
// eye is looked up in the old eye's lens (historyReprojection), which is exact
// for anything far away, where the shimmer is, whatever the head's turn. What
// moved nearer (a swing's travel, a passing car) does not line up, so the old
// colour is first held to the range of the new pixel's 3 x 3 neighbourhood,
// as temporal anti-aliasing does: a ghost cannot leave the new picture's
// colours there. A cut (a turn of more than maximumHistoryTurn between two
// pairs) or a pair without its view starts the history again.
#include <array>
#include <cmath>
#include <cstdint>

namespace spidy::eye_history {
// Update 21: up to 95 (was 80). From 50% on the 3 x 3 clamp is also widened
// (historyLoosen), so the top of the range is plainly heavy: very calm, with
// visible trails behind motion. A cut still starts the history again.
inline constexpr uint32_t maximumPercent = 95;
inline constexpr uint32_t loosenFrom = 50;
inline constexpr float maximumHistoryTurn = 0.26f; // radians, 15 degrees
// One eye's view as the game was told to render it: the world matrix of
// game_tracking's worldPose (x right, y the eye's down, z its forward, each
// as 3 floats at 0, 4 and 8) and the lens's tangents (left, right, down, up;
// left and down negative).
struct EyeView {
    std::array<float, 16> world{};
    std::array<float, 4> tangents{};
};
// The homography (3 x 3, row-major) that takes a pixel of the current eye, as
// (u, v, 1) with u, v in 0-1 from the top left, to the previous eye's image:
// (U, V, W), the place U/W, V/W, in front of the eye while W > 0. False for a
// degenerate lens.
inline bool historyReprojection(const EyeView& previous, const EyeView& current, std::array<float, 9>& out) {
    const auto& c = current.tangents;
    const auto& p = previous.tangents;
    if (!(c[1] > c[0]) || !(c[3] > c[2]) || !(p[1] > p[0]) || !(p[3] > p[2]))
        return false;
    // Each eye's axes in the world in the OpenXR sense: right, up, back.
    auto axes = [](const EyeView& e, int a) {
        const float sign = a == 0 ? 1.f : -1.f;
        return std::array<float, 3>{sign * e.world[a * 4], sign * e.world[a * 4 + 1], sign * e.world[a * 4 + 2]};
    };
    float turn[3][3]; // previous eye's frame from the current eye's
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const auto a = axes(previous, i), b = axes(current, j);
            turn[i][j] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }
    // (u, v, 1) to the current eye's direction (x right, y up, z back).
    const float from[3][3] = {{c[1] - c[0], 0, c[0]}, {0, -(c[3] - c[2]), c[3]}, {0, 0, -1}};
    // A direction in the previous eye to (U, V, W).
    const float w = p[1] - p[0], h = p[3] - p[2];
    const float to[3][3] = {{1 / w, 0, p[0] / w}, {0, -1 / h, -p[3] / h}, {0, 0, -1}};
    float middle[3][3]{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                middle[i][j] += turn[i][k] * from[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float sum = 0;
            for (int k = 0; k < 3; ++k)
                sum += to[i][k] * middle[k][j];
            out[i * 3 + j] = sum;
        }
    for (float v : out)
        if (!std::isfinite(v))
            return false;
    return true;
}
// The angle between the two eyes' forward directions, radians.
inline float historyTurn(const EyeView& previous, const EyeView& current) {
    const float d = previous.world[8] * current.world[8] + previous.world[9] * current.world[9] +
                    previous.world[10] * current.world[10];
    return std::acos(std::fmax(-1.f, std::fmin(1.f, d)));
}
// The share of the old picture a pixel keeps (0 starts the history again).
inline float historyWeight(uint32_t percent, bool bothViews, float turn) {
    if (!bothViews || !(turn <= maximumHistoryTurn))
        return 0;
    return static_cast<float>(percent > maximumPercent ? maximumPercent : percent) / 100.f;
}
// How far the old colour may leave the new pixel's 3 x 3 range, as a share
// of that range on each side: 0 up to 50%, rising to 0.5 at 95%. 0 is the
// plain clamp of Update 14-20.
inline float historyLoosen(uint32_t percent) {
    const uint32_t p = percent > maximumPercent ? maximumPercent : percent;
    if (p <= loosenFrom)
        return 0;
    return .5f * static_cast<float>(p - loosenFrom) / static_cast<float>(maximumPercent - loosenFrom);
}
// Update 23: what a head turn leaves of the history. The reprojection only
// turns the old picture (no head movement, nothing that moved by itself), so
// while the head turns, shadows and edges that shift against the buildings
// were held back by the loosened clamp and dragged behind ("shadows drag as
// you turn your head", October 23). Both now fall off with the turn between
// two pairs: the loosening to none at a cut's turn, the weight to 40% of
// itself there. A still head keeps all of both.
inline float historyTurnShare(float turn) {
    if (!(turn > 0))
        return 0;
    return turn >= maximumHistoryTurn ? 1.f : turn / maximumHistoryTurn;
}
inline float turnedWeight(float weight, float turn) {
    return weight * (1.f - .6f * historyTurnShare(turn));
}
inline float turnedLoosen(float loosen, float turn) {
    return loosen * (1.f - historyTurnShare(turn));
}
} // namespace spidy::eye_history

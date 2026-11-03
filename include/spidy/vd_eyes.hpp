#pragma once
#include "math.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// The eyes as Virtual Desktop's Streamer shares them on the PC: the start of its
// body state ("VirtualDesktop.BodyState", BodyStateV2 in mbucchia's
// VirtualDesktop-OpenXR, BodyState.h), each eye a pose relative to the head.
// Its own runtime gives a gaze only above half confidence, and a Quest Pro's
// eyes come at exactly half (October 27): no gaze in a whole session.
namespace spidy::vd_eyes {
constexpr std::size_t leftValidAt = 292, rightValidAt = 293, leftPoseAt = 296, rightPoseAt = 324,
                      confidenceAt = 352, bytes = 360;
// The gaze relative to the head, -Z forward, or false: both eyes valid, at
// least half confident and unit orientations (anything else is not this layout).
inline bool gaze(const std::uint8_t* state, Quat& out) {
    if (!state[leftValidAt] || !state[rightValidAt])
        return false;
    float left[4], right[4], confidence[2];
    std::memcpy(left, state + leftPoseAt, sizeof left);
    std::memcpy(right, state + rightPoseAt, sizeof right);
    std::memcpy(confidence, state + confidenceAt, sizeof confidence);
    if (!(confidence[0] >= .5f && confidence[1] >= .5f && confidence[0] <= 1 && confidence[1] <= 1))
        return false;
    const Quat eyes[2]{{left[0], left[1], left[2], left[3]}, {right[0], right[1], right[2], right[3]}};
    Vec3 ahead{};
    for (const Quat& q : eyes) {
        const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        if (!(std::abs(n - 1) < .05f))
            return false;
        ahead += q.rotate({0, 0, -1});
    }
    // Both eyes' lines, averaged: EYE AIM takes the gaze's direction.
    if (!(length(ahead) > 1))
        return false;
    out = arc({0, 0, -1}, normalized(ahead));
    return true;
}
} // namespace spidy::vd_eyes

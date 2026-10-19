#pragma once
#include "math.hpp"
#include <cstdint>
#include <cstring>

namespace spidy::native_view {
// The renderer's pose uses right/down/forward rows and translation in row 3.
// OpenXR uses right/up/back. This conversion does not change world units.
inline Mat4 relativePose(const Mat4& native, Pose relative) {
    const Vec3 right{native[0], native[1], native[2]};
    const Vec3 up{-native[4], -native[5], -native[6]};
    const Vec3 back{-native[8], -native[9], -native[10]};
    auto world = [&](Vec3 p) { return right * p.x + up * p.y + back * p.z; };
    const Vec3 x = world(relative.orientation.rotate({1, 0, 0}));
    const Vec3 y = -world(relative.orientation.rotate({0, 1, 0}));
    const Vec3 z = -world(relative.orientation.rotate({0, 0, 1}));
    const Vec3 p = Vec3{native[12], native[13], native[14]} + world(relative.position);
    return {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, p.x, p.y, p.z, 1};
}
inline bool validPose(const Mat4& m) {
    for (float f : m)
        if (!std::isfinite(f) || std::abs(f) > 1e6f)
            return false;
    const Vec3 x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    return std::abs(length(x) - 1) < .01f && std::abs(length(y) - 1) < .01f &&
           std::abs(length(z) - 1) < .01f && std::abs(dot(x, y)) < .01f && std::abs(dot(y, z)) < .01f &&
           std::abs(dot(x, z)) < .01f && dot(cross(x, y), z) > .99f &&
           std::abs(m[3]) + std::abs(m[7]) + std::abs(m[11]) < .01f && std::abs(m[15] - 1) < .01f;
}
// Row-major OpenXR-style view matrix for geometry over the native eye image.
inline Mat4 view(const Mat4& m) {
    const Vec3 right{m[0], m[1], m[2]}, up{-m[4], -m[5], -m[6]}, back{-m[8], -m[9], -m[10]};
    const Vec3 p{m[12], m[13], m[14]};
    return {right.x, right.y, right.z, -dot(right, p), up.x, up.y, up.z, -dot(up, p),
            back.x,  back.y,  back.z,  -dot(back, p),  0,    0,    0,    1};
}
struct Descriptor {
    float values[272]{};
    Mat4 pose() const {
        Mat4 result;
        std::memcpy(result.data(), values, 64);
        return result;
    }
    float nearZ() const {
        return values[0x3f8 / 4];
    }
    float farZ() const {
        return values[0x3fc / 4];
    }
};
static_assert(sizeof(Descriptor) == 0x440);
inline bool valid(const Descriptor& d) {
    if (!validPose(d.pose()) || !std::isfinite(d.nearZ()) || !std::isfinite(d.farZ()) || d.nearZ() <= 0 ||
        d.farZ() <= d.nearZ() || d.farZ() > 1e6f)
        return false;
    for (int i = 0x400 / 4; i <= 0x40c / 4; ++i)
        if (!std::isfinite(d.values[i]) || std::abs(d.values[i]) > 100)
            return false;
    return d.values[0x400 / 4] < d.values[0x404 / 4] && d.values[0x408 / 4] < d.values[0x40c / 4];
}
// The engine's distance scale for a view's lens, as 189e4b0 puts it on view
// +1cc4 in each scene submit: 1 for a 68-degree-wide 16:9 lens (tan 34
// degrees = .6745 over tan of half its width, +410), less for a wider one;
// across the height (tan 20.8 degrees = .3794, +414) where the lens is wider
// than 16:9 (+41c), and the bare number with flag 2 at +438. Every distance a
// view's model job judges by scales with it times view +1cc8 (1796d20): which
// detail level a model draws, how far it is drawn at all, and up to where its
// textures are asked for (16 times its size times the scale).
inline float distanceScale(const Descriptor& d) {
    uint32_t flags{};
    std::memcpy(&flags, &d.values[0x438 / 4], 4);
    const bool wide = d.values[0x41c / 4] > 16.f / 9;
    const float scale = wide ? .37941101f : .67450851f;
    return (flags & 2) ? scale : scale / std::tan(d.values[(wide ? 0x414 : 0x410) / 4] * .5f);
}
// The multiplier (view +1cc8) that gives an eye the distances of the engine's
// 68-degree lens where its own lens is wider, at most `most` times its own.
inline float eyeDetail(const Descriptor& eye, float most = 3) {
    const float scale = distanceScale(eye);
    return std::isfinite(scale) && scale > 0 ? std::clamp(1 / scale, 1.f, most) : 1.f;
}
// How strongly a view's model job asks for textures (1796d20, job +2fc): half
// the image's pixels per unit of lens tangent, across whichever of its width
// and its height (behind half tangents +420 and +424) has more of them.
inline float requestScale(float width, float height, float halfWidth, float halfHeight) {
    return .5f * std::max(height / halfHeight, width / halfWidth);
}
} // namespace spidy::native_view

// SM2 VR (port of SpidyVR to Marvel's Spider-Man 2). GPLv3, as SpidyVR (Ilya Mezerowsky).
#pragma once
// Update 16: the eye sharpening (live.txt "sharpen", the GRAPHICS page's
// SHARPNESS). A contrast-adaptive sharpen after AMD FidelityFX CAS (its
// no-scaling path, per channel; MIT-licensed algorithm by AMD, written anew
// here): each pixel is pushed away from its four neighbours' average by a
// weight that falls where the 3x3 neighbourhood is already near black or near
// white, so flat areas get the most and hard edges little (fewer halos than
// a plain unsharp mask). Drawn on the GPU by native_eye_gpu.cpp's sharpen pass
// (the HLSL there is this same arithmetic); this is the CPU copy the tests
// check it against.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace spidy::eye_sharpen {
// Update 21: 0-200. 1-100 is CAS as before (the October 23 session's 20-50%
// were too faint to tell apart in the headset); 101-200 is a strong mode
// past CAS's own top: the lobe stays at its strongest (-1/5) and CAS's
// halo guard (amp, which takes the weight off where the neighbourhood is
// near black or white or already contrasty) is lifted step by step, so at
// 200% every pixel gets the full -1/5 lobe: plainly visible, with halos.
// The denominator 1 + 4w never goes below 0.2, so nothing can blow up.
inline constexpr uint32_t maximumPercent = 200;
inline constexpr uint32_t casPercent = 100; // the top of plain CAS
// CAS's sharpness (0-1) for the live.txt percent: 1-100 maps to 0.01-1, above
// stays 1; 0 is no pass at all (sharpness 0 would still sharpen a little).
inline float sharpness(uint32_t percent) {
    return static_cast<float>(std::min(percent, casPercent)) / 100.f;
}
// How much of CAS's halo guard is lifted (0-1): 0 up to 100%, 1 at 200%.
inline float boost(uint32_t percent) {
    const uint32_t p = std::min(percent, maximumPercent);
    return p <= casPercent ? 0.f : static_cast<float>(p - casPercent) / static_cast<float>(maximumPercent - casPercent);
}
// CAS's negative lobe: -1 / lerp(8, 5, sharpness): -1/8 lightest, -1/5 strongest.
inline float peak(float sharpness) {
    const float s = std::isfinite(sharpness) ? std::clamp(sharpness, 0.f, 1.f) : 0.f;
    return -1.f / (8.f + (5.f - 8.f) * s);
}
// One channel of one pixel: n holds the 3x3 neighbourhood, row by row
// (a b c / d e f / g h i, e the pixel), values 0-1 (display-encoded, as the
// eyes are). Returns the sharpened e, 0-1. `lift` is boost(percent).
inline float sharpenChannel(const std::array<float, 9>& n, float sharpness, float lift = 0) {
    const float a = n[0], b = n[1], c = n[2], d = n[3], e = n[4], f = n[5], g = n[6], h = n[7], i = n[8];
    float mn = std::min({d, e, f, b, h});
    const float mn2 = std::min({mn, a, c, g, i});
    mn += mn2;
    float mx = std::max({d, e, f, b, h});
    const float mx2 = std::max({mx, a, c, g, i});
    mx += mx2;
    // How far the neighbourhood is from clipping at either end (0-1); none
    // where it is all black (no weight: nothing to sharpen).
    // (mx floored as the shader does: all black gives 0 / 1e-5 = 0.)
    const float guard = std::sqrt(std::clamp(std::min(mn, 2.f - mx) / std::max(mx, 1e-5f), 0.f, 1.f));
    const float l = std::isfinite(lift) ? std::clamp(lift, 0.f, 1.f) : 0.f;
    const float amp = guard + (1.f - guard) * l;
    const float w = amp * peak(sharpness);
    const float out = (b * w + d * w + f * w + h * w + e) / (1.f + 4.f * w);
    return std::isfinite(out) ? std::clamp(out, 0.f, 1.f) : std::clamp(e, 0.f, 1.f);
}
} // namespace spidy::eye_sharpen

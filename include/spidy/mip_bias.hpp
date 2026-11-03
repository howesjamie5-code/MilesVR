#pragma once
// SM2 VR (port of SpidyVR to Marvel's Spider-Man 2), GPLv3; SpidyVR by Ilya Mezerowsky.
//
// Texture mip bias for the eyes. A game that upscales or uses temporal
// anti-aliasing gives its texture samplers a negative mip LOD bias, so the
// picture it accumulates over frames is sharp. The eyes get neither (SM2VR
// stores 0 in SpidyEyeTemporal), so a negative bias undersamples textures:
// moire on brick facades and sparkle that changes every frame. The game's
// samplers are D3D12 descriptors and root signatures' static samplers; the
// render-memory module (loaded while the game starts) sees them as the game's
// device creates them (native_mip_bias.cpp). Only samplers the game gave a
// negative bias change; every other sampler (screen-space passes, mip chains
// read with explicit levels, shadows) stays exactly as the game made it.
#include <cstdint>
#include <cstring>

namespace spidy::mip_bias {
// SpidyMipBiasStart's parameter. Bits 0-15: 0 reports the game's biases and
// changes nothing; otherwise `encoded - 1000` is the bias in hundredths for the
// samplers the game biased negatively (1000 = 0.00, 1050 = +0.50). Bits 16-23:
// the anisotropy (1-16) those same samplers get at least, 0 to leave the game's.
inline constexpr int32_t minimumHundredths = -200, maximumHundredths = 100;
inline constexpr uint32_t maximumAnisotropy = 16; // D3D12_MAX_MAXANISOTROPY
inline constexpr uintptr_t encode(int32_t hundredths, uint32_t anisotropy = 0) {
    return static_cast<uintptr_t>(1000 + hundredths) | static_cast<uintptr_t>(anisotropy) << 16;
}
inline bool decode(uintptr_t parameter, bool& change, float& bias, uint32_t& anisotropy) {
    anisotropy = static_cast<uint32_t>(parameter >> 16);
    parameter &= 0xffff;
    change = parameter != 0;
    bias = 0.f;
    if (anisotropy > maximumAnisotropy)
        return false;
    if (!change)
        return true;
    const auto h = static_cast<int64_t>(parameter) - 1000;
    if (h < minimumHundredths || h > maximumHundredths)
        return false;
    bias = static_cast<float>(h) / 100.f;
    return true;
}
inline bool decode(uintptr_t parameter, bool& change, float& bias) {
    uint32_t anisotropy;
    return decode(parameter, change, bias, anisotropy) && anisotropy == 0;
}
// A texture sampler's filtering: the samplers the game biased negatively are its
// material textures (the ones it sharpens for its upscaler). Those whose filter is
// plain trilinear (D3D12_FILTER_MIN_MAG_MIP_LINEAR 0x15) or anisotropic (0x55) with a
// lower MaxAnisotropy get anisotropic filtering at `wanted`; comparison (shadow) and
// min/max filters, point-sampled mips and every other sampler stay as the game made
// them. True when `filter`/`maxAnisotropy` changed.
inline constexpr uint32_t trilinear = 0x15, anisotropic = 0x55;
inline bool raiseAnisotropy(float gameBias, uint32_t& filter, uint32_t& maxAnisotropy, uint32_t wanted) {
    if (!wanted || !(gameBias < 0.f) || (filter != trilinear && filter != anisotropic))
        return false;
    if (filter == anisotropic && maxAnisotropy >= wanted)
        return false;
    filter = anisotropic;
    if (maxAnisotropy < wanted)
        maxAnisotropy = wanted;
    return true;
}
// The bias a sampler gets: the game's own, unless the game made it negative.
inline float adjusted(float game, bool change, float bias) {
    return change && game < 0.f ? bias : game;
}
// A root signature blob with only a root signature part (D3D12Serialize*RootSignature's
// output) rather than a shader with one embedded: only those are rewritten, so a
// shader's own root signature and the root signature made from it always match.
inline bool standaloneRootSignature(const void* blob, size_t size) {
    const auto* b = static_cast<const uint8_t*>(blob);
    if (!b || size < 32 || std::memcmp(b, "DXBC", 4))
        return false;
    uint32_t total{}, parts{};
    std::memcpy(&total, b + 24, 4);
    std::memcpy(&parts, b + 28, 4);
    if (total > size || parts == 0 || parts > 64 || 32 + 4ull * parts > size)
        return false;
    for (uint32_t i = 0; i < parts; ++i) {
        uint32_t offset{};
        std::memcpy(&offset, b + 32 + 4 * i, 4);
        if (offset + 8ull > size || std::memcmp(b + offset, "RTS0", 4))
            return false;
    }
    return true;
}
// Telemetry, read by the session tools (tools/probe_stereo.py mip_bias_snapshot).
struct Data {
    uint32_t magic = 0x534d4244, version = 2, bytes = sizeof(Data);
    // 0 off, 1 waiting for the game's device, 2 watching the game's device,
    // 3 started after the game made its device (nothing seen: start the game
    // with the launcher), 4 unavailable (error).
    uint32_t status{};
    uint32_t change{}; // 1 when the game's negative biases are replaced
    float bias{};      // what they are replaced with
    float mostNegative{}; // the game's most negative bias seen (0 if none)
    uint32_t error{};
    uint64_t samplers{}, negativeSamplers{}, changedSamplers{};
    uint64_t rootSignatures{}, staticSamplers{}, negativeStaticSamplers{}, changedRootSignatures{};
    // Shaders' own root signatures (left as they are) and their negatively biased static samplers.
    uint64_t embeddedRootSignatures{}, negativeEmbeddedSamplers{}, failedRootSignatures{};
    uint64_t devices{};
    // Version 2: anisotropic filtering of the negatively biased samplers.
    uint32_t anisotropy{};       // what they get at least (0: the game's)
    uint32_t mostAnisotropy{};   // the highest MaxAnisotropy the game gave an anisotropic one
    uint64_t anisotropicSamplers{}; // negatively biased samplers the game made anisotropic
    uint64_t trilinearSamplers{};   // negatively biased samplers the game made trilinear (not anisotropic)
    uint64_t raisedSamplers{};      // of those, the ones given `anisotropy`
};
static_assert(sizeof(Data) == 152);
} // namespace spidy::mip_bias

#pragma once
#include <algorithm>
#include <cstdint>

namespace spidy {
struct XrFrameTiming {
    double total{}, wait{}, tracking{}, prepare{}, acquire{}, release{}, end{}, period{};
};
struct TimingMetric {
    uint64_t count{};
    double totalMs{}, maxMs{}, lastMs{};
    void add(double ms) {
        ++count;
        totalMs += ms;
        maxMs = std::max(maxMs, ms);
        lastMs = ms;
    }
};
struct XrTimingData {
    uint32_t magic = 0x5358544d, version = 4, bytes = sizeof(XrTimingData), reserved{};
    int64_t sequence{};
    // frame, wait-frame, tracking, prepare, acquire, copy submission wait,
    // hand overlay CPU, release, end-frame, runtime display period; version 2:
    // the headset copy's GPU finish after its submission, and after the game
    // submitted the eye pair it copied (native_gpu::copyTiming); version 3:
    // the eye AA pass's GPU time for both eyes, its mode (0 off, 1 FXAA), why
    // it stopped (native_gpu::eyeAATiming) and the pairs it smoothed;
    // version 4 (Update 16): the same for the headset sharpening pass
    // (native_gpu::eyeSharpenTiming: its time, strength 0-200 since Update 21, fault, pairs).
    TimingMetric stages[13];
    uint32_t eyeAA{}, eyeAAFault{};
    uint64_t eyeAAPairs{};
    TimingMetric eyeSharpen;
    uint32_t sharpen{}, sharpenFault{};
    uint64_t sharpenPairs{};
};
static_assert(sizeof(TimingMetric) == 32 && sizeof(XrTimingData) == 504);
} // namespace spidy

#include "spidy/impact_sound.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace spidy {
namespace {
constexpr float pi = 3.14159265f;
// A blow: the tone falls from bodyFrom to bodyTo hertz and dies away at
// bodyDecay a second; the noise is low-passed at snapTone hertz and dies at
// snapDecay. `drive` presses the sum into a soft clip (the harder the blow,
// the denser), `gain` is its peak of full scale.
struct Shape {
    float seconds, bodyFrom, bodyTo, bodyDecay, bodyGain, snapTone, snapDecay, snapGain, drive, gain;
};
constexpr Shape shapes[] = {
    {.12f, 190, 70, 28, .7f, 3500, 110, .5f, 1.2f, .42f},
    {.16f, 160, 55, 22, .9f, 3000, 90, .6f, 1.6f, .58f},
    {.24f, 130, 42, 15, 1.f, 2400, 70, .7f, 2.2f, .74f},
    {.42f, 110, 34, 9, 1.f, 2000, 45, .8f, 2.8f, .86f},
};
constexpr float rushSeconds = .22f, rushGain = .2f;
// The Mega Venom Blast: its charge rises for megaCharge seconds, then the boom.
constexpr float megaSeconds = 1.6f, megaCharge = .22f;
// Uniform noise in -1..1, the same every time.
struct Noise {
    std::uint32_t state = 0x2545f491;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 8388608.f - 1;
    }
};
} // namespace

float impactSeconds(ImpactSound sound) {
    return sound == ImpactSound::Rush   ? rushSeconds
           : sound == ImpactSound::Mega ? megaSeconds
                                        : shapes[static_cast<unsigned>(sound) & 3].seconds;
}
std::vector<std::uint8_t> impactWave(ImpactSound sound, unsigned rate) {
    rate = std::clamp(rate, 8000u, 48000u);
    const float seconds = impactSeconds(sound);
    const auto count = static_cast<std::uint32_t>(std::lround(seconds * static_cast<float>(rate)));
    std::vector<float> samples(count);
    Noise noise;
    if (sound == ImpactSound::Mega) {
        // A rush of noise rising into the boom; the boom a low tone falling to
        // a growl over a burst of noise, pressed hard; a crackle of sparks; and
        // the rumble the boom leaves, dying over the last second.
        float low{}, band{}, phase{}, snap{}, rumble{}, spark{};
        const float r = static_cast<float>(rate);
        const float snapSmooth = 1 - std::exp(-2 * pi * 2200 / r), rumbleSmooth = 1 - std::exp(-2 * pi * 120 / r);
        for (std::uint32_t i = 0; i < count; ++i) {
            const float t = static_cast<float>(i) / r;
            float sum = 0;
            if (t < megaCharge) {
                const float u = t / megaCharge;
                const float centre = 400 + 2400 * u * u;
                const float f = 2 * std::sin(pi * centre / r);
                const float high = noise.next() - low - .6f * band;
                band += f * high;
                low += f * band;
                sum += band * u * u * .55f;
            } else {
                const float b = t - megaCharge;
                phase += 2 * pi * (32 + 90 * std::exp(-b * 9)) / r;
                snap += (noise.next() - snap) * snapSmooth;
                rumble += (noise.next() - rumble) * rumbleSmooth;
                // Sparks: now and then a click that rings for a moment.
                if (noise.next() > .9993f - .0006f * std::exp(-b * 2))
                    spark = noise.next() > 0 ? 1.f : -1.f;
                spark *= .985f;
                sum += std::sin(phase) * std::exp(-b * 2.6f) * 1.1f + snap * std::exp(-b * 14) * .9f +
                       rumble * std::exp(-b * 1.7f) * 2.2f + spark * std::exp(-b * 1.2f) * .35f;
            }
            samples[i] = std::tanh(sum * 2.4f) / std::tanh(2.4f) * .92f;
        }
    } else if (sound == ImpactSound::Rush) {
        // Noise through a band that rises and falls back, swelling and fading.
        float low{}, band{};
        for (std::uint32_t i = 0; i < count; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(count);
            const float centre = 500 + 1100 * std::sin(pi * t);
            const float f = 2 * std::sin(pi * centre / static_cast<float>(rate));
            const float high = noise.next() - low - .7f * band;
            band += f * high;
            low += f * band;
            const float swell = std::sin(pi * t);
            samples[i] = band * swell * swell * rushGain;
        }
    } else {
        const auto& s = shapes[static_cast<unsigned>(sound) & 3];
        const float smooth = 1 - std::exp(-2 * pi * s.snapTone / static_cast<float>(rate));
        const float ceiling = std::tanh(s.drive);
        float phase{}, snap{};
        for (std::uint32_t i = 0; i < count; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(rate);
            phase += 2 * pi * (s.bodyTo + (s.bodyFrom - s.bodyTo) * std::exp(-t * 18)) / static_cast<float>(rate);
            snap += (noise.next() - snap) * smooth;
            const float sum = std::sin(phase) * std::exp(-t * s.bodyDecay) * s.bodyGain +
                              snap * std::exp(-t * s.snapDecay) * s.snapGain;
            samples[i] = std::tanh(sum * s.drive) / ceiling * s.gain;
        }
    }
    // No click at either end: half a millisecond in, five out.
    const auto in = std::max<std::uint32_t>(1, rate / 2000), out = std::max<std::uint32_t>(1, rate / 200);
    for (std::uint32_t i = 0; i < count; ++i) {
        const float fade = std::min({1.f, static_cast<float>(i) / static_cast<float>(in),
                                     static_cast<float>(count - 1 - i) / static_cast<float>(out)});
        samples[i] *= fade;
    }
    std::vector<std::uint8_t> wave(44 + count * 2);
    const auto put = [&](size_t at, std::uint32_t value, unsigned bytes) {
        for (unsigned b = 0; b < bytes; ++b)
            wave[at + b] = static_cast<std::uint8_t>(value >> (8 * b));
    };
    std::memcpy(wave.data(), "RIFF", 4);
    put(4, 36 + count * 2, 4);
    std::memcpy(wave.data() + 8, "WAVEfmt ", 8);
    put(16, 16, 4);       // the format chunk's size
    put(20, 1, 2);        // PCM
    put(22, 1, 2);        // mono
    put(24, rate, 4);
    put(28, rate * 2, 4); // bytes a second
    put(32, 2, 2);        // bytes a sample
    put(34, 16, 2);       // bits a sample
    std::memcpy(wave.data() + 36, "data", 4);
    put(40, count * 2, 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto value = static_cast<std::int16_t>(std::lround(std::clamp(samples[i], -1.f, 1.f) * 32767));
        put(44 + i * 2ull, static_cast<std::uint16_t>(value), 2);
    }
    return wave;
}
} // namespace spidy

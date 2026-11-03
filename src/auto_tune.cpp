#include "spidy/auto_tune.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace spidy::auto_tune {
namespace {
std::string upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
bool has(const std::string& text, std::string_view word) {
    return text.find(word) != std::string::npos;
}
// A card's relative speed in games (raster, 1440p-4K class loads), RTX 4090 = 100. The longer
// names first, so "4070 TI SUPER" is not read as "4070".
struct Card {
    std::string_view name;
    int score;
};
constexpr Card cards[] = {
    // NVIDIA
    {"5090", 125}, {"5080", 95}, {"5070 TI", 82}, {"5070", 68}, {"5060 TI", 50}, {"5060", 45}, {"5050", 34},
    {"4090", 100}, {"4080 SUPER", 82}, {"4080", 80}, {"4070 TI SUPER", 70}, {"4070 TI", 66},
    {"4070 SUPER", 60}, {"4070", 52}, {"4060 TI", 40}, {"4060", 33}, {"4050", 25},
    {"3090 TI", 64}, {"3090", 58}, {"3080 TI", 57}, {"3080", 52}, {"3070 TI", 44}, {"3070", 41},
    {"3060 TI", 36}, {"3060", 28}, {"3050", 18},
    {"2080 TI", 40}, {"2080 SUPER", 34}, {"2080", 32}, {"2070 SUPER", 30}, {"2070", 27},
    {"2060 SUPER", 25}, {"2060", 21},
    {"1660 SUPER", 17}, {"1660 TI", 17}, {"1660", 15}, {"1650", 10},
    {"1080 TI", 30}, {"1080", 24}, {"1070 TI", 21}, {"1070", 19}, {"1060", 12},
    // AMD
    {"9070 XT", 75}, {"9070", 66}, {"9060 XT", 40}, {"9060", 34},
    {"7900 XTX", 82}, {"7900 XT", 70}, {"7900 GRE", 58}, {"7800 XT", 52}, {"7700 XT", 43}, {"7700", 40},
    {"7600 XT", 30}, {"7600", 28},
    {"6950 XT", 55}, {"6900 XT", 52}, {"6800 XT", 50}, {"6800", 43}, {"6750 XT", 36}, {"6700 XT", 34},
    {"6700", 30}, {"6650 XT", 28}, {"6600 XT", 27}, {"6600", 24}, {"6500 XT", 12},
    {"5700 XT", 24}, {"5700", 21}, {"5600 XT", 19},
    // Intel
    {"B580", 30}, {"B570", 26}, {"A770", 27}, {"A750", 24}, {"A580", 20},
};
int fromMemory(uint64_t vramMb) {
    return vramMb >= 20000 ? 70 : vramMb >= 15000 ? 55 : vramMb >= 11000 ? 42 : vramMb >= 9500 ? 36 : vramMb >= 7500 ? 28
           : vramMb >= 5500 ? 18 : 10;
}
} // namespace

const char* modeName(Mode m) {
    switch (m) {
    case Mode::maxFps:
        return "MAX FPS";
    case Mode::balanced:
        return "BALANCED";
    case Mode::bestLooks:
        return "BEST LOOKS";
    default:
        return "OFF";
    }
}
const char* tierName(int t) {
    static const char* names[tiers] = {"ENTRY", "MID", "HIGH", "ULTRA"};
    return names[std::clamp(t, 0, tiers - 1)];
}

int gpuScore(std::string_view name, uint64_t vramMb) {
    const std::string n = upper(name);
    for (const auto& c : cards)
        if (has(n, c.name)) {
            // A laptop's card of the same name runs at about two thirds.
            const bool laptop = has(n, "LAPTOP") || has(n, "MOBILE") || has(n, " MAX-Q");
            return laptop ? c.score * 2 / 3 : c.score;
        }
    return fromMemory(vramMb);
}

int textureCap(uint64_t vramMb) {
    return vramMb >= 11000 ? 3 : vramMb >= 7500 ? 2 : vramMb >= 5500 ? 1 : 0;
}

std::string headsetName(const Hardware& hw) {
    const std::string n = upper(hw.headset);
    if (has(n, "QUEST 3S"))
        return "QUEST 3S";
    if (has(n, "QUEST 3"))
        return "QUEST 3";
    if (has(n, "QUEST PRO"))
        return "QUEST PRO";
    if (has(n, "QUEST 2"))
        return "QUEST 2";
    if (has(n, "QUEST"))
        return "QUEST";
    // A streamer (Virtual Desktop, Steam Link, SteamVR) names itself: the eye size tells the headset's
    // class only roughly, as the streamer scales it, so it is not guessed.
    std::string shown = hw.headset.empty() ? std::string("HEADSET") : upper(hw.headset);
    if (shown.size() > 28)
        shown.resize(28);
    return shown;
}

int tier(const Hardware& hw) {
    const double score = gpuScore(hw.gpu, hw.vramMb);
    // The headset's pixels a second against a Quest 3's 2064 x 2208 at 90 Hz.
    const double hz = hw.refreshHz > 30 && hw.refreshHz < 200 ? hw.refreshHz : 90;
    const double pixels = hw.eyeWidth && hw.eyeHeight ? double(hw.eyeWidth) * hw.eyeHeight : 2064. * 2208.;
    const double load = std::clamp(pixels * hz / (2064. * 2208. * 90.), .4, 3.);
    const double effective = score / std::pow(load, .8);
    int t = effective >= 70 ? 3 : effective >= 45 ? 2 : effective >= 28 ? 1 : 0;
    // The game streams the city on every core: a small processor or memory holds it back.
    if ((hw.cpuThreads && hw.cpuThreads < 8) || (hw.ramMb && hw.ramMb < 15000))
        t = std::min(t, 1);
    if (hw.cpuThreads && hw.cpuThreads < 6)
        t = 0;
    return t;
}

const std::array<GameKey, 10>& gameKeys() {
    static const std::array<GameKey, 10> keys{{
        {"TextureQuality", &GameSettings::textureQuality, 0, 3},
        {"TextureFiltering", &GameSettings::textureFiltering, 0, 4},
        {"ShadowQuality", &GameSettings::shadowQuality, 0, 3},
        {"LevelOfDetail", &GameSettings::levelOfDetail, 0, 3},
        {"AmbientOcclusionQuality", &GameSettings::ambientOcclusion, 0, 2},
        {"ScreenSpaceReflections", &GameSettings::reflections, 0, 2},
        {"HairQuality", &GameSettings::hairQuality, 1, 3},
        {"CrowdDetail", &GameSettings::crowdDetail, 0, 3},
        {"VehicleDetail", &GameSettings::vehicleDetail, 0, 3},
        {"ParticleLighting", &GameSettings::particleLighting, 0, 2},
    }};
    return keys;
}

Preset preset(Mode mode, int t, const Hardware& hw) {
    t = std::clamp(t, 0, tiers - 1);
    const int m = std::clamp(static_cast<int>(mode), 0, modes - 1);
    // [mode][tier]
    static constexpr int reach[modes][tiers] = {{100, 100, 125, 125}, {125, 150, 175, 200}, {175, 200, 250, 300}};
    static constexpr int sharpen[modes][tiers] = {{80, 80, 80, 80}, {100, 100, 100, 100}, {120, 120, 120, 120}};
    static constexpr int monitor[modes][tiers] = {{15, 15, 15, 15}, {30, 30, 30, 30}, {30, 30, 30, 30}};
    // The eye resolution as pixels an eye (millions), whatever the headset asks for: Virtual Desktop's
    // Quest 3 asks 2496 x 2688 (6.7), the panel's own is 2064 x 2208 (4.6). Never above what it asks
    // (0.0.127's BEST LOOKS at 115% crashed the game as it loaded, October 25).
    static constexpr double eyePixels[modes][tiers] = {{1.6, 2.2, 2.8, 3.4}, {2.2, 2.8, 3.6, 4.4}, {2.8, 3.6, 4.6, 5.6}};
    static constexpr int textures[modes][tiers] = {{1, 1, 2, 2}, {1, 2, 2, 3}, {2, 2, 3, 3}};
    static constexpr int filtering[modes][tiers] = {{2, 2, 2, 3}, {3, 3, 3, 4}, {4, 4, 4, 4}};
    // At most Medium shadows and Low reflections: the 0.0.127 crash had High for both, and the shadow
    // atlas has no room for larger maps for every view (stereo_probe.cpp, October 11).
    static constexpr int shadows[modes][tiers] = {{0, 0, 1, 1}, {1, 1, 1, 2}, {1, 2, 2, 2}};
    static constexpr int lod[modes][tiers] = {{1, 1, 1, 2}, {1, 2, 2, 3}, {2, 2, 3, 3}};
    static constexpr int ao[modes][tiers] = {{0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 2}};
    static constexpr int ssr[modes][tiers] = {{0, 0, 0, 0}, {0, 1, 1, 1}, {1, 1, 1, 1}};
    static constexpr int hair[modes][tiers] = {{1, 1, 1, 2}, {1, 2, 2, 2}, {2, 2, 3, 3}};
    static constexpr int crowd[modes][tiers] = {{0, 0, 1, 1}, {1, 1, 1, 2}, {1, 1, 2, 2}};
    static constexpr int particles[modes][tiers] = {{0, 0, 1, 1}, {1, 1, 1, 2}, {1, 1, 2, 2}};
    Preset p;
    p.reach = reach[m][t];
    p.sharpen = sharpen[m][t];
    p.monitor = monitor[m][t];
    const double asked = hw.eyeWidth && hw.eyeHeight ? double(hw.eyeWidth) * hw.eyeHeight / 1e6 : 4.56;
    const double percent = 100 * std::sqrt(eyePixels[m][t] / asked);
    p.renderScale = std::clamp(static_cast<int>(std::lround(percent / 5)) * 5, 50, 100);
    auto& g = p.game;
    g.textureQuality = std::min(textures[m][t], hw.vramMb ? textureCap(hw.vramMb) : 1);
    g.textureFiltering = filtering[m][t];
    g.shadowQuality = shadows[m][t];
    g.levelOfDetail = lod[m][t];
    // HBAO+ is NVIDIA's; another card's best is SSAO.
    g.ambientOcclusion = hw.vendor == 0x10de ? ao[m][t] : std::min(ao[m][t], 1);
    g.reflections = ssr[m][t];
    g.hairQuality = hair[m][t];
    g.crowdDetail = crowd[m][t];
    g.vehicleDetail = crowd[m][t];
    g.particleLighting = particles[m][t];
    return p;
}

float Check::target(Mode m) {
    return m == Mode::maxFps ? .9f : m == Mode::balanced ? .6f : .5f;
}
void Check::start(Mode mode, int t, double now) {
    *this = Check{};
    mode_ = mode;
    tier_ = std::clamp(t, 0, tiers - 1);
    phaseStart_ = now;
    lastNow_ = now;
}
int Check::update(double now, float fps, float refreshHz, bool play) {
    if (!running())
        return -1;
    const double dt = std::clamp(now - lastNow_, 0., .5);
    lastNow_ = now;
    // Only play counts: a menu, a cutscene or a load waits.
    if (!play || !std::isfinite(fps) || !std::isfinite(refreshHz) || refreshHz < 30)
        return -1;
    played_ += dt;
    refresh_ = refreshHz;
    if (played_ < settleSeconds)
        return -1;
    sum_ += fps * dt;
    if (played_ < settleSeconds + measureSeconds)
        return -1;
    measured_ = static_cast<float>(sum_ / measureSeconds);
    sum_ = 0;
    played_ = 0;
    const float share = measured_ / refreshHz;
    // A step down that bought less than 5% more frames is not worth its detail (the processor, not the
    // graphics card, holds the frame rate back): the detail goes back and the check ends.
    if (steps_ && previousTier_ > tier_ && measured_ < previousMeasured_ * 1.05f) {
        tier_ = previousTier_;
        measured_ = previousMeasured_;
        cpuBound_ = true;
        done_ = true;
        return tier_;
    }
    int next = tier_;
    if (share < target(mode_) && tier_ > 0)
        next = tier_ - 1;
    else if (mode_ == Mode::bestLooks && share >= roomToGrow && tier_ < tiers - 1)
        next = tier_ + 1;
    if (next == tier_ || steps_ >= mostSteps) {
        done_ = true;
        return -1;
    }
    previousTier_ = tier_;
    previousMeasured_ = measured_;
    tier_ = next;
    ++steps_;
    return tier_;
}
std::string Check::status(double) const {
    char line[96];
    if (mode_ == Mode::none)
        return "NOT TUNED YET";
    if (!done_) {
        if (std::isfinite(measured_))
            std::snprintf(line, sizeof line, "%.0f FPS: TRYING %s DETAIL...", static_cast<double>(measured_), tierName(tier_));
        else
            std::snprintf(line, sizeof line, "MEASURING IN PLAY...");
        return line;
    }
    const bool met = std::isfinite(measured_) && refresh_ > 0 && measured_ / refresh_ >= target(mode_);
    std::snprintf(line, sizeof line, "%.0f FPS / %.0f HZ - %s", static_cast<double>(measured_),
                  static_cast<double>(refresh_), met ? "ON TARGET" : cpuBound_ ? "CPU LIMITED: KEPT DETAIL"
                                                                               : "AS FAST AS IT GOES");
    return line;
}
} // namespace spidy::auto_tune

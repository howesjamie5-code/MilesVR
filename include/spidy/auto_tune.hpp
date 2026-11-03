#pragma once
// AUTO TUNE (the wrist menu's GRAPHICS page): one press picks every graphics
// setting for the PC and headset this session runs on, in one of three modes:
//   MAX FPS     - the highest frame rate, the picture simpler;
//   BALANCED    - a steady frame rate with most of the detail;
//   BEST LOOKS  - the most detail the PC carries at a playable rate.
// It rates the hardware (Hardware: the game's graphics card by name and video
// memory, the processor's threads, the memory, and the headset's pixels a
// second - its recommended eye size at its refresh rate) into a tier, 0 entry
// to 3 ultra, and takes the mode's settings for that tier (preset):
// - now: the wrist GRAPHICS page's eye settings (building reach, sharpening,
//   the window's rate);
// - from the next launch: the eye resolution (the launcher's render scale)
//   and the game's own graphics settings, which it reads only at its start.
//   Those are written for VR sessions only: each launch sets them before the
//   game starts, and the player's own come back when the game closes
//   (tools/vr_display.py), so the flat game keeps its settings.
// Then it checks itself (Check): after a few seconds to settle it measures
// the new frames a second in play, and steps the eye settings down a tier
// while the mode's target is missed (up a tier, for BEST LOOKS, while the
// frame rate has room), at most three times. A step down that buys under 5%
// more frames goes back: the processor holds the rate, not the detail.
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace spidy::auto_tune {
enum class Mode : int { none = -1, maxFps = 0, balanced = 1, bestLooks = 2 };
constexpr int modes = 3, tiers = 4;
const char* modeName(Mode);  // "MAX FPS", "BALANCED", "BEST LOOKS"
const char* tierName(int);   // "ENTRY", "MID", "HIGH", "ULTRA"

struct Hardware {
    std::string gpu;          // the adapter's name, as DXGI gives it
    uint32_t vendor{};        // PCI vendor: 0x10de NVIDIA, 0x1002 AMD, 0x8086 Intel
    uint64_t vramMb{};        // dedicated video memory
    uint32_t cpuThreads{};
    uint64_t ramMb{};
    std::string headset;      // the runtime's system name (may be the streamer's)
    uint32_t eyeWidth{}, eyeHeight{}; // the runtime's recommended eye size
    float refreshHz{};        // the headset's refresh rate; 0 unknown
};
// The card's speed, 4090 = 100 (a table of known cards; else from its video memory).
int gpuScore(std::string_view name, uint64_t vramMb);
// The headset's name as the panel shows it: a known Quest from its name or its eye size, else the name.
std::string headsetName(const Hardware&);
// The tier the hardware carries: the card's score against the headset's pixels a second (a Quest 3's
// 2064 x 2208 at 90 Hz is 1), capped by a processor under 8 threads or under 16 GB of memory.
int tier(const Hardware&);
// The most texture quality the video memory holds (the game's TextureQuality: 0 low .. 3 very high).
int textureCap(uint64_t vramMb);

// The game's graphics values (HKCU\Software\Insomniac Games\Marvel's Spider-Man 2\Graphics, REG_DWORD),
// in the game's own numbering (its settings log names them).
struct GameSettings {
    int textureQuality{};    // Low, Medium, High, Very High
    int textureFiltering{};  // Trilinear, Anisotropic 2x, 4x, 8x, 16x
    int shadowQuality{};     // Lowest, Low, Medium, High
    int levelOfDetail{};     // Lowest, Low, Medium, High
    int ambientOcclusion{};  // Off, SSAO, HBAO+
    int reflections{};       // Screen Space Reflections: Off, Low, Normal
    int hairQuality{};       // (Lowest), Low, Medium, High
    int crowdDetail{};       // Low, Medium, High
    int vehicleDetail{};     // Low, Medium, High
    int particleLighting{};  // Low, Medium, High
};
struct GameKey {
    const char* name;        // the registry value's name
    int GameSettings::*value;
    int low, high;           // the range written
};
const std::array<GameKey, 10>& gameKeys();

// One mode's settings for one tier.
struct Preset {
    // Now (the wrist GRAPHICS page's values).
    // MilesVR: SM2VR's far shadows, shared LOD, LOD fade, eye bloom and shared shadows are Spider-Man 2
    // engine hooks with no Miles Morales match, so they are left out.
    int reach{}, sharpen{}, monitor{};
    // Next launch.
    int renderScale{}; // percent of the runtime's recommended eye size, per side (50-100)
    GameSettings game;
};
Preset preset(Mode, int tier, const Hardware&);

// The live check after a press. Fed every headset frame with the wrist meter's rate; `play`: the
// player is in VR play (not a menu, cutscene or load). Returns the tier the eye settings should have
// now when it changes (a step), else -1.
class Check {
  public:
    static constexpr double settleSeconds = 5, measureSeconds = 8;
    static constexpr int mostSteps = 3;
    void start(Mode mode, int tier, double now);
    int update(double now, float fps, float refreshHz, bool play);
    bool running() const {
        return mode_ != Mode::none && !done_;
    }
    Mode mode() const {
        return mode_;
    }
    int liveTier() const {
        return tier_;
    }
    float measured() const { // the last measured rate; NaN before one
        return measured_;
    }
    int steps() const {
        return steps_;
    }
    // A line for the panel: "CHECKING..." or what it found.
    std::string status(double now) const;
    // The share of the refresh rate a mode aims for (MAX FPS 0.9, BALANCED 0.6, BEST LOOKS 0.5), and the
    // share above which BEST LOOKS steps up (0.9).
    static float target(Mode);
    static constexpr float roomToGrow = .9f;

  private:
    Mode mode_ = Mode::none;
    int tier_{}, steps_{};
    bool done_{};
    double phaseStart_{}, played_{}, lastNow_{};
    double sum_{};
    int samples_{};
    float measured_ = std::numeric_limits<float>::quiet_NaN();
    float refresh_{};
    // The tier and rate before the last step, and whether a step down bought too little (cpuBound_).
    int previousTier_ = -1;
    float previousMeasured_{};
    bool cpuBound_{};
};
} // namespace spidy::auto_tune

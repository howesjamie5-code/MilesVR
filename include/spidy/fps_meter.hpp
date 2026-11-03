// SM2 VR (port of SpidyVR to Marvel's Spider-Man 2). GPLv3, as SpidyVR (Ilya Mezerowsky).
#pragma once
// Update 16: the wrist hologram's FPS monitor (VR SETTINGS page). Fed once a
// headset frame on the XR thread with what game_xr already knows: whether the
// frame showed a new eye pair from the game (a new generation copied), and
// the runtime's display period. Every updateSeconds it publishes the new pairs
// a second over the last windowSeconds, the headset's refresh, and keeps the
// last history's values for the sparkline. No locks, no allocation: plain
// values, read by the same thread when it draws the panel.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace spidy::wrist_hud {
class FpsMeter {
  public:
    static constexpr double updateSeconds = .25, windowSeconds = .75;
    static constexpr int history = 20; // 5 s of updates
    // One headset frame: its time (s), whether it showed a new pair, the
    // display period in ms (<= 0 or not finite: unknown).
    void frame(double seconds, bool newPair, double periodMs) {
        if (!std::isfinite(seconds))
            return;
        if (seconds < last_) // the clock went back: start again
            *this = FpsMeter{};
        last_ = seconds;
        if (std::isfinite(periodMs) && periodMs > 1 && periodMs < 100)
            periodMs_ = periodMs_ > 0 ? periodMs_ + (periodMs - periodMs_) * .05 : periodMs;
        const int slot = static_cast<int>(std::floor(seconds / bucketSeconds)) ;
        if (!started_) {
            started_ = true;
            firstSlot_ = slot;
            nextUpdate_ = seconds + updateSeconds;
        }
        advance(slot);
        if (newPair)
            ++buckets_[static_cast<size_t>(slot) % buckets];
        if (seconds >= nextUpdate_) {
            nextUpdate_ = seconds + updateSeconds;
            // Only once a whole window has been watched.
            if (slot - firstSlot_ >= windowBuckets) {
                int sum = 0;
                for (int k = 1; k <= windowBuckets; ++k) // the whole buckets before this one
                    sum += buckets_[static_cast<size_t>(slot - k) % buckets];
                fps_ = static_cast<float>(sum / (windowBuckets * bucketSeconds));
                std::rotate(trail_.begin(), trail_.begin() + 1, trail_.end());
                trail_.back() = fps_;
                trailCount_ = std::min(trailCount_ + 1, history);
            }
        }
    }
    // New pairs a second; NaN until a window has been seen.
    float fps() const {
        return fps_;
    }
    // The headset's refresh (Hz); NaN until the runtime gave a period.
    float refreshHz() const {
        return periodMs_ > 0 ? static_cast<float>(1000. / periodMs_) : std::nanf("");
    }
    // The last values, oldest first; the first history - count() are NaN.
    const std::array<float, history>& trail() const {
        return trail_;
    }
    int count() const {
        return trailCount_;
    }

  private:
    static constexpr double bucketSeconds = .05;
    static constexpr int windowBuckets = 15, buckets = 32; // .75 s
    void advance(int slot) {
        if (!have_) {
            have_ = true;
            slot_ = slot;
            buckets_.fill(0);
            return;
        }
        if (slot - slot_ >= buckets)
            buckets_.fill(0);
        else
            for (int s = slot_ + 1; s <= slot; ++s)
                buckets_[static_cast<size_t>(s) % buckets] = 0;
        slot_ = std::max(slot_, slot);
    }
    std::array<uint16_t, buckets> buckets_{};
    std::array<float, history> trail_ = [] {
        std::array<float, history> a{};
        a.fill(std::nanf(""));
        return a;
    }();
    double last_ = -1e300, nextUpdate_ = 0, periodMs_ = 0;
    float fps_ = std::nanf("");
    int slot_ = 0, firstSlot_ = 0, trailCount_ = 0;
    bool started_ = false, have_ = false;
};
// Update 24: the session's HIGH, LOW and AVG (VR SETTINGS page, beside the
// FPS monitor; the report and the console at exit). The same count as
// FpsMeter (headset frames showing an eye pair not shown before), in whole
// windowSeconds windows one after another. Tracking starts warmupSeconds after
// the first new pair (loading and the first streaming hitches left out), or at
// once after reset() (RESET on the page, for A/B tests). A window with no new
// pair for over gapSeconds (a loading screen, the game paused its rendering)
// is left out, a new window starting at the first pair after it, and a stall
// of this thread longer than gapSeconds drops the window under way, so one
// long load cannot be the LOW. Same thread only; no allocation.
class FpsSession {
  public:
    static constexpr double warmupSeconds = 10, windowSeconds = 1, gapSeconds = .5;
    void frame(double seconds, bool newPair) {
        if (!std::isfinite(seconds))
            return;
        if (resetPending_) {
            resetPending_ = false;
            clearStats();
            if (started_) { // tracking again from now, no warm-up
                windowStart_ = seconds;
                pairs_ = 0;
                last_ = seconds;
                lastPair_ = seconds;
                return;
            }
        }
        if (!started_) {
            if (!newPair)
                return;
            started_ = true;
            windowStart_ = seconds + warmupSeconds;
            last_ = lastPair_ = seconds;
            return;
        }
        if (seconds < last_ || seconds - last_ > gapSeconds) {
            // The clock went back or this thread stalled: the window under way is dropped.
            last_ = lastPair_ = seconds;
            windowStart_ = std::max(windowStart_, seconds);
            pairs_ = 0;
            pendingPairs_ = 0;
            return;
        }
        last_ = seconds;
        // A closed window counts once the picture after its last one came in
        // time: the second before a load or pause is not counted either.
        if (pendingPairs_ && (newPair || seconds - lastPair_ > gapSeconds)) {
            if (seconds - lastPair_ <= gapSeconds)
                commit(pendingPairs_, pendingSpan_);
            pendingPairs_ = 0;
        }
        if (newPair && seconds - lastPair_ > gapSeconds && seconds >= windowStart_) {
            // The first picture after a load or pause (none for over half a
            // second): a window starts from it, the idle one is never counted.
            lastPair_ = seconds;
            windowStart_ = seconds;
            pairs_ = 1;
            return;
        }
        if (newPair)
            lastPair_ = seconds;
        if (seconds < windowStart_)
            return; // the warm-up
        const double span = seconds - windowStart_;
        if (span >= windowSeconds) {
            if (pairs_ > 0 && seconds - lastPair_ <= gapSeconds) {
                pendingPairs_ = pairs_;
                pendingSpan_ = span;
            }
            windowStart_ = seconds;
            pairs_ = 0;
        }
        if (newPair)
            ++pairs_;
    }
    // RESET: the next frame starts the tracking again (no warm-up once started).
    void reset() {
        resetPending_ = true;
    }
    // NaN until a whole window has been tracked.
    float high() const {
        return windows_ ? high_ : std::nanf("");
    }
    float low() const {
        return windows_ ? low_ : std::nanf("");
    }
    float average() const {
        return windows_ && totalSeconds_ > 0 ? static_cast<float>(totalPairs_ / totalSeconds_) : std::nanf("");
    }
    // The seconds tracked (whole windows counted), and their number.
    double trackedSeconds() const {
        return totalSeconds_;
    }
    uint32_t windows() const {
        return windows_;
    }
    bool warmingUp() const {
        return !started_ || last_ < windowStart_;
    }

  private:
    void commit(uint32_t pairs, double span) {
        const float fps = static_cast<float>(pairs / span);
        high_ = windows_ ? std::max(high_, fps) : fps;
        low_ = windows_ ? std::min(low_, fps) : fps;
        totalPairs_ += pairs;
        totalSeconds_ += span;
        ++windows_;
    }
    void clearStats() {
        pendingPairs_ = 0;
        high_ = low_ = 0;
        totalPairs_ = 0;
        totalSeconds_ = 0;
        windows_ = 0;
    }
    double last_ = 0, lastPair_ = 0, pendingSpan_ = 0, windowStart_ = 0, totalPairs_ = 0, totalSeconds_ = 0;
    float high_ = 0, low_ = 0;
    uint32_t pairs_ = 0, pendingPairs_ = 0, windows_ = 0;
    bool started_ = false, resetPending_ = false;
};
// Tenths of a frame a second for SpidyHudSettings(8/9), 0 when none (NaN), at most 2047.
inline uint32_t fpsTenths(float fps) {
    if (!std::isfinite(fps) || fps <= 0)
        return 0;
    return static_cast<uint32_t>(std::clamp(std::lround(std::min(fps, 1000.f) * 10), 1l, 2047l));
}
// The FPS readout's colour band: 2 green (at least 90% of the refresh),
// 1 amber (at least 60%), 0 red, -1 unknown (either not finite).
inline int fpsBand(float fps, float refreshHz) {
    if (!std::isfinite(fps) || !std::isfinite(refreshHz) || refreshHz <= 0)
        return -1;
    return fps >= .9f * refreshHz ? 2 : fps >= .6f * refreshHz ? 1 : 0;
}
} // namespace spidy::wrist_hud

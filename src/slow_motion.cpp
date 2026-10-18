#include "spidy/slow_motion.hpp"
#include <algorithm>
#include <cmath>

namespace spidy {
namespace {
float positive(float value, float fallback) {
    return std::isfinite(value) && value > 0 ? value : fallback;
}
} // namespace
SlowMotion::SlowMotion(const SlowMotionTuning& tuning) : tuning_(tuning) {
    const SlowMotionTuning defaults;
    tuning_.scale = std::isfinite(tuning.scale) ? std::clamp(tuning.scale, .05f, 1.f) : defaults.scale;
    tuning_.enterSeconds = positive(tuning.enterSeconds, defaults.enterSeconds);
    tuning_.exitSeconds = positive(tuning.exitSeconds, defaults.exitSeconds);
    tuning_.drainSeconds = positive(tuning.drainSeconds, defaults.drainSeconds);
    tuning_.refillSeconds = positive(tuning.refillSeconds, defaults.refillSeconds);
    tuning_.refillDelay = std::isfinite(tuning.refillDelay) ? std::max(tuning.refillDelay, 0.f) : defaults.refillDelay;
    tuning_.minimum = std::isfinite(tuning.minimum) ? std::clamp(tuning.minimum, 0.f, 1.f) : defaults.minimum;
}
SlowMotion::Event SlowMotion::update(float seconds, bool pressed, bool allowed) {
    // A hitch or a paused headset counts as a tenth of a second at most.
    const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
    sinceChange_ += dt;
    sinceWarning_ += dt;
    sinceNeeded_ += dt;
    Event event = Event::none;
    const auto end = [&](Event why) {
        active_ = false;
        idle_ = sinceChange_ = 0;
        event = why;
    };
    if (active_ && !allowed) {
        end(Event::interrupted);
    } else if (pressed && allowed) {
        if (active_) {
            end(Event::stopped);
        } else if (focus_ >= tuning_.minimum && focus_ > 0) {
            active_ = true;
            sinceChange_ = 0;
            event = Event::started;
        } else {
            sinceWarning_ = sinceNeeded_ = 0;
            event = Event::refused;
        }
    }
    if (active_) {
        focus_ = std::max(0.f, focus_ - dt / tuning_.drainSeconds);
        if (focus_ <= 0) {
            end(Event::emptied);
            sinceWarning_ = 0;
        }
    } else {
        idle_ += dt;
        if (idle_ >= tuning_.refillDelay && focus_ < 1) {
            focus_ = std::min(1.f, focus_ + dt / tuning_.refillSeconds);
            if (focus_ >= 1)
                sinceNeeded_ = 0; // just filled: it shows full for a moment
        }
    }
    if (active_ || focus_ < 1)
        sinceNeeded_ = 0;
    const float rate = active_ ? 1 / tuning_.enterSeconds : -1 / tuning_.exitSeconds;
    progress_ = std::clamp(progress_ + rate * dt, 0.f, 1.f);
    const float shown = sinceNeeded_ < meterHold ? 1.f : 0.f;
    meter_ = shown > meter_ ? std::min(shown, meter_ + dt / meterIn) : std::max(shown, meter_ - dt / meterOut);
    return event;
}
float SlowMotion::blend() const {
    const float t = progress_;
    return t * t * t * (t * (t * 6 - 15) + 10);
}
float SlowMotion::timeScale() const {
    // Even steps in log time: halving from 1 to .5 takes as long as from .5 to .25.
    return std::exp(blend() * std::log(tuning_.scale));
}
SlowMotionView SlowMotion::view() const {
    SlowMotionView v;
    v.blend = blend();
    v.focus = focus_;
    v.meter = meter_;
    v.ripple = sinceChange_ < rippleSeconds ? sinceChange_ / rippleSeconds : -1.f;
    v.warning = sinceWarning_ < warningSeconds ? 1 - sinceWarning_ / warningSeconds : 0.f;
    v.entering = v.active = active_;
    return v;
}
void SlowMotion::reset() {
    *this = SlowMotion(tuning_);
}
SlowMoments::SlowMoments(const MomentTuning& tuning) : tuning_(tuning) {
    const MomentTuning defaults;
    tuning_.scale = std::isfinite(tuning.scale) ? std::clamp(tuning.scale, .05f, 1.f) : defaults.scale;
    tuning_.light = std::isfinite(tuning.light) ? std::clamp(tuning.light, tuning_.scale, 1.f) : defaults.light;
    scale_ = tuning_.scale;
    tuning_.in = positive(tuning.in, defaults.in);
    tuning_.out = positive(tuning.out, defaults.out);
    tuning_.rest = std::isfinite(tuning.rest) ? std::max(tuning.rest, 0.f) : defaults.rest;
    for (unsigned i = 0; i < momentKinds; ++i)
        tuning_.hold[i] = positive(tuning.hold[i], defaults.hold[i]);
}
bool SlowMoments::start(Moment kind) {
    const unsigned k = static_cast<unsigned>(kind) % momentKinds;
    const bool going = left_ > 0;
    if (!going && kind != Moment::dodge && sinceEnd_ < tuning_.rest)
        return false;
    if (!going)
        sinceStart_ = 0;
    const float scale = kind == Moment::knockout ? tuning_.light : tuning_.scale;
    scale_ = going || level_ > 0 ? std::min(scale_, scale) : scale;
    left_ = std::max(left_, tuning_.hold[k]);
    ++counts_[k];
    return true;
}
void SlowMoments::update(float seconds, bool allowed) {
    const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
    sinceStart_ = std::min(sinceStart_ + dt, 1e9f);
    if (!allowed) {
        if (left_ > 0)
            sinceEnd_ = 0;
        left_ = level_ = 0;
        sinceEnd_ = std::min(sinceEnd_ + dt, 1e9f);
        return;
    }
    if (left_ > 0) {
        // It holds once it is in.
        if (level_ >= 1)
            left_ -= dt;
        level_ = std::min(1.f, level_ + dt / tuning_.in);
        if (left_ <= 0) {
            left_ = 0;
            sinceEnd_ = 0;
        }
    } else {
        level_ = std::max(0.f, level_ - dt / tuning_.out);
        sinceEnd_ = std::min(sinceEnd_ + dt, 1e9f);
    }
}
float SlowMoments::blend() const {
    return level_ * level_ * (3 - 2 * level_);
}
float SlowMoments::timeScale() const {
    return std::exp(blend() * std::log(scale_));
}
SlowMotionView SlowMoments::view(SlowMotionView v) const {
    // Fainter than slow motion's own: it is over in a moment. A light one
    // fainter yet, by how much less it slows, and with no ring.
    const float depth = tuning_.scale < 1 ? std::log(scale_) / std::log(tuning_.scale) : 0.f;
    v.blend = std::max(v.blend, .6f * blend() * depth);
    if (depth >= 1 && v.ripple < 0 && sinceStart_ < SlowMotion::rippleSeconds) {
        v.ripple = sinceStart_ / SlowMotion::rippleSeconds;
        v.entering = true;
    }
    return v;
}
void SlowMoments::reset() {
    *this = SlowMoments(tuning_);
}
bool StickPress::update(float seconds, bool focused, bool mine, bool other) {
    if (!focused) {
        // Held through it, the stick waits for a release.
        down_ = mine;
        decided_ = true;
        chord_ = false;
        return false;
    }
    if (!mine) {
        const bool click = down_ && !decided_ && !chord_;
        down_ = decided_ = chord_ = false;
        return click;
    }
    if (!down_) {
        down_ = true;
        decided_ = false;
        chord_ = other;
        held_ = 0;
    } else {
        held_ += std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
    }
    chord_ = chord_ || other;
    if (decided_ || chord_ || held_ < chordSeconds)
        return false;
    decided_ = true;
    return true;
}
} // namespace spidy

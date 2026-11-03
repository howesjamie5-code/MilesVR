#pragma once
#include "wall_crawl_body.hpp"

namespace spidy {
// The visual run is driven by confirmed surface velocity, not the engine's
// animation state. It also works when custom movement leaves that pose frozen.
class WallRunBody {
  public:
    bool update(const GameMotionFrame& frame, const SurfaceHold& hold, Vec3 velocity, bool enabled,
                bool grounded, unsigned hands, float seconds, float momentumPace = 1.f) {
        const bool momentum = running_ ? hold.speed > wallStandSpeed : hold.speed > 3.f;
        if (!wallRunPose(frame, hold, enabled, grounded, hands, momentum) || !finite(velocity) ||
            !std::isfinite(seconds) || seconds <= 0 || seconds > .1f) {
            *this = {}; return false;
        }
        const Vec3 n = normalized(hold.normal);
        velocity -= n * dot(velocity, n);
        const float speed = length(velocity);
        if (speed < .25f || speed > 80.f) { *this = {}; return false; }
        const Vec3 direction = velocity / speed;
        if (!running_) ahead_ = direction;
        else {
            ahead_ = arc(normal_, n).rotate(ahead_);
            ahead_ = normalized(ahead_ - n * dot(ahead_, n));
            const float turn = std::atan2(dot(cross(ahead_, direction), n), dot(ahead_, direction));
            ahead_ = Quat::around(n, std::clamp(turn, -10.f * seconds, 10.f * seconds)).rotate(ahead_);
        }
        // Keep the normal running cycle when swinging carries us faster along a wall.
        // Only the visual cadence is capped; movement and the reported speed stay unchanged.
        const Vec3 wallUp = normalized(Vec3{0, 1, 0} - n * n.y);
        const float horizontal = 1 - std::abs(dot(direction, wallUp));
        const float pace = !frame.standing && std::isfinite(momentumPace) ?
            1 + (std::clamp(momentumPace, .5f, 1.5f) - 1) * horizontal : 1.f;
        const float cycleSpeed = std::min(speed, 6.f) * pace;
        phase_ = std::fmod(phase_ + cycleSpeed * seconds * (6.2831853f / 4.f), 6.2831853f);
        normal_ = n; speed_ = speed; floor_ = hold.anchor - frame.anchor; running_ = true;
        return true;
    }
    Vec3 normal() const { return normal_; }
    Vec3 ahead() const { return ahead_; }
    Vec3 floor() const { return floor_; }
    float phase() const { return phase_; }
    float speed() const { return speed_; }
  private:
    bool running_{};
    Vec3 normal_{}, ahead_{}, floor_{};
    float phase_{}, speed_{};
};
} // namespace spidy

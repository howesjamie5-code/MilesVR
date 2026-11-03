#include "spidy/shooter.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace spidy {
Shooter::Shooter(ShooterConfig c) : config_(c) {
    const auto bad = [](float v) { return !std::isfinite(v) || v < 0; };
    if (bad(c.press) || c.press > 1 || bad(c.release) || c.release >= c.press || bad(c.interval) ||
        bad(c.muzzle) || bad(c.range) || c.range <= 0 || bad(c.beyond) || bad(c.assistAngle) ||
        c.assistAngle >= 1.5f || bad(c.assistRange))
        throw std::invalid_argument("Invalid web shooter configuration");
}
void Shooter::reset() {
    hands_ = {};
}
std::uint32_t Shooter::update(float seconds, const std::array<ShooterHand, 2>& hands) {
    if (!std::isfinite(seconds) || seconds <= 0)
        return 0;
    std::uint32_t shots{};
    for (int i = 0; i < 2; ++i) {
        const auto& in = hands[static_cast<size_t>(i)];
        auto& h = hands_[static_cast<size_t>(i)];
        h.since += seconds;
        const Vec3 forward = in.aim.orientation.rotate({0, 0, -1});
        if (!in.tracked || !std::isfinite(in.trigger) || !finite(in.aim.position) || !finite(forward) ||
            std::abs(length(forward) - 1) > .1f) {
            h.held = true; // a full release before the next shot
            continue;
        }
        const bool held = h.held ? in.trigger > config_.release : in.trigger > config_.press;
        // Only a pull shoots: a trigger pulled while its web held something
        // and still held after the web let go does not.
        if (held && !h.held && !in.busy && h.since >= config_.interval) {
            shots |= 1u << i;
            h.since = 0;
        }
        h.held = held;
    }
    return shots;
}
ShotRequest Shooter::aim(int hand, Pose pose, std::span<const ShooterTarget> targets, const WorldQueries& world,
                         const ShooterGaze* gaze) const {
    const Vec3 forward = normalized(pose.orientation.rotate({0, 0, -1}));
    ShotRequest shot;
    shot.hand = hand;
    shot.origin = pose.position + forward * config_.muzzle;
    shot.direction = forward;
    // The line that picks: the hand's, or the eyes' (EYE AIM).
    const bool eyes = gaze && finite(gaze->origin) && finite(gaze->direction) &&
                      std::abs(length(gaze->direction) - 1) < .01f;
    const Vec3 from = eyes ? gaze->origin : shot.origin, line = eyes ? gaze->direction : forward;
    // The character nearest the line, as a share of the angle he may be off it.
    const ShooterTarget* best{};
    float bestShare = 2;
    for (const auto& t : targets) {
        const Vec3 to = t.centre - from;
        const float distance = length(to);
        if (!finite(t.centre) || distance < .3f || distance > config_.assistRange)
            continue;
        const Vec3 along = to / distance;
        const float angle = std::acos(std::clamp(dot(along, line), -1.f, 1.f));
        const float allowed = std::max(config_.assistAngle, std::atan2(std::max(t.radius, 0.f), distance));
        if (angle > allowed || angle / allowed >= bestShare)
            continue;
        // A wall between them takes the ball first (one the eyes cannot see
        // past, with EYE AIM).
        if (const auto hit = world.raycast(from, along, distance);
            hit && length(hit->point - from) < distance - std::max(t.radius, 0.f))
            continue;
        best = &t;
        bestShare = angle / allowed;
    }
    if (best) {
        shot.direction = normalized(best->centre - shot.origin);
        shot.aimPoint = best->centre;
        shot.target = best->id;
        shot.gazed = eyes;
        return shot;
    }
    const auto hit = world.raycast(from, line, config_.range);
    const Vec3 point = hit ? hit->point : from + line * config_.range;
    if (eyes) {
        // From the hand to what the eyes meet; the hand itself, looked at,
        // aims as it points.
        const Vec3 toPoint = point - shot.origin;
        if (!(length(toPoint) >= ShooterGaze::nearest))
            return aim(hand, pose, targets, world);
        shot.direction = normalized(toPoint);
        shot.gazed = true;
    }
    shot.surface = static_cast<bool>(hit);
    shot.aimPoint = hit ? point + shot.direction * config_.beyond : point;
    return shot;
}
} // namespace spidy

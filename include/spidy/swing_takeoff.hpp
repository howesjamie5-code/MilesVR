#pragma once
#include "math.hpp"
#include <cstdint>

namespace spidy {
// Native walking keeps the actor attached to its support. Give its jump action
// time to clear that support before resuming leased web velocity requests.
class SwingTakeoff {
  public:
    enum Phase : uint32_t { Idle, ReleaseJump, PressJump, AwaitAir, TimedOut };
    struct Result {
        bool waiting{}, jump{}, resumed{}, timedOut{};
        Vec3 launchVelocity{};
    };
    // surface: out of what holds the body as the takeoff begins - the world's
    // up on the ground, a wall's normal on the game's own crawl. The body is
    // off once it is 12 cm out along it: a jump off the ground goes up, one
    // off a wall goes out from it and a little down (October 14, the first
    // mission's shaft: 70.27 m to 69.26 m), and waiting for it to come 12 cm
    // above where it started held a web from a crawled wall a third of a
    // second longer than it needed.
    Result update(uint64_t now, bool attached, bool supported, bool collidable, bool wantsLift,
                  bool newGesture, Vec3 position, Vec3 velocity, Vec3 impulse, bool pointLaunch = false,
                  Vec3 surface = {0, 1, 0}) {
        pointLaunch_ |= pointLaunch;
        if (!attached && !pointLaunch_) {
            reset();
            return {};
        }
        if (phase_ == TimedOut) {
            if (!wantsLift || newGesture) {
                reset();
                pointLaunch_ = pointLaunch;
            } else
                return {};
        }
        if (phase_ == Idle) {
            if (!(supported || !collidable) || !wantsLift)
                return {};
            phase_ = ReleaseJump;
            started_ = attemptStarted_ = now;
            origin_ = position;
            surface_ = finite(surface) && length(surface) > .5f ? normalized(surface) : Vec3{0, 1, 0};
            attempts_ = 1;
        }
        // Remember a deliberate yank while the game performs takeoff. Collision
        // feedback during these frames must not erase that one-shot gesture.
        if (finite(impulse) && impulse.y > 0 && length(impulse) > length(launch_))
            launch_ = impulse;
        if (!supported && collidable && dot(position - origin_, surface_) > .12f)
            ++airSamples_;
        else
            airSamples_ = 0;
        if (airSamples_ >= 2 && now - started_ >= 120) {
            const Vec3 direction = normalized(launch_);
            const Vec3 restored =
                velocity + direction * std::max(0.f, length(launch_) - dot(velocity, direction));
            reset();
            return {false, false, true, false, restored};
        }
        if (now - started_ >= 1400) {
            phase_ = TimedOut;
            launch_ = {};
            return {false, false, false, true, {}};
        }
        // A neutral interval supplies a new edge even if jump was held earlier.
        // Retry once if the first edge arrived during a native animation.
        if (now - attemptStarted_ >= 650 && attempts_ < 2) {
            attemptStarted_ = now;
            ++attempts_;
        }
        const auto elapsed = now - attemptStarted_;
        phase_ = elapsed < 60 ? ReleaseJump : elapsed < 220 ? PressJump : AwaitAir;
        return {true, phase_ == PressJump, false, false, {}};
    }
    void reset() {
        phase_ = Idle;
        started_ = attemptStarted_ = 0;
        attempts_ = airSamples_ = 0;
        pointLaunch_ = false;
        origin_ = launch_ = {};
        surface_ = {0, 1, 0};
    }
    Phase phase() const {
        return phase_;
    }
    uint32_t attempts() const {
        return attempts_;
    }

  private:
    Phase phase_ = Idle;
    uint64_t started_{}, attemptStarted_{};
    uint32_t attempts_{}, airSamples_{};
    bool pointLaunch_{};
    Vec3 origin_{}, launch_{}, surface_{0, 1, 0};
};

// The input bridge's Space bit (game_bridge_protocol.hpp keyCodes[4]).
constexpr uint32_t swingJumpKey = 1u << 4;
inline uint32_t swingNativeKeys(bool active, bool owned, uint32_t keys, SwingTakeoff::Phase phase,
                                bool takeoffJump) {
    constexpr uint32_t jump = swingJumpKey;
    if (!active)
        return 0;
    if (owned)
        keys &= jump; // A jump held since the ground stays held (AirJumpFilter).
    if (phase == SwingTakeoff::ReleaseJump || phase == SwingTakeoff::PressJump ||
        phase == SwingTakeoff::AwaitAir)
        keys &= ~jump;
    if (takeoffJump)
        keys |= jump;
    return keys;
}
// In the air the game's jump is its web zip: a web from Spider-Man's own
// wrist, where VR's webs come from the hands. A press that starts while the
// player is airborne stays away from the game until it is let go. One that
// starts on the ground, a wall or a perch is the game's jump for as long as
// it is held, into the air included.
class AirJumpFilter {
  public:
    uint32_t update(uint32_t keys, bool airborne) {
        const bool jump = (keys & swingJumpKey) != 0;
        if (!jump)
            fromAir_ = false;
        else if (!held_)
            fromAir_ = airborne;
        held_ = jump;
        return fromAir_ ? keys & ~swingJumpKey : keys;
    }

  private:
    bool held_{}, fromAir_{};
};
} // namespace spidy

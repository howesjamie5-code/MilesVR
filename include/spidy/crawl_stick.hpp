#pragma once
#include "math.hpp"
#include <cmath>
#include <cstdint>

namespace spidy {
// What the game's own wall crawl does with the stick Spidy sends it.
//
// Why it exists. On a wall the game's crawl is the only thing that holds the
// player in a state the game calls supported, and so the only state where its
// own interacts - a vent, a door, a pickup, a prompt - work at all. Spidy's
// own wall running holds him beside the wall while the game still has him in
// its air state, and the game takes its interact away 63 ms after that. That
// is the whole family of bugs: not the vent, the state.
//
// So the crawl holds the player, and Spidy gives him the view. What was left
// unmeasured is the stick. The game reads it in a frame of its own while it
// crawls - its camera, as it holds it there - and a player standing on a wall
// with the view turned onto it means something different by "forward" than a
// player on the ground does. GameTrackingRig now lays the stick's axes into
// the surface for the crawl, which is right if the crawl reads them in the
// game camera's frame; whether it does is what this measures.
//
// What it gathers: the turn from the direction sent to the direction taken,
// both in the surface's own axes, as a circular mean. A session then says
// which of two things is true.
//
//   agreement near 1  the crawl's frame is fixed against the surface, and the
//                     stick wants rotating by `turn` - a one-line correction.
//   agreement near 0  the frame is not fixed, so the stick is the wrong
//                     channel and the mover wants driving instead, as the
//                     swing drives it on a wall of its own.
//
// Either way the answer is one number out of one session, which is what three
// builds of guessing could not get at.
class CrawlStick {
  public:
    // A stick pushed less than this is nobody's intent, and a body moving
    // slower than this took no direction from it.
    static constexpr float stick = .35f, moving = .6f;

    // normal: out of the surface the game holds the player on, unit.
    // forwardAxis: the axis `sentForward` was measured along, in the surface.
    // sent: the stick as it went to the game. taken: the body's velocity.
    void update(Vec3 normal, Vec3 forwardAxis, float sentRight, float sentForward, Vec3 taken) {
        const Vec3 up = normalized(normal), ahead = normalized(forwardAxis);
        if (!finite(up) || !finite(ahead) || length(up) < .5f || length(ahead) < .5f)
            return;
        // The surface's own right, as the rig lays it out: ahead x up, so that
        // on the ground (up = the world's) it is the stick's own right.
        const Vec3 side = cross(ahead, up);
        if (!finite(taken) || !std::isfinite(sentRight) || !std::isfinite(sentForward))
            return;
        if (std::hypot(sentRight, sentForward) < stick)
            return;
        // Only what the body did along the surface: the crawl rocks on it, and
        // a step into or out of it is not a direction the stick asked for.
        const Vec3 along{dot(taken, side), dot(taken, ahead), 0};
        const float speed = std::hypot(along.x, along.y);
        if (speed < moving)
            return;
        const float wanted = std::atan2(sentRight, sentForward);
        const float got = std::atan2(along.x, along.y);
        const float turn = got - wanted;
        cos_ += std::cos(turn);
        sin_ += std::sin(turn);
        speed_ += speed;
        ++samples_;
    }

    unsigned samples() const { return samples_; }
    // The mean turn from the stick sent to the way the body went, radians.
    float turn() const { return samples_ ? std::atan2(sin_, cos_) : 0.f; }
    // How alike those turns were, 0 to 1: the circular mean's resultant.
    float agreement() const {
        return samples_ ? std::hypot(cos_, sin_) / static_cast<float>(samples_) : 0.f;
    }
    float speed() const { return samples_ ? speed_ / static_cast<float>(samples_) : 0.f; }

  private:
    float cos_{}, sin_{}, speed_{};
    unsigned samples_{};
};
} // namespace spidy

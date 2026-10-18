#pragma once
#include "math.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// The game's own web pull from the hands. Where the game shows LB + RB (a
// door or a grate it has Spider-Man tear off with two webs), those two
// buttons are the only way on, and in VR play the grips are no bumpers:
// they shoot the player's webs. Those webs do not hold on such a thing (the
// game moves it, as it moves a car; the aim marker is the red cross), and
// the web grab does not know it: on October 11 sixteen grip presses in a
// row missed a door of the first mission, and nothing else reached it.
//
// So: both web buttons pressed with both hands pointing at one such thing
// close by, and then a yank, either hand pulled sharply back from it as for
// a zip, is the game's LB + RB, held for as long as both buttons stay down.
// Spidy does not see the game's prompt; a pull at a thing that has none
// gives the game its LB + RB all the same (its web throw takes a prop that
// lies close by, if one does).
namespace spidy {
struct WebPullConfig {
    // How far from the hand the thing may be, and how far apart the two
    // hands' points on it.
    float reach = 12, span = 4;
    // The yank, as the swing's zip has it (SwingConfig).
    float yankSpeed = 1.3f, yankDistance = .14f, yankWindow = .25f;
    // LB + RB stay down while both web buttons do: this long at least (a
    // yank often lets go of a grip) and at most.
    float minHold = .3f, maxHold = 4;
};
struct WebPullHand {
    bool held{};     // the hand's web button is down
    bool blocked{};  // its web would miss on something it points at (game_swing's AimKind::blocked)
    Vec3 point{};    // that thing, where the hand points
    Vec3 position{}; // the hand, in the world
    Vec3 motion{};   // the grip relative to the head, in tracking space (metres)
};
class WebPull {
  public:
    explicit WebPull(WebPullConfig config = {}) : config_(config) {
    }
    // seconds: since the previous update. ready: the game has the player, on
    // the ground, in VR, and no web of his holds anything. turn: the tracking
    // space's orientation in the world (trackingTurn). Returns whether the
    // game's LB and RB are down.
    bool update(float seconds, bool ready, const Quat& turn, const std::array<WebPullHand, 2>& hands) {
        const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
        if (pulling_) {
            // Once begun it ends by its own rule: what the game does with the
            // two buttons (a cutscene, a camera of its own) may end `ready`.
            since_ += dt;
            const bool both = hands[0].held && hands[1].held;
            if (since_ >= config_.maxHold || (!both && since_ >= config_.minHold))
                pulling_ = false;
        }
        for (int i = 0; i < 2; ++i) {
            const auto& h = hands[i];
            auto& s = hands_[i];
            // A button held into `ready` aims at nothing: a new press does.
            const bool press = h.held && !s.down && ready;
            s.down = h.held;
            if (!h.held || !ready)
                s.aimed = false;
            if (press) {
                s.aimed = h.blocked && finite(h.point) && finite(h.position) &&
                          length(h.point - h.position) <= config_.reach;
                s.point = h.point;
                s.sample = false;
                s.distance = s.time = 0;
            }
        }
        const bool onOne = hands_[0].aimed && hands_[1].aimed &&
                           length(hands_[0].point - hands_[1].point) <= config_.span;
        bool yanked{};
        for (int i = 0; i < 2; ++i) {
            const auto& h = hands[i];
            auto& s = hands_[i];
            if (!s.aimed || !finite(h.motion) || !finite(h.position)) {
                s.sample = false;
                continue;
            }
            // Motion must be both fast and deliberate within a short window,
            // away from the thing (Swing::inputs).
            if (s.sample && dt > 0 && onOne) {
                const float pull = -dot(turn.rotate(h.motion - s.previous), normalized(s.point - h.position));
                if (pull > 0) {
                    s.distance += pull;
                    s.time += dt;
                    if (s.time > config_.yankWindow) {
                        s.distance = pull;
                        s.time = dt;
                    }
                    yanked |= s.distance >= config_.yankDistance && pull / dt >= config_.yankSpeed;
                } else {
                    s.distance = s.time = 0;
                }
            }
            s.previous = h.motion;
            s.sample = true;
        }
        if (yanked && !pulling_) {
            pulling_ = true;
            since_ = 0;
            ++pulls_;
            // One pull a press of the two.
            hands_[0].aimed = hands_[1].aimed = false;
        }
        return pulling_;
    }
    // Both hands are on one thing a yank would pull.
    bool aimed() const {
        return hands_[0].aimed && hands_[1].aimed &&
               length(hands_[0].point - hands_[1].point) <= config_.span;
    }
    uint64_t pulls() const {
        return pulls_;
    }

  private:
    struct Hand {
        bool down{}, aimed{}, sample{};
        Vec3 point{}, previous{};
        float distance{}, time{};
    };
    WebPullConfig config_;
    std::array<Hand, 2> hands_{};
    bool pulling_{};
    float since_{};
    uint64_t pulls_{};
};
} // namespace spidy

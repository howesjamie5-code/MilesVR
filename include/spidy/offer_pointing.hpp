#pragma once
#include "math.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

// Pointing a hand at one of the game's own interactions - a vent, a door - on
// Spidy's wall, and using it with the interact. The player, October 15: "why
// not allow me to point with my hand to select interactions?"
//
// In the game (game_offers) the game is made to see the player crawling where
// Spidy's wall holds him, and says which of the offers it gathered near him he
// could use from there. A use asked for puts its own crawl under him there
// (game_warp); on that crawl the game picks the offer itself, and its own
// interact runs it. Here: which offer a hand points at, and the press. The
// player's interact on an offer asks for its use and stays off the game - on
// Spidy's wall the game's interact does nothing - and once the game's crawl
// has him and the game has picked that offer, the game's interact goes out
// for him.
namespace spidy::offer_pointing {
// game_offers' reasons: the bits the game's Offer::IsAvailableTo says no
// with. 4 is the offer's range (a vent's: within 1.25 m of its opening).
constexpr uint32_t rangeReason = 4;
// One of the offers the game gathered near the player, as the game answered
// for him where he is: `usable`, it would let him use it now, or at a spot on
// the wall within his reach (`reachable`: the game's crawl goes there for the
// use); `outOfReach`, the one thing it says against it is the distance and no
// such spot is known yet, so walking up to it will do.
struct Offer {
    uint32_t handle{};
    Vec3 position{};
    bool usable{}, outOfReach{};
};
inline Offer fromGame(uint32_t handle, Vec3 position, uint32_t reasons, bool enabled, bool reachable = false) {
    return {handle, position, enabled && (!reasons || (reasons == rangeReason && reachable)),
            enabled && reasons == rangeReason && !reachable};
}
// A hand's aim: from where, which way.
struct Aim {
    bool tracked{};
    Vec3 origin{}, direction{};
};
struct Pointed {
    uint32_t handle{};
    Vec3 position{};
    bool usable{};
    float angle{}; // radians off the hand's aim
};
// An offer is pointed at within coneRadians (12 degrees) of the aim, or
// within nearRadius of the aim's line where that is wider - a vent a metre
// away is wider than the cone - from minDistance to maxDistance off the hand.
// Of those, the one nearest the aim. The marker round it is markRadius.
constexpr float coneRadians = .21f, nearRadius = .35f, minDistance = .2f, maxDistance = 6, markRadius = .4f;
inline std::optional<Pointed> pick(const Aim& aim, const Offer* offers, unsigned count) {
    if (!aim.tracked || !offers || !finite(aim.origin) || !finite(aim.direction) || length(aim.direction) < .5f)
        return {};
    const Vec3 way = normalized(aim.direction);
    std::optional<Pointed> best;
    for (unsigned i = 0; i < count; ++i) {
        const auto& o = offers[i];
        if (!o.handle || !(o.usable || o.outOfReach) || !finite(o.position))
            continue;
        const Vec3 to = o.position - aim.origin;
        const float distance = length(to);
        if (!(distance >= minDistance && distance <= maxDistance))
            continue;
        const float angle = std::acos(std::clamp(dot(to, way) / distance, -1.f, 1.f));
        if (angle > std::max(coneRadians, std::atan2(nearRadius, distance)))
            continue;
        if (!best || angle < best->angle)
            best = Pointed{o.handle, o.position, o.usable, angle};
    }
    return best;
}
// Of both hands' picks, the one the interact uses: a usable one before one
// out of reach, then the one nearer its hand's aim.
inline std::optional<Pointed> either(const std::optional<Pointed>& left, const std::optional<Pointed>& right) {
    if (!left || !right)
        return left ? left : right;
    if (left->usable != right->usable)
        return left->usable ? left : right;
    return left->angle <= right->angle ? left : right;
}

// The interact on the offer a hand points at, from the press to the game's own
// interact.
class Use {
  public:
    enum class Phase : uint8_t {
        idle,
        asked,    // the use asked for: the swing puts the game's crawl under the player
        settling, // the crawl has him: the game picks the offer at its next look
        pressing, // the game's interact down for him
        watching, // let go: the game takes it, or it is pressed once more
    };
    // The game's crawl has the player within patience of the press, or the use
    // is given up (the swing has him back by then: it gives the crawl 1.5 s).
    // Then up to settleSeconds for the game to pick the offer, and from its
    // pick pickDelay more: the game raises its own prompt for the offer as it
    // picks it (POIInteractAvailable, in the same frame), and a press that came
    // in that frame was lost and the prompt with it - no press took the player
    // in after it until he had walked out of the offer's reach and back (the
    // first mission's vent, October 15; pressed a second after the pick, the
    // same use went in). The interact is held pressSeconds, a press as a
    // player's is; if the game still holds the offer retryAfter from its
    // start, it is pressed once more, maxPresses in all.
    static constexpr float patience = 2.5f, settleSeconds = .6f, pickDelay = .3f, pressSeconds = .35f,
                           retryAfter = .8f;
    static constexpr unsigned maxPresses = 2;
    // What the game says (game_offers::Data): the uses that ended with its
    // crawl taking the player, and those that did not, so far; the offer it
    // has picked for him now.
    struct Game {
        uint64_t taken{}, declined{};
        uint32_t best{};
    };
    // `seconds` since the previous update; `interact`, the player's interact
    // button down; `onOwnWall`, Spidy's wall holds him; `pointed`, what a
    // hand points at; `ask(handle)`, asks the game for that offer's use
    // (SpidyOffersUse), true when it agreed. Returns whether the game's
    // interact is down: the player's own press - not one that asked for a
    // use, nor one that began while a use is under way - and the use's own.
    template <class Ask>
    bool update(float seconds, bool interact, bool onOwnWall, const std::optional<Pointed>& pointed,
                const Game& game, Ask&& ask) {
        const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
        const bool rising = interact && !down_;
        down_ = interact;
        if (!interact)
            swallowed_ = false;
        else if (rising && phase_ != Phase::idle)
            swallowed_ = true;
        switch (phase_) {
        case Phase::idle:
            if (rising && onOwnWall && pointed && pointed->usable) {
                swallowed_ = true;
                if (ask(pointed->handle)) {
                    phase_ = Phase::asked;
                    handle_ = pointed->handle;
                    time_ = 0;
                    sincePick_ = -1;
                    pressed_ = 0;
                    taken_ = game.taken;
                    declined_ = game.declined;
                    ++asks_;
                } else {
                    ++refusals_;
                }
            }
            break;
        case Phase::asked:
            time_ += dt;
            if (game.taken != taken_) {
                phase_ = Phase::settling;
                time_ = 0;
            } else if (game.declined != declined_ || time_ > patience) {
                phase_ = Phase::idle;
                ++failures_;
            }
            break;
        case Phase::settling:
            time_ += dt;
            if (game.best == handle_)
                sincePick_ = sincePick_ < 0 ? 0 : sincePick_ + dt;
            if (sincePick_ >= pickDelay || (sincePick_ < 0 && time_ >= settleSeconds)) {
                picked_ += sincePick_ >= 0;
                press();
            }
            break;
        case Phase::pressing:
            time_ += dt;
            if (time_ >= pressSeconds)
                phase_ = Phase::watching;
            break;
        case Phase::watching:
            time_ += dt;
            // The game has taken it (its pick goes as the vent's way in begins),
            // or never picked it: done.
            if (game.best != handle_)
                phase_ = Phase::idle;
            else if (time_ >= retryAfter) {
                if (pressed_ < maxPresses) {
                    ++retries_;
                    press();
                } else {
                    phase_ = Phase::idle;
                    ++unanswered_;
                }
            }
            break;
        }
        return phase_ == Phase::pressing || (interact && !swallowed_);
    }
    Phase phase() const {
        return phase_;
    }
    // A use under way: the stick stays off the game, which would walk the
    // player off the place the offer was checked at.
    bool busy() const {
        return phase_ != Phase::idle;
    }
    uint32_t handle() const {
        return handle_;
    }
    // Presses that asked for a use, those the game refused at once (the offer
    // gone by then), those that came to the game's interact and of those the
    // ones the game had picked the offer for, and those given up.
    uint32_t asks() const {
        return asks_;
    }
    uint32_t refusals() const {
        return refusals_;
    }
    uint32_t presses() const {
        return presses_;
    }
    uint32_t picked() const {
        return picked_;
    }
    uint32_t failures() const {
        return failures_;
    }
    // Presses made again because the game still held the offer, and uses whose
    // every press it kept holding it through.
    uint32_t retries() const {
        return retries_;
    }
    uint32_t unanswered() const {
        return unanswered_;
    }

  private:
    void press() {
        phase_ = Phase::pressing;
        time_ = 0;
        ++pressed_;
        ++presses_;
    }
    Phase phase_ = Phase::idle;
    bool down_{}, swallowed_{};
    uint32_t handle_{};
    // Time in the phase; since the game picked the offer (-1 not yet); the
    // presses of this use.
    float time_{}, sincePick_ = -1;
    unsigned pressed_{};
    uint64_t taken_{}, declined_{};
    uint32_t asks_{}, refusals_{}, presses_{}, picked_{}, failures_{}, retries_{}, unanswered_{};
};
} // namespace spidy::offer_pointing

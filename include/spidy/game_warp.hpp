#pragma once
#include "math.hpp"
#include <cstdint>

// The game's own wall crawl, put under the player where Spidy holds him on a
// wall at rest.
//
// On Spidy's own wall the game has the hero in its air state, and in the air
// it registers no interact: the hero's Hero::HeroOfferProcessor takes offers
// of the kinds in its mask (+0x1c8) only - every kind on the ground, 0x23 on
// its crawl, 0x2 in the air - and a vent (VentOffer, kind 0x1) also asks its
// own entry check (+0x128), which fails the moment the crawl ends. Measured
// at the first mission's vent on October 14: on the crawl the vent was the
// processor's best offer; off it, the mask went to 0, then 0x2 on Spidy's
// wall, and the vent's reason (Offer +0xf4, 0x40) was its own check.
//
// Hero::HeroTransitionManager, the hero's component for going from one
// traversal to another, has the game's own way onto a wall: its +0x360
// (980f50) takes a point on the wall, its normal, a hint, flags and a time,
// picks how to go on from the hero's current state and asks the hero's state
// machine ([manager+0x50], a SyncStateMachine, whose +0x50 requests a state)
// for HeroStateWallCrawlEnter - the way its own wall grabs go in. From the
// swing's wall at rest that took the hero onto the wall at the same height,
// no drop, in 0.17 s (in the game, October 14). Its WarpToWallCrawl (9938c0)
// goes straight to HeroStateWallCrawlIdle instead: that held the hero at
// once, then crashed the game in its audio thread as soon as the crawl moved
// (a null at +0x148, 2fde47d) - the crawl's sound is set up by Enter, which
// the warp skips. Its let-go (HeroStateWallCrawlToFall) pushes the hero 1.3 m
// off the wall for 0.35 s before he falls, a body nothing of Spidy's can hold:
// off the crawl the player goes by the game's own ways (its wall run, a jump).
//
// The request goes out on the game's main thread, in the camera's update
// after the hero's own (game_shooter's frame hook calls frame()), the way the
// shooter's shots go out.
namespace spidy::game_warp {
struct Data {
    uint32_t magic = 0x53575044, version = 4, bytes = sizeof(Data), status{}; // 0 off, 2 running
    int64_t sequence{};
    // Crawls asked for, the game's answer to each (applied: its state
    // machine took the request), and those that never reached it.
    uint64_t asked{}, applied{}, refused{}, faulted{}, noManager{}, frames{};
    uint64_t manager{}, record{};
    // The last crawl asked for: the point, the wall's normal.
    Vec3 point{}, normal{};
    // The swing's view of the hand-over as its last visit had it (observe):
    // 1 the game's crawl holds the player, 2 input focused, 4 standing,
    // 8 a crawl found under the actor, 16 the swing owns the body, 32 the
    // game's own wall run, 64 at rest on the swing's wall, 128 handing him
    // over.
    uint32_t swing{};
    // The swing's visits, and those with a step of the player's mover.
    uint64_t visits{}, steps{};
    // Hand-overs the game's crawl took, and those the swing took back (the
    // game did not begin, did not finish, or the body dropped).
    uint64_t taken{}, declined{};
};
static_assert(sizeof(Data) == 152);

// Checks this game build's functions. 0 when ready; 9601 when they are not
// this game's.
uint32_t start(uintptr_t base);
// The hero's transition manager on this actor record, found in the game's
// component registry: a scan of a few milliseconds, so never on the game's
// own thread. False when the record has none.
bool target(uintptr_t record);
// The game's crawl on the wall through `point`, `normal` out of the wall.
// Asked for at the next main-thread frame; a newer request replaces one still
// waiting. False when off or no manager is known.
bool crawl(Vec3 point, Vec3 normal);
// The camera update's turn, on the game's main thread.
void frame();
// The swing's view of the hand-over (Data::swing), at each of its visits,
// and how one it asked for ended.
void observe(uint32_t swing, bool stepped);
void outcome(bool taken);
Data data();
void stop();
} // namespace spidy::game_warp

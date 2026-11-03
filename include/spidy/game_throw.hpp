#pragma once
#include "throw_aim.hpp"
#include <cstdint>

// The game's throw of a prop, aimed where the free hand points
// (throw_aim.hpp). Hooks the three getters the throw reads its target
// through, and while a hand aims (SpidyThrowAim, renewed each frame of the
// hold) gives the throw's slot (4) the enemy the hand points at - his handle
// and his first aim point - or, at no enemy, the way the hand points. Every
// other slot, and the throw's slot with no hand aiming, is the game's own.
namespace spidy::game_throw {
// SpidyThrowData, for a probe and the session report.
struct Data {
    // status: 0 off, 1 running, 3 not this build.
    uint32_t magic = 0x53545244, version = 1, bytes = sizeof(Data), status{};
    int64_t sequence{};
    // The game's reads of the throw's slot while a hand aimed it; of those, at
    // an enemy and at a way; hands' aims at a character no enemy stood at.
    uint64_t reads{}, atEnemy{}, atWay{}, noEnemy{};
    // The enemy aimed at last (his handle, 0 none), the way, and the error a
    // start met.
    uint32_t enemy{}, error{};
    Vec3 direction{};
    float pad{};
};
static_assert(sizeof(Data) == 80);
// How long a hand's aim holds without a new one.
constexpr uint32_t leaseMs = 250;
// Checks this game build's getters (9801 another build) and hooks them. The
// swing starts it with the punches.
uint32_t start(uintptr_t base);
void stop();
// A hand's aim (any thread): at a character, the enemy nearest its centre
// within 2 m is looked up, at most every 0.2 s.
void aim(const throw_aim::Request&);
Data data();
} // namespace spidy::game_throw

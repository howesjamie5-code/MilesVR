#pragma once
// Miles's venom meter (venom_punch.hpp), read from the VR worker: the
// player's Hero::HeroMeleeManager is found in the game's component registry
// by its vtable and its actor record, and its meter read once a frame.
//
// The game can hold more than one for the same player: after a challenge
// restart (0.1.15) the one found read 0 while the game's meter showed full,
// and a blow's charge written to it was followed by a crash in the game.
// So every candidate is kept, the registry searched again every few seconds,
// the meter read from the one that reads highest, and nothing written to it
// (address 0) until it is the only one or its meter has moved by itself -
// the game's own.
#include <cstdint>

namespace spidy::game_venom {
struct Meter {
    uint64_t address{}; // the meter's, to write to (0: not found, or not yet trusted)
    float value = -1;   // 0..venom_punch::fullMeter; -1: unknown
    // The player's HeroMeleeManagers found, and the highest meter of the
    // others (-1: none).
    uint32_t candidates{};
    float other = -1;
};
// Once a frame: `record` is the player's actor record (0 without one).
Meter sample(uintptr_t base, uintptr_t record);
void reset();
} // namespace spidy::game_venom

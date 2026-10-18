#pragma once
// Miles's venom meter (venom_punch.hpp), read from the VR worker: the
// player's Hero::HeroMeleeManager is found in the game's component registry
// by its vtable and its actor record, and its meter read once a frame.
#include <cstdint>

namespace spidy::game_venom {
struct Meter {
    uint64_t address{}; // the meter's (0: not found)
    float value = -1;   // 0..venom_punch::fullMeter; -1: unknown
};
// Once a frame: `record` is the player's actor record (0 without one). A
// lost component is looked for again once a second at most.
Meter sample(uintptr_t base, uintptr_t record);
void reset();
} // namespace spidy::game_venom

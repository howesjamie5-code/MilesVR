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
//
// The meter is no field of the HeroMeleeManager's: that is 0x148 bytes, and
// +meterOffset lies in the component the game made after it. Mostly the one
// that holds the meter; in 0.1.16 a venom punch now and then left the HUD's
// bar full while the meter read empty (the user, October 19: "sometimes its
// broken but sometimes it isnt"). So the component found there is taken for
// the meter's own class once, and from then on the meter is read from the
// player's component of that class wherever the game put it, and written
// only after a check that it still is one.
#include <cstdint>

namespace spidy::game_venom {
struct Meter {
    uint64_t address{}; // the meter's, to write to (0: not found, or not yet trusted)
    float value = -1;   // 0..venom_punch::fullMeter; -1: unknown
    // The player's HeroMeleeManagers found, and the highest meter of the
    // others (-1: none).
    uint32_t candidates{};
    float other = -1;
    // The component the meter is written in and its vtable, for the check
    // before each write (the writer reads it there again).
    uint64_t holder{}, holderClass{};
    // The class that holds the meter (its vtable as an offset in the game's
    // image, 0 until known) and the meter's offset in it; and where this
    // frame's meter came from: 0 nowhere, 1 the HeroMeleeManager's neighbour,
    // 2 the holder found by its class.
    uint32_t holderVtable{}, holderOffset{}, route{};
    // The holder's 16 words around the meter, from aroundBefore bytes before
    // it (the meter is the eighth): how the game keeps the two bars Miles
    // gets later in the story (a meter up to 300, or a count of bars).
    static constexpr uintptr_t aroundBefore = 0x1c;
    uint32_t around[16]{};
};
// Once a frame: `record` is the player's actor record (0 without one).
Meter sample(uintptr_t base, uintptr_t record);
// The meter emptied, if its holder is still there (as the punch module's
// writes check it): the venom let go. False when nothing was written.
bool empty(const Meter&);
void reset();
} // namespace spidy::game_venom

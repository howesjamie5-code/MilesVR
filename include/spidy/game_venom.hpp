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

// MilesVR 0.1.78: which venom moves the game itself would allow now (the user: "all the venom moves get
// enabled at their specific story moments"), read as its own checks do, after sample() this frame:
// - the hero's venom object (a component of his, class 3e15b48) points (+0x68) at his abilities object,
//   whose "disabled" query (slot 0x70, 2398000) is a bit's mask against +0xe8, 16 timed masks at
//   +0x48 + 8k while their float at +0x4c + 8k is above zero, and 32 signed counts at +0xc8 - how a
//   mission shuts venom off for a while (bits: venomPowersBit, venomJumpBit, venomSmashBit);
// - Venom Jump's own story unlock (CanVenomJump b67a10): the game flag 0x46e2a4c5 (ed2db0 on the global
//   at 7a5c3c0: a hash table at +0x1180), off on an early save and on once Miles learns it.
// - Venom Smash's Skills-tab unlock (the user: only "when it requires you to go into the skills tab and
//   manually unlock Venom Smash" at the end of Mission 1): the game flag 0x961f104a, the one flag that
//   flipped as he unlocked it there (October 20). The game's own CanVenomSmash (b67bb0) does not ask it.
// The Mega Venom Blast needs three full bars, which the story gives one at a time (one bar on an early save).
constexpr uint32_t venomPowersBit = 0x08000000, venomJumpBit = 0x10000000, venomSmashBit = 0x20000000;
struct Unlocks {
    bool known{};       // the abilities object was found and read
    uint32_t disabled{}; // its disabled bits now
    int jump = -1;       // Venom Jump's story flag: -1 not found, 0 off, 1 on
    int smash = -1;      // Venom Smash's Skills-tab flag, the same way
    int dash = -1;       // Venom Dash's (CanVenomDash b67ae0: flag 0xaeb527e3), given in "Underground Undercover"
};
Unlocks unlocks(uintptr_t base);
} // namespace spidy::game_venom

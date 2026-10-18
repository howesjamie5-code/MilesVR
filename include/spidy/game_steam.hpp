#pragma once
#include <cstdint>

// The game's Steam ticket, guarded.
//
// On its way to the main menu the game asks Steam for an auth session ticket
// (2aa62c0: ISteamUser::GetAuthSessionTicket, slot 13 of the interface its
// context at 5d69010 holds, with a buffer of 0x400 bytes at +0xb4 of its
// online object). Steam's function takes a fourth argument, the identity the
// ticket is for (a SteamNetworkingIdentity, 0x88 bytes, or null), and sends
// it along. The game passes three. The fourth is whatever its stack holds at
// [rsp+20h]: at the game's start a zero the function itself put there on the
// way (it makes its worker thread first, 2aa63c5); on a return to the main
// menu, with the thread there already, what an earlier call left.
//
// In a VR session that was 0x7ff600000000, the top half of an address in the
// game's image over a zeroed low half, and Steam's copy of 0x88 bytes from
// it closed the game: every session that quit to the main menu from the
// evening of October 11 on (twelve in the play folder's reports, the same
// address in steamclient64.dll each time, in CUtlBuffer::Put under the
// ticket's IPC call, the game's return address 2aa6400 above it), none
// before (ten quits), and none in the game without the VR worker, with
// Spidy's controller, its eye views or its swing running (October 12).
// What the stack holds there depends on everything the main thread did
// before; Spidy's hooks on it change that, the game's own bug is what makes
// it matter.
//
// The guard: Steam's function is hooked, and a call from that place in the
// game gets a null identity, as at the game's start.
namespace spidy::game_steam {
// Hooks Steam's ticket function. 0 when hooked; 9801 while the game holds no
// Steam user interface yet (try again); 9802-9805 when what is there is not
// what the supported game and its Steam have (the call in the game, the
// interface, its table, the function); 9810-9899 when hooking failed.
uint32_t install(uintptr_t base);
struct Telemetry {
    uint32_t installed{}, status{};
    // Tickets asked for; those from the game's call on its way to the main
    // menu; of those, the ones that came with something for an identity; and
    // the latest such something.
    uint64_t calls{}, fromGame{}, cleared{}, lastIdentity{};
};
Telemetry telemetry();
} // namespace spidy::game_steam

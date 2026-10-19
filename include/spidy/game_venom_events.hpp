#pragma once
#include <cstdint>

// The game's venom bar on the HUD, watched (MilesVR 0.1.17).
//
// The bar does not follow the meter (venom_punch.hpp) by itself. The game
// tells it what happened with three UI events: UIVenomBarFillEvent,
// UIVenomBarMaxFillEvent and UIVenomBarUsedEvent (vtables 4246c98, 4246d30,
// 4246dc8 in MilesMorales.exe 4.630.0.0). The venom punch writes the meter
// and sends none, so in 0.1.16 the bar went up with the blows and stayed full
// after the punch had emptied it (the user, October 19: "the meter doesnt
// deplete physically but acts in game as it should").
//
// Nothing in the game's code calls the events' constructors directly: they
// are made through the event type table (692b1c8 holds UIVenomBarUsedEvent's).
// So each constructor (two per event, the type table's and another) is hooked
// to count the events, the meter when each came, and the call stacks the game
// made them from, for the session report: the next build sends the game's own
// "venom used" from there.
namespace spidy::game_venom_events {
enum Event : unsigned { fill, maxFill, used, count };
// Hooks the constructors. 0 when hooked; 9701 when the code there is not the
// supported game's; 9710-9799 when hooking failed.
uint32_t install(uintptr_t base);
// The meter the VR worker read this frame (-1: unknown), stored with each event.
void meter(float value);
} // namespace spidy::game_venom_events

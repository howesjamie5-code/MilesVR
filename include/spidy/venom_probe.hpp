#pragma once
// MilesVR research: a recording of the hero's venom state, to find Miles Morales's venom meter.
//
// Every few seconds it lists the components the game's registry holds for the player's actor (their
// MSVC class names, read from the image's RTTI), and ten times a second it snapshots the ones whose
// names say venom, focus, ability, energy or power - and Hero::HeroVenomModeManager wherever it is -
// writing a line whenever one changed: reports\venom-probe-<time>.csv beside the session's report.
// It only reads the game's memory. Off unless started.
#include <cstdint>

namespace spidy::venom_probe {
// Once a frame from the VR worker: `record` is the player's actor record (0 without one).
void sample(uintptr_t base, uintptr_t record);
// Closes the file (VR's end).
void stop();
} // namespace spidy::venom_probe

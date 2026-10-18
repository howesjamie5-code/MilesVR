#pragma once
#include <cstdint>
#include <vector>

// The sounds of a fight: Spidy's blows go straight to the game's damage
// system, past the attack animations the game hangs its own sounds on, so a
// fist met a thug in silence (the user, October 11: "punches have zero
// feedback now and feel bad"). Each is made here from a falling low tone
// (the body of the blow) under a short burst of noise (the smack), as drum
// machines make a kick and a snare; a strike's rush is noise swept through a
// band. Nothing is loaded from a file.
namespace spidy {
enum class ImpactSound : std::uint8_t {
    Light,    // a jab
    Firm,     // a punch that knocks him back or down
    Heavy,    // one that sends him flying, lifts him or slams him
    Finisher, // the flow's
    Rush,     // a web strike's flight
};
// The sound as a WAV file in memory: 16-bit mono at `rate` samples a second.
std::vector<std::uint8_t> impactWave(ImpactSound sound, unsigned rate = 22050);
// Its length, seconds.
float impactSeconds(ImpactSound sound);
} // namespace spidy

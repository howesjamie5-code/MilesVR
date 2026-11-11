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
    // The Mega Venom Blast (MilesVR 0.1.58): a rising rush of charge, then a
    // deep boom with an electric crackle over a long rumble.
    Mega,
    // A blow a guard stopped (MilesVR 0.1.80, v18; v23: one for each kind of enemy): not made here but the user's
    // own sounds (block_sound.hpp), at the rate asked for - a baton's (the ordinary melee-weapon guards), a
    // brawler's (the Underground's weapon breakers), a slayer's (the heavies) and a shield's.
    BlockBaton,
    BlockBrawler,
    BlockSlayer,
    BlockShield,
    // A web pull a guard refused (MilesVR 0.1.80, v26): the long web lands on an enemy whose guard stands and does not
    // take him. The user's own again (web_block_sound.hpp), one for each kind of guard in the order of the four above,
    // at the loudness of those.
    WebBlockBaton,
    WebBlockBrawler,
    WebBlockSlayer,
    WebBlockShield,
};
// One of the eight recorded sounds of a guard: the four of a blocked blow (BlockBaton .. BlockShield) and, since v26, the
// four of a refused web pull (WebBlockBaton .. WebBlockShield). The game plays all eight through the voice pool, and
// they come out of impactWave at blockWaveRate as they were recorded.
inline bool isBlockSound(ImpactSound sound) {
    return sound >= ImpactSound::BlockBaton && sound <= ImpactSound::WebBlockShield;
}
// One of the four sounds of a refused web pull.
inline bool isWebBlockSound(ImpactSound sound) {
    return sound >= ImpactSound::WebBlockBaton && sound <= ImpactSound::WebBlockShield;
}
// The rate the recorded sounds are at (block_sound.hpp's blockSoundRate and web_block_sound.hpp's webBlockSoundRate, the
// same): ask impactWave for it and they come out as they are, at any other it resamples them.
inline constexpr unsigned blockWaveRate = 44100;
// The sound as a WAV file in memory: 16-bit mono at `rate` samples a second.
std::vector<std::uint8_t> impactWave(ImpactSound sound, unsigned rate = 22050);
// Its length, seconds.
float impactSeconds(ImpactSound sound);
} // namespace spidy

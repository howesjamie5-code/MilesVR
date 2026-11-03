#pragma once
#include <cstdint>

// The game's own sounds for Spidy's blows. Players asked on October 13 and
// 14 ("pls an option to remove the sfx when punching", 12 reactions; "add
// multiple punch sounds"; "less like popping balloons"): the headset played
// four tones of its own for every blow (impact_sound.hpp), the same four
// every time, outside the game's mix and its volume.
//
// Where the game's own blows get theirs (read from Spider-Man.exe, October
// 22): the attacker's AnimImpactEventManager (vtable 3882bd0, component type
// 6d163a0) plays each impact of an attack (69c730), and for a melee one 69ceb0
// sounds it on the one struck: on each sound source of his SoundSourceComponent
// (component type 7a51f80: their count at +0x154, each one's object id at
// +0x48 + 12 i) it sets the five switches of its "Melee Impact Event"
// (196b920: object, group, value and their names; the ids are FNV-1 of the
// lowercased names, as 1bb8a10 makes them) -
//   Enemy_Impact_Location  kImpact_Head, kImpact_Torso, kImpact_Leg
//   Enemy_Impact_Intensity kImpact_None, kImpact_Light, kImpact_Medium,
//                          kImpact_Heavy, kImpact_Extreme
//   Hero_Combo_Depth       Combo_0 ... Combo_5
//   Hero_Elevation         Ground, Air
//   Hero_Attack_Type       BasicMelee, WebStrike
// - and plays one of three hit sounds on him through the game's sound call
// 16d5e80 (the system at 7475690, the hash, his handle and a pointer to it,
// then 1, 0, 0, 0, 1, 0.f, 0.f, 0, 0), chosen by the impact's data (+0x3c):
// 0x7b71909e, 0x9387aab9 or 0xe21849db; one struck who is not the player
// (1f16a40) has the sound's +0x138 set to 1, its flag 0x100 at +0x8 and +0x18
// marking that changed. The impact's data comes from the attack's animation:
// Spidy's blows are plain requests to the DamageSystem (native_bodies::damage)
// with no animation behind them, so they fell silent there and the headset
// made its own.
//
// Now each blow of Spidy's on an enemy sounds as the game's own melee does,
// its switches set from the blow: where on him it landed, how hard (a jab
// Light, a firm blow Medium, a heavy one Heavy, a finisher Extreme), the
// flow's count, the player in the air or not, a fist or a web strike; on his
// sound sources, or the player's when he has none (he is at arm's length).
// The hit sound is the first of the three, the impact's plain case. blow()
// from the blow's thread, played on the game's main thread (frame(), from the
// shooter's camera hook, as game_focus is given). While the game plays them
// (Data::live) the headset plays none of its own; until then, or once the
// game refuses one, it does.
namespace spidy::game_sound {
// A blow, in the switches of the game's Melee Impact Event.
struct Blow {
    uint8_t intensity = 1; // 1 kImpact_Light, 2 Medium, 3 Heavy, 4 Extreme
    uint8_t location = 1;  // 0 kImpact_Head, 1 Torso, 2 Leg
    uint8_t combo{};       // Combo_0 ... Combo_5
    uint8_t air{};         // 0 Ground, 1 Air
    uint8_t webStrike{};   // 0 BasicMelee, 1 WebStrike
};
// SpidySoundData, for the headset and the session report.
struct Data {
    uint32_t magic = 0x53534e44, version = 1, bytes = sizeof(Data), status{}; // 0 off, 2 running, 3 another build
    int64_t sequence{};
    // Blows that asked for a sound; those the game played; those it gave no
    // sound back for; those dropped before it (a full queue, a victim gone,
    // a fault).
    uint64_t asked{}, played{}, refused{}, dropped{};
    // Blows sounded on the player's own sound sources, the one struck having
    // no SoundSourceComponent; and the sound sources the latest blow's
    // switches were set on.
    uint32_t onPlayer{}, sources{};
    // 1 while the game's own sounds are Spidy's blows' (the latest one asked
    // for was played): the headset then plays none of its own.
    uint32_t live{};
    // The switches the latest blow set (five on each sound source, when each
    // took).
    uint32_t switches{};
};
static_assert(sizeof(Data) == 72);
// The game's id for a name of its sound engine's: FNV-1 of the lowercased
// name (1bb8a10).
inline uint32_t audioId(const char* name) {
    uint32_t id = 0x811c9dc5;
    for (; name && *name; ++name) {
        const char c = *name >= 'A' && *name <= 'Z' ? static_cast<char>(*name - 'A' + 'a') : *name;
        id = id * 0x1000193 ^ static_cast<uint8_t>(c);
    }
    return id;
}
// Where a blow lands on him, `above` his feet of his `height`: the head above
// 80% of it, a leg below 35%, else the torso (Blow::location).
inline uint8_t locationOf(float above, float height) {
    if (!(height > 0) || !(above == above))
        return 1;
    const float at = above / height;
    return at > .8f ? 0 : at < .35f ? 2 : 1;
}
// Checks this game build (0, or 9801 another build) and keeps the image base.
// The swing starts it with the punches.
uint32_t start(uintptr_t base);
void stop();
// Whether the game's own impacts sound the blows (the tab's BLOW SOUNDS on
// GAME); off, blow() asks for none (SPIDY: the headset's tones; OFF: none).
// SpidySoundMode from the headset's module.
void sounding(bool on);
// The player (`damager`, his actor record) landed `blow` on `victim` (an actor
// record). Any thread; the game sounds it in the next frame().
void blow(uint64_t damager, uint64_t victim, Blow blow);
// The game's main thread: the blows since the last frame, sounded.
void frame();
Data data();
} // namespace spidy::game_sound

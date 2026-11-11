#pragma once
#include <cstddef>
#include <cstdint>

// MilesVR 0.1.80, v16: a recorder of the game's own hits, for the combat rules that follow it. The user
// (October 30): the VR punches should be "normal attacks" so that the game's own AI blocks and staggers as it
// does for a controller's punch. What a brute's block answers to (a hit's hash, category, hit flags and
// knockback) is not known, so this records what the game itself sends: every damage request the game hands
// out (native_bodies' directDamageRva, the getter every request comes from), read a few milliseconds later,
// after its caller filled the fields in. It logs
//
//   - the hero's hits (the request's damager is the hero): victim, hash, category, type, amount, knockback,
//     knockback amount, hit flags, impulse, and which fields were set;
//   - the hits on the hero (an enemy's baton, a brute's fist): the attacker's classes and the same values;
//   - once for each enemy hit, the classes of all its components (its catalogue: a brute, a shield, a baton);
//   - a quarter of a second after a hit of the hero's, the victim's hit points and its states, so a blocked
//     blow (no hit points lost, a block state) tells from a landed one;
//   - the mod's own requests (the VR punch), the first few, to compare their values with the game's;
//   - every ten seconds, how many requests were seen (a count of none means the game's punches do not come
//     through this getter).
//
// The lines go to the session log through the same note ring the first-person worker uses (game_xr drains it).
namespace spidy::game_record {
// SpidyRecordNote's request: in, the reader's count; out, the line.
struct Note {
    uint32_t seen{};
    char text[200]{};
};
// Checks this game build's getter and hooks it. 0 when running.
uint32_t start(uintptr_t base);
void stop();
// The player's actor record (any thread): whose hits are the hero's.
void hero(uint64_t record);
// The calling thread is issuing a request of the mod's own (native_bodies::damageNow): the hook takes it for
// the mod's, not the game's.
void issuing(bool on);
// One line of the recorder's notes: false when there is none new. `seen` is the reader's own count.
bool note(uint32_t& seen, char* out, size_t size);
// MilesVR 0.1.80, v18: a line into the same notes from any thread, whether the recorder is running or not
// (game_guard writes its census and its breaks there; game_xr drains them into the session log).
void log(const char* text);
} // namespace spidy::game_record

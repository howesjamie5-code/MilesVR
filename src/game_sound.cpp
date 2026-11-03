// The game's own sounds for Spidy's blows (game_sound.hpp). Addresses are
// image offsets in the supported Spider-Man.exe.
#include "spidy/game_sound.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <windows.h>
using namespace spidy;
using namespace spidy::game_sound;
extern "C" {
__declspec(dllexport) Data SpidySoundData;
}

namespace {
// The sound system and its call (system, hash, emitter handle, a pointer to
// it, then the nine the melee impact sets); a switch on a sound source (its
// object id, the group's id and the value's, then their names and the
// event's, which its errors name); whether one struck is spared the mark the
// game gives a sound on anyone else (1f16a40, on his handle).
constexpr uintptr_t systemRva = 0x7e294f0, playRva = 0x1be0fb0, switchRva = 0x1e770e0, spareRva = 0x2425080;
constexpr uint8_t playEntry[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
                                 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr uint8_t switchEntry[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x41, 0x56,
                                   0x41, 0x57, 0x48, 0x83, 0xec, 0x40, 0x8b, 0xd9};
constexpr uint8_t spareEntry[] = {0x89, 0x4c, 0x24, 0x08, 0x53, 0x48, 0x83, 0xec,
                                  0x20, 0x48, 0x8d, 0x4c, 0x24, 0x30, 0xe8, 0x8d};
// The melee impact's three hit sounds (69ceb0, by its data's +0x3c), and the
// one Spidy's blows take: the first, the impact's plain case (data +0x3c 0;
// the second and the third take arguments of their own, the third the
// attacker's component too). Which one the game's own punches name was not
// read: the hero has impact entries only while the flat game's attack runs.
// A recording of the PC's output on October 22 heard none of the three from
// the plaza's noise for certain; the first rose a little the most
// (tools/probe_sound.py, trial by trial).
constexpr uint32_t hitSounds[3] = {0x7b71909e, 0x9387aab9, 0xe21849db};
constexpr unsigned spidyHit = 0;
// The sound the game gives back: its +0x138 marks one struck who is not the
// player, and the flag 0x100 at +0x8 and +0x18 says it changed.
constexpr uintptr_t struckAt = 0x138, changedAt[2] = {0x8, 0x18};
constexpr uint64_t changed = 0x100;
// The one struck's SoundSourceComponent: its sound sources' count, and each
// one's object id.
constexpr uintptr_t sourcesType = 0x8407c20, sourceCountAt = 0x154, sourcesAt = 0x48, sourceBytes = 12;
constexpr uint32_t mostSources = 8;
// The Melee Impact Event's switch groups and their values (the game's own
// tables at 4f90aa0, 4f90af0, 393a430, 393a390 and 393a3e0).
constexpr const char* eventName = "Melee Impact Event";
constexpr const char* groupNames[5] = {"Enemy_Impact_Location", "Enemy_Impact_Intensity", "Hero_Combo_Depth",
                                       "Hero_Elevation", "Hero_Attack_Type"};
constexpr const char* locations[3] = {"kImpact_Head", "kImpact_Torso", "kImpact_Leg"};
constexpr const char* intensities[5] = {"kImpact_None", "kImpact_Light", "kImpact_Medium", "kImpact_Heavy",
                                        "kImpact_Extreme"};
constexpr const char* combos[6] = {"Combo_0", "Combo_1", "Combo_2", "Combo_3", "Combo_4", "Combo_5"};
constexpr const char* elevations[2] = {"Ground", "Air"};
constexpr const char* attackTypes[2] = {"BasicMelee", "WebStrike"};
// An actor's component by its type, as game_focus finds one.
constexpr uintptr_t findInTableRva = 0x1b844b0, findInListRva = 0x1b85650;
// AnimImpactEventManager, for the probe: its type, vtable, impact entries
// (0x150 bytes each from +0x50, their count at +0x5a4) and each one's data
// (d5e320): its hit sound +0x3c, location +0x40, intensity +0x44.
constexpr uintptr_t managerType = 0x777add0, managerVtable = 0x3dd98f8, entryDataRva = 0xf90e10;
constexpr uintptr_t entriesAt = 0x50, entryBytes = 0x150, entryCountAt = 0x5a4;
using FindComponent = uintptr_t (*)(void*, const void*);
using Play = uintptr_t (*)(void*, uint32_t, uint32_t, const uint32_t*, int32_t, uint8_t, uint64_t, uint32_t, uint8_t,
                           float, float, uint64_t, uint64_t);
using SetSwitch = bool (*)(uint32_t, uint32_t, uint32_t, const char*, const char*, const char*);
using Spared = bool (*)(uint32_t);
using EntryData = uintptr_t (*)(const void*);

uintptr_t base{};
std::atomic<bool> enabled{}, wanted{true};
// The queue and SpidySoundData, both under this lock.
SRWLOCK lock = SRWLOCK_INIT;
struct Pending {
    uint64_t damager{}, victim{};
    Blow blow{};
};
constexpr unsigned queueSize = 32;
Pending queue[queueSize];
unsigned queued{};

template <class F> void publish(F change) {
    InterlockedIncrement64(&SpidySoundData.sequence);
    change(SpidySoundData);
    InterlockedIncrement64(&SpidySoundData.sequence);
}
bool read(uintptr_t p, void* out, size_t n) {
    __try {
        if (p < 0x10000)
            return false;
        std::memcpy(out, reinterpret_cast<const void*>(p), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uintptr_t pointer(uintptr_t p) {
    uintptr_t v{};
    read(p, &v, sizeof(v));
    return v;
}
template <class T> T value(uintptr_t p) {
    T v{};
    read(p, &v, sizeof(v));
    return v;
}
bool sameBytes(uintptr_t at, const uint8_t* expected, size_t n) {
    uint8_t got[16]{};
    return n <= sizeof(got) && read(at, got, n) && std::memcmp(got, expected, n) == 0;
}
// An actor's handle, as the game's own sound calls make it (1676900):
// generation << 20 | index. 0 for none.
uint32_t handleOf(uint64_t record) {
    uint16_t generation{};
    uint32_t index{};
    if (!record || !read(record + 0x10, &generation, sizeof(generation)) || !read(record + 0xc, &index, sizeof(index)))
        return 0;
    index &= 0xfffff;
    return index ? (static_cast<uint32_t>(generation & 0x7ff) << 20) | index : 0;
}
// The actor's component of a type; 0 for none.
uintptr_t componentOf(uint64_t record, uintptr_t type) {
    if (!record)
        return 0;
    const bool hashed = value<uint16_t>(record + 0x88) != 0;
    uintptr_t found{};
    __try {
        found = reinterpret_cast<FindComponent>(base + (hashed ? findInTableRva : findInListRva))(
            reinterpret_cast<void*>(record + (hashed ? 0x80 : 0x58)), reinterpret_cast<void*>(base + type));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return found && pointer(found + 8) == record ? found : 0;
}
// The Melee Impact Event's switches for a blow, on every sound source of the
// one struck: the switches that took.
uint32_t setSwitches(uintptr_t sources, const Blow& b) {
    const char* values[5] = {locations[std::min<uint8_t>(b.location, 2)],
                             intensities[std::clamp<uint8_t>(b.intensity, 1, 4)], combos[std::min<uint8_t>(b.combo, 5)],
                             elevations[b.air ? 1 : 0], attackTypes[b.webStrike ? 1 : 0]};
    uint32_t took{};
    const uint32_t count = std::min(value<uint32_t>(sources + sourceCountAt), mostSources);
    for (uint32_t i = 0; i < count; ++i) {
        const auto object = value<uint32_t>(sources + sourcesAt + sourceBytes * i);
        for (unsigned g = 0; g < 5; ++g) {
            __try {
                took += reinterpret_cast<SetSwitch>(base + switchRva)(object, audioId(groupNames[g]),
                                                                       audioId(values[g]), groupNames[g], values[g],
                                                                       eventName);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return took;
            }
        }
    }
    return took;
}
// The game's sound call on the one of `handle`, marked as the melee impact
// marks one struck who is not the player.
uintptr_t play(uint32_t hash, uint32_t handle, bool mark) {
    uintptr_t sound{};
    __try {
        uint32_t emitter = handle;
        sound = reinterpret_cast<Play>(base + playRva)(reinterpret_cast<void*>(base + systemRva), hash, handle, &emitter,
                                                        1, 0, 0, 0, 1, 0.f, 0.f, 0, 0);
        if (sound && mark && !reinterpret_cast<Spared>(base + spareRva)(handle)) {
            *reinterpret_cast<uint8_t*>(sound + struckAt) = 1;
            for (const auto at : changedAt)
                *reinterpret_cast<uint64_t*>(sound + at) |= changed;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return sound;
}
// A blow sounded on the one struck: 1 played, 0 the game gave no sound back,
// -2 nobody to sound it on (he is gone, and the player has no sound source
// either). One with no SoundSourceComponent of his own (thugs of a crime not
// begun yet, 80 m off, had none on October 22) has it sounded on the player's
// own sound sources instead, at arm's length from him (`onPlayer`).
int sound(uint64_t damager, uint64_t victim, const Blow& b, uint32_t& sources, uint32_t& switches, bool& onPlayer,
          unsigned hit = spidyHit) {
    uint64_t emitter = victim;
    uint32_t handle = handleOf(victim);
    uintptr_t component = handle ? componentOf(victim, sourcesType) : 0;
    onPlayer = false;
    if (!component && damager && damager != victim) {
        emitter = damager;
        handle = handleOf(damager);
        component = handle ? componentOf(damager, sourcesType) : 0;
        onPlayer = component != 0;
    }
    if (!handle || !component)
        return -2;
    sources = std::min(value<uint32_t>(component + sourceCountAt), mostSources);
    switches = setSwitches(component, b);
    return play(hitSounds[hit % 3], handle, emitter == victim) ? 1 : 0;
}
// A probe's request (SpidySoundTest), answered on the main thread.
struct Test {
    uint32_t magic = 0x53534e54, version = 2, bytes = sizeof(Test), play{}; // 0 read only, 1 a blow, 2 `hash`
    uint64_t hero{}, emitter{}; // whose impact manager is read; the actor a sound plays on
    uint32_t hash{}, hit{};     // `hash` with play 2; with play 1, which of the three hit sounds
    Blow blow{};                // with play 1
    uint8_t padding[3]{};
    // Answers.
    uint32_t done{}, manager{}, entries{}, sources{}, switches{}, result{};
    uint32_t entryData[8]{}, entryHit[8]{}, entryLocation[8]{}, entryIntensity[8]{};
    uint64_t sound{};
    uint32_t onPlayer{}, reserved{};
};
static_assert(sizeof(Test) == 216);
std::atomic<Test*> testing{};
void answer(Test& t) {
    uintptr_t manager = componentOf(t.hero, managerType);
    if (manager && pointer(manager) != base + managerVtable)
        manager = 0;
    t.manager = manager != 0;
    t.entries = manager ? std::min(value<uint32_t>(manager + entryCountAt), 8u) : 0;
    for (uint32_t i = 0; i < t.entries; ++i) {
        uintptr_t data{};
        __try {
            data = reinterpret_cast<EntryData>(base + entryDataRva)(
                reinterpret_cast<const void*>(manager + entriesAt + entryBytes * i));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            data = 0;
        }
        t.entryData[i] = data != 0;
        t.entryHit[i] = data ? value<uint32_t>(data + 0x3c) : 0;
        t.entryLocation[i] = data ? value<uint32_t>(data + 0x40) : 0;
        t.entryIntensity[i] = data ? value<uint32_t>(data + 0x44) : 0;
    }
    if (t.play == 1) {
        bool onPlayer{};
        t.result = static_cast<uint32_t>(sound(t.hero, t.emitter, t.blow, t.sources, t.switches, onPlayer, t.hit));
        t.onPlayer = onPlayer;
    } else if (t.play == 2) {
        t.sound = play(t.hash, handleOf(t.emitter), false);
        t.result = t.sound != 0;
    }
    t.done = 1;
}
} // namespace

uint32_t game_sound::start(uintptr_t gameBase) {
    if (enabled.load())
        return 0;
    const bool known = sameBytes(gameBase + playRva, playEntry, sizeof(playEntry)) &&
                       sameBytes(gameBase + switchRva, switchEntry, sizeof(switchEntry)) &&
                       sameBytes(gameBase + spareRva, spareEntry, sizeof(spareEntry));
    AcquireSRWLockExclusive(&lock);
    queued = 0;
    publish([&](Data& d) {
        d.status = known ? 2 : 3;
        d.live = known ? 1u : 0u;
    });
    if (known) {
        base = gameBase;
        enabled = true;
    }
    ReleaseSRWLockExclusive(&lock);
    return known ? 0 : 9801;
}
void game_sound::stop() {
    AcquireSRWLockExclusive(&lock);
    enabled = false;
    publish([&](Data& d) {
        d.dropped += queued;
        d.status = 0;
        d.live = 0;
    });
    queued = 0;
    ReleaseSRWLockExclusive(&lock);
}
void game_sound::sounding(bool on) {
    wanted = on;
}
void game_sound::blow(uint64_t damager, uint64_t victim, Blow b) {
    if (!wanted.load(std::memory_order_relaxed))
        return;
    AcquireSRWLockExclusive(&lock);
    const bool room = enabled.load(std::memory_order_relaxed) && damager && victim && queued < queueSize;
    if (room)
        queue[queued++] = {damager, victim, b};
    publish([&](Data& d) {
        ++d.asked;
        d.dropped += !room;
    });
    ReleaseSRWLockExclusive(&lock);
}
void game_sound::frame() {
    if (!enabled.load(std::memory_order_relaxed))
        return;
    if (Test* t = testing.exchange(nullptr))
        answer(*t);
    Pending batch[queueSize];
    AcquireSRWLockExclusive(&lock);
    const unsigned count = queued;
    std::copy(queue, queue + count, batch);
    queued = 0;
    ReleaseSRWLockExclusive(&lock);
    if (!count)
        return;
    uint64_t played{}, refused{}, dropped{};
    uint32_t onPlayer{}, sources{}, switches{};
    int latest{};
    for (unsigned i = 0; i < count; ++i) {
        bool mine{};
        latest = sound(batch[i].damager, batch[i].victim, batch[i].blow, sources, switches, mine);
        played += latest == 1;
        refused += latest == 0;
        dropped += latest == -2;
        onPlayer += mine;
    }
    AcquireSRWLockExclusive(&lock);
    publish([&](Data& d) {
        d.played += played;
        d.refused += refused;
        d.dropped += dropped;
        d.onPlayer += onPlayer;
        d.sources = sources;
        d.switches = switches;
        // Live while the game sounds them: a refusal, or nobody to sound one
        // on, hands the blows back to the headset until the game plays one
        // again.
        d.live = latest == 1 ? 1u : 0u;
    });
    ReleaseSRWLockExclusive(&lock);
}
Data game_sound::data() {
    AcquireSRWLockShared(&lock);
    const Data out = SpidySoundData;
    ReleaseSRWLockShared(&lock);
    return out;
}

// The tab's BLOW SOUNDS from the headset's module: 0 the game's own, any
// other the headset's or none (game_sound::sounding).
extern "C" __declspec(dllexport) DWORD WINAPI SpidySoundMode(void* mode) {
    game_sound::sounding(reinterpret_cast<uintptr_t>(mode) == 0);
    return 0;
}
// A probe's look at the hero's impact manager (each impact entry's hit sound,
// location and intensity), and a blow sounded on an actor or a sound played
// on him with no enemy about, on the main thread's next frame (the shooter's
// frame hook must be running): Test above, answered in place. 0 once
// answered; 9802 a bad request, 9803 no main-thread frame within a second,
// 9801 another build.
extern "C" __declspec(dllexport) DWORD WINAPI SpidySoundTest(void* inout) {
    Test t{};
    if (!read(reinterpret_cast<uintptr_t>(inout), &t, sizeof(t)) || t.magic != 0x53534e54 || t.version != 2 ||
        t.bytes != sizeof(t) || t.play > 2 || (t.play && !t.emitter) || (t.play == 2 && !t.hash))
        return 9802;
    if (const auto code = game_sound::start(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))))
        return code;
    t.done = 0;
    Test* expected = nullptr;
    if (!testing.compare_exchange_strong(expected, &t))
        return 9803;
    const auto limit = GetTickCount64() + 1000;
    while (!t.done && GetTickCount64() < limit)
        Sleep(5);
    if (!t.done) {
        // Taken back if the main thread never came; else it is answering now.
        Test* mine = &t;
        if (testing.compare_exchange_strong(mine, nullptr))
            return 9803;
        while (!t.done)
            Sleep(1);
    }
    __try {
        std::memcpy(inout, &t, sizeof(t));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9802;
    }
    return 0;
}

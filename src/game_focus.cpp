// The game's Focus for Spidy's blows (game_focus.hpp). Addresses are image
// offsets in the supported Spider-Man.exe.
#include "spidy/game_focus.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <windows.h>
using namespace spidy;
using namespace spidy::game_focus;
extern "C" {
__declspec(dllexport) Data SpidyFocusData;
}

namespace {
// Hero::HeroSpiderAssaultManager: its vtable, whose first slot is its event
// setup (960a60); its component type; the game's Focus for a kind of action
// (961e90: manager, kind, flags); and its meter, segments of {current, most}
// from +0x110, their count at +0x128.
constexpr uintptr_t managerVtable = 0x3e13a48, setupRva = 0xb19630, managerType = 0x7843660, earnRva = 0xb1af00;
constexpr uintptr_t segmentsAt = 0x110, segmentCountAt = 0x128;
constexpr uint32_t mostSegments = 16;
constexpr uint8_t earnEntry[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
                                 0xec, 0x20, 0x41, 0x8b, 0xd8, 0x48, 0x8b, 0xf9};
// An actor's component by its type, from the record's table or its list, as
// game_shooter and native_bodies find one.
constexpr uintptr_t findInTableRva = 0x1b844b0, findInListRva = 0x1b85650;
using FindComponent = uintptr_t (*)(void*, const void*);
using Earn = void (*)(void*, uint32_t, uint32_t);

uintptr_t base{};
std::atomic<bool> enabled{};
// The queue and SpidyFocusData, both under this lock.
SRWLOCK lock = SRWLOCK_INIT;
// The blows waiting for the main thread, in order.
struct Pending {
    uint64_t damager{};
    Kind kind{};
};
constexpr unsigned queueSize = 64;
Pending queue[queueSize];
unsigned queued{};

// Changes SpidyFocusData (the lock held), its sequence odd meanwhile.
template <class F> void publish(F change) {
    InterlockedIncrement64(&SpidyFocusData.sequence);
    change(SpidyFocusData);
    InterlockedIncrement64(&SpidyFocusData.sequence);
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
// The damager's own HeroSpiderAssaultManager, still his; 0 for none (a
// character the game gives no Focus, a record gone).
uintptr_t managerOf(uint64_t record) {
    if (!record)
        return 0;
    const bool hashed = value<uint16_t>(record + 0x88) != 0;
    uintptr_t found{};
    __try {
        found = reinterpret_cast<FindComponent>(base + (hashed ? findInTableRva : findInListRva))(
            reinterpret_cast<void*>(record + (hashed ? 0x80 : 0x58)), reinterpret_cast<void*>(base + managerType));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return found && pointer(found) == base + managerVtable && pointer(found + 8) == record ? found : 0;
}
bool give(uintptr_t manager, Kind kind) {
    __try {
        reinterpret_cast<Earn>(base + earnRva)(reinterpret_cast<void*>(manager), static_cast<uint32_t>(kind), 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The meter: its segments' fill and their most.
void meter(uintptr_t manager, float& now, float& most) {
    now = most = 0;
    const auto count = std::min(value<uint32_t>(manager + segmentCountAt), mostSegments);
    for (uint32_t i = 0; i < count; ++i) {
        float segment[2]{};
        if (read(manager + segmentsAt + 8ull * i, segment, sizeof(segment)) && std::isfinite(segment[0]) &&
            std::isfinite(segment[1])) {
            now += segment[0];
            most += segment[1];
        }
    }
}
} // namespace

uint32_t game_focus::start(uintptr_t gameBase) {
    if (enabled.load())
        return 0;
    uint8_t entry[sizeof(earnEntry)]{};
    const bool known = pointer(gameBase + managerVtable) == gameBase + setupRva &&
                       read(gameBase + earnRva, entry, sizeof(entry)) &&
                       std::memcmp(entry, earnEntry, sizeof(entry)) == 0;
    AcquireSRWLockExclusive(&lock);
    queued = 0;
    publish([&](Data& d) { d.status = known ? 2 : 3; });
    if (known) {
        base = gameBase;
        enabled = true;
    }
    ReleaseSRWLockExclusive(&lock);
    return known ? 0 : 9701;
}
void game_focus::stop() {
    AcquireSRWLockExclusive(&lock);
    enabled = false;
    publish([&](Data& d) {
        d.missed += queued;
        d.status = 0;
    });
    queued = 0;
    ReleaseSRWLockExclusive(&lock);
}
void game_focus::earn(uint64_t damager, Kind kind) {
    AcquireSRWLockExclusive(&lock);
    const bool room = enabled.load(std::memory_order_relaxed) && damager && queued < queueSize;
    if (room)
        queue[queued++] = {damager, kind};
    publish([&](Data& d) {
        ++d.asked;
        d.missed += !room;
    });
    ReleaseSRWLockExclusive(&lock);
}
void game_focus::frame() {
    if (!enabled.load(std::memory_order_relaxed))
        return;
    Pending batch[queueSize];
    AcquireSRWLockExclusive(&lock);
    const unsigned count = queued;
    std::copy(queue, queue + count, batch);
    queued = 0;
    ReleaseSRWLockExclusive(&lock);
    if (!count)
        return;
    uint64_t given{}, missed{}, of{};
    uintptr_t manager{};
    for (unsigned i = 0; i < count; ++i) {
        if (batch[i].damager != of) {
            of = batch[i].damager;
            manager = managerOf(of);
        }
        if (manager && give(manager, batch[i].kind))
            ++given;
        else
            ++missed;
    }
    float now{}, most{};
    if (manager)
        meter(manager, now, most);
    AcquireSRWLockExclusive(&lock);
    publish([&](Data& d) {
        d.given += given;
        d.missed += missed;
        if (manager) {
            d.now = now;
            d.most = most;
        }
    });
    ReleaseSRWLockExclusive(&lock);
}
Data game_focus::data() {
    AcquireSRWLockShared(&lock);
    const Data out = SpidyFocusData;
    ReleaseSRWLockShared(&lock);
    return out;
}

// A probe's Focus for the player with no enemy about: one blow of `kind`
// earned for `damager`, given at the main thread's next frame (the shooter's
// frame hook must be running). Writes SpidyFocusData as it then stands over
// the request (56 bytes). 0 once given; 9702 a bad request, 9703 nothing
// given within a second (no main-thread frame, no meter), 9701 another build.
namespace {
struct Test {
    uint32_t magic = 0x53464354, version = 1, bytes = sizeof(Test), kind{};
    uint64_t damager{};
};
static_assert(sizeof(Test) == 24);
} // namespace
extern "C" __declspec(dllexport) DWORD WINAPI SpidyFocusTest(void* inout) {
    Test t{};
    if (!read(reinterpret_cast<uintptr_t>(inout), &t, sizeof(t)) || t.magic != 0x53464354 || t.version != 1 ||
        t.bytes != sizeof(t) || !t.damager || !t.kind || t.kind > 29)
        return 9702;
    if (const auto code = game_focus::start(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))))
        return code;
    const auto before = game_focus::data();
    game_focus::earn(t.damager, static_cast<Kind>(t.kind));
    const auto limit = GetTickCount64() + 1000;
    Data after = game_focus::data();
    while (after.given + after.missed <= before.given + before.missed && GetTickCount64() < limit) {
        Sleep(5);
        after = game_focus::data();
    }
    __try {
        std::memcpy(inout, &after, sizeof(after));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9702;
    }
    return after.given > before.given ? 0 : 9703;
}

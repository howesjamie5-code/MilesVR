// The game's own wall crawl under the player (game_warp.hpp). Addresses are
// image offsets in the supported Spider-Man.exe.
#include "spidy/game_warp.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <windows.h>
using namespace spidy;
using namespace spidy::game_warp;
extern "C" {
__declspec(dllexport) Data SpidyWarpData;
}
namespace {
// Hero::HeroTransitionManager: its vtable, its way onto a wall (+0x360,
// 980f50) and the hero's state machine it asks (+0x50: a SyncStateMachine).
constexpr uintptr_t managerVtable = 0x38b4350, enterSlot = 0x360, enterRva = 0x980f50;
constexpr uintptr_t machineVtable = 0x4f84148;
constexpr uintptr_t registryTable = 0x7a44320, registryCount = 0x7a44340;
using Enter = bool(__fastcall*)(void* manager, const Vec3* point, const Vec3* normal, const Vec3* hint,
                                uint32_t, float, uint8_t, uint8_t);
uintptr_t base{};
std::atomic<bool> enabled{};
SRWLOCK lock = SRWLOCK_INIT, output = SRWLOCK_INIT;
// The crawl waiting for the main thread, and the manager it goes to.
struct Request {
    bool waiting{};
    Vec3 point{}, normal{};
};
Request pending{};
uintptr_t manager{}, managerRecord{};

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
    return read(p, &v, sizeof(v)) ? v : 0;
}
template <class F> void publish(F&& change) {
    AcquireSRWLockExclusive(&output);
    auto& d = SpidyWarpData;
    InterlockedIncrement64(&d.sequence);
    change(d);
    InterlockedIncrement64(&d.sequence);
    ReleaseSRWLockExclusive(&output);
}
// The one transition manager on this actor in the game's component registry
// (decoded at 16798f0, as game_player reads it); 0 for none, or for two.
uintptr_t find(uintptr_t record) {
    const auto table = pointer(base + registryTable);
    int32_t count{};
    if (!table || !read(base + registryCount, &count, 4) || count <= 0 || count > 0x100000)
        return 0;
    uintptr_t found{};
    for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
        uintptr_t address{};
        uint32_t generation{}, handle{};
        if (!read(table + i * 16ull, &address, 8) || !read(table + i * 16ull + 8, &generation, 4) ||
            address < 0x10000 || address % 8 || !generation || generation > 0xfff)
            continue;
        if (pointer(address) != base + managerVtable || pointer(address + 8) != record ||
            !read(address + 0x14, &handle, 4) || handle != (generation << 20 | i))
            continue;
        if (found)
            return 0;
        found = address;
    }
    return found;
}
// The game's own call, which may fault on a hero the game let go of meanwhile.
bool enter(uintptr_t at, const Request& r, bool& faulted) {
    // The game's own callers pass a zero hint, no flags and -1 (none) for
    // the time: the hero's current state picks how he goes onto the wall.
    const Vec3 hint{};
    __try {
        return reinterpret_cast<Enter>(base + enterRva)(reinterpret_cast<void*>(at), &r.point, &r.normal, &hint, 0,
                                                         -1.f, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return false;
    }
}
} // namespace

uint32_t game_warp::start(uintptr_t gameBase) {
    if (enabled.load())
        return 0;
    if (pointer(gameBase + managerVtable + enterSlot) != gameBase + enterRva)
        return 9601;
    base = gameBase;
    enabled = true;
    publish([](Data& d) {
        const auto sequence = d.sequence;
        d = {};
        d.sequence = sequence;
        d.status = 2;
    });
    return 0;
}
bool game_warp::target(uintptr_t record) {
    if (!enabled.load() || !record)
        return false;
    AcquireSRWLockShared(&lock);
    const bool known = manager && managerRecord == record && pointer(manager) == base + managerVtable &&
                       pointer(manager + 8) == record;
    ReleaseSRWLockShared(&lock);
    if (known)
        return true;
    const auto found = find(record);
    AcquireSRWLockExclusive(&lock);
    manager = found;
    managerRecord = found ? record : 0;
    ReleaseSRWLockExclusive(&lock);
    publish([&](Data& d) {
        d.manager = found;
        d.record = record;
    });
    return found != 0;
}
bool game_warp::crawl(Vec3 point, Vec3 normal) {
    if (!enabled.load() || !finite(point) || !finite(normal) || length(normal) < .5f)
        return false;
    const Vec3 n = normalized(normal);
    AcquireSRWLockExclusive(&lock);
    const bool known = manager != 0;
    if (known)
        pending = {true, point, n};
    ReleaseSRWLockExclusive(&lock);
    publish([&](Data& d) {
        if (!known) {
            ++d.noManager;
            return;
        }
        ++d.asked;
        d.point = point;
        d.normal = n;
    });
    return known;
}
void game_warp::frame() {
    if (!enabled.load(std::memory_order_relaxed))
        return;
    AcquireSRWLockExclusive(&lock);
    const Request r = pending;
    pending.waiting = false;
    const uintptr_t at = manager, record = managerRecord;
    ReleaseSRWLockExclusive(&lock);
    if (!r.waiting) {
        publish([](Data& d) { ++d.frames; });
        return;
    }
    // A manager the game has let go of meanwhile (a level change, a respawn)
    // is no one's to ask; so is a state machine of another kind.
    if (!at || pointer(at) != base + managerVtable || pointer(at + 8) != record ||
        pointer(pointer(at + 0x50)) != base + machineVtable) {
        publish([](Data& d) {
            ++d.frames;
            ++d.noManager;
        });
        return;
    }
    bool faulted{};
    const bool taken = enter(at, r, faulted);
    publish([&](Data& d) {
        ++d.frames;
        if (faulted)
            ++d.faulted;
        else if (taken)
            ++d.applied;
        else
            ++d.refused;
    });
}
void game_warp::observe(uint32_t swing, bool stepped) {
    publish([&](Data& d) {
        d.swing = swing;
        ++d.visits;
        d.steps += stepped;
    });
}
void game_warp::outcome(bool taken) {
    publish([&](Data& d) {
        if (taken)
            ++d.taken;
        else
            ++d.declined;
    });
}
Data game_warp::data() {
    AcquireSRWLockShared(&output);
    auto out = SpidyWarpData;
    ReleaseSRWLockShared(&output);
    return out;
}
void game_warp::stop() {
    enabled = false;
    AcquireSRWLockExclusive(&lock);
    pending = {};
    manager = managerRecord = 0;
    ReleaseSRWLockExclusive(&lock);
    publish([](Data& d) { d.status = 0; });
}

// A probe's crawl for the hero on this record, on the wall through the point,
// at the next frame of the camera's update (game_shooter's hook: the shooter
// must be running). 0 once the game's state machine took it.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyWarpCrawl(void* input) {
    struct Probe {
        uint32_t magic, version, bytes, reserved;
        uint64_t record;
        Vec3 point, normal;
    } p{};
    static_assert(sizeof(Probe) == 48);
    if (!read(reinterpret_cast<uintptr_t>(input), &p, sizeof(p)) || p.magic != 0x53575043 || p.version != 2 ||
        p.bytes != sizeof(p) || !p.record)
        return 9602;
    if (const auto code = game_warp::start(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))))
        return code;
    if (!game_warp::target(p.record))
        return 9603;
    const auto before = game_warp::data();
    if (!game_warp::crawl(p.point, p.normal))
        return 9604;
    const auto limit = GetTickCount64() + 1000;
    for (;;) {
        const auto now = game_warp::data();
        if (now.applied > before.applied)
            return 0;
        if (now.refused > before.refused)
            return 9605;
        if (now.faulted > before.faulted)
            return 9606;
        if (GetTickCount64() >= limit)
            return 9607; // no frame: paused, or the shooter's hook is not running
        Sleep(5);
    }
}
extern "C" __declspec(dllexport) DWORD WINAPI SpidyWarpSample(void* out) {
    const auto sample = game_warp::data();
    __try {
        std::memcpy(out, &sample, sizeof(sample));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9608;
    }
    return 0;
}

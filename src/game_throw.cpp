// The game's throw of a prop aimed by the free hand (game_throw.hpp,
// throw_aim.hpp). Addresses are image offsets in the supported Spider-Man.exe.
#include "spidy/game_throw.hpp"
#include "spidy/game_targets.hpp"
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <vector>
#include <windows.h>
using namespace spidy;
using namespace spidy::game_throw;
extern "C" {
__declspec(dllexport) Data SpidyThrowData;
}

namespace {
// The aim results' getters: the slot's actor handle (results, out, slot), its
// aim point (results, slot) and its flat way (results, out, slot). The slot
// HeroStateWebYankEnviro's throw reads (0xa52270).
constexpr uintptr_t handleRva = 0xb0f290, partRva = 0xb0f2b0, wayRva = 0xb0f240; // MilesMorales.exe (by hand)
constexpr uint32_t throwSlot = 4;
// The actors' own table, in which a handle's index names a record of 0xc0
// bytes (0x15a0560): its pointer, and how many there are.
constexpr uintptr_t actorTable = 0x83fa000, actorCount = 0x83fa01c;
constexpr uint32_t actorBytes = 0xc0;
// A character the hand points at stands within this of its centre, looked up
// at most this often.
constexpr float enemyWithin = 2;
constexpr ULONGLONG lookupMs = 200;

using HandleFn = uint32_t*(__fastcall*)(void*, uint32_t*, uint32_t);
using PartFn = uint32_t(__fastcall*)(void*, uint32_t);
using WayFn = float*(__fastcall*)(void*, float*, uint32_t);
HandleFn originalHandle{};
PartFn originalPart{};
WayFn originalWay{};
uintptr_t base{};
void* hooks[3]{};
bool hooked{};
std::atomic<bool> enabled{};
SRWLOCK lifecycle = SRWLOCK_INIT;
// The hand's aim, the enemy found for it and SpidyThrowData, under this.
SRWLOCK lock = SRWLOCK_INIT;
throw_aim::Request latest;
ULONGLONG aimedAt{}, lookedAt{};
Vec3 lookedFor{};
uint32_t enemyHandle{};

template <class F> void publish(F change) {
    InterlockedIncrement64(&SpidyThrowData.sequence);
    change(SpidyThrowData);
    InterlockedIncrement64(&SpidyThrowData.sequence);
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
bool entry(uintptr_t rva, std::initializer_list<uint8_t> bytes) {
    uint8_t have[16]{};
    return bytes.size() <= sizeof(have) && read(base + rva, have, bytes.size()) &&
           std::memcmp(have, bytes.begin(), bytes.size()) == 0;
}
// An actor record's handle, as the game makes it and only when its table
// still has the record under it; 0 otherwise.
uint32_t handleOf(uint64_t record) {
    uint32_t index{}, count{};
    uint16_t generation{};
    uintptr_t table{};
    if (!record || !read(record + 0xc, &index, 4) || !read(record + 0x10, &generation, 2) ||
        !read(base + actorTable, &table, 8) || !read(base + actorCount, &count, 4))
        return 0;
    index &= 0xfffff;
    if (!table || index >= count || table + static_cast<uint64_t>(index) * actorBytes != record ||
        !(generation & 0x7ff))
        return 0;
    return throw_aim::handle(index, generation);
}
// The enemy in the fight standing nearest `point`, within enemyWithin; his
// handle, 0 for none.
uint32_t enemyAt(Vec3 point) {
    std::vector<game_targets::Candidate> candidates;
    if (!game_targets::scan(base, 1u << static_cast<uint32_t>(game_targets::Kind::bot), candidates))
        return 0;
    std::vector<Vec3> feet;
    std::vector<uint64_t> records;
    for (const auto& c : candidates) {
        Vec3 at;
        if (game_targets::fighting(base, c) && game_targets::position(c, at)) {
            feet.push_back(at);
            records.push_back(c.record);
        }
    }
    const int found = throw_aim::nearest(feet.data(), static_cast<int>(feet.size()), point, enemyWithin);
    return found < 0 ? 0 : handleOf(records[static_cast<size_t>(found)]);
}
// The hand's aim while it holds: its way and the enemy's handle (0 none).
bool current(Vec3& way, uint32_t& enemy) {
    if (!enabled.load(std::memory_order_relaxed))
        return false;
    AcquireSRWLockShared(&lock);
    const bool live = (latest.flags & throw_aim::active) && GetTickCount64() - aimedAt < leaseMs;
    way = latest.direction;
    enemy = latest.flags & throw_aim::character ? enemyHandle : 0;
    ReleaseSRWLockShared(&lock);
    return live;
}

uint32_t* __fastcall handleHook(void* results, uint32_t* out, uint32_t slot) {
    uint32_t* r = originalHandle(results, out, slot);
    Vec3 way;
    uint32_t enemy;
    if (slot == throwSlot && out && current(way, enemy)) {
        *out = enemy;
        AcquireSRWLockExclusive(&lock);
        publish([&](Data& d) {
            ++d.reads;
            ++(enemy ? d.atEnemy : d.atWay);
            d.enemy = enemy;
            d.direction = way;
        });
        ReleaseSRWLockExclusive(&lock);
    }
    return r;
}
uint32_t __fastcall partHook(void* results, uint32_t slot) {
    const uint32_t part = originalPart(results, slot);
    Vec3 way;
    uint32_t enemy;
    // His first aim point: the byte index the game asks his targets for (his
    // feet where he has none).
    return slot == throwSlot && current(way, enemy) && enemy ? 0u : part;
}
float* __fastcall wayHook(void* results, float* out, uint32_t slot) {
    float* r = originalWay(results, out, slot);
    Vec3 way;
    uint32_t enemy;
    if (slot == throwSlot && out && current(way, enemy)) {
        out[0] = way.x;
        out[1] = 0;
        out[2] = way.z;
    }
    return r;
}
} // namespace

uint32_t game_throw::start(uintptr_t imageBase) {
    AcquireSRWLockExclusive(&lifecycle);
    uint32_t result{};
    do {
        if (enabled)
            break;
        base = imageBase;
        // The bytes each hook's jump covers: mov eax, r8d, lea r8, [rax+rax*2];
        // mov eax, edx, lea rdx, [rax+rax*2]; push rbx, sub rsp, 30h.
        if (!entry(handleRva, {0x41, 0x8b, 0xc0, 0x48, 0x05, 0x5f, 0x07}) ||
            !entry(partRva, {0x8b, 0xc2, 0x48, 0x8d, 0x14, 0x40}) ||
            !entry(wayRva, {0x40, 0x53, 0x48, 0x83, 0xec, 0x30})) {
            result = 9801;
            break;
        }
        if (!hooked) {
            const auto s = MH_Initialize();
            if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
                result = 9810 + s;
                break;
            }
            hooks[0] = reinterpret_cast<void*>(base + handleRva);
            hooks[1] = reinterpret_cast<void*>(base + partRva);
            hooks[2] = reinterpret_cast<void*>(base + wayRva);
            void* const detours[3]{reinterpret_cast<void*>(handleHook), reinterpret_cast<void*>(partHook),
                                   reinterpret_cast<void*>(wayHook)};
            void** const originals[3]{reinterpret_cast<void**>(&originalHandle),
                                      reinterpret_cast<void**>(&originalPart),
                                      reinterpret_cast<void**>(&originalWay)};
            for (int i = 0; i < 3 && !result; ++i)
                if (const auto c = MH_CreateHook(hooks[i], detours[i], originals[i]); c != MH_OK) {
                    for (int j = 0; j < i; ++j)
                        MH_RemoveHook(hooks[j]);
                    result = 9820 + 10 * i + c;
                }
            if (result)
                break;
            hooked = true;
        }
        enabled = true;
        // Under one freeze of the game's threads (stereo_probe.cpp says why).
        for (auto* hook : hooks)
            if (const auto s = MH_QueueEnableHook(hook); s != MH_OK && !result)
                result = 9860 + s;
        if (const auto s = result ? MH_OK : MH_ApplyQueued(); s != MH_OK)
            result = 9860 + s;
        if (result) {
            enabled = false;
            for (auto* hook : hooks)
                MH_QueueDisableHook(hook);
            MH_ApplyQueued();
        }
    } while (false);
    AcquireSRWLockExclusive(&lock);
    publish([&](Data& d) {
        d.status = result ? 3 : 1;
        d.error = result;
    });
    ReleaseSRWLockExclusive(&lock);
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}
void game_throw::stop() {
    AcquireSRWLockExclusive(&lifecycle);
    enabled = false;
    if (hooked)
        for (auto* hook : hooks)
            MH_DisableHook(hook);
    AcquireSRWLockExclusive(&lock);
    latest = {};
    enemyHandle = 0;
    publish([](Data& d) { d.status = 0; });
    ReleaseSRWLockExclusive(&lock);
    ReleaseSRWLockExclusive(&lifecycle);
}
void game_throw::aim(const throw_aim::Request& request) {
    if (!enabled.load(std::memory_order_relaxed))
        return;
    const ULONGLONG now = GetTickCount64();
    // The enemy for a hand at a character, off the lock (the scan walks the
    // registry): looked up again for a new point or after a while.
    bool lookUp = false;
    AcquireSRWLockShared(&lock);
    if (request.flags & throw_aim::character)
        lookUp = !(latest.flags & throw_aim::character) || now - lookedAt >= lookupMs ||
                 length(request.point - lookedFor) > 1;
    ReleaseSRWLockShared(&lock);
    const uint32_t enemy = lookUp ? enemyAt(request.point) : 0;
    AcquireSRWLockExclusive(&lock);
    latest = request;
    aimedAt = now;
    if (!(request.flags & throw_aim::character))
        enemyHandle = 0;
    else if (lookUp) {
        enemyHandle = enemy;
        lookedAt = now;
        lookedFor = request.point;
        if (!enemy)
            publish([](Data& d) { ++d.noEnemy; });
    }
    ReleaseSRWLockExclusive(&lock);
}
Data game_throw::data() {
    AcquireSRWLockShared(&lock);
    const Data out = SpidyThrowData;
    ReleaseSRWLockShared(&lock);
    return out;
}

// For a probe without the swing (which starts it in a session): the hooks
// in, as game_throw::start returns.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyThrowStart(void*) {
    return game_throw::start(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
}
// From the XR worker, every frame of a throw's hold: the free hand's aim
// (throw_aim::Request). 0 taken, 9802 not running, 9803 a request of another
// shape.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyThrowAim(void* input) {
    if (!enabled.load(std::memory_order_relaxed))
        return 9802;
    throw_aim::Request request;
    const throw_aim::Request expected;
    if (!input || !read(reinterpret_cast<uintptr_t>(input), &request, sizeof(request)) ||
        request.magic != expected.magic || request.version != expected.version ||
        request.bytes != expected.bytes)
        return 9803;
    game_throw::aim(request);
    return 0;
}

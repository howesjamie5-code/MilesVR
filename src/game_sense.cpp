// The game's warnings of attacks about to land on the hero (game_sense.hpp).
#include "spidy/game_sense.hpp"
#include <cmath>
#include <cstring>
#include <windows.h>
using namespace spidy;
using namespace spidy::game_sense;
extern "C" {
__declspec(dllexport) Data SpidySenseData;
}
namespace {
// The actor table (native_bodies.hpp: 0xc0 bytes a record) and the game's
// component lookups on a record: through its table at +0x80 when it has one
// (its size at +0x88), else its list at +0x58.
constexpr uintptr_t actorRecords = 0x83fa000, actorRecordCount = 0x83fa01c, findInTableRva = 0x1b844b0,
                    findInListRva = 0x1b85650;
using FindComponent = uintptr_t (*)(void*, const void*);
SRWLOCK output = SRWLOCK_INIT;
bool copy(uintptr_t p, void* out, size_t n) {
    __try {
        if (p < 0x10000)
            return false;
        std::memcpy(out, reinterpret_cast<const void*>(p), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> T value(uintptr_t p) {
    T v{};
    copy(p, &v, sizeof(v));
    return v;
}
uintptr_t pointer(uintptr_t p) {
    return value<uintptr_t>(p);
}
uintptr_t componentOf(uintptr_t base, uintptr_t record, uintptr_t type) {
    const bool hashed = value<uint16_t>(record + 0x88) != 0;
    __try {
        return reinterpret_cast<FindComponent>(base + (hashed ? findInTableRva : findInListRva))(
            reinterpret_cast<void*>(record + (hashed ? 0x80 : 0x58)), reinterpret_cast<void*>(base + type));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
} // namespace

bool game_sense::read(uintptr_t base, uint64_t hero, uint64_t& manager, Attack (&out)[attackSlots],
                      uint32_t& count, double& now) {
    count = 0;
    if (!base || !hero)
        return false;
    // The manager found before, while it is still the hero's; else his own,
    // by the type his code finds it by.
    if (!manager || pointer(manager) != base + managerTable || pointer(manager + 8) != hero) {
        manager = 0;
        if (pointer(base + managerType + 0x60) != base + managerName)
            return false;
        const auto found = componentOf(base, hero, managerType);
        if (!found || pointer(found) != base + managerTable || pointer(found + 8) != hero)
            return false;
        manager = found;
    }
    uint32_t n{};
    if (!copy(manager + countAt, &n, sizeof(n)) || n > attackSlots ||
        !copy(base + clockRva, &now, sizeof(now)) || !std::isfinite(now))
        return false;
    static thread_local uint8_t raw[attackSlots * attackBytes];
    if (n && !copy(manager + attacksAt, raw, n * attackBytes))
        return false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* e = raw + i * attackBytes;
        Attack a;
        std::memcpy(&a.handle, e + 0x00, sizeof(a.handle));
        std::memcpy(&a.warnAt, e + 0x30, sizeof(a.warnAt));
        std::memcpy(&a.flags, e + 0x5c, sizeof(a.flags));
        std::memcpy(&a.state, e + 0x60, sizeof(a.state));
        std::memcpy(&a.impactAt, e + 0x80, sizeof(a.impactAt));
        out[count++] = a;
    }
    return true;
}
uint64_t game_sense::actorOf(uintptr_t base, uint32_t handle) {
    const uint32_t generation = handle >> 20 & 0x7ff, index = handle & 0xfffff;
    const auto records = pointer(base + actorRecords);
    if (!generation || !records || index >= value<uint32_t>(base + actorRecordCount))
        return 0;
    const auto record = records + index * 0xc0ull;
    return value<uint16_t>(record + 0x10) == generation ? record : 0;
}
bool game_sense::positionOf(uint64_t record, Vec3& out) {
    float m[16]{};
    if (!record || !copy(pointer(record), m, sizeof(m)))
        return false;
    out = {m[12], m[13], m[14]};
    return finite(out) && length(out) < 1e6f;
}
void game_sense::publish(const Data& data) {
    AcquireSRWLockExclusive(&output);
    // Odd while it changes, as a reader outside the game expects.
    const auto sequence = InterlockedIncrement64(&SpidySenseData.sequence);
    SpidySenseData = data;
    SpidySenseData.sequence = sequence;
    InterlockedIncrement64(&SpidySenseData.sequence);
    ReleaseSRWLockExclusive(&output);
}
Data game_sense::data() {
    AcquireSRWLockShared(&output);
    auto out = SpidySenseData;
    ReleaseSRWLockShared(&output);
    return out;
}
// The warnings at the latest input sample (game_sense::Data).
extern "C" __declspec(dllexport) DWORD WINAPI SpidySenseSample(void* out) {
    const auto sample = game_sense::data();
    __try {
        std::memcpy(out, &sample, sizeof(sample));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9503;
    }
    return 0;
}

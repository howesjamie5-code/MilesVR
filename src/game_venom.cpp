#include "spidy/game_venom.hpp"
#include "spidy/venom_punch.hpp"
#include <windows.h>
#include <cmath>
#include <cstring>

namespace {
// The component registry (game_targets.cpp): 16-byte entries of an address and a generation; a
// component's handle (+0x14) is its generation << 20 | its entry's index.
constexpr uintptr_t registryTable = 0x83f9fa0, registryCount = 0x83f9fc0;
constexpr ULONGLONG scanMs = 1000;

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
struct Entry {
    uintptr_t address;
    uint32_t generation, pad;
};
// A component's head: its vtable, its actor record and its registry handle.
struct Head {
    uintptr_t vtable, record;
    uint32_t pad, handle;
};
// The player's HeroMeleeManager, the registry entry it was found in, and its actor record.
uintptr_t component{}, owner{};
uint32_t entryIndex{}, entryGeneration{};
ULONGLONG lastScan{};

bool mine(uintptr_t base, uintptr_t address, uintptr_t record, uint32_t index, uint32_t generation) {
    Head h{};
    return read(address, &h, sizeof(h)) && h.vtable == base + spidy::venom_punch::meleeManagerVtable &&
           h.record == record && h.handle == (generation << 20 | index);
}
// Still the registry's: a challenge restarted, or a load, makes a new component and may leave the old
// one in memory as it was (0.1.10: the meter read as spent while the game's showed full).
bool registered(uintptr_t base) {
    uintptr_t table{};
    Entry e{};
    return read(base + registryTable, &table, 8) && table && read(table + entryIndex * 16ull, &e, sizeof(e)) &&
           e.address == component && e.generation == entryGeneration;
}
bool find(uintptr_t base, uintptr_t record) {
    uintptr_t table{};
    int32_t count{};
    if (!read(base + registryTable, &table, 8) || !table || !read(base + registryCount, &count, 4) || count <= 0 ||
        count > 0x100000)
        return false;
    for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
        Entry e{};
        if (read(table + i * 16ull, &e, sizeof(e)) && e.address >= 0x10000 && !(e.address % 8) && e.generation &&
            e.generation <= 0xfff && mine(base, e.address, record, i, e.generation)) {
            component = e.address;
            entryIndex = i;
            entryGeneration = e.generation;
            return true;
        }
    }
    return false;
}
} // namespace

spidy::game_venom::Meter spidy::game_venom::sample(uintptr_t base, uintptr_t record) {
    Meter out;
    if (!base || !record) {
        component = owner = 0;
        return out;
    }
    if (record != owner ||
        (component && (!registered(base) || !mine(base, component, record, entryIndex, entryGeneration)))) {
        component = 0;
        owner = record;
    }
    const ULONGLONG now = GetTickCount64();
    if (!component && now - lastScan >= scanMs) {
        lastScan = now;
        find(base, record);
    }
    float value{};
    if (component && read(component + venom_punch::meterOffset, &value, 4) && std::isfinite(value) &&
        value >= 0 && value <= venom_punch::fullMeter * 2) {
        out.address = component + venom_punch::meterOffset;
        out.value = value;
    }
    return out;
}
void spidy::game_venom::reset() {
    component = owner = 0;
    lastScan = 0;
}

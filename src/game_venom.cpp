#include "spidy/game_venom.hpp"
#include "spidy/venom_punch.hpp"
#include <windows.h>
#include <cmath>
#include <cstring>

namespace {
// The component registry (game_targets.cpp): 16-byte entries of an address and a generation.
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
// A component's head: its vtable and its actor record.
struct Head {
    uintptr_t vtable, record;
};
uintptr_t component{}, owner{};
ULONGLONG lastScan{};

bool mine(uintptr_t base, uintptr_t address, uintptr_t record) {
    Head h{};
    return read(address, &h, sizeof(h)) && h.vtable == base + spidy::venom_punch::meleeManagerVtable &&
           h.record == record;
}
uintptr_t find(uintptr_t base, uintptr_t record) {
    uintptr_t table{};
    int32_t count{};
    if (!read(base + registryTable, &table, 8) || !table || !read(base + registryCount, &count, 4) || count <= 0 ||
        count > 0x100000)
        return 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
        struct Entry {
            uintptr_t address;
            uint32_t generation, pad;
        } e{};
        if (read(table + i * 16ull, &e, sizeof(e)) && e.address >= 0x10000 && !(e.address % 8) && e.generation &&
            mine(base, e.address, record))
            return e.address;
    }
    return 0;
}
} // namespace

spidy::game_venom::Meter spidy::game_venom::sample(uintptr_t base, uintptr_t record) {
    Meter out;
    if (!base || !record) {
        component = owner = 0;
        return out;
    }
    if (record != owner || (component && !mine(base, component, record))) {
        component = 0;
        owner = record;
    }
    const ULONGLONG now = GetTickCount64();
    if (!component && now - lastScan >= scanMs) {
        lastScan = now;
        component = find(base, record);
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

#include "spidy/game_venom.hpp"
#include "spidy/venom_punch.hpp"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
// The component registry (game_targets.cpp): 16-byte entries of an address and a generation; a
// component's handle (+0x14) is its generation << 20 | its entry's index.
constexpr uintptr_t registryTable = 0x83f9fa0, registryCount = 0x83f9fc0;
// Searched again this often while one is known (another may have appeared), and at most this often
// while none is.
constexpr ULONGLONG rescanMs = 2000, scanMs = 1000;
constexpr size_t mostCandidates = 8;

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
struct Candidate {
    uintptr_t address{};
    uint32_t index{}, generation{};
};
std::vector<Candidate> candidates;
uintptr_t owner{};
// The one read from, the meter it read when chosen, and whether that has moved since.
uintptr_t chosen{};
float chosenAt{};
bool moved{};
ULONGLONG lastScan{};

bool mine(uintptr_t base, const Candidate& c, uintptr_t record) {
    Head h{};
    return read(c.address, &h, sizeof(h)) && h.vtable == base + spidy::venom_punch::meleeManagerVtable &&
           h.record == record && h.handle == (c.generation << 20 | c.index);
}
// Still the registry's: a challenge restarted, or a load, makes a new component and may leave the old
// one in memory as it was (0.1.10: the meter read as spent while the game's showed full).
bool registered(uintptr_t base, const Candidate& c) {
    uintptr_t table{};
    Entry e{};
    return read(base + registryTable, &table, 8) && table && read(table + c.index * 16ull, &e, sizeof(e)) &&
           e.address == c.address && e.generation == c.generation;
}
void scan(uintptr_t base, uintptr_t record) {
    candidates.clear();
    uintptr_t table{};
    int32_t count{};
    if (!read(base + registryTable, &table, 8) || !table || !read(base + registryCount, &count, 4) || count <= 0 ||
        count > 0x100000)
        return;
    for (uint32_t i = 0; i < static_cast<uint32_t>(count) && candidates.size() < mostCandidates; ++i) {
        Entry e{};
        if (!read(table + i * 16ull, &e, sizeof(e)) || e.address < 0x10000 || e.address % 8 || !e.generation ||
            e.generation > 0xfff)
            continue;
        const Candidate c{e.address, i, e.generation};
        if (mine(base, c, record))
            candidates.push_back(c);
    }
}
bool meter(uintptr_t address, float& value) {
    return read(address + spidy::venom_punch::meterOffset, &value, 4) && std::isfinite(value) && value >= 0 &&
           value <= spidy::venom_punch::fullMeter * 2;
}
} // namespace

spidy::game_venom::Meter spidy::game_venom::sample(uintptr_t base, uintptr_t record) {
    Meter out;
    if (!base || !record) {
        reset();
        return out;
    }
    if (record != owner) {
        candidates.clear();
        chosen = 0;
        owner = record;
        lastScan = 0;
    }
    std::erase_if(candidates, [&](const Candidate& c) { return !registered(base, c) || !mine(base, c, record); });
    const ULONGLONG now = GetTickCount64();
    if (now - lastScan >= (candidates.empty() ? scanMs : rescanMs)) {
        lastScan = now;
        scan(base, record);
    }
    // The highest meter is the one the game counts in: the others have stood still.
    uintptr_t best{};
    float bestValue = -1;
    for (const auto& c : candidates) {
        float value{};
        if (!meter(c.address, value))
            continue;
        if (value > bestValue) {
            if (best)
                out.other = std::max(out.other, bestValue);
            best = c.address;
            bestValue = value;
        } else {
            out.other = std::max(out.other, value);
        }
    }
    out.candidates = static_cast<uint32_t>(candidates.size());
    if (!best) {
        chosen = 0;
        return out;
    }
    if (best != chosen) {
        chosen = best;
        chosenAt = bestValue;
        moved = false;
    }
    moved = moved || bestValue != chosenAt;
    out.value = bestValue;
    if (candidates.size() == 1 || moved)
        out.address = best + venom_punch::meterOffset;
    return out;
}
void spidy::game_venom::reset() {
    candidates.clear();
    owner = chosen = 0;
    moved = false;
    lastScan = 0;
}

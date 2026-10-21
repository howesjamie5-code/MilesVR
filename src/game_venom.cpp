#include "spidy/game_venom.hpp"
#include "spidy/venom_punch.hpp"
#include <windows.h>
#include <algorithm>
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
// The meter lies within this far of the start of the component that holds it.
constexpr uintptr_t mostOwnerOffset = 0x1000;

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
// The player's HeroMeleeManagers (the meter at +meterOffset from one: where it was found, October
// 18), and the components of the class that holds the meter once that is known.
std::vector<Candidate> melee, holders;
// The player's components from the latest search, by address.
struct Component {
    uintptr_t address, vtable;
    uint32_t index, generation;
};
std::vector<Component> components;
uintptr_t owner{};
// The class that holds the meter (its vtable, as an offset in the game's image) and the meter's
// offset in it: learnt from the HeroMeleeManager, then searched for by itself.
uintptr_t holderVtable{};
uint32_t holderOffset{};
// The one read from, the meter it read when chosen, and whether that has moved since.
uintptr_t chosen{};
float chosenAt{};
bool moved{};
ULONGLONG lastScan{};

bool mine(uintptr_t base, const Candidate& c, uintptr_t record, uintptr_t vtable) {
    Head h{};
    return read(c.address, &h, sizeof(h)) && h.vtable == base + vtable && h.record == record &&
           h.handle == (c.generation << 20 | c.index);
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
    melee.clear();
    holders.clear();
    components.clear();
    uintptr_t table{};
    int32_t count{};
    if (!read(base + registryTable, &table, 8) || !table || !read(base + registryCount, &count, 4) || count <= 0 ||
        count > 0x100000)
        return;
    for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
        Entry e{};
        Head h{};
        if (!read(table + i * 16ull, &e, sizeof(e)) || e.address < 0x10000 || e.address % 8 || !e.generation ||
            e.generation > 0xfff || !read(e.address, &h, sizeof(h)) || h.record != record ||
            h.handle != (e.generation << 20 | i))
            continue;
        components.push_back({e.address, h.vtable - base, i, e.generation});
        const Candidate c{e.address, i, e.generation};
        if (h.vtable == base + spidy::venom_punch::meleeManagerVtable && melee.size() < mostCandidates)
            melee.push_back(c);
        if (holderVtable && h.vtable == base + holderVtable && holders.size() < mostCandidates)
            holders.push_back(c);
    }
    std::sort(components.begin(), components.end(),
              [](const Component& a, const Component& b) { return a.address < b.address; });
}
bool meter(uintptr_t address, float& value) {
    return read(address, &value, 4) && std::isfinite(value) && value >= 0 &&
           value <= spidy::venom_punch::mostMeter;
}
// The component the meter at `address` lies in: the player's that starts nearest below it.
const Component* holding(uintptr_t address) {
    const Component* found{};
    for (const auto& c : components)
        if (c.address < address && address - c.address < mostOwnerOffset)
            found = &c;
    return found;
}
// The class that holds the meter, from the one HeroMeleeManager: the meter at its +meterOffset lies
// past its end (0x148 bytes, its deleting destructor af6cf0), in the component the game made after
// it. In 0.1.16 a venom punch now and then left the HUD's bar full while the meter read empty: the
// game had put another component there.
void learn() {
    if (holderVtable || melee.size() != 1)
        return;
    const uintptr_t at = melee.front().address + spidy::venom_punch::meterOffset;
    float value{};
    const Component* c = holding(at);
    if (!c || c->address <= melee.front().address || !meter(at, value))
        return;
    holderVtable = c->vtable;
    holderOffset = static_cast<uint32_t>(at - c->address);
    for (const auto& k : components)
        if (k.vtable == holderVtable && holders.size() < mostCandidates)
            holders.push_back({k.address, k.index, k.generation});
}
} // namespace

namespace {
constexpr uintptr_t venomObjectVtable = 0x3e15b48, flagGlobal = 0x7a5c3c0, flagTable = 0x1180;
constexpr uint32_t venomJumpFlag = 0x46e2a4c5, venomSmashFlag = 0x961f104a, venomDashFlag = 0xaeb527e3;
// The component a registry handle names (0: none, or one since remade).
uintptr_t resolveHandle(uintptr_t base, uint32_t handle) {
    uintptr_t table{};
    Entry e{};
    if (!handle || !read(base + registryTable, &table, 8) || !table ||
        !read(table + (handle & 0xfffff) * 16ull, &e, sizeof(e)) || e.generation != handle >> 20)
        return 0;
    return e.address;
}
// A game flag by its hash (ed2db0 -> 20dc290, ed2ab0): -1 not in the table or not a plain flag.
int gameFlag(uintptr_t base, uint32_t hash) {
    const uintptr_t t = base + flagGlobal + flagTable;
    uintptr_t keys{}, values{};
    uint32_t capacity{}, stride{};
    if (!read(t, &keys, 8) || !read(t + 8, &values, 8) || !read(t + 0x14, &capacity, 4) ||
        !read(t + 0x1c, &stride, 4) || !keys || !values || !capacity || capacity > 0x10000 || stride < 16)
        return -1;
    const uint64_t x = static_cast<uint64_t>(hash) * 0x9e3779b1ull;
    uint32_t slot = static_cast<uint32_t>((x >> 32) ^ (x & 0xffffffff)) % capacity;
    for (uint32_t n = 0; n < capacity; ++n, slot = (slot + 1) % capacity) {
        uint32_t key{};
        if (!read(keys + slot * 4ull, &key, 4) || !key)
            return -1;
        if (key != hash)
            continue;
        uintptr_t definition{}, state{};
        uint8_t plain{}, on{};
        const uintptr_t entry = values + static_cast<uintptr_t>(slot) * stride;
        if (!read(entry, &definition, 8) || !read(entry + 8, &state, 8) || !read(definition + 0x54, &plain, 1) ||
            !plain || !read(state + 4, &on, 1))
            return -1;
        return on ? 1 : 0;
    }
    return -1;
}
} // namespace

spidy::game_venom::Unlocks spidy::game_venom::unlocks(uintptr_t base) {
    Unlocks out;
    out.jump = gameFlag(base, venomJumpFlag);
    out.smash = gameFlag(base, venomSmashFlag);
    out.dash = gameFlag(base, venomDashFlag);
    for (const auto& c : components) {
        if (c.vtable != venomObjectVtable)
            continue;
        uint32_t handle{};
        unsigned char raw[0xf0];
        const uintptr_t abilities = read(c.address + 0x68, &handle, 4) ? resolveHandle(base, handle) : 0;
        if (!abilities || !read(abilities, raw, sizeof(raw)))
            break;
        uint32_t mask{};
        std::memcpy(&mask, raw + 0xe8, 4);
        for (int k = 0; k < 16; ++k) {
            uint32_t m{};
            float left{};
            std::memcpy(&m, raw + 0x48 + 8 * k, 4);
            std::memcpy(&left, raw + 0x4c + 8 * k, 4);
            if (left > 0)
                mask |= m;
        }
        for (int i = 0; i < 32; ++i)
            if (static_cast<signed char>(raw[0xc8 + i]) > 0)
                mask |= 1u << i;
        out.known = true;
        out.disabled = mask;
        break;
    }
    return out;
}

spidy::game_venom::Meter spidy::game_venom::sample(uintptr_t base, uintptr_t record) {
    Meter out;
    if (!base || !record) {
        reset();
        return out;
    }
    if (record != owner) {
        melee.clear();
        holders.clear();
        components.clear();
        chosen = 0;
        owner = record;
        lastScan = 0;
    }
    std::erase_if(melee, [&](const Candidate& c) {
        return !registered(base, c) || !mine(base, c, record, venom_punch::meleeManagerVtable);
    });
    std::erase_if(holders, [&](const Candidate& c) {
        return !registered(base, c) || !mine(base, c, record, holderVtable);
    });
    const ULONGLONG now = GetTickCount64();
    const bool none = holderVtable ? holders.empty() : melee.empty();
    if (now - lastScan >= (none ? scanMs : rescanMs)) {
        lastScan = now;
        scan(base, record);
        learn();
    }
    out.candidates = static_cast<uint32_t>(holderVtable ? holders.size() : melee.size());
    out.holderVtable = static_cast<uint32_t>(holderVtable);
    out.holderOffset = holderOffset;
    // The meter's own component once its class is known; until then (or if it never is: the
    // neighbour is no registered component of the player's) the HeroMeleeManager's neighbour, as
    // 0.1.16 had it.
    const bool known = holderVtable != 0;
    const auto& from = known ? holders : melee;
    const uintptr_t offset = known ? holderOffset : venom_punch::meterOffset;
    // The highest meter is the one the game counts in: the others have stood still.
    uintptr_t best{};
    float bestValue = -1;
    for (const auto& c : from) {
        float value{};
        if (!meter(c.address + offset, value))
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
    out.route = best ? (known ? 2u : 1u) : 0u;
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
    // The whole of Miles's bars (venom_punch::Bars): the first alone early in the story.
    {
        float words[venom_punch::mostBars * 2]{};
        if (read(best + offset, words, sizeof(words)))
            bestValue = venom_punch::total(venom_punch::bars(words));
    }
    out.value = bestValue;
    // The fields around the meter, for the report: how the game keeps Miles's later bars.
    for (unsigned i = 0; i < std::size(out.around); ++i)
        read(best + offset - Meter::aroundBefore + i * 4, &out.around[i], 4);
    if (from.size() == 1 || moved) {
        out.address = best + offset;
        out.holder = best;
        out.holderClass = base + (known ? holderVtable : venom_punch::meleeManagerVtable);
    }
    return out;
}
bool spidy::game_venom::empty(const Meter& m) {
    uintptr_t vtable{};
    if (!m.address || !m.holder || m.address <= m.holder || m.address - m.holder >= mostOwnerOffset ||
        !read(m.holder, &vtable, 8) || vtable != m.holderClass)
        return false;
    float words[venom_punch::mostBars * 2]{};
    if (!read(m.address, words, sizeof(words)))
        return false;
    const auto b = venom_punch::bars(words);
    __try {
        for (unsigned i = 0; i < b.count; ++i)
            reinterpret_cast<float*>(m.address)[i * 2] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void spidy::game_venom::reset() {
    melee.clear();
    holders.clear();
    components.clear();
    owner = chosen = 0;
    moved = false;
    lastScan = 0;
}

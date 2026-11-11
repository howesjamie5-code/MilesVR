// MilesVR 0.1.80, v16: records the game's own hits (game_record.hpp). Not built with MSVC and not run in the
// game when it was written (a Linux machine); the logic of its text is tested (tests/record_text_tests.cpp).
#include "spidy/game_record.hpp"
#include "spidy/game_record_text.hpp"
#include "spidy/game_targets.hpp"
#include "spidy/math.hpp"
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>
#include <windows.h>
using namespace spidy;
using namespace spidy::game_record;

extern "C" {
// The recorder's notes for the headset side (a separate module): the request is a Note.
__declspec(dllexport) DWORD WINAPI SpidyRecordNote(void* request);
__declspec(dllexport) DWORD WINAPI SpidyRecordStart(void*);
__declspec(dllexport) DWORD WINAPI SpidyRecordStop(void*);
}

namespace {
// The getter of a damage request (native_bodies' directDamageRva), the actor record table its handles index, and
// the DamageSystem object.
constexpr uintptr_t getterRva = 0x23c5260, actorRecords = 0x83fa000, actorRecordCount = 0x83fa01c;
using GetterFn = uint8_t*(__fastcall*)(void*, const uint32_t*, const Vec3*, const Vec3*, const Vec3*);
GetterFn original{};
uintptr_t base{};
void* hook{};
bool hooked{};
std::atomic<bool> enabled{}, stopping{};
std::atomic<int> active{};
std::atomic<uint64_t> heroRecord{};
SRWLOCK lifecycle = SRWLOCK_INIT;
thread_local int issuingDepth = 0;
HANDLE worker{};
LARGE_INTEGER frequency{};

// The notes: a ring, as the first-person worker's (game_fp::say).
constexpr uint32_t noteSlots = 256;
char noteText[noteSlots][200];
std::atomic<uint32_t> noteSerial{};
std::atomic<uint32_t> noteSeq[noteSlots]{};
void say(const char* text) {
    const uint32_t n = noteSerial.fetch_add(1, std::memory_order_acq_rel);
    std::snprintf(noteText[n % noteSlots], sizeof(noteText[0]), "%s", text);
    noteSeq[n % noteSlots].store(n + 1, std::memory_order_release);
}
void say(const std::string& text) {
    say(text.c_str());
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
template <class T> T value(uintptr_t p) {
    T v{};
    read(p, &v, sizeof(v));
    return v;
}
bool entry(uintptr_t rva, std::initializer_list<uint8_t> bytes) {
    uint8_t have[16]{};
    return bytes.size() <= sizeof(have) && read(base + rva, have, bytes.size()) &&
           std::memcmp(have, bytes.begin(), bytes.size()) == 0;
}
LONGLONG now() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double secondsSince(LONGLONG at) {
    return frequency.QuadPart ? static_cast<double>(now() - at) / frequency.QuadPart : 0.;
}
// An actor handle's record (the table native_bodies::actorHandle reads the other way), 0 when stale.
uintptr_t recordOf(uint32_t handle) {
    const auto records = value<uintptr_t>(base + actorRecords);
    const auto count = value<uint32_t>(base + actorRecordCount);
    const uint32_t index = handle & 0xfffff;
    if (!(handle >> 20) || !records || index >= count)
        return 0;
    const auto record = records + index * 0xc0ull;
    return (value<uint16_t>(record + 0x10) & 0x7ff) == handle >> 20 ? record : 0;
}
uint32_t indexOfRecord(uintptr_t record) {
    return record ? value<uint32_t>(record + 0xc) & 0xfffff : 0xfffff;
}

// A request seen leaving the getter, to be read once its caller has filled it in.
struct Pending {
    uintptr_t request{};
    uint32_t victim{};
    LONGLONG at{};
    bool own{};
};
constexpr int pendingSlots = 32;
Pending pending[pendingSlots];
int pendingNext{};
SRWLOCK pendingLock = SRWLOCK_INIT;
// A hit of the hero's whose outcome is read a little later.
struct Follow {
    uint32_t serial{}, victim{};
    LONGLONG at{};
    float before{};
    bool hasBefore{};
    std::string states;
    std::vector<std::string> names;
};
constexpr int followSlots = 16;
Follow follows[followSlots];
int followNext{};
// Actors whose classes were listed, by handle, each with the classes it had then (what a later state is told
// from).
struct Known {
    uint32_t handle{};
    std::vector<std::string> names;
};
std::vector<Known> known;
const Known* knownOf(uint32_t handle) {
    for (const auto& k : known)
        if (k.handle == handle)
            return &k;
    return nullptr;
}
// The first enemy listed in full is the baseline later ones are told from, and its components' words.
std::vector<std::string> baselineNames;
std::vector<std::pair<std::string, std::string>> baselineWords;
// The components whose own words are dumped for each enemy (a block type, a tank's manager, a hit counter...).
bool dumped(const std::string& name) {
    static const char* const names[] = {"BotBlockType",    "BotTankManager",        "BotHitCounter",
                                        "BotAttackResponseManager", "BotHealth",    "BotMeleeWeaponManager",
                                        "HitReactGame",    "DamageModifierMeleeReaction", "BotCombos",
                                        "BotWeaponManager", "BotReactionProcessor"};
    for (const char* n : names)
        if (name == n)
            return true;
    return false;
}
void listOnce(const char* who, uint32_t handle, uintptr_t record) {
    if (!record || knownOf(handle))
        return;
    game_targets::Description d;
    if (!game_targets::describe(base, record, true, d)) {
        say(std::string("REC ") + who + " classes: not readable");
        return;
    }
    if (known.size() < 160)
        known.push_back({handle, d.classes});
    char prefix[48];
    std::snprintf(prefix, sizeof(prefix), "REC %s #%u classes: ", who, handle & 0xfffff);
    const bool first = baselineNames.empty();
    if (first) {
        baselineNames = d.classes;
        for (const auto& line : lines(prefix, d.classes))
            say(line);
    } else {
        // Later ones only differ from the first by a few classes: those are the ones that tell types apart.
        std::snprintf(prefix, sizeof(prefix), "REC %s #%u vs first: ", who, handle & 0xfffff);
        const std::string diff = changes(baselineNames, d.classes);
        std::vector<std::string> parts;
        for (size_t at = 0; at < diff.size();) {
            const size_t end = diff.find(',', at);
            parts.push_back(diff.substr(at, end == std::string::npos ? std::string::npos : end - at));
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
        for (const auto& line : lines(prefix, parts))
            say(line);
    }
    // The words of the components that decide how it blocks, takes hits and reacts.
    for (size_t i = 0; i < d.classes.size(); ++i) {
        if (!dumped(d.classes[i]))
            continue;
        uint8_t bytes[0x100]{};
        if (!read(d.addresses[i], bytes, sizeof(bytes)))
            continue;
        const std::string text = join(words(bytes, sizeof(bytes)));
        bool same = false;
        if (first)
            baselineWords.emplace_back(d.classes[i], text);
        else
            for (const auto& [n, t] : baselineWords)
                same |= n == d.classes[i] && t == text;
        if (same)
            continue;
        std::snprintf(prefix, sizeof(prefix), "REC %s #%u %s: ", who, handle & 0xfffff, d.classes[i].c_str());
        std::vector<std::string> tokens;
        for (const auto& w : words(bytes, sizeof(bytes)))
            tokens.push_back(w);
        for (const auto& line : lines(prefix, tokens, 175))
            say(line);
    }
}

struct Counts {
    uint64_t total{}, heroHits{}, onHero{}, mods{}, others{};
} counts, reported;
uint32_t hitSerial{};
int modLogged{};

// A request read again a moment later, for what its processing added.
struct Late {
    uintptr_t request{};
    uint32_t serial{};
    LONGLONG at{};
    uint8_t first[requestBytes]{};
};
constexpr int lateSlots = 8;
Late lates[lateSlots];
int lateNext{};
// The masks of fields already dumped raw, and how many raw dumps in all.
uint64_t rawMasks[48];
int rawMaskCount{}, rawLogged{};
// Hits on the hero by what they are (the hash, type, knockback, flags, amount), counted, not repeated.
struct Seen {
    uint64_t key{};
    uint32_t count{};
};
Seen heroSeen[24];
// The hit points of those hit lately, watched for loss no recorded hit explains.
struct Watched {
    uint32_t handle{}, serial{};
    uintptr_t record{}, health{};
    float hp{};
    LONGLONG hitAt{}, until{};
};
Watched watched[16];
int watchNext{};

void dumpRaw(uint32_t serial, const char* what, const uint8_t* bytes, const Request& r) {
    bool newMask = true;
    for (int i = 0; i < rawMaskCount; ++i)
        newMask &= rawMasks[i] != r.fields;
    if (rawLogged >= 40 && !(newMask && rawMaskCount < 48))
        return;
    if (newMask && rawMaskCount < 48)
        rawMasks[rawMaskCount++] = r.fields;
    ++rawLogged;
    char prefix[48];
    std::snprintf(prefix, sizeof(prefix), "REC raw %u %s: ", serial, what);
    for (const auto& line : lines(prefix, words(bytes, requestBytes), 175))
        say(line);
}

void process(const Pending& p) {
    uint8_t bytes[requestBytes]{};
    if (!read(p.request, bytes, sizeof(bytes)))
        return;
    const Request r = parse(bytes);
    ++counts.total;
    if (p.own) {
        ++counts.mods;
        if (modLogged < 8) {
            ++modLogged;
            say("REC mod request to #" + std::to_string(p.victim & 0xfffff) + " " + values(r) +
                (r.hasDamager ? "" : " (no damager)"));
            dumpRaw(1000 + modLogged, "mod", bytes, r);
        }
        return;
    }
    const uint32_t heroIndex = indexOfRecord(static_cast<uintptr_t>(heroRecord.load()));
    const bool fromHero = r.hasDamager && (r.damager & 0xfffff) == heroIndex && heroIndex != 0xfffff;
    const bool onHero = (p.victim & 0xfffff) == heroIndex && heroIndex != 0xfffff;
    if (fromHero) {
        ++counts.heroHits;
        const uint32_t serial = ++hitSerial;
        const uintptr_t victim = recordOf(p.victim);
        game_targets::Description before;
        game_targets::describe(base, victim, true, before);
        listOnce("enemy", p.victim, victim);
        char head[64];
        std::snprintf(head, sizeof(head), "REC hit %u on #%u hp %s%.1f: ", serial, p.victim & 0xfffff,
                      before.hasHealth ? "" : "? ", before.hasHealth ? before.hitPoints : 0.f);
        say(std::string(head) + values(r));
        // What the victim's classes are against those he was first listed with: a block or a stagger is a state.
        if (const Known* k = knownOf(p.victim)) {
            const std::string diff = changes(k->names, before.classes);
            std::snprintf(head, sizeof(head), "REC   hit %u state vs first sight: ", serial);
            say(std::string(head) + (diff.empty() ? "none" : diff.substr(0, 140)));
        }
        dumpRaw(serial, "hero", bytes, r);
        if (rawLogged <= 40 || lateNext < 6) {
            auto& l = lates[lateNext++ % lateSlots];
            l.request = p.request;
            l.serial = serial;
            l.at = now();
            std::memcpy(l.first, bytes, sizeof(bytes));
        }
        auto& f = follows[followNext++ % followSlots];
        f = {serial, p.victim, now(), before.hitPoints, before.hasHealth, join(before.classes)};
        f.names = before.classes;
        if (before.hasHealth) {
            Watched* slot{};
            for (auto& w : watched)
                if (w.handle == p.victim)
                    slot = &w;
            if (!slot)
                slot = &watched[watchNext++ % 16];
            *slot = {p.victim, serial, victim, before.health, before.hitPoints, now(), now() + 6 * frequency.QuadPart};
        }
    } else if (onHero) {
        ++counts.onHero;
        const uintptr_t attacker = r.hasDamager ? recordOf(r.damager) : 0;
        if (attacker)
            listOnce("attacker", r.damager, attacker);
        uint64_t key = r.hash;
        key = key * 31 + r.type;
        key = key * 31 + static_cast<uint32_t>(r.knockback);
        key = key * 31 + r.hitFlags;
        uint32_t amountBits{};
        std::memcpy(&amountBits, &r.amount, 4);
        key = key * 31 + amountBits;
        Seen* seen{};
        for (auto& e : heroSeen)
            if (e.key == key)
                seen = &e;
        if (!seen)
            for (auto& e : heroSeen)
                if (!e.key) {
                    e.key = key;
                    seen = &e;
                    break;
                }
        const uint32_t count = seen ? ++seen->count : 1;
        if (count <= 3 || count % 20 == 0) {
            char head[96];
            std::snprintf(head, sizeof(head), "REC hit on hero from #%u (x%u): ",
                          r.hasDamager ? r.damager & 0xfffff : 0u, count);
            say(std::string(head) + values(r));
            if (count == 1) {
                dumpRaw(2000 + static_cast<uint32_t>(counts.onHero), "on hero", bytes, r);
                if (attacker) {
                    game_targets::Description classes;
                    game_targets::describe(base, r.hasDamager ? attacker : 0, true, classes);
                    if (const Known* k = knownOf(r.damager)) {
                        const std::string diff = changes(k->names, classes.classes);
                        say("REC   attacker state vs first sight: " + (diff.empty() ? std::string("none") : diff.substr(0, 140)));
                    }
                }
            }
        }
    } else {
        ++counts.others;
    }
}
// A request read again: what its processing changed.
void late(const Late& l) {
    uint8_t bytes[requestBytes]{};
    if (!read(l.request, bytes, sizeof(bytes)))
        return;
    std::vector<std::string> changed;
    for (uint32_t at = 0; at + 4 <= requestBytes; at += 4)
        if (std::memcmp(bytes + at, l.first + at, 4)) {
            uint32_t a{}, b{};
            std::memcpy(&a, l.first + at, 4);
            std::memcpy(&b, bytes + at, 4);
            if (at % 8 == 4 || at % 8 == 0) {
                // A pointer's halves are not worth a line.
                uint32_t hi{};
                std::memcpy(&hi, bytes + (at | 4), 4);
                if (at % 8 == 0 && hi >= 1 && hi <= 0x7fff && b >= 0x10000)
                    continue;
                if (at % 8 == 4 && b >= 1 && b <= 0x7fff)
                    continue;
            }
            char text[40];
            std::snprintf(text, sizeof(text), "%x:%x>%x", at, a, b);
            changed.push_back(text);
        }
    char prefix[48];
    std::snprintf(prefix, sizeof(prefix), "REC raw %u later: ", l.serial);
    if (changed.empty())
        say(std::string(prefix) + "unchanged");
    else
        for (const auto& line : lines(prefix, changed, 175))
            say(line);
}
// A hit's outcome a quarter of a second on: the victim's hit points and what its classes gained or lost.
void settle(const Follow& f) {
    const uintptr_t victim = recordOf(f.victim);
    game_targets::Description after;
    char head[96];
    if (!victim || !game_targets::describe(base, victim, true, after)) {
        std::snprintf(head, sizeof(head), "REC after %u: the victim is gone", f.serial);
        say(head);
        return;
    }
    char hp[48] = "hp ?";
    if (f.hasBefore && after.hasHealth)
        std::snprintf(hp, sizeof(hp), "hp %.1f -> %.1f (%s)", f.before, after.hitPoints,
                      after.hitPoints < f.before - 0.01f ? "lost" : "NOT LOST");
    const std::string diff = changes(f.names, after.classes);
    std::snprintf(head, sizeof(head), "REC after %u: %s; classes: ", f.serial, hp);
    say(std::string(head) + (diff.empty() ? "unchanged" : diff.substr(0, 120)));
}
// Hit points that moved with no recorded hit in the last 300 ms: damage that does not come through the getter.
void watch() {
    for (auto& w : watched) {
        if (!w.handle || now() > w.until) {
            w = {};
            continue;
        }
        if (value<uintptr_t>(w.health + 8) != w.record)
            continue;
        const float hp = value<float>(w.health + 0x88);
        if (!std::isfinite(hp) || std::fabs(hp - w.hp) < 0.01f)
            continue;
        const bool explained = secondsSince(w.hitAt) < .3 + .0;
        char text[160];
        std::snprintf(text, sizeof(text), "REC hp #%u %.1f -> %.1f, %s (hit %u was %.0f ms ago)",
                      w.handle & 0xfffff, w.hp, hp, explained ? "within 300 ms of a recorded hit" : "NO RECORDED HIT",
                      w.serial, secondsSince(w.hitAt) * 1000);
        say(text);
        w.hp = hp;
    }
}

DWORD WINAPI run(void*) {
    LONGLONG lastSummary = now(), lastWatch = now();
    while (!stopping.load()) {
        Sleep(1);
        Pending due[pendingSlots];
        int dueCount{};
        AcquireSRWLockExclusive(&pendingLock);
        for (auto& p : pending)
            if (p.request && secondsSince(p.at) >= .003) {
                due[dueCount++] = p;
                p = {};
            }
        ReleaseSRWLockExclusive(&pendingLock);
        for (int i = 0; i < dueCount; ++i)
            process(due[i]);
        for (auto& l : lates)
            if (l.request && secondsSince(l.at) >= .014) {
                const Late copy = l;
                l.request = 0;
                late(copy);
            }
        for (auto& f : follows)
            if (f.serial && secondsSince(f.at) >= .25) {
                const Follow copy = f;
                f = {};
                settle(copy);
            }
        if (secondsSince(lastWatch) >= .04) {
            lastWatch = now();
            watch();
        }
        if (secondsSince(lastSummary) >= 10) {
            lastSummary = now();
            if (counts.total != reported.total || !reported.total) {
                char text[160];
                std::snprintf(text, sizeof(text),
                              counts.total ? "REC requests seen: %llu (the hero's %llu, on the hero %llu, the mod's %llu, "
                                             "others %llu)"
                                           : "REC no damage request seen yet (%llu)",
                              static_cast<unsigned long long>(counts.total),
                              static_cast<unsigned long long>(counts.heroHits),
                              static_cast<unsigned long long>(counts.onHero),
                              static_cast<unsigned long long>(counts.mods),
                              static_cast<unsigned long long>(counts.others));
                say(text);
                reported = counts;
            }
        }
    }
    return 0;
}

uint8_t* __fastcall getterHook(void* system, const uint32_t* victim, const Vec3* direction, const Vec3* point,
                               const Vec3* normal) {
    ++active;
    uint8_t* request = original(system, victim, direction, point, normal);
    if (enabled.load(std::memory_order_relaxed) && request && victim) {
        AcquireSRWLockExclusive(&pendingLock);
        pending[pendingNext++ % pendingSlots] = {reinterpret_cast<uintptr_t>(request), *victim, now(), issuingDepth > 0};
        ReleaseSRWLockExclusive(&pendingLock);
    }
    --active;
    return request;
}
} // namespace

uint32_t game_record::start(uintptr_t imageBase) {
    AcquireSRWLockExclusive(&lifecycle);
    uint32_t result{};
    do {
        if (enabled)
            break;
        base = imageBase;
        if (!entry(getterRva, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24,
                               0x18, 0x48})) {
            result = 9301;
            break;
        }
        if (!hooked) {
            const auto s = MH_Initialize();
            if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
                result = 9310 + s;
                break;
            }
            hook = reinterpret_cast<void*>(base + getterRva);
            if (const auto c = MH_CreateHook(hook, reinterpret_cast<void*>(getterHook),
                                             reinterpret_cast<void**>(&original));
                c != MH_OK) {
                result = 9320 + c;
                break;
            }
            hooked = true;
        }
        QueryPerformanceFrequency(&frequency);
        AcquireSRWLockExclusive(&pendingLock);
        for (auto& p : pending)
            p = {};
        ReleaseSRWLockExclusive(&pendingLock);
        for (auto& f : follows)
            f = {};
        known.clear();
        baselineNames.clear();
        baselineWords.clear();
        rawLogged = rawMaskCount = 0;
        for (auto& l : lates)
            l = {};
        for (auto& w : watched)
            w = {};
        for (auto& e : heroSeen)
            e = {};
        counts = reported = {};
        modLogged = 0;
        stopping = false;
        enabled = true;
        if (const auto s = MH_EnableHook(hook); s != MH_OK && s != MH_ERROR_ENABLED) {
            enabled = false;
            result = 9340 + s;
            break;
        }
        worker = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
        if (!worker) {
            enabled = false;
            MH_DisableHook(hook);
            result = 9350;
            break;
        }
        say("REC the hit recorder is running: punch with the game's own punches (a controller) to record them");
    } while (false);
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}

void game_record::stop() {
    AcquireSRWLockExclusive(&lifecycle);
    enabled = false;
    stopping = true;
    if (hooked)
        MH_DisableHook(hook);
    const auto until = GetTickCount64() + 2000;
    while (active.load() && GetTickCount64() < until)
        Sleep(1);
    if (worker) {
        WaitForSingleObject(worker, 2000);
        CloseHandle(worker);
        worker = nullptr;
    }
    ReleaseSRWLockExclusive(&lifecycle);
}

void game_record::hero(uint64_t record) {
    heroRecord.store(record, std::memory_order_relaxed);
}

void game_record::issuing(bool on) {
    issuingDepth += on ? 1 : (issuingDepth > 0 ? -1 : 0);
}

void game_record::log(const char* text) {
    if (text)
        say(text);
}

bool game_record::note(uint32_t& seen, char* out, size_t size) {
    const uint32_t serial = noteSerial.load(std::memory_order_acquire);
    if (serial == seen || !out || !size)
        return false;
    if (serial - seen > noteSlots)
        seen = serial - noteSlots;
    const uint32_t written = noteSeq[seen % noteSlots].load(std::memory_order_acquire);
    if (written != seen + 1) {
        if (written > seen + 1)
            ++seen;
        return false;
    }
    std::snprintf(out, size, "%s", noteText[seen % noteSlots]);
    ++seen;
    return true;
}

extern "C" DWORD WINAPI SpidyRecordNote(void* request) {
    if (!request)
        return 1;
    auto* n = static_cast<Note*>(request);
    return game_record::note(n->seen, n->text, sizeof(n->text)) ? 0 : 1;
}
extern "C" DWORD WINAPI SpidyRecordStart(void*) {
    return game_record::start(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
}
extern "C" DWORD WINAPI SpidyRecordStop(void*) {
    game_record::stop();
    return 0;
}

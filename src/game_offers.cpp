// The game's own interactions where Spidy's own wall holds the player
// (game_offers.hpp). Addresses are image offsets in the supported
// Spider-Man.exe.
#include "spidy/game_offers.hpp"
#include "MinHook.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <windows.h>
using namespace spidy;
using namespace spidy::game_offers;
extern "C" {
__declspec(dllexport) Data SpidyOffersData;
}
namespace {
// Hero::HeroOfferProcessor's update, the mask it reads and its eight
// candidate slots (0x18 each from +0x48, the offer's handle at +0x14);
// Offer::IsAvailableTo; VentOffer's own entry check (its vtable +0x128).
constexpr uintptr_t updateRva = 0x9e1320, availableRva = 0x237c980, ventCheckRva = 0x817d40;
// MilesVR: the mask is +1d0 in MilesMorales.exe (the gather, 9df330, reads it there; Spider-Man.exe's
// 855460 at +1c8). Miles Morales put a pointer at +1c8: writing the mask there, as 0.1.3 did on a
// wall, broke it, and the update (9e1a20) crashed reading through it.
constexpr uintptr_t maskOffset = 0x1d0, slotsOffset = 0x48, slotBytes = 0x18, slotHandle = 0x14;
// The handle of the offer it picked (SetBest, 8571c0, writes it).
constexpr uintptr_t bestOffset = 0x1ac;
// An offer: its own handle (+0x14), its kind (+0xa8), whether it is on
// (+0xb5), its two reasons (+0xf4, +0xf8) and the one its latest check filled
// (+0xfc), and its actor (+8, whose first word is its transform).
constexpr uintptr_t handleOffset = 0x14, kindOffset = 0xa8, enabledOffset = 0xb5, reasonsOffset = 0xf4,
                    currentOffset = 0xfc;
// The game's component registry: 16-byte entries of an address and a
// generation; a handle is its generation over 20 bits of index.
constexpr uintptr_t registryTable = 0x83f9fa0, registryCount = 0x83f9fc0;
// The part of the hero's actor record an offer's check reads: its transform
// pointer first, its components' lists at +0x58 and +0x80.
constexpr size_t recordBytes = 0x200;
constexpr uint64_t holdMs = 200, useMs = 500;
// The range reason of the offers' check: the one a spot can answer.
constexpr uint32_t rangeReason = 4;
using Update = void(__fastcall*)(void* processor, float seconds);
using Available = bool(__fastcall*)(void* offer, void* query);
using VentCheck = bool(__fastcall*)(void* offer, void* query);
uintptr_t base{};
Update originalUpdate{};
Available originalAvailable{};
VentCheck originalVentCheck{};
void* hooks[3]{};
bool hooked{};
std::atomic<bool> enabled{};
std::atomic<uint32_t> forced{};
// The swing's latest word: whose record, until when its wall holds him, and
// where (GetTickCount64). A probe's nudge of that place (SpidyOffersNudge): a
// probe's hero cannot walk Spidy's wall the way a player does.
struct Held {
    uintptr_t record{};
    uint64_t until{};
    Vec3 point{}, normal{};
};
Held latest{};
Vec3 nudge{};
// A use the headset asked for, when, and where the crawl goes for it: the
// offer's spot, or (no spot) where the offers saw the player.
uint32_t pendingUse{};
uint64_t pendingAt{};
bool pendingAtSpot{};
Vec3 pendingSpot{};
// The offer the headset aims at.
uint32_t aimed{};
// Game thread only: the offers the player's gather checked during its latest
// masked update, and the spot search's last answer (searched again when the
// aim or the player's place changes).
uintptr_t seen[2 * maxOffers]{};
unsigned seenCount{};
// The thread running it (0 none): the offers' check is the game's, and other
// threads may call it meanwhile.
std::atomic<DWORD> gathering{};
struct Search {
    uint32_t handle{};
    Vec3 from{}, spot{};
    bool found{};
} search{};
SRWLOCK lifecycle = SRWLOCK_INIT, output = SRWLOCK_INIT, word = SRWLOCK_INIT;

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
bool write(uintptr_t p, const void* in, size_t n) {
    __try {
        if (p < 0x10000)
            return false;
        std::memcpy(reinterpret_cast<void*>(p), in, n);
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
    auto& d = SpidyOffersData;
    InterlockedIncrement64(&d.sequence);
    change(d);
    InterlockedIncrement64(&d.sequence);
    ReleaseSRWLockExclusive(&output);
}
// Whether Spidy's wall holds the player now, and where.
bool held(Held* out = nullptr) {
    AcquireSRWLockShared(&word);
    Held h = latest;
    h.point = h.point + nudge;
    ReleaseSRWLockShared(&word);
    if (out)
        *out = h;
    const auto f = forced.load();
    if (f == 2)
        return false;
    return f == 1 || GetTickCount64() < h.until;
}
uintptr_t player() {
    AcquireSRWLockShared(&word);
    const auto record = latest.record;
    ReleaseSRWLockShared(&word);
    return record;
}
// The object a handle names in the game's registry (its own decode, 16798f0).
uintptr_t resolve(uint32_t handle) {
    const uint32_t generation = handle >> 20, index = handle & 0xfffff;
    int32_t count{};
    const auto table = pointer(base + registryTable);
    if (!generation || !table || !read(base + registryCount, &count, 4) || index >= static_cast<uint32_t>(count))
        return 0;
    uint32_t have{};
    if (!read(table + index * 16ull + 8, &have, 4) || have != generation)
        return 0;
    return pointer(table + index * 16ull);
}
// An offer as the game holds it: its handle, kind, the game's latest answer
// and where it is.
bool describe(uintptr_t offer, Offer& o) {
    float m[16]{};
    uint32_t handle{}, kind{}, reasons[2]{};
    uint8_t on{}, current{};
    const auto transform = pointer(pointer(offer + 8));
    if (!offer || !read(offer + handleOffset, &handle, 4) || !handle || !read(offer + kindOffset, &kind, 4) ||
        !read(offer + enabledOffset, &on, 1) || !read(offer + reasonsOffset, reasons, 8) ||
        !read(offer + currentOffset, &current, 1) || !read(transform, m, sizeof(m)))
        return false;
    o = {};
    o.handle = handle;
    o.kind = kind;
    o.reasons = reasons[current ? 1 : 0];
    o.enabled = on;
    o.position = {m[12], m[13], m[14]};
    o.axis = {m[8], m[9], m[10]};
    return true;
}
// The hero as the game's crawl would hold him where Spidy's wall does: his
// actor's axes turned so that its up is the wall's normal, his feet on the
// wall under his body.
void crawlPose(float m[16], Vec3 point, Vec3 normal) {
    const Vec3 up = normalized(Vec3{m[4], m[5], m[6]});
    const Vec3 n = normalized(normal);
    if (length(up) < .5f || length(n) < .5f)
        return;
    const Quat turn = arc(up, n);
    for (int row = 0; row < 3; ++row) {
        const Vec3 axis = turn.rotate({m[row * 4], m[row * 4 + 1], m[row * 4 + 2]});
        m[row * 4] = axis.x;
        m[row * 4 + 1] = axis.y;
        m[row * 4 + 2] = axis.z;
    }
    m[12] = point.x;
    m[13] = point.y;
    m[14] = point.z;
}
// A copy of the hero's record whose transform is the crawl's pose at `point`:
// what an offer's check reads. The record itself is never written.
struct Posed {
    alignas(16) uint8_t record[recordBytes]{};
    alignas(16) float pose[16]{};
};
bool pose(uintptr_t record, Vec3 point, Vec3 normal, Posed& p) {
    uintptr_t transform{};
    if (!finite(point) || !finite(normal) || !read(record, p.record, recordBytes) || !read(record, &transform, 8) ||
        !read(transform, p.pose, sizeof(p.pose)))
        return false;
    crawlPose(p.pose, point, normal);
    const auto fake = reinterpret_cast<uintptr_t>(p.pose);
    std::memcpy(p.record, &fake, sizeof(fake));
    return true;
}
// A spot on the wall near an offer where the game says yes to it for the
// crawl's pose, within spotReach of the player: the offer laid onto the wall,
// and points below it, beside it and above it, the nearest to him first. The
// game's own crawl stands a hero at a vent with his feet 1.1 m below its
// middle (the first mission's, October 15).
bool findSpot(uintptr_t offer, const Held& h, Vec3& spot) {
    float m[16]{};
    if (!read(pointer(pointer(offer + 8)), m, sizeof(m)))
        return false;
    const Vec3 at{m[12], m[13], m[14]};
    const Vec3 n = normalized(h.normal);
    const Vec3 up = normalized(Vec3{0, 1, 0} - n * n.y);
    if (!finite(at) || !finite(n) || length(n) < .5f || !finite(up) || length(up) < .5f)
        return false;
    const Vec3 side = normalized(cross(n, up));
    const Vec3 onWall = at - n * dot(at - h.point, n);
    Vec3 tries[24]{};
    unsigned count{};
    for (const float u : {-1.1f, -.7f, -.35f, 0.f, .35f, .7f})
        for (const float s : {0.f, -.5f, .5f, -1.f})
            if (const Vec3 p = onWall + up * u + side * s; length(p - h.point) <= spotReach)
                tries[count++] = p;
    std::sort(tries, tries + count,
              [&](Vec3 a, Vec3 b) { return length(a - h.point) < length(b - h.point); });
    Posed p;
    for (unsigned i = 0; i < count; ++i)
        if (pose(h.record, tries[i], n, p) && originalAvailable(reinterpret_cast<void*>(offer), p.record)) {
            spot = tries[i];
            return true;
        }
    return false;
}
// The player's processor's update, with the crawl's mask while Spidy's wall
// holds him, and the game's own mask back as soon as it is done; then what it
// gathered, and every offer it checked for him.
void __fastcall update(void* processor, float seconds) {
    const auto self = reinterpret_cast<uintptr_t>(processor);
    const auto at = self + maskOffset;
    uint32_t mask{};
    Held h;
    const bool mine = enabled.load(std::memory_order_relaxed) && player() && pointer(self + 8) == player();
    const bool readable = mine && read(at, &mask, 4);
    const bool holding = mine && held(&h) && h.record && finite(h.point) && finite(h.normal);
    const bool masked = readable && holding && mask != crawlMask && write(at, &crawlMask, 4);
    seenCount = 0;
    gathering = holding ? GetCurrentThreadId() : 0;
    originalUpdate(processor, seconds);
    gathering = 0;
    if (masked)
        write(at, &mask, 4);
    if (!mine)
        return;
    // Its own candidates first, then every other offer it checked for him.
    Offer list[maxOffers]{};
    uintptr_t where[maxOffers]{};
    uint32_t count{};
    for (uintptr_t k = 0; k < maxOffers && count < maxOffers; ++k) {
        uint32_t handle{};
        if (!read(self + slotsOffset + k * slotBytes + slotHandle, &handle, 4) || !handle)
            continue;
        if (const auto offer = resolve(handle); offer && describe(offer, list[count]))
            where[count++] = offer;
    }
    const unsigned checked = seenCount;
    for (unsigned i = 0; i < seenCount && count < maxOffers; ++i) {
        Offer o;
        bool known{};
        for (uint32_t k = 0; k < count; ++k)
            known |= where[k] == seen[i];
        if (!known && describe(seen[i], o)) {
            list[count] = o;
            where[count++] = seen[i];
        }
    }
    // The aimed offer out of his reach: a spot within it, searched again only
    // when the aim or his place has changed.
    AcquireSRWLockShared(&word);
    const uint32_t aim = aimed;
    ReleaseSRWLockShared(&word);
    bool searched{};
    for (uint32_t k = 0; k < count && holding; ++k) {
        auto& o = list[k];
        if (o.handle != aim || !o.enabled || o.reasons != rangeReason)
            continue;
        if (search.handle != aim || length(search.from - h.point) > .2f) {
            search = {aim, h.point};
            search.found = findSpot(where[k], h, search.spot);
            searched = true;
        }
        o.reachable = search.found;
    }
    uint32_t best{};
    read(self + bestOffset, &best, 4);
    publish([&](Data& d) {
        ++d.updates;
        d.masked += masked;
        if (readable)
            d.mask = mask;
        d.offers = count;
        d.best = best;
        std::memcpy(d.nearby, list, sizeof(list));
        d.gathered += holding && checked;
        d.mostChecked = std::max(d.mostChecked, checked);
        d.aimed = aim;
        d.spotFound = holding && search.handle == aim && search.found;
        d.spot = search.spot;
        d.searches += searched;
        d.spotsFound += searched && search.found;
    });
}
// An offer's check for the player: against a copy of his record whose
// transform is the crawl's pose while Spidy's wall holds him.
bool __fastcall available(void* offer, void* query) {
    Held h;
    const bool on = enabled.load(std::memory_order_relaxed) && held(&h) && h.record &&
                    reinterpret_cast<uintptr_t>(query) == h.record;
    Posed p;
    const bool posed = on && pose(h.record, h.point, h.normal, p);
    const bool result = originalAvailable(offer, posed ? p.record : query);
    // During the player's own gather every offer it checks is one near him,
    // of a kind his crawl takes, whatever the answer.
    if (posed && gathering.load() == GetCurrentThreadId() && seenCount < std::size(seen)) {
        const auto o = reinterpret_cast<uintptr_t>(offer);
        bool known{};
        for (unsigned i = 0; i < seenCount; ++i)
            known |= seen[i] == o;
        if (!known)
            seen[seenCount++] = o;
    }
    if (on)
        publish([&](Data& d) {
            ++d.checks;
            d.posed += posed;
            if (posed) {
                d.point = h.point;
                d.normal = h.normal;
            }
        });
    return result;
}
// A vent's own entry check: past the side of the vent the hero stands on, it
// wants the crawl's state on him, which Spidy's wall never is.
bool __fastcall ventCheck(void* offer, void* query) {
    const bool open = originalVentCheck(offer, query);
    const bool answered = !open && enabled.load(std::memory_order_relaxed) && held();
    publish([&](Data& d) {
        ++d.ventChecks;
        d.ventAnswered += answered;
    });
    return open || answered;
}
bool entry(uintptr_t rva, std::initializer_list<uint8_t> bytes) {
    uint8_t have[16]{};
    if (bytes.size() > sizeof(have) || !read(base + rva, have, bytes.size()))
        return false;
    // MinHook's jump in place counts as this function: a start after a stop.
    if (have[0] == 0xe9)
        return true;
    return std::memcmp(have, bytes.begin(), bytes.size()) == 0;
}
} // namespace

uint32_t game_offers::start(uintptr_t imageBase) {
    AcquireSRWLockExclusive(&lifecycle);
    uint32_t result{};
    do {
        if (enabled)
            break;
        base = imageBase;
        // The bytes each hook's jump covers: mov rax, rsp, push rbx..r15;
        // mov [rsp+8], rbx, push rdi, sub rsp, 60h; mov [rsp+8], rbx,
        // [rsp+10h], rbp, [rsp+18h], rsi.
        if (!entry(updateRva, {0x48, 0x8b, 0xc4, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56}) ||
            !entry(availableRva, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x60}) ||
            !entry(ventCheckRva,
                   {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18})) {
            result = 9601;
            break;
        }
        if (!hooked) {
            const auto s = MH_Initialize();
            if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
                result = 9700 + s;
                break;
            }
            hooks[0] = reinterpret_cast<void*>(base + updateRva);
            hooks[1] = reinterpret_cast<void*>(base + availableRva);
            hooks[2] = reinterpret_cast<void*>(base + ventCheckRva);
            void* const detours[3]{reinterpret_cast<void*>(update), reinterpret_cast<void*>(available),
                                   reinterpret_cast<void*>(ventCheck)};
            void** const originals[3]{reinterpret_cast<void**>(&originalUpdate),
                                      reinterpret_cast<void**>(&originalAvailable),
                                      reinterpret_cast<void**>(&originalVentCheck)};
            for (int i = 0; i < 3 && !result; ++i)
                if (const auto c = MH_CreateHook(hooks[i], detours[i], originals[i]); c != MH_OK) {
                    for (int j = 0; j < i; ++j)
                        MH_RemoveHook(hooks[j]);
                    result = 9710 + 10 * i + c;
                }
            if (result)
                break;
            hooked = true;
        }
        enabled = true;
        for (auto* hook : hooks)
            if (const auto s = MH_EnableHook(hook); s != MH_OK && s != MH_ERROR_ENABLED && !result)
                result = 9770 + s;
        if (result) {
            enabled = false;
            for (auto* hook : hooks)
                MH_DisableHook(hook);
        }
    } while (false);
    publish([&](Data& d) {
        d.status = result ? 3 : 1;
        d.error = result;
    });
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}
void game_offers::stop() {
    AcquireSRWLockExclusive(&lifecycle);
    enabled = false;
    if (hooked)
        for (auto* hook : hooks)
            MH_DisableHook(hook);
    AcquireSRWLockExclusive(&word);
    pendingUse = aimed = 0;
    ReleaseSRWLockExclusive(&word);
    publish([](Data& d) {
        d.status = 0;
        d.offers = 0;
        d.spotFound = 0;
    });
    ReleaseSRWLockExclusive(&lifecycle);
}
void game_offers::hold(uintptr_t record, bool onSpidysWall, Vec3 point, Vec3 normal) {
    AcquireSRWLockExclusive(&word);
    latest.record = record;
    latest.until = onSpidysWall ? GetTickCount64() + holdMs : 0;
    if (onSpidysWall) {
        latest.point = point;
        latest.normal = normal;
    }
    ReleaseSRWLockExclusive(&word);
    publish([&](Data& d) {
        d.held = held() ? 1u : 0u;
        d.forced = forced.load();
    });
}
void game_offers::aim(uint32_t handle) {
    AcquireSRWLockExclusive(&word);
    aimed = handle;
    ReleaseSRWLockExclusive(&word);
}
bool game_offers::use(uint32_t handle) {
    const auto d = data();
    bool here{}, atSpot{};
    for (uint32_t i = 0; i < d.offers && i < maxOffers; ++i) {
        const auto& o = d.nearby[i];
        if (o.handle != handle || !handle || !o.enabled)
            continue;
        here |= !o.reasons;
        atSpot |= o.reasons && o.reachable && d.spotFound && d.aimed == handle && finite(d.spot);
    }
    if (!here && !atSpot)
        return false;
    AcquireSRWLockExclusive(&word);
    pendingUse = handle;
    pendingAt = GetTickCount64();
    pendingAtSpot = !here;
    pendingSpot = d.spot;
    ReleaseSRWLockExclusive(&word);
    publish([&](Data& out) {
        ++out.usesAsked;
        out.lastUse = handle;
    });
    return true;
}
bool game_offers::takeUse(Vec3& point) {
    Held h;
    if (!held(&h) || !finite(h.point))
        return false;
    AcquireSRWLockExclusive(&word);
    const bool fresh = pendingUse && GetTickCount64() - pendingAt < useMs;
    const bool atSpot = pendingAtSpot;
    const Vec3 spot = pendingSpot;
    pendingUse = 0;
    ReleaseSRWLockExclusive(&word);
    if (!fresh)
        return false;
    point = atSpot ? spot : h.point;
    publish([](Data& d) { ++d.usesStarted; });
    return true;
}
void game_offers::usedOutcome(bool taken) {
    publish([&](Data& d) {
        d.usesTaken += taken;
        d.usesDeclined += !taken;
    });
}
Data game_offers::data() {
    AcquireSRWLockShared(&output);
    auto out = SpidyOffersData;
    ReleaseSRWLockShared(&output);
    return out;
}

extern "C" __declspec(dllexport) DWORD WINAPI SpidyOffersSample(void* out) {
    if (!out)
        return 2001;
    const auto d = game_offers::data();
    return write(reinterpret_cast<uintptr_t>(out), &d, sizeof(d)) ? 0 : 2001;
}
// The player's interact on an offer (a 32-bit handle). 0 asked; 2002 when
// the game does not offer it to him now.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyOffersUse(void* input) {
    uint32_t handle{};
    if (!input || !read(reinterpret_cast<uintptr_t>(input), &handle, 4))
        return 2001;
    return game_offers::use(handle) ? 0 : 2002;
}
// The offer a hand points at (a 32-bit handle, 0 none).
extern "C" __declspec(dllexport) DWORD WINAPI SpidyOffersAim(void* input) {
    uint32_t handle{};
    if (!input || !read(reinterpret_cast<uintptr_t>(input), &handle, 4))
        return 2001;
    game_offers::aim(handle);
    return 0;
}
// A probe's nudge of the crawl's pose, metres in the world (three floats).
extern "C" __declspec(dllexport) DWORD WINAPI SpidyOffersNudge(void* input) {
    float v[3]{};
    if (!input || !read(reinterpret_cast<uintptr_t>(input), v, sizeof(v)) || !finite(Vec3{v[0], v[1], v[2]}) ||
        length(Vec3{v[0], v[1], v[2]}) > 3)
        return 2001;
    AcquireSRWLockExclusive(&word);
    nudge = {v[0], v[1], v[2]};
    ReleaseSRWLockExclusive(&word);
    return 0;
}
// A probe's word over the swing's: 0 the swing's, 1 held, 2 not held.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyOffersForce(void* input) {
    uint32_t value{};
    if (!input || !read(reinterpret_cast<uintptr_t>(input), &value, 4) || value > 2)
        return 2001;
    forced = value;
    publish([&](Data& d) { d.forced = value; });
    return 0;
}

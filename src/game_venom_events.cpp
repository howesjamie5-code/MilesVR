// The game's venom bar events, watched (game_venom_events.hpp).
#include "spidy/game_venom_events.hpp"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <windows.h>
#include <psapi.h>
using namespace spidy;
namespace {
constexpr unsigned stackFrames = 12, mostStacks = 6;
// One way the game made an event: its call stack (offsets in the game's image;
// 1 for a frame in another module), how many times, and the meter the first
// and the latest time.
struct Stack {
    uint32_t frames[stackFrames]{};
    uint32_t times{};
    float meterFirst = -1, meterLast = -1;
};
struct Kind {
    uint64_t events{};
    float meterLast = -1;
    uint32_t stacks{}, unlisted{}, spare{};
    Stack listed[mostStacks]{};
};
} // namespace
// For the session report: plain words; sequence is odd while it is written.
struct VenomEventData {
    uint32_t magic = 0x53564e45, version = 1, bytes = sizeof(VenomEventData), installed{};
    uint32_t status{}, reserved{};
    uint64_t sequence{};
    Kind kinds[game_venom_events::count]{};
};
static_assert(sizeof(Stack) == 60 && sizeof(Kind) == 384 && sizeof(VenomEventData) == 1184);
extern "C" {
__declspec(dllexport) VenomEventData SpidyVenomEventData;
}
namespace {
// The constructors: UIVenomBarFillEvent's, UIVenomBarMaxFillEvent's,
// UIVenomBarUsedEvent's; the type table's first. Each is push rbx; sub rsp,20h;
// mov rbx,rcx; call (the UI event's base constructor).
constexpr std::array<uintptr_t, 6> constructors{0x179d670, 0x17a2660, 0x179ca90, 0x17a2690, 0x179e7e0, 0x17a26c0};
constexpr unsigned char prologue[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8};
using Construct = void*(__fastcall*)(void*);
std::array<Construct, 6> originals{};
uintptr_t imageBase{}, imageSize{};
std::atomic<bool> hooked{};
std::atomic<uint32_t> status{};
std::atomic<float> meterNow{-1.f};
SRWLOCK lifecycle = SRWLOCK_INIT, recording = SRWLOCK_INIT;

bool read(uintptr_t p, void* out, size_t n) {
    __try {
        if (p < 0x10000)
            return false;
        std::memcpy(out, reinterpret_cast<void*>(p), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void record(unsigned kind) {
    void* raw[stackFrames + 1]{};
    // Past this hook's own frame.
    const USHORT got = RtlCaptureStackBackTrace(1, stackFrames, raw, nullptr);
    uint32_t frames[stackFrames]{};
    for (USHORT i = 0; i < got; ++i) {
        const auto p = reinterpret_cast<uintptr_t>(raw[i]);
        frames[i] = p >= imageBase && p < imageBase + imageSize ? static_cast<uint32_t>(p - imageBase) : 1;
    }
    const float m = meterNow.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&recording);
    auto& d = SpidyVenomEventData;
    d.sequence |= 1;
    auto& k = d.kinds[kind];
    ++k.events;
    k.meterLast = m;
    Stack* found{};
    for (uint32_t i = 0; i < k.stacks && !found; ++i)
        if (!std::memcmp(k.listed[i].frames, frames, sizeof(frames)))
            found = &k.listed[i];
    if (!found && k.stacks < mostStacks) {
        found = &k.listed[k.stacks++];
        std::memcpy(found->frames, frames, sizeof(frames));
        found->meterFirst = m;
    }
    if (found) {
        ++found->times;
        found->meterLast = m;
    } else {
        ++k.unlisted;
    }
    ++d.sequence;
    ReleaseSRWLockExclusive(&recording);
}
template <unsigned I> void* __fastcall construct(void* self) {
    void* const made = originals[I](self);
    if (hooked.load(std::memory_order_relaxed))
        record(I / 2);
    return made;
}
constexpr std::array<Construct, 6> hooks{&construct<0>, &construct<1>, &construct<2>,
                                         &construct<3>, &construct<4>, &construct<5>};
} // namespace

uint32_t game_venom_events::install(uintptr_t base) {
    AcquireSRWLockExclusive(&lifecycle);
    uint32_t result{};
    do {
        if (hooked)
            break;
        MODULEINFO info{};
        if (!base || !GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(base), &info, sizeof(info))) {
            result = 9701;
            break;
        }
        bool supported = true;
        for (const auto rva : constructors) {
            unsigned char found[sizeof(prologue)]{};
            supported &= read(base + rva, found, sizeof(found)) && !std::memcmp(found, prologue, sizeof(found));
        }
        if (!supported) {
            result = 9701;
            break;
        }
        const auto started = MH_Initialize();
        if (started != MH_OK && started != MH_ERROR_ALREADY_INITIALIZED) {
            result = 9710 + started;
            break;
        }
        imageBase = base;
        imageSize = info.SizeOfImage;
        size_t made = 0;
        for (; made < constructors.size(); ++made) {
            const auto target = reinterpret_cast<void*>(base + constructors[made]);
            if (const auto created = MH_CreateHook(target, reinterpret_cast<void*>(hooks[made]),
                                                   reinterpret_cast<void**>(&originals[made]));
                created != MH_OK) {
                result = 9730 + created;
                break;
            }
        }
        if (!result)
            for (size_t i = 0; i < constructors.size(); ++i)
                if (const auto enabled = MH_EnableHook(reinterpret_cast<void*>(base + constructors[i]));
                    enabled != MH_OK) {
                    result = 9760 + enabled;
                    break;
                }
        if (result) {
            for (size_t i = 0; i < made; ++i) {
                MH_DisableHook(reinterpret_cast<void*>(base + constructors[i]));
                MH_RemoveHook(reinterpret_cast<void*>(base + constructors[i]));
            }
            break;
        }
        hooked = true;
    } while (false);
    status = result;
    SpidyVenomEventData.installed = hooked;
    SpidyVenomEventData.status = result;
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}
void game_venom_events::meter(float value) {
    meterNow.store(value, std::memory_order_relaxed);
}

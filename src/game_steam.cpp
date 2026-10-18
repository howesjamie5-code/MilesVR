// The guard on the game's Steam ticket call (game_steam.hpp).
#include "spidy/game_steam.hpp"
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <windows.h>
#include <psapi.h>
using namespace spidy;
// The guard's telemetry for the session report: plain words, each written whole.
struct SteamGuardData {
    uint32_t magic = 0x53544b54, version = 1, bytes = sizeof(SteamGuardData), installed{};
    uint32_t status{}, reserved{};
    uint64_t calls{}, fromGame{}, cleared{}, lastIdentity{};
};
static_assert(sizeof(SteamGuardData) == 56);
extern "C" {
__declspec(dllexport) SteamGuardData SpidySteamData;
}
namespace {
// The game's context for its Steam user interface (the interface pointer is
// its third word), the call that asks for the ticket, and where it returns.
constexpr uintptr_t userContext = 0x5d69010, ticketCall = 0x2aa63f7, ticketReturn = 0x2aa6400;
constexpr unsigned ticketSlot = 13;
using Ticket = uint32_t(__fastcall*)(void*, void*, int, uint32_t*, const void*);
Ticket original{};
uintptr_t imageBase{};
std::atomic<bool> hooked{};
std::atomic<uint32_t> status{};
std::atomic<uint64_t> calls{}, fromGame{}, cleared{}, lastIdentity{};
SRWLOCK lifecycle = SRWLOCK_INIT;
void publish() {
    SpidySteamData.installed = hooked;
    SpidySteamData.status = status;
    SpidySteamData.calls = calls;
    SpidySteamData.fromGame = fromGame;
    SpidySteamData.cleared = cleared;
    SpidySteamData.lastIdentity = lastIdentity;
}
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
bool within(const wchar_t* module, uintptr_t p) {
    const HMODULE handle = GetModuleHandleW(module);
    MODULEINFO info{};
    if (!handle || !GetModuleInformation(GetCurrentProcess(), handle, &info, sizeof(info)))
        return false;
    const auto start = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    return p >= start && p < start + info.SizeOfImage;
}
uint32_t __fastcall ticket(void* self, void* buffer, int size, uint32_t* written, const void* identity) {
    ++calls;
    // The game passes no identity there: what arrives as one is its stack's.
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == imageBase + ticketReturn) {
        ++fromGame;
        if (identity) {
            ++cleared;
            lastIdentity = reinterpret_cast<uintptr_t>(identity);
            identity = nullptr;
        }
    }
    publish();
    return original(self, buffer, size, written, identity);
}
} // namespace

uint32_t game_steam::install(uintptr_t base) {
    AcquireSRWLockExclusive(&lifecycle);
    uint32_t result{};
    do {
        if (hooked)
            break;
        // mov rcx,[rax]; mov rax,[rcx]; call [rax+68h]
        const unsigned char expected[] = {0x48, 0x8b, 0x08, 0x48, 0x8b, 0x01, 0xff, 0x50, 0x68};
        unsigned char found[sizeof(expected)]{};
        if (!base || !read(base + ticketCall, found, sizeof(found)) || std::memcmp(found, expected, sizeof(found))) {
            result = 9802;
            break;
        }
        uintptr_t user{}, table{}, function{};
        if (!read(base + userContext, &user, sizeof(user)) || !user) {
            result = 9801; // the game has not asked Steam for it yet
            break;
        }
        if (!read(user, &table, sizeof(table)) || !within(L"steamclient64.dll", table)) {
            result = 9803;
            break;
        }
        if (!read(table + ticketSlot * sizeof(void*), &function, sizeof(function)) ||
            !within(L"steamclient64.dll", function)) {
            result = 9804;
            break;
        }
        const auto started = MH_Initialize();
        if (started != MH_OK && started != MH_ERROR_ALREADY_INITIALIZED) {
            result = 9810 + started;
            break;
        }
        imageBase = base;
        const auto target = reinterpret_cast<void*>(function);
        if (const auto created = MH_CreateHook(target, reinterpret_cast<void*>(&ticket),
                                               reinterpret_cast<void**>(&original));
            created != MH_OK) {
            result = 9830 + created;
            break;
        }
        if (const auto enabled = MH_EnableHook(target); enabled != MH_OK) {
            MH_RemoveHook(target);
            result = 9860 + enabled;
            break;
        }
        hooked = true;
    } while (false);
    status = result;
    publish();
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}
game_steam::Telemetry game_steam::telemetry() {
    Telemetry t;
    t.installed = hooked;
    t.status = status;
    t.calls = calls;
    t.fromGame = fromGame;
    t.cleared = cleared;
    t.lastIdentity = lastIdentity;
    return t;
}
// Installs the guard (if it is not in yet) and reports it, for a check
// without a headset.
extern "C" __declspec(dllexport) DWORD WINAPI SpidySteamGuard(void* output) {
    const auto code = game_steam::install(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    auto t = game_steam::telemetry();
    t.status = code;
    __try {
        if (reinterpret_cast<uintptr_t>(output) >= 0x10000)
            std::memcpy(output, &t, sizeof(t));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9806;
    }
    return code;
}

// The game's prompt texts on their way to its markup resolver (game_prompts.hpp).
#include "spidy/game_prompts.hpp"
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <windows.h>
using namespace spidy;
using namespace spidy::game_prompts;

namespace {
// The UI's markup resolver (a pointer to the object), and its slot.
constexpr uintptr_t resolverGlobal = 0x7be5ba0;
constexpr unsigned resolveSlot = 5;
using Resolve = void(__fastcall*)(void*, void*, const char*);
Resolve original{};
void* target{};
// Research: the game's calls into Flash (the UI's call wrapper, which
// game_menu hooks in a VR session; here only where it has not), by name with
// their first arguments. A Scaleform value as the game lays it out.
constexpr uintptr_t invokeWrapper = 0x1d1cf30;
struct GfxValue {
    unsigned char head[16];
    void* objects;
    uint32_t type, reserved;
    uint64_t data, extra;
};
using Invoke = bool(__fastcall*)(void*, const char*, GfxValue*, uint32_t, uint64_t, float, uint32_t, uint64_t);
Invoke originalInvoke{};
void* invokeTarget{};
// Research: the game's localized texts (its lookup by tag hash, which
// game_menu hooks in a VR session), those with markup in them.
constexpr uintptr_t textLookup = 0x1749970;
using Text = const char*(__fastcall*)(uint32_t, const char*);
Text originalText{};
void* textTarget{};
SRWLOCK lock = SRWLOCK_INIT;
Trace trace;
Rule rule;
std::atomic<bool> hooked{};

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
// The text, as far as it can be read and no longer than the game's own lines.
// How much can be read at `p` before the end of the region it belongs to; 0 if
// it cannot be read at all. A guarded read catches a fault, but the game keeps
// its own handler for those and takes the process down before ours is reached,
// so the answer is to not fault at all: ask Windows first.
size_t readable(const void* p, size_t want) {
    if (reinterpret_cast<uintptr_t>(p) < 0x10000)
        return 0;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return 0;
    constexpr DWORD any = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                          PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & any) || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return 0;
    const auto here = static_cast<const char*>(p);
    const auto base = static_cast<const char*>(info.BaseAddress);
    const size_t left = info.RegionSize - static_cast<size_t>(here - base);
    return left < want ? left : want;
}
size_t copyText(const char* text, char* out, size_t capacity) {
    size_t n = 0;
    // Only to the end of the text's own region, so the walk cannot run off it
    // even where the string has no terminator.
    const size_t room = capacity ? readable(text, capacity - 1) : 0;
    __try {
        while (n < room && text[n])
            out[n] = text[n], ++n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = 0;
    }
    if (capacity)
        out[n] = 0;
    return n;
}
// Adds a line to the list (the lock held); one that begins as `seen` does for
// `same` characters is replaced, and the oldest goes when the list is full.
void keep(const char* seen, size_t same) {
    unsigned at = trace.count;
    for (unsigned i = 0; i < trace.count; ++i)
        if (!std::strncmp(trace.texts[i], seen, same))
            at = i;
    if (at == trace.count && trace.count == traceEntries)
        at = 0;
    if (at < trace.count) {
        std::memmove(trace.texts[at], trace.texts[at + 1], (trace.count - at - 1) * traceChars);
        --trace.count;
    }
    strncpy_s(trace.texts[trace.count++], seen, _TRUNCATE);
}
bool readValue(const GfxValue* v, GfxValue& out) {
    __try {
        out = *v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// `name(arg, arg, ...)` as far as the values can be read. Returns how long
// the name itself was, so a list can key lines on it.
size_t formatCall(const char* name, const GfxValue* args, uint32_t count, char* line, size_t size) {
    char text[160];
    size_t n = reinterpret_cast<uintptr_t>(name) < 0x10000 ? 0 : copyText(name, line, 64);
    const size_t key = n;
    line[n++] = '(';
    for (uint32_t i = 0; i < count && i < 6 && args && n + 24 < size; ++i) {
        GfxValue v;
        if (!readValue(args + i, v))
            break;
        if (i)
            line[n++] = ',', line[n++] = ' ';
        const uint32_t type = v.type & 0x8f;
        if (type == 6) {
            // A string: its characters, or a pointer to them when managed.
            uintptr_t p = v.data;
            if ((v.type & 0x40) && !read(p, &p, sizeof(p)))
                p = 0;
            const size_t got = p < 0x10000 ? 0 : copyText(reinterpret_cast<const char*>(p), text, sizeof(text));
            const size_t room = size - n - 4;
            n += sprintf_s(line + n, size - n, "'%.*s'", static_cast<int>(std::min(got, room)), text);
        } else if (type == 5) {
            double number;
            std::memcpy(&number, &v.data, sizeof(number));
            n += sprintf_s(line + n, size - n, "%g", number);
        } else if (type == 2 || type == 3 || type == 4) {
            n += sprintf_s(line + n, size - n, "%lld", static_cast<long long>(static_cast<int32_t>(v.data)));
        } else {
            n += sprintf_s(line + n, size - n, "<%u>", type);
        }
    }
    if (n + 2 < size)
        line[n++] = ')';
    line[n] = 0;
    return key;
}
bool __fastcall invokeTrace(void* ui, const char* name, GfxValue* args, uint32_t count, uint64_t a, float b,
                            uint32_t c, uint64_t d) {
    char line[traceChars];
    const size_t key = formatCall(name, args, count, line, sizeof(line));
    AcquireSRWLockExclusive(&lock);
    keep(line, key + 1);
    ReleaseSRWLockExclusive(&lock);
    return originalInvoke(ui, name, args, count, a, b, c, d);
}
const char* __fastcall textTrace(uint32_t hash, const char* fallback) {
    const char* text = originalText(hash, fallback);
    char seen[traceChars - 12], line[traceChars];
    if (reinterpret_cast<uintptr_t>(text) < 0x10000 || !copyText(text, seen, sizeof(seen)))
        return text;
    // Markup only: a button in a sentence is a tag or a token of some kind.
    if (!std::strpbrk(seen, "<[{%$#@"))
        return text;
    sprintf_s(line, "%08x %s", hash, seen);
    AcquireSRWLockExclusive(&lock);
    keep(line, 9);
    ReleaseSRWLockExclusive(&lock);
    return text;
}
// ---- The live reading a VR session uses (game_prompts.hpp) ----
// The prompt and QTE interface, by the image offset of each name the game
// hands the UI's wrapper. One copy of each literal in the supported
// Spider-Man.exe, the same kind of constant as game_menu's
// openOptionsName = 0x389f4c0.
struct Call {
    uintptr_t name;
    bool clears;
    // The argument holding a text to read the buttons out of, or -1 for none.
    int text = -1;
};
constexpr Call promptCalls[] = {
    {0x3896ce8, false}, // ShowPlaceablePrompt
    {0x3896cd0, true},  // HidePlaceablePrompt
    {0x3896d00, false}, // SetPlaceablePromptProgress (a hold running on)
    {0x3895ca0, false}, // UpdateInWorldPrompt
    {0x38982c8, false}, // POIInteractAvailable
    {0x3896e5c, false}, // AddQTE
    {0x3896eb8, false}, // UpdateQTE
    {0x3896e68, true},  // ClearQTE
    // Where a prompt's words really come from, found in the game on October
    // 13: ShowText(channel, text, ...) through this same wrapper, its text
    // naming its buttons as `[BTN_B]`. HideText(channel, ...) ends it. The
    // localized lookup never carried one of these texts, in two sessions.
    {0x4f44af0, false, 1}, // ShowText
    {0x4f44b00, true},     // HideText
};
static_assert(std::size(promptCalls) == callNames);
std::atomic<bool> reading{}, rewriting{};
// The newest prompt calls with their arguments, and how often each name came.
SRWLOCK callLock = SRWLOCK_INIT;
Calls seenCalls;
uint32_t callAt{};
// The census, by the name pointer the game passed (a literal of its image, so
// one pointer a name).
const char* censusAt[censusNames]{};
// When the list started, so every line can carry its own moment.
uint64_t ringStart{};
// Appends one line to the ring (the lock held), stamped with the time.
void keepLine(const char* text) {
    const uint64_t now = GetTickCount64();
    if (!ringStart)
        ringStart = now;
    char stamped[callChars];
    sprintf_s(stamped, "%6llu %s", static_cast<unsigned long long>(now - ringStart), text);
    strncpy_s(seenCalls.line[callAt], stamped, _TRUNCATE);
    callAt = (callAt + 1) % callLines;
    if (seenCalls.count < callLines)
        ++seenCalls.count;
}
std::atomic<uint32_t> livePacked{}, lastPacked{}, beyondPlay{}, keyboardTexts{};
// Whether the UI's calls are looked at at all. Off for a session until the
// October 13 death on a save load is found; SpidyPromptsWatch turns it on for
// a probe, which is where it gets found.
std::atomic<bool> observing{};
std::atomic<uint64_t> invokes{}, promptCallsSeen{}, raised{}, cleared{}, named{}, rewrote{}, freshAt{};
// The rewritten lines, kept in turn so a pointer handed to the game stays
// good while its text field reads it.
constexpr unsigned lineCount = 8, lineChars = 512;
char lines[lineCount][lineChars]{};
std::atomic<unsigned> nextLine{};

void __fastcall resolveHook(void* self, void* field, const char* text) {
    char seen[1024];
    if (reinterpret_cast<uintptr_t>(text) < 0x10000) {
        original(self, field, text);
        return;
    }
    const size_t length = copyText(text, seen, sizeof(seen));
    std::string changed;
    AcquireSRWLockExclusive(&lock);
    ++trace.resolved;
    keep(seen, traceChars - 1);
    // A whole text only: one cut short would lose its end.
    if (rule.find[0] && length + 1 < sizeof(seen)) {
        std::string s(seen);
        bool any = false;
        for (size_t p = s.find(rule.find); p != std::string::npos; p = s.find(rule.find, p + std::strlen(rule.replace))) {
            s.replace(p, std::strlen(rule.find), rule.replace);
            any = true;
        }
        if (any) {
            changed = std::move(s);
            ++trace.rewritten;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    original(self, field, changed.empty() ? text : changed.c_str());
}
} // namespace

namespace {
// The buttons a prompt's own text names, published as the live demand. The
// text is a Scaleform string, managed or not (game_menu's layout).
void readPromptText(const GfxValue* value) {
    GfxValue v;
    if (!value || !readValue(value, v) || (v.type & 0x8f) != 6)
        return;
    uintptr_t p = v.data;
    if ((v.type & 0x40) && !read(p, &p, sizeof(p)))
        return;
    if (p < 0x10000)
        return;
    char seen[lineChars];
    const size_t length = copyText(reinterpret_cast<const char*>(p), seen, sizeof(seen));
    if (!length)
        return;
    const std::string_view view(seen, length);
    const auto asked = prompt_buttons::forText(view);
    if (!asked.any()) {
        if (asked.keyboard)
            keyboardTexts.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t word = prompt_buttons::pack(asked);
    livePacked.store(word, std::memory_order_relaxed);
    lastPacked.store(word, std::memory_order_relaxed);
    freshAt.store(GetTickCount64(), std::memory_order_relaxed);
    named.fetch_add(1, std::memory_order_relaxed);
    if (!prompt_buttons::reachableInPlay(asked))
        beyondPlay.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

void game_prompts::setObserving(bool on) {
    observing.store(on, std::memory_order_relaxed);
}
void game_prompts::setReading(bool on) {
    reading.store(on, std::memory_order_relaxed);
    if (!on)
        livePacked.store(0, std::memory_order_relaxed);
}

void game_prompts::setRewriting(bool on) {
    rewriting.store(on, std::memory_order_relaxed);
}

void game_prompts::observeCall(uintptr_t imageBase, const char* name, const void* args, uint32_t count) {
    invokes.fetch_add(1, std::memory_order_relaxed);
    const auto at = reinterpret_cast<uintptr_t>(name);
    if (!imageBase || at <= imageBase)
        return;
    if (!observing.load(std::memory_order_relaxed))
        return;
    // The census first: every name, whether this knows it or not. One example
    // of each call's arguments is kept, which is what says where a prompt's
    // text and its button arrive.
    AcquireSRWLockExclusive(&callLock);
    unsigned slot = seenCalls.names;
    for (unsigned i = 0; i < seenCalls.names; ++i)
        if (censusAt[i] == name) {
            slot = i;
            break;
        }
    if (slot < censusNames) {
        if (slot == seenCalls.names) {
            censusAt[slot] = name;
            ++seenCalls.names;
            char line[censusChars];
            formatCall(name, static_cast<const GfxValue*>(args), count, line, sizeof(line));
            strncpy_s(seenCalls.example[slot], line, _TRUNCATE);
        }
        ++seenCalls.calls[slot];
    }
    ReleaseSRWLockExclusive(&callLock);
    const uintptr_t offset = at - imageBase;
    for (unsigned index = 0; index < callNames; ++index) {
        const auto& call = promptCalls[index];
        if (call.name != offset)
            continue;
        promptCallsSeen.fetch_add(1, std::memory_order_relaxed);
        // A call that carries a text: its buttons are the demand.
        if (call.text >= 0 && args && count > static_cast<uint32_t>(call.text) &&
            reading.load(std::memory_order_relaxed))
            readPromptText(static_cast<const GfxValue*>(args) + call.text);
        // The call with its arguments, for the session report.
        char line[callChars];
        formatCall(name, static_cast<const GfxValue*>(args), count, line, sizeof(line));
        AcquireSRWLockExclusive(&callLock);
        ++seenCalls.perName[index];
        keepLine(line);
        ReleaseSRWLockExclusive(&callLock);
        if (call.clears) {
            // Its manager says the prompt is gone: no demand outlives it.
            livePacked.store(0, std::memory_order_relaxed);
            cleared.fetch_add(1, std::memory_order_relaxed);
        } else {
            raised.fetch_add(1, std::memory_order_relaxed);
            if (livePacked.load(std::memory_order_relaxed))
                freshAt.store(GetTickCount64(), std::memory_order_relaxed);
        }
        return;
    }
}

// The game's localized text, on its way to its UI.
//
// This no longer reads it, and October 13 is why: with the reading on, loading
// a save killed the game every time - the process simply gone, no crash dump -
// while the same save with the reading off loaded and played. Measured both
// ways, twice each. A load resolves a flood of the game's texts, and walking
// every one of them as a C string is a bet this lost.
//
// The prompts never needed it. The vent's own text arrived as an ARGUMENT of
// POIInteractAvailable - `POIInteractAvailable(5, 0, '[BTN_Y] Enter')` -
// which readPromptText reads from the call, and every prompt measured in the
// game has come that way. So this is a pass-through again.
//
// What went with it: the rewriting of a prompt's wording for VR
// (setRewriting), which never ran in a headset, and the texts counted from
// this route. The call route still carries the demand and the keyboard count.
const char* game_prompts::observeText(const char* text) {
    return text;
}

void game_prompts::note(const char* text) {
    if (!text || !reading.load(std::memory_order_relaxed))
        return;
    char line[callChars];
    sprintf_s(line, ">> %.100s", text);
    AcquireSRWLockExclusive(&callLock);
    keepLine(line);
    ReleaseSRWLockExclusive(&callLock);
}

void game_prompts::recentCalls(Calls& out) {
    AcquireSRWLockShared(&callLock);
    const uint32_t count = seenCalls.count, at = callAt;
    out.count = count;
    for (unsigned i = 0; i < callNames; ++i)
        out.perName[i] = seenCalls.perName[i];
    // Oldest first: the ring's next slot is the oldest once it has wrapped.
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t from = count < callLines ? i : (at + i) % callLines;
        strncpy_s(out.line[i], seenCalls.line[from], _TRUNCATE);
    }
    for (uint32_t i = count; i < callLines; ++i)
        out.line[i][0] = 0;
    out.names = seenCalls.names;
    for (unsigned i = 0; i < censusNames; ++i) {
        out.calls[i] = seenCalls.calls[i];
        strncpy_s(out.example[i], seenCalls.example[i], _TRUNCATE);
    }
    ReleaseSRWLockShared(&callLock);
}

prompt_buttons::Demand game_prompts::demand() {
    const uint32_t word = livePacked.load(std::memory_order_relaxed);
    if (!word)
        return {};
    if (GetTickCount64() - freshAt.load(std::memory_order_relaxed) > liveMs) {
        // Nothing has named it for liveMs: the prompt is taken as gone even
        // where its manager's Hide was missed.
        livePacked.store(0, std::memory_order_relaxed);
        return {};
    }
    return prompt_buttons::unpack(word);
}

Live game_prompts::live() {
    Live out;
    out.invokes = invokes.load(std::memory_order_relaxed);
    out.calls = promptCallsSeen.load(std::memory_order_relaxed);
    out.raised = raised.load(std::memory_order_relaxed);
    out.cleared = cleared.load(std::memory_order_relaxed);
    out.named = named.load(std::memory_order_relaxed);
    out.rewritten = rewrote.load(std::memory_order_relaxed);
    out.demand = prompt_buttons::pack(demand());
    out.lastDemand = lastPacked.load(std::memory_order_relaxed);
    out.beyondPlay = beyondPlay.load(std::memory_order_relaxed);
    out.keyboardTexts = keyboardTexts.load(std::memory_order_relaxed);
    return out;
}

// Turns the live reading on or off from outside a VR session, for a headless
// probe. game_menu's hooks are what call into it, so SpidyMenuStart must have
// run first. Input: one uint32 - 0 off, 1 read, 2 read and rewrite the texts.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPromptsWatch(void* input) {
    uint32_t mode = 1;
    __try {
        if (reinterpret_cast<uintptr_t>(input) >= 0x10000)
            std::memcpy(&mode, input, sizeof(mode));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9601;
    }
    game_prompts::setObserving(mode != 0);
    game_prompts::setReading(mode != 0);
    game_prompts::setRewriting(mode > 1);
    return 0;
}
// What it has seen: the counters and both lists.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPromptsWatched(void* output) {
    Watched out;
    out.reading = reading.load(std::memory_order_relaxed) ? 1u : 0u;
    out.live = game_prompts::live();
    game_prompts::recentCalls(out.calls);
    __try {
        if (reinterpret_cast<uintptr_t>(output) < 0x10000)
            return 9611;
        std::memcpy(output, &out, sizeof(out));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9611;
    }
    return 0;
}
// A line of the probe's own in the timeline, so what it pressed sits beside
// what the game said.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPromptsNote(void* input) {
    char text[96]{};
    __try {
        if (reinterpret_cast<uintptr_t>(input) < 0x10000)
            return 9601;
        std::memcpy(text, input, sizeof(text) - 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9601;
    }
    game_prompts::note(text);
    return 0;
}

uint32_t game_prompts::install(uintptr_t base) {
    if (hooked)
        return 0;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const uintptr_t size = nt->OptionalHeader.SizeOfImage;
    uintptr_t object{}, table{}, function{};
    if (!read(base + resolverGlobal, &object, sizeof(object)) || !object)
        return 9601;
    if (!read(object, &table, sizeof(table)) || table < base || table - base >= size)
        return 9602;
    if (!read(table + resolveSlot * sizeof(void*), &function, sizeof(function)) || function < base ||
        function - base >= size)
        return 9603;
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return 9700 + status;
    target = reinterpret_cast<void*>(function);
    if ((status = MH_CreateHook(target, reinterpret_cast<void*>(&resolveHook),
                                reinterpret_cast<void**>(&original))) != MH_OK)
        return 9720 + status;
    if ((status = MH_EnableHook(target)) != MH_OK) {
        MH_RemoveHook(target);
        return 9740 + status;
    }
    AcquireSRWLockExclusive(&lock);
    trace.function = function - base;
    ReleaseSRWLockExclusive(&lock);
    hooked = true;
    return 0;
}
void game_prompts::uninstall() {
    if (textTarget) {
        MH_DisableHook(textTarget);
        MH_RemoveHook(textTarget);
        textTarget = nullptr;
    }
    if (invokeTarget) {
        MH_DisableHook(invokeTarget);
        MH_RemoveHook(invokeTarget);
        invokeTarget = nullptr;
    }
    if (!hooked.exchange(false))
        return;
    MH_DisableHook(target);
    MH_RemoveHook(target);
}
// Research: the localized texts with markup too. 0, or 9800-9839.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPromptsTexts(void*) {
    if (textTarget)
        return 0;
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return 9800 + status;
    void* at = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + textLookup);
    if ((status = MH_CreateHook(at, reinterpret_cast<void*>(&textTrace),
                                reinterpret_cast<void**>(&originalText))) != MH_OK)
        return 9810 + status;
    if ((status = MH_EnableHook(at)) != MH_OK) {
        MH_RemoveHook(at);
        return 9820 + status;
    }
    textTarget = at;
    return 0;
}
// Research: the calls into Flash too. 0, or 9760-9799 (game_menu has the
// wrapper in a VR session).
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPromptsCalls(void*) {
    if (invokeTarget)
        return 0;
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return 9760 + status;
    void* at = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + invokeWrapper);
    if ((status = MH_CreateHook(at, reinterpret_cast<void*>(&invokeTrace),
                                reinterpret_cast<void**>(&originalInvoke))) != MH_OK)
        return 9770 + status;
    if ((status = MH_EnableHook(at)) != MH_OK) {
        MH_RemoveHook(at);
        return 9780 + status;
    }
    invokeTarget = at;
    return 0;
}

// Headless probes: hooks the resolver, and with an input sets the research
// rule (Rule; one with an empty `find` ends it).
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPromptsStart(void* input) {
    if (reinterpret_cast<uintptr_t>(input) >= 0x10000) {
        Rule next;
        if (!read(reinterpret_cast<uintptr_t>(input), &next, sizeof(next)))
            return 9611;
        next.find[ruleChars - 1] = next.replace[ruleChars - 1] = 0;
        AcquireSRWLockExclusive(&lock);
        rule = next;
        ReleaseSRWLockExclusive(&lock);
    }
    return install(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
}
// Output: Trace.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPromptsSample(void* output) {
    AcquireSRWLockShared(&lock);
    Trace copy = trace;
    ReleaseSRWLockShared(&lock);
    copy.status = hooked;
    __try {
        if (reinterpret_cast<uintptr_t>(output) < 0x10000)
            return 9611;
        std::memcpy(output, &copy, sizeof(copy));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9611;
    }
    return 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPromptsStop(void*) {
    game_prompts::uninstall();
    return 0;
}

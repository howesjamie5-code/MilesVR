// The game's prompt texts on their way to its markup resolver (game_prompts.hpp).
#include "spidy/game_prompts.hpp"
#include "spidy/prompt_actions.hpp"
#include <MinHook.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <windows.h>
using namespace spidy;
using namespace spidy::game_prompts;

namespace {
// The UI's markup resolver (a pointer to the object), and its slot.
constexpr uintptr_t resolverGlobal = 0x859bf40;
constexpr unsigned resolveSlot = 5;
using Resolve = void(__fastcall*)(void*, void*, const char*);
Resolve original{};
void* target{};
// Research: the game's calls into Flash (the UI's call wrapper, which
// game_menu hooks in a VR session; here only where it has not), by name with
// their first arguments. A Scaleform value as the game lays it out.
constexpr uintptr_t invokeWrapper = 0x222bac0;
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
constexpr uintptr_t textLookup = 0x1c55d40;
using Text = const char*(__fastcall*)(uint32_t, const char*);
Text originalText{};
void* textTarget{};
SRWLOCK lock = SRWLOCK_INIT;
Trace trace;
Rule rule;
std::atomic<bool> hooked{};

size_t readable(const void* p, size_t want);
// Memory of the game's, read only where Windows says it can be (readable): the
// guard below is a second line, not the first.
bool read(uintptr_t p, void* out, size_t n) {
    if (p < 0x10000 || readable(reinterpret_cast<const void*>(p), n) < n)
        return false;
    __try {
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
// openOptionsName = 0x3df9568.
struct Call {
    uintptr_t name;
    bool clears;
    // The argument holding a text to read the buttons out of, or -1 for none.
    int text = -1;
    // Its first argument is the channel the text shows on (ShowText's own).
    bool channel = false;
};
constexpr Call promptCalls[] = {
    {0x3df0988, false}, // ShowPlaceablePrompt
    {0x3df0970, true},  // HidePlaceablePrompt
    {0x3df09a0, false}, // SetPlaceablePromptProgress (a hold running on)
    {0x3def570, false}, // UpdateInWorldPrompt
    {0x3df2498, false}, // POIInteractAvailable
    {0x3df0f44, false}, // AddQTE
    {0x3df0fc8, false}, // UpdateQTE
    {0x3df0f50, true},  // ClearQTE
    // Where a prompt's words really come from, found in the game on October
    // 13: ShowText(channel, text, ...) through this same wrapper, its text
    // naming its buttons as `[BTN_B]`. HideText(channel, ...) ends it. The
    // localized lookup never carried one of these texts, in two sessions.
    {0x57ac648, false, 1, true}, // ShowText
    {0x57ac658, true, -1, true}, // HideText
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
// Appends one line to the ring (the lock held), stamped with the time, cut to
// the ring's width. A call's line alone can fill callChars - 1 (formatCall
// stops a text where the line ends), and until October 20 the stamp went in
// front of it through sprintf_s into a buffer of that same width: any prompt
// whose text ran past about a hundred characters - a help line, a subtitle,
// the flood of them a save's load brings - overflowed it, and sprintf_s ends
// the process for that, with no exception to catch and no crash dump - which
// is how the reading's deaths of October 13 went (CONTINUE at the main menu,
// every time; the free roam of profile 1, seconds after the reading went on),
// and why sessions had it off since.
void keepLine(const char* text) {
    const uint64_t now = GetTickCount64();
    if (!ringStart)
        ringStart = now;
    _snprintf_s(seenCalls.line[callAt], callChars, _TRUNCATE, "%6llu %s",
                static_cast<unsigned long long>(now - ringStart), text);
    callAt = (callAt + 1) % callLines;
    if (seenCalls.count < callLines)
        ++seenCalls.count;
}
std::atomic<uint32_t> lastPacked{}, beyondPlay{}, keyboardTexts{};
// What is live now (prompt_buttons::LiveDemands: a prompt call's demand for
// liveMs, a channel's text until that channel is hidden). Written from the
// game's UI thread, read from the VR thread.
SRWLOCK demandLock = SRWLOCK_INIT;
prompt_buttons::LiveDemands liveDemands;
// Whether the UI's calls are looked at at all: in every session since October
// 20 (the reading's deaths were keepLine's), unless the launcher's
// --no-prompt-help says otherwise; SpidyPromptsWatch for a probe.
std::atomic<bool> observing{};
std::atomic<uint64_t> invokes{}, promptCallsSeen{}, raised{}, cleared{}, named{}, rewrote{};
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
// The buttons a prompt's own text names, packed (prompt_buttons::pack); 0 for
// none. The text is a Scaleform string, managed or not (game_menu's layout).
uint32_t readPromptText(const GfxValue* value) {
    GfxValue v;
    if (!value || !readValue(value, v) || (v.type & 0x8f) != 6)
        return 0;
    uintptr_t p = v.data;
    if ((v.type & 0x40) && !read(p, &p, sizeof(p)))
        return 0;
    if (p < 0x10000)
        return 0;
    char seen[lineChars];
    const size_t length = copyText(reinterpret_cast<const char*>(p), seen, sizeof(seen));
    if (!length)
        return 0;
    const std::string_view view(seen, length);
    const auto asked = prompt_buttons::forText(view);
    if (!asked.any()) {
        if (asked.keyboard)
            keyboardTexts.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const uint32_t word = prompt_buttons::pack(asked);
    lastPacked.store(word, std::memory_order_relaxed);
    named.fetch_add(1, std::memory_order_relaxed);
    if (!prompt_buttons::reachableInPlay(asked))
        beyondPlay.fetch_add(1, std::memory_order_relaxed);
    return word;
}
// The channel a ShowText or HideText names: its first argument, a whole
// number below LiveDemands::channels; -1 for anything else.
int readChannel(const GfxValue* args, uint32_t count) {
    GfxValue v;
    if (!args || !count || !readValue(args, v))
        return -1;
    const uint32_t type = v.type & 0x8f;
    double number = -1;
    if (type == 5)
        std::memcpy(&number, &v.data, sizeof(number));
    else if (type == 2 || type == 3 || type == 4)
        number = static_cast<int32_t>(v.data);
    if (!(number >= 0 && number < prompt_buttons::LiveDemands::channels) || number != std::floor(number))
        return -1;
    return static_cast<int>(number);
}
} // namespace

void game_prompts::setObserving(bool on) {
    observing.store(on, std::memory_order_relaxed);
}
void game_prompts::setReading(bool on) {
    reading.store(on, std::memory_order_relaxed);
    if (on)
        return;
    AcquireSRWLockExclusive(&demandLock);
    liveDemands.clear();
    ReleaseSRWLockExclusive(&demandLock);
}

void game_prompts::setRewriting(bool on) {
    rewriting.store(on, std::memory_order_relaxed);
}

namespace {
constexpr uint32_t poiSlots = 256;
std::atomic<bool> poiOffered[poiSlots]{};
std::atomic<uint64_t> fnsmShown{}, fnsmShownAt{}, venomCalls{};
std::atomic<bool> fnsmUp{};
std::atomic<int> venomState{-1};
} // namespace

int game_prompts::venomUnlocked() {
    return venomState.load(std::memory_order_relaxed);
}

uint64_t game_prompts::venomUnlockCalls() {
    return venomCalls.load(std::memory_order_relaxed);
}

uint64_t game_prompts::fnsmReminders() {
    return fnsmShown.load(std::memory_order_relaxed);
}

bool game_prompts::fnsmReminderUp() {
    return fnsmUp.load(std::memory_order_relaxed) &&
           GetTickCount64() - fnsmShownAt.load(std::memory_order_relaxed) < fnsmReminderMs;
}

bool game_prompts::interactOffered() {
    for (const auto& offered : poiOffered)
        if (offered.load(std::memory_order_relaxed))
            return true;
    return false;
}

void game_prompts::observeCall(uintptr_t imageBase, const char* name, const void* args, uint32_t count) {
    invokes.fetch_add(1, std::memory_order_relaxed);
    const auto at = reinterpret_cast<uintptr_t>(name);
    if (!imageBase || at <= imageBase)
        return;
    if (at == imageBase + showFnsmReminderName) {
        fnsmShown.fetch_add(1, std::memory_order_relaxed);
        fnsmShownAt.store(GetTickCount64(), std::memory_order_relaxed);
        fnsmUp.store(true, std::memory_order_relaxed);
    } else if (at == imageBase + hideFnsmReminderName) {
        fnsmUp.store(false, std::memory_order_relaxed);
    } else if (at == imageBase + setVenomUnlockedName && args && count >= 1) {
        // Its one value a bool (type 2), the answer in the data's low byte.
        GfxValue v;
        if (readValue(static_cast<const GfxValue*>(args), v) && (v.type & 0x8f) == 2) {
            venomState.store((v.data & 0xff) ? 1 : 0, std::memory_order_relaxed);
            venomCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Which of the game's world markers offer an interaction now: POIInteractAvailable(id, on, ...) (the
    // throwable's LB + RB, October 21), until it is turned off or ClearPOI(id, ...) removes the marker.
    if (args && count >= 1 && reinterpret_cast<uintptr_t>(name) >= 0x10000) {
        char called[24];
        const size_t n = copyText(name, called, sizeof(called));
        const bool offer = n == 20 && !std::strcmp(called, "POIInteractAvailable") && count >= 2;
        const bool gone = n == 8 && !std::strcmp(called, "ClearPOI");
        if (offer || gone) {
            const auto* values = static_cast<const GfxValue*>(args);
            const auto number = [](const GfxValue& v) {
                return (v.type & 0x8f) == 5 ? static_cast<uint32_t>(*reinterpret_cast<const double*>(&v.data))
                                            : static_cast<uint32_t>(v.data);
            };
            GfxValue id, on;
            if (readValue(values, id)) {
                const uint32_t poi = number(id);
                const bool now = offer && readValue(values + 1, on) && (on.data & 0xff) != 0;
                if (poi < poiSlots)
                    poiOffered[poi].store(now, std::memory_order_relaxed);
            }
        }
    }
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
        const auto values = static_cast<const GfxValue*>(args);
        const bool reads = reading.load(std::memory_order_relaxed);
        // A call that carries a text: its buttons are the demand.
        const uint32_t word = call.text >= 0 && args && count > static_cast<uint32_t>(call.text) && reads
                                  ? readPromptText(values + call.text)
                                  : 0;
        // The call with its arguments, for the session report.
        char line[callChars];
        formatCall(name, values, count, line, sizeof(line));
        AcquireSRWLockExclusive(&callLock);
        ++seenCalls.perName[index];
        keepLine(line);
        ReleaseSRWLockExclusive(&callLock);
        (call.clears ? cleared : raised).fetch_add(1, std::memory_order_relaxed);
        // A channel's text: what it names stands until that channel is hidden
        // or shows another text. Any other call: its demand for liveMs, and its
        // manager's Hide or Clear ends it (LiveDemands).
        const int channel = call.channel ? readChannel(values, count) : -1;
        AcquireSRWLockExclusive(&demandLock);
        if (channel < 0)
            liveDemands.call(GetTickCount64(), word, call.clears);
        else if (call.clears || reads)
            liveDemands.text(GetTickCount64(), static_cast<unsigned>(channel), word, call.clears);
        ReleaseSRWLockExclusive(&demandLock);
        return;
    }
}

// The game's localized text, on its way to its UI.
//
// This no longer reads it. On October 13, with the reading on, loading a save
// killed the game every time - the process simply gone, no crash dump - and
// this walk of every text the load resolves was the first suspect. It was not
// the cause: the deaths were keepLine's (October 20, measured in the game with
// and without its fix), and the walk asks Windows before it reads (readable).
//
// The prompts never needed it. The vent's own text arrived as an ARGUMENT of
// POIInteractAvailable - `POIInteractAvailable(5, 0, '[BTN_Y] Enter')` -
// which readPromptText reads from the call, and every prompt measured in the
// game has come that way. So this is a pass-through again.
//
// What went with it: the rewriting of a prompt's wording for VR
// (setRewriting), which never ran in a headset, and the texts counted from
// this route. The call route still carries the demand and the keyboard count.
// MilesVR 0.1.78: it rewrites again, but only what it must and only safely. A text is copied out through
// copyText (SEH, bounded by its region) and looked at only if it holds "[ACTION_" - the game's action
// names, which its prompts, tips and tutorials use instead of buttons - and each rewritten text is kept
// for good, keyed by the game's pointer and its words, so the pointer handed back stays good however long
// the game keeps it (prompt_actions.hpp).
namespace {
std::atomic<bool> controlTriggerWebs{}, controlVenomTrigger{true};
std::atomic<uint64_t> textRewrites{};
SRWLOCK rewriteLock = SRWLOCK_INIT;
struct Rewritten {
    std::string from, to;
};
std::unordered_map<uintptr_t, std::unique_ptr<Rewritten>> rewrittenTexts;
std::unordered_set<std::string> unknownActions;
} // namespace

void game_prompts::setControls(bool triggerWebs, bool venomTrigger) {
    controlTriggerWebs.store(triggerWebs, std::memory_order_relaxed);
    controlVenomTrigger.store(venomTrigger, std::memory_order_relaxed);
}

uint64_t game_prompts::textsRewritten() {
    return textRewrites.load(std::memory_order_relaxed);
}

const char* game_prompts::observeText(const char* text) {
    if (!rewriting.load(std::memory_order_relaxed) || reinterpret_cast<uintptr_t>(text) < 0x10000)
        return text;
    char seen[lineChars];
    const size_t length = copyText(text, seen, sizeof(seen));
    if (!length || length + 1 >= sizeof(seen) ||
        (!std::strstr(seen, "[ACTION_") && !std::strstr(seen, "[BTN_") && !std::strstr(seen, "[DPAD_")))
        return text;
    const prompt_actions::Controls controls{controlTriggerWebs.load(std::memory_order_relaxed),
                                            controlVenomTrigger.load(std::memory_order_relaxed)};
    std::vector<std::string> unknown;
    std::string changed = prompt_actions::rewrite(seen, controls, &unknown);
    const auto key = reinterpret_cast<uintptr_t>(text);
    AcquireSRWLockExclusive(&rewriteLock);
    for (const auto& u : unknown)
        if (unknownActions.insert(u).second) {
            char line[callChars];
            sprintf_s(line, ">> prompt action not in the VR table: %.80s", u.c_str());
            AcquireSRWLockExclusive(&callLock);
            keepLine(line);
            ReleaseSRWLockExclusive(&callLock);
        }
    if (changed == seen) {
        ReleaseSRWLockExclusive(&rewriteLock);
        return text;
    }
    auto& slot = rewrittenTexts[key];
    // The same pointer with other words (a table the game reloaded): a new entry; the old one stays
    // alive in `retired` in case the game still holds it.
    static std::vector<std::unique_ptr<Rewritten>> retired;
    if (slot && slot->from != seen) {
        retired.push_back(std::move(slot));
        slot.reset();
    }
    if (!slot) {
        slot = std::make_unique<Rewritten>(Rewritten{seen, std::move(changed)});
        textRewrites.fetch_add(1, std::memory_order_relaxed);
    }
    const char* out = slot->to.c_str();
    ReleaseSRWLockExclusive(&rewriteLock);
    return out;
}

namespace {
std::atomic<bool> playing{};
// The in-world prompts' texts, rewritten, kept for good by their words (the game's own string goes
// on being its own: the call gets a copy of its arguments).
std::unordered_map<std::string, std::unique_ptr<std::string>> rewrittenArgs;
unsigned argTextsNoted{};
} // namespace

void game_prompts::setPlaying(bool on) {
    playing.store(on, std::memory_order_relaxed);
}

bool game_prompts::rewriteArgs(const char*, const void* args, uint32_t count, void* out, uint32_t room) {
    if (!rewriting.load(std::memory_order_relaxed) || !playing.load(std::memory_order_relaxed) || !args ||
        !out || !count || count > room)
        return false;
    const auto* in = static_cast<const GfxValue*>(args);
    auto* copy = static_cast<GfxValue*>(out);
    bool changedAny = false;
    for (uint32_t i = 0; i < count; ++i) {
        GfxValue v;
        if (!readValue(in + i, v))
            return false;
        copy[i] = v;
        if ((v.type & 0x8f) != 6)
            continue;
        uintptr_t p = v.data;
        if ((v.type & 0x40) && !read(p, &p, sizeof(p)))
            continue;
        if (p < 0x10000)
            continue;
        char seen[lineChars];
        const size_t length = copyText(reinterpret_cast<const char*>(p), seen, sizeof(seen));
        if (!length || length + 1 >= sizeof(seen) ||
            (!std::strstr(seen, "[BTN_") && !std::strstr(seen, "[ACTION_") && !std::strstr(seen, "[DPAD_")))
            continue;
        const prompt_actions::Controls controls{controlTriggerWebs.load(std::memory_order_relaxed),
                                                controlVenomTrigger.load(std::memory_order_relaxed)};
        std::vector<std::string> unknown;
        std::string changed = prompt_actions::rewrite(seen, controls, &unknown);
        AcquireSRWLockExclusive(&rewriteLock);
        // Each prompt text met, the first 60, for the session report: what the game's in-world prompts say.
        const bool fresh = !rewrittenArgs.contains(seen);
        if (fresh && argTextsNoted < 60) {
            ++argTextsNoted;
            char line[callChars];
            sprintf_s(line, ">> prompt text: %.70s -> %.70s", seen, changed.c_str());
            AcquireSRWLockExclusive(&callLock);
            keepLine(line);
            ReleaseSRWLockExclusive(&callLock);
        }
        for (const auto& u : unknown)
            if (unknownActions.insert(u).second) {
                char line[callChars];
                sprintf_s(line, ">> prompt action not in the VR table: %.80s", u.c_str());
                AcquireSRWLockExclusive(&callLock);
                keepLine(line);
                ReleaseSRWLockExclusive(&callLock);
            }
        auto& slot = rewrittenArgs[seen];
        if (!slot)
            slot = std::make_unique<std::string>(std::move(changed));
        const bool same = *slot == seen;
        const char* text = slot->c_str();
        ReleaseSRWLockExclusive(&rewriteLock);
        if (same)
            continue;
        // A plain (unmanaged) string of ours in the copy: the game's managed one stays the caller's.
        copy[i].type = v.type & ~0x40u;
        copy[i].objects = nullptr;
        copy[i].data = reinterpret_cast<uintptr_t>(text);
        copy[i].extra = 0;
        textRewrites.fetch_add(1, std::memory_order_relaxed);
        changedAny = true;
    }
    return changedAny;
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
    AcquireSRWLockExclusive(&demandLock);
    const auto out = liveDemands.demand(GetTickCount64());
    ReleaseSRWLockExclusive(&demandLock);
    return out;
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

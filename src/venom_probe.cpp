#include "spidy/venom_probe.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
// The component registry (game_targets.cpp): 16-byte entries of an address and a generation.
constexpr uintptr_t registryTable = 0x83f9fa0, registryCount = 0x83f9fc0;
// Hero::HeroVenomModeManager's vtable (its RTTI, MilesMorales.exe).
constexpr uintptr_t venomManagerVtable = 0x3e15b48;
// Every component of the player is watched; a line lists the 4-byte words that changed.
constexpr size_t snapshotBytes = 0x400, maxWords = 96;
constexpr ULONGLONG sampleMs = 250, scanMs = 3000;

bool read(uintptr_t p, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(p), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uintptr_t pointer(uintptr_t p) {
    uintptr_t v{};
    return read(p, &v, 8) ? v : 0;
}
// The class an object's vtable belongs to, from its complete object locator (slot -1).
std::string className(uintptr_t base, uintptr_t vtable) {
    const uintptr_t col = pointer(vtable - 8);
    uint32_t signature{}, descriptor{};
    if (!col || !read(col, &signature, 4) || signature != 1 || !read(col + 12, &descriptor, 4) || !descriptor)
        return {};
    char name[128]{};
    if (!read(base + descriptor + 16, name, sizeof(name) - 1))
        return {};
    name[sizeof(name) - 1] = 0;
    return name;
}
struct Watched {
    uintptr_t address{}, vtable{};
    std::string name;
    std::vector<uint8_t> last;
};
FILE* out{};
std::unordered_map<uintptr_t, Watched> watched;
std::unordered_map<uintptr_t, bool> listed;
ULONGLONG lastSample{}, lastScan{}, started{};
uintptr_t lastRecord{};

void open() {
    if (out)
        return;
    // reports\stereo-modules\<hash>\spidy_stereo_probe.dll -> reports\venom-probe-<time>.csv
    HMODULE self{};
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&open), &self) ||
        !GetModuleFileNameW(self, path, MAX_PATH))
        return;
    std::wstring dir(path);
    for (int up = 0; up < 3; ++up)
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"\\venom-probe-%04u%02u%02u-%02u%02u%02u.csv", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
    if (_wfopen_s(&out, (dir + name).c_str(), L"w") != 0)
        out = nullptr;
    if (out)
        std::fprintf(out, "ms,kind,address,class,record,data\n");
    started = GetTickCount64();
}
void scan(uintptr_t base, uintptr_t record) {
    const uintptr_t table = pointer(base + registryTable);
    int32_t count{};
    if (!table || !read(base + registryCount, &count, 4) || count <= 0 || count > 0x100000)
        return;
    for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
        struct Entry {
            uintptr_t address;
            uint32_t generation, pad;
        } e{};
        if (!read(table + i * 16ull, &e, sizeof(e)) || e.address < 0x10000 || e.address % 8 || !e.generation ||
            e.generation > 0xfff)
            continue;
        struct Head {
            uintptr_t vtable, record;
            uint32_t pad, handle;
        } h{};
        if (!read(e.address, &h, sizeof(h)) || h.handle != (e.generation << 20 | i))
            continue;
        const bool manager = h.vtable == base + venomManagerVtable;
        if (!manager && (!record || h.record != record))
            continue;
        const std::string n = className(base, h.vtable);
        if (!listed[e.address]) {
            listed[e.address] = true;
            std::fprintf(out, "%llu,component,%llx,%s,%llx,\n", GetTickCount64() - started,
                         static_cast<unsigned long long>(e.address), n.c_str(),
                         static_cast<unsigned long long>(h.record));
        }
        if ((manager || !n.empty()) && !watched.count(e.address))
            watched[e.address] = {e.address, h.vtable, n, {}};
    }
    std::fflush(out);
}
} // namespace

void spidy::venom_probe::sample(uintptr_t base, uintptr_t record) {
    const ULONGLONG now = GetTickCount64();
    if (!base || now - lastSample < sampleMs)
        return;
    lastSample = now;
    open();
    if (!out)
        return;
    if (record != lastRecord || now - lastScan >= scanMs) {
        if (record != lastRecord)
            std::fprintf(out, "%llu,player,0,,%llx,\n", now - started, static_cast<unsigned long long>(record));
        lastRecord = record;
        lastScan = now;
        scan(base, record);
    }
    std::vector<uint8_t> bytes(snapshotBytes);
    for (auto it = watched.begin(); it != watched.end();) {
        auto& w = it->second;
        uintptr_t vtable{};
        if (!read(w.address, &vtable, 8) || vtable != w.vtable) {
            it = watched.erase(it); // freed or reused
            continue;
        }
        // A component may be shorter than snapshotBytes: read what is readable.
        size_t got = 0;
        while (got < snapshotBytes && read(w.address + got, bytes.data() + got, 16))
            got += 16;
        if (got && w.last.size() == got) {
            // Only the words that changed: offset=value pairs.
            std::string line;
            size_t words = 0;
            for (size_t k = 0; k + 4 <= got && words < maxWords; k += 4)
                if (std::memcmp(w.last.data() + k, bytes.data() + k, 4)) {
                    uint32_t v{};
                    std::memcpy(&v, bytes.data() + k, 4);
                    char item[32];
                    sprintf_s(item, "%zx=%08x;", k, v);
                    line += item;
                    ++words;
                }
            if (!line.empty())
                std::fprintf(out, "%llu,diff,%llx,%s,,%s\n", now - started,
                             static_cast<unsigned long long>(w.address), w.name.c_str(), line.c_str());
        } else if (got) {
            std::fprintf(out, "%llu,snapshot,%llx,%s,,", now - started, static_cast<unsigned long long>(w.address),
                         w.name.c_str());
            for (size_t k = 0; k < got; ++k)
                std::fprintf(out, "%02x", bytes[k]);
            std::fputc('\n', out);
        }
        if (got)
            w.last.assign(bytes.begin(), bytes.begin() + got);
        ++it;
    }
    std::fflush(out);
}

void spidy::venom_probe::stop() {
    if (out)
        std::fclose(out);
    out = nullptr;
    watched.clear();
    listed.clear();
    lastRecord = 0;
}

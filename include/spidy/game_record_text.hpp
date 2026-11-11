#pragma once
// The recorder's pure part (game_record.cpp): reading a damage request's bytes and writing its log lines. No game
// access, so a test on any machine runs it.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace spidy::game_record {
// A request's fields, at the offsets native_bodies::damageNow sets them (the bit is the field's index in the
// reflection, in the masks at +8 and +0x18).
struct Request {
    uint64_t fields{}, fields2{};
    uint32_t damager{}, type{}, category{}, hitFlags{}, hash{};
    float amount{}, knockbackAmount{}, impulse{};
    int32_t knockback{};
    bool hasDamager{};
};
constexpr uint32_t requestBytes = 0x1b0;
inline Request parse(const uint8_t* bytes) {
    Request r;
    const auto word = [&](uint32_t at, void* out) { std::memcpy(out, bytes + at, 4); };
    std::memcpy(&r.fields, bytes + 8, 8);
    std::memcpy(&r.fields2, bytes + 0x18, 8);
    r.hasDamager = (r.fields >> 8) & 1;
    word(0x138, &r.damager);
    word(0x13c, &r.type);
    word(0x140, &r.category);
    word(0x144, &r.amount);
    word(0x150, &r.knockback);
    word(0x154, &r.knockbackAmount);
    word(0x168, &r.hitFlags);
    word(0x190, &r.impulse);
    word(0x19c, &r.hash);
    return r;
}
// "dmg 12.5 type 1 cat 1 kb 3/1.0 flags 80d02000 imp 0.0 hash a621eb5c set 5a0cf00": a field that was not set shows
// as "-" (a value the game left at its default is not one it chose).
inline std::string values(const Request& r) {
    char text[200];
    const auto has = [&](int bit) { return (r.fields >> bit) & 1; };
    char amount[24] = "-", kb[32] = "-", impulse[24] = "-", type[16] = "-", category[16] = "-", flags[16] = "-",
         hash[16] = "-";
    if (has(11))
        std::snprintf(amount, sizeof(amount), "%.2f", r.amount);
    if (has(14) || has(15))
        std::snprintf(kb, sizeof(kb), "%s/%s", has(14) ? std::to_string(r.knockback).c_str() : "-",
                      has(15) ? std::to_string(r.knockbackAmount).substr(0, 5).c_str() : "-");
    if (has(23))
        std::snprintf(impulse, sizeof(impulse), "%.1f", r.impulse);
    if (has(9))
        std::snprintf(type, sizeof(type), "%u", r.type);
    if (has(10))
        std::snprintf(category, sizeof(category), "%u", r.category);
    if (has(20))
        std::snprintf(flags, sizeof(flags), "%08x", r.hitFlags);
    if (has(26))
        std::snprintf(hash, sizeof(hash), "%08x", r.hash);
    std::snprintf(text, sizeof(text), "dmg %s type %s cat %s kb %s flags %s imp %s hash %s set %llx", amount, type,
                  category, kb, flags, impulse, hash, static_cast<unsigned long long>(r.fields & 0xfffffffffull));
    return text;
}
// Class names in lines of at most `width` characters, each starting with `prefix`.
inline std::vector<std::string> lines(const std::string& prefix, const std::vector<std::string>& names,
                                      size_t width = 170) {
    std::vector<std::string> out;
    std::string line = prefix;
    for (const auto& n : names) {
        if (line.size() + n.size() + 1 > width && line.size() > prefix.size()) {
            out.push_back(line);
            line = prefix;
        }
        line += (line.size() > prefix.size() ? "," : "") + n;
    }
    if (line.size() > prefix.size() || out.empty())
        out.push_back(line);
    return out;
}
inline std::string join(const std::vector<std::string>& names) {
    std::string s;
    for (const auto& n : names)
        s += (s.empty() ? "" : ",") + n;
    return s.empty() ? "none" : s;
}

// The non-zero 4-byte words of a block as "offset:value" (hex), pointers left out (a heap address differs
// from one run to the next and says nothing): a qword whose high half is 1 to 0x7fff and whose low half is at
// least 0x10000. Offsets count from `first`.
inline std::vector<std::string> words(const uint8_t* bytes, uint32_t count, uint32_t first = 0) {
    std::vector<std::string> out;
    char text[32];
    for (uint32_t at = 0; at + 4 <= count; at += 4) {
        uint32_t w{};
        std::memcpy(&w, bytes + at, 4);
        if (!w)
            continue;
        if (at % 8 == 0 && at + 8 <= count) {
            uint32_t hi{};
            std::memcpy(&hi, bytes + at + 4, 4);
            if (hi >= 1 && hi <= 0x7fff && w >= 0x10000) {
                at += 4;
                continue;
            }
        } else if (at % 8 == 4) {
            uint32_t lo{};
            std::memcpy(&lo, bytes + at - 4, 4);
            if (w >= 1 && w <= 0x7fff && lo >= 0x10000)
                continue;
        }
        std::snprintf(text, sizeof(text), "%x:%x", first + at, w);
        out.push_back(text);
    }
    return out;
}
// What `now` has that `before` has not (+) and what it lost (-), as "+A,+B,-C"; empty for none.
inline std::string changes(const std::vector<std::string>& before, const std::vector<std::string>& now) {
    std::string s;
    for (const auto& n : now)
        if (std::find(before.begin(), before.end(), n) == before.end())
            s += (s.empty() ? "+" : ",+") + n;
    for (const auto& b : before)
        if (std::find(now.begin(), now.end(), b) == now.end())
            s += (s.empty() ? "-" : ",-") + b;
    return s;
}
} // namespace spidy::game_record

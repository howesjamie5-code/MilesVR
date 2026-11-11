// The hit recorder's text (game_record_text.hpp): reading a request's bytes and writing its log line.
#include "spidy/game_record_text.hpp"
#include <cstdio>
#include <cstdlib>
using namespace spidy::game_record;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
int main() {
    uint8_t bytes[requestBytes]{};
    const auto put = [&](uint32_t at, auto v) { std::memcpy(bytes + at, &v, sizeof(v)); };
    // The boss punch the game itself sends (recorded on Rhino, October 20), as native_bodies sets it.
    uint64_t fields{};
    const auto set = [&](int bit) { fields |= 1ull << bit; };
    put(0x138, uint32_t(0x00500123u)); set(8);
    put(0x13c, uint32_t(1)); set(9);
    put(0x140, uint32_t(1)); set(10);
    put(0x144, 12.5f); set(11);
    put(0x150, int32_t(3)); set(14);
    put(0x154, 1.f); set(15);
    put(0x168, uint32_t(0x80d02000u)); set(20);
    put(0x19c, uint32_t(0xa621eb5cu)); set(26);
    put(8, uint64_t(fields));
    put(0x18, uint64_t(fields));
    const Request r = parse(bytes);
    CHECK(r.hasDamager && r.damager == 0x00500123u && (r.damager & 0xfffff) == 0x123);
    CHECK(r.type == 1 && r.category == 1 && r.knockback == 3 && r.hitFlags == 0x80d02000u && r.hash == 0xa621eb5cu);
    const std::string text = values(r);
    CHECK(text.find("dmg 12.50") != std::string::npos);
    CHECK(text.find("kb 3/1.000") != std::string::npos);
    CHECK(text.find("flags 80d02000") != std::string::npos);
    CHECK(text.find("hash a621eb5c") != std::string::npos);
    CHECK(text.find("imp -") != std::string::npos); // impulse not set
    CHECK(text.size() < 150);
    // Nothing set: every value shows as not set, no damager.
    uint8_t blank[requestBytes]{};
    const Request none = parse(blank);
    CHECK(!none.hasDamager);
    CHECK(values(none).find("dmg - type - cat - kb - flags - imp - hash -") == 0);
    // Class lists wrap at the width and every name survives.
    std::vector<std::string> names;
    for (int i = 0; i < 40; ++i)
        names.push_back("BotStateSomething" + std::to_string(i));
    const auto wrapped = lines("REC x #1 classes: ", names, 100);
    CHECK(wrapped.size() > 3);
    size_t total{};
    for (const auto& l : wrapped) {
        CHECK(l.size() <= 100 + 1);
        CHECK(l.rfind("REC x #1 classes: ", 0) == 0);
        for (const auto& n : names)
            total += l.find(n + ",") != std::string::npos || l.size() >= n.size() && l.compare(l.size() - n.size(), n.size(), n) == 0;
    }
    CHECK(total >= names.size() - 2);
    CHECK(lines("P: ", {}).size() == 1);
    CHECK(join({}) == "none" && join({"a", "b"}) == "a,b");
    // words(): non-zero words, pointers left out, offsets from `first`.
    uint8_t block[32]{};
    const uint64_t pointerValue = 0x000001a2b3c4d5e8ull;
    std::memcpy(block + 8, &pointerValue, 8);
    const uint32_t small = 5, hi1 = 1;
    std::memcpy(block + 16, &small, 4);
    std::memcpy(block + 20, &hi1, 4); // 0x1_00000005: low half under 0x10000, so not a pointer
    const float f = 1.5f;
    std::memcpy(block + 24, &f, 4);
    const auto w = words(block, 32, 0x100);
    CHECK(w.size() == 3);
    CHECK(w[0] == "110:5" && w[1] == "114:1" && w[2] == "118:3fc00000");
    CHECK(changes({"A", "B"}, {"B", "C"}) == "+C,-A");
    CHECK(changes({"A"}, {"A"}).empty());
    std::puts(failures ? "FAILED" : "ALL record text CHECKS PASSED");
    return failures ? 1 : 0;
}

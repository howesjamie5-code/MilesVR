#pragma once
#include <cstdint>

// The game's Focus for Spidy's blows. Asked October 22, "also check our combat
// is filling up focus for healing": it was not.
//
// Focus is the meter the game's heal and finishers spend, its "spider
// assault" (Hero::HeroSpiderAssaultManager, vtable 38b3f10, component type
// 6dd79c0): segments of {current, most} floats from +0x110, their count at
// +0x128. The game adds to it at 961890, an amount 962210 works out from a
// kind of action and the manager's config (HeroSpiderAssaultEnergies, read
// for each kind from +0x90 on) with the difficulty's multiplier; 961e90 does
// both. The kinds are its own enum's (names at 5ee1400 in the exe): 1
// kMeleeDefault, 2 kMeleeAir, 3 kStealthKill, 4 kDodgeAccurate, 5
// kDodgePerfect, 6 kOtherDamage, ... 15 kYankBot, 17 kThrowBot, 19
// kThrowObjectIntoBot, 22 kBotFlungIntoBot, ... 29 kAirTrick.
//
// Where its own blows get theirs: the manager answers the hero's
// DamageDealtEvent (962a00) by the damage's DamageHash and the custom data the
// game's attacks attach to a request (1ed2620 reads it by name hash). Spidy's
// blows are plain requests through the DamageSystem (native_bodies::damage),
// with neither, and fall through that handler to kOtherDamage, whose amount
// 962210 leaves at 0 (no case of its own: read from the exe on October 22).
// So no fist, web strike, counter, yank or throw of Spidy's ever filled Focus;
// what did is the game's own moves in VR (its dodge, the game's Web Shooter
// since October 22), through handlers of their own.
//
// So each of Spidy's blows on an enemy now asks the game for the Focus its own
// blow of that kind earns: earn() from the blow's thread, given on the main
// thread (frame(), in the camera's update after the hero's, where game_warp
// asks the hero's state machine) through 961e90, so the config, the
// difficulty and the perks decide how much, as for the game's own.
namespace spidy::game_focus {
enum class Kind : uint32_t {
    meleeDefault = 1,
    meleeAir = 2,
    yankBot = 15,
    yankBotIntoAirBonus = 16,
    throwBot = 17,
    throwObjectIntoBot = 19,
    botFlungIntoBot = 22,
};
// SpidyFocusData, for the session report.
struct Data {
    uint32_t magic = 0x53464344, version = 1, bytes = sizeof(Data), status{}; // 0 off, 2 running, 3 another build
    int64_t sequence{};
    // Blows that asked for Focus; those the game was given; those that found
    // no meter (no hero component, a full queue, a fault).
    uint64_t asked{}, given{}, missed{};
    // The meter after the latest given: its segments' fill and their most.
    float now{}, most{};
};
static_assert(sizeof(Data) == 56);
// Checks this game build (0, or 9701 another build) and keeps the image base.
// The swing starts it for the punches and the web grab.
uint32_t start(uintptr_t base);
void stop();
// A blow of `kind` the actor `damager` (the player's record) landed on an
// enemy. Any thread; the game hears of it in the next frame().
void earn(uint64_t damager, Kind kind);
// The game's main thread: what was earned since the last frame goes to the
// meter of the damager's own HeroSpiderAssaultManager.
void frame();
Data data();
} // namespace spidy::game_focus

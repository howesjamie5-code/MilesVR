#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

// The local player, found in the game's own component registry (decoded at
// 16798f0; tools/capture_game_state.py reads the same table from outside).
// Loading a save, a respawn or a switch to Peter, MJ or Miles replaces the
// player's actor and components, so a player found once goes stale.
namespace spidy::game_player {
struct Player {
    // Hero component (vtable 38a93c8), its actor record (whose first field
    // points to the actor's transform) and the actor's MoverStandard.
    uint64_t hero{}, record{}, mover{};
    bool operator==(const Player&) const = default;
};
// Exactly one local hero with exactly one movement manager, or false: no
// save is loaded, or a level change has two heroes or none for a moment.
bool find(uintptr_t base, Player& out);
// The player's components are still registered and still belong to its actor.
bool live(uintptr_t base, const Player& player);
// The hero's HeroHealth (vtable 38a6df0: a SegmentedHealth, a Health), a
// component of his actor `record`: his hit points are a float at +0x88 (110
// on a save of October 9, 105 after a probe's blow of 5: reports/
// body-probe.json). 0 for none, or for more than one.
uint64_t heroHealth(uintptr_t base, uint64_t record);
inline constexpr uintptr_t hitPointsAt = 0x88;

// Keeps finding the player on its own thread: a registry scan takes a few
// milliseconds, too long for the headset's frame loop.
class Watch {
  public:
    ~Watch() {
        stop();
    }
    void start(uintptr_t base);
    void stop();
    // The live player (all zero while there is none), and how many times
    // the answer has changed.
    Player current(uint64_t& changes) const;
    // The current player's HeroHealth (heroHealth), 0 while there is none.
    uint64_t health() const;

  private:
    void run();
    uintptr_t base_{};
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stopping_{};
    Player player_{};
    uint64_t changes_{}, health_{};
};
} // namespace spidy::game_player

#pragma once
#include "math.hpp"
#include "spider_sense.hpp"
#include <cstdint>

// The game's own warnings of the attacks about to land on the hero, read for
// the headset's Spider-Sense (spider_sense.hpp). Found from RTTI and
// disassembly, October 18 (docs/REFERENCE.md has the table):
//
// - Every enemy attack the hero may dodge runs the script action
//   HeroDodgeRegisterAttackAction (vtable 38b6840). On its start event
//   (9aeaf0, event a5f8fd64) it registers the attack with the hero's
//   HeroDodgeManager: 92a700 finds the manager by its type (6dd6450) on the
//   hero's actor record and 926600 adds the attack. Its stop event
//   (fce23dc7) takes it off again (928020).
// - The manager (Hero::HeroDodgeManager, vtable 38b23d0, a component of the
//   hero's actor; its own actor record at +0x08) holds up to 16 attacks,
//   0xa8 bytes each from +0x78, their count at +0xaf8. In each: the
//   attacker's actor handle (+0x00), when the hero is to be warned (+0x30, a
//   double on the game's clock), the attack's flags (+0x5c: its
//   BotAttackDodgeData Properties in the low bits, spider_sense.hpp's
//   attack::), its state (+0x60) and when its blow lands (+0x80; -1 when the
//   action set no time).
// - Registering, the manager sets the warning its DodgeTime and a 30th of a
//   second before the blow, or at once. At that moment (926580) it plays the
//   hero's Spider-Sense sound and sets the state's 0x10 (warned) in place of
//   0x20 (waiting): the moment the flat game's mask lights
//   (HeroCounterableDodgeWindowOpenEvent, which the hero's
//   HeroSpiderSenseManager answers at 966dd0).
// - The game's clock: a double at 7a7fbd0, seconds of game time; the
//   manager's times are on it. Its scale (7a7fb90, game_time.hpp) slows it.
namespace spidy::game_sense {
constexpr uintptr_t managerType = 0x78415e0, managerName = 0x3e10c70, managerTable = 0x3e10c20,
                    clockRva = 0x8435910;
// MilesMorales.exe: the attacks at +0x58 and their count at +0xad8, 0x20 before
// Spider-Man.exe's +0x78 and +0xaf8 (its manager's own code: ad18c4, ad3564,
// ad0d60); an attack is 0xa8 bytes in both.
constexpr uint32_t attackSlots = 16, attackBytes = 0xa8, attacksAt = 0x58, countAt = 0xad8;
constexpr uint32_t warnedState = 0x10, waitingState = 0x20;
// One attack as the manager holds it.
struct Attack {
    uint32_t handle{}, flags{}, state{};
    double warnAt{}, impactAt{};
};
// The hero's manager and its attacks now, and the game's clock: false when
// they cannot be read. `manager` keeps the manager found for `hero` (his
// actor record) from call to call; 0 to look again.
bool read(uintptr_t base, uint64_t hero, uint64_t& manager, Attack (&out)[attackSlots], uint32_t& count,
          double& now);
// The actor record an actor handle names, as the game resolves one
// (15a0560): 0 for none.
uint64_t actorOf(uintptr_t base, uint32_t handle);
// An actor record's place: its transform's translation.
bool positionOf(uint64_t record, Vec3& out);

// Wire format: SpidySenseData, SpidySenseSample. The warnings standing at
// the latest input sample, for the headset to draw, and what came of them,
// for the session report.
struct Threat {
    uint64_t attacker{}; // an actor record
    Vec3 head{}, middle{};
    float toImpact{}, warned{}; // game seconds (SenseThreat)
    uint32_t flags{}, state{};  // the attack's, as the manager holds them
    // SenseAnswer; whether a time is set; whether a counter broke it; and
    // 1..0 of its time left a notch at a time (senseLeft).
    uint8_t answer{}, timed{}, countered{}, reserved{};
    float left{};
};
static_assert(sizeof(Threat) == 56);
struct Data {
    uint32_t magic = 0x53534e53, version = 1, bytes = sizeof(Data), status{};
    int64_t sequence{};
    // Input samples read, those in which the hero's manager was read, and
    // reads that faulted or found it gone; managers found (one for each new
    // hero); the most attacks it held at once, and the attacks it holds now.
    uint64_t samples{}, read{}, faults{};
    uint32_t managers{}, mostAttacks{}, attacks{}, count{};
    uint64_t manager{}; // the hero's HeroDodgeManager now (0: none)
    double now{};       // the game's clock at the latest sample
    // Warnings that went off, and by what they asked for (SenseAnswer:
    // none, counter, dodge, jump); and attacks the manager held whose
    // attacker named no actor in play.
    uint64_t warnings{}, asked[4]{}, unknown{};
    // Counters: made by a fist thrown at him, made by a blow landing while
    // the warning stood; of them the perfect ones (thrown in its last fifth
    // of a second); the counters' blows that landed after them, and the
    // breaks sent (the game's flinch at the moment a fist is thrown).
    uint64_t thrown{}, landed{}, perfect{}, counterBlows{}, breaks{};
    // The latest counter: at whom, his middle, the warning's time left
    // (game seconds; below 0 none set), the hand (0 left, 1 right, 2 a blow).
    uint64_t lastCounter{};
    Vec3 lastPoint{};
    float lastLeft{};
    uint32_t lastHand{}, reserved{};
    Threat threats[6]{};
    // What the game's warnings were made of: up to 8 different flags, each
    // with how often it went off and the longest warning it gave (game
    // seconds from the warning to the blow).
    uint32_t censusFlags[8]{}, censusCounts[8]{};
    float censusLead[8]{};
};
static_assert(sizeof(Data) == 632);
// Publishes the latest, as the export's readers expect (an odd sequence
// while it changes).
void publish(const Data& data);
Data data();
} // namespace spidy::game_sense

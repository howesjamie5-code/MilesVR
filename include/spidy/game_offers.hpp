#pragma once
#include "math.hpp"
#include <cstdint>

// The game's own interactions - a vent, a door, a prompt - where Spidy's own
// wall holds the player: the game is made to see him crawling there, and the
// player picks one by pointing at it.
//
// Spidy's wall runs the hero in the game's air state, a body 0.9 m clear of
// the wall, and the game offers nothing there (October 13 and 14, the first
// mission's vent). Three gates shut it, each read from the game on the hero
// beside that vent:
//
// - The hero's Hero::HeroOfferProcessor takes offers of the kinds in its mask
//   (+0x1c8) only: every kind on the ground, 0x23 on the game's crawl, 0x2 in
//   the air. Its update (857360) reads that mask as it gathers its candidates
//   and checks them.
// - Each offer's Offer::IsAvailableTo (1e6e400) takes the hero's record and
//   reads where he stands from its transform: a vent's range (its vtable
//   +0xe8, 6bb460) wants him within 1.25 m of its opening, which a body held
//   off the wall below it is not.
// - A vent's own entry check (+0x128, 6bb650), past which side of the vent
//   the hero stands, asks whether he carries the crawl's state component
//   (HeroStateWallCrawlBaseLocal): hero states are components, and only the
//   crawl's makes a vent enterable.
//
// While the swing reports its own wall holding the player (hold), the update
// runs with the crawl's mask, each offer's check gets a copy of the hero's
// record whose transform is the crawl's - his feet on the wall where the
// swing holds him, his up its normal - and the vent's own check is the
// crawl's answer. Nothing the rest of the game reads is changed: the mask
// goes back when the update is done, and the record is never written.
//
// The game's gather (855460) finds the offers within 3.35 m of the hero and
// lists those its check says yes to - and those it would show as not yet
// usable, by two checks of its own on the real hero (its vtable +0x58 and
// 1e6e610), which a player who swung up a wall from below never passed: the
// first mission's vent, 2 m above his feet and so out of its range, was never
// listed and nothing showed (October 15, the player's session at 11:15). So
// the offers here are every one the gather checked for the player, whatever
// it answered. And for the one a hand points at (aim) that is out of his
// reach where he stands, the wall around it is tried with the same check for
// a spot where the game says yes, within reach of him: a use puts the game's
// own crawl there (game_warp's crawl entry, measured moving the hero 1.3 m in
// 0.18 s), where the game picks the offer itself and its interact runs it -
// the game's own vent entry, from its own crawl.
//
// The game's own pick of the one to offer (its "best", +0x1ac) judges from
// the hero's real place and picks nothing on Spidy's wall (October 15). So
// the player picks: the headset's hand points at one (game_xr), and the
// player's interact on it asks for its use (use).
namespace spidy::game_offers {
// One offer the game gathered near the player.
struct Offer {
    uint32_t handle{}, kind{};
    // Why the game says no to the player, as its own check last answered
    // (Offer +0xf4/+0xf8, +0xfc picks): 0 is yes. And whether it is on.
    uint32_t reasons{}, enabled{};
    Vec3 position{}; // the offer's actor
    Vec3 axis{};     // its third row: the way a vent opens
    // Out of his reach, the one aimed at: a spot on the wall within his reach
    // where the game says yes (Data::spot). A use goes there.
    uint32_t reachable{};
};
static_assert(sizeof(Offer) == 44);
constexpr unsigned maxOffers = 8;
struct Data {
    uint32_t magic = 0x534f4644, version = 5, bytes = sizeof(Data), status{}; // 0 off, 1 hooked, 3 failed
    int64_t sequence{};
    // The processor's updates, and those run with the crawl's mask for
    // Spidy's wall; offers checked for the player, and those checked against
    // the crawl's pose; the vents' own entry checks, and those answered for
    // the wall.
    uint64_t updates{}, masked{}, checks{}, posed{}, ventChecks{}, ventAnswered{};
    // The game's own mask at the latest update; why start failed.
    uint32_t mask{}, error{};
    // Spidy's wall holds the player as the swing last said, and a probe's
    // override (0 the swing's word, 1 held, 2 not held).
    uint32_t held{}, forced{};
    // Where the crawl's pose stood him last, and the wall's normal.
    Vec3 point{}, normal{};
    // The offers the game checked for the player at its latest update - its
    // own candidates first - and the one it picked to offer him (its best,
    // +0x1ac; 0 none): on its own crawl, the one its interact uses.
    uint32_t offers{}, best{};
    Offer nearby[maxOffers]{}; // not "near": a macro of windef.h
    // Uses asked for (use), handed to the swing (takeUse), and how those
    // ended: the game's crawl took the player, or the swing kept him.
    uint64_t usesAsked{}, usesStarted{}, usesTaken{}, usesDeclined{};
    // The offer of the latest use, and the one the headset aims at now.
    uint32_t lastUse{}, aimed{};
    // The spot found for the aimed offer (spotFound): where the crawl would
    // stand him, on the wall.
    Vec3 spot{};
    uint32_t spotFound{};
    // Masked updates whose gather checked any offer for the player, the most
    // it checked at once; spot searches, and those that found one.
    uint32_t gathered{}, mostChecked{}, searches{}, spotsFound{};
};
static_assert(sizeof(Data) == 544);

// The crawl's offer mask (Hero::HeroOfferProcessor +0x1c8 on the game's
// crawl, October 14).
constexpr uint32_t crawlMask = 0x23;
// The farthest a use moves the player to a spot of the aimed offer's.
constexpr float spotReach = 2.5f;

// Hooks the processor's update, the offers' check and the vent's. 0 when
// ready (or already); 9601 when these are not this game build's functions;
// 97xx from MinHook.
uint32_t start(uintptr_t base);
void stop();
// The swing, every visit: the player's actor record, and whether its own
// wall holds him - then the point of the wall under his feet (where the
// game's crawl would have them) and the wall's normal. A word older than a
// fifth of a second is no.
void hold(uintptr_t record, bool onSpidysWall, Vec3 point = {}, Vec3 normal = {});
// The headset, every frame: the offer a hand points at (0 none).
void aim(uint32_t handle);
// The headset: the player's interact on this offer (one of Data::nearby, the
// game saying yes where he stands or at its spot). False when it is not one
// the game offers him now.
bool use(uint32_t handle);
// The swing, on its wall: a use asked for within the last half second, and
// the point to put the game's crawl at for it: where the offers saw him, or
// the offer's spot. Once.
bool takeUse(Vec3& point);
// How the use the swing took ended: the game's crawl has the player, or not.
void usedOutcome(bool taken);
Data data();
} // namespace spidy::game_offers

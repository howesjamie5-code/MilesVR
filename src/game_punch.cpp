// Punches in the game: fists from the swing's input samples, bots from the
// target watch, blows as the game's own damage; and fighting across the
// room, the web strike and the flow (game_punch.hpp).
#include "spidy/game_punch.hpp"
#include "spidy/game_focus.hpp"
#include "spidy/game_grab.hpp"
#include "spidy/game_guard.hpp"
#include "spidy/game_record.hpp"
#include "spidy/game_sense.hpp"
#include "spidy/game_sound.hpp"
#include "spidy/game_targets.hpp"
#include "spidy/game_time.hpp"
#include "spidy/guard_pose.hpp"
#include "spidy/native_bodies.hpp"
#include "spidy/venom_punch.hpp"
#include "spidy/spider_sense.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>
using namespace spidy;
using namespace spidy::game_punch;
extern "C" {
__declspec(dllexport) Data SpidyPunchData;
__declspec(dllexport) Strikes SpidyStrikeData;
}
namespace {
// A street thug from the feet up (other bots by their size, as
// game_targets::Size carries it over), and how far from a fist one of his
// height may stand to be worth testing this sample.
constexpr float botHeight = 1.8f, botRadius = .3f, reach = 3;
// The kinetic blow the game answers with its flight, as the web grab deals
// it (game_grab.cpp's knock): KnockbackAmount and ImpactImpulse.
constexpr float flingAmount = 10, flingImpulse = 30;
// MilesVR 0.1.78: a boss (game_targets::boss) takes the damage of a blow but is never flung by it - no
// push and no flying knockback (Rhino "get flung sometimes"); at most it staggers him.
bool bossRecord(uint64_t record);
// And it carries what the game's own punch on a boss carries (recorded on Rhino, October 20: hash a621eb5c
// for a punch, f3b5304e for a bigger blow, category 1, hit flags 0x80d02000, knockback 3 at 1), so he
// plays his own reaction to it ("they just dont act like they react to it").
constexpr uint32_t bossPunchHash = 0xa621eb5c, bossHeavyHash = 0xf3b5304e, bossCategory = 1,
                   bossHitFlags = 0x80d02000;
native_bodies::Damage tamedForBoss(native_bodies::Damage request) {
    if (!bossRecord(request.victim))
        return request;
    const bool heavy = request.type == 7 || request.knockback >= static_cast<int32_t>(Knockback::Knockdown);
    request.impulse = -1;
    if (request.type == 7) // kKinetic
        request.type = 1;  // kMelee
    request.knockback = static_cast<int32_t>(Knockback::Knockback);
    request.knockbackAmount = 1;
    if (!request.hash)
        request.hash = heavy ? bossHeavyHash : bossPunchHash;
    request.category = bossCategory;
    request.hitFlags = bossHitFlags;
    return request;
}
// A blow's velocity goes to his flight in this many samples once the game
// has him on it: the blow is issued at the next physics step and the flight
// starts with the game's own velocity, away from the hero, a frame or two
// later. A thug not on one flightWait after the blow is asked onto it once
// (a fighting thug's AI may refuse: native_bodies::fling).
constexpr int flightWrites = 5;
constexpr float flightWait = .4f;
// Enemies this much beyond a strike's reach stay known, so one knocked out
// of it during a flight is still its target.
constexpr float beyondReach = 3;
uintptr_t base{};
std::atomic<bool> enabled{};
SRWLOCK lifecycle = SRWLOCK_INIT, updating = SRWLOCK_INIT, output = SRWLOCK_INIT;
game_targets::Watch watch;
std::vector<game_targets::Candidate> bots;
std::vector<PunchTarget> inReach;
std::vector<PunchEvent> events;
Punches punches{PunchConfig{}};
// The tracking space's orientation at the last sample: the right stick turns
// it, a flip tilts it.
Quat lastTurn{};
bool turnSeen{};
// Fighting across the room. The enemies alive within a strike's reach at the
// latest sample; every bot near the player or a fist then, with the state
// machine his flight is steered through; and the blows' velocities waiting
// for their thugs' flights.
WebStrike strike{WebStrikeConfig{}};
Flow flow{FlowConfig{}};
// Called blows: what the flow calls, and the player's head at the latest
// sample (which way he sees the one he hits).
Cues cues{CueConfig{}};
Vec3 headAt{};
// The enemy his next punch goes to: the one a blow locked him onto, else the
// one he looks at within a strike's reach (web_strike.hpp's Lock).
Lock lock{LockConfig{}};
// The enemies his blows landed on lately, each with where his chest was: one
// of them out of hit points is a knockout, and the player's.
struct Struck {
    uint64_t record{};
    Vec3 chest{};
    float age{};
};
std::vector<Struck> struckLately;
// MilesVR 0.1.80, v18: the enemies a blocked blow made flinch lately, and when (GetTickCount64; touchBlocked).
struct Touched {
    uint64_t record{};
    ULONGLONG at{};
};
std::vector<Touched> touched;
// MilesVR 0.1.80, v21: the counters enemies have begun (the repeated blocked punches' answer) that have not reached
// the hero yet, and when each does (GetTickCount64).
struct Retaliation {
    uint64_t record{};
    ULONGLONG due{};
};
std::vector<Retaliation> retaliations;
constexpr float struckSeconds = 3, chestHeight = 1.3f;
std::vector<FightTarget> enemies;
struct Near {
    uint64_t record{}, machine{};
};
std::vector<Near> nearby;
struct Launch {
    uint64_t record{}, machine{};
    Vec3 velocity{};
    float age{};
    int writes{};
};
std::vector<Launch> launches;
Player fighter;
Strikes tally;
// The venom punch (game_punch.hpp's Venom): the charge the headset module
// holds out and the meter's address, the charge a blow spent last, and what
// has landed.
std::atomic<uint32_t> venomCharge{}, venomHand{1};
// MilesVR 0.1.78: the story has not given Miles his venom yet (Venom::locked): no blow or dodge fills the meter.
std::atomic<bool> venomLocked{};
std::atomic<uint32_t> venomLocks{};
std::atomic<uint64_t> venomMeter{}, venomHolder{}, venomHolderClass{};
uint32_t venomUsed{};
Venom venomOut;
// The venom smash: the drop to the ground, then the blast. And the Mega
// Venom Blast's double punch.
venom_punch::SmashDrop smash;
// The Mega Venom Blast's requests (Venom::megaRequest), and the one answered.
std::atomic<uint32_t> megaRequest{};
uint32_t megaAnswered{};
// The headset's dodges and perfect dodges (Venom::dodges), and those answered.
std::atomic<uint32_t> dodgeRequest{}, perfectRequest{};
uint32_t dodgesAnswered{}, perfectAnswered{};
// Seconds since an enemy was in the fight (the headset's calm, the same way):
// the Venom Jump goes off only in a fight.
float sinceFight = 1e9f;
// A venom blow: a finisher's damage at least, times this, and his flight.
constexpr float venomDamage = 3, venomLeast = 28, venomSpeed = 18, venomLift = 5;
// DamageType kVenom and the venom status: Miles Morales's own, after
// Remastered's 0-30 (its DamageType enum: kVenom 31, kAntiVenom 32,
// kAntiCamo 33).
constexpr uint32_t venomType = 31;
constexpr int32_t venomStatus = 31;
constexpr float venomStatusAmount = 100, venomStatusSeconds = 3;
// A charged right fist thrown at the marked enemy this near (from the
// player's chest to his middle) lands the venom punch there and then: in
// Miles Morales a fist seldom meets a thug by itself (0.1.6 to 0.1.8: 3
// punches landed in ten minutes of fighting, against 33 web strikes), and in
// 0.1.8 the charge ran out on the user's punches. Farther, the web strike
// flies him in and its blow is the venom punch.
constexpr float venomReach = 2;
bool venomArmed() {
    const uint32_t charge = venomCharge.load();
    return charge && charge != venomUsed;
}
// Spider-Sense (spider_sense.hpp): the attacks the game warned the hero of
// at the latest sample, the counters made at them, and what the session
// report and the headset read of them (game_sense::Data). A counter's
// attacker is the strike's mark for a moment after the fist goes out, so the
// strike it opens flies to him whomever the player was locked onto.
SpiderSense sense{SpiderSenseConfig{}};
std::vector<SenseThreat> threats;
std::vector<uint32_t> threatStates; // each one's state in the manager
game_sense::Attack attacksRead[game_sense::attackSlots];
uint64_t senseManager{};
game_sense::Data senseTally;
uint64_t counterWish{};
float counterWishLeft{};
constexpr float counterWishSeconds = .15f;
// The warnings seen go off, by attacker and when the game warned: each
// counted once.
struct Warned {
    uint64_t attacker{};
    double at{};
};
std::array<Warned, 16> warnedLately{};
size_t nextWarned{};
// A thug's head is this high (above his feet, carried over to other bots
// by their size), his middle this.
constexpr float headHeight = 1.75f, middleHeight = 1;
// A counter's blow sends him off along it this fast (real m/s; a heavy
// blow's 9-14, DeliveryConfig) and this fast up, and hits this much harder.
constexpr float counterSpeed = 11, counterLift = 3, counterDamage = 1.25f;
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
// The meter set to `value`, if the component it lies in (game_venom) is still there. 0.1.15 wrote
// into whatever the game had put after the HeroMeleeManager and crashed it.
bool holderThere(uint64_t meter) {
    const uint64_t holder = venomHolder.load(), expected = venomHolderClass.load();
    uintptr_t vtable{};
    return meter && holder && meter > holder && meter - holder < 0x1000 && read(holder, &vtable, 8) &&
           vtable == expected;
}
// Miles's bars at the meter (venom_punch::Bars: one early in the story, three later).
bool readBars(uint64_t meter, venom_punch::Bars& out) {
    float words[venom_punch::mostBars * 2]{};
    if (!meter || !read(meter, words, sizeof(words)))
        return false;
    out = venom_punch::bars(words);
    return true;
}
// The bars' values written back, the holder checked first.
bool writeBars(uint64_t meter, const venom_punch::Bars& b) {
    if (!holderThere(meter))
        return false;
    __try {
        for (unsigned i = 0; i < b.count; ++i)
            reinterpret_cast<float*>(meter)[i * 2] = b.value[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// A venom move's cost: one bar off the whole (venom_punch::paid).
bool spendBar(uint64_t meter) {
    venom_punch::Bars b;
    return readBars(meter, b) && writeBars(meter, venom_punch::paid(b));
}
// A blow of the player's charges the bars in order (venom_punch::gain).
void chargeMeter(uint64_t meter, float amount) {
    if (venomLocked.load())
        return;
    venom_punch::Bars b;
    if (readBars(meter, b))
        writeBars(meter, venom_punch::gained(b, amount));
}
template <class F> void publish(F change) {
    AcquireSRWLockExclusive(&output);
    InterlockedIncrement64(&SpidyPunchData.sequence);
    change(SpidyPunchData);
    const auto bodies = native_bodies::counters();
    SpidyPunchData.issued = bodies.damages;
    SpidyPunchData.dropped = bodies.damageDropped;
    SpidyPunchData.unnamed = bodies.damageUnnamed;
    InterlockedIncrement64(&SpidyPunchData.sequence);
    ReleaseSRWLockExclusive(&output);
}
// The tally as it stands, with the strike's and the flow's state.
void publishStrikes(uint32_t status) {
    tally.status = status;
    tally.phase = static_cast<uint32_t>(strike.phase());
    tally.hand = static_cast<uint32_t>(strike.hand());
    tally.flow = static_cast<uint32_t>(flow.count());
    tally.enemies = static_cast<uint32_t>(enemies.size());
    tally.target = strike.phase() == WebStrike::Phase::Flying ? strike.target() : 0;
    tally.destination = strike.destination();
    tally.seconds = strike.elapsed();
    // The mark over the chest of the one the next punch would go to, with
    // what the flow calls.
    tally.cueTarget = 0;
    tally.cueWay = tally.cueFinisher = tally.cueLocked = 0;
    tally.cueLeft = 0;
    tally.locked = lock.locked() ? lock.target() : 0;
    tally.passedOver = strike.passedOver();
    for (const auto& e : enemies)
        if (lock.target() && e.id == lock.target()) {
            tally.cueTarget = e.id;
            tally.cueLocked = lock.locked();
            tally.cuePoint = e.feet + Vec3{0, e.height * .72f, 0};
            tally.cueWay = static_cast<uint32_t>(cues.cue().way);
            tally.cueFinisher = cues.cue().finisher;
            tally.cueLeft = cues.cue().way == CueWay::None ? 1.f : flow.left();
        }
    AcquireSRWLockExclusive(&output);
    // Odd while it changes, as a reader outside the game expects.
    tally.sequence = InterlockedIncrement64(&SpidyStrikeData.sequence);
    SpidyStrikeData = tally;
    InterlockedIncrement64(&SpidyStrikeData.sequence);
    ReleaseSRWLockExclusive(&output);
}
void resetFight() {
    smash.cancel();
    sinceFight = 1e9f;
    punches.reset();
    turnSeen = false;
    strike.cancel();
    flow.reset();
    cues.clear();
    lock.clear();
    struckLately.clear();
    touched.clear();
    retaliations.clear();
    enemies.clear();
    nearby.clear();
    launches.clear();
    fighter = {};
    sense.reset();
    threats.clear();
    threatStates.clear();
    senseManager = 0;
    counterWish = 0;
    counterWishLeft = 0;
}
// The warnings the game's HeroDodgeManager holds for the player now, with
// where each attacker is: a bot's feet and size from the target watch, else
// his actor record's place. A new one goes into the census.
void readSense(const Vec3& chest) {
    threats.clear();
    threatStates.clear();
    ++senseTally.samples;
    uint32_t n{};
    double now{};
    if (!fighter.record || !game_sense::read(base, fighter.record, senseManager, attacksRead, n, now)) {
        senseTally.faults += fighter.record != 0;
        senseTally.manager = 0;
        senseTally.attacks = 0;
        return;
    }
    if (senseManager != senseTally.manager)
        ++senseTally.managers;
    senseTally.manager = senseManager;
    ++senseTally.read;
    senseTally.now = now;
    senseTally.attacks = n;
    senseTally.mostAttacks = std::max(senseTally.mostAttacks, n);
    for (uint32_t i = 0; i < n; ++i) {
        const auto& a = attacksRead[i];
        if (!(a.state & game_sense::warnedState) || !std::isfinite(a.warnAt))
            continue;
        const uint64_t record = game_sense::actorOf(base, a.handle);
        Vec3 feet{};
        game_targets::Size size{};
        bool placed{};
        for (const auto& b : bots)
            if (record && b.record == record) {
                placed = game_targets::position(b, feet);
                size = b.size;
                break;
            }
        if (!record || (!placed && !game_sense::positionOf(record, feet))) {
            ++senseTally.unknown;
            continue;
        }
        SenseThreat t;
        t.attacker = record;
        t.head = feet + Vec3{0, size.height(headHeight), 0};
        t.middle = feet + Vec3{0, size.height(middleHeight), 0};
        t.timed = std::isfinite(a.impactAt) && a.impactAt >= 0;
        t.toImpact = t.timed ? static_cast<float>(a.impactAt - now) : 0.f;
        t.warned = std::max(static_cast<float>(now - a.warnAt), 0.f);
        t.flags = a.flags;
        // Nothing the manager would hold for long: a minute-old warning or
        // one whose blow is a minute off is something else.
        if (t.warned > 60 || (t.timed && std::abs(t.toImpact) > 60)) {
            ++senseTally.unknown;
            continue;
        }
        threats.push_back(t);
        threatStates.push_back(a.state);
        // A warning not seen go off before: counted, by what it asks for,
        // and into the census with how long it gave the player.
        const bool known = std::any_of(warnedLately.begin(), warnedLately.end(), [&](const Warned& w) {
            return w.attacker == record && std::abs(w.at - a.warnAt) < 1e-3;
        });
        if (!known) {
            warnedLately[nextWarned++ % warnedLately.size()] = {record, a.warnAt};
            ++senseTally.warnings;
            const auto asked = answerOf(t.flags, length(t.middle - chest), sense.config());
            ++senseTally.asked[static_cast<size_t>(asked) & 3];
            const float lead = t.timed ? t.warned + std::max(t.toImpact, 0.f) : -1.f;
            size_t slot = 8;
            for (size_t c = 0; c < 8; ++c)
                if (senseTally.censusCounts[c] && senseTally.censusFlags[c] == t.flags) {
                    slot = c;
                    break;
                } else if (!senseTally.censusCounts[c] && slot == 8) {
                    slot = c;
                }
            if (slot < 8) {
                senseTally.censusFlags[slot] = t.flags;
                ++senseTally.censusCounts[slot];
                senseTally.censusLead[slot] = std::max(senseTally.censusLead[slot], lead);
            }
        }
    }
}
bool bossRecord(uint64_t record) {
    for (const auto& b : bots)
        if (b.record == record)
            return (b.traits & game_targets::boss) != 0;
    return false;
}
bool bossStunned(uint64_t record) {
    for (const auto& b : bots)
        if (b.record == record)
            return game_targets::isStunned(base, b);
    return false;
}
// MilesVR 0.1.80, v18 (game_guard): the bot of the target watch with this actor record, null for none.
const game_targets::Candidate* botOf(uint64_t record) {
    for (const auto& b : bots)
        if (b.record == record)
            return &b;
    return nullptr;
}
// He is in the middle of an attack the game warned the hero of. (Until v22 his guard was open to a blow then; v23: it
// holds, and a blocked blow staggers him - v25: only with guard.txt's `touch on`; otherwise he finishes his attack.)
bool attackingNow(uint64_t record) {
    for (const auto& t : threats)
        if (t.attacker == record && standing(t, sense.config()))
            return true;
    return false;
}
// A blow a guard stopped does nothing to him and counts for nothing in the flow (v29: it ends the flow, endCombo
// below), but it charges the venom meter a little (half a plain blow's gain): the venom that breaks a guard has to
// come from somewhere, or an enemy who is all guard could never be hurt.
constexpr float blockedGain = .5f;
// And the enemy takes it: the game's smallest hit reaction (Twitch) with next to no damage, so a blocked blow is
// seen to meet him. (v21: his arms come up as well - the mod's own pose, guard_pose.hpp - for the game's own block
// state is one his AI enters on its own attacks' terms.) Once in touchGap for each enemy. v25: only when guard.txt
// says `touch on` (see below).
// v23 (the user, November 8: "if i punch them as they are attacking, their attack should be cancelled and they block
// me, and if thats not possible, they block me and follow through the attack"): a blow his guard blocks while he is
// in the middle of an attack staggers him (the game's Stagger reaction, as hard as a plain punch's smallest): his
// attack is stopped by the game's own hit reaction when it will be. If the game's reaction lets it go on, he finishes
// it - blocked all the same (guard.hpp). v24: that is all it does. In v23 the next such blow knocked him back as well,
// as a Spider-Sense counter's break does (breakAttack), and the user said "i punch them once, it gets blocked, and
// then none of my next punches register": a fighter flung out of reach is a fighter the next fists whiff at.
// v25 (the user, after v24: "they do a staggered animation when you punch them a couple times"): none of this happens
// unless guard.txt says `touch on` (game_guard::touchWanted). In the log of November 8 the game's hit-reaction state
// (BotStateHitReactGame) was under 24 of the 70 blocked blows; the likeliest source is the Stagger above given to an
// attacker, kept going by the Twitch of the blocks after it. A guard that reels at every punch does not look like one
// that blocks it, and the mod's own pose (guard_pose.hpp: arms up, the body jolted by the blow) is what shows the blow
// meeting him. An attacker finishes his attack, blocked all the same - the user's own alternative of November 8:
// "they block me and follow through the attack".
constexpr float interruptStagger = 2.f;
constexpr float touchDamage = .01f, touchKnock = 1.f;
constexpr ULONGLONG touchGap = 450, touchMemory = 5000;
void touchBlocked(const game_targets::Candidate& bot, const PunchEvent& e, bool attacking) {
    if (!game_guard::touchWanted())
        return;
    const ULONGLONG now = GetTickCount64();
    for (auto it = touched.begin(); it != touched.end();) {
        if (now - it->at > touchMemory)
            it = touched.erase(it);
        else
            ++it;
    }
    const auto found = std::find_if(touched.begin(), touched.end(),
                                    [&](const Touched& t) { return t.record == bot.record; });
    if (found != touched.end()) {
        if (now - found->at < touchGap)
            return;
        found->at = now;
    } else if (touched.size() < 64) {
        touched.push_back({bot.record, now});
    }
    Vec3 way = e.direction;
    way.y = 0;
    if (length(way) < .1f) {
        way = e.point - fighter.feet;
        way.y = 0;
    }
    way = length(way) > 1e-3f ? normalized(way) : Vec3{0, 0, -1};
    native_bodies::Damage request;
    request.victim = bot.record;
    request.damager = fighter.record;
    request.point = e.point;
    request.direction = way;
    request.normal = way * -1.f;
    request.amount = touchDamage;
    request.type = 1; // kMelee
    if (attacking) {
        request.knockback = static_cast<int32_t>(Knockback::Stagger);
        request.knockbackAmount = interruptStagger;
    } else {
        request.knockback = static_cast<int32_t>(Knockback::Twitch);
        request.knockbackAmount = touchKnock;
    }
    native_bodies::damage(request);
}
// MilesVR 0.1.80, v29 (the user, November 9: "if the blocked ui comes up then the combo symbol should disappear, infact
// the combo should reset if you attack a guarding enemy, combo meaning left right up down attacks"): a blow a guard
// stops ends the combo. Until v28 it left the flow where it stood - its count, and the arrow of the blow it called next
// (left, right, up or down, web_strike.hpp's Cues) - so a combo of three went on through a guard that stopped every
// blow after it, with its arrow and its count showing beside the BLOCKED mark. Now the count starts over and the call
// is gone (the headset takes the arrow and the count's marks down with them), and the next blow that lands is the first
// of a new flow. A flow with nothing in it and no call standing has nothing to end, and the log says nothing of it.
void endCombo(const game_targets::Candidate& bot) {
    const int blows = flow.count();
    const CueWay called = cues.cue().way;
    if (blows <= 0 && called == CueWay::None)
        return;
    flow.reset();
    cues.clear();
    char line[160];
    std::snprintf(line, sizeof(line), "COMBO #%u a blocked blow ended the combo (%d blow(s) in it%s)", bot.handle & 0xfffff,
                  blows, called != CueWay::None ? ", a call standing" : "");
    game_record::log(line);
}
void blockedBlow(const game_targets::Candidate& bot, const PunchEvent& e, bool attacking) {
    chargeMeter(venomMeter.load(), venom_punch::gain(false, false) * blockedGain);
    endCombo(bot);
    touchBlocked(bot, e, attacking);
    // Off the ground and on no web, the player stays up for the next blow.
    if (!fighter.grounded && !fighter.swinging)
        strike.hangOn(false);
}
// MilesVR 0.1.80, v21: the guard's answer to blocked punches in a row, as the base game's guarding enemies have it:
// he swings at the hero, and it does no damage - it only staggers him. The swing is seen (guard_pose.hpp: the headset
// side poses his arm when the guard module says it began); it reaches the hero contactSeconds after it began, if
// the hero is still within his reach and his guard has not been opened by then (a venom punch, a gadget, a dodge
// over him: no counter), and then the game gets a stagger-level hit on the hero from the enemy with next to nothing
// of damage in it - the game's own hit reaction does the rest.
constexpr float retaliateDamage = .01f, retaliateKnock = 1.f, retaliateReach = 2.4f;
constexpr ULONGLONG retaliateStale = 1500;
void scheduleCounter(const game_targets::Candidate& bot) {
    if (retaliations.size() >= 8)
        retaliations.erase(retaliations.begin());
    retaliations.push_back({bot.record, GetTickCount64() + static_cast<ULONGLONG>(guard_pose::contactSeconds * 1000.0)});
}
void answerCounters() {
    if (retaliations.empty())
        return;
    const ULONGLONG now = GetTickCount64();
    for (auto it = retaliations.begin(); it != retaliations.end();) {
        if (now < it->due) {
            ++it;
            continue;
        }
        const Retaliation due = *it;
        it = retaliations.erase(it);
        const auto* bot = botOf(due.record);
        Vec3 feet{};
        if (now - due.due > retaliateStale || !bot || !fighter.record || !fighter.grounded ||
            !game_targets::position(*bot, feet) || !game_guard::holds(*bot))
            continue;
        Vec3 way = fighter.feet - feet;
        way.y = 0;
        const float apart = length(way);
        if (apart > retaliateReach)
            continue;
        way = apart > 1e-3f ? way / apart : Vec3{0, 0, 1};
        native_bodies::Damage request;
        request.victim = fighter.record;
        request.damager = bot->record;
        request.point = fighter.feet + Vec3{0, strike.config().chest, 0};
        request.direction = way;
        request.normal = way * -1.f;
        request.amount = retaliateDamage;
        request.type = 1; // kMelee
        request.knockback = static_cast<int32_t>(Knockback::Stagger);
        request.knockbackAmount = retaliateKnock;
        if (native_bodies::damage(request))
            game_guard::counterLanded(*bot);
    }
}
// A counter's break: the game's flinch for the attacker the moment the fist
// goes out at him, so his blow never comes; the blow that lands on him next
// is the counter's (land()).
void breakAttack(const SpiderSense::Counter& c, Vec3 head) {
    native_bodies::Damage request;
    request.victim = c.attacker;
    request.damager = fighter.record;
    request.point = c.point;
    Vec3 way = c.point - head;
    way.y = 0;
    request.direction = length(way) > 1e-3f ? normalized(way) : Vec3{0, 0, -1};
    request.normal = request.direction * -1.f;
    request.amount = fighter.halfPunchDamage ? .5f : 1.f;
    request.type = 1; // kMelee
    request.knockback = static_cast<int32_t>(Knockback::Knockback);
    request.knockbackAmount = 3;
    if (native_bodies::damage(request)) {
        ++senseTally.breaks;
        // v23: a guard's attack is broken, and the punch that follows is blocked all the same: the state this
        // knockback gives him is the mod's own, and does not open his guard to it (game_guard::own).
        if (const auto* bot = botOf(c.attacker))
            game_guard::own(*bot);
    }
}
void noteCounter(const SpiderSense::Counter& c, bool thrown) {
    (thrown ? senseTally.thrown : senseTally.landed) += 1;
    senseTally.perfect += c.perfect;
    senseTally.lastCounter = c.attacker;
    senseTally.lastPoint = c.point;
    senseTally.lastLeft = c.left;
    senseTally.lastHand = c.hand >= 0 ? static_cast<uint32_t>(c.hand) : 2u;
}
// The warnings standing now, the soonest blow first, for the headset.
void publishSense(uint32_t status, Vec3 chest) {
    senseTally.status = status;
    std::vector<const SenseThreat*> shown;
    for (const auto& t : threats)
        if (standing(t, sense.config()))
            shown.push_back(&t);
    std::sort(shown.begin(), shown.end(), [](const SenseThreat* a, const SenseThreat* b) {
        return (a->timed ? a->toImpact : 1e3f) < (b->timed ? b->toImpact : 1e3f);
    });
    senseTally.count = static_cast<uint32_t>(std::min<size_t>(shown.size(), std::size(senseTally.threats)));
    for (uint32_t i = 0; i < std::size(senseTally.threats); ++i) {
        auto& out = senseTally.threats[i];
        out = {};
        if (i >= senseTally.count)
            continue;
        const auto& t = *shown[i];
        out.attacker = t.attacker;
        out.head = t.head;
        out.middle = t.middle;
        out.toImpact = t.toImpact;
        out.warned = t.warned;
        out.flags = t.flags;
        out.state = threatStates[static_cast<size_t>(&t - threats.data())];
        out.answer = static_cast<uint8_t>(answerOf(t.flags, length(t.middle - chest), sense.config()));
        out.timed = t.timed;
        out.countered = sense.countered(t.attacker);
        out.left = senseLeft(t);
    }
    game_sense::publish(senseTally);
}
// The thugs' flights that wait for a blow's velocity.
void steerFlights(float seconds) {
    for (auto it = launches.begin(); it != launches.end();) {
        // A boss is never sent flying (0.1.78: Rhino still "gets flung sometimes" - the flight, not the blow).
        if (bossRecord(it->record)) {
            it = launches.erase(it);
            continue;
        }
        it->age += seconds;
        bool done{};
        if (native_bodies::flung(it->machine)) {
            native_bodies::fling(it->machine, it->record, it->velocity, false);
            tally.flown += !it->writes;
            done = ++it->writes >= flightWrites;
        } else if (it->writes) {
            done = true; // down again already
        } else if (it->age >= flightWait) {
            native_bodies::fling(it->machine, it->record, it->velocity);
            ++tally.unflown;
            done = true;
        }
        it = done ? launches.erase(it) : it + 1;
    }
}
// The player's knockouts: an enemy one of his blows landed on lately who is
// up no longer (game_targets::up: out of hit points, or in one of the game's
// states for a bot who is down).
void knockouts(float seconds) {
    for (auto it = struckLately.begin(); it != struckLately.end();) {
        it->age += seconds;
        const game_targets::Candidate* bot{};
        for (const auto& b : bots)
            if (b.record == it->record)
                bot = &b;
        bool out{};
        if (bot) {
            out = !game_targets::up(base, *bot);
            Vec3 feet{};
            if (!out && game_targets::position(*bot, feet))
                it->chest = feet + Vec3{0, bot->size.height(chestHeight), 0};
        }
        if (out) {
            ++tally.knockouts;
            tally.knockoutPoint = it->chest;
        }
        it = out || !bot || it->age > struckSeconds ? struckLately.erase(it) : it + 1;
    }
}
// The venom smash goes off at `centre`: a venom blow to every enemy within the blast, each thrown up
// and away from it, and one bar of the meter spent. Mega: the Mega Venom Blast's, bigger and harder,
// all three bars spent. Jump: the Venom Jump's, softer, each thrown straight up.
enum class Blast { Smash, Mega, Jump };
void blast(Vec3 centre, Blast kind = Blast::Smash) {
    const bool mega = kind == Blast::Mega, jump = kind == Blast::Jump;
    uint32_t hits{};
    for (const auto& b : bots) {
        Vec3 feet{};
        if ((b.traits & (game_targets::neutral | game_targets::boss)) || !game_targets::enemy(b) ||
            !game_targets::up(base, b) ||
            !game_targets::position(b, feet) ||
            !(mega ? venom_punch::inMega(centre, feet) : venom_punch::inBlast(centre, feet)) ||
            !game_targets::live(base, b))
            continue;
        const Vec3 middle = feet + Vec3{0, b.size.height(1), 0};
        const Vec3 flight = mega   ? venom_punch::megaFlight(centre, feet)
                            : jump ? venom_punch::jumpFlight(centre, feet)
                                   : venom_punch::blastFlight(centre, feet);
        Vec3 way{flight.x, 0, flight.z};
        way = length(way) > .1f ? normalized(way) : Vec3{1, 0, 0};
        native_bodies::Damage request;
        request.victim = b.record;
        request.damager = fighter.record;
        request.point = middle - way * b.size.width(botRadius);
        request.direction = way;
        request.normal = way * -1.f;
        request.amount = venomLeast * venomDamage *
                         (mega   ? venom_punch::megaShare(centre, feet)
                          : jump ? venom_punch::jumpShare(centre, feet)
                                 : venom_punch::blastShare(centre, feet));
        request.type = venomType;
        request.knockback = static_cast<int32_t>(Knockback::SuperFlyBack);
        request.knockbackAmount = flingAmount * 1.5f;
        request.impulse = flingImpulse * 1.5f;
        if (jump) {
            request.direction = Vec3{0, 1, 0};
            request.normal = Vec3{0, -1, 0};
        }
        request.statusType = venomStatus;
        request.statusAmount = venomStatusAmount;
        request.statusDuration = venomStatusSeconds;
        // The venom breaks any guard it reaches (game_guard).
        game_guard::Strike broke;
        broke.venom = true;
        broke.point = middle;
        game_guard::blow(b, broke);
        native_bodies::damage(tamedForBoss(request));
        if (b.machine) {
            const auto waiting = std::find_if(launches.begin(), launches.end(),
                                              [&](const Launch& l) { return l.record == b.record; });
            if (waiting != launches.end())
                *waiting = {b.record, b.machine, flight};
            else
                launches.push_back({b.record, b.machine, flight});
        }
        const auto known = std::find_if(struckLately.begin(), struckLately.end(),
                                        [&](const Struck& s) { return s.record == b.record; });
        const Struck struck{b.record, middle, 0};
        if (known != struckLately.end())
            *known = struck;
        else
            struckLately.push_back(struck);
        ++hits;
    }
    bool paid{};
    if (mega) {
        venom_punch::Bars b;
        paid = readBars(venomMeter.load(), b) && writeBars(venomMeter.load(), venom_punch::paidMega(b));
    } else {
        paid = spendBar(venomMeter.load());
    }
    tally.blows += hits;
    AcquireSRWLockExclusive(&output);
    if (mega) {
        ++venomOut.megas;
        venomOut.megaHits = hits;
        venomOut.megaPoint = centre;
    } else if (jump) {
        ++venomOut.jumps;
        venomOut.jumpHits = hits;
        venomOut.jumpPoint = centre;
    } else {
        ++venomOut.smashes;
        venomOut.smashHits = hits;
        venomOut.smashPoint = centre;
    }
    venomOut.spent += paid;
    ReleaseSRWLockExclusive(&output);
}
// One blow, a fist's or a strike's, into the flow and to the game.
void land(PunchEvent e, bool byStrike) {
    uint64_t machine{};
    for (const auto& n : nearby)
        if (n.record == e.target)
            machine = n.machine;
    const bool airborne = machine && native_bodies::flung(machine);
    // A thug the web is bringing to the player is his to hit from here.
    game_grab::struck(e.target);
    // He is the one from here on, and his knockout would be this blow's.
    lock.take(e.target);
    // MilesVR 0.1.80, v18 (game_guard): whether this blow is a Spider-Sense counter's and whether it is the venom
    // punch is known first; then the guard judges the blow (v21: only the venom breaks a guard; v23: not a
    // Spider-Sense counter either, and not a blow at an enemy in the middle of an attack - he guards then too; with
    // guard.txt's `touch on` a blocked blow staggers him so his attack stops, v25: otherwise he finishes it). A
    // blocked blow ends here: nothing lands, and (v29) the combo is over (blockedBlow: endCombo).
    std::optional<SpiderSense::Counter> made;
    const bool counterBlow = sense.blow(e.target, threats, fighter.feet + Vec3{0, strike.config().chest, 0}, made);
    const uint32_t charge = venomCharge.load();
    const bool venom = e.hand == (venomHand.load() ? 1 : 0) && venomArmed();
    guards::Outcome guarded{};
    if (const auto* bot = botOf(e.target)) {
        game_guard::Strike hit;
        hit.hand = static_cast<uint32_t>(e.hand);
        hit.strength = e.strength;
        hit.venom = venom;
        hit.grounded = fighter.grounded;
        hit.attacking = attackingNow(e.target);
        hit.point = e.point;
        // v30: a swing kick - a dive's blow, whose hand is WebStrike::diveHand - goes through a baton's guard (game_guard).
        hit.kick = byStrike && e.hand == WebStrike::diveHand;
        guarded = game_guard::blow(*bot, hit);
        if (guarded.verdict == guards::Verdict::Blocked) {
            blockedBlow(*bot, e, hit.attacking);
            if (guarded.retaliates)
                scheduleCounter(*bot);
            return;
        }
    }
    {
        Vec3 chest = e.point;
        for (const auto& enemy : enemies)
            if (enemy.id == e.target)
                chest = enemy.feet + Vec3{0, enemy.height * .72f, 0};
        const auto known = std::find_if(struckLately.begin(), struckLately.end(),
                                        [&](const Struck& s) { return s.record == e.target; });
        if (known != struckLately.end())
            *known = {e.target, chest, 0};
        else
            struckLately.push_back({e.target, chest, 0});
    }
    // What the flow calls, and whether this blow answers it. A web strike's
    // blow goes straight at him: what answers is the fist that opened it,
    // and he then goes the way that fist went.
    const Cue before = cues.cue();
    const Vec3 went = byStrike ? strike.direction() : e.direction;
    const bool called = fighter.combatVisuals && cues.answers(went, e.point - headAt);
    if (!fighter.combatVisuals && byStrike) {
        // Keep the player's upward/downward swing when the strike arrives.
        const auto swung = punches.blow(went, e.speed);
        e.direction = swung.direction;
        e.knockback = swung.knockback;
    }
    if (called && byStrike)
        e.direction = normalized(went);
    tally.called += called;
    tally.strikeCalled += called && byStrike;
    tally.missedCalls += fighter.combatVisuals && !byStrike && !called && before.way != CueWay::None;
    // The flow's finisher must be called: a blow that would be it and
    // answers no such call is a plain one that leaves him standing, to be
    // called on, and the count stays.
    const bool uncalled =
        fighter.combatVisuals && flow.finisher(flow.count() + 1) && !(called && before.finisher);
    if (uncalled)
        flow.keep();
    const int count = uncalled ? flow.count() : flow.hit();
    // (The venom punch below takes at least venomLeast whatever this halves: a power of its own.)
    auto d = deliver(e, airborne, count, flow, DeliveryConfig{.halfDamage = fighter.halfPunchDamage},
                     called ? before.way : CueWay::None, cues.config().damage, !fighter.combatVisuals);
    if (uncalled && (d.fly || d.knockback >= Knockback::Knockdown)) {
        d.kind = BlowKind::Hit;
        d.kinetic = d.fly = false;
        d.knockback = Knockback::Knockback;
        d.velocity = {};
    }
    // A counter's blow (spider_sense.hpp): the one after a fist thrown at him
    // while his warning stood, or one that lands while it stands and so
    // counters him itself. It sends him flying, whatever the flow says.
    // (And the blow that breaks a weapon guard under the blows before it is sent the same way, flying: the stagger.)
    const bool strainBreak = guarded.verdict == guards::Verdict::Breaks && guarded.cause == guards::Cause::Strain;
    if (counterBlow) {
        if (made)
            noteCounter(*made, false);
        ++senseTally.counterBlows;
    }
    if (counterBlow || strainBreak) {
        if (d.kind == BlowKind::Hit && (fighter.combatVisuals || verticalBlow(e, airborne) != CueWay::Down)) {
            Vec3 along = e.direction;
            along.y = 0;
            along = length(along) > 1e-3f ? normalized(along) : Vec3{0, 0, -1};
            d.kind = BlowKind::Heavy;
            d.kinetic = d.fly = true;
            d.knockback = Knockback::FlyBack;
            d.velocity = along * counterSpeed + Vec3{0, counterLift, 0};
        }
        d.damage *= counterDamage;
    }
    // The flow's next call: the one that stands, or after an answer another way.
    tally.calls += cues.call(flow.count(), flow.finisher(flow.count() + 1), called,
                             static_cast<uint32_t>((tally.blows + 1) * 2654435761u >> 13));
    // The right fist charged with venom: this blow is the venom punch, and
    // the charge is spent.
    if (venom) {
        venomUsed = charge;
        Vec3 along{e.direction.x, 0, e.direction.z};
        along = length(along) > .1f ? normalized(along) : Vec3{};
        d.kind = BlowKind::Finisher;
        d.damage = std::max(d.damage, venomLeast) * venomDamage;
        d.fly = true;
        d.velocity = along * venomSpeed + Vec3{0, venomLift, 0};
    }
    native_bodies::Damage request;
    request.victim = e.target;
    request.damager = fighter.record;
    request.point = e.point;
    request.direction = e.direction;
    request.normal = e.direction * -1.f;
    request.amount = d.damage;
    if (venom) {
        request.type = venomType;
        request.knockback = static_cast<int32_t>(Knockback::SuperFlyBack);
        request.knockbackAmount = flingAmount * 1.5f;
        request.impulse = flingImpulse * 1.5f;
        request.statusType = venomStatus;
        request.statusAmount = venomStatusAmount;
        request.statusDuration = venomStatusSeconds;
    } else if (d.kinetic) {
        request.type = 7; // kKinetic
        request.knockback = static_cast<int32_t>(Knockback::FlyBack);
        request.knockbackAmount = flingAmount;
        request.impulse = flingImpulse;
    } else {
        request.type = 1; // kMelee
        request.knockback = static_cast<int32_t>(d.knockback);
        request.knockbackAmount = 2 + 6 * e.strength;
    }
    // A plain blow on a boss is the game's own attack (Venom::bossBlows). His venom punch stays the
    // module's own, at once (the user, October 20: the game's Venom Punch "delays the attack").
    const bool onBoss = bossRecord(e.target) && !venom;
    bool landed = false;
    if (onBoss) {
        AcquireSRWLockExclusive(&output);
        venomOut.bossBlows += bossStunned(e.target) ? 1ull << 32 : 1ull;
        ReleaseSRWLockExclusive(&output);
    } else {
        landed = native_bodies::damage(tamedForBoss(request));
    }
    // The Focus the game's own blow earns, in the air its air blow's: the
    // request alone earns none (game_focus.hpp). And the game's own melee
    // impact on him (game_sound.hpp): where it landed, as hard as the headset
    // grades it (a jab light, a firm blow medium, heavy, a finisher extreme),
    // the flow's count, in the air or not, a fist or a web strike.
    if (landed) {
        game_focus::earn(fighter.record,
                         fighter.grounded ? game_focus::Kind::meleeDefault : game_focus::Kind::meleeAir);
        game_sound::Blow sound;
        sound.intensity = d.kind == BlowKind::Finisher        ? 4
                          : d.kind != BlowKind::Hit || called ? 3
                          : e.strength >= .35f                ? 2
                                                              : 1;
        for (const auto& enemy : enemies)
            if (enemy.id == e.target)
                sound.location = game_sound::locationOf(e.point.y - enemy.feet.y, enemy.height);
        sound.combo = static_cast<uint8_t>(std::clamp(count - 1, 0, 5));
        sound.air = !fighter.grounded;
        sound.webStrike = byStrike;
        game_sound::blow(fighter.record, e.target, sound);
    }
    if (!venom)
        chargeMeter(venomMeter.load(), venom_punch::gain(d.kind != BlowKind::Hit && d.kind != BlowKind::Finisher,
                                                         d.kind == BlowKind::Finisher));
    if (venom) {
        const bool emptied = spendBar(venomMeter.load());
        AcquireSRWLockExclusive(&output);
        ++venomOut.blows;
        venomOut.spent += emptied;
        venomOut.lastCharge = charge;
        venomOut.lastPoint = e.point;
        venomOut.lastDamage = d.damage;
        ReleaseSRWLockExclusive(&output);
    }
    if (d.fly && machine) {
        const auto waiting = std::find_if(launches.begin(), launches.end(),
                                          [&](const Launch& l) { return l.record == e.target; });
        if (waiting != launches.end())
            *waiting = {e.target, machine, d.velocity};
        else
            launches.push_back({e.target, machine, d.velocity});
    }
    // Off the ground and on no web, the player stays up for the next blow.
    if (!fighter.grounded && !fighter.swinging)
        strike.hangOn(false);
    ++tally.blows;
    tally.heavies += d.kind == BlowKind::Heavy;
    tally.launches += d.kind == BlowKind::Launch;
    tally.juggles += d.kind == BlowKind::Juggle;
    tally.slams += d.kind == BlowKind::Slam;
    tally.finishers += d.kind == BlowKind::Finisher;
    tally.lastPoint = e.point;
    tally.lastDirection = e.direction;
    tally.lastStrength = e.strength;
    tally.lastDamage = d.damage;
    tally.lastKind = static_cast<uint32_t>(d.kind);
    tally.lastCount = static_cast<uint32_t>(count);
    tally.lastHand = static_cast<uint32_t>(e.hand);
    tally.lastStrike = byStrike;
    tally.lastCalled = called;
}
} // namespace

uint32_t game_punch::start(uintptr_t gameBase) {
    AcquireSRWLockExclusive(&lifecycle);
    uint32_t result{};
    if (!enabled) {
        base = gameBase;
        // The damage requests' main-thread hook; the web grab may have
        // started it already.
        result = native_bodies::start(base);
        if (!result) {
            AcquireSRWLockExclusive(&updating);
            resetFight();
            tally = {};
            senseTally = {};
            warnedLately = {};
            ReleaseSRWLockExclusive(&updating);
            // The guards (game_guard): their rules file is read before the first scan reads the classes.
            game_guard::start(base);
            watch.start(base, 1u << static_cast<unsigned>(game_targets::Kind::bot));
            enabled = true;
        }
        publish([&](Data& d) {
            const auto sequence = d.sequence;
            d = {};
            d.sequence = sequence;
            d.status = result ? 3 : 1;
            d.error = result;
        });
        publishStrikes(result ? 3 : 1);
        publishSense(result ? 3 : 1, {});
    }
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}
uint32_t game_punch::stop() {
    AcquireSRWLockExclusive(&lifecycle);
    enabled = false;
    // An update in flight finishes first.
    AcquireSRWLockExclusive(&updating);
    resetFight();
    publishStrikes(0);
    publishSense(0, {});
    ReleaseSRWLockExclusive(&updating);
    watch.stop();
    game_guard::stop();
    publish([](Data& d) { d.status = 0; });
    ReleaseSRWLockExclusive(&lifecycle);
    return 0;
}
bool game_punch::running() {
    return enabled.load(std::memory_order_relaxed);
}
float game_punch::worldScale() {
    double clock{};
    if (!base || !read(base + game_time::clockScaleRva, &clock, sizeof(clock)) || !std::isfinite(clock) || clock <= 0)
        return 1;
    return static_cast<float>(std::clamp(clock, .05, 1.));
}
Opened game_punch::update(float seconds, const Input& in, const Player& player, const WorldQueries& world) {
    Opened opened;
    if (!enabled.load(std::memory_order_relaxed) || seconds <= 0 || !TryAcquireSRWLockShared(&updating))
        return opened;
    struct Release {
        ~Release() {
            ReleaseSRWLockShared(&updating);
        }
    } release;
    if (!enabled)
        return opened;
    watch.current(bots);
    fighter = player;
    // v19 (game_guard): the guards' hit points and the dodge over them, from the bots as they stand. v27: and how fast
    // each enemy comes at the hero (a runner guards), in the world's time - the game's time scale is told.
    game_guard::watch(bots, player.feet, worldScale());
    // v21: a guard's counter that has arrived staggers the hero.
    answerCounters();
    // The fist: the knuckles, fistAhead along the hand from its web shooter
    // (the swing input's aim, webShooter: the nozzle, 2.5 cm further back
    // since October 18, which this keeps the fist clear of); its motion
    // relative to the player is the grip relative to the head, turned from
    // tracking space.
    constexpr float fistAhead = .155f;
    std::array<PunchHand, 2> hands{};
    const Quat toWorld = trackingTurn(in);
    // A snap or smooth turn, or a flip, since the last sample turned the
    // hands with the player: no punch in it.
    if (std::isfinite(in.trackingYaw)) {
        if (turnSeen)
            punches.turn(toWorld * lastTurn.conjugate());
        lastTurn = toWorld;
        turnSeen = true;
    }
    // The head: each tracked hand's place, less where it is from the head.
    Vec3 head{};
    int tracked{};
    for (int i = 0; i < 2; ++i) {
        const auto& h = in.hands[i];
        hands[i] = {in.focused && h.tracked, h.aim.position + h.aim.orientation.rotate({0, 0, -fistAhead}),
                    toWorld.rotate(h.gripRelativeToHead), ((player.busy >> i) & 1) != 0};
        if (hands[i].tracked) {
            head += hands[i].fist - hands[i].relative;
            ++tracked;
        }
    }
    head = tracked ? head / static_cast<float>(tracked) : player.feet + Vec3{0, 1.6f, 0};
    headAt = head;
    inReach.clear();
    enemies.clear();
    nearby.clear();
    const Vec3 chest = player.feet + Vec3{0, strike.config().chest, 0};
    const float striking = strike.config().maxDistance + beyondReach;
    for (const auto& b : bots) {
        Vec3 feet{};
        // Nor a thug of a later wave, waiting unseen where he will appear,
        // nor one knocked out, nor one something else moves (a car's seat, a
        // cinematic: game_targets::held).
        if ((b.traits & game_targets::neutral) || !game_targets::up(base, b) || game_targets::held(base, b) ||
            !game_targets::position(b, feet))
            continue;
        const float bottom = b.size.height(0), top = b.size.height(botHeight);
        const Vec3 middle = feet + Vec3{0, b.size.height(1), 0};
        bool close{};
        for (const auto& h : hands)
            close |= h.tracked && length(h.fist - middle) < reach + std::max(0.f, top - bottom - botHeight);
        const bool ranged = game_targets::enemy(b) && length(middle - chest) <= striking;
        if ((!close && !ranged) || !game_targets::live(base, b))
            continue;
        const PunchTarget target{b.record, feet + Vec3{0, bottom, 0}, top - bottom, b.size.width(botRadius)};
        nearby.push_back({b.record, b.machine});
        if (close)
            inReach.push_back(target);
        // A strike goes to an enemy who still stands, or flies: not to one
        // knocked out.
        if (ranged && game_targets::fighting(base, b))
            enemies.push_back({target.id, target.feet, target.height, target.radius,
                               b.machine && native_bodies::flung(b.machine)});
    }
    // The attacks the game has warned the player of, before any blow of this
    // sample lands: a blow during a warning counters it.
    readSense(chest);
    // The enemy the next punch goes to: the one a blow locked the player
    // onto, else the one he looks at. A web that holds an enemy makes him
    // the one. And of those his blows landed on, the ones knocked out.
    for (const uint64_t held : player.holds)
        lock.take(held);
    lock.update(seconds, head, in.look, enemies);
    sinceFight = !enemies.empty() || lock.target() ? 0.f : sinceFight + seconds;
    knockouts(seconds);
    events.clear();
    punches.update(seconds, hands, inReach, events);
    flow.update(seconds);
    if (!fighter.combatVisuals || !flow.count())
        cues.clear();
    steerFlights(seconds);
    for (const auto& e : events) {
        // The swing that landed a punch opens no strike behind it.
        strike.spent(e.hand);
        land(e, false);
    }
    // The Mega Venom Blast: all three bars full, both grips squeezed together
    // (the headset asks: venom_punch::BothGrips). It goes off where he is and
    // takes all three bars, and a charge if there is one.
    const int side = venomHand.load() ? 1 : 0;
    // Dodges fill the meter: a little each, a perfect one more. A count that
    // went back (a new VR session) only catches up.
    {
        const uint32_t dodged = dodgeRequest.load(), perfect = perfectRequest.load();
        const uint32_t plain = dodged > dodgesAnswered ? std::min(dodged - dodgesAnswered, 2u) : 0,
                       perfects = perfect > perfectAnswered ? std::min(perfect - perfectAnswered, 2u) : 0;
        dodgesAnswered = dodged;
        perfectAnswered = perfect;
        if (plain || perfects) {
            chargeMeter(venomMeter.load(), plain * venom_punch::dodgeGain + perfects * venom_punch::perfectDodgeExtra);
            // v19 (game_guard): a dodge that carries him over an enemy opens that enemy's guard.
            game_guard::dodgeBegan(player.feet);
        }
    }
    if (const uint32_t asked = megaRequest.load(); asked != megaAnswered) {
        megaAnswered = asked;
        venom_punch::Bars bars;
        if (!smash.active() && strike.phase() != WebStrike::Phase::Flying && readBars(venomMeter.load(), bars) &&
            venom_punch::megaReady(bars)) {
            if (strike.phase() == WebStrike::Phase::Hanging)
                strike.cancel();
            venomUsed = venomCharge.load();
            for (int i = 0; i < 2; ++i) {
                punches.spend(i);
                strike.spent(i);
            }
            blast(player.feet, Blast::Mega);
        }
    }
    // The venom smash: the venom fist, charged, punched down - standing, or
    // in the air on no web (venom_punch.hpp). It takes the charge, and the
    // punch opens no strike nor venom punch.
    if (venomArmed() && !(venomLocks.load() & Venom::lockedSmash) && !smash.active() && hands[side].tracked &&
        !hands[side].busy && !player.swinging && strike.phase() != WebStrike::Phase::Flying &&
        venom_punch::smashing(punches.velocity(side))) {
        // Held up in the air after a blow: the drop has him from here.
        if (strike.phase() == WebStrike::Phase::Hanging)
            strike.cancel();
        venomUsed = venomCharge.load();
        punches.spend(side);
        strike.spent(side);
        smash.start(player.feet, player.grounded);
        if (player.grounded) {
            smash.step(seconds, player.feet, true);
            blast(player.feet);
        }
    }
    // The Venom Jump: the venom fist, charged, punched straight up while he
    // stands, in a fight (venom_punch.hpp; the user, October 20: "make the
    // jump only work in a fight" - out of one the punch is the game's own
    // venom punch). The enemies around go up, and he with them: the
    // strike's rise (the game's own jump first), then a hang up there.
    if (venomArmed() && !(venomLocks.load() & Venom::lockedJump) && !smash.active() && player.grounded &&
        sinceFight < venom_punch::fightCalmSeconds && hands[side].tracked && !hands[side].busy &&
        !player.swinging && strike.phase() != WebStrike::Phase::Flying &&
        venom_punch::jumping(punches.velocity(side))) {
        venomUsed = venomCharge.load();
        punches.spend(side);
        strike.spent(side);
        strike.cancel();
        blast(player.feet, Blast::Jump);
        strike.rise(player.feet, player.feet + Vec3{0, venom_punch::jumpHeight, 0}, venom_punch::jumpSeconds,
                    venom_punch::jumpHang);
        opened = {true, false};
    }
    // The venom punch: the venom fist, charged, thrown out at the marked
    // enemy within venomReach, as a strike's fist is thrown (fast enough,
    // out from the neck, his way). The blow lands on him.
    if (venomArmed() && hands[side].tracked && !hands[side].busy && strike.phase() == WebStrike::Phase::Idle) {
        const auto& sc = strike.config();
        const Vec3 velocity = punches.velocity(side);
        const float speed = length(velocity);
        for (const auto& target : enemies) {
            if (target.id != lock.target() || speed < sc.minSpeed)
                continue;
            const Vec3 moving = velocity / speed;
            const Vec3 neck = head - Vec3{0, sc.neck, 0};
            const Vec3 out = normalized(hands[side].fist - neck);
            const Vec3 toward = normalized(target.middle() - hands[side].fist);
            if (length(target.middle() - chest) > venomReach || dot(moving, out) < sc.outward ||
                dot(moving, toward) < sc.markedAim)
                continue;
            Vec3 way = normalized(target.middle() - chest);
            if (length(way) < .5f)
                way = moving;
            auto e = punches.blow(way, speed);
            e.hand = side;
            e.target = target.id;
            e.point = target.middle() - way * target.radius;
            punches.spend(side);
            strike.spent(side);
            land(e, false);
            break;
        }
    }
    std::array<FightFist, 2> fists{};
    for (int i = 0; i < 2; ++i)
        fists[i] = {hands[i].tracked, hands[i].fist, punches.velocity(i), !hands[i].busy, player.holds[i],
                    in.hands[i].grip};
    const bool flying = strike.phase() == WebStrike::Phase::Flying;
    // Spider-Sense's counter: a fist thrown at an attacker whose warning asks
    // for one breaks his attack now (the game's flinch, unless the fist
    // already landed on him), and he is the one from here: the strike it
    // opens flies to him, whomever the player was locked onto. Arkham
    // Shadow's: "Batman instantly abandons his current target in favor of
    // countering the incoming punch".
    std::array<SenseFist, 2> senseFists{};
    for (int i = 0; i < 2; ++i)
        senseFists[i] = {hands[i].tracked && !hands[i].busy, hands[i].fist, punches.velocity(i)};
    counterWishLeft = std::max(counterWishLeft - seconds, 0.f);
    if (const auto c = sense.update(seconds, threats, senseFists, head, chest)) {
        noteCounter(*c, true);
        const auto struck = [&](const PunchEvent& e) { return e.target == c->attacker; };
        if (std::none_of(events.begin(), events.end(), struck))
            breakAttack(*c, head);
        lock.take(c->attacker);
        counterWish = c->attacker;
        counterWishLeft = counterWishSeconds;
    }
    // A strike goes to the marked enemy alone; a fist swung the way the flow
    // calls opens one at him too.
    StrikeWish wish;
    wish.target = lock.target();
    if (counterWishLeft > 0 && std::any_of(enemies.begin(), enemies.end(),
                                           [&](const FightTarget& e) { return e.id == counterWish; }))
        wish.target = counterWish;
    wish.freeDirections = !fighter.combatVisuals;
    if (fighter.combatVisuals)
        for (const auto& e : enemies)
            if (e.id == wish.target)
                wish.called = Cues::direction(cues.cue().way, e.middle() - head);
    // With a look to mark by, a strike needs its mark: nobody marked, nobody
    // to fly to. (Without one, from an older headset module, the fist picks.)
    const bool looking = finite(in.look) && length(in.look) > .5f;
    const std::span<const FightTarget> strikeable =
        looking && !wish.target ? std::span<const FightTarget>{} : std::span<const FightTarget>{enemies};
    if (const auto strikeOpened = strike.open(seconds, fists, head, player.feet, strikeable, world, wish)) {
        // The blow lands as the player arrives: not on the way, off a fist
        // the flight carries through him.
        punches.spend(strikeOpened->hand);
        lock.take(strikeOpened->target);
        opened = {true, strikeOpened->held};
        ++tally.opened;
        tally.swung += strikeOpened->called;
        tally.held += strikeOpened->held;
        tally.distance = strikeOpened->distance;
    } else if (const auto dove = strike.dive(player.velocity, player.grounded, head, player.feet, enemies, world,
                                             wish.target, punches.config())) {
        // His own flight into the marked enemy: a swing, a zip, a jump or a
        // fall that takes him at him fast, feet first (WebStrike::dive). The
        // webs let go; the rest of the way is the strike's.
        lock.take(dove->target);
        opened = {true, false};
        ++tally.opened;
        tally.distance = dove->distance;
    } else if (flying && strike.phase() == WebStrike::Phase::Idle) {
        ++tally.missed; // the player took no steps: perched, or held by the game
    }
    publish([&](Data& d) {
        d.status = 2;
        ++d.samples;
        d.punches += events.size();
        d.bots = static_cast<uint32_t>(inReach.size());
        for (int i = 0; i < 2; ++i) {
            d.hands[i].speed = punches.speed(i);
            d.hands[i].busy = (player.busy >> i) & 1;
        }
        for (const auto& e : events) {
            auto& h = d.hands[e.hand];
            ++h.punches;
            h.lastTarget = e.target;
            h.lastStrength = e.strength;
            h.lastKnockback = static_cast<uint32_t>(e.knockback);
            d.lastPoint = e.point;
            d.lastDirection = e.direction;
            d.lastDamage = e.damage;
            d.lastSpeed = e.speed;
        }
    });
    publishStrikes(2);
    publishSense(2, chest);
    return opened;
}
bool game_punch::lifting(Vec3 feet) {
    if (!enabled.load(std::memory_order_relaxed) || !TryAcquireSRWLockShared(&updating))
        return false;
    const bool up = strike.lifting(feet);
    ReleaseSRWLockShared(&updating);
    return up;
}
Flight game_punch::fly(float seconds, Vec3 feet, bool grounded, bool waiting) {
    Flight out;
    if (!enabled.load(std::memory_order_relaxed) || !TryAcquireSRWLockShared(&updating))
        return out;
    struct Release {
        ~Release() {
            ReleaseSRWLockShared(&updating);
        }
    } release;
    if (!enabled)
        return out;
    // The venom smash's drop: straight down until he stands, then the blast there.
    if (smash.active()) {
        const auto step = smash.step(seconds, feet, grounded);
        out.drive = step.drive;
        out.velocity = step.velocity;
        if (step.blast)
            blast(step.at);
        if (step.calledOff) {
            AcquireSRWLockExclusive(&output);
            ++venomOut.smashesCalledOff;
            ReleaseSRWLockExclusive(&output);
        }
        return out;
    }
    if (strike.phase() == WebStrike::Phase::Idle)
        return out;
    if (waiting && strike.phase() == WebStrike::Phase::Flying) {
        strike.wait(seconds);
        if (strike.phase() == WebStrike::Phase::Idle) {
            ++tally.missed; // the game did not jump
            publishStrikes(2);
        }
        return out;
    }
    const FightTarget* target{};
    for (const auto& e : enemies)
        if (e.id == strike.target())
            target = &e;
    const bool flying = strike.phase() == WebStrike::Phase::Flying, diving = strike.diving();
    const int hand = strike.hand();
    const auto step = strike.step(seconds, feet, grounded, flying ? target : nullptr);
    out.drive = step.drive;
    out.velocity = step.velocity;
    if (step.landed && target) {
        // The blow: along the line from the chest to his middle, as fast as
        // the fist that opened the strike, and no weaker than a strike's.
        const Vec3 chest = feet + Vec3{0, strike.config().chest, 0};
        Vec3 way = normalized(target->middle() - chest);
        // A dive's: along the dive, the way the player came at him.
        if (length(way) < .5f || diving)
            way = strike.direction();
        auto e = punches.blow(way, std::max(strike.fistSpeed(),
                                            speedOf(punches.config(), strike.config().minStrength)));
        e.hand = hand;
        e.target = target->id;
        e.point = target->middle() - way * target->radius;
        punches.spend(hand);
        fighter.feet = feet;
        fighter.grounded = grounded;
        land(e, true);
        ++tally.landed;
    }
    tally.missed += step.missed;
    if (step.landed || step.missed)
        publishStrikes(2);
    return out;
}
void game_punch::hangInAir(float seconds) {
    if (!enabled.load(std::memory_order_relaxed) || !TryAcquireSRWLockShared(&updating))
        return;
    if (enabled && !fighter.grounded && !fighter.swinging)
        strike.hangOn(false, seconds);
    ReleaseSRWLockShared(&updating);
}
void game_punch::cancelStrike() {
    if (!TryAcquireSRWLockShared(&updating))
        return;
    if (strike.phase() != WebStrike::Phase::Idle) {
        tally.missed += strike.phase() == WebStrike::Phase::Flying;
        strike.cancel();
        publishStrikes(enabled ? 2u : 0u);
    }
    ReleaseSRWLockShared(&updating);
}
Data game_punch::data() {
    AcquireSRWLockShared(&output);
    auto out = SpidyPunchData;
    ReleaseSRWLockShared(&output);
    return out;
}
Strikes game_punch::strikes() {
    AcquireSRWLockShared(&output);
    auto out = SpidyStrikeData;
    ReleaseSRWLockShared(&output);
    return out;
}
Venom game_punch::venom(const Venom& in) {
    venomCharge = in.charge;
    venomMeter = in.meter;
    venomHolder = in.holder;
    venomHolderClass = in.holderClass;
    venomHand = in.hand ? 1u : 0u;
    megaRequest = in.megaRequest;
    dodgeRequest = in.dodges;
    perfectRequest = in.perfectDodges;
    venomLocked = (in.locked & Venom::lockedVenom) != 0;
    venomLocks = in.locked;
    game_guard::setVenomLocked(venomLocked.load());
    AcquireSRWLockShared(&output);
    auto out = venomOut;
    ReleaseSRWLockShared(&output);
    out.charge = in.charge;
    out.meter = in.meter;
    out.hand = in.hand;
    return out;
}
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPunchStart(void* input) {
    Config c{};
    if (!read(reinterpret_cast<uintptr_t>(input), &c, sizeof(c)) || c.magic != 0x53505543 || c.version != 1 ||
        c.bytes != sizeof(c) || c.pid != GetCurrentProcessId() ||
        c.base != reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))
        return 9501;
    return game_punch::start(c.base);
}
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPunchStop(void*) {
    return game_punch::stop();
}
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPunchSample(void* out) {
    const auto sample = game_punch::data();
    __try {
        std::memcpy(out, &sample, sizeof(sample));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9503;
    }
    return 0;
}
// The web strikes and the blows landed (game_punch::Strikes).
extern "C" __declspec(dllexport) DWORD WINAPI SpidyStrikeSample(void* out) {
    const auto sample = game_punch::strikes();
    __try {
        std::memcpy(out, &sample, sizeof(sample));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9503;
    }
    return 0;
}
// The venom charge in, the venom blows out (game_punch::Venom), in place.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyVenom(void* inout) {
    Venom v{};
    if (!read(reinterpret_cast<uintptr_t>(inout), &v, sizeof(v)) || v.magic != Venom{}.magic ||
        v.version != Venom{}.version || v.bytes != sizeof(v))
        return 9502;
    const auto out = game_punch::venom(v);
    __try {
        std::memcpy(inout, &out, sizeof(out));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 9503;
    }
    return 0;
}
// One blow straight to the game's DamageSystem, issued at the next physics
// step: 0 once the main thread has issued it.
extern "C" __declspec(dllexport) DWORD WINAPI SpidyPunchTest(void* input) {
    Test t{};
    if (!read(reinterpret_cast<uintptr_t>(input), &t, sizeof(t)) || t.magic != 0x53505554 || t.version != 1 ||
        t.bytes != sizeof(t) || !t.victim || !finite(t.point) || !finite(t.direction))
        return 9502;
    if (!game_punch::running())
        return 9504;
    native_bodies::Damage d;
    d.victim = t.victim;
    d.damager = t.damager;
    d.point = t.point;
    d.direction = t.direction;
    d.normal = t.direction * -1.f;
    d.amount = t.amount;
    d.type = t.type;
    d.knockback = t.knockback;
    d.knockbackAmount = t.knockbackAmount;
    d.impulse = t.impulse;
    d.hash = t.hash;
    const auto before = native_bodies::counters();
    const auto ticket = native_bodies::damage(d);
    if (!ticket)
        return 9505;
    const auto limit = GetTickCount64() + 1000;
    while (native_bodies::damageIssued() < ticket && GetTickCount64() < limit)
        Sleep(5);
    publish([](Data&) {});
    if (native_bodies::damageIssued() < ticket)
        return 9506; // the game ran no physics step: paused, or loading
    // Issued, or dropped: the victim was gone, or the system's pool full.
    return native_bodies::counters().damages > before.damages ? 0 : 9507;
}

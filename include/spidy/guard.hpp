#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// MilesVR 0.1.80, v18: guards. The user (November 7): "underground brawlers who block attacks until you venom
// punch, enemies with shields and melee enemies with batons or other weapons ... an enemy that is available to
// block cant [be] dragged with a web until their block is broken", and, when what makes the game's own brawler
// block could not be found in the logs: "you dont have to do my idea you can create an entirely new system just
// do what works". So the block here is the mod's own rule, in plain code with no dependence on the game's block
// internals:
//
//   - Some enemies are guarding: a Weapon guard (an enemy with a baton or another melee weapon:
//     BotMeleeWeaponManager), a Brawler's (a heavy, armored or brawling one), a Shield's. Which ones come from the
//     class names of an enemy's components (game_targets reads them from the game's own type information) and, for
//     a heavy one the names do not give away, from his hit points.
//   - A plain blow on a guarding enemy (a fist, a web strike's) is blocked: it does nothing to him.
//   - A venom punch or a venom blast breaks any guard. A broken guard stays down for a few seconds, then he
//     guards again. (v21: nothing else the fists do breaks one - not a Spider-Sense counter, not any number of
//     punches: "i dont want their guard broken just because i kept punching them".)
//   - v21: blocked punches in a row make him answer. After `counterBlows` of them (3) within `counterWindow`
//     seconds of one another he swings back at the hero, as the base game's guarding enemies do: it does no
//     damage and only staggers the hero (game_punch). Book::blow says so in Outcome::retaliates.
//   - v19 (the user, November 7: "i hit a baton, gauntlet, brute or shield enemy and theyre unpunchable unless
//     staggered with a gadget, throwable or dodging over them"): a guard also opens when something other than the
//     hero's fists hurts the enemy - a gadget, a thrown prop, a thrown enemy: his hit points fall and none of the
//     mod's own blows is on him - and when the hero's dodge carries him over or past the enemy. A Weapon guard no
//     longer breaks under blows unless guard.txt asks for it (`blows N`).
//   - Nobody guards who is down, stunned, webbed, held or in the air. (v23, the user, November 8: "sometimes if I kept
//     punching a punch would go through a block ... if i punch them as they are attacking, their attack should be
//     cancelled and they block me, and if thats not possible, they block me and follow through the attack ... if they
//     are webbed the guard should not work at all". So an enemy in the middle of an attack DOES guard - a blow at
//     him is blocked (v23: and it staggered him so the attack was stopped if the game let it be; v25: only with
//     `touch on`, the default is that he finishes his attack) - and a webbed one never
//     does, however lately a blow of the hero's reached him. v24: and not for webbedSeconds after the last moment
//     he was seen webbed either - the guess is that a blow that lands on a webbed enemy puts him in a hit reaction
//     between the web states (no log shows it yet), and the next punch must not find him free.)
//   - A guarding enemy cannot be dragged by a web (game_grab's takes()).
//   - v23: each guard has its sound (Sound): a baton's for the ordinary melee-weapon guards, a brawler's for the
//     Underground ones that break their weapons, a slayer's for the heavies and a shield's.
//   - v27 (the user, November 8, with a log in which nine of the ten enemies met had no guard of their own: "it still
//     feels like I can yank the enemy or punch them while they are running, if i attack them why they are running
//     towards me they should stop and block", and then "they can run and block they dont have to stop, just whatever
//     works"): an enemy RUNNING AT THE HERO guards for as long as he runs, whether or not his classes give him a guard
//     (Kind::Runner). A blow at him is blocked and a web pull at him is refused, he puts his arms up and the guard's
//     sound plays - and he runs on, for the mod cannot make the game's AI stop a run. The moment he stops running (his
//     state is an attack's, or he is standing, or going another way) a runner's guard is gone; the openers (venom, a
//     dodge over him, a gadget, a web) open it as they open every guard. Which states are running ones is
//     runningState(); how fast he must be coming at the hero for it (a walk is no run) is Tuning::runSpeed, measured
//     from his own movement by Book::running(). guard.txt `runners off` takes it away again.
//   - v28 (the user, November 8, after v27: "no we dont want that, it just felt like enemies that could guard were
//     attackable while they ran, or yankable at least"): the runner's guard is OFF by default (guard.txt `runners on`
//     brings it back), and what let an enemy with a guard of his own through while he ran was found in the log of
//     04:02: every web ball that hit him opened his guard for webbedSeconds, though one or two balls only twitch an
//     enemy (11 of webbing a ball against a threshold of 30 for a thug, and it decays): 24 of the 34 enemies a ball
//     hit in that session were never seen in a webbed state, and 5 of the balls hit one in BotStateGotoGame, running
//     at the hero. A web ball no longer opens a guard; an enemy SEEN in a webbed state does, as before, and (v28) the
//     seconds that follow do not hold his guard open while he is running again (runningState()): he is free.
//   - v30 (the user, November 9: "if an enemy hasnt detected miles, they dont guard"): an enemy who has not DETECTED the hero
//     does not guard - a blow at him lands and a web pull takes him. The game keeps no flag of it that the mod can read, so
//     it is read from what he does and has (Book::notice): a behavior of a fight on his actor (fightBehavior: BehaviorMeleeGame,
//     BehaviorRangedCombat, BehaviorStandAndShoot, BehaviorReactGame ...), a state that is no calm one (fightState: Melee,
//     Shuffle, Aim, a hit reaction, a web ...), a run at the hero, a fall of his hit points, and the hero's own blow, web ball
//     or web at him. Each keeps him "detected" for Tuning::detectSeconds (10) after the last time it was seen; with none he has
//     not detected the hero (and an enemy the book has met but never seen a sign of has not either). guard.txt `detect off`
//     brings v29 back. Not built into any of it: Miles's camouflage (the user: "we dont need camo"), which the mod cannot read.
//   - v30 (the user, November 9: "make it so baton enemies cant block swing kicks, this is the kick animations that knocks enemies
//     back in spidey vr, brutes brawlers and shields stay the same"): a swing kick - the dive, the feet-first blow of the hero's own
//     flight at an enemy (WebStrike::dive; game_punch's land() knows it by its hand, WebStrike::diveHand) - goes through the guard of
//     an enemy whose block is the baton's: an ordinary melee-weapon guard (kickPasses). The brutes' and the brawlers' guards (the
//     Brawler kind, and the weapon breakers of the Underground, whose block sounds like a brawler's) and the shields' stop it as
//     they stop a punch. guard.txt `swingkick off` has the baton guards stop it too.
//
// The logic here is plain C++ with no game in it (tests/guard_tests.cpp): the classes, the rules file, and the
// book of each enemy's guard. game_guard.cpp runs it in the game.
namespace spidy::guards {
// What an enemy's guard is. The stronger kind wins when an enemy has more than one. (v27: Runner is no guard of an enemy's
// own but the one any enemy has while he runs at the hero; it is the weakest, whatever its number: the numbers of the
// others stay as they were, for the report the headset reads.)
enum class Kind : std::uint8_t { None = 0, Weapon = 1, Brawler = 2, Shield = 3, Runner = 4 };
inline const char* kindName(Kind kind) {
    switch (kind) {
    case Kind::Weapon:
        return "weapon";
    case Kind::Brawler:
        return "brawler";
    case Kind::Shield:
        return "shield";
    case Kind::Runner:
        return "runner";
    default:
        return "none";
    }
}
// How strong a guard is, for stronger(): none, a runner's, a weapon's, a brawler's, a shield's.
inline int guardRank(Kind kind) {
    switch (kind) {
    case Kind::Runner:
        return 1;
    case Kind::Weapon:
        return 2;
    case Kind::Brawler:
        return 3;
    case Kind::Shield:
        return 4;
    default:
        return 0;
    }
}
inline Kind stronger(Kind a, Kind b) {
    return guardRank(a) >= guardRank(b) ? a : b;
}
// v23: which of the four block sounds an enemy's guard makes (the user's, November 8: batonblock, brawlerblock,
// slayerblock and shieldblock). A Shield guard has the shield's. An enemy with the classes of a weapon breaker (the
// Underground's: WeaponBreakerStatusEffect, UnbrokenWeaponPartialPlayer) is a Brawler whatever else he is. A heavy
// (a Brawler guard: brute, gauntlet, armored, slayer, or a lot of hit points) is a Slayer; every other guard is
// an ordinary melee guard with a Baton's.
enum class Sound : std::uint8_t { Baton = 0, Brawler = 1, Slayer = 2, Shield = 3 };
inline const char* soundName(Sound sound) {
    switch (sound) {
    case Sound::Brawler:
        return "brawler";
    case Sound::Slayer:
        return "slayer";
    case Sound::Shield:
        return "shield";
    default:
        return "baton";
    }
}
inline Sound soundOf(Kind kind, bool breaker) {
    if (kind == Kind::Shield)
        return Sound::Shield;
    if (breaker)
        return Sound::Brawler;
    return kind == Kind::Brawler ? Sound::Slayer : Sound::Baton;
}
// v30 (the user, November 9: "make it so baton enemies cant block swing kicks ... brutes brawlers and shields stay the same"): a swing
// kick - the dive, the blow of the hero's own flight at an enemy, feet first - goes through the guard of an enemy whose block is the
// baton's: an ordinary melee-weapon guard (a Weapon guard that is no weapon breaker's; the weapon breakers' block sounds like a
// brawler's, and they are the brawlers of the Underground). A brute's or a brawler's guard (the Brawler kind), a shield's and a
// runner's stop it as they stop a punch.
inline bool kickPasses(Kind kind, bool breaker) {
    return kind == Kind::Weapon && soundOf(kind, breaker) == Sound::Baton;
}
// Why a guard broke.
// (2 was the Spider-Sense counter's, which broke a guard until v21: retired, the numbers stay as they were.)
enum class Cause : std::uint8_t { None = 0, Venom = 1, Strain = 3, Stagger = 4, Dodge = 5 };
inline const char* causeName(Cause cause) {
    switch (cause) {
    case Cause::Venom:
        return "venom";
    case Cause::Strain:
        return "strain";
    case Cause::Stagger:
        return "stagger";
    case Cause::Dodge:
        return "dodge";
    default:
        return "none";
    }
}

// The numbers a guard runs on (guard.txt can change them).
struct Tuning {
    // How long a broken guard stays down.
    float breakSeconds = 6.f;
    // v21: his counter. This many blocked blows in a row (each no more than counterWindow seconds after the one
    // before) make him swing back at the hero; once he has, counterCooldown seconds pass before he can again. 0:
    // never counters.
    float counterBlows = 3.f, counterWindow = 3.f, counterCooldown = 2.5f;
    // A Weapon guard breaks when the blows it blocked add up to this many: each blow counts strainBase plus
    // strainStrength times its strength (0..1), so about five ordinary punches or four hard ones. Blows more than
    // strainMemory seconds apart start over. 0 (v19, the default): never - the blows alone do not break it.
    float weaponBlows = 0.f, strainMemory = 2.5f, strainBase = .5f, strainStrength = .5f;
    // v19: a guard stays open this long after something other than the hero's fists takes staggerDamage hit points
    // or more from the enemy (a gadget, a thrown prop or enemy), and after the hero dodges over or past him. 0: never.
    float staggerSeconds = 4.f, dodgeSeconds = 3.f;
    float staggerDamage = 3.f;
    // v24 (the user, November 8: "webbed enemies still block, i can punch them once but then they block everything
    // else while webbed"): an enemy seen in a webbed state stays open this long after the last time he was seen in
    // one. The guess (v23's log had no webbed enemy): a blow that lands on a webbed enemy makes the game put him in a
    // hit reaction between the web states, and that reaction is no webbed state by name, so without the hold he looks
    // free to the very next punch. 0: no hold. v28: the hold begins at a moment he is SEEN in a webbed state (a web ball
    // that only twitches him is no web: game_guard::ballHit), and does not hold while he is running (runningState).
    float webbedSeconds = 4.f;
    // The mod's own blow on an enemy keeps the fall of his hit points that follows from counting as a stagger this
    // long (its damage reaches him a physics step later).
    float ownSeconds = .6f;
    // v27: an enemy is running at the hero when his state is a running one (runningState) and his own movement along the
    // line to the hero is at least runSpeed metres a second (a walk is 1 to 1.5, a thug's run several times that), seen
    // in the last runGrace seconds (the samples come every twentieth of a second or so).
    float runSpeed = 2.f, runGrace = .35f;
    // v30: an enemy counts as having detected the hero for this many seconds after the last sign of it (Book::notice).
    float detectSeconds = 10.f;
};

// What guard.txt says, and what is built in. The words are parts of the undecorated class name of a component
// (BotMeleeWeaponManager), case counted.
struct Rules {
    bool enabled = true;
    // The hit recorder (game_record) runs: log lines of the game's own hits, for finding what a rule needs.
    bool record = false;
    // A blocked blow makes the enemy react with the game's own hit reaction (the smallest, Twitch, with no damage to
    // speak of; a Stagger when he is in the middle of an attack). v25: OFF by default - the user (November 8, after
    // v24): "they do a staggered animation when you punch them a couple times". The log showed the game's hit-reaction
    // state (BotStateHitReactGame) under 24 of 70 blocked blows: a guard that reels at every punch does not look like
    // one that blocks, and the mod's own block animation (guard_pose.hpp: the arms up, the body jolted by the blow)
    // is what shows the punch meeting him. `touch on` brings the reaction back.
    bool touch = false;
    // v21: the enemy's guard is seen. A blocked punch or a refused web pull puts his arms up, and his counter is a
    // swing of his arm (guard_pose.hpp): the mod's own pose on his skeleton. `pose off`: not at all.
    bool pose = true;
    // A venom punch (or blast) breaks a guard. `venom off`: it does not, and only the other ways open one.
    bool venom = true;
    // v27: an enemy running at the hero guards while he runs, whatever his classes give him (Kind::Runner). v28: OFF by
    // default (the user: "no we dont want that"); `runners on` brings it back. Off, only the enemies with a guard of
    // their own guard.
    bool runners = false;
    // v30: an enemy who has not detected the hero does not guard (Book::noticed). `detect off`: he guards as in v29.
    bool detect = true;
    // v30: a swing kick (the dive) goes through a baton's guard (kickPasses). `swingkick off`: it is blocked like a punch.
    bool swingKick = true;
    // v20: a blocked blow knocks the player's view back this far, in centimetres (recoil.hpp): a tiny bit, for the
    // feel of it. `recoil 0` turns it off, `recoil 6` doubles it.
    float recoil = 3.f;
    // Extra parts of a class name that give an actor a guard of that kind, and parts that take every guard from
    // an actor with such a class.
    std::vector<std::string> weapon, brawler, shield, exempt;
    // v23: parts of a class name that mark a weapon breaker (the Underground's), whose block sounds like a brawler's.
    std::vector<std::string> breaker;
    // An enemy of this many hit points or more (his health component's maximum) guards as a Brawler, whatever
    // his classes say: the heavy ones are the tough ones. The street thugs seen so far have 80 to 120. 0: never.
    float hp = 160.f;
    Tuning tuning;
};

// One class: the kind of guard it gives its actor, and whether it takes every guard from it.
struct ClassVerdict {
    Kind kind = Kind::None;
    bool exempt = false;
    // v23: the class marks a weapon breaker (soundOf).
    bool breaker = false;
};
inline bool contains(std::string_view name, std::string_view part) {
    return !part.empty() && name.find(part) != std::string_view::npos;
}
// A class by its name without the decoration ("BotMeleeWeaponManager"). The built-in parts: a melee weapon's
// manager and a baton for a Weapon guard (the logs of November 7: the enemies with BotMeleeWeaponManager are the
// ones with batons and other melee weapons), a brawler, a brute, armored health and (v23) a slayer's behavior
// (SlayerBotBehaviorMelee: the heavies of November 7 had it) for a Brawler, a shield. v23: the weapon breaker
// classes the Underground's melee enemies carry (WeaponBreakerStatusEffect, UnbrokenWeaponPartialPlayer) give no
// guard of their own: they only change which block sound the guard makes.
inline ClassVerdict classOf(std::string_view name, const Rules& rules) {
    static constexpr std::string_view weapon[] = {"MeleeWeaponManager", "Baton"};
    static constexpr std::string_view brawler[] = {"Brawl", "Brute", "ArmoredHealth", "Gauntlet", "Slayer"};
    static constexpr std::string_view shield[] = {"Shield"};
    static constexpr std::string_view breaker[] = {"WeaponBreaker", "UnbrokenWeapon"};
    ClassVerdict out;
    for (const auto& part : rules.exempt)
        if (contains(name, part))
            out.exempt = true;
    for (const auto part : breaker)
        if (contains(name, part))
            out.breaker = true;
    for (const auto& part : rules.breaker)
        if (contains(name, part))
            out.breaker = true;
    for (const auto part : weapon)
        if (contains(name, part))
            out.kind = stronger(out.kind, Kind::Weapon);
    for (const auto& part : rules.weapon)
        if (contains(name, part))
            out.kind = stronger(out.kind, Kind::Weapon);
    for (const auto part : brawler)
        if (contains(name, part))
            out.kind = stronger(out.kind, Kind::Brawler);
    for (const auto& part : rules.brawler)
        if (contains(name, part))
            out.kind = stronger(out.kind, Kind::Brawler);
    for (const auto part : shield)
        if (contains(name, part))
            out.kind = stronger(out.kind, Kind::Shield);
    for (const auto& part : rules.shield)
        if (contains(name, part))
            out.kind = stronger(out.kind, Kind::Shield);
    return out;
}
// v19: a bot state class (BotState...) that has him staggered, stunned, webbed or knocked about - his guard is
// open in it. The names are those of the states seen in the logs and the words any such state would carry.
// v25: not "Shock" - the log of November 8 had two blows let in on thugs in BotStateShockwaveAttack (and the game has a
// BotStateShockJump): those are their own slam attacks, and a guard holds in the middle of an attack (v23). A shocked
// enemy is in a Stun state (BotStateElectricStun).
inline bool opensGuard(std::string_view name) {
    static constexpr std::string_view parts[] = {"Stagger", "Stun",    "Knock",  "Ragdoll", "GetUp",
                                                 "WebStruggle", "Webbed", "Flinch", "Dazed",   "Stumble"};
    if (name.substr(0, 8) != "BotState")
        return false;
    for (const auto part : parts)
        if (name.find(part) != std::string_view::npos)
            return true;
    return false;
}
// v27: a bot state class (BotState...) of an enemy on the move at the hero: the game's chase states (BotStateChaseRunTo,
// ChaseJumpTo, ChaseWallRun, ChaseSlideTo, ChaseSpecialGoTo, ChaseVaultOver, ChaseHookshotTo, and their Local and Remote
// halves), its go-to states (BotStateGoto, GotoGame, GotoLocal, GotoRemote) and go-direction ones (BotStateGoDirection and
// the Game, Local and Remote ones), and the plain dash (BotStateDash). The names are those the game's type information
// lists (the log of November 8). Not the attacks that run at him - BotStateDashAttack, Lunge, Pounce, a charge: an attack is
// an attack, and a runner who has begun one is no runner. The class's own name is the one to give, not those of the classes
// it derives from (a dash attack may derive from the dash). Whether he is coming at the hero is another matter: Book::running.
inline bool runningState(std::string_view name) {
    if (name.substr(0, 8) != "BotState")
        return false;
    name.remove_prefix(8);
    return name.substr(0, 5) == "Chase" || name.substr(0, 4) == "Goto" || name.substr(0, 11) == "GoDirection" ||
           name == "Dash";
}
// v23: a bot state class that has him webbed or caught in a web (WebStruggle, WebAttached, WebYank, ... every state
// of the game with Web in its name; the November 7 log lists them). A webbed enemy never guards, whatever the mod's
// own blows did lately - the user, November 8: "when i web them up they still block me, if they are webbed the
// guard should not work at all".
inline bool webbedState(std::string_view name) {
    return name.substr(0, 8) == "BotState" && name.find("Web") != std::string_view::npos;
}
// v30: a class of an enemy's actor that is a behavior of a fight. The game adds and drops these as an enemy goes into and out
// of one: the hit recorder's lists of November 7 have BehaviorMeleeGame, BehaviorRangedCombat, BehaviorStandAndShoot,
// BehaviorBotComboGraph, BehaviorUseCombo, BehaviorUseLosBlocker, BehaviorReactGame and BehaviorAttackResponse on the enemies it
// met - and BehaviorGotoGame, which a patrol's go-to is as well, and the machinery every actor's behaviors run on (BehaviorMap,
// BehaviorHost, BehaviorHostCache, BehaviorEventQueue), which are no sign of anything. By the name without the decoration: a
// name with Behavior in it and a word of a fight (so a slayer's SlayerBotBehaviorMelee is one too).
inline bool fightBehavior(std::string_view name) {
    static constexpr std::string_view words[] = {"Melee",  "Combat", "Ranged", "Shoot", "Combo", "Attack", "React", "LosBlocker",
                                                 "Fight",  "Brawl",  "Charge", "Throw", "Grenade", "Cover", "Chase", "Aim"};
    if (!contains(name, "Behavior"))
        return false;
    for (const auto word : words)
        if (contains(name, word))
            return true;
    return false;
}
// v30: a class that is one of the game's behaviors, for the census line (what an enemy who has not detected the hero has on
// him, and what one who has): every name with Behavior in it but the machinery the behaviors of every actor run on.
inline bool isBehavior(std::string_view name) {
    if (!contains(name, "Behavior"))
        return false;
    return !(name == "BehaviorMap" || name == "BehaviorEventQueue" || name.substr(0, 12) == "BehaviorHost");
}
// v30: a bot state class that an enemy who has not detected the hero can be in: standing, playing an animation, looking about,
// pointing at something, going somewhere (how fast he comes at the hero is judged apart: Book::running), facing, picking
// something up. Not StandUp: getting up is a knocked down enemy's. Every other state is a sign of a fight (fightState): a melee
// or a ranged one, shuffling round the hero, an attack, a block, a hit reaction, a stun, a web, a chase.
inline bool calmState(std::string_view name) {
    static constexpr std::string_view exact[] = {"Stand",    "Initial",       "Base",     "Null", "PlayAnim",
                                                 "LookAtTargets", "PointAtPOI", "PickUpItem", "PutDownItem", "RecoverHealth"};
    static constexpr std::string_view starts[] = {"Goto", "GoDirection", "Face", "PlayRelativeAnim", "PlaySyncedAnimGuest",
                                                  "PlayCinematic", "TreadWater"};
    if (name.substr(0, 8) != "BotState")
        return false;
    name.remove_prefix(8);
    for (const auto part : exact)
        if (name == part)
            return true;
    for (const auto part : starts)
        if (name.substr(0, part.size()) == part)
            return true;
    return false;
}
inline bool fightState(std::string_view name) {
    return name.size() > 8 && name.substr(0, 8) == "BotState" && !calmState(name);
}
// A decorated class name (".?AVBotMeleeWeaponManager@@") to the plain one.
inline std::string_view plainName(std::string_view decorated) {
    if (decorated.size() > 4 && decorated[0] == '.' && decorated[1] == '?' && decorated[2] == 'A' &&
        (decorated[3] == 'V' || decorated[3] == 'U'))
        decorated.remove_prefix(4);
    const auto end = decorated.find('@');
    return end == std::string_view::npos ? decorated : decorated.substr(0, end);
}

namespace detail {
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}
inline bool sameWord(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        if (x != b[i])
            return false;
    }
    return true;
}
inline bool number(std::string_view s, float& out) {
    const std::string text(s);
    char* end{};
    const float v = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != 0 || !(v == v) || v > 1e6f || v < -1e6f)
        return false;
    out = v;
    return true;
}
} // namespace detail

// guard.txt. One setting a line; '#' starts a comment; words are not case-sensitive, names are:
//   off | on               the whole guard system
//   record                 also log the game's own hits (for tracing; not needed for play)
//   touch on | off         a blocked blow makes the enemy flinch (the game's own hit reaction; a stagger if he is
//                          attacking). Off by default since v25: his arms come up and his body jolts instead
//   pose on | off          the enemy puts his arms up for a blocked punch or a refused web pull, and swings
//                          his arm for the counter (on by default)
//   counter N              this many blocked punches in a row make him swing back at the hero: no damage, only
//                          a stagger; 0: never (3 by default)
//   venom on | off         a venom punch breaks a guard (on by default)
//   runners on | off       an enemy running at you guards while he runs, whatever he is (v27; v28: off by default)
//   runspeed N             how fast he must be coming at you, metres a second, to count as running (2 by default; a
//                          walk is about 1 to 1.5)
//   detect on | off        an enemy who has not detected you does not guard (v30; on by default)
//   detecthold SECONDS     how long an enemy counts as having detected you after the last sign of it (v30; 10 by default)
//   swingkick on | off     a swing kick (the dive) goes through a baton's guard; a brute's, a brawler's and a shield's stop
//                          it all the same (v30; on by default)
//   stagger SECONDS       how long a guard stays open after a gadget or a thrown prop hurts him; 0: never
//   dodge SECONDS          how long a guard stays open after the hero dodges over him; 0: never
//   webbed SECONDS         how long a webbed enemy stays open after the last moment he was seen webbed (v24); 0: only
//                          while his state says webbed
//   weapon NAME            a component class whose name has NAME gives its enemy a weapon guard
//   brawler NAME           ... a brawler's guard (broken only by venom)
//   shield NAME            ... a shield's guard (broken only by venom)
//   breaker NAME           ... marks a weapon breaker: his block makes the brawler's sound (v23)
//   exempt NAME            an enemy with such a class never guards
//   hp NUMBER              the hit points from which an enemy guards as a brawler; 0 turns it off
//   blows NUMBER           the blows that break a weapon guard; 0 (the default): the blows never do
//   seconds NUMBER         how long a broken guard stays down
// Returns how many lines were not understood (the rules read up to them stand).
inline int parseRules(std::string_view text, Rules& rules) {
    int unknown{};
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (const auto hash = line.find('#'); hash != std::string_view::npos)
            line = line.substr(0, hash);
        line = detail::trim(line);
        if (line.empty())
            continue;
        const auto space = line.find_first_of(" \t");
        const std::string_view key = line.substr(0, space);
        const std::string_view value =
            space == std::string_view::npos ? std::string_view{} : detail::trim(line.substr(space + 1));
        float n{};
        if (detail::sameWord(key, "off") && value.empty()) {
            rules.enabled = false;
        } else if (detail::sameWord(key, "on") && value.empty()) {
            rules.enabled = true;
        } else if (detail::sameWord(key, "record") && value.empty()) {
            rules.record = true;
        } else if (detail::sameWord(key, "touch") &&
                   (detail::sameWord(value, "on") || detail::sameWord(value, "off"))) {
            rules.touch = detail::sameWord(value, "on");
        } else if (detail::sameWord(key, "pose") &&
                   (detail::sameWord(value, "on") || detail::sameWord(value, "off"))) {
            rules.pose = detail::sameWord(value, "on");
        } else if (detail::sameWord(key, "counter") && detail::number(value, n) && (n == 0 || (n >= 1 && n <= 20))) {
            rules.tuning.counterBlows = n;
        } else if (detail::sameWord(key, "venom") &&
                   (detail::sameWord(value, "on") || detail::sameWord(value, "off"))) {
            rules.venom = detail::sameWord(value, "on");
        } else if (detail::sameWord(key, "runners") &&
                   (detail::sameWord(value, "on") || detail::sameWord(value, "off"))) {
            rules.runners = detail::sameWord(value, "on");
        } else if (detail::sameWord(key, "runspeed") && detail::number(value, n) && n >= .5f && n <= 12) {
            rules.tuning.runSpeed = n;
        } else if (detail::sameWord(key, "detect") &&
                   (detail::sameWord(value, "on") || detail::sameWord(value, "off"))) {
            rules.detect = detail::sameWord(value, "on");
        } else if (detail::sameWord(key, "detecthold") && detail::number(value, n) && n >= .5f && n <= 600) {
            rules.tuning.detectSeconds = n;
        } else if (detail::sameWord(key, "swingkick") &&
                   (detail::sameWord(value, "on") || detail::sameWord(value, "off"))) {
            rules.swingKick = detail::sameWord(value, "on");
        } else if (detail::sameWord(key, "recoil") && detail::number(value, n) && n >= 0 && n <= 15) {
            rules.recoil = n;
        } else if (detail::sameWord(key, "stagger") && detail::number(value, n) && n >= 0 && n <= 120) {
            rules.tuning.staggerSeconds = n;
        } else if (detail::sameWord(key, "dodge") && detail::number(value, n) && n >= 0 && n <= 120) {
            rules.tuning.dodgeSeconds = n;
        } else if (detail::sameWord(key, "webbed") && detail::number(value, n) && n >= 0 && n <= 120) {
            rules.tuning.webbedSeconds = n;
        } else if (detail::sameWord(key, "weapon") && !value.empty()) {
            rules.weapon.emplace_back(value);
        } else if (detail::sameWord(key, "brawler") && !value.empty()) {
            rules.brawler.emplace_back(value);
        } else if (detail::sameWord(key, "shield") && !value.empty()) {
            rules.shield.emplace_back(value);
        } else if (detail::sameWord(key, "breaker") && !value.empty()) {
            rules.breaker.emplace_back(value);
        } else if (detail::sameWord(key, "exempt") && !value.empty()) {
            rules.exempt.emplace_back(value);
        } else if (detail::sameWord(key, "hp") && detail::number(value, n) && n >= 0) {
            rules.hp = n;
        } else if (detail::sameWord(key, "blows") && detail::number(value, n) && (n == 0 || (n >= .5f && n <= 100))) {
            rules.tuning.weaponBlows = n;
        } else if (detail::sameWord(key, "seconds") && detail::number(value, n) && n >= .5f && n <= 120) {
            rules.tuning.breakSeconds = n;
        } else {
            ++unknown;
        }
    }
    return unknown;
}

// A blow of the hero's at an enemy, as the book judges it.
struct Blow {
    // A venom punch, or a venom blast that reached him: breaks any guard.
    bool venom{};
    // v21: the hero is in a state his guard's counter can stagger (on his feet). Off, a counter that is due waits
    // for the next blocked blow.
    bool mayCounter = true;
    // His guard is open at this moment: he is down, stunned, webbed, held or in the air. (Until v22 he was also
    // open in the middle of an attack, and the blows thrown at him then went through; v23: he guards then too.)
    bool open{};
    // v25: the mod gives him a reaction of its own for this blow if it is blocked (touch). Off (`touch off`, the
    // default), a block leaves no reaction of the mod's behind, and the state he is in is the game's.
    bool touches = true;
    // How hard the fist was thrown, 0..1.
    float strength{};
};
enum class Verdict : std::uint8_t {
    Lands,   // the blow is his to take
    Blocked, // the guard stopped it
    Breaks   // the blow breaks the guard (and lands)
};
struct Outcome {
    Verdict verdict = Verdict::Lands;
    Cause cause = Cause::None;
    // v21: this blocked blow is the one that sets off his counter.
    bool retaliates{};
    // v21: blocked blows in a row, this one included (0 when it was not blocked).
    std::uint32_t streak{};
};

// Each enemy's guard, by his actor record and the handle of his marker component (a record's address can be
// the next enemy's: a different handle starts over).
class Book {
  public:
    struct Entry {
        std::uint64_t record{};
        std::uint32_t handle{};
        double brokenUntil{}, lastBlock = -1e9, seen{}, ownAt = -1e9;
        // v23: when the mod itself last gave him a reaction (a blocked blow's touch, a counter's break): a state he
        // enters just then is its own and does not open his guard. Blows that landed leave this alone.
        double touchAt = -1e9;
        // v24: when he was last seen in a webbed state (webbed()).
        double webbedAt = -1e9;
        // v21: the blocked blows in a row and when the latest was, and when his last counter began.
        double streakAt = -1e9, counterAt = -1e9;
        // v27: where he stood on the ground at the latest sample (running()) and when, and when he was last seen
        // running at the hero.
        double pastX{}, pastZ{}, pastAt = -1e9, runAt = -1e9;
        // v27: how fast he comes at the hero, metres a second, smoothed.
        float closing{};
        float strain{}, hp = -1.f;
        std::uint32_t blocks{}, streak{}, counters{};
        bool introduced{};
        // v30: when the book first followed him, and when a sign that he has detected the hero was last seen (notice()).
        double metAt{}, noticedAt = -1e9;
        // v30: whether detection() last found him detected (for the log of the changes).
        bool aware{};
    };
    explicit Book(Tuning tuning = {}) : tuning_(tuning) {}
    void tune(const Tuning& tuning) {
        tuning_ = tuning;
    }
    const Tuning& tuning() const {
        return tuning_;
    }
    // His guard is up now (his kind gives one, and it is not broken).
    bool standing(std::uint64_t record, std::uint32_t handle, Kind kind, double now) const {
        if (kind == Kind::None)
            return false;
        const Entry* e = find(record, handle);
        return !e || now >= e->brokenUntil;
    }
    // Seconds since the mod's own blow last reached him (1e9: none). A state he enters just after one may be the
    // blow's own flinch, not a stagger from elsewhere.
    double sinceOwn(std::uint64_t record, std::uint32_t handle, double now) const {
        const Entry* e = find(record, handle);
        return e ? now - e->ownAt : 1e9;
    }
    // v23: seconds since the mod last gave him a reaction of its own - a blow his guard blocked (the flinch it gets), or
    // own() (1e9: none). A state of the open kind that he enters just after one is that reaction, not a stagger.
    double sinceTouched(std::uint64_t record, std::uint32_t handle, double now) const {
        const Entry* e = find(record, handle);
        return e ? now - e->touchAt : 1e9;
    }
    // v24: he is in a webbed state at this moment (the state's name has Web in it). True when this is a new web: he had
    // not been seen in one for webbedGap seconds (the log says so once for each).
    bool webbed(std::uint64_t record, std::uint32_t handle, Kind kind, double now, double webbedGap = 1.5) {
        if (kind == Kind::None)
            return false;
        Entry& e = touch(record, handle, now);
        const bool fresh = now - e.webbedAt > webbedGap;
        e.webbedAt = now;
        e.streak = 0;
        return fresh;
    }
    // v24: seconds since he was last seen in a webbed state (1e9: never). While it is within Tuning::webbedSeconds he is
    // still webbed, whatever the state he is in just now.
    double sinceWebbed(std::uint64_t record, std::uint32_t handle, double now) const {
        const Entry* e = find(record, handle);
        return e ? now - e->webbedAt : 1e9;
    }
    // v27: where an enemy stands on the ground (x, z, metres) and where the hero does, at this moment - every sample of
    // game_guard::watch(). From the samples the book keeps how fast he is coming at the hero: his own movement along the
    // line to the hero, in metres a second of the world's time (worldScale: the game's time scale, 1 or less in slow
    // motion, so that a runner is one then too), smoothed over about a quarter of a second. `moving`: his state is a
    // running one (runningState). True - and he counts as running at the hero (runner()) for Tuning::runGrace seconds from
    // now - when he is moving and that speed is at least Tuning::runSpeed. A gap of more than 0.6 s between samples, or
    // a jump (more than 30 m/s: he was put somewhere), starts the measure over. The hero's own movement is no part of it.
    bool running(std::uint64_t record, std::uint32_t handle, Kind kind, double x, double z, double heroX, double heroZ,
                 bool moving, double now, double worldScale = 1.) {
        if (kind == Kind::None)
            return false;
        Entry& e = touch(record, handle, now);
        const double dt = now - e.pastAt;
        if (dt >= .02 || dt < 0) { // sooner than that after the last sample there is no speed to tell
            const double scale = worldScale > .05 ? worldScale : .05;
            if (dt > 0 && dt <= .6) {
                const double toX = heroX - x, toZ = heroZ - z, far = std::sqrt(toX * toX + toZ * toZ);
                if (far > .05) {
                    const double along = ((x - e.pastX) * toX + (z - e.pastZ) * toZ) / far;
                    const double speed = along / (dt * scale);
                    if (std::fabs(speed) > 30.) {
                        e.closing = 0;
                    } else {
                        const double blend = dt < .25 ? dt / .25 : 1.;
                        e.closing += static_cast<float>((speed - static_cast<double>(e.closing)) * blend);
                    }
                }
            } else {
                e.closing = 0;
            }
            e.pastX = x;
            e.pastZ = z;
            e.pastAt = now;
        }
        const bool runs = moving && static_cast<double>(e.closing) >= static_cast<double>(tuning_.runSpeed);
        if (runs)
            e.runAt = now;
        return runs;
    }
    // v27: running() saw him running at the hero within the last Tuning::runGrace seconds.
    bool runner(std::uint64_t record, std::uint32_t handle, double now) const {
        const Entry* e = find(record, handle);
        return e && now - e->runAt <= static_cast<double>(tuning_.runGrace);
    }
    // v27: how fast he comes at the hero at the latest sample, metres a second (0: no measure yet).
    double closing(std::uint64_t record, std::uint32_t handle) const {
        const Entry* e = find(record, handle);
        return e ? static_cast<double>(e->closing) : 0.;
    }
    // v23: the mod itself has just staggered or flinched him (a counter's break of his attack): the state he enters is
    // its own, for the grace sinceTouched tells of, and the blow it was part of is a mod blow for hurt().
    void own(std::uint64_t record, std::uint32_t handle, Kind kind, double now) {
        if (kind == Kind::None)
            return;
        Entry& e = touch(record, handle, now);
        e.ownAt = now;
        e.touchAt = now;
    }
    // v30: a sign that he has detected the hero (see the header, v30): he counts as having detected him for
    // Tuning::detectSeconds from now.
    void notice(std::uint64_t record, std::uint32_t handle, Kind kind, double now) {
        if (kind == Kind::None)
            return;
        touch(record, handle, now).noticedAt = now;
    }
    // v30: he has detected the hero: a sign of it was seen within the last Tuning::detectSeconds. An enemy the book has no
    // sign of - or has never met - has not.
    bool noticed(std::uint64_t record, std::uint32_t handle, double now) const {
        const Entry* e = find(record, handle);
        return e && now - e->noticedAt <= static_cast<double>(tuning_.detectSeconds);
    }
    // v30: how long the book has followed him, seconds (0: it has not met him).
    double followedFor(std::uint64_t record, std::uint32_t handle, double now) const {
        const Entry* e = find(record, handle);
        return e ? now - e->metAt : 0.;
    }
    // v30: whether he is detected (noticed()) has changed since this was last asked of him (game_guard::watch asks at every
    // sample, for the log): Detected - a sign came after none; Lost - the last sign is more than Tuning::detectSeconds old. An
    // enemy the book has just met is unaware until a sign comes. One it has not followed for a second or more (out of the
    // watch's list, say) has no change to tell of: the answer is only brought up to date.
    enum class Change : std::uint8_t { None, Detected, Lost };
    Change detection(std::uint64_t record, std::uint32_t handle, double now) {
        const Entry* known = find(record, handle);
        const bool away = known && now - known->seen > 1.;
        Entry& e = touch(record, handle, now);
        const bool aware = now - e.noticedAt <= static_cast<double>(tuning_.detectSeconds);
        const Change change = away || aware == e.aware ? Change::None : aware ? Change::Detected : Change::Lost;
        e.aware = aware;
        return change;
    }
    // How long his broken guard has left, seconds (0: not broken).
    double brokenFor(std::uint64_t record, std::uint32_t handle, double now) const {
        const Entry* e = find(record, handle);
        return e && e->brokenUntil > now ? e->brokenUntil - now : 0.;
    }
    // A blow at an enemy whose guard is of this kind.
    Outcome blow(std::uint64_t record, std::uint32_t handle, Kind kind, const Blow& b, double now) {
        if (kind == Kind::None)
            return {};
        Entry& e = touch(record, handle, now);
        e.ownAt = now; // the damage of this blow, if it lands, is not a stagger from elsewhere
        const bool up = now >= e.brokenUntil;
        if (b.venom) {
            e.brokenUntil = now + tuning_.breakSeconds;
            e.strain = 0;
            e.streak = 0;
            return {up ? Verdict::Breaks : Verdict::Lands, Cause::Venom};
        }
        if (!up || b.open) {
            e.streak = 0;
            return {};
        }
        ++e.blocks;
        if (b.touches)
            e.touchAt = now; // the flinch this block gives him (touch) is the mod's own
        if (kind == Kind::Weapon && tuning_.weaponBlows > 0) {
            if (now - e.lastBlock > tuning_.strainMemory)
                e.strain = 0;
            e.strain += tuning_.strainBase + tuning_.strainStrength * std::clamp(b.strength, 0.f, 1.f);
            e.lastBlock = now;
            if (e.strain >= tuning_.weaponBlows) {
                e.strain = 0;
                e.streak = 0;
                e.brokenUntil = now + tuning_.breakSeconds;
                return {Verdict::Breaks, Cause::Strain};
            }
        }
        e.lastBlock = now;
        // v21: blocked blows in a row (none more than counterWindow after the one before) make him answer. The
        // streak waits at the threshold while he cannot (the cooldown, or the hero is not on his feet) and the
        // first blocked blow after that sets the counter off.
        if (now - e.streakAt > static_cast<double>(tuning_.counterWindow))
            e.streak = 0;
        e.streakAt = now;
        if (e.streak < 1000)
            ++e.streak;
        Outcome out{Verdict::Blocked, Cause::None};
        out.streak = e.streak;
        if (tuning_.counterBlows > 0 && static_cast<float>(e.streak) >= tuning_.counterBlows && b.mayCounter &&
            now - e.counterAt >= static_cast<double>(tuning_.counterCooldown)) {
            e.counterAt = now;
            e.streak = 0;
            ++e.counters;
            out.retaliates = true;
        }
        return out;
    }
    // v19: the guard opens for `seconds` (a dodge over him, say). True when it was standing.
    bool open(std::uint64_t record, std::uint32_t handle, Kind kind, double seconds, double now) {
        if (kind == Kind::None || !(seconds > 0))
            return false;
        Entry& e = touch(record, handle, now);
        const bool up = now >= e.brokenUntil;
        e.brokenUntil = (std::max)(e.brokenUntil, now + seconds);
        e.strain = 0;
        e.streak = 0;
        return up;
    }
    // v19: his hit points now (read every moment he is in sight). A fall of staggerDamage or more that none of the
    // mod's own blows explains - a gadget, a thrown prop or enemy, the game's own punch - opens the guard for
    // staggerSeconds. True when it was standing and has just opened.
    bool hurt(std::uint64_t record, std::uint32_t handle, Kind kind, float hp, double now) {
        if (kind == Kind::None || !(hp == hp))
            return false;
        Entry& e = touch(record, handle, now);
        const float before = e.hp;
        e.hp = hp;
        // v30: a fall of his hit points, whoever's, is a sign that he has detected the hero.
        if (before >= 0 && hp < before - .01f)
            e.noticedAt = now;
        if (before < 0 || before - hp < tuning_.staggerDamage || !(tuning_.staggerSeconds > 0) ||
            now - e.ownAt < static_cast<double>(tuning_.ownSeconds))
            return false;
        const bool up = now >= e.brokenUntil;
        e.brokenUntil = (std::max)(e.brokenUntil, now + static_cast<double>(tuning_.staggerSeconds));
        e.strain = 0;
        e.streak = 0;
        return up;
    }
    // True the first time this enemy is introduced (the census line is written once).
    bool introduce(std::uint64_t record, std::uint32_t handle, double now) {
        Entry& e = touch(record, handle, now);
        const bool first = !e.introduced;
        e.introduced = true;
        return first;
    }
    std::uint32_t blocks(std::uint64_t record, std::uint32_t handle) const {
        const Entry* e = find(record, handle);
        return e ? e->blocks : 0;
    }
    // v21: the counters he has swung, and the blocked blows in a row he has now.
    std::uint32_t counters(std::uint64_t record, std::uint32_t handle) const {
        const Entry* e = find(record, handle);
        return e ? e->counters : 0;
    }
    std::uint32_t streak(std::uint64_t record, std::uint32_t handle) const {
        const Entry* e = find(record, handle);
        return e ? e->streak : 0;
    }
    // Forgets the enemies not seen for `age` seconds (and the oldest beyond a hundred).
    void prune(double now, double age = 180.) {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const Entry& e) { return now - e.seen > age; }),
                       entries_.end());
        while (entries_.size() > 100) {
            auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                           [](const Entry& a, const Entry& b) { return a.seen < b.seen; });
            entries_.erase(oldest);
        }
    }
    void clear() {
        entries_.clear();
    }
    size_t size() const {
        return entries_.size();
    }

  private:
    const Entry* find(std::uint64_t record, std::uint32_t handle) const {
        for (const auto& e : entries_)
            if (e.record == record && e.handle == handle)
                return &e;
        return nullptr;
    }
    Entry& touch(std::uint64_t record, std::uint32_t handle, double now) {
        for (auto& e : entries_)
            if (e.record == record) {
                if (e.handle != handle) {
                    e = Entry{record, handle};
                    e.metAt = now;
                }
                e.seen = now;
                return e;
            }
        Entry fresh;
        fresh.record = record;
        fresh.handle = handle;
        fresh.seen = now;
        fresh.metAt = now;
        entries_.push_back(fresh);
        return entries_.back();
    }
    Tuning tuning_;
    std::vector<Entry> entries_;
};
} // namespace spidy::guards

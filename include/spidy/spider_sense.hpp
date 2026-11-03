#pragma once
#include "math.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>

// Spider-Sense in the headset: the game's own warning of an attack about to
// land, shown as Batman: Arkham Shadow shows one, and its counter.
//
// The flat game warns on the hero's head: the Spider-Sense lines flash on his
// mask and its sound plays. A VR player is that head and sees none of it. In
// Arkham Shadow (Road to VR's hands-on, Destructoid's and Way Too Many Games'
// reviews) a blue icon shows over an enemy about to strike, and "an icon in
// your sightline" when he is out of view; a hand thrown toward it counters
// him, "Batman instantly abandons his current target in favor of countering";
// a red one cannot be countered and is dodged. The October 11 combat (fighting
// across the room, web_strike.hpp) left exactly this out: "the counter (Spidy
// cannot tell yet when an attack is coming)".
//
// Where a warning comes from (game_sense.hpp reads it): every enemy attack the
// hero may dodge registers itself with the hero's HeroDodgeManager, with its
// attacker, when the hero is to be warned, when the blow lands and its
// BotAttackDodgeData, and the game warns the hero at that moment: its
// Spider-Sense sound, the lines on the mask. So every enemy the game warns of
// is warned of here, by the game's own data and timing, with no list of
// enemies.
//
// What it looks like (web_visual.hpp): the comic's Spider-Sense, its jagged
// lines, printed over his head in the colour of what to do, inside a ring that
// closes on the moment the blow lands; out of view, the same mark at the edge
// of the player's sight on his side, with an arrowhead toward him, and the
// lines at the rim of the view. The controller on his side tingles.
//
// What answers it:
// - Counter (blue): a fist thrown at him while the warning stands breaks his
//   attack. Within reach it lands as a punch; out of it the web strike flies
//   the player to him, whomever the player was locked onto. Either way his
//   attack is broken the moment the fist goes out, and the blow that lands on
//   him is a heavy one that sends him flying.
// - Dodge (red): an attack the game says no dodge interrupts, or one from
//   beyond a strike's reach. B, the game's own dodge (its perfect dodge and
//   its slowed time come with it).
// - Jump (yellow): one the game's jump dodges. A.
//
// Engine independent, like web_strike.hpp: game_punch.cpp runs it in the game.
namespace spidy {
// BotAttackDodgeData's Properties, each attack's own say in how the hero may
// answer it, by the names the game's reflection table gives the bits
// (Spider-Man.exe 5df3d80) and its help texts.
namespace attack {
constexpr std::uint32_t dodgeable = 0x1;   // the hero is warned so he can dodge out of the way
constexpr std::uint32_t counterable = 0x2; // the hero dodging can interrupt the attack
constexpr std::uint32_t neverBeaten = 0x4; // no damage immunity near its impact frame
constexpr std::uint32_t dodgeButton = 0x8; // the dodge button interrupts it
constexpr std::uint32_t jumpButton = 0x10; // the jump button dodges it
constexpr std::uint32_t dodgeLeft = 0x20, dodgeRight = 0x40, dodgeAway = 0x80, dodgeToward = 0x100;
constexpr std::uint32_t noAutoCamera = 0x200, reversable = 0x400, cameraOnJump = 0x800;
constexpr std::uint32_t noRangedFx = 0x1000; // keeps the ranged shooter's effects off this attack
} // namespace attack
// What a warning asks of the player, which is its mark's colour.
enum class SenseAnswer : std::uint8_t {
    None,
    Counter, // blue: a fist thrown at him
    Dodge,   // red: out of the way, the game's dodge
    Jump,    // yellow: over it, the game's jump
};
// One attack the game has warned the hero of.
struct SenseThreat {
    std::uint64_t attacker{}; // his actor record
    Vec3 head{};              // the top of his head: his mark goes above it
    Vec3 middle{};            // where a counter goes
    // Game seconds until the blow lands (below 0 once it should have), when
    // `timed`: the game may set no time for it. And since the warning went off.
    float toImpact{}, warned{};
    bool timed{};
    std::uint32_t flags{}; // attack:: bits
};
struct SpiderSenseConfig {
    // A counter reaches as far as a web strike does (WebStrikeConfig:
    // maxDistance from the chest): one from farther off is dodged.
    float reach = 10;
    // A fist thrown at him: at least this fast relative to the player (a
    // punch lands from 2.2 m/s), going out from the base of the neck (`neck`
    // under the head; `outward` the cosine between its motion and the line to
    // the fist), within `aim` of the line from the fist to his middle (a
    // cosine: 40 degrees), for `sustain` seconds: a twitch is no counter.
    float speed = 2.2f, outward = .2f, neck = .25f, aim = .766f, sustain = .03f;
    // The warning stands until this long after the blow should have landed
    // (game seconds): a fist on its way as he swings still counts. One with
    // no time set stands this long after it went off.
    float late = .1f, untimed = 1.2f;
    // A counter thrown in the last part of the warning is a perfect one.
    float perfect = .2f;
    // The blow that lands on him within this long of the counter (real
    // seconds) is the counter's: a web strike's flight comes first.
    float blowSeconds = 1.2f;
};
// What an attack asks of the player, from its flags and how far he is from
// the player's chest: the jump for one the jump dodges; a counter for one a
// dodge interrupts, within a counter's reach; else the dodge.
SenseAnswer answerOf(std::uint32_t flags, float distance, const SpiderSenseConfig& config = {});
// The warning still stands: its blow has not landed, or only just.
bool standing(const SenseThreat& threat, const SpiderSenseConfig& config = {});
// 1..0 of the warning's time left until its blow lands, a notch at a time
// in `notches` notches; 1 for one with no time set.
float senseLeft(const SenseThreat& threat, int notches = 8);
// One hand in one input sample, as the punches see it.
struct SenseFist {
    bool tracked{};
    Vec3 position{}, velocity{}; // in the world; the arm's own motion
};
// Counters: which warned attacker a fist is being thrown at, held for a
// fist's sustain and given once a throw, each warning countered once, and
// the blow that lands on him after it.
class SpiderSense {
  public:
    struct Counter {
        std::uint64_t attacker{};
        int hand{};
        float left = -1; // game seconds the warning had left (below 0: none set)
        bool perfect{};  // thrown in the warning's last part
        Vec3 point{};    // his middle
    };
    explicit SpiderSense(SpiderSenseConfig config = {});
    // One input sample, `seconds` (real) after the one before: the warnings
    // the game holds now, the fists, the player's head and chest. Returns the
    // counter a fist thrown in it made, if one did.
    std::optional<Counter> update(float seconds, std::span<const SenseThreat> threats,
                                  const std::array<SenseFist, 2>& fists, Vec3 head, Vec3 chest);
    // A blow landed on `attacker`. True when it is a counter's blow: the one
    // after a counter at him, once, or one that lands while his warning
    // stands and asks for a counter, which counters him there (`made`).
    bool blow(std::uint64_t attacker, std::span<const SenseThreat> threats, Vec3 chest,
              std::optional<Counter>& made);
    // His warning was answered by a counter (until it is gone).
    bool countered(std::uint64_t attacker) const;
    // New warnings seen so far.
    std::uint64_t warnings() const {
        return warnings_;
    }
    void reset();
    const SpiderSenseConfig& config() const {
        return config_;
    }

  private:
    struct Known {
        std::uint64_t attacker{};
        bool countered{}, seen{};
        float blowLeft{}; // real seconds his counter's blow may still land
    };
    Known* find(std::uint64_t attacker);
    const Known* find(std::uint64_t attacker) const;
    Counter counter(const SenseThreat& threat, int hand);
    SpiderSenseConfig config_;
    std::array<Known, 8> known_{};
    std::array<float, 2> thrownFor_{};
    std::array<std::uint64_t, 2> thrownAt_{};
    std::array<bool, 2> spent_{};
    std::uint64_t warnings_{};
};
// Dodging by moving. Players asked for it on October 14 and 15 ("IRL dodging
// mechanics, similar to Batman: Arkham Shadow"; "make it so when you flip,
// web-zip, you dodge enemies"): a warning stood, and the one answer that took
// the player out of the way was B. Now moving out of the way answers it too:
// while the game warns of an attack a counter or a dodge answers (a jump's is
// A), a quick step or lean across the attacker's line, a step back from him
// or a duck is the game's own dodge, the way the head went (a duck: the way
// the game chooses), so the game's perfect dodge and its slowed time come
// with it as they do with B. A lean toward him is none: a counter leans in.
// The head's move is measured in the tracking space, so walking with the
// stick and a snap turn move nothing.
struct MoveDodgeConfig {
    // The move: within `window` real seconds, `aside` across his line, `back`
    // away from him or `duck` down, and no more than `toward` toward him.
    float window = .3f, aside = .17f, back = .2f, duck = .15f, toward = .08f;
    // A move larger than this between two frames is not the player's (the
    // view re-placed, a lost and found headset): the measure starts over.
    float jump = .1f;
    // The game's B held this long, and none for `rest` after.
    float press = .12f, rest = .6f;
};
// A warning the headset shows, not countered yet.
struct DodgeThreat {
    std::uint64_t attacker{};
    Vec3 at{}; // where he is in the world (his head)
    SenseAnswer answer{};
};
class MoveDodge {
  public:
    explicit MoveDodge(MoveDodgeConfig config = {});
    // One headset frame, `seconds` (real) after the one before. play: the
    // game's B may go out (VR play, no web carrying the player). head: the
    // headset in the world; room: the headset in the tracking space, and
    // roomToWorld the turn from that space's axes to the world's. True while
    // the game's B is to be held.
    bool update(float seconds, bool play, Vec3 head, Vec3 room, Quat roomToWorld,
                std::span<const DodgeThreat> threats);
    // Which way the dodge goes: level, in the world, unit; zero for a duck,
    // whose way the game chooses.
    Vec3 way() const {
        return way_;
    }
    bool pressing() const {
        return pressLeft_ > 0;
    }
    // Dodges made so far.
    std::uint32_t dodges() const {
        return dodges_;
    }
    void reset();
    const MoveDodgeConfig& config() const {
        return config_;
    }

  private:
    struct Sample {
        Vec3 room{};
        float age{};
    };
    MoveDodgeConfig config_;
    // The head's places over the last window, oldest at `first_`.
    std::array<Sample, 128> samples_{};
    std::size_t first_{}, count_{};
    Vec3 way_{};
    float pressLeft_{}, restLeft_{};
    // The attackers whose warnings a dodge answered, until they are gone.
    std::array<std::uint64_t, 8> dodged_{};
    std::uint32_t dodges_{};
};
// Dodging on a web. The user, October 24: "Swinging and moving with webs
// should count as dodging". In the flat game they do: its web rush and its
// zip to a target start the hero's dodge as they start (the hero's
// HeroDodgeManager, its dodge start 92a040 with kinds 8 and 14), and its
// traversal dodge is one on the move (kind 5). In VR the swing carries the
// player, and neither B nor a dodge by moving goes out while a web does (the
// game's dodge would take him off it): nothing dodged a warned blow while a
// web carried him.
//
// So while a web carries the player - a swing, a zip, a pull, a web strike's
// flight, the game's zip to a point out of the swing, and the flight a web
// leaves him in for a moment after it lets go - fast enough to be getting
// out of the way, a warning about to land starts
// the game's own traversal dodge (game_dodge.hpp): it dodges every warning
// standing then and those that go off while it lasts, with the game's damage
// immunity, its Focus for an accurate or a perfect dodge and its sound. Not
// its perfect dodge's slowed time: the swing goes on at its own speed. It
// lasts `hold`, and longer while a blow it may have dodged is still on its
// way, up to `most`.
struct WebDodgeConfig {
    // Carried at least this fast (real m/s): a web that holds the player
    // still, hanging, dodges nothing. The flight after a web counts as the
    // web's for `coast` real seconds.
    float speed = 3, coast = 1;
    // The dodge starts `lead` game seconds before a blow lands, or `untimed`
    // after a warning the game set no time for; a blow `late` game seconds
    // past is past.
    float lead = .3f, untimed = .1f, late = .15f;
    // Real seconds: how long a dodge lasts at least and at most, and the rest
    // between two.
    float hold = .6f, most = 1.5f, rest = .1f;
};
// A warning the game holds, as a dodge on a web sees it.
struct WebDodgeThreat {
    std::uint64_t attacker{};
    float toImpact{}, warned{}; // game seconds, as SenseThreat's
    bool timed{};
    // The game has answered it already: dodged, or countered.
    bool answered{};
};
// The game's own dodge as it stands this frame: none, the one this started
// (still its kind and start), or another (the game's own, a B on the ground).
enum class GameDodge : std::uint8_t { none, ours, other };
class WebDodge {
  public:
    enum class Ask : std::uint8_t { none, start, end };
    explicit WebDodge(WebDodgeConfig config = {});
    // One game frame, `seconds` (real) after the one before. webbed: a web
    // carries the player now; airborne: he is off the ground; velocity: his,
    // real m/s in the world. Returns what to ask of the game: to start its
    // dodge (the way it goes: way()) or to end the one this started.
    Ask update(float seconds, bool webbed, bool airborne, Vec3 velocity, std::span<const WebDodgeThreat> threats,
               GameDodge game);
    // The ask to start was not taken (the game has no dodge on after it).
    void refused();
    bool dodging() const {
        return dodging_;
    }
    // Which way the dodge goes: the way the player moves, level where he
    // moves along the ground at all, unit. The game checks a dodge's way
    // against the sides an attack lets the hero dodge to.
    Vec3 way() const {
        return way_;
    }
    // Dodges started, and those the game ended itself before this did.
    std::uint32_t dodges() const {
        return dodges_;
    }
    std::uint32_t cutShort() const {
        return cutShort_;
    }
    void reset();
    const WebDodgeConfig& config() const {
        return config_;
    }

  private:
    WebDodgeConfig config_;
    bool dodging_{};
    float age_{}, restLeft_{}, sinceWeb_ = 1e9f;
    Vec3 way_{};
    std::uint32_t dodges_{}, cutShort_{};
};
// Where the warning of an attacker out of the player's view shows instead,
// as Arkham Shadow's icon in the sightline: on the circle `angle` (radians)
// round the middle of his view, on the attacker's side, and the rim's lines
// at `rim`. Shown while the attacker is more than `away` off the line of
// sight, kept until he is back within `back` of it. Behind the player the
// side stays the one he was last seen on.
struct SenseSightConfig {
    float away = .55f, back = .45f; // 31.5 and 26 degrees
    // The mark at 21 degrees, clear of whom the player faces (at 17 the first
    // preview put it on the belt of the thug he looked at), the rim's lines
    // at 38.
    float angle = .37f, rim = .66f;
};
struct SenseSight {
    bool shown{};
    float x = 1, y{}; // the way to him across the view, unit (x right, y up)
};
// One image: the head's pose in the world, the attacker's point, and what
// was shown for him on the image before.
SenseSight senseSight(Pose head, Vec3 target, SenseSight before, const SenseSightConfig& config = {});
} // namespace spidy

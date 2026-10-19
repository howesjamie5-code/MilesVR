// Punches in the game: fists from the swing's input samples, bots from the
// target watch, blows as the game's own damage; and fighting across the
// room, the web strike and the flow (game_punch.hpp).
#include "spidy/game_punch.hpp"
#include "spidy/game_grab.hpp"
#include "spidy/game_targets.hpp"
#include "spidy/game_time.hpp"
#include "spidy/native_bodies.hpp"
#include "spidy/venom_punch.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
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
std::atomic<uint64_t> venomMeter{}, venomHolder{}, venomHolderClass{};
uint32_t venomUsed{};
Venom venomOut;
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
bool writeMeter(uint64_t meter, float value) {
    const uint64_t holder = venomHolder.load(), expected = venomHolderClass.load();
    uintptr_t vtable{};
    if (!meter || !holder || meter <= holder || meter - holder >= 0x1000 || !read(holder, &vtable, 8) ||
        vtable != expected)
        return false;
    __try {
        *reinterpret_cast<float*>(meter) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool emptyMeter(uint64_t meter) {
    return writeMeter(meter, 0);
}
// A blow of the player's charges the meter (venom_punch::gain).
void chargeMeter(uint64_t meter, float amount) {
    float now{};
    if (meter && read(meter, &now, 4) && std::isfinite(now))
        writeMeter(meter, venom_punch::filled(now, amount));
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
    punches.reset();
    turnSeen = false;
    strike.cancel();
    flow.reset();
    cues.clear();
    lock.clear();
    struckLately.clear();
    enemies.clear();
    nearby.clear();
    launches.clear();
    fighter = {};
}
// The thugs' flights that wait for a blow's velocity.
void steerFlights(float seconds) {
    for (auto it = launches.begin(); it != launches.end();) {
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
    const bool called = cues.answers(went, e.point - headAt);
    if (called && byStrike)
        e.direction = normalized(went);
    tally.called += called;
    tally.strikeCalled += called && byStrike;
    tally.missedCalls += !byStrike && !called && before.way != CueWay::None;
    // The flow's finisher must be called: a blow that would be it and
    // answers no such call is a plain one that leaves him standing, to be
    // called on, and the count stays.
    const bool uncalled = flow.finisher(flow.count() + 1) && !(called && before.finisher);
    if (uncalled)
        flow.keep();
    const int count = uncalled ? flow.count() : flow.hit();
    auto d = deliver(e, airborne, count, flow, {}, called ? before.way : CueWay::None, cues.config().damage);
    if (uncalled && (d.fly || d.knockback >= Knockback::Knockdown)) {
        d.kind = BlowKind::Hit;
        d.kinetic = d.fly = false;
        d.knockback = Knockback::Knockback;
        d.velocity = {};
    }
    // The flow's next call: the one that stands, or after an answer another way.
    tally.calls += cues.call(flow.count(), flow.finisher(flow.count() + 1), called,
                             static_cast<uint32_t>((tally.blows + 1) * 2654435761u >> 13));
    // The right fist charged with venom: this blow is the venom punch, and
    // the charge is spent.
    const uint32_t charge = venomCharge.load();
    const bool venom = e.hand == (venomHand.load() ? 1 : 0) && venomArmed();
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
    native_bodies::damage(request);
    if (!venom)
        chargeMeter(venomMeter.load(), venom_punch::gain(d.kind != BlowKind::Hit && d.kind != BlowKind::Finisher,
                                                         d.kind == BlowKind::Finisher));
    if (venom) {
        const bool emptied = emptyMeter(venomMeter.load());
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
            ReleaseSRWLockExclusive(&updating);
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
    ReleaseSRWLockExclusive(&updating);
    watch.stop();
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
    // The fist: the knuckles, fistAhead along the hand from its web shooter
    // (the swing input's aim, webShooter); its motion relative to the player
    // is the grip relative to the head, turned from tracking space.
    constexpr float fistAhead = .13f;
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
        // nor one knocked out.
        if ((b.traits & game_targets::neutral) || !game_targets::up(base, b) || !game_targets::position(b, feet))
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
    // The enemy the next punch goes to: the one a blow locked the player
    // onto, else the one he looks at. A web that holds an enemy makes him
    // the one. And of those his blows landed on, the ones knocked out.
    for (const uint64_t held : player.holds)
        lock.take(held);
    lock.update(seconds, head, in.look, enemies);
    knockouts(seconds);
    events.clear();
    punches.update(seconds, hands, inReach, events);
    flow.update(seconds);
    if (!flow.count())
        cues.clear();
    steerFlights(seconds);
    for (const auto& e : events) {
        // The swing that landed a punch opens no strike behind it.
        strike.spent(e.hand);
        land(e, false);
    }
    // The venom punch: the venom fist, charged, thrown out at the marked
    // enemy within venomReach, as a strike's fist is thrown (fast enough,
    // out from the neck, his way). The blow lands on him.
    const int side = venomHand.load() ? 1 : 0;
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
    // A strike goes to the marked enemy alone; a fist swung the way the flow
    // calls opens one at him too.
    StrikeWish wish;
    wish.target = lock.target();
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
    if (!enabled || strike.phase() == WebStrike::Phase::Idle)
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
    const bool flying = strike.phase() == WebStrike::Phase::Flying;
    const int hand = strike.hand();
    const auto step = strike.step(seconds, feet, grounded, flying ? target : nullptr);
    out.drive = step.drive;
    out.velocity = step.velocity;
    if (step.landed && target) {
        // The blow: along the line from the chest to his middle, as fast as
        // the fist that opened the strike, and no weaker than a strike's.
        const Vec3 chest = feet + Vec3{0, strike.config().chest, 0};
        Vec3 way = normalized(target->middle() - chest);
        if (length(way) < .5f)
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

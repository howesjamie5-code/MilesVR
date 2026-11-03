// Spider-Sense's warnings and counters (spider_sense.hpp).
#include "spidy/spider_sense.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace spidy {
SenseAnswer answerOf(std::uint32_t flags, float distance, const SpiderSenseConfig& config) {
    if (flags & attack::jumpButton)
        return SenseAnswer::Jump;
    if ((flags & attack::counterable) && std::isfinite(distance) && distance <= config.reach)
        return SenseAnswer::Counter;
    return SenseAnswer::Dodge;
}
bool standing(const SenseThreat& threat, const SpiderSenseConfig& config) {
    if (!threat.attacker || !std::isfinite(threat.warned) || threat.warned < 0)
        return false;
    return threat.timed ? std::isfinite(threat.toImpact) && threat.toImpact >= -config.late
                        : threat.warned <= config.untimed;
}
float senseLeft(const SenseThreat& threat, int notches) {
    if (!threat.timed || !std::isfinite(threat.toImpact) || !std::isfinite(threat.warned))
        return 1;
    const float whole = threat.warned + std::max(threat.toImpact, 0.f);
    if (whole <= 1e-4f)
        return 0;
    const float left = std::clamp(threat.toImpact / whole, 0.f, 1.f);
    const float steps = static_cast<float>(notches);
    return notches > 0 ? std::ceil(left * steps) / steps : left;
}
SpiderSense::SpiderSense(SpiderSenseConfig config) : config_(config) {
    if (!(config_.reach > 0) || !(config_.speed > 0) || !(config_.aim > -1 && config_.aim < 1) ||
        !(config_.sustain >= 0) || !(config_.late >= 0) || !(config_.untimed > 0) ||
        !(config_.perfect >= 0) || !(config_.blowSeconds > 0))
        throw std::invalid_argument("Invalid Spider-Sense configuration");
}
SpiderSense::Known* SpiderSense::find(std::uint64_t attacker) {
    for (auto& k : known_)
        if (attacker && k.attacker == attacker)
            return &k;
    return nullptr;
}
const SpiderSense::Known* SpiderSense::find(std::uint64_t attacker) const {
    for (const auto& k : known_)
        if (attacker && k.attacker == attacker)
            return &k;
    return nullptr;
}
SpiderSense::Counter SpiderSense::counter(const SenseThreat& threat, int hand) {
    if (auto* k = find(threat.attacker)) {
        k->countered = true;
        k->blowLeft = config_.blowSeconds;
    }
    Counter c;
    c.attacker = threat.attacker;
    c.hand = hand;
    c.left = threat.timed ? std::max(threat.toImpact, 0.f) : -1;
    c.perfect = threat.timed && threat.toImpact <= config_.perfect;
    c.point = threat.middle;
    return c;
}
std::optional<SpiderSense::Counter> SpiderSense::update(float seconds, std::span<const SenseThreat> threats,
                                                        const std::array<SenseFist, 2>& fists, Vec3 head,
                                                        Vec3 chest) {
    if (!std::isfinite(seconds) || seconds < 0)
        seconds = 0;
    // The warnings standing now; one not seen before is a new one. A
    // warning gone is forgotten, but for a counter whose blow is to come.
    for (auto& k : known_)
        k.seen = false;
    for (const auto& t : threats) {
        if (!standing(t, config_))
            continue;
        Known* k = find(t.attacker);
        if (!k) {
            // A free place, else the one whose counter's blow is nearest
            // its end.
            k = &known_[0];
            for (auto& slot : known_)
                if (!slot.attacker || (!slot.seen && slot.blowLeft < k->blowLeft))
                    k = &slot;
            *k = {t.attacker};
            ++warnings_;
        }
        k->seen = true;
    }
    for (auto& k : known_) {
        k.blowLeft = std::max(k.blowLeft - seconds, 0.f);
        if (k.attacker && !k.seen && k.blowLeft <= 0)
            k = {};
    }
    // Each fist: the warned attacker it is thrown at, if any, held for the
    // sustain; a throw counts once.
    std::optional<Counter> made;
    const Vec3 neck = head - Vec3{0, config_.neck, 0};
    for (int i = 0; i < 2; ++i) {
        const auto& fist = fists[static_cast<size_t>(i)];
        const float speed = length(fist.velocity);
        const SenseThreat* at{};
        if (fist.tracked && finite(fist.position) && finite(fist.velocity) && speed >= config_.speed) {
            const Vec3 motion = fist.velocity / speed, out = fist.position - neck;
            float best = config_.aim;
            if (length(out) > 1e-3f && dot(motion, normalized(out)) >= config_.outward)
                for (const auto& t : threats) {
                    if (!standing(t, config_) || countered(t.attacker) || !finite(t.middle) ||
                        answerOf(t.flags, length(t.middle - chest), config_) != SenseAnswer::Counter)
                        continue;
                    const Vec3 to = t.middle - fist.position;
                    const float aim = length(to) > 1e-3f ? dot(motion, normalized(to)) : 1.f;
                    if (aim >= best) {
                        best = aim;
                        at = &t;
                    }
                }
        }
        auto& thrownAt = thrownAt_[static_cast<size_t>(i)];
        auto& thrownFor = thrownFor_[static_cast<size_t>(i)];
        auto& spent = spent_[static_cast<size_t>(i)];
        if (!at) {
            thrownAt = 0;
            thrownFor = 0;
            spent = false;
            continue;
        }
        if (at->attacker != thrownAt) {
            thrownAt = at->attacker;
            thrownFor = 0;
            spent = false;
        }
        thrownFor += seconds;
        if (spent || made || thrownFor < config_.sustain)
            continue;
        spent = true;
        made = counter(*at, i);
    }
    return made;
}
bool SpiderSense::blow(std::uint64_t attacker, std::span<const SenseThreat> threats, Vec3 chest,
                       std::optional<Counter>& made) {
    made.reset();
    if (!attacker)
        return false;
    if (Known* k = find(attacker); k && k->countered && k->blowLeft > 0) {
        k->blowLeft = 0;
        return true;
    }
    // A blow that lands while his warning stands counters him by itself.
    for (const auto& t : threats)
        if (t.attacker == attacker && standing(t, config_) && !countered(attacker) && finite(t.middle) &&
            answerOf(t.flags, length(t.middle - chest), config_) == SenseAnswer::Counter) {
            if (!find(attacker))
                for (auto& slot : known_)
                    if (!slot.attacker) {
                        slot = {attacker, false, true, 0};
                        ++warnings_;
                        break;
                    }
            made = counter(t, -1);
            if (Known* k = find(attacker))
                k->blowLeft = 0; // this blow is its blow
            return true;
        }
    return false;
}
bool SpiderSense::countered(std::uint64_t attacker) const {
    const Known* k = find(attacker);
    return k && k->countered;
}
void SpiderSense::reset() {
    known_ = {};
    thrownFor_ = {};
    thrownAt_ = {};
    spent_ = {};
}
MoveDodge::MoveDodge(MoveDodgeConfig c) : config_(c) {
    const auto bad = [](float v) { return !std::isfinite(v) || v <= 0; };
    if (bad(c.window) || bad(c.aside) || bad(c.back) || bad(c.duck) || !std::isfinite(c.toward) || bad(c.jump) ||
        bad(c.press) || !std::isfinite(c.rest) || c.rest < 0)
        throw std::invalid_argument("Invalid move dodge configuration");
}
void MoveDodge::reset() {
    first_ = count_ = 0;
    way_ = {};
    pressLeft_ = restLeft_ = 0;
    dodged_ = {};
}
bool MoveDodge::update(float seconds, bool play, Vec3 head, Vec3 room, Quat roomToWorld,
                       std::span<const DodgeThreat> threats) {
    const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
    pressLeft_ = std::max(0.f, pressLeft_ - dt);
    restLeft_ = std::max(0.f, restLeft_ - dt);
    // An answered warning is forgotten once the game takes it off.
    for (auto& d : dodged_)
        if (d && std::none_of(threats.begin(), threats.end(), [&](const DodgeThreat& t) { return t.attacker == d; }))
            d = 0;
    const float turn = roomToWorld.x * roomToWorld.x + roomToWorld.y * roomToWorld.y +
                       roomToWorld.z * roomToWorld.z + roomToWorld.w * roomToWorld.w;
    if (!finite(head) || !finite(room) || !std::isfinite(turn) || std::abs(turn - 1) > .01f) {
        count_ = 0;
        pressLeft_ = 0;
        return false;
    }
    const auto at = [&](std::size_t i) -> Sample& { return samples_[(first_ + i) % samples_.size()]; };
    if (count_ && length(room - at(count_ - 1).room) > config_.jump)
        count_ = 0;
    for (std::size_t i = 0; i < count_; ++i)
        at(i).age += dt;
    while (count_ && at(0).age > config_.window) {
        first_ = (first_ + 1) % samples_.size();
        --count_;
    }
    if (count_ == samples_.size()) {
        first_ = (first_ + 1) % samples_.size();
        --count_;
    }
    at(count_++) = {room, 0};
    if (!play) {
        pressLeft_ = 0;
        return false;
    }
    if (pressLeft_ > 0 || restLeft_ > 0)
        return pressLeft_ > 0;
    for (const auto& t : threats) {
        if ((t.answer != SenseAnswer::Counter && t.answer != SenseAnswer::Dodge) || !t.attacker || !finite(t.at) ||
            std::find(dodged_.begin(), dodged_.end(), t.attacker) != dodged_.end())
            continue;
        Vec3 to = t.at - head;
        to.y = 0;
        if (length(to) < .1f)
            continue;
        const Vec3 line = normalized(to);
        // The head's move since each place in the window, in the world's axes.
        for (std::size_t i = 0; i + 1 < count_; ++i) {
            const Vec3 moved = roomToWorld.rotate(room - at(i).room);
            const Vec3 level{moved.x, 0, moved.z};
            const float along = dot(level, line), aside = length(level - line * along);
            if (along > config_.toward)
                continue;
            const bool across = aside >= config_.aside || -along >= config_.back;
            if (!across && -moved.y < config_.duck)
                continue;
            way_ = across && length(level) > .05f ? normalized(level) : Vec3{};
            pressLeft_ = config_.press;
            restLeft_ = config_.press + config_.rest;
            ++dodges_;
            // One dodge answers every warning standing now.
            for (const auto& other : threats)
                if (other.attacker &&
                    std::find(dodged_.begin(), dodged_.end(), other.attacker) == dodged_.end()) {
                    const auto free = std::find(dodged_.begin(), dodged_.end(), 0ull);
                    if (free != dodged_.end())
                        *free = other.attacker;
                }
            count_ = 0;
            return true;
        }
    }
    return false;
}
WebDodge::WebDodge(WebDodgeConfig c) : config_(c) {
    const auto bad = [](float v) { return !std::isfinite(v) || v < 0; };
    if (bad(c.speed) || bad(c.coast) || bad(c.lead) || bad(c.untimed) || bad(c.late) || bad(c.hold) ||
        bad(c.rest) || !std::isfinite(c.most) || c.most < c.hold || c.most <= 0)
        throw std::invalid_argument("Invalid web dodge configuration");
}
void WebDodge::reset() {
    dodging_ = false;
    age_ = restLeft_ = 0;
    sinceWeb_ = 1e9f;
    way_ = {};
}
void WebDodge::refused() {
    if (dodging_ && dodges_)
        --dodges_;
    dodging_ = false;
    restLeft_ = config_.rest;
}
WebDodge::Ask WebDodge::update(float seconds, bool webbed, bool airborne, Vec3 velocity,
                               std::span<const WebDodgeThreat> threats, GameDodge game) {
    const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
    restLeft_ = std::max(0.f, restLeft_ - dt);
    sinceWeb_ = webbed ? 0.f : std::min(sinceWeb_ + dt, 1e9f);
    // A blow still to land, or only just landed: the dodge holds for it.
    const auto coming = [&](const WebDodgeThreat& t) {
        return t.attacker && t.timed && std::isfinite(t.toImpact) && t.toImpact >= -config_.late;
    };
    if (dodging_) {
        // The game ended it itself (a state of the hero's that ends dodges
        // as it ends): nothing left to end.
        if (game != GameDodge::ours) {
            dodging_ = false;
            restLeft_ = config_.rest;
            ++cutShort_;
            return Ask::none;
        }
        age_ += dt;
        const bool waiting = std::any_of(threats.begin(), threats.end(), coming);
        if (age_ >= config_.most || (age_ >= config_.hold && !waiting)) {
            dodging_ = false;
            restLeft_ = config_.rest;
            return Ask::end;
        }
        return Ask::none;
    }
    const float speed = finite(velocity) ? length(velocity) : 0.f;
    const bool carried = (webbed || (airborne && sinceWeb_ <= config_.coast)) && speed >= config_.speed &&
                         speed > 1e-3f;
    if (!carried || game != GameDodge::none || restLeft_ > 0)
        return Ask::none;
    const bool due = std::any_of(threats.begin(), threats.end(), [&](const WebDodgeThreat& t) {
        if (!t.attacker || t.answered)
            return false;
        if (t.timed)
            return coming(t) && t.toImpact <= config_.lead;
        return std::isfinite(t.warned) && t.warned >= config_.untimed;
    });
    if (!due)
        return Ask::none;
    const Vec3 level{velocity.x, 0, velocity.z};
    way_ = length(level) > .25f * speed ? normalized(level) : velocity / speed;
    dodging_ = true;
    age_ = 0;
    ++dodges_;
    return Ask::start;
}
SenseSight senseSight(Pose head, Vec3 target, SenseSight before, const SenseSightConfig& config) {
    SenseSight sight = before;
    const Vec3 to = target - head.position;
    if (!finite(to) || length(to) < 1e-3f || !finite(head.position)) {
        sight.shown = false;
        return sight;
    }
    const Vec3 d = normalized(to);
    const Quat q = head.orientation;
    const Vec3 forward = q.rotate({0, 0, -1}), right = q.rotate({1, 0, 0}), up = q.rotate({0, 1, 0});
    const float off = std::acos(std::clamp(dot(d, forward), -1.f, 1.f));
    sight.shown = before.shown ? off > config.back : off > config.away;
    // Across the view toward him; straight behind, the side he was on (a
    // new one: the side he is a little more to).
    const float x = dot(d, right), y = dot(d, up), across = std::sqrt(x * x + y * y);
    if (across > .2f) {
        sight.x = x / across;
        sight.y = y / across;
    } else if (!before.shown) {
        sight.x = x < 0 ? -1.f : 1.f;
        sight.y = 0;
    }
    return sight;
}
} // namespace spidy

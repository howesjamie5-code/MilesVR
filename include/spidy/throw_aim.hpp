#pragma once
#include "game_swing.hpp"
#include "math.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

// The game's throw of a prop, aimed with the free hand. The user, October 24,
// with X throwing at last: "really hard or not possible to aim where the throw
// will go", at enemies and at a spot alike; and for the aim, the hand that is
// not on X, pointing.
//
// The throw at LB + RB is the hero's HeroStateWebYankEnviro. It takes its
// target from the hero's aim manager (Hero::HeroAimManagerLocal, vtable
// 38b1658), slot 4 of the results its +0xB9CC handle names, read through
// three getters as the throw is held (0xa52270): the actor to throw at
// (957620, its handle at +0x9FB8 + 0x18 slot), the aim point on him (957640,
// +0x9FC4 + 0x18 slot: a byte index into his target points, his feet where
// he has none) and the flat way to throw with no actor (9575d0, x at +0x9B28
// and z at +0x9B30, stride 0x98). Left alone that way is the game camera's
// heading (measured October 24: -39.5 degrees against the camera's -39.8);
// the game reads the slot some hundred times in a throw's hold, and nothing
// reads it outside one. game_throw hands out the hand's instead: the enemy it
// points at, or the way it points.
namespace spidy::throw_aim {
enum Flags : uint32_t {
    active = 1,    // a hand aims the throw now
    character = 2, // at the character whose centre `point` is
};
// SpidyThrowAim's input, from the XR worker every frame of a throw's hold.
struct Request {
    uint32_t magic = 0x41525453, version = 1, bytes = sizeof(Request), flags{};
    Vec3 direction{}; // the way to throw on the ground's plane, unit (y 0)
    Vec3 point{};     // what the hand points at: a character's centre, a surface, the air at reach
};
static_assert(sizeof(Request) == 40);
// `v` on the ground's plane, unit; zero for one that is (nearly) straight up
// or down.
inline Vec3 flat(Vec3 v) {
    const float n = std::hypot(v.x, v.z);
    return std::isfinite(n) && n > 1e-3f ? Vec3{v.x / n, 0, v.z / n} : Vec3{};
}
// The throw a free hand aims: `aim` its aim preview (game_swing's, the point
// its web would go to), `forward` its ray, `player` where he stands. At a
// character, at him; at anything else, the way from the player to that
// point; with no point (the hand untracked, its web busy), the way the hand
// points. A hand pointing straight up or down aims nothing.
inline Request fromHand(const game_swing::Aim& aim, Vec3 forward, Vec3 player) {
    Request r;
    const auto kind = static_cast<game_swing::AimKind>(aim.kind);
    const bool pointed = kind != game_swing::AimKind::none && finite(aim.point);
    Vec3 way = pointed ? flat(aim.point - player) : Vec3{};
    if (!(length(way) > .5f))
        way = flat(forward);
    if (!(length(way) > .5f))
        return r;
    r.flags = active | (pointed && kind == game_swing::AimKind::character ? character : 0u);
    r.direction = way;
    r.point = pointed ? aim.point : player + way * 20.f;
    return r;
}
// An actor's handle as the game makes one from the actor (0xa52429): the
// generation (the word at +0x10, 11 bits) over the index (+0xC, 20 bits).
constexpr uint32_t handle(uint32_t index, uint16_t generation) {
    return (static_cast<uint32_t>(generation & 0x7ffu) << 20) | (index & 0xfffffu);
}
// Of `count` characters standing at `feet`, the one whose body (feet to 2 m
// up) comes nearest `point` and within `within` metres of it; -1 for none.
inline int nearest(const Vec3* feet, int count, Vec3 point, float within) {
    int best = -1;
    float closest = within;
    for (int i = 0; i < count; ++i) {
        if (!finite(feet[i]))
            continue;
        const float up = std::clamp(point.y - feet[i].y, 0.f, 2.f);
        const float d = length(point - (feet[i] + Vec3{0, up, 0}));
        if (d < closest) {
            closest = d;
            best = i;
        }
    }
    return best;
}
} // namespace spidy::throw_aim

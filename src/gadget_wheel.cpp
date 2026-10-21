#include "spidy/gadget_wheel.hpp"
#include <algorithm>
#include <cmath>

namespace spidy {
namespace {
constexpr float pi = 3.14159265358979f;
constexpr Vec3 worldUp{0, 1, 0};
Vec3 across(Vec3 v, Vec3 axis) {
    return v - axis * dot(v, axis);
}
} // namespace

const char* gadgetName(Gadget g) {
    switch (g) {
    case Gadget::webShooter:
        return "WEB SHOOTER";
    case Gadget::impactWeb:
        return "IMPACT WEB";
    case Gadget::webBomb:
        return "WEB BOMB";
    case Gadget::electricWeb:
        return "ELECTRIC WEB";
    case Gadget::tripMine:
        return "TRIP MINE";
    case Gadget::concussionBlast:
        return "CONCUSSION BLAST";
    case Gadget::spiderDrone:
        return "SPIDER-DRONE";
    case Gadget::suspensionMatrix:
        return "SUSPENSION MATRIX";
    case Gadget::gameWebShooter:
        return "WEB SHOOTER";
    case Gadget::remoteMine:
        return "REMOTE MINE";
    case Gadget::gravityWell:
        return "GRAVITY WELL";
    case Gadget::holoDrone:
        return "HOLO DRONE";
    default:
        return "";
    }
}

GadgetWheel::Event GadgetWheel::update(float seconds, bool held, bool allowed, Vec3 hand, Vec3 head,
                                       uint8_t places) {
    Event e;
    const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
    if (!allowed) {
        down_ = open_ = false;
        held_ = age_ = litAge_ = 0;
        lit_ = -1;
        spent_ = held;
        return e;
    }
    if (spent_) {
        spent_ = held;
        return e;
    }
    if (held && !down_) {
        down_ = true;
        held_ = 0;
    }
    if (down_ && held) {
        held_ += dt;
        if (!open_ && held_ >= holdSeconds && finite(hand) && finite(head)) {
            // Square to the line from the hand to the eyes, upright.
            Vec3 out = normalized(head - hand);
            if (length(out) < .5f)
                out = {0, 0, 1};
            Vec3 up = normalized(across(worldUp, out));
            if (length(up) < .5f)
                up = normalized(across({0, 0, -1}, out));
            frame_ = {hand, cross(up, out), up, out};
            open_ = e.opened = true;
            age_ = litAge_ = 0;
            lit_ = -1;
        }
    }
    if (open_) {
        age_ += dt;
        const Vec3 d = hand - frame_.centre;
        const float x = dot(d, frame_.right), y = dot(d, frame_.up), r = std::hypot(x, y);
        int now = -1;
        if (finite(hand) && r >= (lit_ >= 0 ? release : pick)) {
            // The gadget nearest the way the hand moved.
            const float a = std::atan2(x, y);
            float nearest = 2 * pi;
            for (int p = 0; p < gadgetPlaces; ++p)
                if (const float off = std::abs(std::remainder(a - gadgetPlaceAngle(p), 2 * pi));
                    (places >> p & 1) && off < nearest) {
                    nearest = off;
                    now = p;
                }
        }
        e.lit = now != lit_ && now >= 0;
        litAge_ = now != lit_ ? 0 : litAge_ + dt;
        lit_ = now;
    }
    if (!held && down_) {
        if (open_)
            e.chosen = lit_;
        else
            e.tap = true;
        down_ = open_ = false;
        held_ = age_ = litAge_ = 0;
        lit_ = -1;
    }
    return e;
}
} // namespace spidy

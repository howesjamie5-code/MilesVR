#include "spidy/gadget_wheel.hpp"
#include "spidy/overlay_text.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace spidy {
namespace {
constexpr float pi = 3.14159265358979f;
constexpr Vec3 worldUp{0, 1, 0};
Vec3 across(Vec3 v, Vec3 axis) {
    return v - axis * dot(v, axis);
}
// The direction of place `p` from the middle: the first at the top, the rest
// clockwise as seen from in front.
float placeAngle(int p) {
    return 2 * pi * static_cast<float>(p) / gadgetPlaces;
}
// Sizes in metres and colours (linear RGB), as the other panels have them.
constexpr float ringRadius = .13f, marker = .024f, litMarker = .032f, nameHeight = .011f, litNameHeight = .016f;
constexpr Vec3 dark{.02f, .022f, .03f}, white{1, 1, 1}, soft{.78f, .8f, .84f}, amber{1, .52f, .05f},
    accent{.62f, .02f, .03f};
struct Plane {
    WheelFrame f;
    float scale = 1;
    Vec3 at(float x, float y, float z = 0) const {
        return f.centre + f.right * (x * scale) + f.up * (y * scale) + f.out * z;
    }
    void disc(std::vector<Vertex>& v, float x, float y, float radius, float z, Vec3 color) const {
        constexpr int segments = 24;
        const Vec3 middle = at(x, y, z);
        for (int i = 0; i < segments; ++i) {
            const float a0 = 2 * pi * i / segments, a1 = 2 * pi * (i + 1) / segments;
            v.insert(v.end(), {Vertex{middle, color},
                               Vertex{at(x + radius * std::sin(a0), y + radius * std::cos(a0), z), color},
                               Vertex{at(x + radius * std::sin(a1), y + radius * std::cos(a1), z), color}});
        }
    }
    void ring(std::vector<Vertex>& v, float x, float y, float inner, float outer, float z, Vec3 color) const {
        constexpr int segments = 32;
        for (int i = 0; i < segments; ++i) {
            const float a0 = 2 * pi * i / segments, a1 = 2 * pi * (i + 1) / segments;
            const Vec3 i0 = at(x + inner * std::sin(a0), y + inner * std::cos(a0), z),
                       o0 = at(x + outer * std::sin(a0), y + outer * std::cos(a0), z),
                       o1 = at(x + outer * std::sin(a1), y + outer * std::cos(a1), z),
                       i1 = at(x + inner * std::sin(a1), y + inner * std::cos(a1), z);
            v.insert(v.end(), {Vertex{i0, color}, Vertex{o0, color}, Vertex{o1, color}, Vertex{i0, color},
                               Vertex{o1, color}, Vertex{i1, color}});
        }
    }
    // Text on its baseline at (x, y): `align` 0 from x, .5 centred on it, 1
    // ending at it; shrunk to `width`.
    void text(std::vector<Vertex>& v, const char* s, float x, float y, float height, float width, Vec3 color,
              float align = .5f) const {
        TextStyle style;
        style.height = fittedHeight(s, height * scale, width * scale);
        style.color = color;
        appendText(v, s, at(x, y, .003f) - f.right * (textWidth(s, style.height) * align), f.right, f.up, style);
    }
};
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
    case Gadget::webBlossom:
        return "WEB BLOSSOM";
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
        held_ = age_ = 0;
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
            age_ = 0;
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
                if (const float off = std::abs(std::remainder(a - placeAngle(p), 2 * pi));
                    (places >> p & 1) && off < nearest) {
                    nearest = off;
                    now = p;
                }
        }
        e.lit = now != lit_ && now >= 0;
        lit_ = now;
    }
    if (!held && down_) {
        if (open_)
            e.chosen = lit_;
        else
            e.tap = true;
        down_ = open_ = false;
        held_ = age_ = 0;
        lit_ = -1;
    }
    return e;
}

void appendGadgetWheel(std::vector<Vertex>& out, const GadgetWheelView& view) {
    if (!view.open || !finite(view.frame.centre))
        return;
    // It opens out from the hand over an eighth of a second.
    const Plane plane{view.frame, std::clamp(std::isfinite(view.age) ? view.age / .12f : 1.f, .2f, 1.f)};
    plane.ring(out, 0, 0, ringRadius - .002f, ringRadius + .002f, 0, dark);
    plane.disc(out, 0, 0, .008f, .001f, soft);
    for (int p = 0; p < gadgetPlaces; ++p) {
        const Gadget g = view.places[static_cast<size_t>(p)];
        if (g == Gadget::none)
            continue;
        const float x = ringRadius * std::sin(placeAngle(p)), y = ringRadius * std::cos(placeAngle(p));
        const bool lit = p == view.lit;
        plane.disc(out, x, y, lit ? litMarker : marker, .001f, lit ? amber : dark);
        plane.ring(out, x, y, (lit ? litMarker : marker) - .003f, lit ? litMarker : marker, .002f,
                   lit ? white : soft);
        if (p == view.equipped)
            plane.ring(out, x, y, litMarker + .004f, litMarker + .009f, .002f, accent);
        // Each name outside the ring beside its place - to the right of the
        // places on the right, to the left on the left, above the top one and
        // under the bottom one - and the lit one's in the middle.
        const float side = std::sin(placeAngle(p)), up = std::cos(placeAngle(p)), gap = marker + .008f;
        const Vec3 colour = lit ? amber : white;
        if (side > .3f)
            plane.text(out, gadgetName(g), x + gap, y - nameHeight / 2, nameHeight, .13f, colour, 0);
        else if (side < -.3f)
            plane.text(out, gadgetName(g), x - gap, y - nameHeight / 2, nameHeight, .13f, colour, 1);
        else if (up > 0)
            plane.text(out, gadgetName(g), x, y + gap, nameHeight, .13f, colour);
        else
            plane.text(out, gadgetName(g), x, y - gap - nameHeight, nameHeight, .13f, colour);
        if (const auto most = view.most[static_cast<size_t>(p)]) {
            char count[16];
            std::snprintf(count, sizeof count, "%u/%u", static_cast<unsigned>(view.ammo[static_cast<size_t>(p)]),
                          static_cast<unsigned>(most));
            plane.text(out, count, x, y - .0035f, .012f, .04f, lit ? dark : soft);
        }
    }
    if (view.lit >= 0 && view.lit < gadgetPlaces)
        plane.text(out, gadgetName(view.places[static_cast<size_t>(view.lit)]), 0, -.035f, litNameHeight, .15f,
                   amber);
}
} // namespace spidy

#include "spidy/web_visual.hpp"
#include "spidy/gadget_wheel.hpp"
#include "spidy/overlay_text.hpp"
#include <string>

namespace spidy {
namespace {
constexpr float pi = 3.14159265f;
constexpr float braid = .075f; // metres per visible twist of the strand
Vec3 mix(Vec3 a, Vec3 b, float t) {
    return a + (b - a) * t;
}
Vec3 brightest(Vec3 c) {
    return {std::min(c.x, 1.f), std::min(c.y, 1.f), std::min(c.z, 1.f)};
}
void quad(std::vector<Vertex>& out, const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d) {
    out.insert(out.end(), {a, b, c, a, c, d});
}
Vec3 perpendicular(Vec3 d) {
    return normalized(cross(d, std::abs(d.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
}
float halfWidth(const WebLook& look, Vec3 viewer, Vec3 p, float pixelAngle, float radius) {
    return std::max(radius, .5f * look.minPixels * pixelAngle * length(viewer - p));
}
// Deterministic 0..1 value per strand, so a splat keeps its shape.
float hash(float seed, int i) {
    const float s = std::sin(seed * 12.9898f + i * 78.233f) * 43758.547f;
    return s - std::floor(s);
}
void glob(std::vector<Vertex>& out, Vec3 center, Vec3 viewer, float radius, Vec3 color) {
    const Vec3 v = normalized(viewer - center);
    const Vec3 a = perpendicular(v), b = cross(v, a);
    constexpr int sides = 8;
    for (int i = 0; i < sides; ++i) {
        const float t0 = 2 * pi * i / sides, t1 = 2 * pi * (i + 1) / sides;
        const Vec3 p0 = center + (a * std::cos(t0) + b * std::sin(t0)) * radius;
        const Vec3 p1 = center + (a * std::cos(t1) + b * std::sin(t1)) * radius;
        out.insert(out.end(), {{center, color}, {p0, color * .8f}, {p1, color * .8f}});
    }
}
// Soft-edged shapes for the aim markers: at `alpha` inside, fading to nothing
// across `soft` metres at every edge, so their outlines stay smooth without
// multisampling while they move by fractions of a pixel.
//
// An arc of a flat ring around c in the plane of unit axes a and b, from
// angle t0 to t1 (a whole ring when they span 2 pi) in `segments` pieces a
// whole turn, between radii inner and outer (0: a disc); its inner edge
// fades across `innerSoft` instead when that is given.
void softArc(std::vector<Vertex>& out, Vec3 c, Vec3 a, Vec3 b, float inner, float outer, float t0, float t1,
             Vec3 color, float alpha, float soft, int segments, float innerSoft = -1) {
    const bool whole = t1 - t0 > 2 * pi - 1e-3f;
    const int pieces = std::max(2, static_cast<int>(std::ceil((t1 - t0) / (2 * pi) * segments)));
    const float radii[4] = {std::max(inner - (innerSoft >= 0 ? innerSoft : soft), 0.f), inner, outer, outer + soft};
    const float alphas[4] = {inner > 0 ? 0.f : alpha, alpha, alpha, 0};
    const auto at = [&](float t, int k, float opacity) {
        return Vertex{c + (a * std::cos(t) + b * std::sin(t)) * radii[k], color, opacity};
    };
    const int first = inner > 0 ? 0 : 1; // a disc has no inner edge
    for (int i = 0; i < pieces; ++i) {
        const float u0 = t0 + (t1 - t0) * i / pieces, u1 = t0 + (t1 - t0) * (i + 1) / pieces;
        for (int k = first; k < 3; ++k)
            quad(out, at(u0, k, alphas[k]), at(u1, k, alphas[k]), at(u1, k + 1, alphas[k + 1]),
                 at(u0, k + 1, alphas[k + 1]));
    }
    if (whole)
        return;
    // Each end fades out along the ring.
    const float cap = soft / std::max((inner + outer) / 2, 1e-6f);
    for (const float end : {t0, t1}) {
        const float beyond = end == t0 ? t0 - cap : t1 + cap;
        for (int k = first; k < 3; ++k)
            quad(out, at(end, k, alphas[k]), at(beyond, k, 0), at(beyond, k + 1, 0), at(end, k + 1, alphas[k + 1]));
    }
}
// A flat bar from p to q across the line of sight `view`, its ends squared
// off half its width beyond them so two bars close a corner.
void softBar(std::vector<Vertex>& out, Vec3 p, Vec3 q, Vec3 view, float halfWidth, Vec3 color, float alpha,
             float soft) {
    const Vec3 along = normalized(q - p), side = normalized(cross(q - p, view));
    const float span = length(q - p);
    const float x[4] = {-halfWidth - soft, -halfWidth, span + halfWidth, span + halfWidth + soft};
    const float y[4] = {-halfWidth - soft, -halfWidth, halfWidth, halfWidth + soft};
    const auto at = [&](int i, int j) {
        return Vertex{p + along * x[i] + side * y[j], color, i % 3 && j % 3 ? alpha : 0.f};
    };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            quad(out, at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1));
}
// A flat triangle across the line of sight `view`, grown `grow` past its
// corners' edges and then fading out across `soft`.
void softTriangle(std::vector<Vertex>& out, std::array<Vec3, 3> p, Vec3 view, Vec3 color, float alpha, float grow,
                  float soft) {
    const Vec3 centre = (p[0] + p[1] + p[2]) / 3;
    std::array<Vec3, 3> outward{}, mitre{};
    for (int i = 0; i < 3; ++i) {
        const Vec3 normal = normalized(cross(p[(i + 1) % 3] - p[i], view));
        outward[i] = dot(normal, (p[i] + p[(i + 1) % 3]) * .5f - centre) < 0 ? -normal : normal;
    }
    // A corner moves out along the bisector of its edges' normals, far
    // enough to move both edges out by one unit, but no more than three.
    for (int i = 0; i < 3; ++i) {
        const Vec3 bisector = normalized(outward[(i + 2) % 3] + outward[i]);
        mitre[i] = bisector / std::max(dot(bisector, outward[i]), 1.f / 3);
    }
    std::array<Vec3, 3> inside{}, edge{};
    for (int i = 0; i < 3; ++i) {
        inside[i] = p[i] + mitre[i] * grow;
        edge[i] = p[i] + mitre[i] * (grow + soft);
    }
    out.insert(out.end(), {{inside[0], color, alpha}, {inside[1], color, alpha}, {inside[2], color, alpha}});
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        quad(out, {inside[i], color, alpha}, {inside[j], color, alpha}, {edge[j], color, 0}, {edge[i], color, 0});
    }
}
// Each hand's aim markers in their own colour (linear RGB): sky blue for the
// left hand, orange for the right. A miss is red for either.
constexpr Vec3 handColors[2] = {{.13f, .55f, 1.f}, {1.f, .4f, .05f}};
constexpr Vec3 missColor{.9f, .07f, .05f}, shadowColor{.01f, .012f, .018f};
// Tapered camera-facing strand for the impact splat.
void strand(std::vector<Vertex>& out, Vec3 from, Vec3 to, Vec3 viewer, float pixelAngle, const WebLook& look,
            float rootRadius) {
    constexpr int pieces = 4;
    for (int i = 0; i < pieces; ++i) {
        const float u0 = float(i) / pieces, u1 = float(i + 1) / pieces;
        const Vec3 p0 = mix(from, to, u0), p1 = mix(from, to, u1);
        const Vec3 side0 = normalized(cross(to - from, viewer - p0)), side1 = normalized(cross(to - from, viewer - p1));
        const float w0 = halfWidth(look, viewer, p0, pixelAngle, rootRadius * (1 - .7f * u0));
        const float w1 = halfWidth(look, viewer, p1, pixelAngle, rootRadius * (1 - .7f * u1));
        const Vec3 c0 = mix(look.core, look.edge, .25f + .3f * u0), c1 = mix(look.core, look.edge, .25f + .3f * u1);
        quad(out, {p0 - side0 * w0, c0 * .75f}, {p1 - side1 * w1, c1 * .75f}, {p1 + side1 * w1, c1},
             {p0 + side0 * w0, c0});
    }
}
// The combat marks are printed, as the graphics of Spidy's videos are (their
// style system's plates, keylines and timing): flat colour, every shape keyed
// in ink, a hard drop to one side where a shadow would be, and nothing soft
// but an edge's last pixel. They move on twos: a mark is a pose held a
// twelfth of a second, then the next; nothing eases, slides or fades.
namespace plate {
// The style's colour tokens in linear RGB.
constexpr Vec3 ink{.0033f, .0030f, .0070f};   // #0b0a14: every keyline
constexpr Vec3 paper{.9301f, .9131f, .8550f}; // #f7f5ee
constexpr Vec3 red{.7682f, .0194f, .0284f};   // #e3262f, the brand's
constexpr Vec3 yellow{1.f, .7529f, .0742f};   // #ffe14d: a burst, a finisher
constexpr Vec3 cyan{.0060f, .6724f, 1.f};     // #12d6ff: the misprint's left ghost
constexpr Vec3 magenta{1.f, .0273f, .2918f};  // #ff2e93: its right ghost
constexpr float pose = 1.f / 12, degree = pi / 180;
// How far the misprint's ghosts lie to either side on the poses a mark
// lands, as a share of its size.
constexpr float misprint = .11f;
} // namespace plate
// How a mark lands, the style's "pop": three held poses (small and turned,
// past its size, just short of it), then at rest. `hit`: the poses on which
// its misprint is wide.
struct Landing {
    float scale = 1, turn{};
    bool hit{};
};
Landing landing(float age) {
    switch (static_cast<int>(age / plate::pose)) {
    case 0:
        return {.3f, 10 * plate::degree, true};
    case 1:
        return {1.16f, -5 * plate::degree, true};
    case 2:
        return {.95f, 2 * plate::degree, false};
    default:
        return {};
    }
}
struct Flat {
    float x{}, y{};
    Flat operator+(Flat b) const {
        return {x + b.x, y + b.y};
    }
    Flat operator-(Flat b) const {
        return {x - b.x, y - b.y};
    }
    Flat operator*(float s) const {
        return {x * s, y * s};
    }
};
// One printed mark, flat across the line of sight. Its shapes are placed in
// its own plane (x to the viewer's right, y up, metres from its centre),
// sized and turned as a whole by `place`, and laid down a plate at a time by
// `layers`.
class Print {
  public:
    Print(std::vector<Vertex>& out, Vec3 centre, Vec3 right, Vec3 up, Vec3 view, float px)
        : out_(out), centre_(centre), right_(right), up_(up), view_(view), px_(px) {
        place(1, 0);
    }
    // From here on the shapes are `scale` their size and turned by `turn`
    // (radians, anticlockwise) about a centre `moved` from the mark's.
    void place(float scale, float turn, Flat moved = {}) {
        scale_ = scale;
        across_ = right_ * std::cos(turn) + up_ * std::sin(turn);
        along_ = up_ * std::cos(turn) - right_ * std::sin(turn);
        moved_ = moved;
    }
    // Draws what `draw` draws as print lays it down. Under everything the
    // misprint's two ghosts, cyan `misprint` to the left and magenta as far
    // to the right (0: none); then the hard drop in `dropColor`, `drop` to
    // the right and down (0: none); the keyline, ink `key` wide around every
    // shape; and the shapes in their own colours.
    template <class Draw> void layers(float key, float drop, Vec3 dropColor, float misprint, Draw&& draw) {
        const float grow = std::max(key * scale_, 1.1f * px_);
        const auto layer = [&](Flat shift, const Vec3* tinted, float grown) {
            shift_ = shift * scale_;
            plate_ = tinted;
            grow_ = grown;
            draw();
        };
        // What lies under the keyline is a pixel smaller than it, so that an
        // edge both share shows none of it.
        const float under = std::max(grow - px_, 0.f);
        if (misprint > 0) {
            layer({-misprint, 0}, &plate::cyan, under);
            layer({misprint, 0}, &plate::magenta, under);
        }
        if (drop > 0)
            layer({drop, -drop}, &dropColor, under);
        layer({}, &plate::ink, grow);
        layer({}, nullptr, 0);
    }
    void disc(Flat at, float radius, Vec3 color) {
        dot(world(at), radius * scale_ + grow_, tint(color));
    }
    // A ring `half` to either side of `radius`, or its arc from angle `from`
    // to `to` (anticlockwise from the mark's right).
    void ring(Flat at, float radius, float half, Vec3 color, float from = 0, float to = 2 * pi) {
        const float r = radius * scale_, h = half * scale_ + grow_;
        const float beyond = to - from > 2 * pi - 1e-3f ? 0.f : grow_ / std::max(r, 1e-6f);
        softArc(out_, world(at), across_, along_, std::max(r - h, 0.f), r + h, from - beyond, to + beyond,
                tint(color), 1, soft(), segments(r + h));
    }
    // A stroke with round ends, as a marker draws it. `both` false: one
    // that goes on from the end of the stroke before, whose round end is its
    // start.
    void line(Flat p, Flat q, float half, Vec3 color, bool both = true) {
        const Vec3 a = world(p), b = world(q), c = tint(color);
        const float h = half * scale_ + grow_;
        if (length(b - a) > 1e-6f) {
            const Vec3 side = normalized(cross(b - a, view_));
            const float across[4] = {-h - soft(), -h, h, h + soft()}, alpha[4] = {0, 1, 1, 0};
            for (int i = 0; i < 3; ++i)
                quad(out_, {a + side * across[i], c, alpha[i]}, {b + side * across[i], c, alpha[i]},
                     {b + side * across[i + 1], c, alpha[i + 1]}, {a + side * across[i + 1], c, alpha[i + 1]});
        }
        if (both)
            dot(a, h, c);
        dot(b, h, c);
    }
    void triangle(Flat a, Flat b, Flat c, Vec3 color) {
        softTriangle(out_, {world(a), world(b), world(c)}, view_, tint(color), 1, grow_, soft());
    }
    // A burst: a star of `points` spikes from radius `inner` out to `outer`,
    // each its own length and lean by `seed`.
    void star(Flat at, float inner, float outer, int points, float seed, Vec3 color) {
        disc(at, inner, color);
        const float step = 2 * pi / static_cast<float>(points);
        const auto on = [&](float angle, float radius) {
            return Flat{at.x + std::cos(angle) * radius, at.y + std::sin(angle) * radius};
        };
        for (int i = 0; i < points; ++i) {
            const float middle = seed + step * (static_cast<float>(i) + .3f * (hash(seed, i) - .5f));
            const float tip = inner + (outer - inner) * (.55f + .45f * hash(seed, i + 23));
            triangle(on(middle - step * .56f, inner * .96f),
                     on(middle + step * .3f * (hash(seed, i + 47) - .5f), tip),
                     on(middle + step * .56f, inner * .96f), color);
        }
    }
    // Lettering in the overlay's stroke font, leaning as italics do, from
    // the left end of its baseline.
    static constexpr float lean = .18f;
    void text(std::string_view words, Flat origin, float height, float weight, Vec3 color) {
        TextStyle style;
        style.height = height * scale_;
        style.weight = weight + 2 * grow_ / std::max(style.height, 1e-6f);
        style.color = tint(color);
        style.round = true;
        style.soft = soft();
        appendText(out_, words, world(origin), across_, along_ + across_ * lean, style);
    }

  private:
    Vec3 world(Flat at) const {
        return centre_ + across_ * (at.x * scale_) + along_ * (at.y * scale_) + right_ * (moved_.x + shift_.x) +
               up_ * (moved_.y + shift_.y);
    }
    Vec3 tint(Vec3 color) const {
        return plate_ ? *plate_ : color;
    }
    // An edge's last pixel.
    float soft() const {
        return .9f * px_;
    }
    // Enough sides for a circle to stay round to a third of a pixel.
    int segments(float radius) const {
        return std::clamp(static_cast<int>(3.4f * std::sqrt(radius / px_)), 8, 48);
    }
    // A disc with a soft rim.
    void dot(Vec3 centre, float radius, Vec3 color) {
        const int sides = segments(radius);
        const float rim = radius + soft();
        Vec3 from = across_;
        for (int i = 1; i <= sides; ++i) {
            const float angle = 2 * pi * static_cast<float>(i) / static_cast<float>(sides);
            const Vec3 to = across_ * std::cos(angle) + along_ * std::sin(angle);
            const Vertex p{centre + from * radius, color}, q{centre + to * radius, color};
            out_.insert(out_.end(), {{centre, color}, p, q});
            quad(out_, p, q, {centre + to * rim, color, 0}, {centre + from * rim, color, 0});
            from = to;
        }
    }
    std::vector<Vertex>& out_;
    Vec3 centre_, right_, up_, view_, across_{}, along_{};
    float px_, scale_ = 1, grow_{};
    Flat moved_{}, shift_{};
    const Vec3* plate_{};
};
} // namespace

void appendWeb(std::vector<Vertex>& out, const WebLine& web, Vec3 viewer, float pixelAngle, const WebLook& look) {
    const Vec3 chord = web.end - web.start;
    const float distance = length(chord);
    if (!finite(web.start) || !finite(web.end) || !finite(viewer) || distance < .05f || distance > 1000 ||
        !std::isfinite(pixelAngle) || pixelAngle <= 0 || !std::isfinite(web.extended) || web.extended <= 0)
        return;
    const float extended = std::min(web.extended, 1.f);
    // Parabolic approximation: arc length ~= chord + 8 h^2 / (3 chord).
    const float slack = std::isfinite(web.slack) ? std::max(web.slack, 0.f) : 0.f;
    const float sag = std::min(std::sqrt(3 * distance * slack / 8), distance * .5f);
    const Vec3 down{0, -1, 0};
    auto point = [&](float u) { return web.start + chord * u + down * (sag * 4 * u * (1 - u)); };
    auto tangent = [&](float u) { return normalized(chord + down * (sag * 4 * (1 - 2 * u))); };
    // Short segments near the viewer resolve the braid; distant ones only need
    // to follow the curve.
    float u = 0, travelled = 0;
    bool first = true;
    Vertex previous[5]{};
    constexpr float columns[5] = {-1, -.5f, 0, .5f, 1};
    while (true) {
        const Vec3 p = point(u);
        const float range = length(viewer - p);
        const Vec3 t = tangent(u);
        Vec3 side = cross(t, normalized(viewer - p));
        side = length(side) > 1e-4f ? normalized(side) : perpendicular(t);
        const float w = halfWidth(look, viewer, p, pixelAngle, look.radius);
        // Fade the braid out before it would alias at a distance.
        const float detail = std::clamp(1 - range / 14, 0.f, 1.f);
        const float phase = 2 * pi * travelled / braid;
        const float sheen = std::sin(phase) * .55f;
        Vertex current[5];
        for (int c = 0; c < 5; ++c) {
            const float x = columns[c];
            const float body = 1 - x * x;
            const float highlight = .3f * detail * std::exp(-(x - sheen) * (x - sheen) / .09f);
            const float band = 1 - detail * .1f * (.5f + .5f * std::cos(phase * 2));
            current[c] = {p + side * (w * x), brightest(mix(look.edge, look.core, body) * band +
                                                        Vec3{highlight, highlight, highlight})};
        }
        if (!first)
            for (int c = 0; c < 4; ++c)
                quad(out, previous[c], current[c], current[c + 1], previous[c + 1]);
        std::copy(std::begin(current), std::end(current), std::begin(previous));
        first = false;
        if (u >= extended)
            break;
        const float step = std::clamp(.03f + .025f * range, .03f, 6.f);
        const float du = step / std::max(distance, step);
        u = std::min(u + du, extended);
        travelled += step;
    }
    if (extended < 1) {
        glob(out, point(extended), viewer, halfWidth(look, viewer, point(extended), pixelAngle, .018f), look.core);
        return;
    }
    if (!web.splat)
        return;
    // Impact web: a central glob with strands spread across the surface. The
    // surface normal is unknown here; face the strands back along the web.
    const Vec3 normal = normalized(web.start - web.end);
    const Vec3 a = perpendicular(normal), b = cross(normal, a);
    const Vec3 center = web.end + normal * .004f;
    constexpr int strands = 7;
    for (int i = 0; i < strands; ++i) {
        const float angle = 2 * pi * (i + .35f * hash(web.seed, i)) / strands + web.seed;
        const Vec3 direction = a * std::cos(angle) + b * std::sin(angle);
        const float reach = .09f + .13f * hash(web.seed, i + 17);
        const Vec3 curl = (a * -std::sin(angle) + b * std::cos(angle)) * (reach * .18f * (hash(web.seed, i + 31) - .5f));
        strand(out, center, center + direction * reach + curl, viewer, pixelAngle, look, .0055f);
    }
    glob(out, center + normal * .002f, viewer, halfWidth(look, viewer, center, pixelAngle, .026f), look.core);
}
void appendAimMarker(std::vector<Vertex>& out, const AimMarker& marker, Vec3 viewer, float pixelAngle) {
    const float distance = length(viewer - marker.point);
    const float opacity = std::isfinite(marker.opacity) ? std::clamp(marker.opacity, 0.f, 1.f) : 0.f;
    if (!finite(marker.point) || !finite(viewer) || !std::isfinite(pixelAngle) || pixelAngle <= 0 ||
        !(distance >= .3f && distance <= 1000) || opacity <= 0)
        return;
    // Level and upright across the line of sight, so the marker never rolls.
    const Vec3 view = (viewer - marker.point) / distance;
    Vec3 right = cross(Vec3{0, 1, 0}, view);
    right = length(right) > .1f ? normalized(right) : perpendicular(view);
    const Vec3 up = cross(view, right);
    // Metres per pixel at the marker, and how far the grip has closed toward a shot.
    const float px = pixelAngle * distance;
    const float squeeze = std::isfinite(marker.squeeze) ? std::clamp(marker.squeeze / .65f, 0.f, 1.f) : 0.f;
    const Vec3 c = marker.point;
    const Vec3 color = handColors[marker.hand ? 1 : 0], light = mix(color, {1, 1, 1}, .6f);
    // Every shape is drawn twice, first all of a marker's shadows, then its
    // colours: a soft dark shadow reaching `shade` past the shape keeps it
    // readable on the sky and on a lit wall.
    const float shade = 1.3f * px, shadowSoft = 1.8f * px, soft = .8f * px, shadowAlpha = .6f;
    const auto segments = [&](float radius) { return std::clamp(static_cast<int>(radius / px) + 12, 16, 72); };
    // An arc's shadow lies mostly outside it, a thin edge within, and
    // reaches past its ends by `lengthen` metres.
    const auto arc = [&](bool shadow, Vec3 tint, float alpha, float r, float halfWidth, float from = 0,
                         float to = 2 * pi, float lengthen = 0) {
        const float grow = shadow ? shade : 0, beyond = shadow ? lengthen / r : 0;
        softArc(out, c, right, up, std::max(r - halfWidth - grow * .4f, 0.f), r + halfWidth + grow, from - beyond,
                to + beyond, shadow ? shadowColor : tint, alpha * opacity * (shadow ? shadowAlpha : 1),
                shadow ? shadowSoft : soft, segments(r), shadow ? shadowSoft * .6f : soft);
    };
    // A dot's shadow is a thin dark edge: a wider one would read as a second ring.
    const auto dot = [&](bool shadow, Vec3 tint, float alpha, float r) {
        softArc(out, c, right, up, 0, r + (shadow ? shade * .5f : 0), 0, 2 * pi, shadow ? shadowColor : tint,
                alpha * opacity * (shadow ? shadowAlpha : 1), shadow ? shadowSoft * .7f : soft, segments(r));
    };
    switch (marker.kind) {
    case AimMark::anchor: {
        const float r = 10.5f * px * (1 - .4f * squeeze);
        for (const bool shadow : {true, false}) {
            arc(shadow, color, 1, r, 1.15f * px);
            dot(shadow, light, 1, 2 * px);
        }
        break;
    }
    case AimMark::air: {
        // Faint: short translucent dashes, their shadows lighter and no
        // longer than they are, so the gaps stay open on a bright sky.
        const float r = 8.5f * px * (1 - .4f * squeeze);
        constexpr int dashes = 8;
        for (const bool shadow : {true, false})
            for (int i = 0; i < dashes; ++i) {
                const float from = 2 * pi * i / dashes;
                arc(shadow, color, shadow ? .7f : .75f, r, .9f * px, from, from + 2 * pi / dashes * .45f);
            }
        break;
    }
    case AimMark::blocked: {
        const Vec3 rising = (right + up) * (5.5f * px * .7071f), falling = (right - up) * (5.5f * px * .7071f);
        for (const bool shadow : {true, false})
            for (const Vec3 arm : {rising, falling})
                softBar(out, c - arm, c + arm, view, .95f * px + (shadow ? shade : 0), shadow ? shadowColor : missColor,
                        opacity * (shadow ? shadowAlpha : .95f), shadow ? shadowSoft : soft);
        break;
    }
    case AimMark::target: {
        // Three arcs around the target, turning, each with a claw pointing in
        // at it; while the ring locks on it closes in from wider. The right
        // hand's ring is a little wider and turned between the left's arcs,
        // so both hands' rings on one target stay apart.
        const float lock = std::isfinite(marker.lock) ? std::clamp(marker.lock, 0.f, 1.f) : 1.f;
        const float closing = lock * lock * (3 - 2 * lock);
        const float size = std::max(std::isfinite(marker.radius) ? marker.radius * 1.15f : 0.f, 13 * px);
        const float r = size * (marker.hand ? 1.15f : 1.f) * (1 - .25f * squeeze) * (1 + .8f * (1 - closing));
        const float spin = (std::isfinite(marker.spin) ? marker.spin : 0.f) + (marker.hand ? pi / 3 : 0.f);
        const float halfWidth = 1.2f * px, claw = std::clamp(r * .2f, 4.5f * px, 10 * px);
        constexpr float span = 75 * pi / 180;
        for (const bool shadow : {true, false}) {
            for (int i = 0; i < 3; ++i) {
                const float middle = pi / 2 + spin + 2 * pi * i / 3;
                arc(shadow, color, 1, r, halfWidth, middle - span / 2, middle + span / 2, shade);
                const Vec3 outward = right * std::cos(middle) + up * std::sin(middle);
                const Vec3 across = right * -std::sin(middle) + up * std::cos(middle);
                const Vec3 base = c + outward * (r - halfWidth);
                softTriangle(out, {base + across * (claw * .55f), base - outward * claw, base - across * (claw * .55f)},
                             view, shadow ? shadowColor : color, opacity * (shadow ? shadowAlpha : 1),
                             shadow ? shade : 0, shadow ? shadowSoft : soft);
            }
            dot(shadow, light, 1, 1.8f * px);
        }
        break;
    }
    }
}
Vec3 onAimLine(Vec3 point, Vec3 normal, Vec3 origin, Vec3 direction) {
    const Vec3 n = normalized(normal);
    const float facing = dot(direction, n);
    if (!finite(point) || !finite(origin) || !finite(direction) || !finite(n) || length(n) < .5f ||
        !(std::abs(facing) >= .2f))
        return point;
    const float along = dot(point - origin, n) / facing;
    const Vec3 crossing = origin + direction * along;
    return along > 0 && finite(crossing) && length(crossing - point) <= .25f * length(point - origin) + .25f
               ? crossing
               : point;
}
void appendSlowMotionMeter(std::vector<Vertex>& out, const SlowMotionView& view, Pose hand, Vec3 viewer,
                           float pixelAngle, float seconds) {
    const float opacity = std::isfinite(view.meter) ? std::clamp(view.meter, 0.f, 1.f) : 0.f;
    if (opacity <= 0 || !finite(hand.position) || !finite(viewer) || !std::isfinite(pixelAngle) || pixelAngle <= 0)
        return;
    // Over the back of the wrist, toward the forearm, like a watch's face.
    const Vec3 c = hand.position + hand.orientation.rotate({0, .045f, .065f});
    const float distance = length(viewer - c);
    if (!finite(c) || !(distance > .08f && distance < 3))
        return;
    // Level and upright across the line of sight, as the aim markers are.
    const Vec3 view3 = (viewer - c) / distance;
    Vec3 right = cross(Vec3{0, 1, 0}, view3);
    right = length(right) > .1f ? normalized(right) : perpendicular(view3);
    const Vec3 up = cross(view3, right);
    const float px = pixelAngle * distance, soft = std::max(.8f * px, .0003f);
    constexpr float radius = .02f, half = .0026f;
    constexpr Vec3 cyan{.3f, .85f, 1.f}, bright{.78f, .97f, 1.f}, red{1.f, .16f, .1f}, glass{.88f, .94f, 1.f};
    const auto unit = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f; };
    const float focus = unit(view.focus), blend = unit(view.blend), warning = unit(view.warning);
    const float t = std::isfinite(seconds) ? seconds : 0.f;
    // In slow motion it glows and breathes; a warning flashes it red.
    const float pulse = view.active ? .5f + .5f * std::sin(t * 6.f) : 0.f;
    Vec3 fill = mix(cyan, bright, blend * (.3f + .3f * pulse));
    fill = mix(fill, red, warning * (.55f + .45f * std::cos(t * 28.f)));
    const Vec3 track = mix(mix(cyan, {0, 0, 0}, .62f), red, warning * .5f);
    // A soft dark shadow keeps it readable on the sky and on a lit wall; the
    // empty track under the fill.
    softArc(out, c, right, up, radius - half - 1.2f * px, radius + half + 1.6f * px, 0, 2 * pi, shadowColor,
            .55f * opacity, 1.8f * px, 72, 1.1f * px);
    softArc(out, c, right, up, radius - half, radius + half, 0, 2 * pi, track, .45f * opacity, soft, 72);
    if (focus > .002f) {
        constexpr float top = pi / 2;
        softArc(out, c, right, up, radius - half, radius + half, top - 2 * pi * focus, top, fill, opacity, soft,
                72);
    }
    // The hourglass: two triangles tip to tip, its shadow first.
    constexpr float width = .0062f, height = .0088f, neck = .0007f;
    const std::array<Vec3, 3> upper{c + (up * height) - right * width, c + (up * height) + right * width,
                                    c + up * neck};
    const std::array<Vec3, 3> lower{c - (up * height) + right * width, c - (up * height) - right * width,
                                    c - up * neck};
    for (const bool shadow : {true, false})
        for (const auto& shape : {upper, lower})
            softTriangle(out, shape, view3, shadow ? shadowColor : mix(glass, fill, .35f),
                         opacity * (shadow ? .55f : .9f), shadow ? 1.2f * px : 0.f, shadow ? 1.6f * px : soft);
}
void appendImpact(std::vector<Vertex>& out, const ImpactView& impact, Vec3 viewer, float pixelAngle) {
    const float distance = length(viewer - impact.point);
    if (!finite(impact.point) || !finite(viewer) || !std::isfinite(pixelAngle) || pixelAngle <= 0 ||
        !std::isfinite(impact.age) || impact.age < 0 || impact.age > ImpactView::seconds ||
        !(distance >= .12f && distance <= 60))
        return;
    // Level and upright across the line of sight, as the aim markers are.
    const Vec3 view = (viewer - impact.point) / distance;
    Vec3 right = cross(Vec3{0, 1, 0}, view);
    right = length(right) > .1f ? normalized(right) : perpendicular(view);
    const Vec3 up = cross(view, right);
    const float strength = std::isfinite(impact.strength) ? std::clamp(impact.strength, 0.f, 1.f) : 0.f;
    const bool finisher = impact.kind == BlowKind::Finisher, heavy = impact.kind != BlowKind::Hit;
    // How far the burst reaches; a jab's is the size of the fist, a
    // finisher's of his chest.
    const float reach = finisher ? .26f : heavy ? .16f : .07f + .08f * strength;
    const int pose = static_cast<int>(impact.age / plate::pose);
    // Each blow's burst is its own drawing, by where it landed.
    const float seed = impact.point.x * 3.1f + impact.point.y * 5.3f + impact.point.z * 7.7f;
    Print print(out, impact.point, right, up, view, pixelAngle * distance);
    // The star: small, then whole; a finisher's a third pose, wider yet. Each
    // pose is drawn anew. A called blow's is out of register. A finisher's
    // star is the smaller share of its reach: an arm's length away it must
    // not fill the view; its lines carry the rest.
    if (pose < (finisher ? 3 : 2)) {
        const float outer = reach * (finisher ? .6f : .75f) * (pose == 0 ? .6f : pose == 1 ? 1.f : 1.15f);
        const int points = finisher ? 13 : heavy ? 11 : 9;
        const float turned = seed + static_cast<float>(pose);
        const Vec3 body = heavy ? plate::red : plate::yellow, heart = heavy ? plate::yellow : plate::paper;
        print.layers(outer * .045f, 0, {}, impact.called ? outer * plate::misprint : 0, [&] {
            print.star({}, outer * .5f, outer, points, turned, body);
            print.star({}, outer * .26f, outer * .56f, points, turned, heart);
        });
    }
    // Then its lines, thrown clear of where the star was: a pose for a jab,
    // two for a harder blow and three for a finisher, each farther out and
    // thinner.
    const int lines = pose - 2;
    if (lines >= 0 && lines < (finisher ? 3 : heavy ? 2 : 1)) {
        const int count = finisher ? 14 : heavy ? 10 : 7;
        const float from = reach * ((finisher ? .8f : .7f) + .4f * static_cast<float>(lines));
        const float width = reach * (.05f - .014f * static_cast<float>(lines));
        print.layers(reach * .018f, 0, {}, 0, [&] {
            for (int i = 0; i < count; ++i) {
                const float angle = seed + 2 * pi * (static_cast<float>(i) + .5f * hash(seed, i + 60)) /
                                               static_cast<float>(count);
                const Flat way{std::cos(angle), std::sin(angle)}, side{-way.y * width, way.x * width};
                const float to = from + reach * (.3f + .35f * hash(seed, i + 80));
                print.triangle(way * from + side, way * to, way * from - side, plate::paper);
            }
        });
    }
    // The count: lettered in over the blow as a sound word is. It lands in
    // three poses, off level; holds; and is gone in two, a pose large and
    // out of register, a pose small.
    if (impact.count >= 2) {
        const int last = static_cast<int>(ImpactView::seconds / plate::pose + .5f);
        Landing land = landing(impact.age);
        if (pose == last - 2)
            land = {1.12f, 0, true};
        else if (pose >= last - 1)
            land.scale = .6f;
        const std::string text = "X" + std::to_string(std::min(impact.count, 999));
        const float height = (finisher ? .06f : .042f) * std::clamp(distance / .7f, .8f, 2.5f);
        const float width = textWidth(text, height) + Print::lean * height;
        print.place(land.scale, land.turn + 8 * plate::degree, {0, reach * .6f + .05f + height * .5f});
        print.layers(height * .06f, height * .1f, finisher ? plate::red : plate::ink,
                     land.hit ? height * plate::misprint : 0,
                     [&] { print.text(text, {width * -.5f, height * -.5f}, height, .2f, plate::yellow); });
    }
}
void appendCue(std::vector<Vertex>& out, const CueView& cue, Vec3 viewer, float pixelAngle) {
    const float distance = length(viewer - cue.point);
    if (!finite(cue.point) || !finite(viewer) || !std::isfinite(pixelAngle) || pixelAngle <= 0 ||
        !std::isfinite(cue.left) || cue.left <= 0 || !std::isfinite(cue.age) || cue.age < 0 ||
        !(distance >= .15f && distance <= 60))
        return;
    // Level and upright across the line of sight, as the bursts are.
    const Vec3 view = (viewer - cue.point) / distance;
    Vec3 right = cross(Vec3{0, 1, 0}, view);
    right = length(right) > .1f ? normalized(right) : perpendicular(view);
    const Vec3 up = cross(view, right);
    const bool called = cue.way != CueWay::None, finisher = called && cue.finisher;
    // The ring's radius: an arm's length away the mark is a hand wide;
    // farther off it keeps most of its size on screen.
    const float size = (finisher ? .13f : .1f) * std::clamp(distance / .9f, 1.f, 5.f);
    const int pose = static_cast<int>(cue.age / plate::pose);
    Print print(out, cue.point, right, up, view, pixelAngle * distance);
    if (!called) {
        // Nothing called: a target and no more. A look's mark is a thin
        // ring; a lock's is bolder, with four ticks in from it, a little off
        // level. Either snaps on, a pose wide and a pose short.
        print.place(pose == 0 ? 1.16f : pose == 1 ? .95f : 1.f, -12 * plate::degree);
        const float half = size * (cue.locked ? .022f : .014f);
        print.layers(size * .022f, 0, {}, 0, [&] {
            print.ring({}, size, half, plate::paper);
            for (int i = 0; cue.locked && i < 4; ++i) {
                const float angle = pi / 4 + pi / 2 * static_cast<float>(i);
                const Flat way{std::cos(angle), std::sin(angle)};
                print.line(way * (size * .72f), way * size, half, plate::paper);
            }
        });
        return;
    }
    const Landing land = landing(cue.age);
    const Vec3 fill = finisher ? plate::yellow : plate::paper, second = finisher ? plate::red : plate::cyan;
    // The ring: what is left of the flow's time, running down clockwise from
    // the top a notch at a time, over a thin ink track that stays whole.
    const float left = std::clamp(cue.left, 0.f, 1.f);
    print.place(land.scale, land.turn);
    if (finisher) {
        // The finisher's is a burst's edge: a zigzag, a stroke to a notch.
        constexpr int strokes = 28;
        const auto corner = [&](int i) {
            const float angle = pi / 2 - 2 * pi * static_cast<float>(i) / strokes;
            return Flat{std::cos(angle), std::sin(angle)} * (size * (i % 2 ? 1.14f : .96f));
        };
        const int shown = static_cast<int>(std::ceil(left * strokes));
        print.layers(size * .03f, 0, {}, 0, [&] {
            for (int i = shown; i < strokes; ++i)
                print.line(corner(i), corner(i + 1), size * .012f, plate::ink, i == shown);
            for (int i = 0; i < shown; ++i)
                print.line(corner(i), corner(i + 1), size * .045f, second, i == 0);
        });
    } else {
        const float turn = 2 * pi * std::ceil(left * 48) / 48;
        print.layers(size * .03f, 0, {}, 0, [&] {
            print.ring({}, size, size * .012f, plate::ink);
            // Angles run from the mark's right toward its top: clockwise
            // from the top is downward from pi / 2.
            print.ring({}, size, size * .05f, second, pi / 2 - turn, pi / 2);
        });
    }
    // The arrow: a marker's stroke with a hard drop, pointing the way the
    // fist must travel. It comes in from outside the ring, on the side the
    // fist comes from, and ends at his middle. It lands out of register, and
    // then boils as a held drawing does: drawn in three slightly different
    // places, two poses each.
    Flat way{0, -1};
    switch (cue.way) {
    case CueWay::Left:
        way = {-1, 0};
        break;
    case CueWay::Right:
        way = {1, 0};
        break;
    case CueWay::Up:
        way = {0, 1};
        break;
    default:
        break;
    }
    const Flat across{-way.y, way.x};
    struct Boil {
        float turn;
        Flat shift;
    };
    static constexpr Boil boils[3] = {{0, {}}, {-.8f, {.015f, .022f}}, {.7f, {-.022f, -.015f}}};
    const Boil& boil = boils[pose < 3 ? 0 : (pose / 2) % 3];
    print.place(land.scale, land.turn + boil.turn * plate::degree, boil.shift * size);
    print.layers(size * .04f, size * .06f, second, land.hit ? size * plate::misprint : 0, [&] {
        const Flat tip = way * (size * .3f), tail = way * (size * -1.3f), neck = way * (size * -.11f);
        print.line(tail, tip, size * .085f, fill);
        print.line(tip, neck + across * (size * .4f), size * .085f, fill, false);
        print.line(tip, neck - across * (size * .4f), size * .085f, fill, false);
    });
}
void appendKnockout(std::vector<Vertex>& out, const KnockoutView& knockout, Vec3 viewer, float pixelAngle) {
    const float distance = length(viewer - knockout.point);
    if (!finite(knockout.point) || !finite(viewer) || !std::isfinite(pixelAngle) || pixelAngle <= 0 ||
        !std::isfinite(knockout.age) || knockout.age < 0 || knockout.age >= KnockoutView::seconds ||
        !(distance >= .15f && distance <= 60))
        return;
    // Level and upright across the line of sight, as the bursts are.
    const Vec3 view = (viewer - knockout.point) / distance;
    Vec3 right = cross(Vec3{0, 1, 0}, view);
    right = length(right) > .1f ? normalized(right) : perpendicular(view);
    const Vec3 up = cross(view, right);
    // It lands as a sound word does, holds three poses, and leaves in two.
    const int pose = static_cast<int>(knockout.age / plate::pose);
    Landing land = landing(knockout.age);
    if (pose == 6)
        land = {1.12f, 0, true};
    else if (pose >= 7)
        land.scale = .6f;
    constexpr std::string_view text = "KO!";
    const float height = .055f * std::clamp(distance / .7f, .8f, 2.5f);
    const float width = textWidth(text, height) + Print::lean * height;
    const float seed = knockout.point.x * 2.3f + knockout.point.y * 4.1f + knockout.point.z * 6.7f;
    Print print(out, knockout.point, right, up, view, pixelAngle * distance);
    print.place(land.scale, land.turn + 6 * plate::degree);
    print.layers(height * .07f, 0, {}, 0, [&] {
        print.star({}, width * .42f, width * .8f, 12, seed, plate::red);
        print.star({}, width * .34f, width * .64f, 12, seed, plate::paper);
    });
    print.layers(height * .06f, height * .1f, plate::red, land.hit ? height * plate::misprint : 0,
                 [&] { print.text(text, {width * -.5f, height * -.5f}, height, .2f, plate::yellow); });
}
namespace {
// What a Spider-Sense warning asks for, as its plate.
Vec3 senseColor(SenseAnswer answer) {
    switch (answer) {
    case SenseAnswer::Dodge:
        return plate::red;
    case SenseAnswer::Jump:
        return plate::yellow;
    default:
        return plate::cyan;
    }
}
// The comic's Spider-Sense lines about the mark's centre, `s` its size: five
// jagged strokes fanned up and out from a point under them, the middle one
// the longest, each kinked twice. `width` scales the strokes; `reach` how far
// out they go.
void senseLines(Print& print, float s, Vec3 color, float width = 1, float reach = 1) {
    static constexpr float angles[5] = {-56, -28, 0, 28, 56};
    const Flat from{0, -.62f * s};
    for (int i = 0; i < 5; ++i) {
        const float a = angles[i] * plate::degree;
        const Flat way{std::sin(a), std::cos(a)}, across{way.y, -way.x};
        const float r0 = .42f * s * reach, r1 = (i == 2 ? 1.18f : .98f) * s * reach, kink = .1f * s;
        Flat p[4];
        for (int k = 0; k < 4; ++k)
            p[k] = from + way * (r0 + (r1 - r0) * static_cast<float>(k) / 3) +
                   across * (k == 1 ? kink : k == 2 ? -kink : 0.f);
        const float half = .062f * s * width;
        print.line(p[0], p[1], half, color);
        print.line(p[1], p[2], half, color, false);
        print.line(p[2], p[3], half, color, false);
    }
}
// The ring that closes on the blow, `left` 1..0 of the warning's time; in its
// last notch a burst's edge, yellow: now.
void senseRing(Print& print, float s, float left) {
    if (left <= 1.f / 8 + 1e-4f) {
        constexpr int strokes = 22;
        const auto corner = [&](int i) {
            const float angle = pi / 2 - 2 * pi * static_cast<float>(i) / strokes;
            return Flat{std::cos(angle), std::sin(angle)} * (s * (i % 2 ? 1.16f : .98f));
        };
        for (int i = 0; i < strokes; ++i)
            print.line(corner(i), corner(i + 1), .05f * s, plate::yellow, i == 0);
        return;
    }
    print.ring({}, s * (1.02f + .55f * left), .035f * s, plate::paper);
}
// The held drawing's boil: three places, two poses each, once it has landed.
struct SenseBoil {
    float turn;
    Flat shift;
};
SenseBoil senseBoil(int pose) {
    static constexpr SenseBoil boils[3] = {{0, {}}, {-1.2f, {.02f, .03f}}, {1, {-.03f, -.02f}}};
    return boils[pose < 3 ? 0 : (pose / 2) % 3];
}
// One Spider-Sense mark: its ring, its lines (a jump's under a chevron), or
// as a counter breaks it.
void senseMark(Print& print, SenseAnswer answer, float s, float left, bool timed, float age, float broken) {
    const Vec3 color = senseColor(answer);
    if (broken >= 0) {
        switch (static_cast<int>(broken / plate::pose)) {
        case 0:
            print.place(1.2f, 0);
            print.layers(.035f * s, 0, {}, plate::misprint * s, [&] { senseLines(print, s, plate::paper); });
            break;
        case 1:
            print.place(1, 0);
            print.layers(.025f * s, 0, {}, 0, [&] { senseLines(print, s, color, .5f, 1.6f); });
            break;
        default:
            print.place(.6f, 0);
            print.layers(.035f * s, 0, {}, 0, [&] { senseLines(print, s, color); });
            break;
        }
        return;
    }
    const Landing land = landing(age);
    // The ring stands still while the lines boil.
    print.place(land.scale, land.turn);
    if (timed)
        print.layers(.03f * s, 0, {}, 0, [&] { senseRing(print, s, std::clamp(left, 0.f, 1.f)); });
    const SenseBoil boil = senseBoil(static_cast<int>(age / plate::pose));
    print.place(land.scale, land.turn + boil.turn * plate::degree, boil.shift * s);
    print.layers(.035f * s, .055f * s, plate::ink, land.hit ? plate::misprint * s : 0, [&] {
        senseLines(print, s, color);
        if (answer == SenseAnswer::Jump) {
            print.line({-.22f * s, .66f * s}, {0, .86f * s}, .06f * s, plate::paper);
            print.line({0, .86f * s}, {.22f * s, .66f * s}, .06f * s, plate::paper, false);
        }
    });
}
bool senseShows(SenseAnswer answer, float age, float left, float broken, float pixelAngle) {
    return answer != SenseAnswer::None && std::isfinite(age) && age >= 0 && std::isfinite(left) &&
           std::isfinite(broken) && broken < SenseView::brokenSeconds && std::isfinite(pixelAngle) &&
           pixelAngle > 0;
}
} // namespace
void appendSense(std::vector<Vertex>& out, const SenseView& sense, Vec3 viewer, float pixelAngle) {
    const float away = length(viewer - sense.point);
    if (!finite(sense.point) || !finite(viewer) || !(away >= .3f && away <= 60) ||
        !senseShows(sense.answer, sense.age, sense.left, sense.broken, pixelAngle))
        return;
    // An arm's length away the mark is a hand wide; farther off it keeps
    // most of its size on screen (six degrees across). It stands clear over
    // his head.
    const float s = .055f * std::clamp(away / .9f, 1.4f, 9.f);
    const Vec3 centre = sense.point + Vec3{0, .14f + .5f * s, 0};
    const float distance = length(viewer - centre);
    if (!(distance >= .15f))
        return;
    // Level and upright across the line of sight, as the bursts are.
    const Vec3 view = (viewer - centre) / distance;
    Vec3 right = cross(Vec3{0, 1, 0}, view);
    right = length(right) > .1f ? normalized(right) : perpendicular(view);
    const Vec3 up = cross(view, right);
    Print print(out, centre, right, up, view, pixelAngle * distance);
    senseMark(print, sense.answer, s, sense.left, sense.timed, sense.age, sense.broken);
}
void appendSenseSight(std::vector<Vertex>& out, const SenseSightView& view, Pose head, float pixelAngle,
                      const SenseSightConfig& config) {
    const float across = std::sqrt(view.sight.x * view.sight.x + view.sight.y * view.sight.y);
    if (!view.sight.shown || !finite(head.position) || !(across > .5f && across < 2) ||
        !senseShows(view.answer, view.age, view.left, view.broken, pixelAngle))
        return;
    const Quat q = head.orientation;
    const Vec3 forward = q.rotate({0, 0, -1}), right = q.rotate({1, 0, 0}), up = q.rotate({0, 1, 0});
    if (!finite(forward) || !finite(right) || !finite(up))
        return;
    const Vec3 way = (right * view.sight.x + up * view.sight.y) / across;
    // A metre off, upright as the view is.
    constexpr float distance = 1, s = .032f;
    const auto at = [&](float angle) {
        return head.position + (forward * std::cos(angle) + way * std::sin(angle)) * distance;
    };
    {
        const Vec3 centre = at(config.angle), facing = normalized(head.position - centre);
        const Vec3 r = normalized(cross(up, facing)), u = cross(facing, r);
        Print print(out, centre, r, u, facing, pixelAngle * distance);
        senseMark(print, view.answer, s, view.left, view.timed, view.age, view.broken);
        if (view.broken < 0) {
            // The arrowhead beyond the ring, pointing to him.
            const float fx = dot(way, r), fy = dot(way, u), f = std::sqrt(fx * fx + fy * fy);
            const Flat ahead{fx / std::max(f, 1e-4f), fy / std::max(f, 1e-4f)}, side{-ahead.y, ahead.x};
            const Landing land = landing(view.age);
            print.place(land.scale, 0);
            print.layers(.035f * s, .055f * s, plate::ink, 0, [&] {
                print.triangle(ahead * (1.95f * s) + side * (.5f * s), ahead * (2.75f * s),
                               ahead * (1.95f * s) - side * (.5f * s), senseColor(view.answer));
            });
        }
    }
    if (view.broken >= 0)
        return;
    // The rim's lines: three, wavy, out toward him, boiling.
    const Vec3 centre = at(config.rim), facing = normalized(head.position - centre);
    const Vec3 radial = normalized(way - facing * dot(way, facing));
    const Vec3 tangent = normalized(cross(radial, facing));
    if (!finite(radial) || !finite(tangent))
        return;
    Print print(out, centre, tangent, radial, facing, pixelAngle * distance);
    const SenseBoil boil = senseBoil(static_cast<int>(view.age / plate::pose));
    const Landing land = landing(view.age);
    print.place(land.scale, boil.turn * plate::degree, boil.shift * .02f);
    const Vec3 color = senseColor(view.answer);
    print.layers(.004f, .006f, plate::ink, 0, [&] {
        for (int i = -1; i <= 1; ++i) {
            const float tilt = static_cast<float>(i) * 14 * plate::degree;
            const Flat dir{std::sin(tilt), std::cos(tilt)}, side{dir.y, -dir.x};
            const Flat base{static_cast<float>(i) * .06f, -.04f};
            Flat before = base;
            for (int k = 1; k <= 6; ++k) {
                const float t = static_cast<float>(k) / 6;
                const Flat p = base + dir * (.085f * t) + side * (.009f * std::sin(t * 3 * pi));
                print.line(before, p, .0055f, color, k == 1);
                before = p;
            }
        }
    });
}
void AimMarkerMotion::reset() {
    *this = {};
}
void AimMarkerMotion::begin(int64_t timeNs) {
    seconds_ = 0;
    if (timeNs <= time_)
        return; // the same image again
    if (time_ && static_cast<double>(timeNs - time_) * 1e-9 > gapSeconds)
        reset();
    else if (time_)
        seconds_ = static_cast<float>(static_cast<double>(timeNs - time_) * 1e-9);
    time_ = timeNs;
}
Vec3 AimMarkerMotion::aim(Vec3 direction, float yaw) {
    if (!std::isfinite(yaw))
        return direction;
    return aim(direction, Quat::yaw(yaw));
}
Vec3 AimMarkerMotion::aim(Vec3 direction, Quat turn) {
    const float n = turn.x * turn.x + turn.y * turn.y + turn.z * turn.z + turn.w * turn.w;
    if (!finite(direction) || length(direction) < .5f || !std::isfinite(n) || std::abs(n - 1) > .01f)
        return direction;
    const Vec3 raw = turn.conjugate().rotate(normalized(direction));
    if (!filtering_) {
        steady_ = raw;
        speed_ = {};
        filtering_ = true;
    } else if (seconds_ > 0) {
        // The share of the way to the raw direction a low-pass at `hz` takes
        // in this image's time; the cutoff rises with the aim's speed.
        const auto share = [dt = seconds_](float hz) { return dt / (dt + 1 / (2 * pi * hz)); };
        speed_ += ((raw - steady_) / seconds_ - speed_) * share(speedCutoffHz);
        const float cutoff = minCutoffHz + speedCutoff * std::max(length(speed_) - speedDeadband, 0.f);
        steady_ = normalized(steady_ + (raw - steady_) * share(cutoff));
    }
    return turn.rotate(steady_);
}
void AimMarkerMotion::markers(const AimMarker* wanted, Vec3 origin, Vec3 direction, std::vector<AimMarker>& out) {
    const bool ray = finite(origin) && finite(direction) && length(direction) > .5f;
    if (wanted && (wanted->kind > AimMark::target || !finite(wanted->point) || (wanted->kind != AimMark::target && !ray)))
        wanted = nullptr;
    direction = normalized(direction);
    const float dt = seconds_;
    // The share of the way to a new value an ease over `seconds` takes now.
    const auto ease = [dt](float seconds) { return 1 - std::exp(-dt / seconds); };
    for (unsigned k = 0; k < kinds_.size(); ++k) {
        auto& shown = kinds_[k];
        const bool target = k == static_cast<unsigned>(AimMark::target);
        if (wanted && static_cast<unsigned>(wanted->kind) == k) {
            const bool fresh = shown.weight <= 0;
            const AimMarker before = shown.marker;
            shown.marker = *wanted;
            if (target && !fresh) {
                // The ring slides to where the target is now, or to the next one.
                shown.marker.point = mix(before.point, wanted->point, ease(targetSeconds));
                shown.marker.radius = before.radius + (wanted->radius - before.radius) * ease(targetSeconds);
            } else if (!target) {
                // A marker on the ray eases to a new distance (its depth).
                const float distance = std::max(length(wanted->point - origin), .01f);
                shown.distance = fresh || !(shown.distance > 0)
                                     ? distance
                                     : std::exp(std::log(shown.distance) +
                                                (std::log(distance) - std::log(shown.distance)) * ease(distanceSeconds));
                shown.marker.point = origin + direction * shown.distance;
            }
            // A new marker shows at once, faintly, and is whole within fadeInSeconds.
            shown.weight = fresh ? .35f : std::min(1.f, shown.weight + dt / fadeInSeconds);
            shown.lock = fresh ? 0.f : std::min(1.f, shown.lock + dt / lockSeconds);
        } else if (shown.weight > 0) {
            shown.weight = std::max(0.f, shown.weight - dt / fadeOutSeconds);
            shown.lock = std::min(shown.lock, shown.weight); // a target's ring widens as it goes
        }
        if (shown.weight <= 0)
            continue;
        // A marker fading out stays where it was last wanted: where the web
        // just went, or where the aim left it.
        AimMarker marker = shown.marker;
        marker.opacity = shown.weight;
        marker.lock = target ? shown.lock : 1.f;
        marker.spin = spin_;
        out.push_back(marker);
    }
    // The target's ring turns, faster as the grip closes.
    const float squeeze = kinds_[static_cast<unsigned>(AimMark::target)].marker.squeeze;
    spin_ = std::fmod(spin_ + dt * (.8f + 2.4f * std::clamp(std::isfinite(squeeze) ? squeeze : 0.f, 0.f, 1.f)),
                      2 * pi);
}
namespace {
// Venom's gold, and its white-hot core (linear RGB).
constexpr Vec3 venomGold{1.f, .62f, .06f}, venomCore{1.f, .93f, .62f};
// The venom target's orange (0.1.53, the user: "an orange venom electro styled icon"), and its hot core.
constexpr Vec3 venomOrange{1.f, .33f, .02f}, venomOrangeCore{1.f, .66f, .28f};
// A bolt from `from` along `way` (unit) for `reach` metres: a jagged line of
// `kinks` pieces, each knocked sideways at random, a faint wide glow under a
// thin bright core.
void venomBolt(std::vector<Vertex>& out, Vec3 from, Vec3 way, float reach, Vec3 view, float px, float alpha,
               float seed, int kinks = 4, Vec3 glow = venomGold, Vec3 core = venomCore) {
    const Vec3 a = perpendicular(way), b = cross(way, a);
    Vec3 at = from;
    for (int k = 1; k <= kinks; ++k) {
        const float u = static_cast<float>(k) / kinks;
        const float jag = k == kinks ? .15f : .35f;
        const Vec3 next = from + way * (reach * u) +
                          (a * (hash(seed, k) - .5f) + b * (hash(seed, k + 40) - .5f)) * (reach * jag);
        const float thin = 1 - .6f * u;
        softBar(out, at, next, view, std::max(3.f * px, .004f) * thin, glow, .35f * alpha, 2 * px);
        softBar(out, at, next, view, std::max(.9f * px, .0012f) * thin, core, alpha, px);
        at = next;
    }
}
// A direction on the sphere, from two 0..1 values.
Vec3 sphere(float u, float v) {
    const float z = 2 * u - 1, r = std::sqrt(std::max(0.f, 1 - z * z)), t = 2 * pi * v;
    return {r * std::cos(t), r * std::sin(t), z};
}
} // namespace
void appendVenomFist(std::vector<Vertex>& out, float glow, Pose hand, Vec3 viewer, float pixelAngle,
                     float seconds) {
    glow = std::isfinite(glow) ? std::clamp(glow, 0.f, 1.f) : 0.f;
    if (glow <= 0 || !finite(hand.position) || !finite(viewer) || !std::isfinite(pixelAngle) || pixelAngle <= 0)
        return;
    // The fist: a little ahead of the glove's middle, toward the knuckles.
    const Vec3 c = hand.position + hand.orientation.rotate({0, -.01f, -.06f});
    const float distance = length(viewer - c);
    if (!finite(c) || !(distance > .08f && distance < 5))
        return;
    const Vec3 view = (viewer - c) / distance;
    Vec3 right = cross(Vec3{0, 1, 0}, view);
    right = length(right) > .1f ? normalized(right) : perpendicular(view);
    const Vec3 up = cross(view, right);
    const float px = pixelAngle * distance;
    const float t = std::isfinite(seconds) ? seconds : 0.f;
    // Charged, it breathes.
    const float breath = glow >= 1 ? .85f + .15f * std::sin(t * 9.f) : glow;
    softArc(out, c, right, up, 0, .055f * (.6f + .4f * glow), 0, 2 * pi, venomGold, .22f * breath, .05f, 32);
    softArc(out, c, right, up, 0, .028f, 0, 2 * pi, venomCore, .28f * breath, .02f, 24);
    // The crackle: new bolts every 1/30 s, more of them as it charges.
    const int frame = static_cast<int>(std::floor(t * 30));
    const int bolts = 2 + static_cast<int>(5 * glow);
    for (int i = 0; i < bolts; ++i) {
        const float seed = static_cast<float>(frame * 7 + i) * .618f;
        const Vec3 way = sphere(hash(seed, 1), hash(seed, 2));
        const float reach = (.04f + .07f * hash(seed, 3)) * (.5f + .5f * glow);
        venomBolt(out, c + way * .03f, way, reach, view, px, breath * (.6f + .4f * hash(seed, 4)), seed, 3);
    }
}
float venomBurstSeconds(const VenomBurstView& burst) {
    const float s = std::isfinite(burst.scale) ? std::clamp(burst.scale, .1f, 10.f) : 1.f;
    return std::min(VenomBurstView::seconds, .6f + .065f * std::max(0.f, s - 1));
}
void appendVenomBurst(std::vector<Vertex>& out, const VenomBurstView& burst, Vec3 viewer, float pixelAngle) {
    const float distance = length(viewer - burst.point);
    const float lasts = venomBurstSeconds(burst);
    if (!finite(burst.point) || !finite(viewer) || !std::isfinite(pixelAngle) || pixelAngle <= 0 ||
        !std::isfinite(burst.age) || burst.age < 0 || burst.age > lasts || !(distance >= .12f && distance <= 60))
        return;
    const Vec3 view = (viewer - burst.point) / distance;
    Vec3 right = cross(Vec3{0, 1, 0}, view);
    right = length(right) > .1f ? normalized(right) : perpendicular(view);
    const Vec3 up = cross(view, right);
    const float px = pixelAngle * distance;
    const float u = burst.age / lasts, fade = 1 - u * u;
    const float s = std::isfinite(burst.scale) ? std::clamp(burst.scale, .1f, 10.f) : 1.f;
    // MilesVR 0.1.60 (the user: the venom punch "more brighter and like a
    // bigger explosion that is acceptable for a forceful punch"): a bigger,
    // white-hot flash that holds a quarter of a second, an orange shockwave
    // ring blasting out from it, more and longer bolts, and an afterglow.
    const float flash = std::max(0.f, 1 - burst.age / .25f);
    if (flash > 0) {
        // The smash's and the Mega's (4 and 8 times) would white out the view
        // round the player: their flash grows no more than 2.5 times (the
        // Mega's 3.5).
        const float f = std::min(s, s >= 6 ? 2.5f : 1.8f) * (1 + .35f * (1 - flash)); // it swells as it goes
        softArc(out, burst.point, right, up, 0, .75f * f, 0, 2 * pi, venomOrange, .5f * flash, .45f * f, 48);
        softArc(out, burst.point, right, up, 0, .42f * f, 0, 2 * pi, venomGold, .75f * flash, .25f * f, 40);
        softArc(out, burst.point, right, up, 0, .2f * f, 0, 2 * pi, venomCore, flash, .1f * f, 32);
    }
    // The shockwave: a ring racing out to 1.5 m (times the scale) in a third
    // of a second, thinning as it goes.
    const float wave = std::min(1.f, burst.age / .33f);
    if (wave < 1) {
        const float r = 1.5f * s * std::sqrt(wave), width = std::max(.05f * s * (1 - wave), 2.f * px);
        softArc(out, burst.point, right, up, std::max(0.f, r - width * 2.5f), r, 0, 2 * pi, venomOrange,
                .6f * (1 - wave), width, 64);
        softArc(out, burst.point, right, up, std::max(0.f, r - width), r, 0, 2 * pi, venomCore, .85f * (1 - wave),
                std::max(px, width * .4f), 64);
    }
    // The afterglow, fading over the whole burst.
    softArc(out, burst.point, right, up, 0, .3f * s, 0, 2 * pi, venomOrange, .25f * fade * (1 - flash), .2f * s, 32);
    // On the ground (the smash's and the Mega's, MilesVR 0.1.61, the user: "a
    // bigger more brighter effect on the ground ... so that it looks like a
    // big explosion"): a white-hot flash where he lands, three shockwave rings
    // racing out one after another to the blast's edge, and electric cracks
    // splitting along the ground.
    if (std::isfinite(burst.ring) && burst.ring > 0) {
        const float R = burst.ring;
        const float lift = std::isfinite(burst.lift) ? std::clamp(burst.lift, 0.f, 3.f) : 0.f;
        const Vec3 ground = burst.point + Vec3{0, .05f - lift, 0};
        const Vec3 east{1, 0, 0}, north{0, 0, 1};
        const float land = std::max(0.f, 1 - burst.age / .35f);
        if (land > 0) {
            softArc(out, ground, east, north, 0, R * (.25f + .3f * (1 - land)), 0, 2 * pi, venomOrange, .55f * land,
                    R * .2f, 64);
            softArc(out, ground, east, north, 0, R * (.1f + .12f * (1 - land)), 0, 2 * pi, venomCore, .9f * land,
                    R * .08f, 48);
        }
        for (int k = 0; k < 3; ++k) {
            const float start = .09f * k, span = lasts * .7f;
            const float w = (burst.age - start) / span;
            if (w <= 0 || w >= 1)
                continue;
            const float r = R * std::sqrt(w), alpha = (1 - w) * (k ? .7f : 1.f);
            const float width = std::max(.06f * R * (1 - w), 3 * px);
            softArc(out, ground, east, north, std::max(0.f, r - width * 3), r, 0, 2 * pi, venomOrange, .7f * alpha,
                    width * 1.5f, 96);
            softArc(out, ground, east, north, std::max(0.f, r - width), r, 0, 2 * pi, venomCore, alpha,
                    std::max(px, width * .5f), 96);
        }
        // The cracks: bolts along the ground out to most of the edge, new ones
        // a few times a second.
        const int crackFrame = static_cast<int>(burst.age * 12);
        const float crackSeed = burst.point.x * 1.7f + burst.point.z * 2.9f;
        const int cracks = R > 7 ? 16 : 12;
        for (int i = 0; i < cracks; ++i) {
            const float seed = crackSeed + static_cast<float>(crackFrame * 17 + i) * .618f;
            const float angle = 2 * pi * (static_cast<float>(i) + hash(seed, 1)) / static_cast<float>(cracks);
            const Vec3 way{std::cos(angle), 0, std::sin(angle)};
            const float reach = R * (.35f + .5f * std::sqrt(u)) * (.7f + .3f * hash(seed, 2));
            venomBolt(out, ground + way * (.1f * R), way, reach, view, px, std::min(1.f, 1.4f * fade), seed, 6,
                      i % 2 ? venomOrange : venomGold);
        }
        // The Mega Venom Blast: a pillar of light up from where he stands and
        // bolts arcing up out of it.
        if (R > 7) {
            const float rise = std::min(1.f, burst.age / .2f);
            const float top = 9 * rise;
            const Vec3 base = ground, peak = ground + Vec3{0, top, 0};
            softBar(out, base, peak, view, R * .14f, venomOrange, .45f * fade, R * .1f);
            softBar(out, base, peak, view, R * .05f, venomGold, .7f * fade, R * .03f);
            softBar(out, base, peak, view, R * .015f, venomCore, .9f * fade, px * 2);
            for (int i = 0; i < 8; ++i) {
                const float seed = crackSeed + static_cast<float>(crackFrame * 7 + i) * 1.3f;
                const float angle = 2 * pi * hash(seed, 4);
                const Vec3 way = normalized(Vec3{std::cos(angle) * .5f, 1, std::sin(angle) * .5f});
                venomBolt(out, ground + Vec3{0, top * hash(seed, 5) * .5f, 0}, way, 3 + 3 * hash(seed, 6), view, px,
                          fade, seed, 6, venomOrange);
            }
        }
    }
    // Bolts thrown out, longer as it goes, new ones every 1/30 s.
    const int frame = static_cast<int>(burst.age * 30);
    const float seedBase = burst.point.x * 3.1f + burst.point.y * 5.3f + burst.point.z * 7.7f;
    constexpr int bolts = 16;
    for (int i = 0; i < bolts; ++i) {
        const float seed = seedBase + static_cast<float>(frame * 13 + i) * .618f;
        const Vec3 way = sphere(hash(seed, 1), hash(seed, 2));
        // In the air no longer than 2.5 punches' (the ground's cracks carry the big ones out).
        const float reach = (.35f + .75f * u) * (.6f + .4f * hash(seed, 3)) * std::min(s, 2.5f);
        venomBolt(out, burst.point + way * .08f * s, way, reach, view, px, std::min(1.f, 1.3f * fade), seed, 5,
                  i % 3 ? venomGold : venomOrange);
    }
}
void appendVenomTarget(std::vector<Vertex>& out, const VenomTargetView& target, Vec3 viewer, float pixelAngle) {
    const float strength = std::isfinite(target.strength) ? std::clamp(target.strength, 0.f, 1.f) : 0.f;
    // Over his head: the cue's point is his chest.
    const Vec3 c = target.point + Vec3{0, .75f, 0};
    const float distance = length(viewer - c);
    if (strength <= .02f || !finite(target.point) || !finite(viewer) || !std::isfinite(pixelAngle) ||
        pixelAngle <= 0 || !(distance >= .3f && distance <= 60))
        return;
    const Vec3 view = (viewer - c) / distance;
    Vec3 right = cross(Vec3{0, 1, 0}, view);
    right = length(right) > .1f ? normalized(right) : perpendicular(view);
    const Vec3 up = cross(view, right);
    const float px = pixelAngle * distance;
    // A readable size: 0.16 m, and never under 22 pixels across its radius.
    const float r = std::max(.16f, 22 * px);
    const float t = std::isfinite(target.seconds) ? target.seconds : 0.f;
    const float a = strength * (.85f + .15f * std::sin(t * 8));
    // The glow, the ring and its white-hot edge.
    softArc(out, c, right, up, 0, r * 1.35f, 0, 2 * pi, venomOrange, .22f * a, r * .6f, 40);
    softArc(out, c, right, up, r * .8f, r, 0, 2 * pi, venomOrange, .85f * a, 1.5f * px, 48);
    softArc(out, c, right, up, r * .88f, r * .93f, 0, 2 * pi, venomOrangeCore, .9f * a, px, 48);
    // The lightning bolt in it: a jagged stroke, wide gold under a bright core.
    const auto at = [&](float x, float y) { return c + right * (x * r) + up * (y * r); };
    const Vec3 glyph[] = {at(.16f, .58f), at(-.14f, .04f), at(.12f, .04f), at(-.16f, -.58f)};
    for (int k = 0; k + 1 < 4; ++k) {
        softBar(out, glyph[k], glyph[k + 1], view, r * .11f, venomOrange, .95f * a, 1.5f * px);
        softBar(out, glyph[k], glyph[k + 1], view, r * .045f, venomOrangeCore, a, px);
    }
    // Bolts crackling off the ring, new ones twenty times a second.
    const int frame = static_cast<int>(t * 20);
    const float seedBase = target.point.x * 3.1f + target.point.z * 7.7f;
    constexpr int bolts = 6;
    for (int i = 0; i < bolts; ++i) {
        const float seed = seedBase + static_cast<float>(frame * 11 + i) * .618f;
        const float angle = 2 * pi * (static_cast<float>(i) + hash(seed, 1)) / bolts;
        const Vec3 way = right * std::cos(angle) + up * std::sin(angle);
        venomBolt(out, c + way * r, way, r * (.35f + .45f * hash(seed, 2)), view, px, .8f * a, seed, 3, venomOrange,
                  venomOrangeCore);
    }
}
void appendStealthGlow(std::vector<Vertex>& out, const StealthGlowView& glow, Vec3 viewer, float pixelAngle) {
    const Vec3 c = glow.point;
    const float distance = length(viewer - c);
    if (!finite(c) || !finite(viewer) || !std::isfinite(pixelAngle) || pixelAngle <= 0 ||
        !(distance >= .05f && distance <= 5))
        return;
    constexpr Vec3 blue{.35f, .78f, 1.f};
    const Vec3 view = (viewer - c) / distance;
    Vec3 right = cross(Vec3{0, 1, 0}, view);
    right = length(right) > .1f ? normalized(right) : perpendicular(view);
    const Vec3 up = cross(view, right);
    const float t = std::isfinite(glow.seconds) ? glow.seconds : 0.f;
    const float a = .7f + .3f * std::sin(t * 3);
    softArc(out, c, right, up, 0, .09f, 0, 2 * pi, blue, .2f * a, .06f, 32);
    softArc(out, c, right, up, 0, .035f, 0, 2 * pi, blue, .18f * a, .03f, 24);
}
} // namespace spidy

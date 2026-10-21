#pragma once
#include "vertex.hpp"
#include <array>
#include <cmath>
#include <vector>

// Flat shapes for the overlay's marks, both styles' (web_visual.cpp,
// tech_marks.cpp): at `alpha` inside, fading to nothing across `soft` metres
// at every edge, so their outlines stay smooth without multisampling while
// they move by fractions of a pixel.
namespace spidy::soft {
inline void quad(std::vector<Vertex>& out, const Vertex& a, const Vertex& b, const Vertex& c,
                 const Vertex& d) {
    out.insert(out.end(), {a, b, c, a, c, d});
}
inline Vec3 perpendicular(Vec3 d) {
    return normalized(cross(d, std::abs(d.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
}
// An arc of a flat ring around c in the plane of unit axes a and b, from
// angle t0 to t1 (a whole ring when they span 2 pi) in `segments` pieces a
// whole turn, between radii inner and outer (0: a disc); its inner edge
// fades across `innerSoft` instead when that is given.
inline void softArc(std::vector<Vertex>& out, Vec3 c, Vec3 a, Vec3 b, float inner, float outer, float t0,
                    float t1, Vec3 color, float alpha, float soft, int segments, float innerSoft = -1) {
    constexpr float pi = 3.14159265f;
    const bool whole = t1 - t0 > 2 * pi - 1e-3f;
    const int pieces = std::max(2, static_cast<int>(std::ceil((t1 - t0) / (2 * pi) * segments)));
    const float radii[4] = {std::max(inner - (innerSoft >= 0 ? innerSoft : soft), 0.f), inner, outer,
                            outer + soft};
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
            quad(out, at(end, k, alphas[k]), at(beyond, k, 0), at(beyond, k + 1, 0),
                 at(end, k + 1, alphas[k + 1]));
    }
}
// A flat bar from p to q across the line of sight `view`, its ends squared
// off half its width beyond them so two bars close a corner.
inline void softBar(std::vector<Vertex>& out, Vec3 p, Vec3 q, Vec3 view, float halfWidth, Vec3 color,
                    float alpha, float soft) {
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
inline void softTriangle(std::vector<Vertex>& out, std::array<Vec3, 3> p, Vec3 view, Vec3 color, float alpha,
                         float grow, float soft) {
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
        quad(out, {inside[i], color, alpha}, {inside[j], color, alpha}, {edge[j], color, 0},
             {edge[i], color, 0});
    }
}
} // namespace spidy::soft

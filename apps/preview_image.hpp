#pragma once
// The previews' drawing (spidy_mark_preview, spidy_phone_preview): an image in
// linear RGB, as the overlay blends, read from and written to 24-bit BMP
// files, and the overlay's own triangles drawn into it from an eye at the
// origin, a sample a pixel with their blending.
#include "spidy/vertex.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace spidy::preview {
inline float decode(float c) {
    return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
}
inline float encode(float c) {
    c = std::clamp(c, 0.f, 1.f);
    return c <= .0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - .055f;
}
struct Image {
    int w{}, h{};
    std::vector<float> rgb; // linear, as the overlay blends
    Image() = default;
    Image(int width, int height) : w(width), h(height), rgb(static_cast<size_t>(width) * height * 3, 0.f) {}
    float* at(int x, int y) {
        return &rgb[(static_cast<size_t>(y) * w + x) * 3];
    }
    const float* at(int x, int y) const {
        return &rgb[(static_cast<size_t>(y) * w + x) * 3];
    }
};
inline bool load(const std::string& path, Image& image) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)), {});
    const auto word = [&](size_t o) {
        return static_cast<uint32_t>(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | static_cast<uint32_t>(d[o + 3]) << 24);
    };
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M' || (d[28] != 24 && d[28] != 32) || word(30))
        return false;
    const int w = static_cast<int>(word(18)), h = static_cast<int>(word(22)), bytes = d[28] / 8;
    const size_t offset = word(10), stride = (static_cast<size_t>(w) * bytes + 3) & ~size_t{3};
    if (w <= 0 || !h || offset + stride * static_cast<size_t>(std::abs(h)) > d.size())
        return false;
    image = Image(w, std::abs(h));
    for (int y = 0; y < image.h; ++y) {
        const unsigned char* row = &d[offset + stride * (h > 0 ? image.h - 1 - y : y)];
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = row + x * bytes;
            float* o = image.at(x, y);
            o[0] = decode(p[2] / 255.f);
            o[1] = decode(p[1] / 255.f);
            o[2] = decode(p[0] / 255.f);
        }
    }
    return true;
}
inline bool save(const Image& image, const std::string& path) {
    const size_t stride = (static_cast<size_t>(image.w) * 3 + 3) & ~size_t{3};
    std::vector<unsigned char> d(54 + stride * image.h, 0);
    const auto put = [&](size_t o, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            d[o + i] = static_cast<unsigned char>(v >> (8 * i));
    };
    d[0] = 'B';
    d[1] = 'M';
    put(2, static_cast<uint32_t>(d.size()));
    put(10, 54);
    put(14, 40);
    put(18, static_cast<uint32_t>(image.w));
    put(22, static_cast<uint32_t>(image.h));
    d[26] = 1;
    d[28] = 24;
    put(34, static_cast<uint32_t>(stride * image.h));
    for (int y = 0; y < image.h; ++y) {
        unsigned char* row = &d[54 + stride * (image.h - 1 - y)];
        for (int x = 0; x < image.w; ++x) {
            const float* p = image.at(x, y);
            for (int k = 0; k < 3; ++k)
                row[x * 3 + k] = static_cast<unsigned char>(std::lround(encode(p[2 - k]) * 255));
        }
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
    std::printf("%s %dx%d\n", path.c_str(), image.w, image.h);
    return file.good();
}
// The eye: at the origin, looking along -z, `f` pixels a unit of tangent
// about (cx, cy).
struct Camera {
    float f, cx, cy;
    Vec3 ray(float u, float v) const {
        return normalized({(u - cx) / f, -(v - cy) / f, -1});
    }
};
// Draws the vertices into the cell (x0, y0, w, h) of `image`, camera pixel
// (u0, v0) at the cell's corner: a sample at each pixel's centre, blended by
// the vertices' opacity as the overlay's translucent pass blends.
inline void draw(Image& image, const std::vector<Vertex>& vertices, const Camera& camera, float u0, float v0,
                 int x0, int y0, int w, int h) {
    for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
        float x[3], y[3];
        bool ahead = true;
        for (int k = 0; k < 3 && ahead; ++k) {
            const Vec3 p = vertices[i + k].position;
            ahead = p.z < -.01f;
            x[k] = camera.cx + camera.f * p.x / -p.z - u0 + static_cast<float>(x0);
            y[k] = camera.cy - camera.f * p.y / -p.z - v0 + static_cast<float>(y0);
        }
        const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if (!ahead || std::abs(area) < 1e-9f)
            continue;
        const int left = std::max(x0, static_cast<int>(std::floor(std::min({x[0], x[1], x[2]}))));
        const int right = std::min(x0 + w - 1, static_cast<int>(std::ceil(std::max({x[0], x[1], x[2]}))));
        const int top = std::max(y0, static_cast<int>(std::floor(std::min({y[0], y[1], y[2]}))));
        const int bottom = std::min(y0 + h - 1, static_cast<int>(std::ceil(std::max({y[0], y[1], y[2]}))));
        for (int py = top; py <= bottom; ++py)
            for (int px = left; px <= right; ++px) {
                // A centre on an edge two triangles share belongs to one of them, as on the GPU: the sample
                // sits a hair off the centre, so no edge runs through it, and a translucent mark's seams are
                // not blended twice.
                const float sx = static_cast<float>(px) + .5f + 1.7e-4f;
                const float sy = static_cast<float>(py) + .5f + 3.1e-4f;
                const float b0 = ((x[1] - sx) * (y[2] - sy) - (x[2] - sx) * (y[1] - sy)) / area;
                const float b1 = ((x[2] - sx) * (y[0] - sy) - (x[0] - sx) * (y[2] - sy)) / area;
                const float b2 = 1 - b0 - b1;
                if (b0 < 0 || b1 < 0 || b2 < 0)
                    continue;
                const float a = std::clamp(
                    vertices[i].alpha * b0 + vertices[i + 1].alpha * b1 + vertices[i + 2].alpha * b2, 0.f, 1.f);
                const Vec3 c = vertices[i].color * b0 + vertices[i + 1].color * b1 + vertices[i + 2].color * b2;
                float* o = image.at(px, py);
                o[0] = c.x * a + o[0] * (1 - a);
                o[1] = c.y * a + o[1] * (1 - a);
                o[2] = c.z * a + o[2] * (1 - a);
            }
    }
}
} // namespace spidy::preview

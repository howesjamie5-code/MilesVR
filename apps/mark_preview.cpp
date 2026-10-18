// Draws the combat marks (web_visual.hpp's appendCue, appendImpact and
// appendKnockout) over a
// headset's eye captures, without a headset: the overlay's own vertices, one
// sample a pixel with its blending, at about the headset's pixel density.
// It writes sheets of the marks' states and poses as BMP files.
// tools/preview-marks.ps1 feeds it captures and turns the sheets into PNG.
//
//   spidy_mark_preview <out folder> <scale> <far.bmp|-> <u> <v> <metres> <near.bmp|-> <u> <v> <metres>
//
// Each backdrop is a 24-bit BMP of an eye capture enlarged `scale` times
// (the captures are 512 pixels wide, about 235 pixels a unit of tangent),
// with the capture pixel (u, v) of the enemy's chest and how far off he is;
// "-" for a plain backdrop, dark to the left and pale to the right.
#include "spidy/overlay_text.hpp"
#include "spidy/web_visual.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace spidy;

namespace {
float decode(float c) {
    return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
}
float encode(float c) {
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
Image plain(int w, int h) {
    Image image(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool pale = x >= w / 2;
            float* p = image.at(x, y);
            p[0] = pale ? .55f : .012f;
            p[1] = pale ? .5f : .012f;
            p[2] = pale ? .42f : .02f;
        }
    return image;
}
bool load(const std::string& path, Image& image) {
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
bool save(const Image& image, const std::string& path) {
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
void draw(Image& image, const std::vector<Vertex>& vertices, const Camera& camera, float u0, float v0, int x0,
          int y0, int w, int h) {
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
                const float sx = static_cast<float>(px) + .5f, sy = static_cast<float>(py) + .5f;
                const float b0 = ((x[1] - sx) * (y[2] - sy) - (x[2] - sx) * (y[1] - sy)) / area;
                const float b1 = ((x[2] - sx) * (y[0] - sy) - (x[0] - sx) * (y[2] - sy)) / area;
                const float b2 = 1 - b0 - b1;
                // (A centre on an edge two triangles share belongs to one of them, as on the GPU.)
                if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f)
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
void label(Image& image, const std::string& text, int x, int y) {
    constexpr float height = 15;
    const Camera flat{1, 0, 0};
    for (const bool shadow : {true, false}) {
        TextStyle style;
        style.height = height;
        style.weight = shadow ? .4f : .14f;
        style.color = shadow ? Vec3{0, 0, 0} : Vec3{1, .9f, .2f};
        std::vector<Vertex> v;
        appendText(v, text, {static_cast<float>(x), -static_cast<float>(y) - height, -1}, {1, 0, 0}, {0, 1, 0}, style);
        draw(image, v, flat, 0, 0, 0, 0, image.w, image.h);
    }
}
struct Cell {
    std::string name;
    std::vector<Vertex> vertices;
};
// A sheet of cells, each a crop of `backdrop` `side` pixels square about
// camera pixel (u, v), with its marks and its name.
bool sheet(const std::string& path, const Image& backdrop, const Camera& camera, float u, float v, int side,
           int columns, const std::vector<Cell>& cells) {
    constexpr int gap = 6;
    const int rows = (static_cast<int>(cells.size()) + columns - 1) / columns;
    Image image(columns * (side + gap) + gap, rows * (side + gap) + gap);
    const int u0 = static_cast<int>(u) - side / 2, v0 = static_cast<int>(v) - side / 2;
    for (size_t i = 0; i < cells.size(); ++i) {
        const int x0 = gap + static_cast<int>(i % columns) * (side + gap);
        const int y0 = gap + static_cast<int>(i / columns) * (side + gap);
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x) {
                const int su = u0 + x, sv = v0 + y;
                const bool inside = su >= 0 && sv >= 0 && su < backdrop.w && sv < backdrop.h;
                for (int k = 0; k < 3; ++k)
                    image.at(x0 + x, y0 + y)[k] = inside ? backdrop.at(su, sv)[k] : .01f;
            }
        draw(image, cells[i].vertices, camera, static_cast<float>(u0), static_cast<float>(v0), x0, y0, side, side);
        label(image, cells[i].name, x0 + 6, y0 + 6);
    }
    return save(image, path);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 11) {
        std::printf("spidy_mark_preview <out folder> <scale> <far.bmp|-> <u> <v> <metres> <near.bmp|-> <u> <v> <metres>\n");
        return 1;
    }
    const std::string out = argv[1];
    const float scale = std::clamp(static_cast<float>(std::atof(argv[2])), 1.f, 8.f);
    struct Scene {
        Image backdrop;
        float u{}, v{}, metres{};
    } scenes[2];
    for (int i = 0; i < 2; ++i) {
        auto& scene = scenes[i];
        const std::string path = argv[3 + i * 4];
        if (path == "-")
            scene.backdrop = plain(static_cast<int>(512 * scale), static_cast<int>(544 * scale));
        else if (!load(path, scene.backdrop)) {
            std::printf("%s is not a 24-bit BMP\n", path.c_str());
            return 2;
        }
        scene.u = static_cast<float>(std::atof(argv[4 + i * 4])) * scale;
        scene.v = static_cast<float>(std::atof(argv[5 + i * 4])) * scale;
        scene.metres = std::clamp(static_cast<float>(std::atof(argv[6 + i * 4])), .2f, 50.f);
    }
    const Vec3 viewer{};
    constexpr float pose = 1.f / 12;
    const auto at = [&](int i) { return pose * (static_cast<float>(i) + .5f); };
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        const auto& scene = scenes[i];
        const Camera camera{235 * scale, static_cast<float>(scene.backdrop.w) / 2, static_cast<float>(scene.backdrop.h) / 2};
        const float pixelAngle = 1 / camera.f;
        const Vec3 p = camera.ray(scene.u, scene.v) * scene.metres;
        const auto cue = [&](CueWay way, bool finisher, float left, float age, bool locked = false) {
            std::vector<Vertex> v;
            appendCue(v, {p, way, finisher, left, age, locked}, viewer, pixelAngle);
            return v;
        };
        const auto knockout = [&](float age) {
            std::vector<Vertex> v;
            appendKnockout(v, {p, age}, viewer, pixelAngle);
            return v;
        };
        const auto impact = [&](BlowKind kind, float strength, int count, float age, bool called = false) {
            std::vector<Vertex> v;
            appendImpact(v, {p, strength, kind, count, age, called}, viewer, pixelAngle);
            return v;
        };
        const std::string name = out + (i ? "/near-" : "/far-");
        // A cell holds the mark with room around it at either distance.
        const int side = static_cast<int>((i ? 150 : 120) * scale);
        ok &= sheet(name + "marks.bmp", scene.backdrop, camera, scene.u, scene.v, side, 5,
                    {{"LOOKED AT", cue(CueWay::None, false, 1, 1)},
                     {"LOCKED", cue(CueWay::None, false, 1, 1, true)},
                     {"CALL LEFT", cue(CueWay::Left, false, .85f, 1, true)},
                     {"CALL UP 50%", cue(CueWay::Up, false, .5f, 1.17f, true)},
                     {"FINISHER RIGHT", cue(CueWay::Right, true, .9f, 1, true)},
                     {"LOCKED POSE 1", cue(CueWay::None, false, 1, at(0), true)},
                     {"CALL DOWN 20%", cue(CueWay::Down, false, .2f, 1.34f, true)},
                     {"CALL POSE 1", cue(CueWay::Right, false, 1, at(0), true)},
                     {"CALL POSE 2", cue(CueWay::Right, false, 1, at(1), true)},
                     {"FINISHER POSE 2", cue(CueWay::Up, true, 1, at(1), true)}});
        std::vector<Cell> blows;
        for (int k = 0; k < 5; ++k)
            blows.push_back({"JAB X2 POSE " + std::to_string(k + 1), impact(BlowKind::Hit, .5f, 2, at(k))});
        for (int k = 0; k < 5; ++k)
            blows.push_back({"CALLED X3 POSE " + std::to_string(k + 1), impact(BlowKind::Heavy, 1, 3, at(k), true)});
        for (int k = 0; k < 5; ++k)
            blows.push_back({"FINISHER X5 POSE " + std::to_string(k + 1), impact(BlowKind::Finisher, 1, 5, at(k))});
        blows.push_back({"FIRST JAB POSE 2", impact(BlowKind::Hit, .2f, 1, at(1))});
        blows.push_back({"HEAVY POSE 2", impact(BlowKind::Heavy, 1, 1, at(1))});
        blows.push_back({"COUNT HELD", impact(BlowKind::Hit, .5f, 4, at(5))});
        blows.push_back({"COUNT LEAVING 1", impact(BlowKind::Hit, .5f, 4, at(7))});
        blows.push_back({"COUNT LEAVING 2", impact(BlowKind::Hit, .5f, 4, at(8))});
        blows.push_back({"KNOCKOUT POSE 1", knockout(at(0))});
        blows.push_back({"KNOCKOUT POSE 2", knockout(at(1))});
        blows.push_back({"KNOCKOUT HELD", knockout(at(4))});
        blows.push_back({"KNOCKOUT LEAVING 1", knockout(at(6))});
        blows.push_back({"KNOCKOUT LEAVING 2", knockout(at(7))});
        ok &= sheet(name + "blows.bmp", scene.backdrop, camera, scene.u, scene.v, side, 5, blows);
        // Miles's venom: round the right fist (half a metre off, as a fist
        // held up is) as it charges and once charged, and a venom punch's
        // burst where it landed.
        const auto fist = [&](float glow, float seconds) {
            std::vector<Vertex> v;
            const Vec3 knuckles = normalized(p) * .5f;
            appendVenomFist(v, glow, {knuckles - Vec3{0, -.01f, -.06f}, {}}, viewer, pixelAngle, seconds);
            return v;
        };
        const auto venomBurst = [&](float age) {
            std::vector<Vertex> v;
            appendVenomBurst(v, {p, age}, viewer, pixelAngle);
            return v;
        };
        ok &= sheet(name + "venom.bmp", scene.backdrop, camera, scene.u, scene.v, side, 5,
                    {{"FIST CHARGING 30%", fist(.3f, 1)},
                     {"FIST CHARGING 70%", fist(.7f, 1.2f)},
                     {"FIST CHARGED", fist(1, 1.4f)},
                     {"FIST CHARGED LATER", fist(1, 1.55f)},
                     {"FIST FADING", fist(.4f, 1.7f)},
                     {"VENOM BURST 0.03 S", venomBurst(.03f)},
                     {"VENOM BURST 0.1 S", venomBurst(.1f)},
                     {"VENOM BURST 0.2 S", venomBurst(.2f)},
                     {"VENOM BURST 0.35 S", venomBurst(.35f)},
                     {"VENOM BURST 0.45 S", venomBurst(.45f)}});
        // The whole eye image with a call on him.
        Image whole = scene.backdrop;
        draw(whole, cue(CueWay::Left, false, .7f, 1), camera, 0, 0, 0, 0, whole.w, whole.h);
        ok &= save(whole, name + "eye.bmp");
    }
    return ok ? 0 : 3;
}

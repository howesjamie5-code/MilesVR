// The wrist menu (wrist_hud.hpp), from the SM2 port of SpidyVR (GPLv3; SpidyVR
// by Ilya Mezerowsky), adapted to SM2VR.
#include "spidy/wrist_hud.hpp"
#include "spidy/overlay_text.hpp"
#include "spidy/soft_shapes.hpp"
#include "spidy/tracking.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace spidy::wrist_hud {
namespace {
constexpr float pi = 3.14159265f;

// Draws in the panel's own measures: u to its right and v up from its middle,
// metres at scale 1, in the theme's colours (c), v shifted up by dv (a page's
// scroll) for what scrolls.
struct Painter {
    std::vector<Vertex>& out;
    Frame f;
    float s, soft;
    Vec3 normal;
    const Palette& c;
    float dv = 0;
    Vec3 at(float u, float v) const {
        return f.centre + f.right * (u * s) + f.up * ((v + dv) * s);
    }
    void bar(float u0, float v0, float u1, float v1, float width, Vec3 color, float alpha) const {
        soft::softBar(out, at(u0, v0), at(u1, v1), normal, width * s / 2, color, alpha, soft);
    }
    void rect(float u0, float v0, float u1, float v1, Vec3 color, float alpha) const {
        soft::quad(out, {at(u0, v0), color, alpha}, {at(u1, v0), color, alpha}, {at(u1, v1), color, alpha},
                   {at(u0, v1), color, alpha});
    }
    // A ring (inner 0: a disc), squeezed to rx across by ry up.
    void ring(float u, float v, float rx, float ry, float inner, float outer, Vec3 color, float alpha,
              int segments = 48) const {
        soft::softArc(out, at(u, v), f.right * (rx / ry), f.up, inner * s, outer * s, 0, 2 * pi, color, alpha,
                      soft, segments);
    }
    void triangle(float u0, float v0, float u1, float v1, float u2, float v2, Vec3 color, float alpha) const {
        soft::softTriangle(out, {at(u0, v0), at(u1, v1), at(u2, v2)}, normal, color, alpha, 0, soft);
    }
    void text(const std::string& t, float u, float v, float height, Vec3 color) const {
        appendText(out, t, at(u, v), f.right, f.up, TextStyle{height * s, .15f, color, false, soft});
    }
};

// The panel's outline: 18 by 22 cm, its corners cut.
constexpr float halfW = panelWidth / 2, halfH = panelHeight / 2, cut = .016f;
std::array<std::array<float, 2>, 8> outlineOf(float top) {
    return {{{-halfW + cut, top},
             {halfW - cut, top},
             {halfW, top - cut},
             {halfW, -halfH + cut},
             {halfW - cut, -halfH},
             {-halfW + cut, -halfH},
             {-halfW, -halfH + cut},
             {-halfW, top - cut}}};
}

void backing(const Painter& p, float top) {
    const auto outline = outlineOf(top);
    // Dark glass, so the lines read over a bright sky.
    for (size_t i = 0; i < outline.size(); ++i) {
        const auto& a = outline[i];
        const auto& b = outline[(i + 1) % outline.size()];
        const Vertex c{p.at(0, 0), p.c.backing, p.c.backingAlpha};
        p.out.insert(p.out.end(), {c, {p.at(a[0], a[1]), p.c.backing, p.c.backingAlpha},
                                   {p.at(b[0], b[1]), p.c.backing, p.c.backingAlpha}});
    }
    // Scanlines, faint.
    for (float v = -halfH + .003f; v < top - .002f; v += .003f) {
        const float over = std::max(0.f, v > 0 ? v - (top - cut) : -v - (halfH - cut));
        const float h = halfW - over - .003f;
        p.rect(-h, v, h, v + .0006f, p.c.line, .045f);
    }
}

void frame(const Painter& p, float top) {
    const auto outline = outlineOf(top);
    for (size_t i = 0; i < outline.size(); ++i) {
        const auto& a = outline[i];
        const auto& b = outline[(i + 1) % outline.size()];
        p.bar(a[0], a[1], b[0], b[1], .007f, p.c.frame, .12f); // glow
        p.bar(a[0], a[1], b[0], b[1], .0017f, p.c.frame, .95f);
    }
    // An inner accent line, and frame-coloured ticks at the cut corners.
    const float in = .005f;
    for (size_t i = 0; i < outline.size(); ++i) {
        const auto& a = outline[i];
        const auto& b = outline[(i + 1) % outline.size()];
        const auto inward = [&](const std::array<float, 2>& q) {
            return std::array<float, 2>{q[0] - std::copysign(in, q[0]), q[1] - std::copysign(in, q[1])};
        };
        const auto ia = inward(a), ib = inward(b);
        p.bar(ia[0], ia[1], ib[0], ib[1], .0006f, p.c.accent, .35f);
    }
    for (const float x : {-1.f, 1.f})
        for (const float y : {-1.f, 1.f}) {
            const float edge = y > 0 ? top : halfH;
            p.bar(x * (halfW - cut + .002f), y * (edge - .0045f), x * (halfW - .0045f), y * (edge - cut + .002f),
                  .0012f, p.c.frame, .75f);
        }
    // The spider on the top edge, on a dark disc.
    const float ev = top;
    p.ring(0, ev, 1, 1, 0, .0125f, p.c.backing, .95f);
    p.ring(0, ev, 1, 1, .011f, .0128f, p.c.frame, .95f);
    p.ring(0, ev - .0012f, .0028f, .0042f, 0, .0042f, p.c.frame, 1, 24);
    p.ring(0, ev + .0047f, 1, 1, 0, .0021f, p.c.frame, 1, 16);
    for (const float side : {-1.f, 1.f})
        for (int leg = 0; leg < 4; ++leg) {
            const float v0 = ev + .0035f - static_cast<float>(leg) * .0022f;
            const float knee = v0 + (leg < 2 ? .0028f : -.0010f);
            const float footV = knee + (leg < 2 ? .0012f : -.0034f);
            p.bar(side * .002f, v0, side * .0055f, knee, .0007f, p.c.frame, 1);
            p.bar(side * .0055f, knee, side * .0078f, footV, .0007f, p.c.frame, 1);
        }
}

// The light on the wrist and its rays fanned up to the panel.
void light(const Painter& p, Vec3 emitter, float seconds) {
    const float pulse = .85f + .15f * std::sin(2 * pi * .8f * (std::isfinite(seconds) ? seconds : 0.f));
    const Vec3 r = p.f.right, u = p.f.up;
    const Vec3 left = p.at(-.072f, -halfH), rightEnd = p.at(.072f, -halfH);
    p.out.insert(p.out.end(),
                 {{emitter, p.c.accent, .10f * pulse}, {left, p.c.accent, .01f}, {rightEnd, p.c.accent, .01f}});
    for (const float a : {-.062f, -.034f, -.008f, .02f, .046f, .068f}) {
        const Vec3 at0 = p.at(a - .0018f, -halfH), at1 = p.at(a + .0018f, -halfH);
        p.out.insert(p.out.end(), {{emitter, p.c.line, .42f * pulse}, {at0, p.c.line, .03f}, {at1, p.c.line, .03f}});
    }
    soft::softArc(p.out, emitter, r, u, 0, .0026f, 0, 2 * pi, p.c.text, .95f, p.soft, 24);
    soft::softArc(p.out, emitter, r, u, .0026f, .0045f, 0, 2 * pi, p.c.line, .45f * pulse, .009f, 32);
}

struct Box {
    Control control;
    float u0, v0, u1, v1;
    bool in(float u, float v) const {
        return u >= u0 && u <= u1 && v >= v0 && v <= v1;
    }
};
// The arrow strip under the panel.
constexpr float stripTop = -halfH - .004f, stripBottom = -halfH - .024f, stripHalf = .075f;
constexpr Box previousBox{Control::previous, -stripHalf, stripBottom, -.045f, stripTop},
    nextBox{Control::next, .045f, stripBottom, stripHalf, stripTop};
constexpr float trackLeft = -.066f, trackRight = .066f, sliderReach = .009f;
const char* const scrollFooter = "STICK UP/DOWN OR DRAG TO SCROLL";
constexpr float titleHeight = .0068f, labelHeight = .0046f, headingHeight = .0042f, footerHeight = .0040f,
                fpsHeight = .0056f;
const char* const fpsWidest = "FPS 888 / 888 HZ POOR";
const char* const gpuWidest = "EYE PASSES 88.88 MS GPU";
constexpr float sparkLeft = .030f;
constexpr float scrollbarU = (scrollbarU0 + scrollbarU1) / 2, scrollTrackTop = viewTop - .002f,
                scrollTrackBottom = viewBottom + .012f, thumbLeast = .016f;

bool slider(Control c) {
    return c == Control::history || c == Control::reach || c == Control::monitor || c == Control::sharpen ||
           c == Control::hudHeight || c == Control::hudScale || c == Control::hudText || c == Control::musicVolume;
}
bool toggle(Control c) {
    return c == Control::aa || c == Control::eyeTest;
}
bool setting(Control c) {
    return slider(c) || toggle(c) || cycles(c);
}
bool off(const View& view, Control c) {
    const auto i = static_cast<unsigned>(c);
    return i < maskedControls && (view.disabled >> i & 1);
}
uint32_t bit(Control c) {
    const auto i = static_cast<unsigned>(c);
    return i < maskedControls ? 1u << i : 0u;
}
const char* labelOf(Control c) {
    switch (c) {
    case Control::aa:
        return "ANTI-ALIASING";
    case Control::eyeTest:
        return "EYE TEST (SAME PICTURE)";
    case Control::history:
        return "HISTORY BLEND";
    case Control::reach:
        return "BUILDING REACH";
    case Control::monitor:
        return "WINDOW FPS";
    case Control::sharpen:
        return "SHARPNESS";
    case Control::hudHeight:
        return "HOLOGRAM HEIGHT";
    case Control::hudScale:
        return "HOLOGRAM SCALE";
    case Control::hudText:
        return "TEXT SIZE";
    case Control::hudTheme:
        return "COLOR THEME";
    case Control::hudTextColor:
        return "TEXT COLOR";
    case Control::musicVolume:
        return "VOLUME";
    case Control::musicAutoplay:
        return "AUTOPLAY";
    default:
        return "";
    }
}
const char* widestNumber(Control c) {
    switch (c) {
    case Control::monitor:
        return "EVERY FRAME";
    case Control::hudHeight:
        return "88 CM";
    case Control::hudScale:
        return "8.88X";
    default:
        return "888%";
    }
}
// Text the panel can draw: the degree sign as DEG, what it has no glyph for a space.
std::string panelText(std::string_view t) {
    std::string out;
    out.reserve(t.size() + 8);
    for (size_t i = 0; i < t.size(); ++i) {
        if (static_cast<unsigned char>(t[i]) == 0xC2 && i + 1 < t.size() && static_cast<unsigned char>(t[i + 1]) == 0xB0) {
            out += " DEG";
            ++i;
            continue;
        }
        out += drawable(t[i]) ? t[i] : ' ';
    }
    return out;
}

// Where everything on a page goes, laid out from the top of the inner area
// down for one text size, each control's explanation wrapped under it.
struct Help {
    float v = 0; // the first line's bottom
    float height = helpHeight, pitch = helpPitch;
    std::vector<std::string> lines;
};
enum class Kind : uint8_t { title, fps, fpsStats, slider, toggle, cycle, vr, section, footer, song, transport, gear, wide };
struct Item {
    Kind kind = Kind::title;
    Control control = Control::none;
    std::string text;
    Box box{Control::none, 0, 0, 0, 0}; // a button; the FPS sparkline
    float v = 0, h = 0;                 // the text's bottom and height
    float track = 0;                    // a slider's track; a section's line
    float numberH = 0, numberW = 0;     // a slider's number
    float v2 = 0, h2 = 0;               // the FPS monitor's GPU line; the stats' numbers
    Help help;
};
Item itemOf(Kind kind, Control control = Control::none) {
    Item it;
    it.kind = kind;
    it.control = control;
    return it;
}
struct Layout {
    float text = 1;
    std::vector<Item> items;
    float bottom = viewTop; // the content's lowest edge, scroll 0
    std::vector<Area> areas;
};

Layout build(int page, float k) {
    Layout l;
    l.text = k;
    const float gapV = .0065f * k;
    float c = viewTop;
    auto helpUnder = [&](const char* text, Help& h) {
        if (!text || !*text)
            return;
        h.height = helpHeight * k;
        h.pitch = helpPitch * k;
        h.lines = wrapText(text, h.height, helpWidth);
        h.v = c - .0022f * k - h.height;
        c = h.v - h.pitch * static_cast<float>(h.lines.size() - 1);
    };
    {
        Item t = itemOf(Kind::title);
        t.text = pageName(page);
        t.h = fittedHeight(t.text, titleHeight * k, .13f);
        t.v = c - .006f - t.h;
        c = t.v - .0045f - .002f * k;
        l.items.push_back(std::move(t));
    }
    auto sliderItem = [&](Control control) {
        Item s = itemOf(Kind::slider, control);
        s.text = labelOf(control);
        s.numberH = fittedHeight(widestNumber(control), .0050f * k, .060f);
        s.numberW = textWidth(widestNumber(control), s.numberH);
        s.h = fittedHeight(s.text, labelHeight * k, trackRight - trackLeft - s.numberW - .006f);
        s.v = c - gapV - std::max(s.h, s.numberH);
        s.track = s.v - std::max(.008f * k, .0075f);
        c = s.track - .005f;
        helpUnder(helpText(control), s.help);
        l.items.push_back(std::move(s));
    };
    auto toggleItem = [&](Control control) {
        Item t = itemOf(Kind::toggle, control);
        t.text = labelOf(control);
        const float bh = std::max(.020f * k, .016f);
        t.box = {control, .030f, c - gapV - bh, .074f, c - gapV};
        t.h = fittedHeight(t.text, labelHeight * k, .030f - .004f - trackLeft);
        t.v = (t.box.v0 + t.box.v1) / 2 - t.h / 2;
        c = t.box.v0;
        helpUnder(helpText(control), t.help);
        l.items.push_back(std::move(t));
    };
    // A button with its name over it, stepping through choices.
    auto cycleItem = [&](Kind kind, Control control, std::string label, const char* help) {
        Item y = itemOf(kind, control);
        y.text = std::move(label);
        y.h = fittedHeight(y.text, labelHeight * k, trackRight - trackLeft);
        y.v = c - gapV - y.h;
        const float bh = std::max(.018f * k, .015f);
        y.box = {control, trackLeft, y.v - .003f * k - bh, trackRight, y.v - .003f * k};
        c = y.box.v0;
        helpUnder(help, y.help);
        l.items.push_back(std::move(y));
    };
    auto footerItem = [&](const char* text) {
        Item f = itemOf(Kind::footer);
        f.text = text;
        f.h = fittedHeight(f.text, footerHeight * k, .14f);
        f.v = c - gapV - f.h;
        c = f.v;
        l.items.push_back(std::move(f));
    };
    if (page == 0) {
        // The FPS monitor: its sparkline beside the readout while the readout
        // fits there near its size, else under it.
        Item f = itemOf(Kind::fps);
        const float beside = sparkLeft - trackLeft - .004f;
        const float near = fittedHeight(fpsWidest, fpsHeight * k, beside);
        const bool fits = near >= .85f * fpsHeight * k;
        f.h = fits ? near : fittedHeight(fpsWidest, fpsHeight * k, trackRight - trackLeft);
        f.v = c - gapV - f.h;
        f.h2 = fittedHeight(gpuWidest, helpHeight * k, fits ? beside : trackRight - trackLeft);
        f.v2 = f.v - .003f * k - f.h2;
        if (fits) {
            f.box = {Control::none, sparkLeft, f.v2, trackRight, f.v + f.h};
        } else {
            const float top = f.v2 - .004f * k;
            f.box = {Control::none, trackLeft, top - .012f * k, trackRight, top};
        }
        c = f.box.v0;
        helpUnder(fpsHelp, f.help);
        l.items.push_back(std::move(f));
        // The session's HIGH, LOW and AVG, and RESET at the row's end.
        Item st = itemOf(Kind::fpsStats, Control::fpsReset);
        const float sh = std::max(.017f * k, .014f);
        st.box = {Control::fpsReset, trackRight - .028f, c - gapV - sh, trackRight, c - gapV};
        st.h = fittedHeight("AVG", .0030f * k, .009f);
        st.h2 = fittedHeight("888.8", .0048f * k, .026f);
        c = st.box.v0;
        helpUnder(helpText(Control::fpsReset), st.help);
        l.items.push_back(std::move(st));
        // Every setting of the MILES VR tab, by its sections.
        const auto& rows = vr_settings::rows();
        for (size_t r = 0; r < rows.size(); ++r) {
            const auto& row = rows[r];
            if (!shownRow(row))
                continue;
            if (row.item == vr_settings::Item::none) {
                Item h = itemOf(Kind::section);
                h.text = panelText(row.title ? row.title : "");
                h.track = c - gapV;
                h.h = fittedHeight(h.text, headingHeight * k, trackRight - trackLeft);
                h.v = h.track - .0035f * k - h.h;
                c = h.v;
                l.items.push_back(std::move(h));
                continue;
            }
            cycleItem(Kind::vr, vrControl(r), panelText(row.title ? row.title : ""), row.help);
        }
        footerItem(scrollFooter);
    } else if (page == musicPage) {
        // The song and where it is in the list, then the player's buttons.
        Item g = itemOf(Kind::song);
        g.h = .0052f * k;
        g.v = c - gapV - g.h;
        g.h2 = .0040f * k;
        g.v2 = g.v - .003f * k - g.h2;
        c = g.v2;
        l.items.push_back(std::move(g));
        {
            const Control row[3] = {Control::musicPrevious, Control::musicPlay, Control::musicNext};
            const float space = .004f, w = (trackRight - trackLeft - 2 * space) / 3;
            const float th = std::max(.020f * k, .016f), top = c - gapV;
            for (int i = 0; i < 3; ++i) {
                Item b = itemOf(Kind::transport, row[i]);
                const float u0 = trackLeft + static_cast<float>(i) * (w + space);
                b.box = {row[i], u0, top - th, u0 + w, top};
                l.items.push_back(std::move(b));
            }
            c = top - th;
            helpUnder("Previous song, play or pause, next song. Songs: the Music folder beside the launcher.",
                      l.items.back().help);
        }
        toggleItem(Control::musicAutoplay);
        sliderItem(Control::musicVolume);
        footerItem(scrollFooter);
    } else if (page == gearPage) {
        // A row per gadget: its name, its charges, the equipped one marked.
        const float rh = std::max(.0115f * k, .0095f);
        for (int i = 0; i < gearRows; ++i) {
            Item r = itemOf(Kind::gear);
            r.track = static_cast<float>(i); // which gadget
            r.box = {Control::none, trackLeft, c - gapV * .5f - rh, trackRight, c - gapV * .5f};
            r.h = std::min(.0042f * k, rh * .55f);
            c = r.box.v0;
            l.items.push_back(std::move(r));
        }
        Item h = itemOf(Kind::section);
        h.text = "APPS";
        h.track = c - gapV;
        h.h = fittedHeight(h.text, headingHeight * k, trackRight - trackLeft);
        h.v = h.track - .0035f * k - h.h;
        c = h.v;
        l.items.push_back(std::move(h));
        Item f = itemOf(Kind::wide, Control::fnsmApp);
        f.text = "FNSM APP";
        f.box = {Control::fnsmApp, trackLeft, c - gapV - .022f * k, trackRight, c - gapV};
        c = f.box.v0;
        helpUnder(helpText(Control::fnsmApp), f.help);
        l.items.push_back(std::move(f));
        footerItem(scrollFooter);
    } else if (page == 1) {
        toggleItem(Control::aa);
        for (const Control control : {Control::history, Control::sharpen, Control::reach, Control::monitor})
            sliderItem(control);
        toggleItem(Control::eyeTest);
        footerItem(scrollFooter);
    } else {
        for (const Control control : {Control::hudHeight, Control::hudScale, Control::hudText})
            sliderItem(control);
        cycleItem(Kind::cycle, Control::hudTheme, labelOf(Control::hudTheme), helpText(Control::hudTheme));
        cycleItem(Kind::cycle, Control::hudTextColor, labelOf(Control::hudTextColor), helpText(Control::hudTextColor));
        footerItem(scrollFooter);
    }
    l.bottom = c - .007f * k;

    // Everything drawn, as rectangles (tests: none overlaps another).
    auto area = [&](float u0, float v0, float u1, float v1, const char* what) {
        l.areas.push_back({u0, v0, u1, v1, what});
    };
    auto centred = [&](const std::string& t, float v, float h, const char* what) {
        const float w = textWidth(t, h);
        area(-w / 2, v, w / 2, v + h, what);
    };
    auto help = [&](const Help& h) {
        for (size_t i = 0; i < h.lines.size(); ++i) {
            const float v = h.v - h.pitch * static_cast<float>(i);
            area(trackLeft, v, trackLeft + textWidth(h.lines[i], h.height), v + h.height, "an explanation line");
        }
    };
    for (const Item& it : l.items) {
        switch (it.kind) {
        case Kind::title:
            centred(it.text, it.v, it.h, "the title");
            break;
        case Kind::fps:
            area(trackLeft, it.v, trackLeft + textWidth(fpsWidest, it.h), it.v + it.h, "the FPS readout");
            area(trackLeft, it.v2, trackLeft + textWidth(gpuWidest, it.h2), it.v2 + it.h2, "the eye GPU line");
            area(it.box.u0, it.box.v0, it.box.u1, it.box.v1, "the FPS sparkline");
            break;
        case Kind::fpsStats:
            area(trackLeft, it.box.v0, it.box.u0 - .003f, it.box.v1, "the session FPS boxes");
            area(it.box.u0, it.box.v0, it.box.u1, it.box.v1, "RESET");
            break;
        case Kind::slider:
            area(trackLeft, it.v, trackLeft + textWidth(it.text, it.h), it.v + it.h, "a control's name");
            area(trackRight - it.numberW, it.v, trackRight, it.v + it.numberH, "a slider's number");
            area(trackLeft - .005f, it.track - .005f, trackRight + .005f, it.track + .005f, "a slider's track");
            break;
        case Kind::toggle:
        case Kind::cycle:
        case Kind::vr:
            area(trackLeft, it.v, trackLeft + textWidth(it.text, it.h), it.v + it.h, "a control's name");
            area(it.box.u0, it.box.v0, it.box.u1, it.box.v1, "a button");
            break;
        case Kind::section:
            area(trackLeft, it.track - .0004f, trackRight, it.track + .0004f, "a section's line");
            centred(it.text, it.v, it.h, "a section's name");
            break;
        case Kind::footer:
            centred(it.text, it.v, it.h, "the footer");
            break;
        case Kind::song:
            area(trackLeft, it.v, trackRight, it.v + it.h, "the song");
            area(trackLeft, it.v2, trackRight, it.v2 + it.h2, "the song's number");
            break;
        case Kind::transport:
        case Kind::wide:
            area(it.box.u0, it.box.v0, it.box.u1, it.box.v1, "a button");
            break;
        case Kind::gear:
            area(it.box.u0, it.box.v0 + .0005f, it.box.u1, it.box.v1 - .0005f, "a gadget's row");
            break;
        }
        help(it.help);
    }
    return l;
}
// Per thread (the overlay and the input stage each keep their own), rebuilt
// only when the text size changes: no allocation in a steady frame.
const Layout& layoutOf(int page, float text) {
    const int p = ((page % pages) + pages) % pages;
    const float k = textFactor(text);
    thread_local std::array<Layout, pages> cache;
    thread_local std::array<long, pages> key = [] {
        std::array<long, pages> a{};
        a.fill(-1);
        return a;
    }();
    const long want = std::lround(k * 1000);
    if (key[static_cast<size_t>(p)] != want) {
        cache[static_cast<size_t>(p)] = build(p, k);
        key[static_cast<size_t>(p)] = want;
    }
    return cache[static_cast<size_t>(p)];
}
const Item* itemOf(const Layout& l, Control c) {
    for (const Item& it : l.items)
        if (it.control == c && c != Control::none)
            return &it;
    return nullptr;
}

void helpLines(const Painter& p, const Help& h, Vec3 color) {
    for (size_t k = 0; k < h.lines.size(); ++k)
        p.text(h.lines[k], trackLeft, h.v - h.pitch * static_cast<float>(k), h.height, color);
}
Vec3 bandColor(const Palette& c, int band) {
    return band == 2 ? c.good : band == 1 ? c.warn : band == 0 ? c.bad : c.line;
}
std::string whole(float value) {
    if (!std::isfinite(value) || value < 0 || value > 999)
        return "--";
    return std::to_string(static_cast<int>(std::lround(value)));
}
// "FPS 44 / 90 HZ" in its band's colour and word, the eye passes' GPU time,
// and the last 5 s as a sparkline with the refresh as a line. Nothing known
// shows "--"; no number is made up.
void fpsMonitor(const Painter& p, const Item& it, const View& view) {
    const int band = fpsBand(view.fps, view.refreshHz);
    std::string readout = "FPS " + whole(view.fps) + " / " + whole(view.refreshHz) + " HZ";
    if (*fpsWord(band))
        readout += std::string(" ") + fpsWord(band);
    p.text(readout, trackLeft, it.v, it.h, bandColor(p.c, band));
    char gpu[48];
    if (view.eyeGpuMs == 0)
        std::snprintf(gpu, sizeof gpu, "EYE PASSES OFF");
    else if (std::isfinite(view.eyeGpuMs) && view.eyeGpuMs > 0 && view.eyeGpuMs < 100)
        std::snprintf(gpu, sizeof gpu, "EYE PASSES %.2f MS GPU", view.eyeGpuMs);
    else
        std::snprintf(gpu, sizeof gpu, "EYE PASSES -- MS GPU");
    p.text(gpu, trackLeft, it.v2, it.h2, p.c.help);
    const Box& b = it.box;
    p.rect(b.u0, b.v0, b.u1, b.v1, p.c.backing, .7f);
    p.bar(b.u0, b.v0, b.u1, b.v0, .0005f, p.c.accent, .5f);
    float top = std::isfinite(view.refreshHz) && view.refreshHz > 0 ? view.refreshHz * 1.15f : 0.f;
    for (const float f : view.fpsTrail)
        if (std::isfinite(f))
            top = std::max(top, f * 1.05f);
    if (!(top > 0))
        return;
    const auto y = [&](float f) {
        return b.v0 + .0008f + (b.v1 - b.v0 - .0016f) * std::clamp(f / top, 0.f, 1.f);
    };
    if (std::isfinite(view.refreshHz) && view.refreshHz > 0)
        p.bar(b.u0, y(view.refreshHz), b.u1, y(view.refreshHz), .0004f, p.c.text, .35f);
    const size_t n = view.fpsTrail.size();
    const float step = (b.u1 - b.u0) / static_cast<float>(n - 1);
    for (size_t k = 1; k < n; ++k) {
        const float a = view.fpsTrail[k - 1], c = view.fpsTrail[k];
        if (!std::isfinite(a) || !std::isfinite(c))
            continue;
        p.bar(b.u0 + step * static_cast<float>(k - 1), y(a), b.u0 + step * static_cast<float>(k), y(c), .0007f,
              bandColor(p.c, fpsBand(c, view.refreshHz)), .95f);
    }
}

void button(const Painter& p, const Box& b, std::string label, const View& view, float k = 1) {
    const bool hover = view.hover == b.control, held = view.held == b.control;
    if (off(view, b.control))
        label = "N/A";
    p.rect(b.u0, b.v0, b.u1, b.v1, p.c.backing, .75f);
    p.rect(b.u0, b.v0, b.u1, b.v1, held ? p.c.line : p.c.accent, held ? .5f : hover ? .3f : .12f);
    const Vec3 edge = hover || held ? p.c.text : p.c.accent;
    const float w = hover || held ? .0011f : .0007f, a = hover || held ? .95f : .8f;
    p.bar(b.u0, b.v0, b.u1, b.v0, w, edge, a);
    p.bar(b.u1, b.v0, b.u1, b.v1, w, edge, a);
    p.bar(b.u1, b.v1, b.u0, b.v1, w, edge, a);
    p.bar(b.u0, b.v1, b.u0, b.v0, w, edge, a);
    if (hover)
        p.bar(b.u0, (b.v0 + b.v1) / 2, b.u1, (b.v0 + b.v1) / 2, (b.v1 - b.v0) + .006f, p.c.line, .06f);
    if (label.empty())
        return;
    const float h = std::min(fittedHeight(label, .0052f * k, (b.u1 - b.u0) - .014f), (b.v1 - b.v0) * .62f);
    p.text(label, (b.u0 + b.u1) / 2 - textWidth(label, h) / 2, (b.v0 + b.v1) / 2 - h / 2, h, p.c.text);
}

// The session's HIGH, LOW and AVG in three boxes, and RESET.
void fpsStats(const Painter& p, const Item& it, const View& view, float k) {
    const Box& r = it.box;
    const float space = .003f, right = r.u0 - space, w = (right - trackLeft - 2 * space) / 3;
    const char* names[3] = {"HIGH", "LOW", "AVG"};
    const float values[3] = {view.fpsHigh, view.fpsLow, view.fpsAvg};
    for (int i = 0; i < 3; ++i) {
        const float u0 = trackLeft + static_cast<float>(i) * (w + space), u1 = u0 + w;
        p.rect(u0, r.v0, u1, r.v1, p.c.backing, .7f);
        p.bar(u0, r.v0, u1, r.v0, .0005f, p.c.accent, .5f);
        const float mid = (u0 + u1) / 2;
        p.text(names[i], mid - textWidth(names[i], it.h) / 2, r.v1 - .0015f - it.h, it.h, p.c.label);
        char n[16];
        if (std::isfinite(values[i]) && values[i] > 0)
            std::snprintf(n, sizeof n, "%.1f", static_cast<double>(std::min(values[i], 999.f)));
        else
            std::snprintf(n, sizeof n, "--");
        const int band = fpsBand(values[i], view.refreshHz);
        p.text(n, mid - textWidth(n, it.h2) / 2, r.v0 + .0012f, it.h2, band < 0 ? p.c.text : bandColor(p.c, band));
    }
    button(p, r, "RESET", view, k);
}

// The < > of a stepping button, at its two ends.
void arrowsOn(const Painter& p, const Box& b, Vec3 color, float alpha) {
    const float h = (b.v1 - b.v0) * .34f, w = h * .8f, mid = (b.v0 + b.v1) / 2, inset = .0035f;
    p.triangle(b.u0 + inset, mid, b.u0 + inset + w, mid + h / 2, b.u0 + inset + w, mid - h / 2, color, alpha);
    p.triangle(b.u1 - inset, mid, b.u1 - inset - w, mid - h / 2, b.u1 - inset - w, mid + h / 2, color, alpha);
}
void sliderRow(const Painter& p, const Item& it, const View& view) {
    const float value = view.settings[it.control];
    const Range r = range(it.control);
    const bool hover = view.hover == it.control, held = view.held == it.control;
    const float track = it.track;
    p.text(it.text, trackLeft, it.v, it.h, hover || held ? p.c.text : p.c.label);
    const bool disabled = off(view, it.control);
    const std::string number = disabled ? "UNAVAILABLE" : valueText(it.control, value);
    const float nh = fittedHeight(number, it.numberH, it.numberW);
    p.text(number, trackRight - textWidth(number, nh), it.v, nh, p.c.value);
    const float f = std::isfinite(value) ? std::clamp((value - r.low) / (r.high - r.low), 0.f, 1.f) : 0.f;
    const float knob = trackLeft + (trackRight - trackLeft) * f;
    p.rect(trackLeft, track - .0011f, trackRight, track + .0011f, p.c.accent, .3f);
    p.rect(trackLeft, track - .0011f, knob, track + .0011f, disabled ? p.c.backing : p.c.accent, .9f);
    for (const float end : {trackLeft, trackRight})
        p.bar(end, track - .003f, end, track + .003f, .0006f, p.c.line, .7f);
    if (hover || held)
        p.ring(knob, track, 1, 1, 0, .0085f, p.c.line, held ? .3f : .16f);
    p.ring(knob, track, 1, 1, 0, held ? .0050f : .0042f, p.c.backing, .95f);
    p.ring(knob, track, 1, 1, .0030f, held ? .0050f : .0042f, held || hover ? p.c.text : p.c.line, .95f);
    p.ring(knob, track, 1, 1, 0, .0016f, p.c.frame, 1, 16);
}

// A page's content, drawn by its layout (the Painter scrolled), then clipped.
void content(const Painter& p, const Layout& l, const View& view) {
    const float k = l.text;
    for (const Item& it : l.items) {
        switch (it.kind) {
        case Kind::title:
            p.text(it.text, -textWidth(it.text, it.h) / 2, it.v, it.h, p.c.text);
            p.bar(-.05f, it.v - .004f, .05f, it.v - .004f, .0007f, p.c.frame, .8f);
            break;
        case Kind::fps:
            fpsMonitor(p, it, view);
            break;
        case Kind::fpsStats:
            fpsStats(p, it, view, k);
            break;
        case Kind::slider:
            sliderRow(p, it, view);
            break;
        case Kind::toggle:
        case Kind::cycle:
        case Kind::vr: {
            const bool lit = view.hover == it.control || view.held == it.control;
            p.text(it.text, trackLeft, it.v, it.h, lit ? p.c.text : p.c.label);
            const int row = vrRow(it.control);
            const std::string value =
                it.kind == Kind::vr                     ? vrValueText(row, view.vr)
                : it.control == Control::musicAutoplay ? std::string(view.music.autoplay ? "ON" : "OFF")
                                                         : valueText(it.control, view.settings[it.control]);
            button(p, it.box, value, view, k);
            // A list steps both ways; a switch just flips.
            if (it.kind == Kind::cycle || (row >= 0 && !vr_settings::rows()[static_cast<size_t>(row)].choices.empty()))
                arrowsOn(p, it.box, lit ? p.c.text : p.c.accent, off(view, it.control) ? .25f : .9f);
            break;
        }
        case Kind::section:
            p.bar(trackLeft, it.track, trackRight, it.track, .0006f, p.c.frame, .7f);
            p.text(it.text, -textWidth(it.text, it.h) / 2, it.v, it.h, p.c.frame);
            break;
        case Kind::footer:
            p.text(it.text, -textWidth(it.text, it.h) / 2, it.v, it.h, p.c.accent);
            break;
        case Kind::song: {
            const MusicView& m = view.music;
            const std::string song = m.tracks <= 0 ? std::string("NO SONGS YET")
                                     : m.song.empty() ? std::string("--")
                                                      : panelText(m.song);
            p.text(song, trackLeft, it.v, fittedHeight(song, it.h, trackRight - trackLeft), p.c.value);
            std::string line = m.tracks <= 0 ? "ADD MP3, WAV, WMA OR M4A FILES TO THE MUSIC FOLDER"
                                             : "TRACK " + std::to_string(m.track + 1) + "/" + std::to_string(m.tracks) +
                                                   (m.failed ? "  - CANNOT PLAY THIS ONE" : m.playing ? "  - PLAYING" : "  - PAUSED");
            p.text(line, trackLeft, it.v2, fittedHeight(line, it.h2, trackRight - trackLeft),
                   m.failed ? p.c.warn : m.playing ? p.c.good : p.c.help);
            break;
        }
        case Kind::transport: {
            button(p, it.box, "", view, k);
            const Box& b = it.box;
            const float cx = (b.u0 + b.u1) / 2, cy = (b.v0 + b.v1) / 2;
            const float s = std::min((b.v1 - b.v0) * .26f, (b.u1 - b.u0) * .2f);
            const bool lit = view.hover == it.control || view.held == it.control;
            const Vec3 col = lit ? p.c.text : p.c.line;
            const auto right = [&](float x) { p.triangle(x - s * .8f, cy + s, x - s * .8f, cy - s, x + s * .8f, cy, col, .95f); };
            const auto left = [&](float x) { p.triangle(x + s * .8f, cy + s, x - s * .8f, cy, x + s * .8f, cy - s, col, .95f); };
            const auto upright = [&](float x) { p.rect(x - s * .2f, cy - s, x + s * .2f, cy + s, col, .95f); };
            if (it.control == Control::musicPrevious) {
                upright(cx - s);
                left(cx + s * .25f);
            } else if (it.control == Control::musicNext) {
                right(cx - s * .25f);
                upright(cx + s);
            } else if (view.music.playing) {
                upright(cx - s * .45f);
                upright(cx + s * .45f);
            } else {
                right(cx + s * .1f);
            }
            break;
        }
        case Kind::gear: {
            const int i = static_cast<int>(it.track);
            if (i >= view.gearCount) {
                if (i == 0)
                    p.text("NO GADGETS YET", trackLeft, it.box.v0 + .002f, it.h, p.c.help);
                break;
            }
            const GearRow& g = view.gear[static_cast<size_t>(i)];
            const Box& b = it.box;
            p.rect(b.u0, b.v0 + .0005f, b.u1, b.v1 - .0005f, p.c.backing, .6f);
            if (g.equipped)
                p.rect(b.u0, b.v0 + .0005f, b.u0 + .002f, b.v1 - .0005f, p.c.frame, .95f);
            const float mid = (b.v0 + b.v1) / 2 - it.h / 2;
            const std::string name = panelText(g.name);
            p.text(name, b.u0 + .005f, mid, fittedHeight(name, it.h, .085f), g.equipped ? p.c.text : p.c.label);
            if (g.most > 0) {
                const std::string n = std::to_string(g.ammo) + "/" + std::to_string(g.most);
                p.text(n, b.u1 - .004f - textWidth(n, it.h), mid, it.h, p.c.value);
            }
            break;
        }
        case Kind::wide:
            button(p, it.box, it.control == Control::fnsmApp && view.fnsmOpen ? "CLOSE FNSM APP" : it.text, view, k);
            break;
        }
        helpLines(p, it.help, p.c.help);
    }
}

// The slim scrollbar on the right, while the page scrolls.
void scrollbar(const Painter& p, int page, float scroll, float k, bool dragging) {
    const Thumb t = scrollThumb(page, scroll, k);
    if (!t.shown)
        return;
    p.bar(scrollbarU, scrollTrackBottom, scrollbarU, scrollTrackTop, .0010f, p.c.accent, .35f);
    p.bar(scrollbarU, t.v0, scrollbarU, t.v1, dragging ? .0034f : .0026f, dragging ? p.c.text : p.c.line, .9f);
}

// The arrows and the page between them.
void strip(const Painter& p, const View& view, int page) {
    p.rect(-stripHalf, stripBottom, stripHalf, stripTop, p.c.backing, p.c.backingAlpha);
    p.bar(-stripHalf, stripBottom, stripHalf, stripBottom, .0008f, p.c.frame, .8f);
    p.bar(-stripHalf, stripTop, stripHalf, stripTop, .0006f, p.c.accent, .5f);
    button(p, previousBox, "", view);
    button(p, nextBox, "", view);
    const float mid = (stripTop + stripBottom) / 2;
    for (const Box* b : {&previousBox, &nextBox}) {
        const float c = (b->u0 + b->u1) / 2, d = b == &previousBox ? -1.f : 1.f;
        const Vec3 color = view.hover == b->control ? p.c.text : p.c.line;
        p.triangle(c + d * .0060f, mid, c - d * .0040f, mid + .0060f, c - d * .0040f, mid - .0060f, color, .95f);
    }
    char label[48];
    std::snprintf(label, sizeof label, "%d/%d %s", page + 1, pages, pageName(page));
    // A refused setting's note takes the page's place for a while, in red.
    const std::string t = view.note.empty() ? std::string(label) : view.note;
    const float h = fittedHeight(t, .0046f, .084f);
    p.text(t, -textWidth(t, h) / 2, mid - h / 2, h, view.note.empty() ? p.c.text : p.c.bad);
}

// Where the other controller points: a thin beam from its tip, a dot there.
void pointerMark(const Painter& p, const View& view) {
    const Vec3 at = p.at(view.pointU, view.pointV);
    if (!finite(view.tip) || !finite(at))
        return;
    soft::softBar(p.out, view.tip, at, p.normal, .0007f, p.c.line, .35f, p.soft);
    p.ring(view.pointU, view.pointV, 1, 1, 0, .0030f, p.c.text, .95f, 16);
    p.ring(view.pointU, view.pointV, 1, 1, .0030f, .0055f, p.c.line, .35f, 24);
}

// The themes: their text stands out on their glass, and their bands differ in
// more than red against green.
constexpr std::array<Palette, themes> palettes{{
    // Spider-Man: red frame, blue lines.
    {{1, .06f, .05f}, {.1f, .5f, 1}, {.5f, .85f, 1}, {.85f, .95f, 1}, {.5f, .85f, 1}, {1, .72f, .66f},
     {.62f, .74f, .88f}, {.004f, .016f, .05f}, {.35f, .95f, .45f}, {1, .55f, .08f}, {1, .32f, .28f}, .58f},
    // High contrast: white and yellow on near-black glass.
    {{1, 1, 1}, {1, .8f, 0}, {1, .9f, .25f}, {1, 1, 1}, {1, 1, 1}, {1, .85f, 0}, {.9f, .9f, .85f},
     {0, 0, 0}, {1, 1, 1}, {1, .8f, 0}, {1, .3f, 0}, .85f},
    // Deuteranopia/protanopia safe: blue and orange, never red against green.
    {{1, .35f, 0}, {.05f, .3f, 1}, {.45f, .7f, 1}, {.95f, .95f, 1}, {.55f, .78f, 1}, {1, .6f, .12f},
     {.7f, .76f, .88f}, {.004f, .01f, .04f}, {.25f, .55f, 1}, {1, .85f, .2f}, {1, .35f, 0}, .62f},
    // Tritanopia safe: red and teal, never blue against yellow.
    {{.9f, .04f, .06f}, {0, .55f, .5f}, {.3f, .9f, .82f}, {.95f, .98f, .98f}, {.4f, .92f, .85f}, {1, .55f, .6f},
     {.72f, .82f, .8f}, {.01f, .02f, .02f}, {.15f, .9f, .8f}, {1, .6f, .75f}, {.95f, .08f, .08f}, .62f},
    // Monochrome: greys only; the bands differ in brightness and by word.
    {{.85f, .85f, .85f}, {.5f, .5f, .5f}, {.75f, .75f, .75f}, {1, 1, 1}, {.85f, .85f, .85f}, {1, 1, 1},
     {.7f, .7f, .7f}, {.01f, .01f, .01f}, {1, 1, 1}, {.6f, .6f, .6f}, {.35f, .35f, .35f}, .65f},
}};
constexpr std::array<Vec3, textColors> textColorValues{{{0, 0, 0}, {1, 1, 1}, {1, .85f, .1f}, {.2f, .95f, 1}}};
} // namespace

bool shownRow(const vr_settings::Row& row) {
    using vr_settings::Item;
    switch (row.item) {
    case Item::bodyHaptics:
    case Item::hapticWebs:
    case Item::hapticWind:
    case Item::hapticCombat:
    case Item::hapticMovement:
        return false;
    case Item::none: // a section's heading, but not BODY HAPTICS' own
        return !row.title || std::string_view(row.title).find("BODY HAPTICS") == std::string_view::npos;
    default:
        return true;
    }
}
std::string vrValueText(int row, const vr_settings::Values& v) {
    if (row < 0)
        return {};
    const auto& r = vr_settings::rows()[static_cast<size_t>(row)];
    const int at = vr_settings::choice(r.item, v);
    if (r.choices.empty())
        return at ? "ON" : "OFF";
    return at >= 0 && static_cast<size_t>(at) < r.choices.size() ? panelText(r.choices[static_cast<size_t>(at)])
                                                                 : std::string("--");
}

float& Settings::operator[](Control c) {
    switch (c) {
    case Control::aa:
        return aa;
    case Control::history:
        return history;
    case Control::reach:
        return reach;
    case Control::monitor:
        return monitor;
    case Control::sharpen:
        return sharpen;
    case Control::eyeTest:
        return eyeTest;
    case Control::hudHeight:
        return hudHeight;
    case Control::hudScale:
        return hudScale;
    case Control::hudText:
        return hudText;
    case Control::hudTheme:
        return hudTheme;
    case Control::hudTextColor:
        return hudTextColor;
    case Control::musicVolume:
        return musicVolume;
    default: {
        static thread_local float none;
        none = 0;
        return none;
    }
    }
}
Range range(Control c) {
    switch (c) {
    case Control::history:
        return {0, 95, 5};
    case Control::reach:
        return {50, 400, 10};
    case Control::monitor:
        return {0, 240, 5};
    case Control::sharpen:
        return {0, 200, 10};
    case Control::hudHeight:
        return {0, 25, 1};
    case Control::hudScale:
        return {.5f, 2, .05f};
    case Control::hudText:
        return {75, 175, 5};
    case Control::hudTheme:
        return {0, themes - 1, 1};
    case Control::hudTextColor:
        return {0, textColors - 1, 1};
    case Control::musicVolume:
        return {0, 100, 10};
    default:
        return {0, 1, 1};
    }
}
const char* pageName(int page) {
    switch (page) {
    case 1:
        return "GRAPHICS";
    case 2:
        return "MUSIC";
    case 3:
        return "GEAR";
    case 4:
        return "HOLOGRAM";
    default:
        return "VR SETTINGS";
    }
}
float textFactor(float textScale) {
    return std::isfinite(textScale) ? std::clamp(textScale, .75f, 1.75f) : 1.f;
}
bool onPanel(int page, float u, float v) {
    if (!std::isfinite(u) || !std::isfinite(v))
        return false;
    return (std::abs(u) <= halfW && v >= -halfH && v <= panelTop(page)) ||
           (std::abs(u) <= stripHalf && v >= stripBottom && v <= stripTop + .004f);
}
float panelTop(int) {
    return halfH;
}
float contentHeight(int page, float text) {
    return viewTop - layoutOf(page, text).bottom;
}
float maxScroll(int page, float text) {
    return std::max(0.f, contentHeight(page, text) - (viewTop - viewBottom));
}
float clampScroll(int page, float scroll, float text) {
    return std::isfinite(scroll) ? std::clamp(scroll, 0.f, maxScroll(page, text)) : 0.f;
}
Thumb scrollThumb(int page, float scroll, float text) {
    const float most = maxScroll(page, text);
    if (!(most > 0))
        return {false, 0, 0};
    const float track = scrollTrackTop - scrollTrackBottom;
    const float length = std::max(thumbLeast, track * (viewTop - viewBottom) / contentHeight(page, text));
    const float top = scrollTrackTop - (track - length) * clampScroll(page, scroll, text) / most;
    return {true, top - length, top};
}
bool onScrollbar(int page, float u, float v, float text) {
    return std::isfinite(u) && std::isfinite(v) && u >= scrollbarU0 && u <= scrollbarU1 && v >= viewBottom &&
           v <= viewTop && maxScroll(page, text) > 0;
}
Control controlAt(int page, float u, float v, float scroll, float text) {
    if (!onPanel(page, u, v))
        return Control::none;
    for (const Box& b : {previousBox, nextBox})
        if (b.in(u, v))
            return b.control;
    // Only what shows in the inner area can be pressed, where it shows.
    if (v < viewBottom || v > viewTop || onScrollbar(page, u, v, text))
        return Control::none;
    const float cv = v - clampScroll(page, scroll, text);
    for (const Item& it : layoutOf(page, text).items) {
        if (it.control == Control::none)
            continue;
        if (it.kind == Kind::slider) {
            if (std::abs(cv - it.track) <= sliderReach && u >= trackLeft - .008f && u <= trackRight + .008f)
                return it.control;
        } else if (it.box.in(u, cv)) {
            return it.control;
        }
    }
    return Control::none;
}
Spot controlSpot(int page, Control c, float scroll, float text) {
    const int p = ((page % pages) + pages) % pages;
    if (c == Control::previous || c == Control::next) {
        const Box& b = c == Control::previous ? previousBox : nextBox;
        return {true, (b.u0 + b.u1) / 2, (b.v0 + b.v1) / 2, true};
    }
    const Item* it = itemOf(layoutOf(p, text), c);
    if (!it)
        return {false, 0, 0, false};
    const float dv = clampScroll(p, scroll, text);
    Spot s = it->kind == Kind::slider ? Spot{true, 0, it->track + dv}
                                      : Spot{true, (it->box.u0 + it->box.u1) / 2, (it->box.v0 + it->box.v1) / 2 + dv};
    s.visible = s.v >= viewBottom && s.v <= viewTop;
    return s;
}
std::vector<Area> pageAreas(int page, float text) {
    return layoutOf(page, text).areas;
}

// Sutherland-Hodgman against v >= low and v <= high, in the panel's plane: a
// clipped triangle becomes a fan (up to 5 points), its colour and alpha
// interpolated along.
void clipBand(std::vector<Vertex>& out, size_t from, const Frame& f, float scale, float low, float high) {
    if (from >= out.size() || !(scale > 0))
        return;
    const size_t end = from + (out.size() - from) / 3 * 3;
    const auto vOf = [&](const Vertex& x) {
        return dot(x.position - f.centre, f.up) / scale;
    };
    const auto mix = [](const Vertex& a, const Vertex& b, float t) {
        Vertex r = a;
        r.position = a.position + (b.position - a.position) * t;
        r.color = a.color + (b.color - a.color) * t;
        r.alpha = a.alpha + (b.alpha - a.alpha) * t;
        return r;
    };
    thread_local std::vector<Vertex> kept;
    kept.clear();
    for (size_t i = from; i < end; i += 3) {
        const float v0 = vOf(out[i]), v1 = vOf(out[i + 1]), v2 = vOf(out[i + 2]);
        if (!std::isfinite(v0) || !std::isfinite(v1) || !std::isfinite(v2))
            continue;
        if (std::min({v0, v1, v2}) >= low && std::max({v0, v1, v2}) <= high) {
            kept.insert(kept.end(), {out[i], out[i + 1], out[i + 2]});
            continue;
        }
        if (std::max({v0, v1, v2}) < low || std::min({v0, v1, v2}) > high)
            continue;
        std::array<Vertex, 8> a{out[i], out[i + 1], out[i + 2]}, b{};
        size_t n = 3;
        for (int side = 0; side < 2; ++side) {
            const auto inside = [&](const Vertex& x) {
                return side == 0 ? vOf(x) >= low : vOf(x) <= high;
            };
            const float edge = side == 0 ? low : high;
            size_t m = 0;
            for (size_t k = 0; k < n; ++k) {
                const Vertex& p = a[k];
                const Vertex& q = a[(k + 1) % n];
                const bool pin = inside(p), qin = inside(q);
                if (pin)
                    b[m++] = p;
                if (pin != qin) {
                    const float vp = vOf(p), vq = vOf(q);
                    b[m++] = mix(p, q, (edge - vp) / (vq - vp));
                }
            }
            a = b;
            n = m;
            if (n < 3)
                break;
        }
        for (size_t k = 1; k + 1 < n; ++k)
            kept.insert(kept.end(), {a[0], a[k], a[k + 1]});
    }
    out.resize(from);
    out.insert(out.end(), kept.begin(), kept.end());
}

const char* fpsWord(int band) {
    switch (band) {
    case 2:
        return "OK";
    case 1:
        return "LOW";
    case 0:
        return "POOR";
    default:
        return "";
    }
}
const char* themeName(int theme) {
    switch (theme) {
    case 1:
        return "HIGH CONTRAST";
    case 2:
        return "DEUTAN/PROTAN SAFE";
    case 3:
        return "TRITAN SAFE";
    case 4:
        return "MONOCHROME";
    default:
        return "SPIDER-MAN RED/BLUE";
    }
}
const char* textColorName(int color) {
    switch (color) {
    case 1:
        return "WHITE";
    case 2:
        return "YELLOW";
    case 3:
        return "CYAN";
    default:
        return "THEME";
    }
}
Palette palette(int theme, int textColor) {
    Palette p = palettes[static_cast<size_t>(theme >= 0 && theme < themes ? theme : 0)];
    if (textColor > 0 && textColor < textColors) {
        const Vec3 t = textColorValues[static_cast<size_t>(textColor)];
        p.text = p.label = p.value = t;
        p.help = t * .8f;
    }
    return p;
}
const char* helpText(Control c) {
    switch (c) {
    case Control::fpsReset:
        return fpsStatsHelp;
    case Control::aa:
        return "Smooths jagged edges; FXAA is cheap but softens fine detail a little.";
    case Control::history:
        return "Mixes in the last frame: 20-40 calms shimmer, 80-95 is heavy (trails behind motion).";
    case Control::reach:
        return "How far buildings keep full detail; more costs some frame rate and video memory.";
    case Control::monitor:
        return "Limits desktop window updates; lower leaves more for the headset. 0: every frame.";
    case Control::sharpen:
        return "Up to 100% is normal CAS; 110-200% is extra strong, with halos and more shimmer.";
    case Control::eyeTest:
        return "ON shows both eyes one picture (flat): if far flicker stops, the eyes disagree.";
    case Control::hudHeight:
        return "How far above your wrist the hologram floats.";
    case Control::hudScale:
        return "How big the whole hologram is.";
    case Control::hudText:
        return "Bigger text is easier to read; the page scrolls to fit.";
    case Control::hudTheme:
        return "Colour sets for colour-blind players; FPS also says OK/LOW.";
    case Control::hudTextColor:
        return "The colour of names and values; THEME uses the set's own.";
    case Control::musicAutoplay:
        return "ON plays the next song when one ends.";
    case Control::musicVolume:
        return "Your music's loudness only; the game's own sound is not changed.";
    case Control::fnsmApp:
        return "Opens the game's app over your left hand; tap it with your right index finger. B closes it.";
    default:
        return "";
    }
}
std::vector<std::string> wrapText(const std::string& text, float height, float width) {
    std::vector<std::string> lines;
    std::string line;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(' ', at);
        if (end == std::string::npos)
            end = text.size();
        const std::string word = text.substr(at, end - at);
        at = end + 1;
        if (word.empty())
            continue;
        const std::string longer = line.empty() ? word : line + " " + word;
        if (!line.empty() && textWidth(longer, height) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = longer;
        }
    }
    if (!line.empty() || lines.empty())
        lines.push_back(line);
    return lines;
}
float sliderValue(Control c, float u) {
    const Range r = range(c);
    const float f = std::isfinite(u) ? std::clamp((u - trackLeft) / (trackRight - trackLeft), 0.f, 1.f) : 0.f;
    const float steps = std::round(f * (r.high - r.low) / r.step);
    const float value = std::clamp(r.low + steps * r.step, r.low, r.high);
    return r.step < 1 ? std::round(value * 100) / 100 : value;
}
std::string valueText(Control c, float value) {
    char n[32];
    const float v = std::isfinite(value) ? value : 0.f;
    switch (c) {
    case Control::aa:
        return v > .5f ? "FXAA" : "OFF";
    case Control::eyeTest:
        return v > .5f ? "ON" : "OFF";
    case Control::history:
    case Control::sharpen:
        if (v < .5f)
            return "OFF";
        std::snprintf(n, sizeof n, "%d%%", static_cast<int>(std::lround(v)));
        break;
    case Control::reach:
    case Control::hudText:
    case Control::musicVolume:
        std::snprintf(n, sizeof n, "%d%%", static_cast<int>(std::lround(v)));
        break;
    case Control::monitor:
        if (v < .5f)
            return "EVERY FRAME";
        std::snprintf(n, sizeof n, "%d FPS", static_cast<int>(std::lround(v)));
        break;
    case Control::hudHeight:
        std::snprintf(n, sizeof n, "%d CM", static_cast<int>(std::lround(v)));
        break;
    case Control::hudScale:
        std::snprintf(n, sizeof n, "%.2fX", static_cast<double>(v));
        break;
    case Control::hudTheme:
        return themeName(static_cast<int>(std::lround(v)));
    case Control::hudTextColor:
        return textColorName(static_cast<int>(std::lround(v)));
    default:
        return {};
    }
    return n;
}
const char* shortName(Control c) {
    switch (c) {
    case Control::aa:
        return "AA";
    case Control::history:
        return "HISTORY";
    case Control::reach:
        return "REACH";
    case Control::monitor:
        return "WINDOW FPS";
    case Control::sharpen:
        return "SHARPNESS";
    case Control::eyeTest:
        return "EYE TEST";
    case Control::fpsReset:
        return "FPS RESET";
    case Control::hudHeight:
        return "HEIGHT";
    case Control::hudScale:
        return "SCALE";
    case Control::hudText:
        return "TEXT SIZE";
    case Control::hudTheme:
        return "THEME";
    case Control::hudTextColor:
        return "TEXT COLOR";
    case Control::musicVolume:
        return "VOLUME";
    default:
        return "";
    }
}
bool onRelease(Control c) {
    return c == Control::hudHeight || c == Control::hudScale || c == Control::hudText;
}
bool cycles(Control c) {
    return c == Control::hudTheme || c == Control::hudTextColor;
}
bool gpuSide(Control c) {
    return c == Control::aa || c == Control::history || c == Control::reach || c == Control::monitor ||
           c == Control::sharpen || c == Control::eyeTest;
}
float clampTo(Control c, float value) {
    const Range r = range(c);
    return std::isfinite(value) ? std::clamp(value, r.low, r.high) : r.low;
}

void Applier::start(const Settings& live) {
    last_.fill(-1e9);
    for (unsigned i = 0; i < controls; ++i) {
        const auto c = static_cast<Control>(i);
        if (setting(c))
            good_[i] = wanted_[i] = clampTo(c, live[c]);
    }
    dirty_ = 0;
}
void Applier::want(Control c, float value) {
    const auto i = static_cast<unsigned>(c);
    if (i >= controls || !setting(c) || (disabled_ >> i & 1))
        return;
    wanted_[i] = clampTo(c, value);
    dirty_ |= 1u << i;
}
uint32_t Applier::due(double now, Control held) const {
    uint32_t out = 0;
    for (unsigned i = 0; i < maskedControls; ++i) {
        if (!(dirty_ >> i & 1))
            continue;
        const auto c = static_cast<Control>(i);
        if (onRelease(c) && held == c)
            continue; // the panel would move under the pointer dragging it
        if (!gpuSide(c) || held != c || !(now - last_[i] < gpuInterval))
            out |= 1u << i;
    }
    return out;
}
bool Applier::result(Control c, bool ok, float actual, double now) {
    const auto i = static_cast<unsigned>(c);
    if (i >= maskedControls)
        return false;
    last_[i] = now;
    dirty_ &= ~(1u << i);
    const float tolerance = range(c).step / 2;
    if (ok && std::isfinite(actual) && std::abs(actual - wanted_[i]) <= tolerance) {
        good_[i] = clampTo(c, actual);
        failures_[i] = 0;
        return true;
    }
    wanted_[i] = good_[i];
    char text[48];
    if (++failures_[i] >= failuresToDisable) {
        disabled_ |= 1u << i;
        std::snprintf(text, sizeof text, "%s OFF: REFUSED", shortName(c));
    } else {
        std::snprintf(text, sizeof text, "%s REFUSED", shortName(c));
    }
    fault(text, now);
    return false;
}
Settings Applier::shown(const Settings& live) const {
    Settings out = live;
    for (unsigned i = 0; i < controls; ++i) {
        const auto c = static_cast<Control>(i);
        if (setting(c))
            out[c] = clampTo(c, dirty_ >> i & 1 ? wanted_[i] : live[c]);
    }
    return out;
}
std::string Applier::note(double now) const {
    return now - noteAt_ >= 0 && now - noteAt_ < noteSeconds ? note_ : std::string();
}

Pointer point(const Frame& f, float scale, const Pose& grip, unsigned hand) {
    Pointer out;
    if (!validTrackedPose(grip) || !finite(f.centre) || !finite(f.right) || !finite(f.up))
        return out;
    const float s = std::isfinite(scale) ? std::clamp(scale, .5f, 2.f) : 1.f;
    const Vec3 dir = webShooter(grip, hand).orientation.rotate({0, 0, -1});
    out.tip = grip.position + dir * .05f;
    const Vec3 n = cross(f.right, f.up); // toward the eyes
    const float depth = dot(out.tip - f.centre, n);
    if (std::abs(depth) <= touchDepth) {
        out.at = out.tip - n * depth;
    } else {
        const float facing = dot(dir, n);
        if (depth < 0 || facing > -1e-3f)
            return out;
        const float t = -depth / facing;
        if (!(t >= 0 && t <= pointReach))
            return out;
        out.at = out.tip + dir * t;
    }
    out.u = dot(out.at - f.centre, f.right) / s;
    out.v = dot(out.at - f.centre, f.up) / s;
    out.hit = std::isfinite(out.u) && std::isfinite(out.v);
    return out;
}

void Menu::setScroll(int page, float scroll, float text) {
    const int p = ((page % pages) + pages) % pages;
    scroll_[static_cast<size_t>(p)] = clampScroll(p, scroll, textFactor(text));
}
Menu::Result Menu::update(const Input& in, Settings& settings) {
    Result r;
    const float t = std::isfinite(in.trigger) ? std::clamp(in.trigger, 0.f, 1.f) : 0.f;
    const bool touched = t > (touched_ ? Toggle::rest : Toggle::touch);
    const bool first = touched && !touched_;
    touched_ = touched;
    const bool down = t > (down_ ? Toggle::released : Toggle::pressed);
    const bool press = down && !down_;
    down_ = down;
    const float k = textFactor(in.text);
    // The update the panel's size, place or text changes, what the pointer is
    // over moves under it: hover nothing, press nothing.
    const bool relaid = layoutSeen_ && in.layout != layout_;
    layout_ = in.layout;
    layoutSeen_ = true;
    for (int i = 0; i < pages; ++i)
        scroll_[static_cast<size_t>(i)] = clampScroll(i, scroll_[static_cast<size_t>(i)], k);
    float& scroll = scroll_[static_cast<size_t>(page_)];
    const bool on = in.active && in.pointer.hit && onPanel(page_, in.pointer.u, in.pointer.v);
    Control now =
        on && !relaid && drag_ == Drag::none ? controlAt(page_, in.pointer.u, in.pointer.v, scroll, k) : Control::none;
    if (bit(now) & in.disabled)
        now = Control::none; // a disabled control: the press is still the panel's, it does nothing
    r.hovered = now != Control::none && now != hover_;
    hover_ = now;
    // A pull that began on the panel is the panel's until it is let go.
    if (!touched)
        claim_ = false;
    else if (first && on)
        claim_ = true;
    if (!down || !in.active || relaid) {
        held_ = Control::none;
        drag_ = Drag::none;
    }
    if (!in.active)
        return r;
    if (claim_ && press && !relaid) {
        if (hover_ != Control::none) {
            r.pressed = true;
            held_ = hover_;
            const int row = vrRow(hover_);
            if (row >= 0 && in.vr) {
                // A VR SETTINGS row: a switch flips, a list steps (< back, > forward).
                const auto& rw = vr_settings::rows()[static_cast<size_t>(row)];
                const int n = rw.choices.empty() ? 2 : static_cast<int>(rw.choices.size());
                const int at = vr_settings::choice(rw.item, *in.vr);
                const int step = !rw.choices.empty() && in.pointer.u < (trackLeft + trackRight) / 2 ? -1 : 1;
                r.vrItem = rw.item;
                r.vrChoice = ((at + step) % n + n) % n;
            } else {
                switch (hover_) {
                case Control::previous:
                    setPage(page_ - 1);
                    break;
                case Control::next:
                    setPage(page_ + 1);
                    break;
                case Control::fpsReset:
                    r.fpsReset = true;
                    break;
                case Control::musicPrevious:
                    r.music = Result::Music::previous;
                    break;
                case Control::musicPlay:
                    r.music = Result::Music::toggle;
                    break;
                case Control::musicNext:
                    r.music = Result::Music::next;
                    break;
                case Control::musicAutoplay:
                    r.music = Result::Music::autoplay;
                    break;
                case Control::fnsmApp:
                    r.fnsm = true;
                    break;
                case Control::aa:
                case Control::eyeTest:
                    settings[hover_] = settings[hover_] > .5f ? 0.f : 1.f;
                    r.changed |= bit(hover_);
                    break;
                case Control::hudTheme:
                case Control::hudTextColor: {
                    const int n = static_cast<int>(range(hover_).high) + 1;
                    const float v = settings[hover_];
                    const int at = std::isfinite(v) ? static_cast<int>(std::lround(v)) : 0;
                    settings[hover_] = static_cast<float>(((at + 1) % n + n) % n);
                    r.changed |= bit(hover_);
                    break;
                }
                default:
                    break;
                }
            }
        } else if (on && maxScroll(page_, k) > 0 && in.pointer.v >= viewBottom && in.pointer.v <= viewTop &&
                   std::abs(in.pointer.u) <= halfW) {
            // Empty panel or the scrollbar: a drag that scrolls.
            drag_ = onScrollbar(page_, in.pointer.u, in.pointer.v, k) ? Drag::bar : Drag::content;
            dragV_ = in.pointer.v;
            dragScroll_ = scroll;
        }
    }
    float& shown = scroll_[static_cast<size_t>(page_)]; // the page may have changed
    if (claim_ && down && drag_ != Drag::none && in.pointer.hit && std::isfinite(in.pointer.v)) {
        float want = dragScroll_ + (in.pointer.v - dragV_); // the content follows the pointer up
        if (drag_ == Drag::bar) {
            const Thumb th = scrollThumb(page_, shown, k);
            const float room = scrollTrackTop - scrollTrackBottom - (th.v1 - th.v0);
            want = room > 1e-4f ? dragScroll_ + (dragV_ - in.pointer.v) * maxScroll(page_, k) / room : dragScroll_;
        }
        want = clampScroll(page_, want, k);
        if (std::abs(want - shown) > 1e-6f) {
            shown = want;
            r.scrolled = true;
        }
    }
    // The stick: the panel's while the pointer is on it; it scrolls while
    // nothing is held or dragged.
    if (on) {
        r.stick = true;
        const float y = std::isfinite(in.stick) ? std::clamp(in.stick, -1.f, 1.f) : 0.f;
        const float dt = std::isfinite(in.seconds) ? std::clamp(in.seconds, 0.f, .1f) : 0.f;
        if (std::abs(y) > stickDead && held_ == Control::none && drag_ == Drag::none) {
            const float tilt = (std::abs(y) - stickDead) / (1 - stickDead) * (y > 0 ? 1.f : -1.f);
            const float want = clampScroll(page_, shown - tilt * stickSpeed * dt, k); // up: toward the top
            if (std::abs(want - shown) > 1e-6f) {
                shown = want;
                r.scrolled = true;
            }
        }
    }
    // A held slider follows the pointer along its track, on the panel or off it.
    if (claim_ && down && slider(held_) && in.pointer.hit) {
        const float value = sliderValue(held_, in.pointer.u);
        if (!(std::abs(value - settings[held_]) < 1e-4f)) {
            settings[held_] = value;
            r.changed |= bit(held_);
        }
    }
    return r;
}

Vec3 wrist(const Pose& grip, unsigned hand) {
    // body::Config::wristFromGrip, a right hand's; a left mirrors x.
    const float back = hand == 0 ? -.02f : .02f;
    return compose(grip, Pose{{back, .09f, 0}, {}}).position;
}
Vec3 emitter(const Pose& grip, unsigned hand) {
    const float back = hand == 0 ? -.045f : .045f;
    return compose(grip, Pose{{back, .085f, 0}, {}}).position;
}
float armDistance(const Pose& point, unsigned pointHand, const Pose& arm, unsigned armHand) {
    // cos and sin of 30 degrees: the forearm from the grip's +y toward its +z.
    const Vec3 along = arm.orientation.rotate({0, .8660254f, .5f});
    // From the grip, 2 cm to the back of the hand as wrist() is.
    const Vec3 from = arm.position + arm.orientation.rotate({armHand == 0 ? -.02f : .02f, 0, 0});
    const auto toSegment = [&](Vec3 p) {
        const float t = std::clamp(dot(p - from, along), 0.f, Toggle::forearm);
        return length(p - (from + along * t));
    };
    return std::min(toSegment(point.position), toSegment(wrist(point, pointHand)));
}
bool Toggle::update(float seconds, bool play, unsigned hudHand, const Pose grips[2], const bool tracked[2],
                    const float triggers[2]) {
    const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
    wait_ = std::max(0.f, wait_ - dt);
    const unsigned hud = hudHand ? 1u : 0u, other = 1u - hud;
    bool toggled = false;
    for (unsigned i = 0; i < 2; ++i) {
        const float trigger = tracked[i] && std::isfinite(triggers[i]) ? triggers[i] : 0.f;
        // Touched: the trigger off its rest. The claim starts there, before the
        // pull is deep enough for anything of the game's to answer it.
        const bool touched = trigger > (touched_[i] ? rest : touch);
        const bool first = touched && !touched_[i];
        touched_[i] = touched;
        const bool down = trigger > (down_[i] ? released : pressed);
        const bool press = down && !down_[i];
        down_[i] = down;
        const uint32_t bit = 1u << i;
        float distance = -1;
        if (i == other && tracked[i] && tracked[hud] && validTrackedPose(grips[i]) && validTrackedPose(grips[hud])) {
            distance = armDistance(grips[i], i, grips[hud], hud);
            if (!std::isfinite(distance))
                distance = -1;
        }
        const bool inReach = distance >= 0 && distance <= reach;
        if (!touched || !play) {
            hands_ &= ~bit;
            continue;
        }
        // The pull is the toggle's from its start to its release, even one too
        // soon after the last toggle or drifting out of reach.
        if (first && inReach)
            hands_ |= bit;
        if (!press || !inReach)
            continue;
        hands_ |= bit;
        if (wait_ > 0)
            continue;
        on_ = !on_;
        ++toggles_;
        wait_ = rearm;
        toggled = true;
    }
    const float step = dt / fadeSeconds;
    opacity_ = std::clamp(opacity_ + (on_ ? step : -step), 0.f, 1.f);
    return toggled;
}
bool Toggle::flip() {
    if (wait_ > 0)
        return false;
    on_ = !on_;
    ++toggles_;
    wait_ = rearm;
    return true;
}

Frame place(const View& view, Vec3 viewer) {
    const float s = std::isfinite(view.scale) ? std::clamp(view.scale, .5f, 2.f) : 1.f;
    Vec3 up = view.headUp;
    up = finite(up) && length(up) > 1e-3f ? normalized(up) : Vec3{0, 1, 0};
    // The height in cm whatever the scale; the panel's size scales.
    const float height = std::isfinite(view.heightCm) ? std::clamp(view.heightCm, 0.f, 25.f) / 100 : gap;
    const Vec3 centre = view.emitter + up * (height + panelHeight / 2 * s);
    Vec3 look = centre - viewer;
    look = length(look) > .02f ? normalized(look) : up * -1.f;
    Vec3 right = cross(look, up);
    right = length(right) > 1e-3f ? normalized(right) : soft::perpendicular(look);
    return {centre, right, normalized(cross(right, look))};
}

void appendWristHud(std::vector<Vertex>& out, const View& view, Vec3 viewer, float pixelAngle) {
    const float opacity = std::isfinite(view.opacity) ? std::clamp(view.opacity, 0.f, 1.f) : 0.f;
    if (opacity < .002f || !finite(view.emitter) || !finite(viewer))
        return;
    const size_t from = out.size();
    const Frame f = place(view, viewer);
    const float s = std::isfinite(view.scale) ? std::clamp(view.scale, .5f, 2.f) : 1.f;
    const float distance = std::max(length(f.centre - viewer), .1f);
    const float pa = std::isfinite(pixelAngle) && pixelAngle > 0 ? pixelAngle : .001f;
    const Palette colors = palette(view.theme, view.textColor);
    const Painter p{out, f, s, pa * distance, cross(f.right, f.up), colors};
    light(p, view.emitter, view.seconds);
    const int page = ((view.page % pages) + pages) % pages;
    const float top = panelTop(page);
    backing(p, top);
    // The page's content, scrolled and clipped to the inner area; the
    // scrollbar beside it. The strip and frame stay put.
    const float k = textFactor(view.textScale);
    const float scroll = clampScroll(page, view.scroll, k);
    const size_t clipFrom = out.size();
    Painter scrolled = p;
    scrolled.dv = scroll;
    content(scrolled, layoutOf(page, k), view);
    clipBand(out, clipFrom, f, s, viewBottom, viewTop);
    scrollbar(p, page, scroll, k, view.pointing && onScrollbar(page, view.pointU, view.pointV, k));
    strip(p, view, page);
    frame(p, top);
    if (view.pointing)
        pointerMark(p, view);
    for (size_t i = from; i < out.size(); ++i)
        out[i].alpha *= opacity;
}

void appendAppGlass(std::vector<Vertex>& out, const std::array<Vec3, 4>& corners, Vec3 viewer, float pixelAngle) {
    for (const auto& c : corners)
        if (!finite(c))
            return;
    const Vec3 mid = (corners[0] + corners[1] + corners[2] + corners[3]) * .25f;
    const Vec3 rightHalf = (corners[1] - corners[0]) * .5f, upHalf = (corners[0] - corners[3]) * .5f;
    const float w = length(rightHalf), h = length(upHalf);
    if (!(w > 1e-4f) || !(h > 1e-4f))
        return;
    const Vec3 r = rightHalf / w, u = upHalf / h;
    const float pad = .006f, soft = (std::isfinite(pixelAngle) && pixelAngle > 0 ? pixelAngle : .001f) *
                                    std::max(length(mid - viewer), .1f);
    const Palette c = palette(0, 0);
    // Just behind the app, so the app draws over it.
    const Vec3 n = normalized(cross(r, u)), back = n * -.0015f;
    const auto at = [&](float x, float y) { return mid + back + r * x + u * y; };
    const float x = w + pad, y = h + pad;
    soft::quad(out, {at(-x, -y), c.backing, .92f}, {at(x, -y), c.backing, .92f}, {at(x, y), c.backing, .92f},
               {at(-x, y), c.backing, .92f});
    const Vec3 edge[4] = {at(-x, -y), at(x, -y), at(x, y), at(-x, y)};
    for (int i = 0; i < 4; ++i) {
        soft::softBar(out, edge[i], edge[(i + 1) % 4], n, .0035f, c.frame, .12f, soft);
        soft::softBar(out, edge[i], edge[(i + 1) % 4], n, .0009f, c.frame, .95f, soft);
    }
}

bool sane(const std::vector<Vertex>& out, size_t from, Vec3 near, float radius) {
    if (from > out.size() || out.size() - from > maxVertices)
        return false;
    for (size_t i = from; i < out.size(); ++i) {
        const auto& v = out[i];
        if (!finite(v.position) || !finite(v.color) || !std::isfinite(v.alpha) || length(v.position - near) > radius)
            return false;
    }
    return true;
}
} // namespace spidy::wrist_hud

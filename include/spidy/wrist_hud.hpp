#pragma once
// The wrist menu: a hologram over one wrist, from the SM2 port of SpidyVR
// (its Updates 14-24, GPLv3; SpidyVR by Ilya Mezerowsky), adapted to SM2VR.
//
// A small soft light on top of one wrist (the left by default) throws faint
// rays up to a translucent panel floating over it, turned to the eyes: a red
// frame with cut corners and a spider at its top.
//
// Toggled, not gazed at: the other hand anywhere along the hologram arm's
// forearm and that hand's trigger pulled there turns it on; the same again
// turns it off. That pull is the menu's alone until the trigger is let go
// (no web, no reel, nothing of the game's). F8 on the PC's keyboard toggles
// it too.
//
// Three pages, flipped with the arrow buttons on a strip under the panel:
// 1 VR SETTINGS - the frame rate (FPS now, its last 5 s, the session's HIGH,
//   LOW and AVG with RESET), then every setting of the MILES VR / SPIDY VR
//   tab (vr_settings.hpp rows; the game's own Settings tab cannot be shown in
//   Spider-Man 2), each a button that steps through its choices.
// 2 GRAPHICS - the eye passes, live: ANTI-ALIASING (FXAA), HISTORY BLEND,
//   SHARPNESS, BUILDING REACH, WINDOW FPS and the EYE TEST.
// 3 MUSIC - the songs in the Music folder (phone_music.hpp): the song, its
//   number, PREVIOUS / PLAY-PAUSE / NEXT, AUTOPLAY and the music's VOLUME.
// 4 GEAR - the gadgets in the gadget wheel's order with their charges, the
//   equipped one marked, and FNSM APP: the game's Friendly Neighborhood
//   Spider-Man app, shown over the left hand and tapped with the right finger.
// 5 HOLOGRAM - its height over the wrist, its size, the text size, a colour
//   theme and a text colour.
// Pages 3 and 4 are what SM2VR's phone had; there is no phone any more.
// The other hand's controller points at the panel or touches it: what it is
// on lights up, and its trigger presses a button or grabs a slider and drags
// it. A page taller than the panel scrolls (the pointing hand's stick, or a
// drag on the scrollbar or on empty panel).
//
// Not taken from the SM2 port: its STATUS page (placeholder health, gadgets
// and map), its web look and music pages (SM2VR draws the game's own webs and
// plays music on the phone) and eye supersampling.
#include "math.hpp"
#include "vertex.hpp"
#include "vr_settings.hpp"
#include "fps_meter.hpp"
#include "gadget_wheel.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace spidy::wrist_hud {
// Where the wrist joint is from a hand's grip pose, and the hologram's light:
// on top of that wrist, 2.5 cm further out on the back of the hand.
// hand: 0 the left, 1 the right.
Vec3 wrist(const Pose& grip, unsigned hand);
Vec3 emitter(const Pose& grip, unsigned hand);

// The toggle: the other hand's grip or wrist within `reach` of the hologram
// arm's forearm (`forearm` back from its grip, armDistance) and its trigger
// pulled past `pressed` there. A pull begun in reach is claimed from its
// start (hands(): neither a web nor anything of the game's sees it); one begun
// out of reach and pressed in reach from the press, until the trigger is let
// go (under `rest`). A second toggle needs `rearm` seconds after the first.
// The panel fades in and out over fadeSeconds. flip(): the keyboard's toggle.
class Toggle {
  public:
    static constexpr float reach = .15f, forearm = .28f, touch = .12f, rest = .05f, pressed = .6f,
                           released = .3f, rearm = .25f, fadeSeconds = .15f;
    // seconds: since the previous update. play: the controllers play VR (no
    // phone, wheel or pad layer). hudHand: the hologram's wrist (0 left).
    // Returns true when it toggled on this update.
    bool update(float seconds, bool play, unsigned hudHand, const Pose grips[2], const bool tracked[2],
                const float triggers[2]);
    bool flip();
    bool on() const {
        return on_;
    }
    void set(bool on) {
        on_ = on;
    }
    float opacity() const { // 0 hidden .. 1 shown, faded
        return opacity_;
    }
    uint32_t hands() const { // a bit per hand whose trigger the toggle holds
        return hands_;
    }
    uint32_t toggles() const {
        return toggles_;
    }

  private:
    bool on_{}, down_[2]{}, touched_[2]{};
    float opacity_{}, wait_{};
    uint32_t hands_{}, toggles_{};
};
// How far the pointing hand is from the hologram arm's forearm: the nearer of
// its grip and its wrist joint to the segment from the arm's grip `forearm`
// back along the forearm (the grip's +y turned 30 degrees toward its +z).
float armDistance(const Pose& point, unsigned pointHand, const Pose& arm, unsigned armHand);

// The panel's controls. Below maskedControls: the GRAPHICS and HOLOGRAM
// settings (a bit each in a 32-bit mask). fpsReset is a press. From vrFirst:
// vr_settings::rows()[control - vrFirst], a setting of the VR SETTINGS page.
enum class Control : uint8_t {
    none,
    previous,
    next,
    aa,
    history,
    reach,
    monitor,
    sharpen,
    eyeTest,
    hudHeight,
    hudScale,
    hudText,
    hudTheme,
    hudTextColor,
    musicVolume,
    fpsReset = 32,
    musicPrevious,
    musicPlay,
    musicNext,
    musicAutoplay,
    fnsmApp,
    tuneFps,      // AUTO TUNE (auto_tune.hpp, from SM2VR 0.0.127): MAX FPS
    tuneBalanced, // BALANCED
    tuneLooks,    // BEST LOOKS
    vrFirst = 64,
};
constexpr int controls = 32;
constexpr int maskedControls = 32;
static_assert(static_cast<int>(Control::musicVolume) < maskedControls);
// The VR SETTINGS page's control for vr_settings::rows()[row], and back (-1: not one).
inline Control vrControl(size_t row) {
    return static_cast<Control>(static_cast<int>(Control::vrFirst) + static_cast<int>(row));
}
inline int vrRow(Control c) {
    const int r = static_cast<int>(c) - static_cast<int>(Control::vrFirst);
    return r >= 0 && r < static_cast<int>(vr_settings::rows().size()) ? r : -1;
}
// Whether a row of the MILES VR tab is on the VR SETTINGS page: not the
// bHaptics rows (SM2VR has no body haptics).
bool shownRow(const vr_settings::Row&);
// The GRAPHICS and HOLOGRAM values, as the mod runs now.
struct Settings {
    float aa = 1;          // 0 off, 1 FXAA
    float history = 0;     // percent
    float reach = 150;     // building reach, percent
    float monitor = 30;    // the game window's pictures a second (0 every frame)
    float sharpen = 0;     // percent (0 off: no pass)
    float eyeTest = 0;     // 1 both eyes the left eye's picture (never kept)
    float hudHeight = 9;   // cm from the light up to the panel's lower edge
    float hudScale = 1;    // the whole hologram's size
    float hudText = 100;   // the pages' text size, percent
    float hudTheme = 0;    // a colour theme (themeName)
    float hudTextColor = 0; // 0 the theme's own text colours (textColorName)
    float musicVolume = 80; // the music's, percent
    float& operator[](Control);
    float operator[](Control c) const {
        return const_cast<Settings&>(*this)[c];
    }
};
struct Range {
    float low, high, step;
};
Range range(Control);
constexpr int pages = 5; // VR SETTINGS, GRAPHICS, MUSIC, GEAR, HOLOGRAM
constexpr int musicPage = 2, gearPage = 3;
const char* pageName(int page);
// The text size as a factor (hud_text / 100, held to .75-1.75): bigger text
// wraps onto more lines and the page scrolls.
float textFactor(float textScale);
// Where a control is on the panel (u right, v up from its middle, metres at
// scale 1) on `page`, its content scrolled up by `scroll` and laid out for
// `text`; none off them all, and none for a control scrolled out of view.
Control controlAt(int page, float u, float v, float scroll = 0, float text = 1);
// Whether (u, v) is on the panel or its arrow strip under it.
bool onPanel(int page, float u, float v);
float panelTop(int page);
// The inner area a page's content is clipped to, and the scrollbar's grab zone across.
constexpr float viewTop = .22f / 2 - .015f, viewBottom = -.22f / 2 + .005f, scrollbarU0 = .0765f,
                scrollbarU1 = .0835f;
float contentHeight(int page, float text = 1);
float maxScroll(int page, float text = 1);
float clampScroll(int page, float scroll, float text = 1);
struct Thumb {
    bool shown;
    float v0, v1;
};
Thumb scrollThumb(int page, float scroll, float text = 1);
bool onScrollbar(int page, float u, float v, float text = 1);
struct Frame;
// Clips the triangles of out[from, end) to the panel's band low <= v <= high.
void clipBand(std::vector<Vertex>& out, size_t from, const Frame& f, float scale, float low, float high);
// A control's plain-English explanation, drawn under it in smaller, dimmer text.
const char* helpText(Control);
constexpr const char* fpsStatsHelp = "Per-second rates from 10 s after load; loads and pauses skipped; RESET restarts.";
constexpr const char* fpsHelp = "New frames per second you're actually getting, versus your headset's refresh rate.";
// The FPS band (fpsBand: 2 good, 1 low, 0 poor, -1 unknown) as a word.
const char* fpsWord(int band);
constexpr int themes = 5, textColors = 4;
const char* themeName(int theme);
const char* textColorName(int color);
// Linear RGB: frame, accent, line, text, label, value, help, backing (its
// alpha), and good, warn, bad for the FPS bands and notes.
struct Palette {
    Vec3 frame, accent, line, text, label, value, help, backing, good, warn, bad;
    float backingAlpha;
};
Palette palette(int theme, int textColor);
constexpr float helpHeight = .0036f, helpPitch = .0054f, helpWidth = .132f;
std::vector<std::string> wrapText(const std::string& text, float height, float width);
// Where a control's middle is drawn on `page` (hit tests and tests).
struct Spot {
    bool found;
    float u, v;
    bool visible = true;
};
Spot controlSpot(int page, Control, float scroll = 0, float text = 1);
// Every piece drawn on a page, as rectangles: none may overlap another.
struct Area {
    float u0, v0, u1, v1;
    const char* what;
};
std::vector<Area> pageAreas(int page, float text = 1);
float sliderValue(Control, float u);
std::string valueText(Control, float value);
// A VR SETTINGS row's value as the panel prints it ("30°" -> "30 DEG", "ON").
std::string vrValueText(int row, const vr_settings::Values&);
const char* shortName(Control);
// Settings that move or resize the panel: applied only when let go.
bool onRelease(Control);
// The buttons a press steps through, wrapping round.
bool cycles(Control);
// Settings the eye passes read on the render thread: applied when let go,
// or at most every Applier::gpuInterval while dragged.
bool gpuSide(Control);
float clampTo(Control, float value);

// Between the panel and the mod: a value the panel sets waits here until it
// is due; game_xr applies it at the start of its frame and reports whether it
// took. A refused value goes back to the last good one with a short red note;
// a control refused failuresToDisable times in a row is off for the session.
class Applier {
  public:
    static constexpr double gpuInterval = .25, noteSeconds = 3;
    static constexpr int failuresToDisable = 2;
    void start(const Settings& live);
    void want(Control, float value);
    uint32_t due(double now, Control held) const;
    bool result(Control, bool ok, float actual, double now);
    float revert(Control c) const {
        return good_[static_cast<unsigned>(c)];
    }
    Settings shown(const Settings& live) const;
    uint32_t disabled() const {
        return disabled_;
    }
    std::string note(double now) const;
    void fault(const std::string& note, double now) {
        note_ = note;
        noteAt_ = now;
    }

  private:
    std::array<float, controls> wanted_{}, good_{};
    std::array<double, controls> last_ = [] {
        std::array<double, controls> a{};
        a.fill(-1e9);
        return a;
    }();
    std::array<uint8_t, controls> failures_{};
    uint32_t dirty_{}, disabled_{};
    std::string note_;
    double noteAt_ = -1e9;
};

// The MUSIC page's player (phone_music.hpp PhoneMusic::Status) and the GEAR
// page's gadgets (the gadget wheel's places in its order).
struct MusicView {
    int tracks{}, track{}; // track from 0; tracks 0: no songs
    bool playing{}, autoplay = true, failed{};
    std::string song;
};
struct GearRow {
    std::string name;
    int ammo{}, most{}; // most 0: none to count
    bool equipped{};
};
constexpr int gearRows = gadgetPlaces;
struct View {
    Vec3 emitter{};       // the light on the wrist, in the world
    Vec3 headUp{0, 1, 0}; // the headset's up: the panel stands along it
    float opacity = 1;    // Toggle::opacity
    float scale = 1;      // the hologram's size
    float seconds{};      // a clock, for the light's slow pulse
    int page{};
    Settings settings;
    vr_settings::Values vr; // the VR SETTINGS page's values
    MusicView music;
    std::array<GearRow, gearRows> gear{};
    int gearCount{};
    bool fnsmOpen{}; // the game's FNSM app is up (its button reads CLOSE FNSM APP)
    // AUTO TUNE: the mode last pressed (-1 none) and its three lines (the PC, the check, the next launch).
    int tuneMode = -1;
    std::array<std::string, 3> tune;
    Control hover{}, held{};
    bool pointing{};
    Vec3 tip{};
    float pointU{}, pointV{};
    uint32_t disabled{}; // Applier::disabled: shown UNAVAILABLE
    std::string note;    // Applier::note: red, in the arrow strip
    // The FPS monitor (FpsMeter, fed by game_xr); NaN shows "--". eyeGpuMs:
    // the eye passes' GPU time a pair (AA, history, sharpen).
    float fps = std::numeric_limits<float>::quiet_NaN(), refreshHz = std::numeric_limits<float>::quiet_NaN(),
          eyeGpuMs = std::numeric_limits<float>::quiet_NaN();
    float fpsHigh = std::numeric_limits<float>::quiet_NaN(), fpsLow = std::numeric_limits<float>::quiet_NaN(),
          fpsAvg = std::numeric_limits<float>::quiet_NaN();
    // The hologram as applied (not as a dragged slider shows it), and the page's scroll.
    float heightCm = 9, textScale = 1;
    int theme{}, textColor{};
    float scroll{};
    std::array<float, FpsMeter::history> fpsTrail = [] {
        std::array<float, FpsMeter::history> a{};
        a.fill(std::numeric_limits<float>::quiet_NaN());
        return a;
    }();
};
// The panel: 18 by 22 cm at scale 1, its lower edge heightCm over the light.
constexpr float panelWidth = .18f, panelHeight = .22f, gap = .09f;
static_assert(viewTop == panelHeight / 2 - .015f);
struct Frame {
    Vec3 centre, right, up;
};
Frame place(const View&, Vec3 viewer);
// Where a controller points on the panel: the ray from the controller's tip
// (5 cm out along where the hand points) meets the panel's plane within
// `pointReach`; a tip within touchDepth of the plane touches it there.
struct Pointer {
    bool hit{};
    float u{}, v{};
    Vec3 tip{}, at{};
};
constexpr float pointReach = .6f, touchDepth = .025f;
Pointer point(const Frame&, float scale, const Pose& grip, unsigned hand);

// The panel's buttons and sliders, worked by the other hand's trigger. A
// pull that starts with the pointer on the panel is claimed (claims()) until
// the trigger is let go. Past Toggle::pressed on a control presses it: an
// arrow flips the page, a slider is held and follows the pointer along its
// track until let go, a VR SETTINGS row steps to its next choice (< on its
// left half back, > on its right half forward; a switch flips).
class Menu {
  public:
    static constexpr float stickDead = .25f, stickSpeed = .16f; // panel metres a second at full tilt
    struct Input {
        bool active{};   // the panel shown whole and in play, the trigger not the toggle's
        Pointer pointer; // the other hand's
        float trigger{}; // the other hand's, 0 untracked
        uint32_t disabled{};
        float stick{};   // the pointing hand's stick, up positive
        float seconds{}; // since the previous update
        float text = 1;  // the applied text factor
        uint32_t layout{}; // anything that sizes or moves the panel, as one key
        const vr_settings::Values* vr{}; // the VR SETTINGS page's values now
    };
    struct Result {
        bool pressed{}, hovered{};
        bool fpsReset{};
        uint32_t changed{}; // a bit (1 << Control) per setting changed in `Settings`
        bool stick{};       // the stick is the panel's this update
        bool scrolled{};
        // A VR SETTINGS row pressed: its item and the choice it steps to (vr_settings::choose).
        vr_settings::Item vrItem = vr_settings::Item::none;
        int vrChoice{};
        // The MUSIC page's buttons, and FNSM APP.
        enum class Music : uint8_t { none, previous, toggle, next, autoplay } music{};
        bool fnsm{};
        int tune = -1; // an AUTO TUNE button: its auto_tune::Mode
    };
    Result update(const Input&, Settings&);
    int page() const {
        return page_;
    }
    void setPage(int page) {
        page_ = ((page % pages) + pages) % pages;
    }
    Control hover() const {
        return hover_;
    }
    Control held() const {
        return held_;
    }
    bool claims() const {
        return claim_;
    }
    float scroll() const {
        return scroll_[static_cast<size_t>(page_)];
    }
    void setScroll(int page, float scroll, float text = 1);
    bool dragging() const {
        return drag_ != Drag::none;
    }

  private:
    enum class Drag : uint8_t { none, bar, content };
    int page_{};
    Control hover_{}, held_{};
    bool claim_{}, touched_{}, down_{};
    std::array<float, pages> scroll_{};
    Drag drag_{};
    float dragV_{}, dragScroll_{};
    uint32_t layout_{};
    bool layoutSeen_{};
};

// The glass the game's FNSM app is drawn on, over the left hand (phoneAppCorners'
// frame): a dark pane with the hologram's red edge, a little larger than the app.
void appendAppGlass(std::vector<Vertex>& out, const std::array<Vec3, 4>& corners, Vec3 viewer, float pixelAngle);
// Appends the light, its rays and the panel to `out` (the overlay's
// translucent list): nothing when it is hidden.
void appendWristHud(std::vector<Vertex>& out, const View&, Vec3 viewer, float pixelAngle);
// Whether vertices [from, end) are fit to draw: finite, within `radius` of
// `near`, and no more than maxVertices of them.
constexpr size_t maxVertices = 120000;
bool sane(const std::vector<Vertex>& out, size_t from, Vec3 near, float radius);
} // namespace spidy::wrist_hud

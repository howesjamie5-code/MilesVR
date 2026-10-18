// The game's button vocabulary, and the VR controls for it (prompt_buttons.hpp).
#include "spidy/prompt_buttons.hpp"
#include <cstring>

using namespace spidy;
using namespace spidy::prompt_buttons;

namespace {
constexpr Demand held(uint16_t bits) {
    Demand d;
    d.buttons = bits;
    return d;
}
constexpr Demand pulled(bool left, bool right) {
    Demand d;
    d.leftTrigger = left;
    d.rightTrigger = right;
    return d;
}
constexpr Demand pushed(bool right, int8_t x, int8_t y) {
    Demand d;
    if (right)
        d.rightX = x, d.rightY = y;
    else
        d.leftX = x, d.leftY = y;
    return d;
}
// A stick the prompt says to move without naming a way, and a prompt that
// named the keyboard or the mouse rather than the pad.
constexpr Demand moved(bool right) {
    Demand d;
    (right ? d.rightStickMotion : d.leftStickMotion) = true;
    return d;
}
constexpr Demand typed() {
    Demand d;
    d.keyboard = true;
    return d;
}
constexpr uint16_t shoulders = padLeftShoulder | padRightShoulder;
// The device families the UI has icons for. Longest first: PSDualShockButton
// must not be read as PSButton with the suffix "DualShockButtonOptions".
constexpr const char* families[] = {
    "PSDualShockButton", "PSDualSenseButton", "SwitchProButton", "GenericButton", "SteamButton",
    "XB360Button",       "XBOneButton",       "XBButton",        "PSButton",      "MkbKeyboard",
    "MkbMouse"};
// Whether a family names its face buttons by shape (PlayStation): its X is
// the pad's A, its Square the pad's X.
bool shapes(std::string_view family) {
    return family.starts_with("PS");
}
// Whether a family is no controller's (a keyboard key, a mouse button): its
// ids say nothing about the pad.
bool keyboard(std::string_view family) {
    return family.starts_with("Mkb");
}
struct Named {
    const char* name;
    Demand demand;
};
// What an id's last word asks of the pad, whatever family it came from.
constexpr Named endings[] = {
    {"A", held(padA)},
    {"ASmall", held(padA)},
    {"B", held(padB)},
    {"BSmall", held(padB)},
    {"X", held(padX)},
    {"Y", held(padY)},
    {"DpadUp", held(padDpadUp)},
    {"DpadDown", held(padDpadDown)},
    {"DpadLeft", held(padDpadLeft)},
    {"DpadRight", held(padDpadRight)},
    {"DpadLeftRight", held(padDpadLeft | padDpadRight)},
    {"StickLeftPress", held(padLeftThumb)},
    {"StickRightPress", held(padRightThumb)},
    {"LeftShoulder", held(padLeftShoulder)},
    {"LeftShoulderOutline", held(padLeftShoulder)},
    {"RightShoulder", held(padRightShoulder)},
    {"RightShoulderOutline", held(padRightShoulder)},
    {"BothShoulders", held(shoulders)},
    {"Start", held(padStart)},
    {"Menu", held(padStart)},
    {"Plus", held(padStart)},
    {"Options", held(padStart)},
    {"Back", held(padBack)},
    {"View", held(padBack)},
    {"Select", held(padBack)},
    {"Minus", held(padBack)},
    {"Touchpad", held(padBack)},
    {"LeftTrigger", pulled(true, false)},
    {"LeftTriggerSoftpull", pulled(true, false)},
    {"RightTrigger", pulled(false, true)},
    {"RightTriggerSoftpull", pulled(false, true)},
    {"BothTriggers", pulled(true, true)},
};
// The same for a PlayStation family, whose shapes come before the table above.
constexpr Named shapeEndings[] = {
    {"X", held(padA)},        {"XSmall", held(padA)},      {"Circle", held(padB)},
    {"CircleSmall", held(padB)}, {"Square", held(padX)},   {"Triangle", held(padY)},
};
// The ids that stand without a family: a stick pushed a way.
constexpr Named stickIds[] = {
    {"LeftStickUp", pushed(false, 0, 1)},    {"LeftStickDown", pushed(false, 0, -1)},
    {"LeftStickLeft", pushed(false, -1, 0)}, {"LeftStickRight", pushed(false, 1, 0)},
    {"RightStickUp", pushed(true, 0, 1)},    {"RightStickDown", pushed(true, 0, -1)},
    {"RightStickLeft", pushed(true, -1, 0)}, {"RightStickRight", pushed(true, 1, 0)},
};
// What a prompt text actually names its buttons with, found in the game on
// October 13: the markup token `[BTN_B]`, in a text that reached Flash as an
// argument of ShowText -
//   ShowText(7, 'When wall running horizontally, <span class='emphasis'>hold
//   </span> [BTN_B] to whip around a corner.', 1, 1, 1, 0)
// - not an icon id and not spelled letters. The whole vocabulary is in the
// supported Spider-Man.exe, and the exe also lays its QTE handles out in one
// table (image 0x3dcfce0 and up, "CineQTEShotController - Button Prompt
// Location Handles"): UNKNOWN, [BTN_A], [BTN_B], [BTN_R2], [BTN_R3],
// [BTN_L1], [BTN_L2], [BTN_L3], the four [DPAD_*], and [MKB_CUSTOM00] to
// [MKB_CUSTOM08]. [LS] and [RS] stand beside it for
// CineQTEShotControllerStickMotionAction, a QTE that asks for a stick to be
// moved. Those are exactly the prompts of the story beats and encounters
// that stopped players, so all of them are read here, not the [BTN_*] alone:
// the game uses both the PlayStation names for the bumpers and triggers (L1,
// R1, L2, R2) and the Xbox ones (LB, RB).
//
// The nine [MKB_CUSTOM*] name a key or a mouse button and ask nothing of the
// pad. They are taken all the same, so that a text carrying one is counted
// as the game prompting a device the player in a headset is not holding
// (game_prompts::Live::keyboardTexts) instead of passing for a text that
// named nothing.
//
// [BTN_CANCEL] and [BTN_MAP_WAYPOINT_SET] are deliberately absent: the first
// is whatever the screen it belongs to calls cancel (it stands beside
// HeroStateDialog and ConfirmRadioTower in the exe), the second a map
// action, and sending a guess for either would press the wrong thing.
constexpr Named tokens[] = {
    {"[BTN_A]", held(padA)},
    {"[BTN_B]", held(padB)},
    {"[BTN_X]", held(padX)},
    {"[BTN_Y]", held(padY)},
    {"[BTN_L1]", held(padLeftShoulder)},
    {"[BTN_R1]", held(padRightShoulder)},
    {"[BTN_LB]", held(padLeftShoulder)},
    {"[BTN_RB]", held(padRightShoulder)},
    {"[BTN_L2]", pulled(true, false)},
    {"[BTN_R2]", pulled(false, true)},
    {"[BTN_L3]", held(padLeftThumb)},
    {"[BTN_R3]", held(padRightThumb)},
    {"[DPAD_UP]", held(padDpadUp)},
    {"[DPAD_DOWN]", held(padDpadDown)},
    {"[DPAD_LEFT]", held(padDpadLeft)},
    {"[DPAD_RIGHT]", held(padDpadRight)},
    {"[LS]", moved(false)},
    {"[RS]", moved(true)},
    {"[MKB_CUSTOM00]", typed()},
    {"[MKB_CUSTOM01]", typed()},
    {"[MKB_CUSTOM02]", typed()},
    {"[MKB_CUSTOM03]", typed()},
    {"[MKB_CUSTOM04]", typed()},
    {"[MKB_CUSTOM05]", typed()},
    {"[MKB_CUSTOM06]", typed()},
    {"[MKB_CUSTOM07]", typed()},
    {"[MKB_CUSTOM08]", typed()},
};
// The letters a prompt text spells instead of an icon ("LB + RB (Hold)
// Throw"). Longest first, so a pair is one token and not two.
constexpr Named spellings[] = {
    {"LB + RB", held(shoulders)},   {"RB + LB", held(shoulders)},   {"L1 + R1", held(shoulders)},
    {"R1 + L1", held(shoulders)},   {"LB+RB", held(shoulders)},     {"L1+R1", held(shoulders)},
    {"LT + RT", pulled(true, true)}, {"RT + LT", pulled(true, true)}, {"L2 + R2", pulled(true, true)},
    {"R2 + L2", pulled(true, true)}, {"LT+RT", pulled(true, true)},  {"L2+R2", pulled(true, true)},
    {"LB", held(padLeftShoulder)},  {"RB", held(padRightShoulder)}, {"L1", held(padLeftShoulder)},
    {"R1", held(padRightShoulder)}, {"LT", pulled(true, false)},    {"RT", pulled(false, true)},
    {"L2", pulled(true, false)},    {"R2", pulled(false, true)},    {"L3", held(padLeftThumb)},
    {"R3", held(padRightThumb)},
};
// What the player does in VR for a demand. The gameplay controls first, then
// the pad layer's for what VR play cannot send on its own.
constexpr Named vrControls[] = {
    {"A", held(padA)},
    {"B", held(padB)},
    {"Y", held(padY)},
    {"MENU BUTTON", held(padStart)},
    {"X", held(padBack)},
    {"RIGHT STICK CLICK", held(shoulders)},
    {"RIGHT STICK UP", pulled(true, true)},
    {"RIGHT STICK DOWN", held(padDpadDown)},
    {"TRIGGER AT YOUR TEMPLE", held(padRightThumb)},
    {"LEFT STICK UP", pushed(false, 0, 1)},
    {"LEFT STICK DOWN", pushed(false, 0, -1)},
    {"LEFT STICK LEFT", pushed(false, -1, 0)},
    {"LEFT STICK RIGHT", pushed(false, 1, 0)},
    {"PAD + X", held(padX)},
    {"PAD + LEFT GRIP", held(padLeftShoulder)},
    {"PAD + RIGHT GRIP", held(padRightShoulder)},
    {"PAD + LEFT TRIGGER", pulled(true, false)},
    {"PAD + RIGHT TRIGGER", pulled(false, true)},
    {"PAD + LEFT STICK CLICK", held(padLeftThumb)},
    {"PAD + RIGHT STICK UP", held(padDpadUp)},
    {"PAD + RIGHT STICK LEFT", held(padDpadLeft)},
    {"PAD + RIGHT STICK RIGHT", held(padDpadRight)},
    {"PAD + RIGHT STICK LEFT OR RIGHT", held(padDpadLeft | padDpadRight)},
    {"PAD + RIGHT STICK UP", pushed(true, 0, 1)},
    {"PAD + RIGHT STICK DOWN", pushed(true, 0, -1)},
    {"PAD + RIGHT STICK LEFT", pushed(true, -1, 0)},
    {"PAD + RIGHT STICK RIGHT", pushed(true, 1, 0)},
    {"LEFT STICK", moved(false)},
    {"PAD + RIGHT STICK", moved(true)},
};
// Every control of the pad, so a check covers them all.
constexpr Control allControls[] = {
    {"A", held(padA)},
    {"B", held(padB)},
    {"X", held(padX)},
    {"Y", held(padY)},
    {"Start", held(padStart)},
    {"Back", held(padBack)},
    {"LB", held(padLeftShoulder)},
    {"RB", held(padRightShoulder)},
    {"LT", pulled(true, false)},
    {"RT", pulled(false, true)},
    {"L3", held(padLeftThumb)},
    {"R3", held(padRightThumb)},
    {"D-pad up", held(padDpadUp)},
    {"D-pad down", held(padDpadDown)},
    {"D-pad left", held(padDpadLeft)},
    {"D-pad right", held(padDpadRight)},
    {"LB + RB", held(shoulders)},
    {"LT + RT", pulled(true, true)},
};
// A character that can stand inside an icon id or a spelling.
bool wordChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
// `name` stands at `at` in `text` and nothing runs on past either end.
bool standsAt(std::string_view text, size_t at, std::string_view name) {
    if (text.compare(at, name.size(), name) != 0)
        return false;
    const size_t end = at + name.size();
    if (at && wordChar(text[at - 1]) && wordChar(name.front()))
        return false;
    return end >= text.size() || !wordChar(text[end]) || !wordChar(name.back());
}
// The longest token of the vocabulary that starts at `at`: its length, what
// it asks for, and whether the text spelled it out in letters rather than
// naming an icon id (only a spelling may be rewritten; an id sits inside the
// UI's markup). {0, {}} where no token does.
struct Token {
    size_t length{};
    Demand demand{};
    bool spelled{};
};
Token tokenAt(std::string_view text, size_t at) {
    Token best;
    const auto take = [&](size_t length, Demand demand, bool spelled = false) {
        if (length > best.length)
            best = {length, demand, spelled};
    };
    for (const auto& id : stickIds)
        if (standsAt(text, at, id.name))
            take(std::strlen(id.name), id.demand);
    for (const char* family : families) {
        const size_t n = std::strlen(family);
        if (text.compare(at, n, family) != 0)
            continue;
        if (at && wordChar(text[at - 1]))
            continue;
        if (keyboard(family)) {
            // A keyboard key or a mouse button: a token of the vocabulary,
            // asking nothing of the pad. Taken so a rewrite leaves it alone.
            size_t end = at + n;
            while (end < text.size() && wordChar(text[end]))
                ++end;
            take(end - at, typed());
            continue;
        }
        const std::string_view rest = text.substr(at + n);
        size_t word = 0;
        while (word < rest.size() && wordChar(rest[word]))
            ++word;
        const std::string_view ending = rest.substr(0, word);
        bool found = false;
        if (shapes(family))
            for (const auto& e : shapeEndings)
                if (ending == e.name)
                    take(n + ending.size(), e.demand), found = true;
        if (!found)
            for (const auto& e : endings)
                if (ending == e.name)
                    take(n + ending.size(), e.demand), found = true;
        // A family's id whose ending is none of these (its stick with no way,
        // a back grip, a touchpad swipe) asks nothing, and stays a token.
        if (!found && word)
            take(n + ending.size(), {});
    }
    for (const auto& s : spellings)
        if (standsAt(text, at, s.name))
            take(std::strlen(s.name), s.demand, true);
    // The game's own markup token. Its brackets are no word characters, so
    // standsAt's boundaries do not apply: it is matched as it stands.
    for (const auto& t : tokens)
        if (!text.compare(at, std::strlen(t.name), t.name))
            take(std::strlen(t.name), t.demand, true);
    return best;
}
// The text says to hold the button rather than tap it.
bool saysHold(std::string_view text) {
    for (size_t i = 0; i + 4 <= text.size(); ++i) {
        const char* hold = "hold";
        bool same = true;
        for (size_t j = 0; j < 4 && same; ++j) {
            char c = text[i + j];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            same = c == hold[j];
        }
        if (same && (!i || !wordChar(text[i - 1])) && (i + 4 >= text.size() || !wordChar(text[i + 4])))
            return true;
    }
    return false;
}
// Worth a scan at all: the vocabulary's own words, or a spelling's letters.
bool mayName(std::string_view text) {
    if (text.find("[BTN_") != std::string_view::npos || text.find("Button") != std::string_view::npos ||
        text.find("Stick") != std::string_view::npos || text.find("Mkb") != std::string_view::npos ||
        text.find("[DPAD_") != std::string_view::npos || text.find("[MKB_") != std::string_view::npos ||
        text.find("[LS]") != std::string_view::npos || text.find("[RS]") != std::string_view::npos)
        return true;
    for (const auto& s : spellings)
        if (text.find(s.name) != std::string_view::npos)
            return true;
    return false;
}
} // namespace

Demand prompt_buttons::forIcon(std::string_view id) {
    if (id.empty())
        return {};
    const auto token = tokenAt(id, 0);
    // The whole id, not a word of it: "XBButtonAway" is no XBButtonA.
    return token.length == id.size() ? token.demand : Demand{};
}

Demand prompt_buttons::forText(std::string_view text) {
    Demand out;
    if (!mayName(text))
        return out;
    for (size_t i = 0; i < text.size();) {
        const auto token = tokenAt(text, i);
        if (!token.length) {
            ++i;
            continue;
        }
        out |= token.demand;
        i += token.length;
    }
    if (out.any())
        out.hold = saysHold(text);
    return out;
}

bool prompt_buttons::reachableInPlay(Demand d) {
    if (!d.any())
        return true;
    // The right stick turns the player and steers the game's own camera in VR
    // play; only the pad layer passes it to the game. The left stick is the
    // player's own walking, so a QTE that asks for it to be moved is already
    // his to answer.
    if (d.rightX || d.rightY || d.rightStickMotion)
        return false;
    // Each bumper and each trigger reaches the game only with its pair: the
    // right stick's click sends LB + RB, that stick pushed up sends LT + RT.
    if (d.leftTrigger != d.rightTrigger)
        return false;
    if ((d.buttons & shoulders) && (d.buttons & shoulders) != shoulders)
        return false;
    constexpr uint16_t play =
        padA | padB | padY | padStart | padBack | padDpadDown | padRightThumb | shoulders;
    return (d.buttons & ~play) == 0;
}

const char* prompt_buttons::vrControl(Demand d) {
    if (!d.any())
        return nullptr;
    d.hold = false;
    for (const auto& c : vrControls)
        if (c.demand == d)
            return c.name;
    return nullptr;
}

size_t prompt_buttons::rewrite(std::string_view text, char* out, size_t capacity, bool interactServes) {
    if (!out || capacity < 2 || !mayName(text))
        return 0;
    size_t n = 0;
    bool any = false;
    const auto put = [&](std::string_view s) {
        const size_t room = capacity - 1 - n;
        const size_t take = s.size() < room ? s.size() : room;
        std::memcpy(out + n, s.data(), take);
        n += take;
        return take == s.size();
    };
    for (size_t i = 0; i < text.size();) {
        const auto token = tokenAt(text, i);
        // An icon id stays as it is, whatever it asks for: see the header.
        const char* control = token.spelled ? vrControl(token.demand) : nullptr;
        // Where VR play has no control of its own, the interact button is the
        // answer and the shortest thing to tell the player.
        if (control && interactServes && !reachableInPlay(token.demand))
            control = "Y";
        if (!control) {
            // A token of the vocabulary that is not rewritten goes out whole,
            // so nothing inside it is read again as a spelling.
            const size_t run = token.length ? token.length : 1;
            if (!put(text.substr(i, run)))
                break;
            i += run;
            continue;
        }
        if (!put(control))
            break;
        any = true;
        i += token.length;
    }
    out[n] = 0;
    return any ? n : 0;
}

uint32_t prompt_buttons::pack(Demand d) {
    const auto way = [](int8_t v) { return static_cast<uint32_t>(v < 0 ? 1 : v > 0 ? 2 : 0); };
    return d.buttons | (d.leftTrigger ? 1u << 16 : 0) | (d.rightTrigger ? 1u << 17 : 0) |
           (d.hold ? 1u << 18 : 0) | (way(d.leftX) << 19) | (way(d.leftY) << 21) | (way(d.rightX) << 23) |
           (way(d.rightY) << 25) | (d.leftStickMotion ? 1u << 27 : 0) | (d.rightStickMotion ? 1u << 28 : 0) |
           (d.keyboard ? 1u << 29 : 0);
}

Demand prompt_buttons::unpack(uint32_t word) {
    const auto way = [&](unsigned shift) {
        const unsigned v = (word >> shift) & 3;
        return static_cast<int8_t>(v == 1 ? -1 : v == 2 ? 1 : 0);
    };
    Demand d;
    d.buttons = static_cast<uint16_t>(word & 0xffff);
    d.leftTrigger = (word >> 16) & 1;
    d.rightTrigger = (word >> 17) & 1;
    d.hold = (word >> 18) & 1;
    d.leftX = way(19);
    d.leftY = way(21);
    d.rightX = way(23);
    d.rightY = way(25);
    d.leftStickMotion = (word >> 27) & 1;
    d.rightStickMotion = (word >> 28) & 1;
    d.keyboard = (word >> 29) & 1;
    return d;
}

std::span<const Control> prompt_buttons::controls() {
    return allControls;
}

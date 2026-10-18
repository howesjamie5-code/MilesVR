#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// The game's own button vocabulary, and the VR controls that answer it.
//
// Spidy's trouble with the game's prompts was never one prompt: VR play can
// send nine of the pad's controls and the game picks the one it asks for from
// all of them, so every story beat or encounter that wanted another one
// stopped a player ("Can't pull objects with LB + RB", "cant use Vents",
// "it says tap x repeat but tapping x opens the menu", Air Launch, the Fisk
// door, the Sable turrets). Each was patched on its own: the right stick's
// click for the game's LB + RB on October 11, that stick's pushes for its
// heal and its LT + RT on October 12.
//
// The game, though, names the button it wants, in a vocabulary it owns and
// that does not change with the moment: 203 icon ids (image offset 0x3a80300
// and up) with one icon map a device family in the UI's boot config
// (X360IconMap, XOneIconMap, DualShockIconMap, DualSenseIconMap,
// SteamDeckIconMap, SwitchProIconMap, GenericIconMap, MkbIconMap,
// SteamIconMap), and YUIIconMarkupConfig turning a text's markup into them.
// Every family's ids end in the same words - A, B, X, Y, DpadUp,
// StickRightPress, LeftShoulder, BothShoulders, BothTriggers - so one table
// over those endings answers for all nine families at once, and the letters
// the texts spell out ("LB + RB (Hold) Throw", the hint line of the Shocker
// fight) go in the same table. Nothing here is a list of the game's moments:
// a prompt says its button, this says what that asks of the pad and what the
// player does in VR instead.
namespace spidy::prompt_buttons {
// XInput's button bits, the ones game_pad::Button holds (game_pad.hpp
// asserts that they agree).
constexpr uint16_t padDpadUp = 0x1, padDpadDown = 0x2, padDpadLeft = 0x4, padDpadRight = 0x8, padStart = 0x10,
                   padBack = 0x20, padLeftThumb = 0x40, padRightThumb = 0x80, padLeftShoulder = 0x100,
                   padRightShoulder = 0x200, padA = 0x1000, padB = 0x2000, padX = 0x4000, padY = 0x8000;
// What a prompt asks of the controller: the buttons held together, each
// trigger pulled, and a stick pushed (-1, 0 or 1 an axis, y up). `hold`: its
// own word says to hold it rather than tap it.
struct Demand {
    uint16_t buttons{};
    bool leftTrigger{}, rightTrigger{}, hold{};
    int8_t leftX{}, leftY{}, rightX{}, rightY{};
    // A stick to be moved with no way named ([LS] and [RS], the QTE's stick
    // motion), and a prompt that named a key or a mouse button instead of a
    // pad control ([MKB_CUSTOM00] and up, the MkbKeyboard and MkbMouse
    // icons). The game prompts for the device it last saw, so a VR player
    // whose keyboard spoke last reads a letter with nothing to press.
    bool leftStickMotion{}, rightStickMotion{}, keyboard{};
    bool operator==(const Demand&) const = default;
    bool any() const {
        return buttons || leftTrigger || rightTrigger || leftX || leftY || rightX || rightY ||
               leftStickMotion || rightStickMotion;
    }
    Demand& operator|=(const Demand& o) {
        buttons |= o.buttons;
        leftTrigger |= o.leftTrigger;
        rightTrigger |= o.rightTrigger;
        hold |= o.hold;
        // A way already asked for stands: two prompts in one text do not cancel.
        leftX = leftX ? leftX : o.leftX;
        leftY = leftY ? leftY : o.leftY;
        rightX = rightX ? rightX : o.rightX;
        rightY = rightY ? rightY : o.rightY;
        leftStickMotion |= o.leftStickMotion;
        rightStickMotion |= o.rightStickMotion;
        keyboard |= o.keyboard;
        return *this;
    }
};
// One icon id of any family ("XBButtonBothShoulders", "PSButtonCircle",
// "SteamButtonRightTrigger", "RightStickUp"), with its family's prefix or
// without it: what it asks of the pad. An id that asks nothing of a
// controller - a gadget, an emoji, a stick with no way - gives {}; a
// keyboard key or a mouse button gives a demand with `keyboard` set and
// nothing to press. The PlayStation families name the face buttons by shape,
// so their X is the pad's A and their Square its X.
Demand forIcon(std::string_view id);
// Every button a prompt text names, together: the icon ids in its markup and
// the letters it spells ("LB + RB", "L1 + R1", "LT + RT", "R3"), with `hold`
// from its own word. A text that names none gives {}.
Demand forText(std::string_view text);
// Whether VR play sends `demand` as it stands (game_pad::Mapping::gameplay),
// with no pad layer. The baseline is Touch and Index; a Steam Frame has the
// game's own D-pad and View besides.
bool reachableInPlay(Demand);
// What the player does in VR for it, in capitals for a prompt line: the
// gameplay control where there is one ("RIGHT STICK CLICK"), else the pad
// layer's ("PAD + LEFT GRIP"). Null for {} and for a mix of buttons no one
// control gives.
const char* vrControl(Demand);
// `text` with the buttons it spells out replaced by the VR controls for
// them: "LB + RB (Hold) Throw" becomes "RIGHT STICK CLICK (Hold) Throw".
// `interactServes` (the interact button sends what a live prompt asks for,
// game_pad::ContextPress) puts "Y" where VR play has no control of its own,
// so "Tap LB" reads "Tap Y" rather than naming the pad layer.
//
// An icon id is left exactly as it stands. The ids sit inside the UI's own
// markup, whose form is not known (YUIIconMarkupConfig, with MarkupText,
// IconMapping and a Separator), and a name changed inside it would be markup
// the game cannot resolve. Reading one is safe and is what feeds the interact
// button; rewriting one is not.
//
// Returns the length written; 0 where nothing was replaced, and `out` is then
// untouched.
size_t rewrite(std::string_view text, char* out, size_t capacity, bool interactServes);
// A demand in one word, so a hook can hand it to the VR thread without a
// lock and a session report can carry it: bits 0-15 the buttons, 16 the left
// trigger, 17 the right, 18 the hold, then two bits a stick axis (0 no way,
// 1 back or left, 2 forward or right) through bit 26, then 27 and 28 a
// stick to be moved any way and 29 a key or a mouse button. An empty demand
// packs to 0. Round trips through unpack.
uint32_t pack(Demand);
Demand unpack(uint32_t);
// Every control of the pad the game can ask for, so a check can cover them
// all instead of the ones a player has reported.
struct Control {
    const char* name;
    Demand demand;
};
std::span<const Control> controls();
} // namespace spidy::prompt_buttons

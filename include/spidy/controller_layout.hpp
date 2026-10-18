#pragma once

#include <cstdint>
#include <span>

// The VR controllers Spidy describes its controls for (OpenXR suggested
// bindings, XrRuntime), each with the buttons it has.
//
// A runtime that gets no layout for the controllers in the player's hands
// translates one of another's. SteamVR does that for the Steam Frame's from
// the Touch layout (its frame_controller_remapping.json, and Valve's "Steam
// Frame Input" page: "The top button of the Oculus Touch controllers (B / Y)
// will map to all three top buttons of the Steam Frame Controllers (Dpad
// Left, Dpad Up, Dpad Right / X, Y, B)"). So until October 12 the Frame's B,
// X and Y were all Spidy's B, the dodge ("X is registering as dodge on Steam
// Frame Controllers"), three ways of its D-pad were Y and the fourth X. The
// Frame has a gamepad's buttons, split between the hands: with a layout of
// its own each is the button its name says.
namespace spidy {
// What a control of the controllers is to Spidy: one action of XrRuntime's.
enum class ControllerInput : uint8_t {
    // One for each hand.
    aim,
    grip,
    trigger,
    squeeze,
    stick,
    stickClick,
    haptic,
    // XrFrame::buttons.
    a,
    b,
    x,
    y,
    menu,
    // The Steam Frame's own (frameOnly).
    view,
    dpadUp,
    dpadDown,
    dpadLeft,
    dpadRight,
};
// Controls only the Steam Frame has. XrRuntime makes their actions only where
// the runtime knows that controller: every other runtime gets the actions it
// got before.
constexpr bool frameOnly(ControllerInput input) {
    return input >= ControllerInput::view;
}
// The layout the runtime uses for a hand's controller (XrData::controllers).
// `other`: one of the runtime's own, with none of Spidy's.
enum class ControllerKind : uint32_t { none, touch, index, frame, other };
constexpr const char* controllerName(ControllerKind kind) {
    switch (kind) {
    case ControllerKind::touch:
        return "Touch";
    case ControllerKind::index:
        return "Index";
    case ControllerKind::frame:
        return "Steam Frame";
    case ControllerKind::other:
        return "the runtime's own";
    default:
        return "none";
    }
}
struct ControllerBinding {
    ControllerInput input;
    const char* path;
};
struct ControllerLayout {
    ControllerKind kind;
    // The OpenXR interaction profile, and the extension that brings it (null:
    // every runtime takes bindings for it).
    const char* profile;
    const char* extension;
    std::span<const ControllerBinding> bindings;
};
namespace controller_layout {
using enum ControllerInput;
// Touch: A and B on the right, X, Y and the menu button on the left.
inline constexpr ControllerBinding touch[] = {
    {aim, "/user/hand/left/input/aim/pose"},
    {grip, "/user/hand/left/input/grip/pose"},
    {trigger, "/user/hand/left/input/trigger/value"},
    {squeeze, "/user/hand/left/input/squeeze/value"},
    {stick, "/user/hand/left/input/thumbstick"},
    {stickClick, "/user/hand/left/input/thumbstick/click"},
    {haptic, "/user/hand/left/output/haptic"},
    {aim, "/user/hand/right/input/aim/pose"},
    {grip, "/user/hand/right/input/grip/pose"},
    {trigger, "/user/hand/right/input/trigger/value"},
    {squeeze, "/user/hand/right/input/squeeze/value"},
    {stick, "/user/hand/right/input/thumbstick"},
    {stickClick, "/user/hand/right/input/thumbstick/click"},
    {haptic, "/user/hand/right/output/haptic"},
    {a, "/user/hand/right/input/a/click"},
    {y, "/user/hand/left/input/y/click"},
    {b, "/user/hand/right/input/b/click"},
    {x, "/user/hand/left/input/x/click"},
    {menu, "/user/hand/left/input/menu/click"},
};
// Index: A and B on each hand, the left ones Spidy's X and Y. No menu
// button: their system button is the runtime's.
inline constexpr ControllerBinding index[] = {
    {aim, "/user/hand/left/input/aim/pose"},
    {grip, "/user/hand/left/input/grip/pose"},
    {trigger, "/user/hand/left/input/trigger/value"},
    {squeeze, "/user/hand/left/input/squeeze/force"},
    {stick, "/user/hand/left/input/thumbstick"},
    {stickClick, "/user/hand/left/input/thumbstick/click"},
    {haptic, "/user/hand/left/output/haptic"},
    {aim, "/user/hand/right/input/aim/pose"},
    {grip, "/user/hand/right/input/grip/pose"},
    {trigger, "/user/hand/right/input/trigger/value"},
    {squeeze, "/user/hand/right/input/squeeze/force"},
    {stick, "/user/hand/right/input/thumbstick"},
    {stickClick, "/user/hand/right/input/thumbstick/click"},
    {haptic, "/user/hand/right/output/haptic"},
    {a, "/user/hand/right/input/a/click"},
    {y, "/user/hand/left/input/b/click"},
    {b, "/user/hand/right/input/b/click"},
    {x, "/user/hand/left/input/a/click"},
};
// Steam Frame: A, B, X, Y and Menu on the right, the D-pad and View on the
// left. Its bumpers have no name the sources agree on (Valve's page says
// /input/bumper, Godot's layout /input/shoulder) and a path the runtime does
// not know loses the whole layout, so they are left out: the triggers beside
// them do what SteamVR's translation had the bumpers do.
inline constexpr ControllerBinding frame[] = {
    {aim, "/user/hand/left/input/aim/pose"},
    {grip, "/user/hand/left/input/grip/pose"},
    {trigger, "/user/hand/left/input/trigger/value"},
    {squeeze, "/user/hand/left/input/squeeze/value"},
    {stick, "/user/hand/left/input/thumbstick"},
    {stickClick, "/user/hand/left/input/thumbstick/click"},
    {haptic, "/user/hand/left/output/haptic"},
    {aim, "/user/hand/right/input/aim/pose"},
    {grip, "/user/hand/right/input/grip/pose"},
    {trigger, "/user/hand/right/input/trigger/value"},
    {squeeze, "/user/hand/right/input/squeeze/value"},
    {stick, "/user/hand/right/input/thumbstick"},
    {stickClick, "/user/hand/right/input/thumbstick/click"},
    {haptic, "/user/hand/right/output/haptic"},
    {a, "/user/hand/right/input/a/click"},
    {y, "/user/hand/right/input/y/click"},
    {b, "/user/hand/right/input/b/click"},
    {x, "/user/hand/right/input/x/click"},
    {menu, "/user/hand/right/input/menu/click"},
    {view, "/user/hand/left/input/view/click"},
    {dpadUp, "/user/hand/left/input/dpad_up/click"},
    {dpadDown, "/user/hand/left/input/dpad_down/click"},
    {dpadLeft, "/user/hand/left/input/dpad_left/click"},
    {dpadRight, "/user/hand/left/input/dpad_right/click"},
};
inline constexpr ControllerLayout all[] = {
    {ControllerKind::touch, "/interaction_profiles/oculus/touch_controller", nullptr, touch},
    {ControllerKind::index, "/interaction_profiles/valve/index_controller", nullptr, index},
    {ControllerKind::frame, "/interaction_profiles/valve/frame_controller_valve",
     "XR_VALVE_frame_controller_interaction", frame},
};
} // namespace controller_layout
constexpr std::span<const ControllerLayout> controllerLayouts() {
    return controller_layout::all;
}
} // namespace spidy

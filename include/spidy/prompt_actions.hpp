#pragma once
// MilesVR 0.1.78: the game's texts as a VR player reads them (the user: "Redesign every In game prompt
// message in game with our new controls so the game says venom punch = l1 + sqaure we would need to change
// that to match the vr version").
//
// The game never writes a button into a text. Its prompts, tips and tutorials name ACTIONS -
// "[ACTION_WEB_YANK] (Hold) Throw", "[ACTION_WEB_STRIKE] Open Door", "Use Venom Dash ... by pressing
// [ACTION_VENOM] + [ACTION_WEB_STRIKE]" - and its UI turns each into the device's icon at the very end (the
// texts measured on October 20 through the localized lookup 1c55d40 and the markup resolver 9778f0). So
// each action becomes here the VR control MilesVR maps to its button (game_pad, the headset's "VR active"
// line), and a pair of them the VR gesture that pair is (the venom moves). An action not in the table is
// left for the game's own icon, and named back (unknown) so it can be added.
#include <string>
#include <string_view>
#include <vector>

namespace spidy::prompt_actions {

// The player's buttons (the launcher's WEB BUTTON and VENOM BUTTON, always the two different ones).
struct Controls {
    bool triggerWebs{};  // webs on the trigger (else the grip)
    bool venomTrigger{}; // venom on the trigger (else the grip)
};

// The text with every known action (or button) spelt as its VR control; `unknown` gets each action left as it was.
inline std::string rewrite(std::string_view text, const Controls& c, std::vector<std::string>* unknown = nullptr) {
    const std::string web = c.triggerWebs ? "TRIGGER" : "GRIP", webs = c.triggerWebs ? "TRIGGERS" : "GRIPS";
    const std::string venom = std::string("VENOM ") + (c.venomTrigger ? "TRIGGER" : "GRIP");
    // Pairs first: a venom move is one gesture, not two buttons.
    // The game's attack is [ACTION_MELEE] (the October 21 session's venom punch tutorial).
    const std::pair<std::string_view, std::string> rules[] = {
        {"[ACTION_VENOM] + [ACTION_WEB_STRIKE]", venom + " + PUNCH"},
        {"[ACTION_VENOM] + [ACTION_MELEE]", "HOLD " + venom + " + PUNCH"},
        {"[ACTION_VENOM] + [ACTION_ATTACK]", "HOLD " + venom + " + PUNCH"},
        {"[ACTION_VENOM] + [ACTION_JUMP]", "HOLD " + venom + " + PUNCH UP"},
        {"[ACTION_VENOM] + [ACTION_DODGE]", "HOLD " + venom + " + PUNCH DOWN"},
        {"[ACTION_VENOM]", venom},
        {"[ACTION_MELEE]", "PUNCH"},
        {"[ACTION_ATTACK]", "PUNCH"},
        {"[ACTION_ACTIVE_FINISHER]", "Y + B"},
        {"[ACTION_GADGET]", c.venomTrigger ? "TRIGGER" : "GRIP"}, // the other button with no web: web ball or gadget
        {"[ACTION_PING]", "RIGHT HAND TO TEMPLE + TRIGGER"},
        {"[ACTION_WEBZIP]", "A"},
        {"[ACTION_FALL_DIVE]", "B"},
        {"[ACTION_SPRINT]", "LEFT STICK (ALL THE WAY)"},
        {"[ACTION_JUMP]", "A"},
        {"[ACTION_DODGE]", "B"},
        {"[ACTION_WEB_STRIKE]", "Y"},
        {"[ACTION_WEB_YANK_BOT]", "BOTH " + webs},
        {"[ACTION_WEB_YANK]", "RIGHT STICK UP"}, // a throw: the stick up at a marker (game_xr)
        {"[ACTION_SWING]", web},
        {"[ACTION_LS]", "LEFT STICK"},
        {"[ACTION_RS]", "RIGHT STICK"},
        {"[ACTION_ZIP]", "RIGHT STICK UP"},
        {"[ACTION_ZIP_TO_POINT]", "RIGHT STICK UP"},
        {"[ACTION_HEAL]", "RIGHT STICK DOWN"},
        {"[ACTION_HEALTH]", "RIGHT STICK DOWN"},
        {"[ACTION_CAMOUFLAGE]", "LEFT HAND TO TEMPLE + TRIGGER"},
        {"[ACTION_CAMO]", "LEFT HAND TO TEMPLE + TRIGGER"},
        {"[ACTION_INVISIBILITY]", "LEFT HAND TO TEMPLE + TRIGGER"}, // Miles's camouflage
        {"[ACTION_MELEEANDJUMP]", "UPPERCUT"},
        {"[ACTION_AIM]", "HOLD RIGHT STICK UP"},
        {"[ACTION_MAP]", "X"},
        {"[ACTION_PAUSE]", "MENU"},
        // The game's own buttons, which its in-world prompts name instead of actions ("[BTN_Y] Enter",
        // the throw's LB + RB; October 21). Both its PlayStation and Xbox names; pairs first.
        {"[BTN_LB] + [BTN_RB]", "RIGHT STICK UP"},
        {"[BTN_LB]+[BTN_RB]", "RIGHT STICK UP"},
        {"[BTN_L1] + [BTN_R1]", "RIGHT STICK UP"},
        {"[BTN_L1]+[BTN_R1]", "RIGHT STICK UP"},
        {"[BTN_L2] + [BTN_R2]", "RIGHT STICK UP"},
        {"[BTN_L2]+[BTN_R2]", "RIGHT STICK UP"},
        {"[BTN_LB] + [BTN_X]", "HOLD " + venom + " + PUNCH"},
        {"[BTN_L1] + [BTN_X]", "HOLD " + venom + " + PUNCH"},
        {"[BTN_A]", "A"},
        {"[BTN_B]", "B"},
        {"[BTN_X]", "PUNCH"},
        {"[BTN_Y]", "Y"},
        {"[BTN_LB]", venom},
        {"[BTN_L1]", venom},
        {"[BTN_RB]", c.venomTrigger ? "TRIGGER" : "GRIP"}, // the web ball's button
        {"[BTN_R1]", c.venomTrigger ? "TRIGGER" : "GRIP"},
        {"[BTN_R2]", web},
        {"[BTN_L3]", "CLICK LEFT STICK"},
        {"[BTN_R3]", "RIGHT HAND TO TEMPLE + TRIGGER"},
        {"[DPAD_UP]", "LEFT HAND TO TEMPLE + TRIGGER"},
        {"[DPAD_DOWN]", "RIGHT STICK DOWN"},
        {"[LS]", "LEFT STICK"},
        {"[RS]", "RIGHT STICK"},
    };
    std::string out;
    out.reserve(text.size() + 16);
    size_t at = 0;
    while (at < text.size()) {
        const size_t open = text.find('[', at);
        if (open == std::string_view::npos) {
            out.append(text.substr(at));
            break;
        }
        out.append(text.substr(at, open - at));
        bool done = false;
        for (const auto& [find, replace] : rules)
            if (text.substr(open, find.size()) == find) {
                out += replace;
                at = open + find.size();
                done = true;
                break;
            }
        if (done)
            continue;
        const auto tag = text.substr(open);
        if (!tag.starts_with("[ACTION_") && !tag.starts_with("[BTN_") && !tag.starts_with("[DPAD_")) {
            out += '[';
            at = open + 1;
            continue;
        }
        const size_t close = text.find(']', open);
        const size_t end = close == std::string_view::npos ? text.size() : close + 1;
        if (unknown)
            unknown->emplace_back(text.substr(open, end - open));
        out.append(text.substr(open, end - open));
        at = end;
    }
    return out;
}

} // namespace spidy::prompt_actions

#pragma once
#include "math.hpp"
#include "ui_style.hpp"
#include "vertex.hpp"
#include <array>
#include <cstdint>
#include <vector>

// Spidy's gadget wheel, as Blade & Sorcery's spell wheel (the user, October
// 15: "something like the blade and sorcery spell selection - wheel shows im
// front of arm while holding button, motion with arm selects and releasing
// sets seleciton"). Holding the wheel button opens a ring of the player's
// gadgets in front of that hand, each where the game's own wheel has it;
// moving the hand toward one lights it, and letting go equips it in the game,
// whose gadget the free hand's trigger then fires. Let go with none lit (the
// hand back near the middle) and nothing changes. A press shorter than
// holdSeconds is the button's own.
namespace spidy {
// The game's gadgets, by their weapons' classes (WeaponWebShooter,
// WeaponWebBlast - the Impact Web -, ...). The last is the game's charged
// Web Shooter gadget (WeaponImpactWeb), which the wheel does not list.
// MilesVR: Miles's own three after them, his game's own classes (none of
// them in Spider-Man.exe): the Remote Mine (WeaponRemoteTrap), the Gravity
// Well (WeaponPeopleSucker) and the Holo Drone (WeaponHoloAllyBot). With the
// web shooter at the top they are Miles's whole wheel.
enum class Gadget : uint8_t {
    none,
    webShooter,
    impactWeb,
    webBomb,
    electricWeb,
    tripMine,
    concussionBlast,
    spiderDrone,
    suspensionMatrix,
    gameWebShooter,
    remoteMine,
    gravityWell,
    holoDrone,
};
// The name the wheel shows, in capitals ("" for none).
const char* gadgetName(Gadget);
// The game's wheel has eight places, the first at the top, the rest round
// it clockwise as the player sees it.
constexpr int gadgetPlaces = 8;
// The way to place `p` from the wheel's middle: radians clockwise from the
// top as the player sees it (along right * sin, up * cos).
inline float gadgetPlaceAngle(int p) {
    return 2 * 3.14159265358979f * static_cast<float>(p) / gadgetPlaces;
}
// The wheel's plane: its middle, x to the player's right, y up, z toward
// the player.
struct WheelFrame {
    Vec3 centre{}, right{1, 0, 0}, up{0, 1, 0}, out{0, 0, 1};
};
class GadgetWheel {
  public:
    // A press this long opens the wheel; the hand this far from where it
    // opened lights a gadget, and back within `release` of it none.
    static constexpr float holdSeconds = .3f, pick = .06f, release = .04f;
    struct Event {
        bool tap{};      // a short press: the button's own
        bool opened{};   // the wheel opened
        bool lit{};      // another gadget lit (a tick in the hand)
        int chosen = -1; // let go with this place lit (0..7); -1: none
    };
    // seconds: since the previous update. held: the wheel button. allowed:
    // VR play; a wheel open as it ends closes choosing nothing, and a press
    // held through it does nothing until it is let go. hand: the wheel
    // hand's grip, head: the headset, in one space (tracking space keeps the
    // wheel with the player as webs carry him). places: a bit per place with
    // a gadget.
    Event update(float seconds, bool held, bool allowed, Vec3 hand, Vec3 head, uint8_t places);
    bool open() const {
        return open_;
    }
    const WheelFrame& frame() const {
        return frame_;
    }
    int lit() const {
        return lit_;
    }
    // Seconds since it opened, and since the gadget lit now (or none) lit.
    float age() const {
        return age_;
    }
    float litAge() const {
        return litAge_;
    }

  private:
    bool down_{}, open_{}, spent_{};
    float held_{}, age_{}, litAge_{};
    int lit_ = -1;
    WheelFrame frame_{};
};
// The wheel as an image shows it (NativeEyeFrame), in the world.
struct GadgetWheelView {
    bool open{};
    WheelFrame frame{};
    float age{}, litAge{};
    int lit = -1, equipped = -1;
    std::array<Gadget, gadgetPlaces> places{}; // none: an empty place
    // Each gadget's charges and the most it holds (most 0: none to count).
    std::array<uint8_t, gadgetPlaces> ammo{}, most{};
};
// The wheel printed as the combat marks are (web_visual.hpp): each gadget a
// paper badge keyed in ink with its picture, on a ring with a bead where a
// place is empty; the equipped one's badge yellow, one out of charges grey,
// its charges pips beside it (yellow left, ink spent). The lit one is red
// and larger, its picture paper, and its name lettered in a yellow caption
// across the middle. The wheel lands in three held poses as it opens, and
// so do the lit badge and its caption each time another gadget lights; the
// lit badge then boils, as a held drawing does. viewer: the eyes' middle;
// pixelAngle as for appendWeb. UiStyle::tech draws tech_marks.hpp's wheel.
void appendGadgetWheel(std::vector<Vertex>& out, const GadgetWheelView& view, Vec3 viewer, float pixelAngle,
                       UiStyle style = UiStyle::comic);
} // namespace spidy

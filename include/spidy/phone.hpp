#pragma once
#include "gadget_wheel.hpp"
#include "math.hpp"
#include "tracking.hpp"
#include "vertex.hpp"
#include <cstdint>
#include <string>
#include <vector>

// Spidy's phone (the player, October 17: "an overlay of something that looks
// like a flat phone screen ... I can start stop go back a track and auto play
// music"). Drawn from a back pocket, it opens in the hand that drew it
// (Phone). The stick picks a button, A presses it, B puts the phone away. The
// music goes on with the phone away. Songs are the files in the Music folder
// beside the launcher (phone_music.hpp).
namespace spidy {
enum class PhoneButton : int { previous, play, next, autoplay };
constexpr int phoneButtons = 4;
// The phone's screens: its home with an app for each of the others (the
// player, October 17: "a home screen with apps on it ... look more like the
// fnsm app from the 2nd game").
enum class PhoneScreen : int { home, music, map, gear };
constexpr int phoneApps = 3; // music, map, gear, in that order on the home screen
// The controllers while the phone is out, as steps: the stick pushed one way
// (x: -1 left, 1 right; y: -1 down, 1 up), A, B, and the volume from the
// triggers, a pull of the left one a step down and of the right one a step up
// (the player, October 17: "left trigger for down and right trigger for up").
// Each counts once until let go. Each opening counts everything as held, so
// the press that opened the phone does nothing.
class PhoneControls {
  public:
    static constexpr float pulled = .6f, released = .3f, pushed = .6f, centred = .3f;
    struct Event {
        int x{}, y{};
        bool pressed{}; // A
        bool closed{};  // B
        int volume{};   // -1 a step down, 1 a step up
    };
    // stick: -1 to 1 each way. a, b: held now. triggers: 0 to 1.
    Event update(float stickX, float stickY, bool a, bool b, float leftTrigger = 0, float rightTrigger = 0);
    void reset() {
        aHeld_ = bHeld_ = stickHeld_ = leftHeld_ = rightHeld_ = true;
    }

  private:
    bool aHeld_ = true, bHeld_ = true, stickHeld_ = true, leftHeld_ = true, rightHeld_ = true;
};
// What the phone's screens make of those steps. On the home screen the stick
// picks an app and A opens it, B puts the phone away; in an app B goes home.
// In MUSIC left and right pick a button and A presses it; in GEAR up and down
// scroll the list.
enum class PhoneAction : int { none, close, previous, play, next, autoplay };
class PhoneUi {
  public:
    // Rows GEAR shows at once.
    static constexpr int gearShown = 6;
    // gearRows: how many rows GEAR has now.
    PhoneAction update(const PhoneControls::Event& e, int gearRows);
    PhoneScreen screen() const {
        return screen_;
    }
    int app() const {
        return app_;
    }
    PhoneButton button() const {
        return button_;
    }
    int gearTop() const {
        return gearTop_;
    }
    // Each opening starts on the home screen, MUSIC picked.
    void reset() {
        screen_ = PhoneScreen::home;
        app_ = 0;
        button_ = PhoneButton::play;
        gearTop_ = 0;
    }

  private:
    PhoneScreen screen_ = PhoneScreen::home;
    int app_{}, gearTop_{};
    PhoneButton button_ = PhoneButton::play;
};
// The volume's step, and where it starts until the player sets it, percent.
constexpr int phoneVolumeStep = 10, phoneVolume = 80;
// The phone as it is drawn: open, its frame in the world, and what its screen
// shows.
struct PhoneView {
    bool open{};
    WheelFrame frame{};
    float age{};
    PhoneScreen screen = PhoneScreen::home;
    int app{}; // the app picked on the home screen
    PhoneButton lit = PhoneButton::play;
    // GEAR: the player's gadgets in the gadget wheel's places (gadget_wheel.hpp),
    // from gearTop on.
    struct GearRow {
        char name[24]{};
        int ammo{}, most{}; // most 0: none to count
        bool equipped{};
    };
    GearRow gear[gadgetPlaces]{};
    int gearCount{}, gearTop{};
    // failed: the song could not be played (no player for its kind of file).
    bool playing{}, autoplay = true, failed{};
    int track{}, tracks{}; // track counts from 0; tracks 0: no songs
    int volume = phoneVolume; // percent
    char song[64]{};
};
// The phone as an eye image has it (NativeEyeFrame): open as the game rendered
// it, and the image's own way from tracking space into its world. Where the
// phone is in tracking space and what its screen shows are the newest as the
// image goes out (Phone::place): a phone in the hand is placed from the
// newest hand pose, not where the hand was when the game rendered the image (a
// frame or more behind: "the phone is laggy kind of when its in my hand",
// October 17).
struct PhoneImage {
    bool open{};
    Pose toWorld{};
};
// The phone held in a hand: just above it, facing the eyes, its top up (the
// player, October 17: "make the phone kinda stick to you hand until u press
// b"). In front of the head instead if the hand is not to be had.
WheelFrame phoneFrameAt(Pose head, Vec3 hand);
// Which of the controllers' controls the phone has (Phone). It takes a
// control, and gives one back, only while that control is let go of: one held
// as the phone comes out stays the game's until let go - a web held keeps
// holding the player (October 19: drawing the phone mid-swing let go of the
// web), a gadget wheel held open stays open -, and one held as the phone goes
// away stays the phone's until let go - the B that put it away, a grip
// squeezed meanwhile. So nothing sees a press or a release the player did not
// make, the phone included: a B held from the pause menu back into play is no
// press of the phone's. The menu button is always the game's.
class PhoneHold {
  public:
    // Let go of: a trigger or a grip under this, a stick nearer its centre.
    static constexpr float released = .2f;
    // live: the phone wants the controllers. `frame` keeps the game's controls
    // and has the phone's let go of; `phone` gets the phone's and has the
    // game's let go of.
    void update(bool live, XrFrame& frame, XrFrame& phone);
    // Whether the phone has any control now.
    bool any() const {
        return mine_ != 0;
    }

  private:
    uint32_t mine_{}; // a bit per control the phone has (phone.cpp)
};
// The phone in VR play (game_xr.cpp's XR worker): open from a back-pocket
// draw until B; live - its screen shown, the controllers its own (PhoneHold)
// - while VR shows play and no calibration has the controllers. It opens in
// the hand that drew it and stays in it until B (a hand the headset loses
// leaves it where it last was).
class Phone {
  public:
    // What the phone made of its controls this frame (take).
    struct Use {
        PhoneControls::Event controls;
        PhoneAction action = PhoneAction::none;
        unsigned stick{}; // the hand whose stick it read
    };
    // First thing each frame: the phone takes its controls out of `frame`
    // (PhoneHold) and, live, works its screens with them. gearRows: how many
    // rows GEAR has. B on the home screen puts the phone away at once.
    Use take(XrFrame& frame, int gearRows);
    // Once the frame knows. play: VR shows play and no calibration has the
    // controllers. head, grips: tracking space; tracked: each grip's.
    // drawHand: the hand a back-pocket draw brought the phone out with this
    // frame (PocketDraw), at `drawnAt`; -1 none. True as the phone opens.
    bool update(float seconds, bool play, const Pose& head, const Pose grips[2], const bool tracked[2],
                int drawHand = -1, Vec3 drawnAt = {});
    bool open() const {
        return open_;
    }
    bool live() const {
        return live_;
    }
    // Seconds since it opened.
    float age() const {
        return age_;
    }
    // The hand that drew it: 0 the left, 1 the right; -1 before any draw.
    int hand() const {
        return hand_;
    }
    // Where it is now, in tracking space.
    const WheelFrame& place() const {
        return place_;
    }
    const PhoneUi& ui() const {
        return ui_;
    }
    // Whether the phone has any control (PhoneHold::any).
    bool holds() const {
        return hold_.any();
    }

  private:
    PhoneControls controls_;
    PhoneUi ui_;
    PhoneHold hold_;
    bool open_{}, live_{};
    int hand_ = -1;
    float age_{};
    WheelFrame place_{};
};
// The phone drawn from a back pocket (the player, October 17: "like im
// pulling it from my back pocket then the screen pops up"). A grip squeezed
// with its hand behind a hip, then that hand brought up in front within
// drawSeconds, still squeezed: the phone opens there. That grip is the
// draw's from its squeeze until it is let go (hands()), so it shoots no web.
// Let go or too slow, nothing opens. Nothing begins or comes out outside VR
// play; a moment out of it (a game hitch) does not end a draw.
//
// The pocket is the body's, not the head's: the body faces where the head
// has for a while - it turns with the head only past yawDeadZone, at up to
// yawSpeed, and drifts toward it at yawDrift a second meanwhile, as the
// body's own does (body_ik.hpp) - so a look over a shoulder does not bring a
// hand hanging at the side into a pocket (October 19: 20 degrees of a turn
// did, and a hanging arm's grip shot no web). A hand the headset loses
// behind the back (the cameras of a headset do not see there) neither lets
// go of the grip nor ends the draw: the grip is the squeeze's alone, and a
// squeeze begins a draw where the hand was last seen, lostSeconds ago at most.
class PocketDraw {
  public:
    // In the body's level axes from the head, metres: a pocket is
    // `pocketBelow` to `pocketAbove` under the head, `pocketSide` to
    // `pocketOut` to either side, and at least `behind` back of the eyes (a
    // hand on a back pocket is about 20 cm behind them, one hanging at the
    // side about 8 cm, and a look down brings the eyes 5 cm forward); the hand
    // is out in front once it is `front` ahead and no lower than `raised`
    // under the head.
    static constexpr float pocketBelow = 1.1f, pocketAbove = .55f, pocketSide = .05f, pocketOut = .4f,
                           behind = .15f, front = .15f, raised = .65f, drawSeconds = 1.5f, pressed = .6f,
                           released = .3f, yawDeadZone = .6f, yawSpeed = 6, yawDrift = .5f, lostSeconds = .5f;
    // seconds: since the previous update. play: the controllers play VR.
    // head, grips: in one space; tracked: each grip's pose; squeezes: each
    // hand's. True as the phone comes out: hand() says where.
    bool update(float seconds, bool play, const Pose& head, const Pose grips[2], const bool tracked[2],
                const float squeezes[2]);
    // A bit per hand whose grip is the draw's.
    unsigned hands() const {
        return hands_;
    }
    Vec3 hand() const {
        return hand_;
    }
    // The hand that drew the latest phone: 0 the left, 1 the right.
    int drewWith() const {
        return drewWith_;
    }
    unsigned draws() const {
        return draws_;
    }
    // The way the body faces: radians about the up from -z toward -x, as
    // Quat::yaw turns -z.
    float yaw() const {
        return yaw_;
    }

  private:
    bool down_[2]{}, yawSet_{};
    unsigned hands_{}, draws_{};
    int drawing_ = -1, drewWith_{};
    float time_{}, yaw_{}, lost_[2]{lostSeconds + 1, lostSeconds + 1};
    Vec3 hand_{}, seen_[2]{};
};
void appendPhone(std::vector<Vertex>& out, const PhoneView& view);
// What the phone shows for a song's file name (UTF-8): no folder or extension,
// an accented Latin letter as its letter ("Beyoncé" as BEYONCE), and only what
// the overlay's font draws. A name with nothing of that (a Hebrew, Cyrillic or
// Japanese one) is TRACK and its number: `track`, from 0.
std::string phoneSongName(const std::string& fileName, int track);
} // namespace spidy

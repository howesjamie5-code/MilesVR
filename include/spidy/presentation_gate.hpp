#pragma once
#include "math.hpp"
#include <cstdint>

namespace spidy {
// How long the last eye image stays on show while the game delivers no newer
// one. A frame submitted without an image is a black flash in the headset;
// with 150 ms, stretches where the game rendered the eyes a quarter as often
// flashed black up to 19 times a second (October 5). The runtime reprojects a
// held image by head rotation, as it does for any late frame.
constexpr uint64_t imageHoldMs = 1000;
// Movement requires a recent new image. A session that once displayed an
// image must not keep driving controls after the game stops delivering them,
// but a stutter must not drop the webs either.
constexpr uint64_t controlHoldMs = 500;
inline bool recentPresentation(uint64_t lastNewImageMs, uint64_t nowMs) {
    return lastNewImageMs && nowMs >= lastNewImageMs && nowMs - lastNewImageMs <= controlHoldMs;
}
// What a submitted headset frame showed.
// stereoScreen: the game screen with a scene's own two eye images on it.
enum class Presentation : uint32_t { none = 0, immersive = 1, flat = 2, gameScreen = 3, stereoScreen = 4 };
// Pause menus and hint cards, cutscenes, finishers, death and loading give the
// tracked rig no gameplay camera. The headset then shows the game's presented
// frame on a virtual screen; before the sixth build of October 5 it showed
// nothing, which is black. The last immersive image stays up for
// screenDelayMs first, so a late camera sample does not flash the screen.
constexpr uint64_t screenDelayMs = 250;
class GameScreen {
  public:
    // Once per headset frame: true while the game's camera belongs on the screen.
    bool update(bool gameplay, uint64_t nowMs) {
        entered_ = false;
        if (gameplay) {
            closed_ = showing_ = false;
            return false;
        }
        if (!closed_) {
            closed_ = true;
            closedMs_ = nowMs;
        }
        if (!showing_ && nowMs >= closedMs_ && nowMs - closedMs_ >= screenDelayMs)
            showing_ = entered_ = true;
        return showing_;
    }
    // The update that put the screen up: place it in front of the viewer.
    bool entered() const {
        return entered_;
    }
    // Gameplay ended and the screen is not up yet: keep the last image.
    bool holding() const {
        return closed_ && !showing_;
    }
    void reset() {
        *this = {};
    }

  private:
    uint64_t closedMs_{};
    bool closed_{}, showing_{}, entered_{};
};
// A scene the game plays on the game screen (a cutscene, a finisher, a death)
// is shown there in stereo: the game renders it once for each eye. Its time
// runs. In a pause menu and on a hint card the game's frames take no game
// time, and the menu is drawn only into the game's own frame: those keep it.
constexpr uint64_t sceneHoldMs = 200;
// A scene shown in stereo needs the HUD's panel on its screen, which carries
// the subtitles there. One that has gone this long without it stays flat.
constexpr uint64_t scenePanelMs = 1000;
class SceneClock {
  public:
    // Once per headset frame, with whether the game's last frame took game
    // time: true while such a frame is no older than sceneHoldMs.
    bool running(bool advancing, uint64_t nowMs) {
        if (advancing) {
            tickMs_ = nowMs;
            ticked_ = true;
        }
        return ticked_ && nowMs >= tickMs_ && nowMs - tickMs_ <= sceneHoldMs;
    }

  private:
    uint64_t tickMs_{};
    bool ticked_{};
};
// One stretch of a scene's stereo eyes on the screen: whether to go on asking
// the game for them.
class ScenePanel {
  public:
    // Once per headset frame. `scene`: a scene plays on the screen. `panel`:
    // the HUD's panel was on the stereo screen just now. False once a stretch
    // has had none for scenePanelMs, until the scene or the screen has gone.
    bool wanted(bool scene, bool panel, uint64_t nowMs) {
        if (!scene) {
            started_ = flat_ = false;
            return false;
        }
        if (!started_ || panel || nowMs < sinceMs_) {
            started_ = true;
            sinceMs_ = nowMs;
        } else if (nowMs - sinceMs_ > scenePanelMs) {
            flat_ = true;
        }
        return !flat_;
    }

  private:
    uint64_t sinceMs_{};
    bool started_{}, flat_{};
};
// The viewer's eye distance, metres, from the two tracked eyes; the usual
// 64 mm when they say none a person has.
inline float eyeDistance(Vec3 left, Vec3 right) {
    const float d = length(right - left);
    return std::isfinite(d) && d >= .05f && d <= .08f ? d : .064f;
}
// A level virtual screen `distance` ahead of the head, facing it. Only the
// head's heading counts, so a tilted head does not tilt the screen.
inline Pose screenAhead(const Pose& head, float distance = 2.5f) {
    const Vec3 forward = head.orientation.rotate({0, 0, -1}), up = head.orientation.rotate({0, 1, 0});
    Vec3 heading{forward.x, 0, forward.z};
    // Looking straight up or down, the top of the head points along the heading.
    if (length(heading) < .1f)
        heading = forward.y < 0 ? Vec3{up.x, 0, up.z} : Vec3{-up.x, 0, -up.z};
    heading = normalized(heading);
    if (length(heading) < .5f)
        heading = {0, 0, -1};
    return {head.position + heading * distance, Quat::yaw(std::atan2(-heading.x, -heading.z))};
}
} // namespace spidy

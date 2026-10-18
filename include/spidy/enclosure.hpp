#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace spidy {
// Whether the player is inside something, read from the room around him.
//
// Why it exists. The game takes its own interact away the moment the player
// leaves a surface - 63 ms, measured at the vent of the first mission on
// October 13 - and Spidy's wall running drives the hero in the game's air
// state. So inside a vent, where every way on is an interact, nothing can be
// pressed, and no button was ever going to help. Letting the wall go for the
// press only drops the player: the swing stops holding a hero the game still
// has in the air, and the game's own crawl does not pick him up where he
// stands.
//
// The player found the answer: "wall walking will need to be disabled inside
// vents, the camera was fine without it". Indoors, let the game keep the
// walls. Its own crawl holds him, its interacts work, and its camera behaves -
// all three, for free, because it is the game doing what it already does.
//
// So this is a rule about the place, not about the press, and that is what
// makes it safe: the walls are off before the player is ever stuck to one, so
// there is nothing to let go of. It reads the room from the world rays a
// session already casts - one up, four along the ground - and asks how closed
// in he is. A vent, a duct, a lift shaft or a corridor closes on him from
// above or from opposite sides; a street, a rooftop or a building face does
// not. There is no list of places in it, so the next vent is already covered.
class Enclosure {
  public:
    // A surface this near counts as closing the player in. A vent is about a
    // metre across and a corridor three; a street is tens.
    static constexpr float nearby = 4.f;
    // Two of the three ways of being closed in, so that one wall, one ceiling
    // or one alley is not "indoors": a vent has all three, a lift shaft has
    // its two opposite pairs, a corridor its sides and its ceiling, and an
    // alley with open sky has only one.
    static constexpr unsigned ways = 2;
    // How long a reading has to hold before the verdict follows it, so that a
    // doorway, a beam or a passing truck does not flap the walls.
    static constexpr float settle = .4f;
    // What each ray found, in metres, along the world's axes: whatever the ray
    // was cast to where it found nothing.
    struct Room {
        float up{}, north{}, south{}, east{}, west{};
    };
    // How many of the three ways this room closes the player in.
    static unsigned closedWays(Room room) {
        return (room.up < nearby ? 1u : 0u) + (room.north < nearby && room.south < nearby ? 1u : 0u) +
               (room.east < nearby && room.west < nearby ? 1u : 0u);
    }
    // One reading. `seconds` is the time SINCE THE PREVIOUS UPDATE, as every
    // other rule here takes it (game_pad::PadLayer) - not a clock. Taking it
    // for a clock is what kept the first build of this from ever deciding:
    // `now` was the frame's own length, about 11 ms every time, so nothing
    // could hold for the 400 ms the verdict waits and `indoor_frames` came
    // back 0 from a session spent in a vent.
    //
    // Returns the verdict, which only moves once a reading has held.
    bool update(float seconds, Room room) {
        const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
        const bool closed = closedWays(room) >= ways;
        if (closed != held_) {
            held_ = closed;
            heldFor_ = 0;
        } else {
            heldFor_ += dt;
        }
        if (held_ != indoors_ && heldFor_ >= settle) {
            indoors_ = held_;
            ++changes_;
        }
        return indoors_;
    }
    bool indoors() const {
        return indoors_;
    }
    // Times the verdict changed, for a session report.
    uint32_t changes() const {
        return changes_;
    }

  private:
    bool indoors_{}, held_{};
    float heldFor_{};
    uint32_t changes_{};
};
} // namespace spidy

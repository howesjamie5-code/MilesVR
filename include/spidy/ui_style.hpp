#pragma once
#include <cstdint>

namespace spidy {
// How Spidy's own marks in the headset look (the SPIDY VR tab's UI STYLE): the
// aim markers, the combat marks, Spider-Sense, the gadget wheel, slow motion's
// meter and the calibration panel. Comic, printed as the graphics of Spidy's
// videos are (web_visual.cpp); or tech, as the game's own HUD and menus are:
// thin light lines, cyan for what is picked, translucent blue panels, corner
// brackets and diamonds, everything easing in and fading (tech_marks.hpp). The
// player, October 21: "an optional alternative set of our custom ingame vr ui
// in more scifi techy style like the base game ui".
enum class UiStyle : std::uint8_t { comic, tech };
} // namespace spidy

#pragma once

#include <cstdint>

namespace spidy {

// How often the game's window gets the left eye while the game renders no
// view of its own (immersive VR): presents a second, or 0 for every frame the
// game presents. A present the window does not get never reaches DXGI. On
// October 11 each present of a 3840 x 2400 window took 8 ms of a 23 ms frame
// on a laptop whose screen hangs on the other graphics adapter (43 frames a
// second with every present, 66 with none).
inline constexpr uint32_t defaultMonitorRate = 30, maximumMonitorRate = 240;

constexpr bool validMonitorRate(uint32_t rate) {
    return rate <= maximumMonitorRate;
}

// Whether the present at `now` goes to the window, and when the next one is
// `due` (ticks, `frequency` of them a second). Due times stay on the rate's
// grid, so 30 a second from 66 frames is 30, not every third frame's 22; a
// window that fell more than a period behind starts a new grid.
constexpr bool monitorPresentDue(int64_t now, int64_t& due, int64_t frequency, uint32_t rate) {
    if (!rate)
        return true;
    if (now < due)
        return false;
    const int64_t period = frequency / rate;
    due = (now - due > period ? now : due) + period;
    return true;
}

// The eye the window mirrors is the one the headset showed, with Spidy's own
// drawings on it (native_gpu::keepShownEye): a copy made for each picture the
// window takes. A copy stays on the window this long (ms) with no newer one:
// half a second, or two and a half pictures of a slower window. After that
// the window shows the game's own image of the eye, which goes on while the
// headset takes no frames.
constexpr uint64_t shownEyeHoldMs(uint32_t rate) {
    return rate && rate < 5 ? 2500 / rate : 500;
}
constexpr bool shownEyeFresh(uint64_t copiedMs, uint64_t nowMs, uint32_t rate) {
    return copiedMs && nowMs >= copiedMs && nowMs - copiedMs <= shownEyeHoldMs(rate);
}

} // namespace spidy

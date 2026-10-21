#pragma once
#include "body_haptics.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

// Body haptics on bHaptics devices (body_haptics.hpp) through the bHaptics
// Player, with bHaptics' own SDK (github.com/bhaptics/tact-cpp2, v2.7.1;
// tools/bootstrap.ps1 fetches it). Its bhaptics_library.dll is loaded only
// once body haptics are on, from beside the module that plays them, so a
// player without bHaptics loads nothing of it, and a missing library costs
// only the haptics. Everything runs on a thread of its own: every SDK call
// waits for the SDK's own worker, and the headset's frame loop must not.
//
// The thread mixes what it was given (body_haptics::Mixer) every `tickMs` and
// sends each connected device its frame (the SDK's playDot), each playing for
// `frameMs`, longer than a tick, so that a late tick leaves no gap. Every
// device keeps one request id for its frames, and its last frame is stopped
// before the next plays, so one plays at a time: whether the Player replaces a
// playing frame with a new one under the same id, or plays both (and adds them
// up, or takes the stronger), is not to be seen from outside - neither its log
// nor its status says what a motor does. Until the second build of October 20,
// 70 ms frames every 40 ms overlapped for 30 ms of each tick. A motor under
// minimumLevel percent is sent as off: too weak to turn, and it would keep a
// device streaming for nothing.
//
// The Player knows an app by the workspace ID and API key of an app on
// bHaptics' Developer Portal (developer.bhaptics.com), deployed with at least
// one event, which Spidy never plays. bHaptics' server answers "No workspace"
// for any other ID, the app's own record ID among them (October 20); taken, the
// Player sends the app's event list as it connects, which is how the thread
// tells a Player that took the app from one that did not (Status::rejected).
// A build gets them from SPIDY_BHAPTICS_APP_ID and SPIDY_BHAPTICS_API_KEY
// (CMakeLists.txt); one without them plays nothing.
namespace spidy::bhaptics {
// What the connection does (SpidyHapticsData::status).
enum class Status : uint32_t {
    off,       // not started: body haptics have not been on
    starting,  // the library loads
    noLibrary, // no bhaptics_library.dll, or one without the SDK's functions
    noApp,     // this build has no bHaptics app (its App ID and API key)
    noPlayer,  // the bHaptics Player is not installed
    waiting,   // waiting for the Player: not running, or not answering yet
    connected, // connected, without a device on
    playing,   // connected, with a device on
    failed,    // the SDK would not start
    rejected,  // connected, but the Player sent no event list for the app within 5 s: it did not take the app
};
const char* name(Status);
struct Config {
    std::wstring library;      // bhaptics_library.dll, a full path
    std::string appId, apiKey; // the Developer Portal app
    int tickMs = 40, frameMs = 100;
};
inline constexpr int minimumLevel = 3;
struct Counters {
    Status status{};
    // Devices on now and at any time (bit 1 << body_haptics::Device); the
    // times the Player was connected; the player's level now, percent.
    uint32_t devices{}, devicesSeen{}, connects{}, level{};
    // Events posted, device frames sent and those the SDK did not send.
    uint64_t events{}, frames{}, refused{};
    std::array<uint32_t, body_haptics::kindCount> kinds{};
    // Ticks of VR play (the state live) with the Player connected; of those,
    // the ticks each device played, and the sum of its strongest motor over
    // them (percent): how much of play a device was on, and how strongly.
    uint32_t liveTicks{};
    std::array<uint32_t, body_haptics::deviceCount> onTicks{}, peakSum{};
};
class Output {
  public:
    Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    ~Output() {
        stop();
    }
    // Starts the thread; it loads the library and connects. Once: a stopped
    // output does not start again.
    void start(const Config&);
    // Lets the devices go quiet and disconnects, waiting a few seconds at
    // most. The library stays loaded until the process ends: the game runs
    // one VR session, and a library unloaded under threads of its own is a
    // crash waiting for its moment.
    void stop();
    bool started() const;
    // The player's level, 0-100 percent: 0 sends nothing.
    void level(int percent);
    void post(const body_haptics::Event&);
    // Lasts 300 ms: given every frame, as the headset's loop does; a loop
    // that stops leaves the devices quiet.
    void set(const body_haptics::State&);
    void hold(const body_haptics::Frame&, float seconds);
    Counters counters() const;
    // The Player's own words about its devices (getDeviceInfoJson), for the
    // test tool: it asks the SDK holding the lock the other calls take. Empty
    // while the Player is not connected.
    std::string deviceInfo();

  private:
    // What the callers and the thread share. The thread holds it too: one
    // that does not finish within a few seconds of stop() (an SDK call that
    // hangs) is let go of, and must find it still there.
    struct Shared;
    std::shared_ptr<Shared> shared_;
    std::thread thread_;
};
// The app this build was made for, from SPIDY_BHAPTICS_APP_ID and
// SPIDY_BHAPTICS_API_KEY; empty in a build without.
const char* builtAppId();
const char* builtApiKey();
// bhaptics_library.dll beside the module (DLL or program) this code is in.
std::wstring libraryBeside();
// The devices in `devices` (bit 1 << body_haptics::Device) by the names the
// player knows them by: "TactSuit, left TactSleeve and TactVisor"; "none".
std::string deviceNames(uint32_t devices);
} // namespace spidy::bhaptics

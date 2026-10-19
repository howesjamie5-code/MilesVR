#include "spidy/launcher_text.hpp"
#include "spidy/launcher_update.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace spidy::launcher;
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main() {
    int total = 0, failed = 0;
    auto test = [&](const char* name, const std::function<void()>& f) {
        ++total;
        try {
            f();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    test("Steam library folders: every path, escapes decoded, nested blocks skipped", [] {
        const auto values = vdfValues(R"("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"apps"
		{
			"1817070"		"80213345236"
		}
	}
	"1"
	{
		"PATH"		"D:\\SteamLibrary"   // a comment "path" "E:\\not"
	}
})", "path");
        check(values.size() == 2, "two libraries");
        check(values[0] == R"(C:\Program Files (x86)\Steam)", "backslashes decoded");
        check(values[1] == R"(D:\SteamLibrary)", "keys match without case; comments skipped");
    });
    test("app manifest gives the install folder, apostrophe included", [] {
        const auto folders = vdfValues(R"("AppState" { "appid" "1817070" "installdir" "Marvel's Spider-Man Remastered" })",
                                       "installdir");
        check(folders.size() == 1 && folders[0] == "Marvel's Spider-Man Remastered", "installdir");
        check(vdfValues(R"("a" { "installdir" })", "installdir").empty(), "a key without a value");
    });
    test("Epic manifests are JSON, but read the same way", [] {
        const auto folders =
            vdfValues(R"({"DisplayName": "Marvel's Spider-Man Remastered", "InstallLocation": "D:\\Epic\\MarvelsSpiderMan"})",
                      "InstallLocation");
        check(folders.size() == 1 && folders[0] == R"(D:\Epic\MarvelsSpiderMan)", "install location");
    });
    test("a block's text, blocks inside it included", [] {
        const char* text = R"("AppState" { "UserConfig" { "language" "english" "x" { "BetaKey" "inner" } }
            "MountedConfig" { "BetaKey" "mounted" "brace" "}" } })";
        check(vdfValues(vdfBlock(text, "userconfig"), "BetaKey") == std::vector<std::string>{"inner"},
              "a block inside, the name without case");
        check(vdfValues(vdfBlock(text, "MountedConfig"), "brace") == std::vector<std::string>{"}"},
              "a quoted brace");
        check(vdfBlock(text, "Missing").empty(), "no such block");
        check(vdfBlock(R"("language" "UserConfig")", "UserConfig").empty(), "a value, not a block");
    });
    test("Steam's app manifest gives the beta chosen and an update waiting", [] {
        auto app = steamApp(R"("AppState"
{
	"appid"		"1817070"
	"StateFlags"		"4"
	"installdir"		"Marvel's Spider-Man Remastered"
	"buildid"		"10131361"
	"TargetBuildID"		"10131361"
	"UserConfig"
	{
		"language"		"english"
		"BetaKey"		"previous_version2"
	}
	"MountedConfig"
	{
		"language"		"english"
		"BetaKey"		"previous_version2"
	}
})");
        check(app.found && app.beta == "previous_version2" && !app.updateWaiting, "on a beta");
        // Back on None, the installed files stay the beta's until Steam updates them.
        app = steamApp(R"("AppState" { "appid" "1817070" "StateFlags" "6" "buildid" "10131361"
            "TargetBuildID" "23986256" "UserConfig" { "language" "english" }
            "MountedConfig" { "BetaKey" "previous_version2" } })");
        check(app.found && app.beta.empty() && app.updateWaiting, "leaving a beta");
        app = steamApp(R"("AppState" { "appid" "1817070" "StateFlags" "4" "buildid" "23986256"
            "TargetBuildID" "23986256" "UserConfig" { "BetaKey" "" } })");
        check(app.found && app.beta.empty() && !app.updateWaiting, "up to date, no beta");
        check(steamApp(R"("AppState" { "appid" "1817070" "StateFlags" "1026" })").updateWaiting,
              "downloading");
        check(!steamApp("").found, "no manifest");
    });
    test("constants come from the Python tools", [] {
        const std::string source = "\"\"\"doc EXPECTED_SHA256 = 'no'\"\"\"\nimport x\nEXPECTED_SHA256 = \"e297d4\"\n"
                                   "MAX_EYE_SIZE = 8192\n# MAX_EYE_SIZE = 1\nOTHER_MAX_EYE_SIZE = 5\n";
        check(pythonConstant(source, "EXPECTED_SHA256") == std::optional<std::string>("e297d4"), "string at a line start");
        check(pythonConstant(source, "MAX_EYE_SIZE") == std::optional<std::string>("8192"), "number, not a comment");
        check(!pythonConstant(source, "MISSING"), "missing");
    });
    test("runtime names match tools/xr_runtime.py", [] {
        check(runtimeLabel(R"(C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json)") ==
                  "Virtual Desktop", "Virtual Desktop");
        check(runtimeLabel(R"(C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json)") == "SteamVR",
              "SteamVR");
        check(runtimeLabel(R"(C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_64.json)") == "Meta Quest Link",
              "Oculus");
        check(runtimeLabel(R"(C:\elsewhere\runtime.json)").empty(), "unknown");
    });
    test("OpenVR's path file says where SteamVR is", [] {
        // As SteamVR writes it: its folder twice, in two spellings, after other lists.
        const std::string paths = "{\n\t\"config\" : \n\t[\n\t\t\"c:\\\\program files (x86)\\\\steam\\\\config\"\n\t],\n"
                                  "\t\"external_drivers\" : \n\t[\n\t\t\"C:\\\\Program Files\\\\Virtual Desktop "
                                  "Streamer\\\\OpenVRDriver\"\n\t],\n\t\"jsonid\" : \"vrpathreg\",\n\t\"runtime\" : \n\t[\n"
                                  "\t\t\"c:\\\\program files (x86)\\\\steam\\\\steamapps\\\\common\\\\SteamVR\",\n"
                                  "\t\t\"C:\\\\Program Files (x86)\\\\Steam\\\\steamapps\\\\common\\\\SteamVR\"\n\t],\n"
                                  "\t\"version\" : 1\n}\n";
        check(openVrRuntimes(paths) ==
                  std::vector<std::string>{R"(c:\program files (x86)\steam\steamapps\common\SteamVR)",
                                           R"(C:\Program Files (x86)\Steam\steamapps\common\SteamVR)"},
              "SteamVR's folders");
        // A library named in another alphabet, written as escapes or as it is; a character of two escapes.
        const std::string games = "\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B";
        check(openVrRuntimes(R"({"runtime":["D:\\\u0418\u0433\u0440\u044b\\SteamVR","D:/)" + games +
                             R"(/SteamVR","E:\\\ud83d\ude00"]})") ==
                  std::vector<std::string>{"D:\\" + games + "\\SteamVR", "D:/" + games + "/SteamVR",
                                           "E:\\\xF0\x9F\x98\x80"},
              "escaped characters");
        check(openVrRuntimes(R"({"log":["runtime"],"runtime":[]})").empty(), "no runtime listed");
        check(openVrRuntimes(R"({"runtime":"C:\\SteamVR"})").empty(), "not a list");
        check(openVrRuntimes("\xEF\xBB\xBF{\"runtime\":[\"C:\\\\SteamVR\", 7]}") == std::vector<std::string>{"C:\\SteamVR"},
              "a byte order mark; an entry that is no text");
        check(openVrRuntimes("").empty() && openVrRuntimes("{\"runtime\" : [ \"C:\\\\Steam").empty(),
              "an empty file, a cut one");
    });
    test("the headset check names the headset and the runtime it was found in", [] {
        const char* probe = "Headset available: Oculus Quest3; position tracking=1; orientation tracking=1. No session "
                            "was started.\r\nRecommended eye 0: 3072x3264\r\nRecommended eye 1: 3072x3264\r\n";
        check(headsetSummary(probe, true) == "Connected: Oculus Quest3 - 3072x3264 per eye", "one runtime's probe");
        const std::string detect = std::string("Looking for the headset in Virtual Desktop...\nLooking for the headset "
                                               "in SteamVR...\nVR runtime: SteamVR (C:\\SteamVR\\steamxr_win64.json), "
                                               "found automatically.\n") + probe;
        check(headsetSummary(detect, true) == "Connected: Oculus Quest3 via SteamVR - 3072x3264 per eye", "detected");
        check(headsetSummary("Headset unavailable: XR_ERROR_FORM_FACTOR_UNAVAILABLE (-35).\n", false) ==
                  "Not found. Put the headset on and connect it (Headset unavailable: XR_ERROR_FORM_FACTOR_UNAVAILABLE "
                  "(-35).).",
              "the probe's reason");
        check(headsetSummary("Looking for the headset in Virtual Desktop...\nNo headset found (Virtual Desktop: x).\n",
                             false) == "Not found. Put the headset on and connect it (No headset found (Virtual Desktop: "
                                       "x).).",
              "detection's reason, not its progress");
        check(headsetSummary("", false) == "Not found. Put the headset on and connect it.", "nothing said");
        check(headsetSummary("", true) == "Headset available.", "found, nothing said");
    });
    test("arguments survive Windows command-line quoting", [] {
        check(quoteArgument(L"plain") == L"plain", "plain");
        check(quoteArgument(L"") == L"\"\"", "empty");
        check(quoteArgument(LR"(C:\Program Files\Spidy\run.py)") == LR"("C:\Program Files\Spidy\run.py")", "spaces");
        check(quoteArgument(LR"(C:\Spidy Folder\)") == LR"("C:\Spidy Folder\\")", "trailing backslash doubled");
        check(quoteArgument(LR"(say "hi")") == LR"("say \"hi\"")", "quotes escaped");
    });
    test("session arguments carry every option", [] {
        SessionOptions options;
        auto args = sessionArguments(options, L"r.json", L"vd.json", L"Local\\Stop");
        const std::vector<std::wstring> defaults{L"--auto-launch", L"--seconds", L"0", L"--swing-speed", L"32",
                                                 L"--output", L"r.json", L"--xr-runtime", L"vd.json",
                                                 L"--stop-event", L"Local\\Stop"};
        check(args == defaults, "defaults");
        options = {false, true, 2048, 90, false, false};
        args = sessionArguments(options, L"r.json", L"", L"");
        const std::vector<std::wstring> changed{L"--auto-launch", L"--seconds", L"0", L"--swing-speed", L"65",
                                                L"--output", L"r.json", L"--full-desktop-view",
                                                L"--stock-monitor-view", L"--no-aim-markers", L"--no-air-webs",
                                                L"--render-scale", L"200"};
        check(args == changed, "every flag, speed capped at 65 and the render scale at 200%");
        options = {};
        options.renderScale = 125;
        options.snapTurn = 45;
        options.smoothTurn = 120;
        options.haptics = 0;
        options.screenSize = 7;
        options.weight = 150;
        args = sessionArguments(options, L"r.json", L"", L"");
        const std::vector<std::wstring> settings{L"--auto-launch", L"--seconds", L"0", L"--swing-speed", L"32",
                                                 L"--output", L"r.json", L"--render-scale", L"125", L"--snap-turn",
                                                 L"45", L"--smooth-turn", L"120", L"--haptics", L"0",
                                                 L"--screen-size", L"2", L"--weight", L"150"};
        check(args == settings, "the VR settings, the screen size capped at large");
        options = {};
        options.renderScale = 10;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--render-scale" && args[8] == L"50", "the render scale at least 50%");
        // Upscaling: the default goes unsaid, another choice is passed, within its range.
        options = {};
        options.upscale = 100;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--upscale" && args[8] == L"100", "no upscaling");
        options.upscale = 20;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--upscale" && args[8] == L"50", "the upscale at least 50%");
        options.renderScale = 125;
        options.upscale = 58;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 11 && args[7] == L"--render-scale" && args[9] == L"--upscale" && args[10] == L"58",
              "the upscale after the render scale");
        // The monitor's rate in VR: 30 a second goes unsaid, another is passed, at most 240.
        options = {};
        options.monitorRate = 0;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--monitor-rate" && args[8] == L"0", "the monitor at every frame");
        options.monitorRate = 999;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--monitor-rate" && args[8] == L"240", "the monitor at most 240");
        options = {};
        options.weight = 999;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--weight" && args[8] == L"300", "the weight at most 300%");
        options = {};
        check(!options.flips, "the experimental flips are on in a new launcher");
        options.flips = true;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 8 && args[7] == L"--flips", "the experimental flips switched on");
        options = {};
        check(!options.triggerWebs, "the trigger webs in a new launcher");
        options.triggerWebs = true;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 8 && args[7] == L"--trigger-webs", "the web button on the trigger");
        options = {};
        check(!options.venomLeftHand, "the venom punch on the left hand in a new launcher");
        options.venomLeftHand = true;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 8 && args[7] == L"--venom-left-hand", "the venom punch on the left hand");
        options.venomLeftHand = false;
        check(!options.venomTrigger, "the venom on the trigger in a new launcher");
        options.venomTrigger = true;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 8 && args[7] == L"--venom-trigger", "the venom button on the trigger");
        options.venomTrigger = false;
        check(!options.arcadeWebs, "arcade webs in a new launcher");
        options.arcadeWebs = true;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 8 && args[7] == L"--arcade-webs", "arcade webs chosen");
        // The view turning onto a wall is no option of its own and is never passed: it turns whoever holds
        // the player there, the swing or the game's own crawl, which is the whole point of giving the crawl
        // the view (the player, October 14: "i like how it feel to walk on building and stand on their side",
        // of a vent that only works while the game holds him). A launcher.ini from the build that tied the
        // view to wall running can still have it off, and that is not read back either.
        options = {};
        check(options.standOnWalls, "no standing on walls in a new launcher");
        options.standOnWalls = false;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 7, "the view on walls was passed to the session");
        // Wall running, the default, goes unsaid; off, the game's own wall crawl takes a wall, which is
        // what tells whether something on a wall is the swing's doing (October 13, a vent the game would
        // not interact with while the swing owned the hero).
        options = {};
        check(options.wallRun, "wall running is not the default");
        options.wallRun = false;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 8 && args[7] == L"--no-wall-run", "wall running switched off");
        // Real gravity is the default weight since October 12, and goes unsaid; the old default is passed.
        options = {};
        check(options.weight == 100, "the weight in a new launcher is not real gravity");
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 7, "the default weight said to the session");
        options.weight = 80;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--weight" && args[8] == L"80", "a lighter weight");
        // The game's depth of field and sharpening are the session's to turn off, unless switched.
        options = {};
        check(!options.gameDepthOfField && !options.gameSharpening, "the game's picture kept in a new launcher");
        options.gameDepthOfField = options.gameSharpening = true;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--game-depth-of-field" && args[8] == L"--game-sharpening",
              "the game's own depth of field and sharpening kept");
        options = {};
        check(options.screenDepth == 2, "cutscenes at medium depth in a new launcher");
        options.screenDepth = 0;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--screen-depth" && args[8] == L"0", "flat cutscenes");
        options.screenDepth = 9;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--screen-depth" && args[8] == L"3", "the depth at most high");
        options = {};
        check(options.hud == 2, "the medium HUD in a new launcher");
        options.hud = 0;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--hud" && args[8] == L"0", "the HUD off");
        options.hud = 9;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--hud" && args[8] == L"3", "the HUD at most large");
        options = {};
        check(options.flipSpeed == 180, "flips at 180 degrees a second in a new launcher");
        options.flipSpeed = 240;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--flip-speed" && args[8] == L"240", "the flip speed");
        options.flipSpeed = 9999;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--flip-speed" && args[8] == L"480", "the flip speed at most 480");
        options.flipSpeed = 1;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 9 && args[7] == L"--flip-speed" && args[8] == L"90", "the flip speed at least 90");
    });
    test("the headset check's eye size", [] {
        const char* probe = "Headset available: Oculus Quest3; position tracking=1; orientation tracking=1. No session "
                            "was started.\r\nRecommended eye 0: 2496x2688\r\nRecommended eye 1: 2496x2688\r\n";
        const auto eye = recommendedEye(probe);
        check(eye && (*eye)[0] == 2496 && (*eye)[1] == 2688, "the probe's first eye");
        check(!recommendedEye("Headset available: x.\n") && !recommendedEye("Recommended eye 0: 0x2688\n") &&
                  !recommendedEye("Recommended eye 0: 2496 x 2688\n") &&
                  !recommendedEye("Not Recommended eye 0: 2496x2688\n"),
              "no size, an invalid one, another format, another line");
    });
    test("what the VR settings were left at in the headset becomes the next session's options", [] {
        SessionOptions options;
        check(headsetSettings("VR settings from the headset: aim_markers=0 web_grab=1 air_webs=0 web_shooter=0 "
                              "punch=0 body=1 swing_speed=48 weight=150 snap_turn=45 smooth_turn=90 haptics=50 "
                              "flips=1 flip_speed=300 trigger_webs=1 hud=0 stand_on_walls=0 screen_depth=3 "
                              "arcade_webs=1 screen_size=2\r",
                              options),
              "the line changed nothing");
        check(!options.aimMarkers && !options.airWebs && options.swingSpeed == 48 && options.weight == 150 &&
                  options.snapTurn == 45 && options.smoothTurn == 90 && options.haptics == 50 && options.flips &&
                  options.flipSpeed == 300 && options.triggerWebs && options.hud == 0 &&
                  options.screenDepth == 3 && options.arcadeWebs && options.screenSize == 2,
              "every value, the last one before a carriage return");
        // ... and `stand_on_walls` in that line is no longer one of them.
        check(options.standOnWalls, "the view on walls was taken from a headset line");
        // Webs are always physical since October 15: an older session's physical_webs changes nothing.
        {
            const auto before = options;
            check(!headsetSettings("VR settings from the headset: physical_webs=0", options) && options == before,
                  "a headset line's physical_webs, no longer a setting, changed the options");
        }
        const auto kept = options;
        check(!headsetSettings("VR settings from the headset: aim_markers=0 air_webs=0", options) && options == kept,
              "the same values again");
        check(!headsetSettings("VR settings from the headset: web_grab=0 web_shooter=0 punch=0 body=0", options) &&
                  options == kept,
              "web grab, the web shooter, punching and the body are no options");
        check(!headsetSettings("Game closed. Session report: r.json", options) && options == kept, "another line");
        check(headsetSettings("VR settings from the headset: swing_speed=99 snap_turn=x smooth_turn=999 haptics=-4 "
                              "weight=7 hud=8 flip_speed=5000 colour=3",
                              options) &&
                  options.swingSpeed == 65 && options.snapTurn == 45 && options.smoothTurn == 360 &&
                  options.haptics == 0 && options.weight == 40 && options.hud == 3 && options.flipSpeed == 480,
              "values outside their ranges, unreadable ones and unknown keys");
    });
    test("a T-pose calibration in the headset sizes every next session, until Redo", [] {
        SessionOptions options;
        check(!calibrated(options) && options.calibrationPrompt, "a new launcher has one");
        // Calibrated in the headset: eyes 1.63 m high, arms 0.59 m.
        check(headsetSettings("VR settings from the headset: aim_markers=1 air_webs=1 swing_speed=32 weight=100 "
                              "snap_turn=30 smooth_turn=0 haptics=100 screen_size=1 eye_height_mm=1630 "
                              "arm_length_mm=590 calibration_prompt=1",
                              options) &&
                  calibrated(options) && options.eyeHeightMm == 1630 && options.armLengthMm == 590,
              "the calibration did not come back from the headset");
        auto args = sessionArguments(options, L"r.json", L"", L"");
        const std::vector<std::wstring> sized{L"--auto-launch", L"--seconds", L"0",    L"--swing-speed",
                                              L"32",            L"--output",  L"r.json", L"--eye-height",
                                              L"1630",          L"--arm-length", L"590"};
        check(args == sized, "the next session is not sized");
        // MilesVR: VENOM HAND moved to LEFT in the headset is the next session's Venom punch hand.
        check(headsetSettings("VR settings from the headset: venom_left_hand=1", options) && options.venomLeftHand &&
                  headsetSettings("VR settings from the headset: venom_left_hand=0", options) && !options.venomLeftHand,
              "the venom hand did not come back from the headset");
        check(headsetSettings("VR settings from the headset: venom_trigger=1", options) && options.venomTrigger &&
                  headsetSettings("VR settings from the headset: venom_trigger=0", options) && !options.venomTrigger,
              "the venom button did not come back from the headset");
        // Half a calibration, or one out of range, keeps the one there was.
        const auto kept = options;
        check(!headsetSettings("VR settings from the headset: eye_height_mm=1700 arm_length_mm=0", options) &&
                  options == kept,
              "half a calibration replaced it");
        check(!headsetSettings("VR settings from the headset: eye_height_mm=99999 arm_length_mm=590", options) &&
                  options == kept,
              "an eye height out of range replaced it");
        options.armLengthMm = 1300;
        check(!calibrated(options), "an arm out of range counted");
        // Skipped in the headset: no calibration, and none asked for at the next sessions' first gameplay.
        options = {};
        check(headsetSettings("VR settings from the headset: eye_height_mm=0 arm_length_mm=0 calibration_prompt=0",
                              options) &&
                  !calibrated(options) && !options.calibrationPrompt,
              "skipping it did not come back");
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 8 && args[7] == L"--no-calibration-prompt", "the next session still asks");
        // A calibration made from the SPIDY VR tab after a skip counts; the skip says nothing then.
        options.eyeHeightMm = 1500;
        options.armLengthMm = 520;
        args = sessionArguments(options, L"r.json", L"", L"");
        check(args.size() == 11 && args[7] == L"--eye-height" && args[8] == L"1500", "the later calibration");
    });
    test("log lines are coloured by what they say", [] {
        check(classifyLine("WARNING: render memory module unavailable") == LineKind::warning, "warning");
        check(classifyLine("Spidy VR unavailable: no headset") == LineKind::error, "error");
        check(classifyLine("Headset available: Oculus Quest3") == LineKind::good, "good");
        check(classifyLine("Starting Spider-Man.") == LineKind::normal, "normal");
    });
    test("a session an administrator's game kept out is an error with its own exit code", [] {
        check(classifyLine("Spidy VR unavailable: Spider-Man is running as administrator, and Windows keeps Spidy "
                           "out of such a game.") == LineKind::error, "error line");
        // tools/vr_launcher.py's NEEDS_ADMINISTRATOR_EXIT (tests/launcher_protocol_tests.py holds it to 5 too).
        check(kNeedsAdministratorExit == 5, "exit code");
    });
    test("a game that already holds Spidy can be closed and VR started again", [] {
        // tools/vr_launcher.py's CLOSE_GAME_EXIT (tests/launcher_protocol_tests.py holds it to 6 too).
        check(kCloseGameExit == 6 && kCloseGameExit != kNeedsAdministratorExit, "exit code");
        check(classifyLine("Spidy VR unavailable: Spider-Man is still running with an older Spidy build in it: it "
                           "was open while Spidy was updated.") == LineKind::error,
              "error line");
        const SessionOptions options;
        const auto plain = sessionArguments(options, L"r.json", L"", L"");
        const auto closing = sessionArguments(options, L"r.json", L"", L"", true);
        check(std::find(plain.begin(), plain.end(), L"--close-game") == plain.end(), "asked without the player");
        check(closing.size() == plain.size() + 1 && closing.back() == L"--close-game", "the agreed close");
    });
    test("runtime versions compare field by field", [] {
        check(parseVersion("14.44.35211.0") >= std::array<int, 4>{14, 40, 0, 0}, "newer");
        check(parseVersion("14.38.33135") < std::array<int, 4>{14, 40, 0, 0}, "older");
        check(parseVersion("v14.40") == std::array<int, 4>{14, 40, 0, 0}, "prefix");
    });
    test("another game version says which it is and what fixes it", [] {
        check(unsupportedGame("1.1212.0.0", "4.630.0.0", {true, "previous_version2", false}) ==
                  "Version 1.1212.0.0, older than the 4.630.0.0 Spidy supports. Steam is set to the beta "
                  "\"previous_version2\": in Steam, right-click the game > Properties > Betas, choose None "
                  "and let it update. Then press Re-check.",
              "older, on a beta");
        check(unsupportedGame("3.618.0.0", "4.630.0.0", {true, "", true}) ==
                  "Version 3.618.0.0, older than the 4.630.0.0 Spidy supports. Steam has an update for it "
                  "waiting: let Steam install it, then press Re-check.",
              "older, an update waiting");
        const std::string verify = " In Steam, right-click the game > Properties > Installed Files > Verify "
                                   "integrity of game files, then press Re-check.";
        check(unsupportedGame("3.618.0.0", "4.630.0.0", {true, "", false}) ==
                  "Version 3.618.0.0, older than the 4.630.0.0 Spidy supports." + verify,
              "older, Steam sees nothing to do");
        check(unsupportedGame("4.630.0.0", "4.630.0.0", {true, "", false}) ==
                  "Version 4.630.0.0, the one Spidy supports, but a mod, a patch or a damaged download "
                  "changed MilesMorales.exe." + verify,
              "the supported version, changed");
        check(unsupportedGame("4.1015.0.0", "4.630.0.0", {true, "", true}) ==
                  "Version 4.1015.0.0, newer than the 4.630.0.0 Spidy supports. Spidy reads the game's code "
                  "at fixed places, so a game update needs a Spidy update.",
              "newer: nothing to do in Steam");
        check(unsupportedGame("", "4.630.0.0", {}) ==
                  "A different game version than the one Spidy supports (4.630.0.0). Spidy works with Steam's "
                  "copy of the game: choose the MilesMorales.exe in your Steam library with Change...",
              "no version resource, not installed by Steam");
        check(unsupportedGame("4.630.0.0", "", {true, "", false}) ==
                  "A different game version than the one Spidy supports." + verify,
              "no supported version known");
    });
    test("JSON: nesting, escapes and the four kinds of scalar", [] {
        const auto value = parseJson(R"( {"a": [1, -2.5e1, true, false, null], "s": "q\"\\\/é🕸\n",
            "o": {"in": {"deep": "x"}}, "empty": {}, "none": []} )");
        check(value && value->type == Json::Type::object, "an object");
        const Json* a = value->get("a");
        check(a && a->items.size() == 5 && a->items[0].number == 1 && a->items[1].number == -25, "numbers");
        check(a->items[2].boolean && a->items[3].type == Json::Type::boolean && !a->items[3].boolean, "true, false");
        check(a->items[4].type == Json::Type::null, "null");
        check(value->text("s") == "q\"\\/\xC3\xA9\xF0\x9F\x95\xB8\n", "escapes, a surrogate pair as one UTF-8 character");
        check(value->get("o")->get("in")->text("deep") == "x", "nested objects");
        check(value->get("empty")->members.empty() && value->get("none")->items.empty(), "empty object and array");
        check(value->text("a").empty() && !value->get("missing"), "a non-string and a missing member");
        for (const char* bad : {"", "{", "{\"a\" 1}", "[1,]", "\"open", "{\"a\":1} x", "tru", "\"\x01\"", "01x"})
            check(!parseJson(bad), bad);
        check(!parseJson(std::string(100, '[') + std::string(100, ']')), "nesting beyond 64 levels");
    });
    // GitHub's answer for v0.2.6 (October 8), the user objects and URLs Spidy does not read cut short.
    const std::string latest = R"({"url":"https://api.github.com/repos/IlyaMez/SpidyVR/releases/407178026",
"html_url":"https://github.com/IlyaMez/SpidyVR/releases/tag/v0.2.6","id":407178026,
"author":{"login":"github-actions[bot]","id":41898282,"type":"Bot","site_admin":false},
"tag_name":"v0.2.6","target_commitish":"main","name":"Spidy 0.2.6","draft":false,"immutable":false,
"prerelease":false,"created_at":"2026-10-08T19:13:59Z","assets":[{"url":"https://api.github.com/x/622754987",
"id":622754987,"name":"MilesVR-0.2.6-win64.zip","label":"","uploader":{"login":"github-actions[bot]","id":41898282},
"content_type":"application/zip","state":"uploaded","size":12780581,
"digest":"sha256:53d31980e78e2334b7274f2c3c07f627b2afbe7b76961a68bf5e565ef3ae9031","download_count":390,
"browser_download_url":"https://github.com/IlyaMez/SpidyVR/releases/download/v0.2.6/MilesVR-0.2.6-win64.zip"}],
"body":"Extract the zip anywhere you can write and start **Spidy Launcher.exe**.\r\n\r\nSHA-256 of MilesVR-0.2.6-win64.zip: `53D31980E78E2334B7274F2C3C07F627B2AFBE7B76961A68BF5E565EF3AE9031`\r\n\r\n## Changes since v0.2.5\r\n\r\n- Implement slow motion\r\n- Keep A pressed in the air\r\n"})";
    test("the newest release: its zip, size, SHA-256 and changes", [&] {
        std::string error;
        const auto release = parseRelease(latest, error);
        check(release.has_value(), error.c_str());
        check(release->version == "0.2.6" && release->zipName == "MilesVR-0.2.6-win64.zip", "version and zip");
        check(release->zipUrl == "https://github.com/IlyaMez/SpidyVR/releases/download/v0.2.6/MilesVR-0.2.6-win64.zip",
              "download address");
        check(release->zipSize == 12780581, "size");
        check(release->sha256 == "53d31980e78e2334b7274f2c3c07f627b2afbe7b76961a68bf5e565ef3ae9031", "digest");
        check(release->page == "https://github.com/IlyaMez/SpidyVR/releases/tag/v0.2.6", "page");
        check(release->changes == std::vector<std::string>{"Implement slow motion", "Keep A pressed in the air"},
              "the list of changes, line ends dropped");
    });
    test("a release without a digest uses its notes' SHA-256; without either it is refused", [&] {
        const auto replace = [&](std::string text, const std::string& from, const std::string& to) {
            return text.replace(text.find(from), from.size(), to);
        };
        const std::string digest = "\"sha256:53d31980e78e2334b7274f2c3c07f627b2afbe7b76961a68bf5e565ef3ae9031\"";
        std::string error;
        const auto notes = parseRelease(replace(latest, digest, "null"), error);
        check(notes && notes->sha256 == "53d31980e78e2334b7274f2c3c07f627b2afbe7b76961a68bf5e565ef3ae9031",
              "from the notes, in lower case");
        const std::string unchecked = replace(replace(latest, digest, "null"), "SHA-256 of", "Hash of");
        check(!parseRelease(unchecked, error) && error.find("no SHA-256") != std::string::npos, error.c_str());
        check(!parseRelease(replace(latest, "\"name\":\"MilesVR-0.2.6-win64.zip\"", "\"name\":\"other.zip\""), error) &&
                  error == "MilesVR 0.2.6 has no MilesVR-0.2.6-win64.zip to download.",
              "another asset only");
        check(!parseRelease(replace(latest, "\"https://github.com/IlyaMez/SpidyVR/releases/download",
                                    "\"http://github.com/IlyaMez/SpidyVR/releases/download"),
                            error),
              "a download address that is not https");
        check(!parseRelease(replace(latest, "\"v0.2.6\"", "\"nightly\""), error) &&
                  error == "The newest release has no version number (its tag is \"nightly\").",
              "a tag without a version");
        check(!parseRelease("<html>rate limited</html>", error) && error == "GitHub's answer could not be read.",
              "not JSON");
    });
    test("versions: three numbers, newer field by field", [] {
        for (const char* good : {"0.2.6", "1.0.0", "10.20.300"})
            check(releaseVersion(good), good);
        for (const char* bad : {"", "0.2", "0.2.6.1", "0.2.", ".2.6", "0..6", "v0.2.6", "0.2.6-rc1", "1.123456.0"})
            check(!releaseVersion(bad), bad);
        check(newerVersion("0.2.7", "0.2.6") && newerVersion("0.10.0", "0.9.9") && newerVersion("1.0.0", "0.99.99"),
              "newer");
        check(!newerVersion("0.2.6", "0.2.6") && !newerVersion("0.2.5", "0.2.6"), "the same or older");
    });
    test("an update replaces and adds the package's files and retires stale ones in its folders only", [] {
        const std::vector<std::wstring> installed{
            L"MilesVR Launcher.exe", L"README.txt", L"my notes.txt", L"tools/run_game_vr.py", L"Tools/Old_Tool.py",
            L"tools/__pycache__/x.pyc", L"python/python312.dll", L"build/windows-ninja/spidy_bridge.dll",
            L"build/windows-ninja/spidy_gone.dll", L"reports/game-vr-20261008-120000.json", L"saves/mine.txt"};
        const std::vector<std::wstring> package{L"MilesVR Launcher.exe", L"README.txt", L"tools/run_game_vr.py",
                                                L"tools/new_tool.py", L"python/python313.dll",
                                                L"build/windows-ninja/SPIDY_BRIDGE.dll", L"docs/licenses/MIT.txt"};
        const auto plan = updatePlan(installed, package);
        check(plan.replace == std::vector<std::wstring>{L"MilesVR Launcher.exe", L"README.txt", L"tools/run_game_vr.py",
                                                         L"build/windows-ninja/SPIDY_BRIDGE.dll"},
              "the package's files already there, case aside");
        check(plan.add == std::vector<std::wstring>{L"tools/new_tool.py", L"python/python313.dll",
                                                     L"docs/licenses/MIT.txt"},
              "new files");
        check(plan.retire == std::vector<std::wstring>{L"Tools/Old_Tool.py", L"python/python312.dll",
                                                        L"build/windows-ninja/spidy_gone.dll"},
              "stale files in the package's folders; the player's files beside the launcher, subfolders the "
              "package has no files in, reports and other folders stay");
    });
    std::cout << (total - failed) << '/' << total << " launcher checks passed\n";
    return failed ? 1 : 0;
}

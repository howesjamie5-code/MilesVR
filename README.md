<h1 align="center">MilesVR</h1>

<p align="center">
  <b>VR for Marvel's Spider-Man: Miles Morales</b><br>
  Your head is the camera, your hands shoot the webs, your fists throw the punches - and Miles's venom is yours too.
</p>

<p align="center">
  <a href="../../releases/latest"><b>Download the latest release</b></a>
</p>

MilesVR is a free PC VR mod for **Marvel's Spider-Man: Miles Morales** on Steam. It is a port of
[SpidyVR](https://github.com/IlyaMez/SpidyVR) by IlyaMez (the VR mod for Marvel's Spider-Man
Remastered) to Miles Morales, with Miles's own powers added. All the credit for the VR core -
the view, swinging, webs, fighting and the launcher - goes to SpidyVR. MilesVR is an unofficial
fan project, not made by or connected with Insomniac Games, Sony or Marvel.

It changes nothing in the game's folder: it attaches to the game while it runs.

## Features

From SpidyVR:

- Full VR view of the game, with the HUD on a panel.
- Swing with your own hands: each hand shoots its own web, reel in, zip, climb a web.
- Grab props and enemies with webs and throw them.
- Fist fighting: punches, web strikes that fly you to an enemy, uppercuts, slams, combos,
  called blows, finishers, slow motion, Spider-Sense warnings and counters.
- The phone from your back pocket, with a music player for your own songs.
- Walk, run, jump and dodge with the controllers.

New for Miles:

- **Venom meter** with up to three bars, filled by your blows.
- **Venom Punch**, **Venom Jump**, **Venom Smash** and the **Mega Venom Blast**, all done with
  gestures.
- **Camouflage** from your left hand at your temple.
- A launcher in Miles's colours, and a MILES VR tab in the game's settings.

## What you need

- **Marvel's Spider-Man: Miles Morales on Steam** (the current Steam version).
- **A PC VR headset with an OpenXR runtime.** Played on Quest 3 with
  [Virtual Desktop](https://www.vrdesktop.net/); SteamVR headsets should work too.
- **A gaming PC** on Windows.

## Install and play

1. Download the zip from [Releases](../../releases/latest) and extract it to a folder you can
   write to, such as `Documents\MilesVR`.
2. Start **MilesVR Launcher.exe**. If Windows says it protected your PC, choose *More info*,
   then *Run anyway*.
3. Fix anything the launcher marks red (it can install the Visual C++ runtime for you).
4. Connect your headset, close the game, and press **START VR**.

Keep the game window in front on the desktop: the game pauses behind other windows.

From 0.1.37 the launcher checks this page's releases as it starts and offers each new MilesVR
with an **Update now** button: your settings, reports and songs stay.

For the phone's music, put mp3, wav, wma, m4a or aac files in the `Music` folder next to
`MilesVR Launcher.exe`.

## Controls

The launcher's **Controls** tab lists every control. The basics:

| Do this | With |
|---|---|
| Shoot a web, swing on it | Squeeze a **grip**; let go to release |
| Reel in / web ball | **Trigger** (with a web attached it reels, without one it shoots a web ball) |
| Zip | Pull your hand sharply away from the web's anchor |
| Punch | Throw a fast fist at an enemy; at one up to 10 m away you fly in and strike |
| Walk, run / jump / dodge | **Left stick** / **A** / **B** |
| Turn | **Right stick** |
| Slow motion | Click the **left stick** |
| Interact | **Y** |
| Scan | **Right** hand to your temple, pull its **trigger** |
| Camouflage | **Left** hand to your temple, pull its **trigger** |
| Stealth takedown | Behind an unaware enemy: squeeze **both grips** and **push both hands** at him, hold 1.5 s - he's webbed up. From a perch: **point both hands** at him, **both grips**, **pull up hand over hand** - he's left hanging |
| Phone | Reach to your back pocket, squeeze the grip, bring it up in front of you; **B** puts it away |
| Pause | **Menu**, or hold both sticks in |

### Venom powers

The venom hand (the right one, unless you set VENOM HAND to LEFT) has a **web button** and a
**venom button**, always opposite buttons (launcher Play tab, or WEB BUTTON / VENOM BUTTON in the
game's MILES VR settings; change one and the other switches):

| Web button | Venom button | Web button does | Venom button does |
|---|---|---|---|
| **Grip** (default) | **Trigger** | Webs and swinging, always | Web balls; reels with a web attached; with a bar full, tap: web ball, hold: charge |
| **Trigger** | **Grip** | Webs and swinging, always | Web balls; reels with a web attached; with a bar full, tap: web ball, hold: charge |

With a web attached, the venom button reels in. Hold the venom button until the fist glows and the controller buzzes, then:

| Venom power | How | Cost |
|---|---|---|
| Venom Punch | Punch the marked enemy. Within 2 m your fist lands it; farther away the web strike flies you in | 1 bar |
| Venom Jump | In a fight, standing, punch straight **up**: enemies within 5 m go up into the air, and you jump up with them | 1 bar |
| Venom Smash | Punch straight **down**, standing or in the air: everyone within 5 m is hit and thrown | 1 bar |
| Mega Venom Blast | With all three bars full, squeeze **both grips together**: a boom and a moment of slow motion | 3 bars |
| Let the venom go | Click the venom hand's stick: the meter empties and your webs are back | all |

## Reports

Every VR session saves a report in the `reports` folder of the build. When you report a problem,
attach the newest one.

## Building from source

Needs Visual Studio 2022 C++ Build Tools with CMake and Ninja, and a Windows SDK.

```powershell
.\tools\bootstrap.ps1 -Observer   # dependencies, including Dear ImGui for the launcher
.\tools\build.ps1 -Observer       # build\windows-ninja
.\build\windows-ninja\spidy_tests.exe
.\tools\package.ps1               # dist\MilesVR-<version>-win64.zip
```

## License

GPL-3.0, as SpidyVR is: see [LICENSE](LICENSE). Third-party parts are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

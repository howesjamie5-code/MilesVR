"""Start the game with a small desktop view for VR, and restore the display settings afterwards.

In immersive VR the game's own view must keep running: the engine does occlusion,
key-light shadow setup, and auto-exposure for it, and Spidy places it at the head
so that work fits the eyes. Its pixels are not needed. A small window keeps that
work and drops most of its rendering cost (a 3440 x 1440 desktop renders about a
fifth of the pixels of two 3072 x 3264 eyes). The game reads these values once at
startup. The previous values are saved in reports/ and written back once the game
has closed.

Every VR session also turns off four game settings, whatever its window (SESSION):
frame generation, vertical sync, ray-traced reflections, and the game's
Windows.Gaming.Input, which under SteamVR takes the controllers away from Spidy's
XInput controller. It puts the game's Aspect Ratio at Auto. They are saved and
restored with the window's. So are the game's depth of field and its sharpening,
which a session turns off unless told to keep them (PICTURE).
"""
import argparse
import ctypes
import json
import pathlib
import sys
import winreg

ROOT = pathlib.Path(__file__).resolve().parents[1]
KEY = r"Software\Insomniac Games\Marvel's Spider-Man Remastered\Graphics"
INPUT_KEY = r"Software\Insomniac Games\Marvel's Spider-Man Remastered\Input"
BACKUP = ROOT/'reports'/'desktop-view-before-vr.json'
NAMES = ('Fullscreen', 'ExclusiveFullscreen', 'WindowMaximized', 'WindowLeft', 'WindowTop',
         'WindowWidth', 'WindowHeight', 'UserWindowWidth', 'UserWindowHeight')
# The window's rows and shape. VR's HUD panel draws the game's HUD at the window's size
# (src/native_hud.cpp), and the game sizes its HUD by the window's height: 1920 x 1080 gives the
# HUD half again the pixels of the 1720 x 720 (the desktop's shape) of October 9's fifth build,
# whose HUD a Quest 3 player still found soft, and its 16:9 layout, as on a console, puts the same
# HUD on a panel as wide with a third more height, so all of it shows a third larger. Next to two
# 3840 x 4080 eyes, the frame draws about 3% more pixels than at 1720 x 720. No more than three
# quarters of the desktop's height.
HEIGHT = 1080
ASPECT = 16/9
# What every VR session changes, whatever its window: (key, values, create). A value the game never
# saved is created only where `create` is set, and deleted again afterwards.
# - Frame generation (DLSS or FSR) off: Spidy draws the eyes itself, and generated frames change how
#   the game presents its window (which the headset's game screen copies) on a queue of their own.
#   Every tested session had it off.
# - Vertical sync off: with it the game waits for the monitor before each new frame, so it runs at the
#   monitor's rate divided by a whole number (120, 60, 40, 30 a second on a 120 Hz display) whatever the
#   headset's: the October 10 session, started with it on, ran 36-45 ms frames. The headset's own frames
#   do not wait for the game's.
# - Windows.Gaming.Input off: in the Steam Link session of October 7 the game found two Steam virtual
#   gamepads (Valve 28de:11ff) there, then logged "XInput disabled, disconnecting controller" and never
#   read Spidy's XInput controller again (pad_reads 0): its menus did not move. At 0 the game skips
#   Windows.Gaming.Input altogether (Spider-Man.exe 1d14c89); XInput and DualSense (libScePad) stay.
# - Depth of field off (DOFQuality 0) and sharpening at its lowest (Sharpness 1, where the game's slider
#   stores its 0), since October 12: the game blurs by distance from its own camera's focus, which in the
#   headset is not where you look (the 3D game screen's cutscenes most of all), and its sharpening pass
#   rings and shimmers on edges at the eyes' resolution. `keep` in prepare_session leaves either as the
#   player has it (the launcher's "Game's depth of field" and "Game's sharpening").
# - Ray-traced reflections off (RTReflections 0), since October 13. The eyes cannot have them (the game
#   takes another way through its reflection pass, for which an eye's buffers have no targets), and the
#   game's own view, the only one that had them, rests in VR play (restGameView in src/stereo_probe.cpp).
#   With them on, the first frame that view drew again hung the graphics device: a menu, the map or a
#   cutscene ended the game with its display driver error (DXGI_ERROR_DEVICE_HUNG). In two players'
#   reports (October 11-13, an RTX 4070 Ti and an RTX 5080) every session ended on its first such frame,
#   three more players ended the error by turning the setting off, and none of the 30 game logs kept
#   here with it off has the error. With it off the eyes follow Screen Space Reflections. A game that
#   runs with it on all the same keeps its own view rendering (ray_traced_view in run_game_vr.py).
# - Aspect Ratio at Auto (ForcedAspectRatio 0), since October 13's third build. The value is the choice
#   in the game's Display settings, counted from Auto, and the option reads and writes nothing else
#   (Spider-Man.exe 1cc1e70 and 1cca6d0; AspectRatio beside it is a number of its own). On October 11
#   two players' games ended as VR started, and each had VR once the option was back at Auto, which a
#   third player had advised. One of the two named his choice, 16:9, and his report of that day has the
#   graphics device gone by the 30th eye picture, in a 1920 x 1080 window. It was not run here with a
#   fixed shape, so what the game does to an eye with one is not known.
PICTURE = dict(depth_of_field='DOFQuality', sharpening='Sharpness')
SESSION = ((KEY, dict(DLSSG=0, FrameGen=0, VSync=0, DOFQuality=0, Sharpness=1, RTReflections=0,
                      ForcedAspectRatio=0), False),
           (INPUT_KEY, dict(EnableWindowsGamingInput=0), True))


class Registry:
    """The game's keys; only REG_DWORD values are read or written (by default NAMES in the graphics key)."""

    def read(self, names=NAMES, key=KEY):
        values = {}
        try:
            opened = winreg.OpenKey(winreg.HKEY_CURRENT_USER, key)
        except FileNotFoundError:
            return values
        with opened:
            for name in names:
                try:
                    value, kind = winreg.QueryValueEx(opened, name)
                except FileNotFoundError:
                    continue
                if kind == winreg.REG_DWORD:
                    values[name] = value
        return values

    def write(self, values, key=KEY):
        """None deletes a value: the session created it."""
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, key, 0, winreg.KEY_SET_VALUE) as opened:
            for name, value in values.items():
                if value is None:
                    try:
                        winreg.DeleteValue(opened, name)
                    except FileNotFoundError:
                        pass
                else:
                    winreg.SetValueEx(opened, name, 0, winreg.REG_DWORD, value)


def desktop():
    """Primary display size in physical pixels."""
    user32 = ctypes.windll.user32
    user32.SetProcessDPIAware()
    return user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)


def small_window(screen_width, screen_height, height=HEIGHT, aspect=ASPECT):
    """Centred 16:9 window `height` rows tall, within three quarters of the desktop's height and its width."""
    height = min(height, screen_height*3//4)
    width = round(height*aspect)
    if width > screen_width:
        width = screen_width
        height = round(width/aspect)
    return dict(Fullscreen=0, ExclusiveFullscreen=0, WindowMaximized=0,
                WindowLeft=(screen_width-width)//2, WindowTop=(screen_height-height)//2,
                WindowWidth=width, WindowHeight=height, UserWindowWidth=width, UserWindowHeight=height)


def pending(backup=BACKUP):
    return backup.exists()


def session_changes(backup=BACKUP):
    """The player's own values of the settings a session replaced (SESSION), by name, from the backup a
    launch left; empty without one. None: a value the session made."""
    if not backup.exists():
        return {}
    session = json.loads(backup.read_text()).get('session', [])
    return {name: value for entry in session for name, value in entry['values'].items()}


def _saved(values, names, created=False):
    """Values a backup may hold: known names with REG_DWORD numbers, and None for one the session created."""
    return isinstance(values, dict) and all(
        name in names and ((created and value is None) or (isinstance(value, int) and 0 <= value < 1 << 32))
        for name, value in values.items())


def restore(registry=None, backup=BACKUP):
    """Write back the values saved before VR. Returns False when nothing was saved."""
    if not backup.exists():
        return False
    saved = json.loads(backup.read_text())
    values = saved.get('values') if saved.get('key') == KEY else None
    # Backups before October 7's SteamVR fix hold the window's values only.
    session = saved.get('session', [])
    allowed = {key: (tuple(wanted), create) for key, wanted, create in SESSION}
    known = isinstance(session, list) and all(
        isinstance(entry, dict) and entry.get('key') in allowed and
        _saved(entry.get('values'), *allowed[entry['key']]) for entry in session)
    if not _saved(values, NAMES) or not known:
        raise RuntimeError(f'Unrecognized display backup: {backup}')
    registry = registry or Registry()
    # Only what differs now: where Windows refused the launch's changes, the user's values are still there,
    # and writing them again would be refused too.
    for key, wanted in [(KEY, values)]+[(entry['key'], entry['values']) for entry in session]:
        current = registry.read(tuple(wanted), key)
        changed = {name: value for name, value in wanted.items() if current.get(name) != value}
        if changed:
            registry.write(changed, key)
    backup.unlink()
    return True


def shrink(registry=None, backup=BACKUP, screen=None):
    """Save the current values (once) and select the small window. Returns the values written."""
    registry = registry or Registry()
    current = registry.read()
    if not current:
        return None  # the game has not saved display settings yet
    if not backup.exists():
        backup.parent.mkdir(parents=True, exist_ok=True)
        backup.write_text(json.dumps(dict(key=KEY, values=current), indent=2)+'\n')
    wanted = {name: value for name, value in small_window(*(screen or desktop())).items() if name in current}
    registry.write(wanted)
    return wanted


def prepare_session(registry=None, backup=BACKUP, keep=()):
    """Turn off what SESSION lists, adding the user's values to the backup (made here if the window
    keeps the user's settings). keep: PICTURE names ('depth_of_field', 'sharpening') to leave as the
    player has them. Returns the values written."""
    registry = registry or Registry()
    kept = {PICTURE[name] for name in keep}
    saved = json.loads(backup.read_text()) if backup.exists() else dict(key=KEY, values={})
    if 'session' not in saved:
        saved['session'] = []
        for key, wanted, create in SESSION:
            current = registry.read(tuple(wanted), key)
            saved['session'].append(dict(key=key, values={name: current.get(name) for name in wanted
                                                          if (name in current or create) and name not in kept}))
        backup.parent.mkdir(parents=True, exist_ok=True)
        backup.write_text(json.dumps(saved, indent=2)+'\n')
    before = {entry['key']: entry['values'] for entry in saved['session']}
    written = {}
    for key, wanted, _ in SESSION:
        values = {name: value for name, value in wanted.items() if name in before.get(key, {})}
        if values:
            registry.write(values, key)
            written.update(values)
    return written


def prepare_launch(small=True, registry=None, backup=BACKUP, screen=None, keep=()):
    """Called with the game closed, just before it starts. A previous session's values are restored
    first, so a backup always holds the user's own settings. keep: as prepare_session's. Returns the
    window's values written (None when the window keeps the user's settings).

    Raises OSError when a settings change or its backup fails. Any changes made during this attempt
    are restored where possible; if restoration also fails, the backup stays for a later retry."""
    registry = registry or Registry()
    restore(registry, backup)
    try:
        window = shrink(registry, backup, screen) if small else None
        prepare_session(registry, backup, keep)
    except OSError:
        try:
            restore(registry, backup)
        except OSError:
            pass  # the backup stays; it is restored after the game closes, or at the next launch
        raise
    return window


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--restore', action='store_true', help='write back the display values saved before VR')
    a = p.parse_args()
    if not a.restore:
        print('Restore pending.' if pending() else 'No display values are waiting to be restored.')
        return 0
    from capture_game_state import find_game
    try:
        find_game()
        print('Close Spider-Man first; it may save its window settings when it exits.', file=sys.stderr)
        return 1
    except RuntimeError as error:
        if not str(error).startswith('Spider-Man is not running.'):
            raise
    print('Restored your display settings.' if restore() else 'No display values are waiting to be restored.')
    return 0


if __name__ == '__main__':
    sys.exit(main())

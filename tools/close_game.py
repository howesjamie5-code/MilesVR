"""Close the running game: ask its window to close, and end the process if it has not left in time.

Then restore the display settings a VR-style launch saved (vr_display); --keep-settings leaves them
for the next launch. For checks without a headset.
"""
import ctypes as c
import subprocess
import sys
import time
from capture_game_state import find_game
from probe_game_screen import game_window, user32
import vr_display


def running():
    try:
        return find_game()
    except RuntimeError as error:
        if str(error).startswith('Spider-Man is not running.'):
            return None
        raise


def main():
    pid = running()
    if pid:
        try:
            user32.PostMessageW(game_window(pid), 0x0010, 0, 0)  # WM_CLOSE
        except (OSError, RuntimeError):
            pass
        deadline = time.monotonic()+12
        while running() and time.monotonic() < deadline:
            time.sleep(.25)
        if running():
            subprocess.run(['taskkill', '/F', '/PID', str(pid)], capture_output=True)
            time.sleep(1)
    # Steam drops a launch asked for while the last process is still leaving.
    time.sleep(3)
    print('Game closed.' if not running() else 'The game is still running.', flush=True)
    if '--keep-settings' not in sys.argv[1:] and not running() and vr_display.pending():
        print('Restored display settings.' if vr_display.restore() else 'Nothing to restore.', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())

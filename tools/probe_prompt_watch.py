"""Watches the game's own prompts in the running game, with no headset.

What the game asks a player for arrives through its UI's call wrapper, which game_menu hooks; game_prompts
reads it from there (see its header). A VR session turns that reading on, and so does this, so a prompt can be
measured without the headset:

    python tools/probe_menu_pad.py start          # launch the game
    python tools/probe_menu_pad.py pad a --until-player
    python tools/probe_prompt_watch.py --seconds 40 --press y

It prints the timeline - the game's prompt calls and this probe's presses, in order, each stamped in
milliseconds - plus the census of every name the UI called with one example of its arguments, and the player's
position and wall state per sample. `--press y` sends the game's own Y (its interact) every couple of seconds,
which is the question left open on October 13: the game announced an interact, took 12 presses of its Y from a
wall, and did nothing.
"""
import argparse
import ctypes as c
import json
import struct
import time

from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from observe_game import call_remote, call_with_payload
import probe_menu_pad as pad

# SpidySwingSample belongs to the ray bridge, not this module, so where the player stands comes from the
# session report instead; everything here is the stereo probe's own.
EXPORTS = ('SpidyMenuStart', 'SpidyMenuSample', 'SpidyMenuStop', 'SpidyPromptsWatch', 'SpidyPromptsWatched',
           'SpidyPromptsNote', 'SpidyPadSubmit')
# game_prompts::Watched, pinned by a static_assert in its header.
WATCHED = 12096
CALLS_AT = 80
LINE_AT, LINES, LINE_CHARS = CALLS_AT + 44, 32, 128
NAMES_AT = LINE_AT + LINES * LINE_CHARS
CENSUS_AT = NAMES_AT + 4 + 48 * 4
CENSUS_CHARS = 160
PROMPT_NAMES = ('ShowPlaceablePrompt', 'HidePlaceablePrompt', 'SetPlaceablePromptProgress', 'UpdateInWorldPrompt',
                'POIInteractAvailable', 'AddQTE', 'UpdateQTE', 'ClearQTE', 'ShowText', 'HideText')


def text_at(raw, at, size):
    return raw[at:at + size].split(b'\0', 1)[0].decode('utf-8', 'replace')


def watched(game, process, exports):
    """Everything the live reading has seen."""
    code, raw = pad.call_with_output(game, process, exports['SpidyPromptsWatched'], b'', WATCHED)
    if code or len(raw) != WATCHED:
        return dict(error=f'SpidyPromptsWatched: {code}')
    magic, version, size, reading = struct.unpack_from('<4I', raw)
    if (magic, version, size) != (0x5750524D, 1, WATCHED):
        return dict(error=f'Watched mismatch: {hex(magic)} v{version} {size} bytes')
    out = dict(reading=bool(reading))
    out.update(zip(('invokes', 'calls', 'raised', 'cleared', 'texts', 'rewrites'),
                   struct.unpack_from('<6Q', raw, 16)))
    out.update(zip(('demand', 'last_demand', 'beyond_play', 'keyboard_texts'), struct.unpack_from('<4I', raw, 64)))
    counts = struct.unpack_from('<10I', raw, CALLS_AT + 4)
    out['by_name'] = {name: n for name, n in zip(PROMPT_NAMES, counts) if n}
    lines = struct.unpack_from('<I', raw, CALLS_AT)[0]
    out['timeline'] = [t for t in (text_at(raw, LINE_AT + i * LINE_CHARS, LINE_CHARS)
                                   for i in range(min(lines, LINES))) if t]
    names = struct.unpack_from('<I', raw, NAMES_AT)[0]
    calls = struct.unpack_from('<48I', raw, NAMES_AT + 4)
    census = [dict(call=text_at(raw, CENSUS_AT + i * CENSUS_CHARS, CENSUS_CHARS), times=calls[i])
              for i in range(min(names, 48))]
    out['census'] = sorted((c for c in census if c['call']), key=lambda c: -c['times'])
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--seconds', type=float, default=40, help='how long to watch')
    p.add_argument('--press', default='', help="buttons to send every --every seconds: y, lbrb, lt_rt, x, a, b")
    p.add_argument('--every', type=float, default=2.5)
    p.add_argument('--rewrite', action='store_true', help="also rewrite the prompts' texts for VR")
    p.add_argument('--output', default=str(ROOT / 'reports/prompt-watch.json'))
    a = p.parse_args()

    game = Game(find_game())
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    if not process:
        raise c.WinError(c.get_last_error())
    try:
        exports, _ = prepare(game.pid, process, pad.XR_DLL, ROOT / 'reports/stereo-modules', EXPORTS)
        # The menu's hooks are what carry the prompt reading, so they go in first. Settings values are
        # whatever; nothing here opens the tab.
        from probe_menu import settings_payload, FLAGS
        flags = FLAGS['web_grab'] | FLAGS['punch'] | FLAGS['body'] | FLAGS['air_webs'] | FLAGS['stand_on_walls']
        code = call_with_payload(process, exports['SpidyMenuStart'], settings_payload(flags, 30, 100, 1, 32.0))
        if code not in (0, 9301, 9302, 9303):      # already hooked by a session is fine
            print(json.dumps(dict(warning=f'SpidyMenuStart: {code}')), flush=True)
        code = call_with_payload(process, exports['SpidyPromptsWatch'],
                                 struct.pack('<I', 2 if a.rewrite else 1))
        if code:
            raise RuntimeError(f'SpidyPromptsWatch: {code}')

        report = dict(pid=game.pid, samples=[])
        presses = [b for b in a.press.split(',') if b]
        started = time.monotonic()
        next_press = started
        while time.monotonic() - started < a.seconds:
            now = round(time.monotonic() - started, 2)
            if presses and time.monotonic() >= next_press:
                next_press = time.monotonic() + a.every
                for name in presses:
                    note = f"the probe's {name}".encode()
                    call_with_payload(process, exports['SpidyPromptsNote'], note.ljust(96, b'\0'))
                    if name == 'y':
                        pad.pad(process, exports, pad.BUTTONS['y'], hold_ms=700)
                    elif name == 'lbrb':
                        pad.pad(process, exports, pad.BUTTONS['lb'] | pad.BUTTONS['rb'], hold_ms=1200)
                    elif name == 'lt_rt':
                        pad.pad(process, exports, 0, hold_ms=1200, triggers=(255, 255))
                    else:
                        pad.pad(process, exports, pad.BUTTONS[name], hold_ms=700)
                    time.sleep(.4)
            report['samples'].append(dict(seconds=now))
            time.sleep(.5)
        report.update(watched(game, process, exports))
        (ROOT / 'reports').mkdir(parents=True, exist_ok=True)
        with open(a.output, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
        print(json.dumps({k: v for k, v in report.items() if k != 'samples'}, indent=1), flush=True)
        print(f"\n{len(report['samples'])} samples, report {a.output}", flush=True)
    finally:
        close(process)


if __name__ == '__main__':
    main()

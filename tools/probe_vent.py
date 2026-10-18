"""Drives the game to a saved spot with the virtual pad and reads its prompts there.

One attach, then a little script of steps, so a place in the game can be reached and watched without a
headset and without a dozen process attaches:

    python tools/probe_vent.py a*6@2 shot:menu                 # past the intro, look at the title screen
    python tools/probe_vent.py down down a shot:slots          # pick the third profile
    python tools/probe_vent.py watch walk:6 press:y watched    # run forward, interact, read the prompts

Steps:
    <button>      one press (a b x y up down left right start back lb rb ls rs), `name*N@S` repeats it
    walk:SECONDS  holds the left stick forward (add `,X,Y` for another direction)
    press:NAME    a press with a note in the prompt timeline beside the game's own calls
    watch         turns the prompt reading on (needs the menu hooks; it puts them in)
    watched       prints the counters, the timeline and the census, and writes the report
    calls[:N]     the same in one or two lines, with the last N timeline lines
    front         brings the game's window to the front (it pauses behind another)
    look:SECONDS  turns the camera with the right stick (add `,X,Y`)
    shot:NAME     a screenshot of the game window
    wait:SECONDS  does nothing for a while
    release       hands the player's controller back (the pad holds input slot 0 until this)
"""
import argparse
import ctypes as c
import json
import struct
import sys
import time

from bridge_game import ROOT, prepare
from vr_launcher import bring_to_front
from capture_game_state import Game, find_game, open_process, close
from observe_game import call_remote, call_with_payload
import probe_menu_pad as pad
import probe_prompt_watch as watch

EXPORTS = ('SpidyPadSubmit', 'SpidyPadSample', 'SpidyPadRelease', 'SpidyPlayerFind', 'SpidyMenuStart',
           'SpidyPromptsWatch', 'SpidyPromptsWatched', 'SpidyPromptsNote')


def hold(process, xr, seconds, sticks=(0, 0, 0, 0), buttons=0):
    """Keeps the pad at one reading for a while; a lease is two seconds at most, so it is renewed."""
    until = time.monotonic()+seconds
    while time.monotonic() < until:
        pad.pad(process, xr, buttons, hold_ms=2000, sticks=sticks)
        time.sleep(.35)


def note(process, xr, text):
    call_with_payload(process, xr['SpidyPromptsNote'], text.encode()[:95].ljust(96, b'\0'))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('steps', nargs='+')
    p.add_argument('--output', default=str(ROOT/'reports/vent-watch.json'))
    a = p.parse_args()

    game = Game(find_game())
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    if not process:
        raise c.WinError(c.get_last_error())
    try:
        xr, _ = prepare(game.pid, process, pad.XR_DLL, ROOT/'reports/stereo-modules', EXPORTS)
        for step in a.steps:
            name, _, argument = step.partition(':')
            times, every = 1, .8
            if '*' in name:
                name, _, repeat = name.partition('*')
                repeat, _, gap = repeat.partition('@')
                times, every = int(repeat), float(gap or .8)
            if name in pad.BUTTONS:
                for i in range(times):
                    pad.pad(process, xr, pad.BUTTONS[name], hold_ms=200)
                    if i+1 < times:
                        time.sleep(every)
                print(f'pressed {name} x{times}', flush=True)
            elif name == 'press':
                note(process, xr, f"the probe's {argument}")
                pad.pad(process, xr, pad.BUTTONS[argument], hold_ms=700)
                print(f'pressed {argument} with a note', flush=True)
            elif name == 'walk':
                parts = argument.split(',')
                seconds = float(parts[0])
                x, y = (float(parts[1]), float(parts[2])) if len(parts) > 2 else (0., 1.)
                note(process, xr, f'the probe walks {x:+.1f},{y:+.1f} for {seconds:.0f} s')
                hold(process, xr, seconds, (round(x*32767), round(y*32767), 0, 0))
                print(f'walked {x:+.1f},{y:+.1f} for {seconds} s', flush=True)
            elif name == 'look':
                parts = argument.split(',')
                seconds = float(parts[0])
                x, y = (float(parts[1]), float(parts[2])) if len(parts) > 2 else (1., 0.)
                hold(process, xr, seconds, (0, 0, round(x*32767), round(y*32767)))
                print(f'looked {x:+.1f},{y:+.1f} for {seconds} s', flush=True)
            elif name == 'watch':
                from probe_menu import settings_payload, FLAGS
                flags = FLAGS['web_grab'] | FLAGS['punch'] | FLAGS['body'] | FLAGS['air_webs']
                code = call_with_payload(process, xr['SpidyMenuStart'], settings_payload(flags, 30, 100, 1, 32.0))
                mode = 0 if argument == 'off' else 2 if argument == 'rewrite' else 1
                started = call_with_payload(process, xr['SpidyPromptsWatch'], struct.pack('<I', mode))
                print(f'menu hooks {code}, prompt reading {started}', flush=True)
            elif name == 'watched':
                report = watch.watched(game, process, xr)
                with open(a.output, 'w', encoding='utf-8') as f:
                    json.dump(report, f, indent=1)
                print(json.dumps(report, indent=1), flush=True)
            elif name == 'calls':
                w = watch.watched(game, process, xr)
                print(f"  invokes {w.get('invokes')} calls {w.get('calls')} texts {w.get('texts')} "
                      f"keyboard {w.get('keyboard_texts')} demand {w.get('demand')} "
                      f"last {w.get('last_demand')} by_name {w.get('by_name')}", flush=True)
                for line in w.get('timeline', [])[-int(argument or 8):]:
                    print('   |', line, flush=True)
                print('   census:', ', '.join(f"{c['call']} x{c['times']}"
                                              for c in w.get('census', [])), flush=True)
            elif name == 'front':
                print(f'game in front: {bring_to_front(game.pid)}', flush=True)
            elif name == 'player':
                player, _ = pad.find_player(game, process, xr)
                print(json.dumps(dict(player=player)), flush=True)
            elif name == 'shot':
                path, mean, front = pad.shot(game, argument or 'vent')
                print(f'{path} mean {mean:.1f}; in front: {front}', flush=True)
            elif name == 'wait':
                time.sleep(float(argument))
            elif name == 'release':
                print(f"pad released: {call_remote(process, xr['SpidyPadRelease'])}", flush=True)
            else:
                raise SystemExit(f'unknown step {step!r}')
    finally:
        close(process)
        game.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())

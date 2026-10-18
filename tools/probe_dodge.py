"""The game's dodge on Spidy's virtual Xbox controller, in the running game without a headset.

In VR the controllers' B is the game's B: its dodge, the way the left stick points (src/game_xr.cpp sends it
through src/game_pad.cpp while the game has the player). This presses that B on the same virtual controller and
measures what the game does with it, against the stick alone:

    walk         the left stick to the left for --hold ms, no button
    dodge        B with the left stick the --way given (to the right unless given) for --hold ms
    dodge_rest   B alone

For each: how far the player went over the ground in the next --seconds and how fast at most (over a tenth of
a second), how far it rose and dropped, and how often the gameplay gate was open meanwhile (the player's own camera committing: a dodge that took the camera away would put the game
screen in front of the player in the headset).

Needs a loaded save with the player standing on open ground or a roof, played with the virtual controller
(`tools/probe_menu_pad.py start`, then `pad a --until-player`). Copy the save folder to reports/backups first.
"""
import argparse
import ctypes as c
import json
import math
import struct
import sys
import time
from bridge_game import snapshot as bridge_snapshot
from capture_game_state import close
from observe_game import call_with_payload
from probe_menu_pad import BUTTONS, GAMEPLAY_MOVERS, attach, find_player, valid_matrix


def press(process, xr, buttons, hold_ms, left=(0, 0)):
    """The press alone: the caller samples while the game has it."""
    state = struct.pack('<H2B4h', buttons, 0, 0, *(round(v*32767) for v in left), 0, 0)
    code = call_with_payload(process, xr['SpidyPadSubmit'], state+struct.pack('<I', hold_ms))
    if code:
        raise RuntimeError(f'Virtual controller: {code}')


def follow(game, bridge, seconds):
    """The player's path and the gate, as the bridge has them at each camera commit."""
    frequency, now = c.c_int64(), c.c_int64()
    c.windll.kernel32.QueryPerformanceFrequency(c.byref(frequency))
    path, opened, samples, movers, last = [], 0, 0, {}, None
    end = time.monotonic()+seconds
    while time.monotonic() < end:
        b = bridge_snapshot(game, bridge['SpidyBridgeData'])
        c.windll.kernel32.QueryPerformanceCounter(c.byref(now))
        if b:
            samples += 1
            matrix = lambda t: [*t['basis'][0], 0, *t['basis'][1], 0, *t['basis'][2], 0, *t['position'], 1]
            fresh = b['qpc'] and 0 <= now.value-b['qpc'] < frequency.value/10
            opened += bool(b['state'] == 1 and fresh and valid_matrix(matrix(b['before'])) and
                           valid_matrix(matrix(b['player'])))
            table = game.pointer(b['mover'])
            name = GAMEPLAY_MOVERS.get(table-game.base, hex(table-game.base) if table > game.base else None)
            movers[name] = movers.get(name, 0)+1
            if b['qpc'] != last and valid_matrix(matrix(b['player'])):
                last = b['qpc']
                path.append((b['qpc']/frequency.value, b['player']['position']))
        time.sleep(.005)
    if not path:
        return dict(horizontal=None, gate_open=0, cameras=movers)
    level = lambda p, q: math.hypot(q[0]-p[0], q[2]-p[2])
    # Over a tenth of a second: a single commit's step is noisy.
    speeds = [level(p, q)/(t-s) for i, (s, p) in enumerate(path) for t, q in path[i+1:i+40] if .1 <= t-s < .13]
    heights = [p[1] for _, p in path]
    return dict(horizontal=round(level(path[0][1], path[-1][1]), 2), top_speed=round(max(speeds, default=0), 1),
                rise=round(max(heights)-heights[0], 2), drop=round(heights[0]-min(heights), 2), commits=len(path),
                gate_open=round(opened/samples, 3) if samples else 0, cameras=movers)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--hold', type=int, default=200, help='milliseconds a press lasts')
    p.add_argument('--seconds', type=float, default=1.5, help='how long the player is followed after each press')
    p.add_argument('--way', type=float, nargs=2, default=(1, 0), metavar=('X', 'Y'),
                   help="the left stick during the dodge, -1..1 (the walk's is the other way)")
    a = p.parse_args()
    game, process, xr, bridge = attach()
    try:
        player, _ = find_player(game, process, xr)
        if not player:
            raise RuntimeError('No player: load a save first (probe_menu_pad.py pad a --until-player)')
        code = call_with_payload(process, bridge['SpidyRetarget'],
                                 struct.pack('<4I3Q', 0x53424346, 1, 40, game.pid, game.base,
                                             int(player['hero'], 16), int(player['record'], 16)))
        if code:
            raise RuntimeError(f'The input bridge did not take the player: {code}')
        time.sleep(.5)
        result = dict(rest=follow(game, bridge, 1))
        for name, buttons, left in (('walk', 0, (-a.way[0], -a.way[1])), ('dodge', BUTTONS['b'], a.way),
                                    ('dodge_rest', BUTTONS['b'], (0, 0))):
            press(process, xr, buttons, a.hold, left)
            result[name] = follow(game, bridge, a.seconds)
            time.sleep(1)
        walk, dodge, alone = result['walk'], result['dodge'], result['dodge_rest']
        # A dodge is a leap of metres within a second; a tap of the stick is a step or two.
        result['dodged'] = bool(dodge['horizontal'] is not None and walk['horizontal'] is not None and
                                dodge['horizontal'] >= 1.5 and dodge['horizontal'] >= 2*walk['horizontal'])
        result['dodged_at_rest'] = bool(alone['horizontal'] is not None and alone['horizontal'] >= .5)
        result['gate_stayed_open'] = min(dodge['gate_open'], alone['gate_open']) >= min(.9, result['rest']['gate_open'])
        print(json.dumps(result, indent=1), flush=True)
        return 0 if result['dodged'] and result['gate_stayed_open'] else 1
    finally:
        close(process)
        game.close()


if __name__ == '__main__':
    sys.exit(main())

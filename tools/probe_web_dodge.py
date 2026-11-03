"""The game's own dodge started and ended for the player by Spidy, in the running game with no headset and no enemy.

    python tools/probe_web_dodge.py [--kind 5] [--times 3]

It needs a running game in free roam with the game window in front, which it brings there. It loads the ray module,
hooks the hero's HeroDodgeManager update (SpidyDodgeStart, src/game_dodge.cpp) and asks SpidyDodgeTest --times times
for a dodge of --kind (5: HeroStateTraversalDodge's, the one a web carrying the player starts): at the next frame of
the hero's manager the game's dodge start (92a040), at the one after its end (929ef0). Each time it records the
dodge's kind before, after the start and after the end (from inside, and the manager's +0xcd8 read from outside
after), the game's answer (0 with no warning standing: none answered) and an error. Then SpidyDodgeData (the hook's
frames) and the hook's entry bytes after SpidyDodgeStop. Writes reports/web-dodge-probe.json.
"""
import argparse
import ctypes as c
import json
import pathlib
import struct
import time

from bridge_game import ROOT, prepare
from capture_game_state import Game, close, find_game, open_process
from inspect_game import PE
from observe_game import call_remote
from probe_game_grab import HERO_LOCAL, registry
from probe_game_screen import user32
from probe_menu_pad import call_with_output
from run_game_vr import dodge_snapshot
from vr_launcher import bring_to_front

# The hooked update, the start and the end: untouched before the probe, the update restored after it.
UPDATE, START, END = 0xad8e60, 0xad5610, 0xad54d0
MANAGER_TABLE = 0x3e10c20
KIND_AT = 0xcb8


def dodge_test(kind, hero):
    """game_dodge.cpp's Test: a dodge of `kind` for the hero whose actor record is `hero`."""
    return struct.pack('<4IQ6I', 0x53444754, 1, 48, kind, hero, 0, 0, 0, 0, 0, 0)


def main():
    try:
        user32.SetProcessDpiAwarenessContext(c.c_void_p(-4))
    except AttributeError:
        user32.SetProcessDPIAware()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kind', type=int, default=5, help="the dodge's kind (5: the traversal dodge's)")
    parser.add_argument('--times', type=int, default=3)
    parser.add_argument('--output', type=pathlib.Path, default=ROOT/'reports/web-dodge-probe.json')
    args = parser.parse_args()
    game = Game(find_game())
    process = rays = None
    hooked = False
    report = dict(steps=[])
    try:
        pe = PE(game.path.read_bytes())
        if any(game.read(game.base+rva, 16) != pe.bytes(rva, 16) for rva in (UPDATE, START, END)):
            raise RuntimeError('A dodge entry is already patched: start the game afresh')
        bring_to_front(game.pid)
        components = list(registry(game))
        heroes = [r for _, v, r, _ in components if v == HERO_LOCAL]
        if len(heroes) != 1:
            raise RuntimeError('Exactly one local hero is required: load free roam first')
        hero = heroes[0]
        managers = [a for a, v, r, _ in components if v == MANAGER_TABLE and r == hero]
        if len(managers) != 1:
            raise RuntimeError(f"The hero has {len(managers)} dodge managers, not one")
        manager = managers[0]
        report.update(hero=hex(hero), manager=hex(manager))
        process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
        if not process:
            raise c.WinError(c.get_last_error())
        rays, report['ray_hash'] = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                                           ROOT/'reports/ray-modules',
                                           ('SpidyDodgeStart', 'SpidyDodgeStop', 'SpidyDodgeTest', 'SpidyDodgeData'))
        code = call_remote(process, rays['SpidyDodgeStart'])
        report['start'] = code
        if code:
            raise RuntimeError(f'SpidyDodgeStart: {code}')
        hooked = True
        for _ in range(args.times):
            code, raw = call_with_output(game, process, rays['SpidyDodgeTest'], dodge_test(args.kind, hero), 48)
            before, started, after, answer, error = struct.unpack_from('<5I', raw, 24)
            step = dict(code=code, before=before, started=started, after=after, answer=answer, error=error,
                        outside=struct.unpack('<I', game.read(manager+KIND_AT, 4))[0])
            report['steps'].append(step)
            print(json.dumps(step), flush=True)
            time.sleep(.2)
        report['data'] = dodge_snapshot(game, rays['SpidyDodgeData'])
    finally:
        if hooked:
            report['stop'] = call_remote(process, rays['SpidyDodgeStop'])
        if process:
            close(process)
        try:
            pe = PE(game.path.read_bytes())
            report['entry_restored'] = game.read(game.base+UPDATE, 16) == pe.bytes(UPDATE, 16)
        except Exception as error:  # the game may have closed
            report['entry_restored'] = str(error)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=1)+'\n')
    steps = report['steps']
    ok = steps and all(s['code'] == 0 and not s['error'] and s['started'] == args.kind and s['after'] == 0 and
                       s['outside'] == 0 for s in steps) and report.get('entry_restored') is True
    print(json.dumps(dict(ok=bool(ok), frames=(report.get('data') or {}).get('frames'))))
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())

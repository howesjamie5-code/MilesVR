"""The game's Focus for Spidy's blows in the running game, with no enemy about and no headset.

    python tools/probe_focus.py [--blows 6]

It needs a freshly started game in free roam (the ray module starts once per game process) and the game window in
front, which it brings there. It starts the ray module and the web shooter (whose main-thread frame hook gives what
was earned), then asks SpidyFocusTest for the player: first kOtherDamage (6), the kind every blow of Spidy's fell
to until October 22 and which the game's config gives nothing, then --blows times kMeleeDefault (1), then once each
kMeleeAir (2) and kYankBot (15). After each it reads SpidyFocusData: blows given, and the meter (its segments'
fill and their most) - which must not move for kOtherDamage and must rise for the others. A screenshot of the HUD's
Focus bar after the blows. Writes reports/focus-probe.json.
"""
import argparse
import ctypes as c
import json
import pathlib
import struct
import time

from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from inspect_game import PE
from observe_game import call_remote, call_with_payload
from probe_game_grab import HERO_LOCAL, registry, shot
from probe_game_screen import user32
from probe_menu_pad import call_with_output
from run_game_vr import focus_snapshot
from vr_launcher import bring_to_front

# The shooter's main-thread hooks (camera update, muzzle): untouched before the probe, restored after it.
ENTRIES = (0xa24e90, 0x265d730)
KINDS = {6: 'kOtherDamage', 1: 'kMeleeDefault', 2: 'kMeleeAir', 15: 'kYankBot'}


def focus_test(kind, damager):
    """game_focus.cpp's Test: one blow of `kind` earned for `damager`, given at the main thread's next frame."""
    return struct.pack('<4IQ', 0x53464354, 1, 24, kind, damager)


def main():
    try:
        user32.SetProcessDpiAwarenessContext(c.c_void_p(-4))
    except AttributeError:
        user32.SetProcessDPIAware()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--blows', type=int, default=6, help='kMeleeDefault blows to give')
    parser.add_argument('--output', type=pathlib.Path, default=ROOT/'reports/focus-probe.json')
    args = parser.parse_args()
    game = Game(find_game())
    process = rays = None
    rays_active = shooter_active = False
    report = dict(steps=[])
    try:
        pe = PE(game.path.read_bytes())
        if any(game.read(game.base+rva, 16) != pe.bytes(rva, 16) for rva in ENTRIES):
            raise RuntimeError('A hook entry is already patched: start the game afresh')
        bring_to_front(game.pid)
        heroes = [r for a, v, r, _ in registry(game) if v == HERO_LOCAL]
        if len(heroes) != 1:
            raise RuntimeError('Exactly one local hero is required: load free roam first')
        hero = heroes[0]
        process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
        if not process:
            raise c.WinError(c.get_last_error())
        rays, report['ray_hash'] = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                                           ROOT/'reports/ray-modules',
                                           ('SpidyRayStart', 'SpidyRayStop', 'SpidyShooterStart', 'SpidyShooterStop',
                                            'SpidyFocusTest', 'SpidyFocusData'))

        def invoke(address, payload):
            code = call_with_payload(process, address, payload)
            if code:
                raise RuntimeError(f'Native call rejected: {code}')
        invoke(rays['SpidyRayStart'], struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410))
        rays_active = True
        invoke(rays['SpidyShooterStart'], struct.pack('<4IQ', 0x53484f43, 1, 24, game.pid, game.base))
        shooter_active = True

        def blow(kind):
            code, raw = call_with_output(game, process, rays['SpidyFocusTest'], focus_test(kind, hero), 56)
            data = focus_snapshot(game, rays['SpidyFocusData'])
            step = dict(kind=KINDS.get(kind, kind), code=code, data=data)
            report['steps'].append(step)
            print(json.dumps(step), flush=True)
            return step

        # The first finds the meter as it is; the second must leave it there.
        first, second = blow(6), blow(6)
        meleed = [blow(1) for _ in range(args.blows)]
        air = blow(2)
        yank = blow(15)
        time.sleep(.3)
        report['screenshot'] = shot(game, 'focus-after-blows')
        start_now = first['data']['now'] if first['data'] else None
        report['summary'] = dict(
            other_damage_moved=None if start_now is None or not second['data'] else second['data']['now'] != start_now,
            meter_start=start_now,
            after_melee=meleed[-1]['data']['now'] if meleed and meleed[-1]['data'] else None,
            after_air=air['data']['now'] if air['data'] else None,
            after_yank=yank['data']['now'] if yank['data'] else None,
            most=yank['data']['most'] if yank['data'] else None,
            given=yank['data']['given'] if yank['data'] else None)
    finally:
        if shooter_active:
            report['shooter_stop'] = call_remote(process, rays['SpidyShooterStop'])
        if rays_active:
            report['ray_stop'] = call_remote(process, rays['SpidyRayStop'])
        if process:
            close(process)
        try:
            pe = PE(game.path.read_bytes())
            report['entries_restored'] = all(game.read(game.base+rva, 16) == pe.bytes(rva, 16) for rva in ENTRIES)
        except Exception as error:  # the game may have closed
            report['entries_restored'] = str(error)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=1)+'\n')
    print(json.dumps(report.get('summary')))
    summary = report.get('summary') or {}
    rose = summary.get('after_yank') is not None and summary.get('meter_start') is not None and \
        summary['after_yank'] > summary['meter_start']
    return 0 if rose and report.get('entries_restored') is True else 1


if __name__ == '__main__':
    raise SystemExit(main())

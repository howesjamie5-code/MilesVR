"""Does the running game's swing accept walls = 2 ("keep this wall, take no new ones")?

The indoors rule sends it instead of 0. Switching the walls off outright lets go of the wall at once and the
body is held 0.9 m clear of it, so that is a fall straight past it - the player was dropped by it twice in the
headset. A silently rejected setting would leave the whole rule inert, which is the failure this checks for.

    python tools/probe_walls_setting.py

It starts the native swing in the running game and sends each value of `walls`, printing what the module
answers: 0 taken, 2001 rejected. 4 must be rejected: it was the yield that dropped the player at 42 m/s.
"""
import ctypes as c
import json
import struct
import sys

from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from observe_game import call_remote, call_with_payload, modules
from probe_game_grab import HERO_LOCAL, HERO_MOVERS, registry, resolve
from probe_game_swing import snapshot as swing_snapshot
from probe_wall_run import settings

RAY_EXPORTS = ('SpidyRayStart', 'SpidyRayStop', 'SpidySwingStart', 'SpidySwingStop', 'SpidySwingData',
               'SpidySwingSettings')


def main():
    game = Game(find_game())
    components = list(registry(game))
    heroes = [(at, r) for at, v, r, _ in components if v == HERO_LOCAL]
    if len(heroes) != 1:
        raise RuntimeError('Exactly one local hero is required: load the game first')
    record = heroes[0][1]
    managers = [at for at, v, r, _ in components if v == HERO_MOVERS and r == record]
    mover = resolve(game, struct.unpack('<I', game.read(managers[0]+0xdb4, 4))[0])
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    if not process:
        raise c.WinError(c.get_last_error())
    started = False
    rays = None
    try:
        prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_movement_bridge.dll',
                ROOT/'reports/motion-modules', ('SpidyMotionStart', 'SpidyMotionStop', 'SpidyMotionData'))
        motion_module = next(m['base'] for m in modules(game.pid) if m['name'].lower() == 'spidy_movement_bridge.dll')
        rays, _ = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                          ROOT/'reports/ray-modules', RAY_EXPORTS)
        call_remote(process, rays['SpidySwingStop'])
        call_remote(process, rays['SpidyRayStop'])
        code = call_with_payload(process, rays['SpidyRayStart'],
                                 struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410))
        if code:
            raise RuntimeError(f'World rays start: {code}')
        code = call_with_payload(process, rays['SpidySwingStart'],
                                 struct.pack('<4I4QI2fI', 0x53574346, 1, 64, game.pid, game.base, record, mover,
                                             motion_module, 30000, 32., 7.848, 0))
        if code:
            raise RuntimeError(f'Native swing start: {code}')
        started = True
        out = {}
        for walls in (1, 2, 3, 0, 4):
            out[f'walls={walls}'] = call_with_payload(process, rays['SpidySwingSettings'], settings(walls, 7.848))
        sample = swing_snapshot(game, rays['SpidySwingData']) or {}
        print(json.dumps(dict(answers=out, taken=[k for k, v in out.items() if v == 0],
                              rejected=[k for k, v in out.items() if v == 2001],
                              wall_now=sample.get('wall')), indent=1), flush=True)
    finally:
        if started:
            call_remote(process, rays['SpidySwingStop'])
        if rays:
            call_remote(process, rays['SpidyRayStop'])
        close(process)
        game.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())

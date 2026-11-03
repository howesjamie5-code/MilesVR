"""The player's body as Spidy's body rays see it, in the running game with no headset.

    python tools/probe_body_rays.py

It needs a running game in free roam with the game window in front, which it brings there. It loads the ray module,
starts it, and asks SpidyRayBody (src/native_rays.cpp) for the player's actor record: the categories and types of
the collision profile his MoverManager queries with, which the rays standing in for his body (walls, the ground,
the climbing hands, the head) test their hits against (native_rays::meets). Measured October 24 in a research read:
profile 3, categories 0x004, types 0xe7. Also asks for no record (0, 0: every hit, as before) and reports what the
game's own test makes of the filter words Spidy's rays met in the user's burning office: the broken glass (0x10)
passed, the walls and floor (0x43f, 0x63f) met. Writes reports/body-rays-probe.json.
"""
import ctypes as c
import json
import pathlib
import struct

from bridge_game import ROOT, prepare
from capture_game_state import Game, close, find_game, open_process
from observe_game import call_remote
from probe_game_grab import HERO_LOCAL, registry
from probe_game_screen import user32
from probe_menu_pad import call_with_output
from vr_launcher import bring_to_front

OUTPUT = ROOT/'reports/body-rays-probe.json'


def meets(categories, types, word):
    """native_rays::meets, for the report."""
    if not types:
        return True
    mine, theirs = categories & 0x7ff, word & 0x7ff
    if mine and theirs and not mine & theirs:
        return False
    return bool(types & (1 << ((word >> 12) & 0xf)) & 0xff)


def main():
    try:
        user32.SetProcessDpiAwarenessContext(c.c_void_p(-4))
    except AttributeError:
        user32.SetProcessDPIAware()
    game = Game(find_game())
    process = rays = None
    started = False
    report = {}
    try:
        bring_to_front(game.pid)
        heroes = [r for _, v, r, _ in registry(game) if v == HERO_LOCAL]
        if len(heroes) != 1:
            raise RuntimeError('Exactly one local hero is required: load free roam first')
        process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
        if not process:
            raise c.WinError(c.get_last_error())
        rays, report['ray_hash'] = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                                           ROOT/'reports/ray-modules', ('SpidyRayStart', 'SpidyRayStop', 'SpidyRayBody'))
        code = call_with_output(game, process, rays['SpidyRayStart'],
                                struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410), 32)[0]
        report['start'] = code
        if code:
            raise RuntimeError(f'SpidyRayStart: {code}')
        started = True
        for name, record in (('player', heroes[0]), ('nobody', 0)):
            code, raw = call_with_output(game, process, rays['SpidyRayBody'], struct.pack('<Q', record), 8)
            categories, types = struct.unpack('<2I', raw)
            report[name] = dict(record=hex(record), code=code, categories=hex(categories), types=hex(types))
        player = report['player']
        categories, types = int(player['categories'], 16), int(player['types'], 16)
        report['office'] = {hex(w): meets(categories, types, w) for w in (0x10, 0x43f, 0x63f, 0x6bf)}
    finally:
        if started:
            report['stop'] = call_remote(process, rays['SpidyRayStop'])
        if process:
            close(process)
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        OUTPUT.write_text(json.dumps(report, indent=1)+'\n')
    print(json.dumps(report, indent=1))
    office = report.get('office') or {}
    return 0 if office.get('0x10') is False and office.get('0x43f') is True else 1


if __name__ == '__main__':
    raise SystemExit(main())

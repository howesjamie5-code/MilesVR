"""Into the first mission's duct with the swing running as a session runs it: does the swing report the game's
crawl on the duct's floor (game_swing::Surface::floor, 3), and nothing else, on the way in and through it?

Run at the vent on the game's crawl (to_vent.sh, HEIGHT=91).
"""
import json
import struct
import sys
import time

from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from observe_game import call_remote, call_with_payload, modules
from probe_game_grab import HERO_LOCAL, HERO_MOVERS, registry, resolve
from probe_game_swing import snapshot as swing_snapshot
from probe_menu_pad import BUTTONS, XR_DLL
from probe_native_motion import snapshot as motion_snapshot
from probe_wall_crawl import actor, norm, tilt
from probe_wall_run import swing_command
from research_vent_offer import walls_setting
from vr_launcher import bring_to_front

RAY_EXPORTS = ('SpidyRayStart', 'SpidyRayStop', 'SpidySwingStart', 'SpidySwingSubmit', 'SpidySwingStop',
               'SpidySwingData', 'SpidySwingSettings')


def main():
    game = Game(find_game())
    components = list(registry(game))
    record = [r for at, v, r, _ in components if v == HERO_LOCAL][0]
    managers = [at for at, v, r, _ in components if v == HERO_MOVERS and r == record]
    mover = resolve(game, struct.unpack('<I', game.read(managers[0] + 0xdb4, 4))[0])
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    report = dict(samples=[])
    rays = xr = None
    try:
        xr, _ = prepare(game.pid, process, XR_DLL, ROOT / 'reports/stereo-modules', ('SpidyPadSubmit', 'SpidyPadRelease'))
        motion, _ = prepare(game.pid, process, ROOT / 'build/windows-ninja/spidy_movement_bridge.dll',
                            ROOT / 'reports/motion-modules', ('SpidyMotionStart', 'SpidyMotionStop', 'SpidyMotionData'))
        motion_module = next(m['base'] for m in modules(game.pid) if m['name'].lower() == 'spidy_movement_bridge.dll')
        rays, _ = prepare(game.pid, process, ROOT / 'build/windows-ninja/spidy_ray_bridge.dll',
                          ROOT / 'reports/ray-modules', RAY_EXPORTS)
        call_remote(process, rays['SpidySwingStop'])
        call_remote(process, rays['SpidyRayStop'])
        print('rays', call_with_payload(process, rays['SpidyRayStart'],
              struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410)), flush=True)
        print('swing', call_with_payload(process, rays['SpidySwingStart'],
              struct.pack('<4I4QI2fI', 0x53574346, 1, 64, game.pid, game.base, record, mover, motion_module, 30000,
                          32., 7.848, 0)), flush=True)
        call_with_payload(process, rays['SpidySwingSettings'], walls_setting(1, 7.848))
        print('front', bring_to_front(game.pid), flush=True)
        body = actor(game, record)
        ahead = norm(body['forward'])
        serial = [0]
        begun = time.monotonic()
        last = [None]

        def sample(phase, stick=(0., 0.), buttons=0):
            st = struct.pack('<H2B4h', buttons, 0, 0, int(stick[0] * 32767), int(stick[1] * 32767), 0, 0)
            call_with_payload(process, xr['SpidyPadSubmit'], st + struct.pack('<I', 150))
            serial[0] += 1
            here = actor(game, record)
            call_with_payload(process, rays['SpidySwingSubmit'],
                              swing_command(serial[0], here['feet'] if here else body['feet'], ahead))
            s = swing_snapshot(game, rays['SpidySwingData']) or {}
            m = motion_snapshot(game, motion['SpidyMotionData']) or {}
            e = dict(t=round(time.monotonic() - begun, 2), phase=phase, wall=s.get('wall'), owned=s.get('owned'),
                     normal=[round(v, 2) for v in (s.get('wall_normal') or (0, 0, 0))],
                     mover=hex(m.get('mover_flags') or 0), grounded=m.get('grounded'),
                     feet=[round(v, 2) for v in here['feet']] if here else None,
                     tilt=round(tilt(here['up']), 1) if here else None)
            report['samples'].append(e)
            key = (e['phase'], e['wall'], e['owned'], e['mover'], e['grounded'])
            if key != last[0]:
                last[0] = key
                print(json.dumps(e), flush=True)
            time.sleep(.03)

        def run(phase, seconds, stick=(0., 0.), buttons=0):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                sample(phase, stick, buttons)

        run('at the vent', 1.0)
        run('Y', .7, buttons=BUTTONS['y'])
        run('going in', 5.0)
        run('crawling the duct', 3.0, stick=(0., .999))
        run('stopped in it', 2.0)
        run('back', 2.0, stick=(0., -.999))
        run('stopped again', 1.5)
        walls = {}
        for e in report['samples']:
            walls.setdefault(e['phase'], set()).add(e['wall'])
        print('walls seen per phase:', {k: sorted(v) for k, v in walls.items()}, flush=True)
    finally:
        try:
            if xr:
                call_remote(process, xr['SpidyPadRelease'])
            if rays:
                call_remote(process, rays['SpidySwingStop'])
                call_remote(process, rays['SpidyRayStop'])
        except Exception as ex:
            print('cleanup', ex)
        with open(ROOT / 'reports/research-duct-crawl.json', 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
        close(process)
        game.close()


if __name__ == '__main__':
    sys.exit(main())

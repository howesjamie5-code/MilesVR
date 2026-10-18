"""Does letting the swing's wall go hand the player to the game, or drop him?

The one thing the interact's hand-over rests on (game_pad::WallHandover). The game withdraws its own
interact 63 ms after the player leaves the surface, and the swing's wall running holds him off it, so the
interact press lets the walls go for a moment and expects the game's own crawl to take him where he stands.
If instead he falls, the press would cost a player his footing - so this measures it.

Stand the player at a wall first (walk him into it), then:

    python tools/probe_wall_handover.py

It finds the nearest wall with world rays, walks him at it to mount the swing's own wall, then sends Settings
with walls off (Swing::allowWalls(false), which leaves the wall it holds), watches for a second who has him, and
puts the walls back.

With --drive it keeps pushing him INTO the wall for the first moments of the hand-over, which is the half the
October 13 and 14 builds never did: the swing holds the body 0.9 m clear of the wall, so letting go without a
push is a fall straight past it, measured twice in the headset. The game's own crawl does take a player who
flies into a wall with the swing's walls off (that is how probe_wall_crawl gets him there), so the question this
answers is whether a driven release lands him on it.
"""
import argparse
import ctypes as c
import json
import math
import struct
import sys
import time

from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from observe_game import call_remote, call_with_payload, modules
from probe_game_grab import HERO_LOCAL, HERO_MOVERS, registry, resolve
from probe_game_swing import snapshot as swing_snapshot
from probe_menu_pad import XR_DLL
from probe_native_rays import snapshot as ray_snapshot, command as ray_command
from probe_wall_crawl import actor, add, dot, length, norm, scale, sub, tilt
from probe_wall_run import settings, swing_command

RAY_EXPORTS = ('SpidyRayStart', 'SpidyRayStop', 'SpidyRaySubmit', 'SpidyRayData', 'SpidySwingStart',
               'SpidySwingSubmit', 'SpidySwingStop', 'SpidySwingData', 'SpidySwingSettings')
SURFACES = {0: 'nothing', 1: "the swing's own wall", 2: "the game's wall crawl"}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--mount-seconds', type=float, default=16, help='longest walk at the wall to mount it')
    p.add_argument('--handover-seconds', type=float, default=1.5, help='how long the walls stay off')
    p.add_argument('--drive', action='store_true',
                   help="push him into the wall as the walls go off, so the game's crawl can catch him")
    p.add_argument('--drive-seconds', type=float, default=.5, help='how long that push lasts')
    p.add_argument('--gravity', type=float, default=7.848)
    p.add_argument('--output', default=str(ROOT/'reports/wall-handover.json'))
    a = p.parse_args()

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
    report = dict(phases=[], samples=[])
    rays = None
    started_rays = started_swing = False
    try:
        xr, _ = prepare(game.pid, process, XR_DLL, ROOT/'reports/stereo-modules',
                        ('SpidyPadSubmit', 'SpidyPadRelease'))
        prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_movement_bridge.dll',
                ROOT/'reports/motion-modules', ('SpidyMotionStart', 'SpidyMotionStop', 'SpidyMotionData'))
        motion_module = next(m['base'] for m in modules(game.pid) if m['name'].lower() == 'spidy_movement_bridge.dll')
        rays, _ = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                          ROOT/'reports/ray-modules', RAY_EXPORTS)

        def invoke(address, payload, what, already=()):
            code = call_with_payload(process, address, payload)
            # 1000: the bridge is up already, from a probe before this one in the same game process.
            if code and code not in already:
                raise RuntimeError(f'{what}: {code}')
            return code

        invoke(rays['SpidyRayStart'], struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410),
               'World rays start', already=(1000,))
        started_rays = True
        invoke(rays['SpidySwingStart'],
               struct.pack('<4I4QI2fI', 0x53574346, 1, 64, game.pid, game.base, record, mover, motion_module,
                           30000, 32., a.gravity, 0), 'Native swing start', already=(1000, 2002))
        started_swing = True
        invoke(rays['SpidySwingSettings'], settings(True, a.gravity), 'Walls on')

        body = actor(game, record)
        if not body:
            raise RuntimeError("The player's actor could not be read")

        def cast(batch, serial):
            call_with_payload(process, rays['SpidyRaySubmit'], ray_command(serial, batch))
            for _ in range(60):
                shot = ray_snapshot(game, rays['SpidyRayData'])
                if shot and shot.get('serial') == serial and shot.get('count'):
                    return shot['hits']
                time.sleep(.016)
            return []

        # The nearest wall around him, level with his chest: a face steep enough to stand on, far enough to
        # walk at. Whichever way he happens to be facing is not it - the October 14 run walked him into the
        # open and never mounted anything ("mounted": null).
        eye = add(body['feet'], (0., 1.2, 0.))
        found = []
        for sweep in range(3):
            batch = []
            for i in range(8):
                heading = (sweep*8+i)*(6.2831853/24)
                batch.append((eye, (math.cos(heading), 0., math.sin(heading)), 60., i))
            for (_, direction, distance, _), hit in zip(batch, cast(batch, 100+sweep)):
                reach = hit['fraction']*distance
                if hit['count'] and abs(hit['normal'][1]) < .35 and dot(hit['normal'], direction) < -.3 and \
                        2 <= reach <= 55:
                    found.append(dict(distance=round(reach, 2), normal=hit['normal'], direction=direction))
        if not found:
            raise RuntimeError('No wall within 2-55 m of the player: stand him beside a building')
        wall = min(found, key=lambda w: w['distance'])
        into = norm(wall['direction'])
        normal = norm(wall['normal'])
        report['wall'] = wall
        report['start'] = dict(feet=[round(v, 2) for v in body['feet']], walking=[round(v, 3) for v in into])
        print(f"wall {wall['distance']} m away, normal {[round(v, 2) for v in normal]}", flush=True)
        serial = 0
        begun = time.monotonic()

        def step(move=(0., 0., 0.), phase=''):
            nonlocal serial
            serial += 1
            here = actor(game, record)
            call_with_payload(process, rays['SpidySwingSubmit'],
                              swing_command(serial, here['feet'] if here else body['feet'], into, move=move))
            s = swing_snapshot(game, rays['SpidySwingData']) or {}
            entry = dict(seconds=round(time.monotonic()-begun, 3), phase=phase, wall=s.get('wall'),
                         owned=s.get('owned'), grounded=s.get('grounded'),
                         collision=s.get('collision_flags'), contact=s.get('native_contact'),
                         wall_normal=[round(v, 2) for v in (s.get('wall_normal') or (0, 0, 0))],
                         wall_distance=round(s.get('wall_distance') or 0, 2),
                         takeoff=s.get('takeoff'), mount=s.get('mount'),
                         moved=[round(v, 2) for v in move])
            if here:
                entry.update(y=round(here['feet'][1], 3), tilt=tilt(here['up']))
            report['samples'].append(entry)
            return entry

        def controller(buttons=0, milliseconds=60):
            """The virtual Xbox controller for a moment, as the VR worker holds it."""
            call_with_payload(process, xr['SpidyPadSubmit'],
                              struct.pack('<H2B4h', buttons, 0, 0, 0, 0, 0, 0)+struct.pack('<I', milliseconds))

        # Onto the swing's own wall. Walking at one needs the PAD's stick to carry him there, in the game
        # camera's frame, which takes a calibration of its own (probe_wall_mount does it); the swing's flight
        # owns him in the air, where its own stick steers, so the shortest way from a ledge is to jump and fly
        # into the wall - the swing takes the walls a player comes into. A on the pad for the jump.
        mounted = None
        controller(0x1000, 150)
        time.sleep(.2)
        until = time.monotonic()+a.mount_seconds
        while time.monotonic() < until:
            entry = step(into, 'mounting')
            if entry.get('takeoff'):
                controller(0x1000)
            if entry.get('wall') == 1:
                mounted = entry
                break
            # Grounded again without a wall: he landed back on the ledge. Jump once more.
            if entry.get('grounded') and time.monotonic()-begun > 1.5:
                controller(0x1000, 150)
                time.sleep(.25)
            time.sleep(.011)
        report['mounted'] = mounted
        if not mounted:
            print(json.dumps(dict(mounted=False, reason="the swing never took a wall of its own",
                                  last=report['samples'][-1] if report['samples'] else None), indent=1), flush=True)
            return 1
        # Standing still on it, as a player does at a vent.
        for _ in range(45):
            step(phase='standing')
            time.sleep(.011)
        before = report['samples'][-1]

        # The wall he is actually on, as the swing has it: its own normal beats the ray's, which was cast
        # from the ground before he climbed.
        held = report['samples'][-1]
        if length(held['wall_normal']) > .5:
            normal = norm(held['wall_normal'])
        report['held_normal'] = [round(v, 3) for v in normal]

        # The hand-over: the walls go off, and the question is who has him now. Driven, he is pushed into the
        # wall meanwhile - the swing holds him 0.9 m clear of it, so a release without a push falls past it.
        invoke(rays['SpidySwingSettings'], settings(False, a.gravity), 'Walls off')
        begun_handover = time.monotonic()
        until = begun_handover+a.handover_seconds
        while time.monotonic() < until:
            pushing = a.drive and time.monotonic()-begun_handover < a.drive_seconds
            step(scale(normal, -1.) if pushing else (0., 0., 0.), 'driven in' if pushing else 'handed back')
            time.sleep(.011)
        after = report['samples'][-1]

        invoke(rays['SpidySwingSettings'], settings(True, a.gravity), 'Walls on')
        until = time.monotonic()+1.
        while time.monotonic() < until:
            step(phase='walls back')
            time.sleep(.011)
        back = report['samples'][-1]

        handed = [s for s in report['samples'] if s['phase'] in ('handed back', 'driven in')]
        took = [s for s in handed if s.get('wall') == 2]
        supported = [s for s in handed if s.get('contact') == 0]
        print(json.dumps(dict(
            driven=bool(a.drive),
            mounted_on=SURFACES.get(mounted.get('wall')),
            before=dict(y=before.get('y'), tilt=before.get('tilt'), surface=SURFACES.get(before.get('wall'))),
            after=dict(y=after.get('y'), tilt=after.get('tilt'), surface=SURFACES.get(after.get('wall'))),
            back=dict(y=back.get('y'), surface=SURFACES.get(back.get('wall'))),
            dropped_metres=round((before.get('y') or 0)-(after.get('y') or 0), 3),
            the_game_took_him=bool(took),
            frames_the_game_held_him=f"{len(took)} of {len(handed)}",
            frames_the_game_called_him_supported=f"{len(supported)} of {len(handed)}",
            took_after_seconds=round(took[0]['seconds']-begun_handover+begun, 3) if took else None),
            indent=1), flush=True)
    finally:
        if started_swing:
            call_remote(process, rays['SpidySwingStop'])
        if started_rays:
            call_remote(process, rays['SpidyRayStop'])
        with open(a.output, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
        close(process)
        game.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())

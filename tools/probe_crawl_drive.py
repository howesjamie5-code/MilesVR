"""Can Spidy drive the player while the GAME's own wall crawl holds him?

The one thing the whole wall design now rests on. The game's crawl is the only hold the game itself calls
standing on a wall, so it is the only one whose own interacts, prompts, doors and vents work there; Spidy's own
wall runs the hero in the game's AIR state, where the game withdraws its interact 63 ms after he leaves a
surface. So the crawl should hold every wall and Spidy should supply the view (it does now), the body (it does
now) and the movement - which is this.

    python tools/probe_crawl_drive.py

It finds the nearest wall, jumps the player at it until the game's crawl has him (its actor turned onto the
surface), then leases the mover a velocity along that wall (native_movement, as the swing does in flight) and
reads what happens:

  moved      the body went the way it was told, at the speed it was told
  held       the actor stayed turned onto the wall, and the game kept calling him supported

Both true: wall running can keep the game's crawl and simply drive it, and every interaction on a wall works
for free. Either false: the crawl fights a lease, and wall running cannot be built on it.

It needs a running game with a loaded player beside a building. It writes reports/crawl-drive.json.
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
from probe_native_motion import snapshot as motion_snapshot
from probe_native_rays import snapshot as ray_snapshot, command as ray_command
from probe_wall_crawl import actor, add, dot, length, norm, scale, sub, tilt
from probe_menu_pad import XR_DLL

RAY_EXPORTS = ('SpidyRayStart', 'SpidyRayStop', 'SpidyRaySubmit', 'SpidyRayData')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--crawl-seconds', type=float, default=14, help='longest wait for the game to take him')
    p.add_argument('--drive-seconds', type=float, default=1.2, help='how long the lease drives him')
    p.add_argument('--speed', type=float, default=4., help='metres a second along the wall')
    p.add_argument('--direction', choices=('up', 'away'), default='up',
                   help="up the wall, or away from it and up: what a web's takeoff from the crawl needs")
    p.add_argument('--output', default=str(ROOT/'reports/crawl-drive.json'))
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
    report = dict(samples=[], events=[])
    rays = motion = None
    started_rays = started_motion = False
    try:
        xr, _ = prepare(game.pid, process, XR_DLL, ROOT/'reports/stereo-modules',
                        ('SpidyPadSubmit', 'SpidyPadRelease'))
        motion, _ = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_movement_bridge.dll',
                            ROOT/'reports/motion-modules',
                            ('SpidyMotionStart', 'SpidyMotionSubmit', 'SpidyMotionStop', 'SpidyMotionData'))
        rays, _ = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                          ROOT/'reports/ray-modules', RAY_EXPORTS)

        def invoke(address, payload, what, already=()):
            code = call_with_payload(process, address, payload)
            if code and code not in already:
                raise RuntimeError(f'{what}: {code}')
            return code

        # A probe before this one in the same game process leaves the bridges up with a lease that has since
        # run out, so a start that answers 1000 ("already") leaves them dead. Stop them first.
        call_remote(process, rays['SpidyRayStop'])
        call_remote(process, motion['SpidyMotionStop'])
        invoke(rays['SpidyRayStart'], struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410),
               'World rays start')
        started_rays = True
        invoke(motion['SpidyMotionStart'],
               struct.pack('<4I3QIf2I', 0x534d5643, 2, 56, game.pid, game.base, record, mover, 30000, 2.0, 0, 0),
               'Native motion start')
        started_motion = True

        def controller(buttons=0, milliseconds=120):
            call_with_payload(process, xr['SpidyPadSubmit'],
                              struct.pack('<H2B4h', buttons, 0, 0, 0, 0, 0, 0)+struct.pack('<I', milliseconds))

        serial = [0]

        def cast(batch):
            serial[0] += 1
            call_with_payload(process, rays['SpidyRaySubmit'], ray_command(serial[0], batch))
            for _ in range(60):
                shot = ray_snapshot(game, rays['SpidyRayData'])
                if shot and shot.get('serial') == serial[0] and shot.get('count'):
                    return shot['hits']
                time.sleep(.016)
            return []

        begun = time.monotonic()

        def sample(phase, asked=(0., 0., 0.)):
            body = actor(game, record)
            m = motion_snapshot(game, motion['SpidyMotionData']) or {}
            entry = dict(seconds=round(time.monotonic()-begun, 3), phase=phase,
                         tilt=round(tilt(body['up']), 1) if body else None,
                         feet=[round(v, 2) for v in body['feet']] if body else None,
                         up=[round(v, 2) for v in body['up']] if body else None,
                         velocity=[round(v, 2) for v in m.get('velocity', (0, 0, 0))],
                         requested=[round(v, 2) for v in m.get('requested', (0, 0, 0))],
                         asked=[round(v, 2) for v in asked],
                         steps=m.get('steps'), controlled=m.get('controlled'), status=m.get('status'),
                         grounded=m.get('grounded'), contact=m.get('contact'),
                         collision=m.get('collision_flags'))
            report['samples'].append(entry)
            return entry

        def mark(name):
            report['events'].append(dict(name=name, seconds=round(time.monotonic()-begun, 3)))
            print(f"{report['events'][-1]['seconds']:6.2f} s  {name}", flush=True)

        body = actor(game, record)
        if not body:
            raise RuntimeError("The player's actor could not be read")
        already = tilt(body['up']) > 45
        eye = add(body['feet'], (0., 1.2, 0.)) if not already else add(body['feet'], scale(norm(body['up']), 1.2))
        found = []
        for sweep in range(3):
            batch = [(eye, (math.cos((sweep*8+i)*(6.2831853/24)), 0., math.sin((sweep*8+i)*(6.2831853/24))),
                      60., i) for i in range(8)]
            for (_, direction, distance, _), hit in zip(batch, cast(batch)):
                reach = hit['fraction']*distance
                if hit['count'] and abs(hit['normal'][1]) < .35 and dot(hit['normal'], direction) < -.3 and \
                        1.5 <= reach <= 55:
                    found.append(dict(distance=round(reach, 2), normal=hit['normal'], direction=direction))
        if not found and already:
            found.append(dict(distance=0., normal=norm(body['up']), direction=scale(norm(body['up']), -1.)))
        if not found:
            raise RuntimeError('No wall within 1.5-55 m: stand the player beside a building')
        wall = min(found, key=lambda w: w['distance'])
        report['wall'] = wall
        normal = norm(wall['normal'])
        mark(f"wall {wall['distance']} m away, normal {[round(v, 2) for v in normal]}")

        # Onto the game's own crawl: jump at it until its actor turns onto the surface. The game takes a
        # player who flies into a wall, and a player who walks into one.
        crawling = None
        last_jump = 0.
        until = time.monotonic()+(0 if already else a.crawl_seconds)
        if already:
            crawling = sample('already on the crawl')
        while time.monotonic() < until:
            entry = sample('reaching the wall')
            if (entry.get('tilt') or 0) > 30:
                crawling = entry
                break
            if time.monotonic()-last_jump > 1.1:
                controller(0x1000, 150)
                last_jump = time.monotonic()
            time.sleep(.02)
        report['crawling'] = crawling
        if not crawling:
            print(json.dumps(dict(crawling=False, reason="the game's crawl never took him",
                                  last=report['samples'][-1]), indent=1), flush=True)
            return 1
        mark(f"the game's crawl has him: actor turned {crawling['tilt']} degrees")
        for _ in range(30):
            sample('crawling')
            time.sleep(.012)
        before = report['samples'][-1]

        # Up the wall, in its own plane: the direction a player on a wall wants most. Or away from it and up:
        # a web's takeoff from the crawl, which the game's own jump does not do (October 14: A with the stick
        # neutral left him where he was, twice).
        body = actor(game, record)
        n = norm(body['up']) if body else normal
        up_the_wall = sub((0., 1., 0.), scale(n, dot((0., 1., 0.), n)))
        if length(up_the_wall) < .3:
            up_the_wall = sub(scale(normal, -1.), scale(n, dot(scale(normal, -1.), n)))
        if a.direction == 'away':
            asked = scale(norm(add(n, (0., .6, 0.))), a.speed)
        else:
            asked = scale(norm(up_the_wall), a.speed)
        report['asked'] = [round(v, 3) for v in asked]
        mark(f"driving {[round(v, 2) for v in asked]} along the wall")
        until = time.monotonic()+a.drive_seconds
        while time.monotonic() < until:
            call_with_payload(process, motion['SpidyMotionSubmit'],
                              struct.pack('<4IQ2I3fI', 0x534d564d, 1, 48, 1, 1, 160, 0, *asked, 0))
            sample('driven', asked)
            time.sleep(.012)
        after = report['samples'][-1]
        for _ in range(40):
            sample('let go')
            time.sleep(.012)
        rested = report['samples'][-1]

        driven = [s for s in report['samples'] if s['phase'] == 'driven']
        settled = driven[2:] if len(driven) > 3 else driven
        errors = [math.dist(s['velocity'], asked) for s in settled if s['velocity']]
        held = [s for s in settled if (s.get('tilt') or 0) > 30]
        supported = [s for s in settled if s.get('contact') == 0]
        travelled = round(length(sub(after['feet'], before['feet'])), 2) if after['feet'] and before['feet'] else None
        verdict = dict(
            crawl_tilt_before=before.get('tilt'), crawl_tilt_after=after.get('tilt'),
            asked=[round(v, 2) for v in asked],
            mean_velocity_error=round(sum(errors)/len(errors), 3) if errors else None,
            worst_velocity_error=round(max(errors), 3) if errors else None,
            travelled_m=travelled,
            frames_still_on_the_crawl=f"{len(held)} of {len(settled)}",
            frames_the_game_called_him_supported=f"{len(supported)} of {len(settled)}",
            tilt_after_letting_go=rested.get('tilt'),
            moved=bool(errors) and max(errors) < 1.5,
            stayed=bool(settled) and len(held) == len(settled))
        report['verdict'] = verdict
        print(json.dumps(verdict, indent=1), flush=True)
    finally:
        if started_motion:
            call_remote(process, motion['SpidyMotionStop'])
        if started_rays:
            call_remote(process, rays['SpidyRayStop'])
        with open(a.output, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
        close(process)
        game.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())

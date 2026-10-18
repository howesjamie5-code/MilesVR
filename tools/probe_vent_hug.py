"""At the vent, on Spidy's own wall: does holding the hero AGAINST the wall let the game register him?

The player, October 14: "its not even prompting me to press anything in walking mode which means that its not
registering me". On Spidy's wall the game has the hero in its AIR state, and the swing holds the body 0.9 m clear
of the wall - on purpose, because when the game's own collision met the wall "the game held such a body still for
a second and then stuck it to the wall in its crawl" (October 9 headset reports; swing.hpp). At a vent that crawl
is exactly what is wanted: it is the one hold the game calls standing, and the prompt shows only there
(reports/vent-wall.json, October 13: POIInteractAvailable(h, 0, '[BTN_Y] Enter') while the game's crawl held him).

Every earlier hand-over let go of the hero first and dropped him. This one never lets go until the game has him:

  mount     Spidy's swing takes the wall at the vent (the game's jump while the swing asks for it)
  park      the swing's stick brings him back to the vent's height; the prompt should be hidden here
  hug       the swing holds him against the wall (Settings walls = 3): his collision meets it
  hand      only once the game's crawl has turned him onto the wall, the swing lets go (walls = 0),
            then takes no new walls (walls = 2) so it cannot steal him back
  press     the game's Y

Run it with the game standing at the vent on its own crawl (probe_vent.py walks there with profile 3):

    python tools/probe_vent_hug.py

It prints, per phase, who holds him, his height (a fall shows at once), the actor's tilt, and the prompt
timeline with the phases marked in it. It writes reports/vent-hug.json.
"""
import argparse
import ctypes as c
import json
import struct
import sys
import time

from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from observe_game import call_remote, call_with_payload, modules
from probe_game_grab import HERO_LOCAL, HERO_MOVERS, registry, resolve
from probe_game_swing import snapshot as swing_snapshot
from probe_menu_pad import BUTTONS, XR_DLL, pad
from probe_native_motion import snapshot as motion_snapshot
from probe_wall_crawl import actor, dot, norm, tilt
from probe_wall_run import swing_command
import probe_prompt_watch as watch

XR_EXPORTS = ('SpidyPadSubmit', 'SpidyPadSample', 'SpidyPadRelease', 'SpidyMenuStart', 'SpidyPromptsWatch',
              'SpidyPromptsWatched', 'SpidyPromptsNote')
RAY_EXPORTS = ('SpidyRayStart', 'SpidyRayStop', 'SpidyRaySubmit', 'SpidyRayData', 'SpidySwingStart',
               'SpidySwingSubmit', 'SpidySwingStop', 'SpidySwingData', 'SpidySwingSettings')
SURFACES = {0: 'nothing', 1: "Spidy's wall", 2: "the game's crawl"}


def walls_setting(walls, gravity):
    """game_swing::Settings v4 with `walls` 0 off, 1 on, 2 keep and take no new, 3 on and held against it."""
    return struct.pack('<4IfIfI', 0x53575354, 4, 32, 0, 32., 1, gravity, int(walls))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--gravity', type=float, default=7.848)
    p.add_argument('--hug-seconds', type=float, default=5, help='longest hold against the wall')
    p.add_argument('--release-stick', choices=('none', 'forward', 'back', 'left', 'right'), default='forward',
                   help="the game's stick while the swing lets go against the wall")
    p.add_argument('--skip-stick-ways', action='store_true', help='hold still only, no stick search while held')
    p.add_argument('--press-stick', choices=('none', 'forward', 'back', 'left', 'right'), default='none',
                   help="the game's own stick while pressed into the wall")
    p.add_argument('--press', action='store_true',
                   help="press into the wall at speed (walls = 4) instead of holding against it: the game's grasp")

    p.add_argument('--output', default=str(ROOT/'reports/vent-hug.json'))
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
    report = dict(samples=[], phases={})
    swing_started = rays_started = False
    xr = rays = None
    try:
        xr, _ = prepare(game.pid, process, XR_DLL, ROOT/'reports/stereo-modules', XR_EXPORTS)
        motion, _ = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_movement_bridge.dll',
                            ROOT/'reports/motion-modules', ('SpidyMotionStart', 'SpidyMotionStop', 'SpidyMotionData'))
        motion_module = next(m['base'] for m in modules(game.pid) if m['name'].lower() == 'spidy_movement_bridge.dll')
        rays, _ = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                          ROOT/'reports/ray-modules', RAY_EXPORTS)

        def invoke(address, payload, what, already=()):
            code = call_with_payload(process, address, payload)
            if code and code not in already:
                raise RuntimeError(f'{what}: {code}')
            return code

        from probe_menu import settings_payload, FLAGS
        call_with_payload(process, xr['SpidyMenuStart'],
                          settings_payload(FLAGS['web_grab'] | FLAGS['punch'] | FLAGS['body'], 30, 100, 1, 32.))
        call_with_payload(process, xr['SpidyPromptsWatch'], struct.pack('<I', 1))
        before = watch.watched(game, process, xr)
        report['offered_at_start'] = dict(demand=before.get('demand'), last=before.get('last_demand'))

        call_remote(process, rays['SpidySwingStop'])
        call_remote(process, rays['SpidyRayStop'])
        invoke(rays['SpidyRayStart'], struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410),
               'World rays start')
        rays_started = True
        invoke(rays['SpidySwingStart'],
               struct.pack('<4I4QI2fI', 0x53574346, 1, 64, game.pid, game.base, record, mover, motion_module,
                           30000, 32., a.gravity, 0), 'Native swing start')
        swing_started = True

        def stick(sx, sy, milliseconds=200):
            """The game's own left stick for a moment, without waiting: the swing's commands must keep coming."""
            state = struct.pack('<H2B4h', 0, 0, 0, int(sx*32767), int(sy*32767), 0, 0)
            call_with_payload(process, xr['SpidyPadSubmit'], state+struct.pack('<I', milliseconds))

        def walls(value):
            invoke(rays['SpidySwingSettings'], walls_setting(value, a.gravity), f'walls = {value}')

        def note(text):
            call_with_payload(process, xr['SpidyPromptsNote'], text.encode()[:95].ljust(96, b'\0'))

        body = actor(game, record)
        if not body:
            raise RuntimeError("The player's actor could not be read")
        vent_height = body['feet'][1]
        at_the_wall = norm(body['forward'])
        report['start'] = dict(feet=[round(v, 3) for v in body['feet']], up=[round(v, 3) for v in body['up']],
                               tilt=round(tilt(body['up']), 1))
        print(f"start: feet {report['start']['feet']} tilt {report['start']['tilt']}", flush=True)
        walls(1)

        serial = [0]
        begun = time.monotonic()

        def step(move, phase):
            serial[0] += 1
            here = actor(game, record)
            origin = here['feet'] if here else body['feet']
            call_with_payload(process, rays['SpidySwingSubmit'], swing_command(serial[0], origin, at_the_wall,
                                                                               move=move))
            s = swing_snapshot(game, rays['SpidySwingData']) or {}
            entry = dict(seconds=round(time.monotonic()-begun, 3), phase=phase, wall=s.get('wall'),
                         owned=s.get('owned'), grounded=s.get('grounded'), contact=s.get('native_contact'),
                         mount=s.get('mount'), takeoff=s.get('takeoff'),
                         distance=round(s.get('wall_distance') or 0, 2),
                         normal=[round(v, 2) for v in (s.get('wall_normal') or (0, 0, 0))])
            if here:
                entry.update(y=round(here['feet'][1], 2), tilt=round(tilt(here['up']), 1),
                             up=[round(v, 2) for v in here['up']])
            # The game's own airborne state, as the movement module last saw it: what it will fall at the
            # first step it makes itself.
            m = motion_snapshot(game, motion['SpidyMotionData']) or {}
            entry.update(mover_flags=hex(m.get('mover_flags') or 0), motion_collision=hex(m.get('collision_flags') or 0),
                         air_vertical=round(m.get('air_vertical') or 0, 2),
                         air_gravity=round(m.get('air_gravity') or 0, 2),
                         air_velocity=[round(v, 2) for v in (m.get('air_velocity') or (0, 0, 0))],
                         air_state=hex(m.get('air_state') or 0), air_events=m.get('air_events'),
                         air_overrides=m.get('air_overrides'), steps=m.get('steps'),
                         controlled=m.get('controlled'), velocity=[round(v, 2) for v in (m.get('velocity') or (0, 0, 0))])
            report['samples'].append(entry)
            return entry

        def phase(name):
            report['phases'][name] = round(time.monotonic()-begun, 3)
            note(f'>> {name}')
            print(f"{report['phases'][name]:6.2f} s  {name}", flush=True)

        # 1. Onto Spidy's own wall: walking into it, and the game's jump while the swing asks for it.
        phase('mount: onto spidy wall')
        mounted = None
        until = time.monotonic()+8
        leapt = 0.
        while time.monotonic() < until:
            entry = step((0., 0., 0.), 'mount')
            if entry.get('mount') or entry.get('takeoff'):
                pad(process, xr, BUTTONS['a'], hold_ms=150)
            if entry.get('wall') == 1 and entry.get('owned'):
                mounted = entry
                break
            # Walked onto, a wall is the game's now (mountFromGround off), and the swing takes nothing from
            # its crawl. Onto the swing's own wall the way a player gets there: off the crawl with the game's
            # RT + A, the only jump that leaves it, and the swing's wall takes him in flight.
            if entry.get('wall') == 2 and time.monotonic()-leapt > 2.:
                state = struct.pack('<H2B4h', BUTTONS['a'], 0, 255, 0, 0, 0, 0)
                call_with_payload(process, xr['SpidyPadSubmit'], state+struct.pack('<I', 150))
                leapt = time.monotonic()
            time.sleep(.011)
        report['mounted'] = mounted
        if not mounted:
            print(json.dumps(dict(mounted=False, last=report['samples'][-1]), indent=1), flush=True)
            return 1

        # 2. Still, on that wall, where the mount left him. No steering up: the vent's own opening is above,
        # and walking up into it took the swing's wall away and its flight dropped him 43 m (October 14, a run
        # of this probe - and very likely the player's own "wall walked to the vent and fell").
        phase('park: still at the vent on spidy wall')
        for _ in range(180):
            step((0., 0., 0.), 'on spidy wall')
            time.sleep(.011)
        report['before_hug'] = report['samples'][-1]
        if report['before_hug'].get('wall') != 1 or (report['before_hug'].get('y') or 0) < vent_height-2.5:
            print(json.dumps(dict(invalid='he left the wall before the test',
                                  before_hug=report['before_hug']), indent=1), flush=True)
            return 2

        # 3. Held against the wall, never let go of. Contact alone did not do it (October 14: drawn in to 0.59 m,
        # the game's collision, and its crawl never came), so the game's own stick goes with it: walking into a
        # wall is what starts its crawl from the ground, and on the swing's wall that stick is otherwise zero.
        # Each way in turn, because which way is "into the wall" depends on where the game's camera points.
        phase('hug: held against the wall')
        walls(4 if a.press else 3)
        crawled = None
        ways = (('still', (0, 0)),) if a.skip_stick_ways else (
            ('still', (0, 0)), ('stick forward', (0, 1)), ('stick back', (0, -1)), ('stick left', (-1, 0)),
            ('stick right', (1, 0)))
        report['hug_ways'] = {}
        for name, (sx, sy) in ways:
            note(f'>> hug, {name}')
            began = time.monotonic()
            until = began+(a.hug_seconds if name == 'still' else 1.6)
            press_way = {'none': (0, 0), 'forward': (0, 1), 'back': (0, -1), 'left': (-1, 0),
                         'right': (1, 0)}[a.press_stick]
            while time.monotonic() < until:
                if sx or sy:
                    stick(sx, sy)
                elif a.press and press_way != (0, 0):
                    stick(*press_way)
                entry = step((0., 0., 0.), f'hug, {name}')
                if (entry.get('tilt') or 0) > 45:
                    crawled = dict(entry, way=name, after=round(time.monotonic()-began, 2))
                    break
                time.sleep(.011)
            report['hug_ways'][name] = dict(distance=report['samples'][-1].get('distance'),
                                            contact=report['samples'][-1].get('contact'),
                                            tilt=report['samples'][-1].get('tilt'),
                                            y=report['samples'][-1].get('y'))
            print(f"   {name}: {report['hug_ways'][name]}", flush=True)
            if crawled:
                break
        report['crawled'] = crawled

        if crawled:
            # 4. The game's crawl has him: the swing lets go, then cannot steal him back.
            phase("hand: the game's crawl has him, the swing lets go")
            walls(0)
            for _ in range(30):
                step((0., 0., 0.), 'released')
                time.sleep(.011)
            walls(2)
            for _ in range(150):
                step((0., 0., 0.), "on the game's crawl")
                time.sleep(.011)
        else:
            # Contact while the swing drives the body does nothing: the game's own wall logic runs only on the
            # steps the game makes itself. So the swing lets go HERE, against the wall, the game's stick pushed
            # one way - the plain game sticks a falling Spider-Man to a wall he touches with the stick at it.
            stick_way = {'none': (0, 0), 'forward': (0, 1), 'back': (0, -1), 'left': (-1, 0),
                         'right': (1, 0)}[a.release_stick]
            phase(f'release against the wall, stick {a.release_stick}')
            walls(0)
            for _ in range(140):
                if stick_way != (0, 0):
                    stick(*stick_way)
                entry = step((0., 0., 0.), 'released')
                if (entry.get('tilt') or 0) > 45 and not crawled:
                    crawled = dict(entry, way=f'release, stick {a.release_stick}',
                                   after=round(entry['seconds']-report['phases'][
                                       f'release against the wall, stick {a.release_stick}'], 2))
                time.sleep(.011)
            if crawled:
                phase("the game's crawl has him: the swing takes no new walls")
                walls(2)
                for _ in range(150):
                    step((0., 0., 0.), "on the game's crawl")
                    time.sleep(.011)
            else:
                phase('not taken: walls back on')
                walls(1)
                for _ in range(60):
                    step((0., 0., 0.), 'unhugged')
                    time.sleep(.011)
        report['after'] = report['samples'][-1]
        offered = watch.watched(game, process, xr)
        report['offered_before_press'] = dict(demand=offered.get('demand'), last=offered.get('last_demand'))

        # 5. The game's Y.
        phase("press: the game's y")
        pad(process, xr, BUTTONS['y'], hold_ms=700)
        for _ in range(150):
            step((0., 0., 0.), 'pressed')
            time.sleep(.011)
        after = watch.watched(game, process, xr)
        report['timeline'] = after.get('timeline', [])
        report['census'] = after.get('census', [])

        heights = [s['y'] for s in report['samples'] if s.get('y') is not None and
                   (s['phase'] in ('on spidy wall', 'released', "on the game's crawl", 'unhugged') or
                    s['phase'].startswith('hug'))]
        released = [s for s in report['samples'] if s['phase'] == 'released']
        report['release_trace'] = [dict(t=s['seconds'], y=s.get('y'), tilt=s.get('tilt'), wall=s.get('wall'),
                                        owned=s.get('owned'), contact=s.get('contact'),
                                        air_vertical=s.get('air_vertical'), air_gravity=s.get('air_gravity'),
                                        air_velocity=s.get('air_velocity'), velocity=s.get('velocity'))
                                   for s in released[:12]]
        hugged = [s for s in report['samples'] if s['phase'].startswith('hug')]
        report['hug_air'] = [dict(t=s['seconds'], air_vertical=s.get('air_vertical'), air_gravity=s.get('air_gravity'),
                                  air_velocity=s.get('air_velocity'), air_overrides=s.get('air_overrides'),
                                  air_events=s.get('air_events'), controlled=s.get('controlled'))
                             for s in hugged[-4:]]
        verdict = dict(
            mounted_on=SURFACES.get(mounted.get('wall')),
            before_hug=dict(surface=SURFACES.get(report['before_hug'].get('wall')),
                            distance=report['before_hug'].get('distance'), y=report['before_hug'].get('y'),
                            contact=report['before_hug'].get('contact')),
            game_crawl_took_him=bool(crawled),
            crawl_by=crawled.get('way') if crawled else None,
            crawl_after_seconds=crawled.get('after') if crawled else None,
            after=dict(surface=SURFACES.get(report['after'].get('wall')), y=report['after'].get('y'),
                       tilt=report['after'].get('tilt'), contact=report['after'].get('contact')),
            lowest_y_from_the_vent=round(min(heights)-vent_height, 2) if heights else None,
            hug_air=report.get('hug_air'),
            release_trace=report.get('release_trace'),
            offered_before_press=report['offered_before_press'])
        report['verdict'] = verdict
        print(json.dumps(verdict, indent=1), flush=True)
        for line in report['timeline'][-16:]:
            print('  |', line, flush=True)
    finally:
        if swing_started:
            try:
                call_with_payload(process, rays['SpidySwingSettings'], walls_setting(1, a.gravity))
            except Exception:
                pass
            call_remote(process, rays['SpidySwingStop'])
        if rays_started:
            call_remote(process, rays['SpidyRayStop'])
        if xr:
            call_remote(process, xr['SpidyPadRelease'])
        with open(a.output, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
        close(process)
        game.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())

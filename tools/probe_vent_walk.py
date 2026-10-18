"""The first mission's vent, end to end, with Spidy's swing running and its walls on - as a VR session plays it.

The player, October 14: "i went forward, wall walked to the vent" and then "its not even prompting me to press
anything in walking mode which means that its not registering me". Measured at that vent: the swing's own wall is
one the game calls the air, so it registers nobody there, and it cannot hand a body over in midair either (the
game's first own step falls at 42 m/s). Since October 14 a player WALKED onto a wall is the game's (its crawl,
SwingConfig::mountFromGround off), so this walks him there the way the XR worker does and checks every step:

  walk   the game's stick forward and the swing's stick the same way, A whenever the swing asks for its jump
         (it should not any more); who holds him on the shaft's wall, all the way up
  vent   at the top: what the game is offering, as its own prompt calls say
  enter  the game's Y; whether he goes in

    python tools/probe_vent_walk.py              # from profile 3's start (tools/probe_vent.py loads it)
    python tools/probe_vent_walk.py --web-at 70  # instead: at 70 m up the crawl, shoot a web up and time the
                                                 # takeoff - a swing must not freeze on the game's crawl

It writes reports/vent-walk.json.
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
from probe_wall_crawl import actor, norm, tilt
from probe_wall_run import swing_command
import probe_prompt_watch as watch

XR_EXPORTS = ('SpidyPadSubmit', 'SpidyPadSample', 'SpidyPadRelease', 'SpidyMenuStart', 'SpidyPromptsWatch',
              'SpidyPromptsWatched', 'SpidyPromptsNote')
RAY_EXPORTS = ('SpidyRayStart', 'SpidyRayStop', 'SpidyRaySubmit', 'SpidyRayData', 'SpidySwingStart',
               'SpidySwingSubmit', 'SpidySwingStop', 'SpidySwingData', 'SpidySwingSettings')
SURFACES = {0: 'nothing', 1: "Spidy's wall", 2: "the game's crawl"}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--gravity', type=float, default=7.848)
    p.add_argument('--walk-seconds', type=float, default=30)
    p.add_argument('--web-at', type=float, default=None, help='shoot a web up from the crawl at this height')
    p.add_argument('--no-watch', action='store_true',
                   help="leave the game's prompt observer off, as a VR session does (it killed the game on profile 1)")
    p.add_argument('--jump-buttons', action='store_true', help='with --jump-ways: B, the bumpers, the triggers')
    p.add_argument('--crawl-run', action='store_true',
                   help="at --web-at: climb 3 s with the stick alone, then 3 s with RT held - the game's wall run")
    p.add_argument('--jump-ways', action='store_true',
                   help="at --web-at, try the game's A with each stick way in turn: which one leaves its crawl")
    p.add_argument('--web-on-crawl', action='store_true',
                   help="shoot the web once the game's crawl has held him for a second, wherever that is")
    p.add_argument('--output', default=str(ROOT/'reports/vent-walk.json'))
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
        prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_movement_bridge.dll',
                ROOT/'reports/motion-modules', ('SpidyMotionStart', 'SpidyMotionStop', 'SpidyMotionData'))
        motion_module = next(m['base'] for m in modules(game.pid) if m['name'].lower() == 'spidy_movement_bridge.dll')
        rays, _ = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                          ROOT/'reports/ray-modules', RAY_EXPORTS)

        def invoke(address, payload, what):
            code = call_with_payload(process, address, payload)
            if code:
                raise RuntimeError(f'{what}: {code}')

        if not a.no_watch:
            from probe_menu import settings_payload, FLAGS
            call_with_payload(process, xr['SpidyMenuStart'],
                              settings_payload(FLAGS['web_grab'] | FLAGS['punch'] | FLAGS['body'], 30, 100, 1, 32.))
            call_with_payload(process, xr['SpidyPromptsWatch'], struct.pack('<I', 1))
        invoke(rays['SpidyRayStart'], struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410),
               'World rays start')
        rays_started = True
        invoke(rays['SpidySwingStart'],
               struct.pack('<4I4QI2fI', 0x53574346, 1, 64, game.pid, game.base, record, mover, motion_module,
                           30000, 32., a.gravity, 0), 'Native swing start')
        swing_started = True
        invoke(rays['SpidySwingSettings'], struct.pack('<4IfIfI', 0x53575354, 4, 32, 0, 32., 1, a.gravity, 1),
               'walls on')

        def note(text):
            if a.no_watch:
                return
            call_with_payload(process, xr['SpidyPromptsNote'], text.encode()[:95].ljust(96, b'\0'))

        def press(sx, sy, button):
            """A button, a trigger or both, with the stick, for a moment."""
            buttons = sum(BUTTONS[b] for b in button.split('+') if b in BUTTONS)
            triggers = (255 if 'lt' in button.split('+') else 0, 255 if 'rt' in button.split('+') else 0)
            state = struct.pack('<H2B4h', buttons, *triggers, int(sx*32767), int(sy*32767), 0, 0)
            call_with_payload(process, xr['SpidyPadSubmit'], state+struct.pack('<I', 60))

        def stick(sx, sy, buttons=0, milliseconds=200):
            state = struct.pack('<H2B4h', buttons, 0, 0, int(sx*32767), int(sy*32767), 0, 0)
            call_with_payload(process, xr['SpidyPadSubmit'], state+struct.pack('<I', milliseconds))

        body = actor(game, record)
        ahead = norm(body['forward'])
        report['start'] = dict(feet=[round(v, 2) for v in body['feet']])
        serial = [0]
        begun = time.monotonic()
        asked_for_jump = [0]

        def step(phase, move=(0., 0., 0.), grip=0., aim=(0., 1., 0.)):
            serial[0] += 1
            here = actor(game, record)
            origin = here['feet'] if here else body['feet']
            # Where a player's hand is: a body's length out along whatever he stands on - the world's up on
            # the ground, the wall's normal on a wall. "Up" from feet on a wall is the wall's own surface.
            up = here['up'] if here else (0., 1., 0.)
            hand = (origin[0]+up[0]*1.2, origin[1]+up[1]*1.2, origin[2]+up[2]*1.2)
            call_with_payload(process, rays['SpidySwingSubmit'],
                              swing_command(serial[0], hand, aim, grip, 0., move))
            s = swing_snapshot(game, rays['SpidySwingData']) or {}
            entry = dict(seconds=round(time.monotonic()-begun, 3), phase=phase, wall=s.get('wall'),
                         owned=s.get('owned'), grounded=s.get('grounded'), contact=s.get('native_contact'),
                         mount=s.get('mount'), takeoff=s.get('takeoff'), walls_taken=s.get('walls'),
                         wall_normal=[round(v, 2) for v in (s.get('wall_normal') or (0, 0, 0))],
                         wall_distance=round(s.get('wall_distance') or 0, 2),
                         webs=[w.get('attached') for w in (s.get('webs') or [])])
            if here:
                entry.update(y=round(here['feet'][1], 2), tilt=round(tilt(here['up']), 1))
            # As the worker does: on the game's crawl with a web held, its RT is down already.
            if entry.get('wall') == 2 and any(entry.get('webs') or ()) and not (entry.get('mount') or entry.get('takeoff')):
                press(0, 0, 'rt')
            if entry.get('mount') or entry.get('takeoff'):
                asked_for_jump[0] += 1
                # As the XR worker gives the game its jump: with the stick the player holds, which while
                # walking is forward and while holding a web is nothing.
                press(0, 1 if phase == 'walk' else 0, 'rt+a' if entry.get('wall') == 2 else 'a')
            report['samples'].append(entry)
            return entry

        def phase(name):
            report['phases'][name] = round(time.monotonic()-begun, 3)
            note(f'>> {name}')
            print(f"{report['phases'][name]:6.2f} s  {name}", flush=True)

        phase('walk: forward from the start, swing running, walls on')
        top = None
        last_y, still_for, crawling_for = None, 0., 0.
        until = time.monotonic()+a.walk_seconds
        while time.monotonic() < until:
            stick(0, 1)
            entry = step('walk', move=ahead)
            y = entry.get('y') or 0
            if a.web_at is not None and y >= a.web_at and entry.get('wall') == 2:
                break
            crawling_for = crawling_for+.011 if entry.get('wall') == 2 else 0.
            if a.web_on_crawl and crawling_for > 1.:
                break
            if last_y is not None and abs(y-last_y) < .01:
                still_for += .011
            else:
                still_for = 0.
            last_y = y
            if y > 90 and still_for > .8:
                top = entry
                break
            time.sleep(.011)

        walk = [s for s in report['samples'] if s['phase'] == 'walk']
        seen = sorted({SURFACES.get(s.get('wall'), s.get('wall')) for s in walk})
        verdict = dict(surfaces_on_the_way=seen,
                       swing_wall_samples=sum(1 for s in walk if s.get('wall') == 1),
                       game_crawl_samples=sum(1 for s in walk if s.get('wall') == 2),
                       owned_samples=sum(1 for s in walk if s.get('owned')),
                       asked_for_the_game_jump=asked_for_jump[0],
                       highest_y=max((s.get('y') or 0) for s in walk) if walk else None,
                       lowest_y_after_leaving_the_ground=None)
        climbing = [s for s in walk if (s.get('y') or 0) > report['start']['feet'][1]+1]
        if climbing:
            verdict['lowest_y_after_leaving_the_ground'] = min(s['y'] for s in climbing[len(climbing)//10:])

        if a.crawl_run:
            # How fast the game's crawl climbs on the stick alone, and with its RT held - its wall run.
            speeds = {}
            for name, button in (('stick', ''), ('stick + RT', 'rt')):
                y0 = report['samples'][-1].get('y') or 0
                t0 = time.monotonic()
                while time.monotonic()-t0 < 3:
                    press(0, 1, button) if button else stick(0, 1, milliseconds=60)
                    entry = step(f'climb {name}')
                    time.sleep(.011)
                y1 = report['samples'][-1].get('y') or 0
                surfaces = sorted({SURFACES.get(s.get('wall')) for s in report['samples'] if s['phase'] == f'climb {name}'})
                speeds[name] = dict(metres_a_second=round((y1-y0)/3, 2), from_y=y0, to_y=y1, surfaces=surfaces)
                print(f"   {name}: {speeds[name]}", flush=True)
                if y1 > 88:
                    break
            verdict['crawl_run'] = speeds
            report['verdict'] = verdict
            return 0

        if a.jump_ways:
            # Which of the game's own jumps leaves its crawl. A with the stick neutral did nothing (October
            # 14); with it forward, up along the wall. A failed way leaves him on the crawl for the next.
            results = {}
            ways = (('neutral', (0, 0), 'a', .25), ('back', (0, -1), 'a', .25), ('left', (-1, 0), 'a', .25),
                    ('right', (1, 0), 'a', .25), ('back-left', (-.7, -.7), 'a', .25), ('forward', (0, 1), 'a', .25))
            if a.jump_buttons:
                ways = (('B', (0, 0), 'b', .25), ('B back', (0, -1), 'b', .25), ('A held', (0, 0), 'a', .9),
                        ('LB', (0, 0), 'lb', .25), ('RB', (0, 0), 'rb', .25), ('RT', (0, 0), 'rt', .6),
                        ('LT', (0, 0), 'lt', .6), ('RT+A', (0, 0), 'rt+a', .4))
            for name, (sx, sy), button, held in ways:
                for _ in range(30):
                    stick(0, 0, milliseconds=60)
                    step('settle')
                    time.sleep(.011)
                before = report['samples'][-1]
                phase(f'jump {name}')
                pressed = time.monotonic()
                left = None
                while time.monotonic()-pressed < 1.2:
                    if time.monotonic()-pressed < held:
                        press(sx, sy, button)
                    entry = step(f'jump {name}')
                    if left is None and entry.get('wall') != 2 and (entry.get('tilt') or 90) < 45:
                        left = round(time.monotonic()-pressed, 3)
                    time.sleep(.011)
                after = report['samples'][-1]
                results[name] = dict(left_after_s=left, y_before=before.get('y'), y_after=after.get('y'),
                                     wall_after=SURFACES.get(after.get('wall')), tilt_after=after.get('tilt'),
                                     contact_after=after.get('contact'))
                print(f"   {name}: {results[name]}", flush=True)
                if left is not None:
                    break
            verdict['jump_ways'] = results
            report['verdict'] = verdict
            print(json.dumps(results, indent=1), flush=True)
            return 0

        if a.web_at is not None or a.web_on_crawl:
            # Away from the wall he is on and up, at the shaft's far side: what a player leaving a wall does.
            here = actor(game, record)
            n = norm(here['up']) if here else (0., 0., 1.)
            aim = norm((n[0], n[1]+.35, n[2]))
            phase(f'web: shot from the crawl at {walk[-1].get("y")}, aimed {[round(v, 2) for v in aim]}')
            # The walk's stick lease is still running: a player holding a web is not pushing the stick, so
            # the game's stick goes neutral first. Forward on the crawl is up the wall, and A with it is a
            # jump up along the wall rather than off it.
            for _ in range(40):
                stick(0, 0, milliseconds=60)
                step('web', aim=aim)
                time.sleep(.011)
            shot = time.monotonic()
            took = None
            for _ in range(160):
                entry = step('web', grip=1., aim=aim)
                if took is None and entry.get('owned'):
                    took = round(time.monotonic()-shot, 3)
                time.sleep(.011)
            web = [s for s in report['samples'] if s['phase'] == 'web']
            verdict.update(web_owned_after_seconds=took,
                           web_rose_m=round(max(s.get('y') or 0 for s in web)-(web[0].get('y') or 0), 2),
                           web_surfaces=sorted({SURFACES.get(s.get('wall'), s.get('wall')) for s in web}))
            print(json.dumps(verdict, indent=1), flush=True)
            report['verdict'] = verdict
            return 0

        phase('vent: at the top')
        for _ in range(90):
            step('vent')
            time.sleep(.011)
        offered = watch.watched(game, process, xr)
        verdict['at_the_vent'] = dict(surface=SURFACES.get(report['samples'][-1].get('wall')),
                                      y=report['samples'][-1].get('y'), tilt=report['samples'][-1].get('tilt'),
                                      offered=dict(demand=offered.get('demand'), last=offered.get('last_demand')))
        before = actor(game, record)
        phase("enter: the game's y")
        pad(process, xr, BUTTONS['y'], hold_ms=700)
        for _ in range(300):
            step('enter')
            time.sleep(.011)
        after = actor(game, record)
        moved = None
        if before and after:
            moved = round(sum((x-y)**2 for x, y in zip(after['feet'], before['feet']))**.5, 2)
        final = watch.watched(game, process, xr)
        verdict['after_y'] = dict(moved_m=moved, feet=[round(v, 2) for v in after['feet']] if after else None)
        report['timeline'] = final.get('timeline', [])
        report['verdict'] = verdict
        print(json.dumps(verdict, indent=1), flush=True)
        for line in report['timeline'][-14:]:
            print('  |', line, flush=True)
    finally:
        if swing_started:
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

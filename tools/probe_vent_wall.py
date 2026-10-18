"""Does the game still offer its own interact while Spidy's wall running owns the hero?

The question October 13 left open. A player climbs to the vent of the first mission on the wall it sits in,
the prompt is there, and the game's interact does nothing - with Spidy's walls on, VR play holds the hero in
the game's AIR state (that is how the swing drives him), and a game does not let an airborne player interact.

Run it with the game already standing at the interact (probe_vent.py walks there), so that the only thing
that changes is who holds the player:

    python tools/probe_vent_wall.py --walls on      # Spidy's wall: does the interact survive?
    python tools/probe_vent_wall.py --walls off     # the game's own crawl, for comparison

It prints the swing's hold (owned, and which surface), what the game was offering before and after, and the
prompt timeline with the probe's own presses in it. `SpidyPadRelease` hands the controller back at the end.
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
from probe_wall_crawl import actor, norm
from probe_wall_run import settings, swing_command
import probe_prompt_watch as watch

XR_EXPORTS = ('SpidyPadSubmit', 'SpidyPadSample', 'SpidyPadRelease', 'SpidyMenuStart', 'SpidyPromptsWatch',
              'SpidyPromptsWatched', 'SpidyPromptsNote')
RAY_EXPORTS = ('SpidyRayStart', 'SpidyRayStop', 'SpidyRaySubmit', 'SpidyRayData', 'SpidySwingStart',
               'SpidySwingSubmit', 'SpidySwingStop', 'SpidySwingData', 'SpidySwingSettings')
SURFACES = {0: 'nothing', 1: "the swing's own wall", 2: "the game's wall crawl"}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--walls', choices=('on', 'off'), default='on', help="Spidy's own wall running")
    p.add_argument('--seconds', type=float, default=10, help='how long to hold the hero')
    p.add_argument('--press-at', type=float, default=5, help='when to send the game its interact')
    p.add_argument('--gravity', type=float, default=.8)
    p.add_argument('--output', default=str(ROOT/'reports/vent-wall.json'))
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
    report = dict(walls=a.walls, samples=[])
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

        # The prompt reading, as a VR session has it.
        from probe_menu import settings_payload, FLAGS
        call_with_payload(process, xr['SpidyMenuStart'],
                          settings_payload(FLAGS['web_grab'] | FLAGS['punch'] | FLAGS['body'], 30, 100, 1, 32.))
        call_with_payload(process, xr['SpidyPromptsWatch'], struct.pack('<I', 1))
        before = watch.watched(game, process, xr)
        report['offered_before'] = dict(demand=before.get('demand'), last=before.get('last_demand'))

        invoke(rays['SpidyRayStart'], struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410),
               'World rays start')
        rays_started = True
        invoke(rays['SpidySwingStart'],
               struct.pack('<4I4QI2fI', 0x53574346, 1, 64, game.pid, game.base, record, mover, motion_module,
                           30000, 32., a.gravity, 0), 'Native swing start')
        swing_started = True
        invoke(rays['SpidySwingSettings'], settings(a.walls == 'on', a.gravity), 'Swing settings')

        body = actor(game, record)
        if not body:
            raise RuntimeError("The player's actor could not be read")
        # Walking straight at what he faces is how a player mounts a wall in VR.
        at_the_wall = norm(body['forward'])
        report['start'] = dict(feet=body['feet'], walking=at_the_wall)

        def note(text):
            call_with_payload(process, xr['SpidyPromptsNote'], text.encode()[:95].ljust(96, b'\0'))

        note(f"the probe holds the hero, Spidy's walls {a.walls}")
        serial = 0
        started = time.monotonic()
        pressed = False
        while True:
            elapsed = time.monotonic()-started
            if elapsed > a.seconds:
                break
            serial += 1
            hand = actor(game, record)
            origin = hand['feet'] if hand else body['feet']
            call_with_payload(process, rays['SpidySwingSubmit'],
                              swing_command(serial, origin, at_the_wall, move=at_the_wall))
            if not pressed and elapsed >= a.press_at:
                pressed = True
                s = swing_snapshot(game, rays['SpidySwingData']) or {}
                note(f"the probe's y, held by {SURFACES.get(s.get('wall'), s.get('wall'))}")
                pad(process, xr, BUTTONS['y'], hold_ms=700)
            if serial % 20 == 0:
                s = swing_snapshot(game, rays['SpidySwingData']) or {}
                report['samples'].append(dict(seconds=round(elapsed, 2), owned=s.get('owned'),
                                              wall=s.get('wall'), grounded=s.get('grounded'),
                                              collision=s.get('collision_flags')))
            time.sleep(.011)
        after = watch.watched(game, process, xr)
        report['offered_after'] = dict(demand=after.get('demand'), last=after.get('last_demand'))
        report['timeline'] = after.get('timeline', [])
        report['census'] = after.get('census', [])
        held = [s for s in report['samples'] if s.get('wall')]
        print(json.dumps(dict(walls=a.walls, samples=len(report['samples']),
                              on_a_wall=len(held), surfaces=sorted({SURFACES.get(s['wall'], s['wall'])
                                                                    for s in held}),
                              owned=sum(1 for s in report['samples'] if s.get('owned')),
                              offered_before=report['offered_before'],
                              offered_after=report['offered_after']), indent=1), flush=True)
        for line in report['timeline'][-12:]:
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

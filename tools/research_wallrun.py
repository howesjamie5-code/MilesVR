"""The game's own wall run, with nothing of Spidy's moving the player: walk onto the shaft wall (its crawl), then
hold RT with the stick, and record his height, his speed and how he lies (tilt from upright: 90 on the wall).

    python research_wallrun.py [--ways forward,rt-forward,rt-neutral,...]
"""
import argparse
import json
import struct
import sys
import time

from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from observe_game import call_remote, call_with_payload
from probe_game_grab import HERO_LOCAL, registry
from probe_menu_pad import XR_DLL
from probe_wall_crawl import actor, tilt
from probe_game_grab import HERO_MOVERS, resolve
from probe_native_motion import snapshot as motion_snapshot
from vr_launcher import bring_to_front

XR_EXPORTS = ('SpidyPadSubmit', 'SpidyPadSample', 'SpidyPadRelease')
RAY_EXPORTS = ('SpidySwingStop', 'SpidyRayStop', 'SpidyShooterStop')
WAYS = {
    'forward': (0, 1, 0),        # the stick alone: the crawl
    'rt-forward': (0, 1, 255),   # RT held with it: the wall run?
    'rt-neutral': (0, 0, 255),
    'rt-right': (1, 0, 255),
    'rt-back': (0, -1, 255),
    'right': (1, 0, 0),
}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--ways', default='forward,rt-forward')
    p.add_argument('--seconds', type=float, default=2.0)
    p.add_argument('--walk-in', type=float, default=1.5, help='seconds of stick forward to get onto the wall first')
    p.add_argument('--output', default=str(ROOT / 'reports/research-wallrun.json'))
    a = p.parse_args()
    game = Game(find_game())
    record = [r for at, v, r, _ in registry(game) if v == HERO_LOCAL][0]
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    report = dict(ways={})
    try:
        xr, _ = prepare(game.pid, process, XR_DLL, ROOT / 'reports/stereo-modules', XR_EXPORTS)
        try:
            rays, _ = prepare(game.pid, process, ROOT / 'build/windows-ninja/spidy_ray_bridge.dll',
                              ROOT / 'reports/ray-modules', RAY_EXPORTS)
            for name in RAY_EXPORTS:
                call_remote(process, rays[name])
        except Exception as ex:
            print('no ray bridge to stop:', ex)
        comps = list(registry(game))
        managers = [at for at, v, r, _ in comps if v == HERO_MOVERS and r == record]
        mover = resolve(game, struct.unpack('<I', game.read(managers[0] + 0xdb4, 4))[0])
        motion, _ = prepare(game.pid, process, ROOT / 'build/windows-ninja/spidy_movement_bridge.dll',
                            ROOT / 'reports/motion-modules', ('SpidyMotionStart', 'SpidyMotionStop', 'SpidyMotionData'))
        call_remote(process, motion['SpidyMotionStop'])
        code = call_with_payload(process, motion['SpidyMotionStart'], struct.pack('<4I3QIf2I', 0x534d5643, 2, 56,
                                 game.pid, game.base, record, mover, 30000, 32.0, 1, 0))
        print('motion start:', code, flush=True)
        print('front:', bring_to_front(game.pid), flush=True)

        def pad(sx, sy, rt, ms=80):
            state = struct.pack('<H2B4h', 0, 0, rt, int(sx * 32767), int(sy * 32767), 0, 0)
            call_with_payload(process, xr['SpidyPadSubmit'], state + struct.pack('<I', ms))

        def sample():
            b = actor(game, record)
            m = motion_snapshot(game, motion['SpidyMotionData']) or {}
            return dict(t=time.monotonic(), feet=b['feet'], tilt=tilt(b['up']), mover=m.get('mover_flags'),
                        contact=m.get('contact'), grounded=m.get('grounded'),
                        collision=m.get('collision_flags'), steps=m.get('steps')) if b else None

        def hold(sx, sy, rt, seconds):
            out = []
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                pad(sx, sy, rt)
                s = sample()
                if s:
                    out.append(s)
                time.sleep(.03)
            return out

        if a.walk_in > 0:
            hold(0, 1, 0, a.walk_in)
            s = sample()
            print(f"after walking in: feet {[round(v, 2) for v in s['feet']]} tilt {s['tilt']:.0f}", flush=True)
        for way in a.ways.split(','):
            sx, sy, rt = WAYS[way]
            trace = hold(sx, sy, rt, a.seconds)
            # And let go of everything for a moment: where does it leave him?
            after = hold(0, 0, 0, .8)
            first, last = trace[0], trace[-1]
            dt = last['t'] - first['t']
            moved = [last['feet'][i] - first['feet'][i] for i in range(3)]
            peak = max(((x['feet'][0] - y['feet'][0]) ** 2 + (x['feet'][1] - y['feet'][1]) ** 2 +
                        (x['feet'][2] - y['feet'][2]) ** 2) ** .5 / max(x['t'] - y['t'], 1e-3)
                       for x, y in zip(trace[1:], trace[:-1]))
            summary = dict(seconds=round(dt, 2), moved=[round(v, 2) for v in moved],
                           mean_speed=round(sum(v * v for v in moved) ** .5 / max(dt, 1e-3), 2),
                           peak_speed=round(peak, 2), tilt_from=round(first['tilt']), tilt_to=round(last['tilt']),
                           tilts=sorted({round(x['tilt'] / 15) * 15 for x in trace}),
                           after=dict(feet=[round(v, 2) for v in after[-1]['feet']], tilt=round(after[-1]['tilt'])),
                           movers=sorted({hex(x['mover'] or 0) for x in trace}),
                           contacts=sorted({x['contact'] for x in trace if x['contact'] is not None}),
                           grounded=sorted({x['grounded'] for x in trace if x['grounded'] is not None}),
                           collisions=sorted({hex(x['collision'] or 0) for x in trace}),
                           steps=(trace[0]['steps'], trace[-1]['steps']))
            print('  timeline:', ' '.join(f"{x['t'] - first['t']:.2f}:{hex(x['mover'] or 0)}/{x['contact']}/y{x['feet'][1]:.1f}/t{x['tilt']:.0f}"
                                         for x in trace[::4]), flush=True)
            report['ways'][way] = dict(summary=summary, trace=[dict(t=round(x['t'] - first['t'], 3),
                                                                  feet=[round(v, 3) for v in x['feet']],
                                                                  tilt=round(x['tilt'], 1)) for x in trace])
            print(way, json.dumps(summary), flush=True)
    finally:
        try:
            call_remote(process, xr['SpidyPadRelease'])
            call_remote(process, motion['SpidyMotionStop'])
        except Exception:
            pass
        with open(a.output, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
        close(process)
        game.close()


if __name__ == '__main__':
    sys.exit(main())

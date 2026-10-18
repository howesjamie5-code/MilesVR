"""How does the game's own wall crawl read its stick? On the crawl, with the camera turned several ways, push the
left stick eight ways and record where the hero went along the wall, with the game camera's axes at that moment.

    python research_crawl_shaft.py [--cameras none,yaw+,yaw-,pitch+,pitch-]

Needs a fresh game with the hero on the lift shaft wall's crawl (profile 3, climbed to ~60 m). There the
game's camera is locked looking up the shaft and the right stick does not turn it.
"""
import argparse
import json
import math
import struct
import sys
import time

from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from observe_game import call_remote, call_with_payload
from probe_game_grab import HERO_LOCAL, registry
from probe_menu_pad import XR_DLL
from probe_wall_crawl import actor, tilt
from vr_launcher import bring_to_front

CAMERAS = {'none': (0, 0, 0), 'yaw+': (1, 0, .5), 'yaw-': (-1, 0, 1.0), 'pitch+': (0, 1, .5), 'pitch-': (0, -1, 1.0),
           'yaw++': (1, 0, 1.2), 'pitch++': (0, 1, 1.2)}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cameras', default='none,yaw+,yaw-,pitch+,pitch-')
    p.add_argument('--push', type=float, default=1.0)
    p.add_argument('--output', default=str(ROOT / 'reports/research-crawl-stick.json'))
    a = p.parse_args()
    game = Game(find_game())
    comps = list(registry(game))
    heroes = [(at, r) for at, v, r, _ in comps if v == HERO_LOCAL]
    if len(heroes) != 1:
        raise SystemExit('one hero needed')
    hero, record = heroes[0]
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    report = dict(tests=[])
    xr = bridge = None
    try:
        xr, _ = prepare(game.pid, process, XR_DLL, ROOT / 'reports/stereo-modules',
                        ('SpidyPadSubmit', 'SpidyPadRelease'))
        bridge, _ = prepare(game.pid, process)
        code = call_with_payload(process, bridge['SpidyStart'],
                                 struct.pack('<4I3Q', 0x53424346, 1, 40, game.pid, game.base, hero, record))
        print('camera bridge start:', code, flush=True)
        print('front:', bring_to_front(game.pid), flush=True)

        def pad(lx=0., ly=0., rx=0., ry=0., ms=150):
            state = struct.pack('<H2B4h', 0, 0, 0, int(lx * 32767), int(ly * 32767), int(rx * 32767), int(ry * 32767))
            call_with_payload(process, xr['SpidyPadSubmit'], state + struct.pack('<I', ms))

        raw = game.read(bridge['SpidyBridgeData'], 384)
        mover = struct.unpack_from('<Q', raw, 88)[0]
        holder = struct.unpack('<Q', game.read(mover + 8, 8))[0]
        print('camera mover', hex(mover), 'vtable', hex(struct.unpack('<Q', game.read(mover, 8))[0] - game.base), flush=True)

        def camera():
            address = struct.unpack('<Q', game.read(holder, 8))[0]
            t = game.transform(address)
            if not t:
                return None
            r = t['basis']
            return dict(r0=list(r[0]), r1=list(r[1]), r2=list(r[2]), pos=list(t['position']))

        def sample():
            b = actor(game, record)
            cam = camera()
            return dict(t=time.monotonic(), feet=list(b['feet']), up=list(b['up']), fwd=list(b['forward']),
                        tilt=tilt(b['up']), cam=cam) if b else None

        def hold(lx, ly, rx, ry, seconds):
            out = []
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                pad(lx, ly, rx, ry)
                s = sample()
                if s:
                    out.append(s)
                time.sleep(.025)
            return out

        start = sample()
        print('start: feet', [round(v, 2) for v in start['feet']], 'tilt', round(start['tilt']), 'cam', start['cam'],
              flush=True)
        for name in a.cameras.split(','):
            cx, cy, cs = CAMERAS[name]
            if cs:
                hold(0, 0, cx, cy, cs)
            hold(0, 0, 0, 0, .4)
            for k in range(8):
                ang = math.radians(45 * k)
                sx, sy = math.sin(ang), math.cos(ang)
                trace = hold(sx * .999, sy * .999, 0, 0, a.push)
                back = hold(-sx * .999, -sy * .999, 0, 0, a.push * .8)
                rest = hold(0, 0, 0, 0, .3)
                report['tests'].append(dict(camera=name, angle=45 * k, stick=[sx, sy], trace=trace, back=back,
                                            rest=rest[-1] if rest else None))
                if len(trace) > 4:
                    f, l = [x for x in trace if x['t'] >= trace[-1]['t'] - .4][0], trace[-1]
                    d = [l['feet'][i] - f['feet'][i] for i in range(3)]
                    dt = l['t'] - f['t']
                    print(f"{name:7} stick {45*k:3d}: moved {[round(x, 2) for x in d]} in {dt:.2f}s tilt {l['tilt']:.0f} "
                          f"cam r2 {[round(x, 2) for x in l['cam']['r2']] if l['cam'] else None} feet {[round(x, 1) for x in l['feet']]}",
                          flush=True)
    finally:
        try:
            if xr:
                call_remote(process, xr['SpidyPadRelease'])
            if bridge:
                call_remote(process, bridge['SpidyStop'])
        except Exception as ex:
            print('cleanup:', ex)
        with open(a.output, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
        close(process)
        game.close()


if __name__ == '__main__':
    sys.exit(main())

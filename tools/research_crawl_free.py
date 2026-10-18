"""The game's crawl stick rule where its camera is free: walk the hero into the nearest wall, then for several
camera turns push the left stick eight ways, recording the hero's motion and the camera's axes.

    python research_crawl_free.py
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
from probe_native_rays import snapshot as ray_snapshot, command as ray_command
from probe_wall_crawl import actor, tilt
from vr_launcher import bring_to_front

RAY_EXPORTS = ('SpidyRayStart', 'SpidyRayStop', 'SpidyRaySubmit', 'SpidyRayData')
CAMERAS = [('none', 0, 0, 0), ('yaw+', 1, 0, .6), ('yaw+', 1, 0, .6), ('yaw-', -1, 0, 1.8), ('pitch+', 0, 1, .5),
           ('pitch-', 0, -1, 1.0), ('yaw+', 1, 0, 1.2)]


def norm(v):
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v] if n > 1e-9 else [0, 0, 0]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--push', type=float, default=1.0)
    p.add_argument('--output', default=str(ROOT / 'reports/research-crawl-free.json'))
    p.add_argument('--climb', type=float, default=1.0)
    a = p.parse_args()
    game = Game(find_game())
    hero, record = [(at, r) for at, v, r, _ in registry(game) if v == HERO_LOCAL][0]
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    report = dict(tests=[], walk=[])
    xr = bridge = rays = None
    try:
        xr, _ = prepare(game.pid, process, XR_DLL, ROOT / 'reports/stereo-modules', ('SpidyPadSubmit', 'SpidyPadRelease'))
        bridge, _ = prepare(game.pid, process)
        print('bridge start', call_with_payload(process, bridge['SpidyStart'],
              struct.pack('<4I3Q', 0x53424346, 1, 40, game.pid, game.base, hero, record)), flush=True)
        rays, _ = prepare(game.pid, process, ROOT / 'build/windows-ninja/spidy_ray_bridge.dll',
                          ROOT / 'reports/ray-modules', RAY_EXPORTS)
        call_remote(process, rays['SpidyRayStop'])
        print('rays start', call_with_payload(process, rays['SpidyRayStart'],
              struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410)), flush=True)
        print('front', bring_to_front(game.pid), flush=True)

        def pad(lx=0., ly=0., rx=0., ry=0., ms=150):
            st = struct.pack('<H2B4h', 0, 0, 0, int(lx * 32767), int(ly * 32767), int(rx * 32767), int(ry * 32767))
            call_with_payload(process, xr['SpidyPadSubmit'], st + struct.pack('<I', ms))

        def camera():
            raw = game.read(bridge['SpidyBridgeData'], 384)
            m = struct.unpack_from('<16f', raw, 144)
            if any(m[0:3]):
                return dict(r0=list(m[0:3]), r1=list(m[4:7]), r2=list(m[8:11]), pos=list(m[12:15]), via='bridge')
            t = game.transform(game.pointer(game.base + 0x7a34dd0))
            if t:
                r = t['basis']
                return dict(r0=list(r[0]), r1=list(r[1]), r2=list(r[2]), pos=list(t['position']), via='global')
            return None

        def sample():
            b = actor(game, record)
            return dict(t=time.monotonic(), feet=list(b['feet']), up=list(b['up']), tilt=tilt(b['up']),
                        cam=camera()) if b else None

        def hold(lx, ly, rx, ry, seconds):
            res = []
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                pad(lx, ly, rx, ry)
                s = sample()
                if s:
                    res.append(s)
                time.sleep(.025)
            return res

        serial = [int(time.time())]

        def cast(batch):
            serial[0] += 1
            call_with_payload(process, rays['SpidyRaySubmit'], ray_command(serial[0], batch))
            for _ in range(60):
                shot = ray_snapshot(game, rays['SpidyRayData'])
                if shot and shot.get('serial') == serial[0] and shot.get('count'):
                    return shot['hits']
                time.sleep(.016)
            return []

        s0 = sample()
        print('start feet', [round(x, 2) for x in s0['feet']], 'tilt', round(s0['tilt']),
              'cam', s0['cam'] and s0['cam']['via'], flush=True)
        if s0['tilt'] < 45:
            eye = [s0['feet'][0], s0['feet'][1] + 1.2, s0['feet'][2]]
            found = []
            for sweep in range(3):
                batch = [(eye, (math.cos((sweep * 8 + i) * (2 * math.pi / 24)), 0.,
                                math.sin((sweep * 8 + i) * (2 * math.pi / 24))), 40., i) for i in range(8)]
                for (_, d, dist, _), hit in zip(batch, cast(batch)):
                    reach = hit['fraction'] * dist
                    if hit['count'] and abs(hit['normal'][1]) < .35 and dot(hit['normal'], d) < -.3 and \
                            1.0 <= reach <= 35:
                        found.append(dict(distance=reach, normal=list(hit['normal']), direction=list(d),
                                          point=list(hit['position'])))
            if not found:
                raise SystemExit('no wall near')
            wall = min(found, key=lambda w: w['distance'])
            report['wall'] = wall
            print('wall', round(wall['distance'], 1), 'm, normal', [round(x, 2) for x in wall['normal']], flush=True)
            until = time.monotonic() + 15
            while time.monotonic() < until:
                s = sample()
                if s['tilt'] > 45:
                    print('on the crawl at', [round(x, 2) for x in s['feet']], flush=True)
                    break
                cam = s['cam']
                if not cam:
                    time.sleep(.02)
                    continue
                f = norm([cam['r2'][0], 0, cam['r2'][2]])
                r = cross(f, [0, 1, 0])
                to = norm([wall['point'][0] - s['feet'][0], 0, wall['point'][2] - s['feet'][2]])
                pad(dot(to, r) * .999, dot(to, f) * .999)
                report['walk'].append(s)
                time.sleep(.03)
            else:
                raise SystemExit('never reached the crawl')
        # Up the wall a little, so every way has room.
        hold(0, 1, 0, 0, a.climb)
        hold(0, 0, 0, 0, .4)
        for name, cx, cy, cs in CAMERAS:
            if cs:
                hold(0, 0, cx, cy, cs)
            hold(0, 0, 0, 0, .3)
            for k in range(8):
                ang = math.radians(45 * k)
                sx, sy = math.sin(ang), math.cos(ang)
                trace = hold(sx * .999, sy * .999, 0, 0, a.push)
                hold(-sx * .999, -sy * .999, 0, 0, a.push * .8)
                hold(0, 0, 0, 0, .3)
                report['tests'].append(dict(camera=name, angle=45 * k, stick=[sx, sy], trace=trace))
                late = [x for x in trace if x['t'] >= trace[-1]['t'] - .4]
                f, l = late[0], late[-1]
                d = [l['feet'][i] - f['feet'][i] for i in range(3)]
                c = l['cam']
                print(f"{name:7} {45*k:3d}: moved {[round(x, 2) for x in d]} tilt {l['tilt']:.0f} "
                      f"up {[round(x, 2) for x in l['up']]} "
                      f"cam f {[round(x, 2) for x in c['r2']] if c else None} "
                      f"u {[round(x, 2) for x in c['r1']] if c else None}", flush=True)
    finally:
        try:
            if xr:
                call_remote(process, xr['SpidyPadRelease'])
            if bridge:
                call_remote(process, bridge['SpidyStop'])
            if rays:
                call_remote(process, rays['SpidyRayStop'])
        except Exception as ex:
            print('cleanup', ex)
        with open(a.output, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
        close(process)
        game.close()


if __name__ == '__main__':
    sys.exit(main())

"""Does the new crawl stick rule move the hero where the player means, where the old one did not?

On the game's own crawl, for several camera turns, it asks for eight directions along the wall, each sent as
the new rule (GameTrackingRig, October 15) and as the old one would have sent it, recomputed from the live
camera every frame, and measures the angle between the way asked for and the way the hero went.

    python research_crawl_verify.py
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

CAMERAS = [('none', 0, 0, 0), ('yaw+', 1, 0, .6), ('yaw-', -1, 0, 1.2), ('pitch+', 0, 1, .5), ('pitch-', 0, -1, .9)]


def norm(v):
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v] if n > 1e-9 else [0, 0, 0]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def sub(a, b):
    return [x - y for x, y in zip(a, b)]


def scale(a, s):
    return [x * s for x in a]


def plane(v, n):
    return sub(v, scale(n, dot(v, n)))


def turn_up_to(n, v):
    """The rotation that takes the world's up to n, applied to v (the view's turn onto the wall)."""
    up = [0, 1, 0]
    axis = cross(up, n)
    s = math.sqrt(dot(axis, axis))
    c = dot(up, n)
    if s < 1e-6:
        return v
    axis = scale(axis, 1 / s)
    angle = math.atan2(s, c)
    return [v[i] * math.cos(angle) + cross(axis, v)[i] * math.sin(angle) + axis[i] * dot(axis, v) * (1 - math.cos(angle))
            for i in range(3)]


def turn_back(n, v):
    up = [0, 1, 0]
    axis = cross(up, n)
    s = math.sqrt(dot(axis, axis))
    c = dot(up, n)
    if s < 1e-6:
        return v
    axis = scale(axis, -1 / s)
    angle = math.atan2(s, c)
    return [v[i] * math.cos(angle) + cross(axis, v)[i] * math.sin(angle) + axis[i] * dot(axis, v) * (1 - math.cos(angle))
            for i in range(3)]


def new_stick(want, n, cam):
    """GameTrackingRig, October 15: the camera's look laid onto the surface, or its up where it faces it."""
    look = plane(cam['r2'], n)
    top = plane(cam['r1'], n)
    along = look if math.sqrt(dot(look, look)) > .2 else top
    ahead = norm(along)
    side = cross(ahead, n)
    return dot(want, side), dot(want, ahead)


def old_stick(want, n, cam):
    """GameTrackingRig before: the camera's level look laid onto the wall, or the level axes."""
    f = norm([cam['r2'][0], 0, cam['r2'][2]])
    inside = plane(f, n)
    if math.sqrt(dot(inside, inside)) > .3:
        ahead = norm(inside)
        side = cross(ahead, n)
        return dot(want, side), dot(want, ahead)
    level = turn_back(n, want)
    return dot(level, cross(f, [0, 1, 0])), dot(level, f)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--push', type=float, default=1.0)
    p.add_argument('--output', default=str(ROOT / 'reports/research-crawl-verify.json'))
    a = p.parse_args()
    game = Game(find_game())
    hero, record = [(at, r) for at, v, r, _ in registry(game) if v == HERO_LOCAL][0]
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    report = dict(tests=[])
    xr = bridge = None
    try:
        # The pad from whichever stereo module the game has resident (a rebuild since does not matter here).
        from observe_game import modules
        import pathlib
        resident = next((pathlib.Path(m['path']) for m in modules(game.pid) if m['name'].lower() == 'spidy_stereo_probe.dll'),
                        None)
        xr, _ = prepare(game.pid, process, resident or XR_DLL, ROOT / 'reports/stereo-modules',
                        ('SpidyPadSubmit', 'SpidyPadRelease'))
        bridge, _ = prepare(game.pid, process)
        print('bridge start', call_with_payload(process, bridge['SpidyStart'],
              struct.pack('<4I3Q', 0x53424346, 1, 40, game.pid, game.base, hero, record)), flush=True)
        print('front', bring_to_front(game.pid), flush=True)

        def pad(lx=0., ly=0., rx=0., ry=0., buttons=0, ms=150):
            m = math.hypot(lx, ly)
            if m > .999:
                lx, ly = lx * .999 / m, ly * .999 / m
            st = struct.pack('<H2B4h', buttons, 0, 0, int(lx * 32767), int(ly * 32767), int(rx * 32767), int(ry * 32767))
            call_with_payload(process, xr['SpidyPadSubmit'], st + struct.pack('<I', ms))

        def camera():
            raw = game.read(bridge['SpidyBridgeData'], 384)
            m = struct.unpack_from('<16f', raw, 144)
            if any(m[0:3]):
                return dict(r0=list(m[0:3]), r1=list(m[4:7]), r2=list(m[8:11]), pos=list(m[12:15]))
            return None

        def sample():
            b = actor(game, record)
            return dict(t=time.monotonic(), feet=list(b['feet']), up=list(b['up']), tilt=tilt(b['up']),
                        cam=camera()) if b else None

        def hold(lx, ly, rx, ry, seconds):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                pad(lx, ly, rx, ry)
                time.sleep(.025)

        s = sample()
        print('start feet', [round(x, 2) for x in s['feet']], 'tilt', round(s['tilt']), flush=True)
        if s['tilt'] < 45:
            # Onto the wall: jump at it with the stick toward it (the facade is the camera's way).
            t0 = time.monotonic()
            while time.monotonic() - t0 < 6:
                el = time.monotonic() - t0
                pad(0, 1, buttons=0x1000 if .2 < el < .35 else 0)
                time.sleep(.03)
                s = sample()
                if s['tilt'] > 45 and el > .8:
                    break
            print('on the wall?', [round(x, 2) for x in s['feet']], round(s['tilt']), flush=True)
            if s['tilt'] < 45:
                raise SystemExit('not on the crawl')
        hold(0, 1, 0, 0, 1.2)
        hold(0, 0, 0, 0, .4)
        directions = [(name, ang) for name, ang in (('up', 0), ('up-right', 45), ('right', 90), ('down-right', 135),
                                                     ('down', 180), ('down-left', 225), ('left', 270),
                                                     ('up-left', 315))]
        for cname, cx, cy, cs in CAMERAS:
            if cs:
                hold(0, 0, cx, cy, cs)
            hold(0, 0, 0, 0, .3)
            for rule in ('new', 'old'):
                for dname, ang in directions:
                    trace = []
                    until = time.monotonic() + a.push
                    want = None
                    while time.monotonic() < until:
                        s = sample()
                        if not s or not s['cam']:
                            time.sleep(.02)
                            continue
                        n = norm(s['up'])
                        # The wall's own axes: up along it, and its right as the player standing on it faces up.
                        upw = norm(plane([0, 1, 0], n))
                        rightw = cross(upw, n)
                        want = norm([math.cos(math.radians(ang)) * upw[i] + math.sin(math.radians(ang)) * rightw[i]
                                     for i in range(3)])
                        lx, ly = (new_stick if rule == 'new' else old_stick)(want, n, s['cam'])
                        pad(lx, ly)
                        s.update(want=want, stick=[lx, ly])
                        trace.append(s)
                        time.sleep(.02)
                    # Back where it started, on the same rule, and a rest.
                    until = time.monotonic() + a.push * .8
                    while time.monotonic() < until:
                        s = sample()
                        if s and s['cam']:
                            n = norm(s['up'])
                            upw = norm(plane([0, 1, 0], n))
                            rightw = cross(upw, n)
                            back = [-(math.cos(math.radians(ang)) * upw[i] + math.sin(math.radians(ang)) * rightw[i])
                                    for i in range(3)]
                            lx, ly = (new_stick if rule == 'new' else old_stick)(back, n, s['cam'])
                            pad(lx, ly)
                        time.sleep(.02)
                    hold(0, 0, 0, 0, .3)
                    late = [x for x in trace if x['t'] >= trace[-1]['t'] - .4]
                    f, l = late[0], late[-1]
                    d = sub(l['feet'], f['feet'])
                    n = norm(l['up'])
                    went = norm(plane(d, n))
                    speed = math.sqrt(dot(plane(d, n), plane(d, n))) / max(l['t'] - f['t'], 1e-3)
                    err = math.degrees(math.atan2(dot(cross(l['want'], went), n), dot(l['want'], went)))
                    report['tests'].append(dict(camera=cname, rule=rule, direction=dname, error=err, speed=speed,
                                                trace=trace))
                    print(f"{cname:7} {rule}  {dname:10} error {err:7.1f} deg  speed {speed:4.2f} m/s  "
                          f"stick {[round(x, 2) for x in l['stick']]}  camF {[round(x, 2) for x in l['cam']['r2']]}",
                          flush=True)
        for rule in ('new', 'old'):
            e = [abs(t['error']) for t in report['tests'] if t['rule'] == rule]
            sp = [t['speed'] for t in report['tests'] if t['rule'] == rule]
            e.sort()
            print(f"{rule}: mean |error| {sum(e)/len(e):.1f}, median {e[len(e)//2]:.1f}, worst {e[-1]:.1f}, "
                  f"over 45: {sum(1 for x in e if x > 45)} of {len(e)}, mean speed {sum(sp)/len(sp):.2f} m/s", flush=True)
    finally:
        try:
            if xr:
                call_remote(process, xr['SpidyPadRelease'])
            if bridge:
                call_remote(process, bridge['SpidyStop'])
        except Exception as ex:
            print('cleanup', ex)
        with open(a.output, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
        close(process)
        game.close()


if __name__ == '__main__':
    sys.exit(main())

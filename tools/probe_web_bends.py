"""Lay one of the game's ropes round edges and read back the points the game draws, without a headset.

A web that bends round edges is drawn with the game's own rope (native_webs): Spidy lays its first points,
the wrist and then each edge, and the game draws on to where the web sticks. Until October 15 the game's
line left from the wrist again rather than from the last edge: a second line from the hand to the web's
end, folded back on itself at the moment a web takes an edge (that edge lies on the line from the wrist to
the end). In a headset session the game closed 0.1 s after one such moment, with no crash report.

The probe holds one game web from a point in front of the hero and walks it through the cases below. Every
rope the game builds is read back and checked: it leaves the wrist, passes each edge in order, and goes on
from the last of them to the end without coming back toward the wrist. It reports whether the game kept
running, and saves a window picture per case in reports/web-bends-probe/.

    python tools/probe_web_bends.py                         the cases with this build
    python tools/probe_web_bends.py --modules DIR           spidy_stereo_probe.dll from DIR (a before/after)

It needs a freshly started game in free roam (Spidy's modules start once per process), which it brings to
the front: the game pauses behind other windows.
"""
import argparse
import json
import math
import pathlib
import struct
import sys
import time
from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from observe_game import call_remote, call_with_payload
from probe_game_grab import registry, camera, HERO_LOCAL, ROPE_MANAGER
from probe_game_screen import game_window, client_size, window_rgb
from run_game_vr import write_rgb_png
from vr_launcher import alive, bring_to_front

OUTPUT = ROOT/'reports/web-bends-probe'
MAX_BENDS = 6


def add(a, b): return tuple(x+y for x, y in zip(a, b))
def sub(a, b): return tuple(x-y for x, y in zip(a, b))
def scale(a, s): return tuple(x*s for x in a)
def norm(a): return math.sqrt(sum(x*x for x in a))
def unit(a): return scale(a, 1/norm(a))
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def segment_distance(p, a, b):
    ab, ap = sub(b, a), sub(p, a)
    length2 = sum(x*x for x in ab)
    t = 0 if length2 < 1e-9 else max(0., min(1., sum(x*y for x, y in zip(ab, ap))/length2))
    return norm(sub(p, add(a, scale(ab, t))))


def web_command(feet, wrist, end, bends=(), held=None, slack=0., attached_at=1):
    """native_webs::ProbeCommand v3: request entry 0 holds one game web from `wrist` to `end`, round `bends`
    (the nearest the hand first); with `held` (a wrist), the other hand holds it too, below."""
    raw = struct.pack('<4I3f4x', 0x5357424d, 3, 288, 200, *feet)
    raw += struct.pack('<2Iq6f', 1, 1, attached_at, *end, *wrist)
    raw += struct.pack('<2Iq6f', 0, 0, 0, *([0.]*6))
    raw += struct.pack('<If3f4x', 1 if held else 0, slack, *(held or (0., 0., 0.)))
    for i in range(2):
        points = list(bends) if i == 0 else []
        raw += struct.pack('<I', len(points))
        for k in range(MAX_BENDS):
            raw += struct.pack('<3f', *(points[k] if k < len(points) else (0., 0., 0.)))
    assert len(raw) == 288
    return raw


def rope(game, manager, end):
    """The hero's swing rope aimed at `end`: its flags, the points the game built and their count."""
    for slot in range(16):
        base = manager+0x50+slot*0x7c8
        raw = game.read(base, 0x7c8)
        if len(raw) != 0x7c8 or not raw[0x70c] & 1 or struct.unpack_from('<I', raw, 0x700)[0] != 2:
            continue
        target = struct.unpack_from('<3f', raw, 0x67c)
        if norm(sub(target, end)) > .5:
            continue
        count = struct.unpack_from('<i', raw, 0x6ac)[0]
        if not 0 < count <= 128:
            continue
        points = [struct.unpack_from('<3f', raw, 0x1c+i*12) for i in range(count)]
        return dict(slot=slot, flags=raw[0x70c], count=count, points=points)
    return None


def assess(built, wrist, end, bends, lower):
    """How the built rope compares with the one wanted: from the wrist (the lower one for a web in both
    hands) round each edge, then the game's own line from the last edge to the end."""
    points = built['points']
    first = lower or wrist
    laid = [p for p in bends]
    # The laid points the rope must pass, in order; the game's line follows the last of them.
    last = laid[-1] if laid else wrist
    at = 0
    edge_errors = []
    for edge in laid:
        while at < len(points) and norm(sub(points[at], edge)) > .05:
            at += 1
        edge_errors.append(None if at == len(points) else round(norm(sub(points[at], edge)), 4))
    after = points[at+1:] if laid and at < len(points) else points[1:]
    off_line = max((segment_distance(p, last, end) for p in after), default=0.)
    # Coming back toward the wrist past the last edge: the fold.
    back = max((norm(sub(last, wrist))-norm(sub(p, wrist)) for p in after), default=0.) if laid else 0.
    return dict(count=built['count'], first_error=round(norm(sub(points[0], first)), 4),
                edge_errors=edge_errors, end_error=round(norm(sub(points[-1], end)), 4),
                off_line_m=round(off_line, 4), back_toward_wrist_m=round(back, 4))


def shot(game, name):
    hwnd = game_window(game.pid)
    width, height = client_size(hwnd)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    path = OUTPUT/f'{name}.png'
    write_rgb_png(path, width, height, window_rgb(hwnd, width, height))
    return str(path)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--modules', type=pathlib.Path, default=ROOT/'build/windows-ninja',
                   help='folder of the spidy_stereo_probe.dll to use')
    p.add_argument('--seconds', type=float, default=1.5, help='seconds per case')
    p.add_argument('--output', type=pathlib.Path, default=ROOT/'reports/web-bends-probe.json')
    a = p.parse_args()
    game = Game(find_game())
    process = None
    webs = None
    started = False
    report = dict(modules=str(a.modules), cases=[])
    try:
        bring_to_front(game.pid)
        components = list(registry(game))
        heroes = [r for _, v, r, _ in components if v == HERO_LOCAL]
        if len(heroes) != 1:
            raise RuntimeError('Exactly one local hero is required: load free roam first')
        record = heroes[0]
        managers = [x for x, v, r, _ in components if v == ROPE_MANAGER and r == record]
        if len(managers) != 1:
            raise RuntimeError(f'Expected one hero rope manager, found {len(managers)}')
        manager = managers[0]
        process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002 | 0x00100000, False, game.pid)
        webs, digest = prepare(game.pid, process, a.modules/'spidy_stereo_probe.dll', ROOT/'reports/stereo-modules',
                               ('SpidyWebsStart', 'SpidyWebsSubmit', 'SpidyWebsStop'))
        report['stereo_sha256'] = digest
        code = call_with_payload(process, webs['SpidyWebsStart'],
                                 struct.pack('<4I2Q', 0x53574243, 1, 32, game.pid, game.base, record))
        if code:
            raise RuntimeError(f'Game webs start: {code}')
        started = True

        def hero():
            return tuple(game.transform(game.pointer(record))['position'])
        origin = hero()
        forward, _ = camera(game)
        flat = unit((forward[0], 0., forward[2])) if forward and math.hypot(forward[0], forward[2]) > .1 \
            else (0., 0., 1.)
        up = (0., 1., 0.)
        side = unit(cross(flat, up))
        wrist = add(add(origin, (0., 1.3, 0.)), scale(flat, .4))
        end = add(add(wrist, scale(flat, 20.)), scale(up, 6.))
        along = sub(end, wrist)

        def on_line(t, off=0., lift=0.):
            return add(add(add(wrist, scale(along, t)), scale(side, off)), scale(up, lift))
        lower = add(add(wrist, (0., -.35, 0.)), scale(side, .1))
        cases = [
            ('straight', [], None),
            ('edge on the line', [on_line(.4)], None),
            ('edge 1 cm off the line', [on_line(.4, .01)], None),
            ('edge 10 cm off the line', [on_line(.4, .1)], None),
            ('edge 1 m off the line', [on_line(.4, 1.)], None),
            ('edge 5 m off the line', [on_line(.4, 5.)], None),
            ('two edges', [on_line(.3, 2.), on_line(.6, 3.)], None),
            ('two edges on the line', [on_line(.3), on_line(.6)], None),
            ('edge at the wrist', [on_line(.002)], None),
            ('edge at the end', [end], None),
            ('six edges', [on_line(.1*(k+1), .5*(k % 2), .3*k) for k in range(6)], None),
            ('both hands round an edge', [on_line(.4, 2.)], lower),
            ('both hands on the line', [on_line(.4)], lower),
            ('edge sweeping off the line', 'sweep', None),
        ]
        print(f'Hero at {[round(x, 1) for x in origin]}; web from {[round(x, 1) for x in wrist]} '
              f'to {[round(x, 1) for x in end]}', flush=True)
        for name, bends, held in cases:
            if not alive(game):
                report['cases'].append(dict(name=name, alive=False))
                break
            began = time.monotonic()
            reads = []
            while time.monotonic()-began < a.seconds:
                t = time.monotonic()-began
                now = [on_line(.4, 3.*min(1., t/a.seconds))] if bends == 'sweep' else bends
                code = call_with_payload(process, webs['SpidyWebsSubmit'],
                                         web_command(hero(), wrist, end, now, held, .15 if held else 0.))
                if code:
                    raise RuntimeError(f'Game web submit ({name}): {code}')
                if t > .3 or bends == 'sweep':
                    built = rope(game, manager, end)
                    if built:
                        reads.append(dict(t=round(t, 3), **assess(built, wrist, end, now, held)))
                time.sleep(.008)
            case = dict(name=name, alive=alive(game), reads=len(reads))
            if reads:
                worst = max(reads, key=lambda r: (r['off_line_m'], r['back_toward_wrist_m']))
                case.update(last=reads[-1], worst=worst)
                built = rope(game, manager, end)
                if built:
                    case['points'] = [[round(x, 3) for x in pt] for pt in built['points'][:12]]
            if case['alive']:
                case['picture'] = shot(game, name.replace(' ', '-'))
            report['cases'].append(case)
            worst = case.get('worst', {})
            print(f"{name:28s} alive={case['alive']} reads={len(reads)} count={worst.get('count')} "
                  f"off_line={worst.get('off_line_m')} back={worst.get('back_toward_wrist_m')} "
                  f"edges={worst.get('edge_errors')} first={worst.get('first_error')} end={worst.get('end_error')}",
                  flush=True)
        report['alive'] = alive(game)
    finally:
        if started and alive(game):
            call_remote(process, webs['SpidyWebsStop'])
        if process:
            close(process)
        a.output.parent.mkdir(parents=True, exist_ok=True)
        a.output.write_text(json.dumps(report, indent=1), encoding='utf-8')
        game.close()
    print(f"Game running at the end: {report.get('alive')}. Report: {a.output}")
    ok = report.get('alive') and all(c.get('alive') and c.get('reads') and c['worst']['off_line_m'] < .05
                                     and c['worst']['back_toward_wrist_m'] < .05 for c in report['cases'])
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

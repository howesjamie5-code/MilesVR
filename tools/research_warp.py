"""On Spidy's wall at the vent: the game's own way onto the wall (game_warp), then the vent's offer, then Y.

Run with the game standing on its crawl at the vent (to_vent.sh, HEIGHT=91). It mounts Spidy's wall off the
crawl the way the player did (RT + A, the swing's wall takes him), climbs back to the vent's height on it,
asks the game for its crawl there and reads who holds him, his height, the hero's offer processor and the
vent's offer; then, as asked: walks on that crawl (--walk), runs up the wall (--rt-climb), presses the game's Y.
"""
import argparse
import json
import struct
import sys
import time

from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from observe_game import call_remote, call_with_payload, modules
from probe_game_grab import HERO_LOCAL, HERO_MOVERS, registry, resolve
from probe_game_swing import snapshot as swing_snapshot
from probe_menu_pad import BUTTONS, XR_DLL, pad, shot
from probe_native_motion import snapshot as motion_snapshot
from probe_wall_crawl import actor, norm, tilt
from probe_wall_run import swing_command
from research_vent_offer import CACHE, PROCESSOR, VENT_OFFER, offer_state, processor_state, scan, walls_setting

XR_EXPORTS = ('SpidyPadSubmit', 'SpidyPadSample', 'SpidyPadRelease', 'SpidyMenuStart', 'SpidyPromptsNote')
RAY_EXPORTS = ('SpidyRayStart', 'SpidyRayStop', 'SpidyRaySubmit', 'SpidyRayData', 'SpidySwingStart',
               'SpidySwingSubmit', 'SpidySwingStop', 'SpidySwingData', 'SpidySwingSettings', 'SpidyShooterStart',
               'SpidyShooterStop', 'SpidyWarpCrawl', 'SpidyWarpSample', 'SpidyWarpData', 'SpidyOffersData',
               'SpidyOffersNudge', 'SpidyOffersUse', 'SpidyOffersAim')


def warp_data(game, address):
    raw = game.read(address, 152)
    if len(raw) < 152:
        return None
    v = struct.unpack_from('<4Iq8Q', raw, 0)
    names = ('asked', 'applied', 'refused', 'faulted', 'noManager', 'frames', 'manager', 'record')
    out = dict(zip(names, v[5:]))
    out['manager'] = hex(out['manager'])
    out['record'] = hex(out['record'])
    out['swing'] = hex(struct.unpack_from('<I', raw, 112)[0])
    out['visits'], out['steps'], out['taken'], out['declined'] = struct.unpack_from('<4Q', raw, 120)
    return out


def offers_data(game, address):
    """game_offers::Data v5: updates run with the crawl's mask, offers checked against the crawl's pose, vents'
    own checks answered for Spidy's wall; every offer the player's gather checked and the one the game picked;
    the aimed offer's spot; the uses asked for and how they ended."""
    import struct as s
    raw = game.read(address, 544)
    if len(raw) < 544 or s.unpack_from('<3I', raw) != (0x534f4644, 5, 544):
        return None
    updates, masked, checks, posed, vent_checks, vent_answered = s.unpack_from('<6Q', raw, 24)
    mask, error, held, forced = s.unpack_from('<4I', raw, 72)
    point = s.unpack_from('<3f', raw, 88)
    count, best = s.unpack_from('<2I', raw, 112)
    nearby = []
    for k in range(min(count, 8)):
        handle, kind, reasons, enabled, *v, reachable = s.unpack_from('<4I6fI', raw, 120 + 44 * k)
        nearby.append(dict(handle=hex(handle), kind=kind, reasons=hex(reasons), enabled=enabled,
                           reachable=reachable, position=[round(x, 2) for x in v[:3]],
                           axis=[round(x, 2) for x in v[3:]]))
    asked, started, taken, declined = s.unpack_from('<4Q', raw, 472)
    last, aimed = s.unpack_from('<2I', raw, 504)
    spot = s.unpack_from('<3f', raw, 512)
    spot_found, = s.unpack_from('<I', raw, 524)
    gathered, most_checked, searches, spots_found = s.unpack_from('<4I', raw, 528)
    return dict(status=s.unpack_from('<I', raw, 12)[0], updates=updates, masked=masked, checks=checks, posed=posed,
                vent_checks=vent_checks, vent_answered=vent_answered, mask=hex(mask), error=error, held=held,
                point=[round(v, 2) for v in point], best=hex(best), nearby=nearby, aimed=hex(aimed),
                spot=[round(v, 2) for v in spot] if spot_found else None, gathered=gathered,
                most_checked=most_checked, searches=searches, spots_found=spots_found,
                uses=dict(asked=asked, started=started, taken=taken, declined=declined, last=hex(last)))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--gravity', type=float, default=7.848)
    p.add_argument('--manual', action='store_true', help='ask the crawl from the probe (else the swing does)')
    p.add_argument('--no-mount', action='store_true', help='start from where he is (the crawl): no RT + A')
    p.add_argument('--rt-climb', type=float, default=0, help="seconds of RT + stick up on the crawl (its wall run)")
    p.add_argument('--walk', type=float, default=0, help='seconds of stick on the crawl after the warp')
    p.add_argument('--no-press', action='store_true')
    p.add_argument('--climb', type=float, default=91.2, help='height to climb to on Spidy\'s wall first')
    p.add_argument('--nudge', type=float, default=0,
                   help="metres up the wall the offers' crawl pose is nudged (a probe cannot walk Spidy's wall)")
    p.add_argument('--use', action='store_true',
                   help="as the headset's interact on a pointed offer: ask for its use, wait for the game's crawl "
                        "and its pick, press the game's Y")
    p.add_argument('--walk-in', type=float, default=0,
                   help="seconds of the game's stick (--walk-stick) once its crawl has him, before Y")
    p.add_argument('--walk-stick', default='0,1', help='the stick for --walk-in, x,y')
    p.add_argument('--press-wait', type=float, default=0, help='seconds on the crawl before Y')
    p.add_argument('--output', default=str(ROOT / 'reports/research-warp.json'))
    a = p.parse_args()
    game = Game(find_game())
    try:
        cached = json.load(open(CACHE))
    except Exception:
        cached = {}
    if cached.get('pid') != game.pid:
        t = time.monotonic()
        found, total = scan(game, (VENT_OFFER, PROCESSOR))
        cached = dict(pid=game.pid, vents=found[VENT_OFFER], processors=found[PROCESSOR])
        json.dump(cached, open(CACHE, 'w'))
        print(f"scanned {total / 2**30:.2f} GiB in {time.monotonic() - t:.1f} s", flush=True)
    vents, processors = cached['vents'], cached['processors']
    components = list(registry(game))
    record = [r for at, v, r, _ in components if v == HERO_LOCAL][0]
    managers = [at for at, v, r, _ in components if v == HERO_MOVERS and r == record]
    mover = resolve(game, struct.unpack('<I', game.read(managers[0] + 0xdb4, 4))[0])
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    report = dict(samples=[], phases={})
    swing_started = rays_started = shooter_started = False
    xr = rays = None
    try:
        xr, _ = prepare(game.pid, process, XR_DLL, ROOT / 'reports/stereo-modules', XR_EXPORTS)
        motion, _ = prepare(game.pid, process, ROOT / 'build/windows-ninja/spidy_movement_bridge.dll',
                            ROOT / 'reports/motion-modules', ('SpidyMotionStart', 'SpidyMotionStop', 'SpidyMotionData'))
        motion_module = next(m['base'] for m in modules(game.pid) if m['name'].lower() == 'spidy_movement_bridge.dll')
        rays, _ = prepare(game.pid, process, ROOT / 'build/windows-ninja/spidy_ray_bridge.dll',
                          ROOT / 'reports/ray-modules', RAY_EXPORTS)
        call_remote(process, rays['SpidySwingStop'])
        call_remote(process, rays['SpidyRayStop'])
        code = call_with_payload(process, rays['SpidyRayStart'],
                                 struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410))
        if code:
            raise RuntimeError(f'rays: {code}')
        rays_started = True
        code = call_with_payload(process, rays['SpidySwingStart'],
                                 struct.pack('<4I4QI2fI', 0x53574346, 1, 64, game.pid, game.base, record, mover,
                                             motion_module, 30000, 32., a.gravity, 0))
        if code:
            raise RuntimeError(f'swing: {code}')
        swing_started = True
        call_with_payload(process, rays['SpidySwingSettings'], walls_setting(1, a.gravity))
        code = call_with_payload(process, rays['SpidyShooterStart'],
                                 struct.pack('<4IQ', 0x53484f43, 1, 24, game.pid, game.base))
        shooter_started = not code
        print(f"shooter start: {code}", flush=True)
        body = actor(game, record)
        at_the_wall = norm(body['forward'])
        serial = [0]
        begun = time.monotonic()
        last = [None]

        def step(phase, move=(0., 0., 0.)):
            serial[0] += 1
            here = actor(game, record)
            origin = here['feet'] if here else body['feet']
            call_with_payload(process, rays['SpidySwingSubmit'], swing_command(serial[0], origin, at_the_wall,
                                                                               move=move))
            s = swing_snapshot(game, rays['SpidySwingData']) or {}
            m = motion_snapshot(game, motion['SpidyMotionData']) or {}
            q = processor_state(game, processors[0]) if processors else None
            near = [offer_state(game, v) for v in vents]
            near = [o for o in near if o and (o['reasons'] != '0x0' or (q and o['handle'] in
                                                                         [x['handle'] for x in q['slots']]))]
            entry = dict(t=round(time.monotonic() - begun, 3), phase=phase, wall=s.get('wall'), owned=s.get('owned'),
                         contact=s.get('native_contact'), distance=round(s.get('wall_distance') or 0, 2),
                         normal=[round(v, 2) for v in (s.get('wall_normal') or (0, 0, 0))],
                         mover=hex(m.get('mover_flags') or 0),
                         feet=[round(v, 2) for v in here['feet']] if here else None,
                         tilt=round(tilt(here['up']), 1) if here else None, processor=q, vents=near,
                         warp=warp_data(game, rays['SpidyWarpData']), offers=offers_data(game, rays['SpidyOffersData']))
            report['samples'].append(entry)
            key = json.dumps({k: entry[k] for k in ('phase', 'wall', 'owned', 'contact', 'mover')}) + json.dumps(
                {k: v for k, v in (q or {}).items() if k != 'cooldown'}) + json.dumps(
                [{k: v for k, v in o.items() if k != 'frame'} for o in near])
            if key != last[0]:
                last[0] = key
                print(json.dumps(entry), flush=True)
            return entry

        def phase(name):
            report['phases'][name] = round(time.monotonic() - begun, 3)
            print(f"== {report['phases'][name]:6.2f} s  {name}", flush=True)
            # Beside the game's own prompt calls, where a probe has the prompt reading on (probe_vent watch).
            try:
                call_with_payload(process, xr['SpidyPromptsNote'], ('warp: ' + name).encode()[:95].ljust(96, b'\0'))
            except Exception:
                pass

        phase('crawl at the vent')
        for _ in range(60):
            step('crawl at the vent')
            time.sleep(.011)
        if a.no_mount:
            report['before_warp'] = e = step('as found')
        phase('mount')
        leapt = 0.
        until = time.monotonic() + (0 if a.no_mount else 8)
        while time.monotonic() < until:
            # The walk on that mounts Spidy's wall off the game's crawl (Swing::mounting needs the stick
            # since October 15), then the game's RT + A.
            e = step('mount', move=(0., .999, 0.))
            if e.get('wall') == 1 and e.get('owned'):
                break
            if e.get('wall') == 2 and time.monotonic() - leapt > 2.:
                state = struct.pack('<H2B4h', BUTTONS['a'], 0, 255, 0, 0, 0, 0)
                call_with_payload(process, xr['SpidyPadSubmit'], state + struct.pack('<I', 150))
                leapt = time.monotonic()
            time.sleep(.011)
        phase('spidy wall: settle')
        for _ in range(0 if a.no_mount else 60):
            e = step('spidy wall')
            time.sleep(.011)
        phase(f'spidy wall: climb to {a.climb}')
        until = time.monotonic() + 1.5
        while time.monotonic() < until and (e.get('feet') or [0, 0, 0])[1] < a.climb and e.get('wall') == 1:
            n = e.get('normal') or [0, 0, 0]
            size = sum(x * x for x in n) ** .5 or 1
            e = step('climb', move=(-n[0] / size * .999, -n[1] / size * .999, -n[2] / size * .999))
            time.sleep(.011)
        phase('spidy wall: still')
        for _ in range(0 if a.no_mount else 90):
            e = step('spidy wall still')
            time.sleep(.011)
        if not a.no_mount:
            report['before_warp'] = e
        if a.nudge:
            print('nudge', call_with_payload(process, rays['SpidyOffersNudge'], struct.pack('<3f', 0, a.nudge, 0)),
                  flush=True)
            phase(f'nudged {a.nudge} m')
            for _ in range(90):
                e = step('nudged')
                time.sleep(.011)
        if a.use:
            # offer_pointing::Use, as the headset runs it: an offer the game would let him use (reasons 0, on),
            # its use asked for; once the game's crawl has him, up to half a second for the game to pick it, and
            # its Y for 0.35 s.
            o = offers_data(game, rays['SpidyOffersData']) or {}
            report['offers_before_use'] = o
            print('offers', json.dumps(o), flush=True)
            # As a hand pointing at it does: the aim, and the wall round it tried for a spot within reach.
            near = [x for x in o.get('nearby', []) if x['enabled'] and x['reasons'] in ('0x0', '0x4')]
            usable = []
            if near:
                aim = int(near[0]['handle'], 16)
                print('aim', call_with_payload(process, rays['SpidyOffersAim'], struct.pack('<I', aim)), flush=True)
                for _ in range(60):
                    e = step('aimed')
                    o = offers_data(game, rays['SpidyOffersData']) or {}
                    usable = [x for x in o.get('nearby', []) if x['handle'] == hex(aim) and x['enabled'] and
                              (x['reasons'] == '0x0' or x['reachable'])]
                    if usable:
                        break
                    time.sleep(.011)
                report['offers_after_aim'] = o
                print('after the aim', json.dumps({k: o.get(k) for k in ('nearby', 'aimed', 'spot', 'searches',
                                                                        'spots_found', 'point')}), flush=True)
            if not usable:
                print('nothing usable to point at', flush=True)
            else:
                handle = int(usable[0]['handle'], 16)
                taken0, declined0 = o['uses']['taken'], o['uses']['declined']
                phase(f"use {usable[0]['handle']}")
                code = call_with_payload(process, rays['SpidyOffersUse'], struct.pack('<I', handle))
                report['use_code'] = code
                print('use asked:', code, flush=True)
                outcome, settled, until = None, None, time.monotonic() + 2.5
                while not code and time.monotonic() < until:
                    e = step('use asked')
                    u = (offers_data(game, rays['SpidyOffersData']) or {}).get('uses') or {}
                    if u.get('taken', 0) != taken0:
                        outcome = 'taken'
                        break
                    if u.get('declined', 0) != declined0:
                        outcome = 'declined'
                        break
                    time.sleep(.011)
                report['use_outcome'] = outcome
                phase(f'use: {outcome}')
                if outcome == 'taken':
                    until = time.monotonic() + .5
                    while time.monotonic() < until:
                        e = step('use settling')
                        if (offers_data(game, rays['SpidyOffersData']) or {}).get('best') == hex(handle):
                            settled = round(.5 - (until - time.monotonic()), 3)
                            break
                        time.sleep(.011)
                    report['use_picked_after'] = settled
                    if a.walk_in:
                        sx, sy = (float(v) for v in a.walk_stick.split(','))
                        phase(f'use: walk in {a.walk_in} s, stick {sx:+.1f},{sy:+.1f}')
                        until = time.monotonic() + a.walk_in
                        while time.monotonic() < until:
                            state = struct.pack('<H2B4h', 0, 0, 0, int(sx * 32767), int(sy * 32767), 0, 0)
                            call_with_payload(process, xr['SpidyPadSubmit'], state + struct.pack('<I', 120))
                            e = step('use walking in')
                            time.sleep(.011)
                    until = time.monotonic() + a.press_wait
                    while time.monotonic() < until:
                        e = step('use waiting')
                        time.sleep(.011)
                    phase(f'use: press y (picked after {settled})')
                    pad(process, xr, BUTTONS['y'], hold_ms=350)
                    for _ in range(400):
                        e = step('use pressed')
                        time.sleep(.011)
                    report['after_use'] = e
                    a.no_press = True
        if a.manual:
            here = actor(game, record)
            normal = e.get('normal') or [0, 0, 0]
            # The wall's own face under him, as the game's callers pass it: the body is held distance off it.
            d = e.get('distance') or 0
            point = [here['feet'][i] - normal[i] * d for i in range(3)]
            report['crawl_point'] = point
            payload = struct.pack('<4IQ6f', 0x53575043, 2, 48, 0, record, *point, *normal)
            phase('crawl asked')
            t0 = time.monotonic()
            code = call_with_payload(process, rays['SpidyWarpCrawl'], payload)
            report['warp_code'] = code
            print(f"crawl asked: {code} in {time.monotonic() - t0:.3f} s; {warp_data(game, rays['SpidyWarpData'])}",
                  flush=True)
        phase('after the crawl was asked')
        for _ in range(220):
            e = step('after crawl asked')
            time.sleep(.011)
        report['after_warp'] = e
        print('warp data', warp_data(game, rays['SpidyWarpData']), flush=True)
        if a.rt_climb:
            # As the headset does on the game's crawl with the stick up the wall: the game's RT with its stick
            # (game_xr's climbing), and the swing's own stick up the wall.
            n = next((s['normal'] for s in reversed(report['samples']) if any(s.get('normal') or [])),
                     [-0.7071, 0., -0.7071])
            phase(f'rt climb {a.rt_climb} s')
            until = time.monotonic() + a.rt_climb
            while time.monotonic() < until:
                state = struct.pack('<H2B4h', 0, 0, 255, 0, 32767, 0, 0)
                call_with_payload(process, xr['SpidyPadSubmit'], state + struct.pack('<I', 120))
                e = step('rt climbing', move=(0., .999, 0.))
                time.sleep(.011)
            phase('let go of the run')
            for _ in range(220):
                e = step('after the run')
                time.sleep(.011)
            print('warp data', warp_data(game, rays['SpidyWarpData']), flush=True)
        if a.walk:
            phase(f'walk {a.walk} s on the crawl')
            until = time.monotonic() + a.walk
            while time.monotonic() < until:
                state = struct.pack('<H2B4h', 0, 0, 0, 0, 32767, 0, 0)
                call_with_payload(process, xr['SpidyPadSubmit'], state + struct.pack('<I', 120))
                e = step('walking on the crawl')
                time.sleep(.011)
            for _ in range(60):
                e = step('after the walk')
                time.sleep(.011)
        try:
            path, mean, front = shot(game, 'warp-at-vent')
            report['shot'] = str(path)
        except Exception as ex:
            print(f"no screenshot: {ex}", flush=True)
        if not a.no_press:
            phase('press y')
            pad(process, xr, BUTTONS['y'], hold_ms=700)
            for _ in range(300):
                e = step('pressed y')
                time.sleep(.011)
            report['after_press'] = e
        print('final', json.dumps({k: e.get(k) for k in ('wall', 'owned', 'contact', 'mover', 'feet', 'tilt')}),
              warp_data(game, rays['SpidyWarpData']), flush=True)
    finally:
        if swing_started:
            call_remote(process, rays['SpidySwingStop'])
        if shooter_started:
            call_remote(process, rays['SpidyShooterStop'])
        if rays_started:
            call_remote(process, rays['SpidyRayStop'])
        if xr:
            call_remote(process, xr['SpidyPadRelease'])
        with open(a.output, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
        close(process)
        game.close()


if __name__ == '__main__':
    sys.exit(main())

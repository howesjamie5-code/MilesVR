"""Why the vent's interact is not offered on Spidy's wall: read the game's own reasons.

Offer::IsAvailableTo (0x1e6e400) ORs a reason into the offer at +0xf4/+0xf8 (two frames, +0xfc picks the
current one) for the first check that fails: 1 disabled (+0xb5), 2 vf148, 4 vf0e8 (range), 8 vf158,
0x10 vf150, 0x20 the owner's component vfa8, 0x40 vf128 (VentOffer: its entry conditions).
Hero::HeroOfferProcessor (vtable 0x38ab038): +0x1c8 allowed kinds, +0x1cc cooldown, 8 candidate slots of 0x18
from +0x48 (+0x10 can-accept byte, +0x11 has-locator byte, +0x14 offer handle), +0x1ac best offer handle.

Run with the game standing on its crawl at the vent (to_vent.sh, HEIGHT=91).
"""
import argparse
import pathlib
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
from probe_wall_crawl import actor, norm, tilt
from probe_wall_run import swing_command

XR_EXPORTS = ('SpidyPadSubmit', 'SpidyPadSample', 'SpidyPadRelease', 'SpidyMenuStart')
RAY_EXPORTS = ('SpidyRayStart', 'SpidyRayStop', 'SpidyRaySubmit', 'SpidyRayData', 'SpidySwingStart',
               'SpidySwingSubmit', 'SpidySwingStop', 'SpidySwingData', 'SpidySwingSettings')
VENT_OFFER = 0x3885a70
PROCESSOR = 0x38ab038
CACHE = str(pathlib.Path(__file__).resolve().parent.parent / 'reports/offer_instances.json')


def scan(game, needles):
    found = {n: [] for n in needles}
    packed = {n: struct.pack('<Q', game.base + n) for n in needles}
    total = 0
    for base, size in game.regions():
        offset = 0
        while offset < size:
            chunk = min(size - offset, 1 << 24)
            data = game.read(base + offset, chunk)
            total += len(data)
            for n, p in packed.items():
                i = data.find(p)
                while i >= 0:
                    if i % 8 == 0:
                        found[n].append(base + offset + i)
                    i = data.find(p, i + 1)
            offset += chunk
    return found, total


def walls_setting(walls, gravity):
    return struct.pack('<4IfIfI', 0x53575354, 4, 32, 0, 32., 1, gravity, int(walls))


def offer_state(game, address):
    raw = game.read(address, 0x100)
    if len(raw) < 0x100:
        return None
    frame, r0, r1, current = struct.unpack_from('<3IB', raw, 0xf0)
    return dict(enabled=raw[0xb5], kind=hex(struct.unpack_from('<I', raw, 0xa8)[0]),
                handle=hex(struct.unpack_from('<I', raw, 0x14)[0]), frame=frame,
                reasons=hex((r0, r1)[current]), previous=hex((r1, r0)[current]), d4=raw[0xd4], d6=raw[0xd6],
                d8=raw[0xd8])


def processor_state(game, address):
    raw = game.read(address, 0x2e0)
    if len(raw) < 0x2e0:
        return None
    slots = []
    for i in range(8):
        b = 0x48 + i * 0x18
        handle = struct.unpack_from('<I', raw, b + 0x14)[0]
        if handle:
            slots.append(dict(handle=hex(handle), accept=raw[b + 0x10], locator=raw[b + 0x11]))
    return dict(mask=hex(struct.unpack_from('<I', raw, 0x1c8)[0]),
                cooldown=round(struct.unpack_from('<f', raw, 0x1cc)[0], 3),
                best=hex(struct.unpack_from('<I', raw, 0x1ac)[0]), slots=slots,
                owner=hex(struct.unpack_from('<Q', raw, 8)[0]))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--rescan', action='store_true')
    p.add_argument('--gravity', type=float, default=7.848)
    p.add_argument('--no-mount', action='store_true')
    p.add_argument('--output', default=str(ROOT / 'reports/research-vent-offer.json'))
    a = p.parse_args()
    game = Game(find_game())
    try:
        cached = json.load(open(CACHE))
    except Exception:
        cached = {}
    if a.rescan or cached.get('pid') != game.pid:
        t = time.monotonic()
        found, total = scan(game, (VENT_OFFER, PROCESSOR))
        cached = dict(pid=game.pid, vents=found[VENT_OFFER], processors=found[PROCESSOR])
        json.dump(cached, open(CACHE, 'w'))
        print(f"scanned {total / 2**30:.2f} GiB in {time.monotonic() - t:.1f} s: {len(cached['vents'])} vent offers, "
              f"{len(cached['processors'])} processors", flush=True)
    vents, processors = cached['vents'], cached['processors']
    for v in vents:
        print(f"vent offer {v:#x}: {offer_state(game, v)}")
    for q in processors:
        print(f"processor {q:#x}: {processor_state(game, q)}")

    components = list(registry(game))
    heroes = [(at, r) for at, v, r, _ in components if v == HERO_LOCAL]
    record = heroes[0][1]
    managers = [at for at, v, r, _ in components if v == HERO_MOVERS and r == record]
    mover = resolve(game, struct.unpack('<I', game.read(managers[0] + 0xdb4, 4))[0])
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    report = dict(samples=[], vents=[hex(v) for v in vents], processors=[hex(q) for q in processors])
    swing_started = rays_started = False
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
        body = actor(game, record)
        at_the_wall = norm(body['forward'])
        serial = [0]
        begun = time.monotonic()
        last = [None]

        def step(phase):
            serial[0] += 1
            here = actor(game, record)
            origin = here['feet'] if here else body['feet']
            call_with_payload(process, rays['SpidySwingSubmit'], swing_command(serial[0], origin, at_the_wall,
                                                                               move=(0., 0., 0.)))
            s = swing_snapshot(game, rays['SpidySwingData']) or {}
            m = motion_snapshot(game, motion['SpidyMotionData']) or {}
            entry = dict(t=round(time.monotonic() - begun, 3), phase=phase, wall=s.get('wall'), owned=s.get('owned'),
                         contact=s.get('native_contact'), distance=round(s.get('wall_distance') or 0, 2),
                         mover=hex(m.get('mover_flags') or 0), y=round(here['feet'][1], 2) if here else None,
                         tilt=round(tilt(here['up']), 1) if here else None,
                         vents={hex(v): offer_state(game, v) for v in vents},
                         processors={hex(q): processor_state(game, q) for q in processors})
            report['samples'].append(entry)
            key = json.dumps({k: entry[k] for k in ('phase', 'wall', 'owned', 'contact', 'mover')}) + json.dumps(
                {k: {kk: vv for kk, vv in (v or {}).items() if kk not in ('frame',)}
                 for k, v in entry['vents'].items()}) + json.dumps(
                {k: {kk: vv for kk, vv in (v or {}).items() if kk != 'cooldown'}
                 for k, v in entry['processors'].items()})
            if key != last[0]:
                last[0] = key
                print(json.dumps(entry), flush=True)
            return entry

        for _ in range(120):
            step('crawl at the vent')
            time.sleep(.011)
        if not a.no_mount:
            leapt = 0.
            until = time.monotonic() + 8
            while time.monotonic() < until:
                e = step('mount')
                if e.get('wall') == 1 and e.get('owned'):
                    break
                if e.get('wall') == 2 and time.monotonic() - leapt > 2.:
                    state = struct.pack('<H2B4h', BUTTONS['a'], 0, 255, 0, 0, 0, 0)
                    call_with_payload(process, xr['SpidyPadSubmit'], state + struct.pack('<I', 150))
                    leapt = time.monotonic()
                time.sleep(.011)
            for _ in range(250):
                step('spidy wall')
                time.sleep(.011)
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


if __name__ == '__main__':
    sys.exit(main())

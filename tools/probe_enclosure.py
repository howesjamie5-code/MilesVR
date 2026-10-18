"""What the room around the player looks like to the indoors rule (spidy::Enclosure).

The rule that keeps the game's own walls indoors reads five world rays - one up and four along the ground -
and counts how many ways the player is closed in. Two of three is indoors. This casts exactly those rays and
prints what they find, so the thresholds can be checked in the places that matter rather than guessed:

    python tools/probe_enclosure.py --seconds 20       # with the game loaded and the player where you want

A vent, a duct or a lift shaft should come out closed in; a street, a rooftop or a building face should not.
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
from probe_native_rays import command as ray_command, snapshot as ray_snapshot
from probe_wall_crawl import actor

RAY_EXPORTS = ('SpidyRayStart', 'SpidyRayStop', 'SpidyRaySubmit', 'SpidyRayData')
# spidy::Enclosure
NEARBY, WAYS = 4., 2
SIDES = ('up', 'north', 'south', 'east', 'west')
DIRECTIONS = ((0, 1, 0), (0, 0, -1), (0, 0, 1), (1, 0, 0), (-1, 0, 0))


def closed_ways(room):
    """How many of the three ways the room closes the player in (Enclosure::closedWays)."""
    return ((room['up'] < NEARBY) + (room['north'] < NEARBY and room['south'] < NEARBY) +
            (room['east'] < NEARBY and room['west'] < NEARBY))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--seconds', type=float, default=20)
    p.add_argument('--every', type=float, default=1.)
    p.add_argument('--output', default=str(ROOT/'reports/enclosure.json'))
    a = p.parse_args()

    game = Game(find_game())
    heroes = [r for at, v, r, _ in registry(game) if v == HERO_LOCAL]
    if len(heroes) != 1:
        raise RuntimeError('Exactly one local hero is required: load the game first')
    record = heroes[0]
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    if not process:
        raise c.WinError(c.get_last_error())
    report = dict(readings=[])
    started = False
    rays = None
    try:
        rays, _ = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                          ROOT/'reports/ray-modules', RAY_EXPORTS)
        code = call_with_payload(process, rays['SpidyRayStart'],
                                 struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410))
        if code:
            raise RuntimeError(f'World rays start: {code}')
        started = True
        serial = 0
        until = time.monotonic()+a.seconds
        while time.monotonic() < until:
            body = actor(game, record)
            if not body:
                time.sleep(.1)
                continue
            # From the head, as the session does: on a wall the feet are on the surface and rays from
            # there start inside the building, which reads every way as closed.
            up = body['up']
            at = tuple(body['feet'][i] + up[i]*1.5 for i in range(3))
            serial += 1
            batch = [(at, d, NEARBY+1., 2+i) for i, d in enumerate(DIRECTIONS)]
            call_with_payload(process, rays['SpidyRaySubmit'], ray_command(serial, batch, 250))
            deadline = time.monotonic()+2
            result = None
            while time.monotonic() < deadline:
                got = ray_snapshot(game, rays['SpidyRayData'])
                if got and got['serial'] == serial and got['status'] == 2:
                    result = got
                    break
                time.sleep(.005)
            if not result:
                print('the rays did not answer', flush=True)
                continue
            room = {name: NEARBY+1. for name in SIDES}
            for hit in result['hits']:
                tag = hit['ray']['tag'] if isinstance(hit.get('ray'), dict) else hit.get('tag')
                if tag is None or not 2 <= tag <= 6:
                    continue
                reach = NEARBY+1.
                # Only what the world is built of: a car or a passer-by is not a room.
                fixed = (hit['count'] and hit['body_matches'] and (hit['body_flags'] & 3) == 1 and
                         hit['motion_id'] == 0 and hit['broad_phase_id'] != 0xffffffff)
                room[SIDES[tag-2]] = round(hit['fraction']*reach, 2) if fixed else reach
            ways = closed_ways(room)
            entry = dict(feet=[round(v, 1) for v in body['feet']], room=room, ways=ways, indoors=ways >= WAYS)
            report['readings'].append(entry)
            print(f"{'INDOORS ' if entry['indoors'] else 'outside '} ways {ways}  " +
                  '  '.join(f"{k} {room[k]:.2f}" for k in SIDES), flush=True)
            time.sleep(a.every)
        indoors = [r for r in report['readings'] if r['indoors']]
        print(f"\n{len(indoors)} of {len(report['readings'])} readings were indoors", flush=True)
    finally:
        if started:
            call_remote(process, rays['SpidyRayStop'])
        with open(a.output, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
        close(process)
        game.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())

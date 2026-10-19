"""Spider-Sense in the running game without a headset: the game's own warnings of the attacks on the player, as Spidy
reads them from the hero's HeroDodgeManager (src/game_sense.cpp), and its counter (src/game_punch.cpp).

    python tools/probe_sense.py [--seconds 30] [--provoke] [--counter] [--range 40]

Loads Spidy's ray and movement modules, which start once per game process (so a freshly started game, in free roam,
the game window in front: the probe brings it there), starts the swing and the punches, and holds the player's hand
at rest so the punch module reads the warnings at every input sample. Every tenth of a second it records
SpidySenseData:

- whether the hero's manager was found (`managers`, and `read` of `samples`) and how the game's clock ran against
  real time,
- every warning that went off: its attacker, what it asked for (counter, dodge, jump), its flags, how long it gave
  the player (the census) and how long it stood,
- with --provoke, first a scripted fist through the chest of the nearest enemy within --range (as
  tools/probe_combat.py's punch does), so that a fight starts and his crew attacks the standing player,
- with --counter, a scripted fist thrown from the guard at 6 m/s at the attacker of each warning that asks for a
  counter: the counters Spidy counted, the breaks it sent, the attacker's states after it, the strike it opened, and
  the player's health across the warning's blow (HeroHealth's hit points).

Writes reports/sense-probe.json. Copy the save folder to reports/backups first: a fight changes the autosave.
"""
import argparse
import ctypes as c
import json
import math
import pathlib
import struct
import time
from bridge_game import ROOT, prepare
from capture_game_state import Game, close, find_game, open_process
from inspect_game import PE
from observe_game import call_remote, call_with_payload, modules
from probe_combat import ENTRIES, FIST, World, add, describe, height, norm, scale, sub
from probe_game_grab import HERO_MOVERS, hand_command, registry, resolve
from probe_game_screen import user32
from run_game_vr import punch_snapshot, sense_snapshot, strike_snapshot
from vr_launcher import bring_to_front


def hero_health(game, world, hero):
    """The hero's hit points (HeroHealth, derived from Health: a float at +0x88), None when it cannot be read."""
    for address, vtable, record, _ in registry(game):
        if record == hero and 'Health' in world.lineage(vtable):
            return round(struct.unpack('<f', game.read(address+0x88, 4))[0], 2)
    return None


def main():
    try:
        user32.SetProcessDpiAwarenessContext(c.c_void_p(-4))
    except AttributeError:
        user32.SetProcessDPIAware()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--seconds', type=float, default=30, help='how long to watch the warnings')
    parser.add_argument('--provoke', action='store_true', help='punch the nearest enemy first to start a fight')
    parser.add_argument('--counter', action='store_true', help="throw a fist at each warning's attacker")
    parser.add_argument('--range', type=float, default=40)
    parser.add_argument('--output', type=pathlib.Path, default=ROOT/'reports/sense-probe.json')
    args = parser.parse_args()
    game = Game(find_game())
    pe = PE(game.path.read_bytes())
    world = World(game, pe)
    seen = world.scan()
    feet = seen['feet']
    if not seen['hero'] or not feet:
        raise SystemExit('No player')
    if any(game.read(game.base+rva, 16) != pe.bytes(rva, 16) for rva in ENTRIES):
        raise SystemExit('A hook entry is already patched: start the game afresh')
    enemies = [b for b in seen['bots'] if b['enemy'] and b['distance'] is not None and b['distance'] <= args.range]
    report = dict(args=vars(args) | dict(output=str(args.output)), hero=dict(record=hex(seen['hero']), feet=feet),
                  enemies=[describe(b) for b in enemies], front=bring_to_front(game.pid))
    print('enemies within range:', len(enemies), flush=True)
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    started = []
    rows, warnings, counters = [], {}, []
    try:
        managers = [a for a, v, r, _ in registry(game) if v == HERO_MOVERS and r == seen['hero']]
        mover = resolve(game, struct.unpack('<I', game.read(managers[0]+0xdb4, 4))[0])
        _, report['motion_hash'] = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_movement_bridge.dll',
                                           ROOT/'reports/motion-modules',
                                           ('SpidyMotionStart', 'SpidyMotionStop', 'SpidyMotionDrive'))
        motion_module = next(m['base'] for m in modules(game.pid) if m['name'].lower() == 'spidy_movement_bridge.dll')
        rays, report['ray_hash'] = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                                           ROOT/'reports/ray-modules',
                                           ('SpidyRayStart', 'SpidyRayStop', 'SpidySwingStart', 'SpidySwingSubmit',
                                            'SpidySwingStop', 'SpidyPunchStart', 'SpidyPunchStop', 'SpidyPunchData',
                                            'SpidyStrikeData', 'SpidySenseData'))

        def invoke(name, payload, stop=None):
            code = call_with_payload(process, rays[name], payload)
            if code:
                raise RuntimeError(f'{name}: {code}')
            if stop:
                started.insert(0, stop)
        invoke('SpidyRayStart', struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410),
               'SpidyRayStop')
        invoke('SpidySwingStart', struct.pack('<4I4QI2fI', 0x53574346, 1, 64, game.pid, game.base, seen['hero'],
                                              mover, motion_module, 30000, 32., 6., 1 << 2), 'SpidySwingStop')
        invoke('SpidyPunchStart', struct.pack('<4IQ', 0x53505543, 1, 24, game.pid, game.base), 'SpidyPunchStop')
        serial = [time.perf_counter_ns() // 1000000]
        began = time.monotonic()

        def player():
            return world.position(seen['hero']) or feet

        def submit(position, direction, relative=(0, 0, 0)):
            serial[0] += 1
            invoke('SpidySwingSubmit', hand_command(serial[0], position, direction, tuple(relative)))

        # The guard: a quarter of a metre ahead of the head and 0.3 m under it, where a fist rests.
        guard = (.0, -.3, -.25)

        def rest():
            here = player()
            head = [here[0], here[1]+1.65, here[2]]
            submit(add(head, guard), (0, 0, -1), guard)

        def watch(sense):
            """One row of what the module says now, and each warning's first and last sight."""
            t = round(time.monotonic()-began, 3)
            rows.append(dict(t=t, clock=sense['clock'], read=sense['read'], samples=sense['samples'],
                             attacks=sense['attacks'], standing=[(s['attacker'], s['answer'], s['to_impact'])
                                                                 for s in sense['standing']]))
            for s in sense['standing']:
                key = s['attacker']
                w = warnings.setdefault(key, dict(attacker=key, first=t, flags=s['flags'], answer=s['answer'],
                                                  head=s['head'], first_to_impact=s['to_impact']))
                w.update(last=t, last_to_impact=s['to_impact'], countered=s['countered'], state=s['state'])

        rest()
        time.sleep(.4)
        if args.provoke and enemies:
            # The nearest enemy's chest, the fist driven through it at 6 m/s (the punch module does not ask where
            # the player is): he is hit, and the fight is on.
            bot = enemies[0]
            at = world.state(bot)['position'] or bot['position']
            chest = [at[0], at[1]+height(bot, FIST), at[2]]
            here = player()
            direction = norm(sub(chest, [here[0], here[1]+1.4, here[2]]))
            begin, end = sub(chest, scale(direction, .9)), add(chest, scale(direction, .1))
            before = punch_snapshot(game, rays['SpidyPunchData'])
            for k in [0]*30+list(range(1, 14))+[13]*30:
                position = add(begin, scale(sub(end, begin), k/13))
                submit(position, direction, add([.25, -.35, -.3], sub(position, begin)))
                time.sleep(1/90)
            after = punch_snapshot(game, rays['SpidyPunchData'])
            report['provoked'] = dict(bot=describe(bot), punches=after['punches']-before['punches'],
                                      target=after['hands'][0]['last_target'], states=world.state(bot))
            print('provoked', report['provoked'], flush=True)
        health = [dict(t=0., hp=hero_health(game, world, seen['hero']))]
        polled = 0.
        while time.monotonic()-began < args.seconds:
            rest()
            if time.monotonic()-polled >= .1:
                polled = time.monotonic()
                sense = sense_snapshot(game, rays['SpidySenseData'])
                if sense and 'protocol_mismatch' not in sense:
                    watch(sense)
                    wanted = [s for s in sense['standing'] if s['answer'] == 'counter' and not s['countered'] and
                              (s['to_impact'] is None or s['to_impact'] > .12)]
                    if args.counter and wanted:
                        target = wanted[0]
                        record = int(target['attacker'], 16)
                        bot = next((b for b in seen['bots'] if b['record'] == record), None)
                        hp_before = hero_health(game, world, seen['hero'])
                        fight_before = strike_snapshot(game, rays['SpidyStrikeData']) or {}
                        here = player()
                        head = [here[0], here[1]+1.65, here[2]]
                        middle = sub(target['head'], [0, .75, 0])
                        way = norm(sub(middle, add(head, guard)))
                        thrown = []
                        # Thrown from the guard at 6 m/s for 0.09 s, held out for a second (a strike's flight goes on
                        # with the hand where it is), then back to rest.
                        for frame in range(8+90):
                            out = 6/90*min(frame+1, 8)
                            now = player()
                            relative = add(guard, scale(way, out))
                            submit(add([now[0], now[1]+1.65, now[2]], relative), way, relative)
                            if frame % 9 == 0:
                                s = sense_snapshot(game, rays['SpidySenseData'])
                                thrown.append(dict(t=round(time.monotonic()-began, 3),
                                                   states=world.state(bot)['states'] if bot else None,
                                                   thrown=s['thrown'], breaks=s['breaks'],
                                                   counter_blows=s['counter_blows'], player=now))
                                watch(s)
                            time.sleep(1/90)
                        fight_after = strike_snapshot(game, rays['SpidyStrikeData']) or {}
                        counters.append(dict(attacker=target['attacker'], answer=target['answer'],
                                             to_impact=target['to_impact'], flags=target['flags'],
                                             hp_before=hp_before, hp_after=hero_health(game, world, seen['hero']),
                                             strikes_opened=fight_after.get('opened', 0)-fight_before.get('opened', 0),
                                             blows=fight_after.get('blows', 0)-fight_before.get('blows', 0),
                                             last_kind=fight_after.get('last_kind'), rows=thrown))
                        print('counter', json.dumps({k: v for k, v in counters[-1].items() if k != 'rows'}),
                              flush=True)
                if int(time.monotonic()-began) != int(polled-began-.1):
                    health.append(dict(t=round(time.monotonic()-began, 1),
                                       hp=hero_health(game, world, seen['hero'])))
            time.sleep(1/90)
        final = sense_snapshot(game, rays['SpidySenseData'])
        clocks = [(r['t'], r['clock']) for r in rows if r['read']]
        rate = (clocks[-1][1]-clocks[0][1])/(clocks[-1][0]-clocks[0][0]) \
            if len(clocks) > 1 and clocks[-1][0] > clocks[0][0] else None
        report.update(sense=final, warnings=list(warnings.values()), counters=counters, health=health,
                      clock_rate=round(rate, 3) if rate is not None else None, rows=rows)
        print(json.dumps(dict(managers=final['managers'], read=final['read'], samples=final['samples'],
                              faults=final['faults'], clock_rate=report['clock_rate'], warnings=final['warnings'],
                              asked=final['asked'], thrown=final['thrown'], landed=final['landed'],
                              counter_blows=final['counter_blows'], breaks=final['breaks'],
                              census=final['census'])), flush=True)
    finally:
        for name in started:
            try:
                report.setdefault('stops', {})[name] = call_remote(process, rays[name])
            except OSError as error:
                report.setdefault('stops', {})[name] = str(error)
        close(process)
        report['restored'] = all(game.read(game.base+rva, 16) == pe.bytes(rva, 16) for rva in ENTRIES)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=1, default=str)+'\n')
        print('restored', report['restored'], 'stops', report.get('stops'), 'report', args.output, flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

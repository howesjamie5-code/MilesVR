"""The game's own melee impact sound for Spidy's blows in the running game, with no enemy about and no headset.

    python tools/probe_sound.py [--punches 4] [--repeats 12]

It needs a freshly started game in free roam (the ray module starts once per game process) and the game window in
front, which it brings there; a hero perched (on a pole, a sign) punches nothing, and --jump takes him off it first
(the game's A, then two seconds to land). It starts the ray module and the web shooter (whose main-thread frame hook sounds the
blows, game_sound.hpp). First the hero punches the air with the virtual controller's X --punches times, and his
AnimImpactEventManager's impact entries are read every few frames meanwhile: which of the three hit sounds, which
location and which intensity the game's own punches give their impacts. Then, 0.6 s apart, blows are sounded on the
player himself as on one struck - the Melee Impact Event's switches set on his sound sources, the hit sound played
on him - --repeats times for each of the three hit sounds, each followed by a moment of nothing, and the game's
Spider-Sense sound (one known to play) as many times for comparison. Every row has the wall-clock time it was asked,
to line up with a recording of the PC's output: a sound that plays rises at the same moment after every ask.
Writes reports/sound-probe.json.
"""
import argparse
import ctypes as c
import json
import pathlib
import struct
import time

from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from inspect_game import PE
from observe_game import call_remote, call_with_payload
from probe_game_grab import HERO_LOCAL, registry
from probe_game_screen import user32
from probe_menu_pad import BUTTONS, XR_DLL, call_with_output, pad
from run_game_vr import sound_snapshot
from vr_launcher import bring_to_front

# The shooter's main-thread hooks (camera update, muzzle): untouched before the probe, restored after it.
ENTRIES = (0xa24e90, 0x265d730)
SENSE = 0x4b5f5740
TEST_BYTES = 216
LOCATIONS = ('head', 'torso', 'leg')
INTENSITIES = ('none', 'light', 'medium', 'heavy', 'extreme')


def sound_test(play, hero, emitter=0, hash_=0, hit=0, blow=(2, 1, 1, 0, 0)):
    """game_sound.cpp's Test: the hero's impact entries read, and with play 1 a blow sounded on `emitter` (its
    intensity, location, combo, air and web strike), with play 2 the sound `hash_` played on him."""
    return struct.pack('<4I2Q2I5B3x', 0x53534e54, 2, TEST_BYTES, play, hero, emitter, hash_, hit,
                       *blow).ljust(TEST_BYTES, b'\0')


def answer(raw):
    done, manager, entries, sources, switches, result = struct.unpack_from('<6I', raw, 48)
    data, hits, places, strengths = (struct.unpack_from('<8I', raw, 72 + 32 * k) for k in range(4))
    sound, = struct.unpack_from('<Q', raw, 200)
    on_player, = struct.unpack_from('<I', raw, 208)
    return dict(done=bool(done), manager=bool(manager), sources=sources, switches=switches, on_player=bool(on_player),
                result=result if result < 0x80000000 else result - 0x100000000, sound=f'0x{sound:x}',
                entries=[dict(data=bool(data[i]), hit=hits[i],
                              location=LOCATIONS[places[i]] if places[i] < 3 else places[i],
                              intensity=INTENSITIES[strengths[i]] if strengths[i] < 5 else strengths[i])
                         for i in range(min(entries, 8))])


def main():
    try:
        user32.SetProcessDpiAwarenessContext(c.c_void_p(-4))
    except AttributeError:
        user32.SetProcessDPIAware()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--punches', type=int, default=4, help='presses of the game\'s X, the hero punching the air')
    parser.add_argument('--repeats', type=int, default=12, help='blows sounded for each hit sound')
    parser.add_argument('--jump', action='store_true', help='the game\'s A first: off a perch, down to the ground')
    parser.add_argument('--output', type=pathlib.Path, default=ROOT/'reports/sound-probe.json')
    args = parser.parse_args()
    game = Game(find_game())
    process = rays = xr = None
    rays_active = shooter_active = pad_taken = False
    report = dict(reads=[], plays=[])
    try:
        pe = PE(game.path.read_bytes())
        if any(game.read(game.base+rva, 16) != pe.bytes(rva, 16) for rva in ENTRIES):
            raise RuntimeError('A hook entry is already patched: start the game afresh')
        bring_to_front(game.pid)
        heroes = [r for a, v, r, _ in registry(game) if v == HERO_LOCAL]
        if len(heroes) != 1:
            raise RuntimeError('Exactly one local hero is required: load free roam first')
        hero = heroes[0]
        report['hero'] = hex(hero)
        process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
        if not process:
            raise c.WinError(c.get_last_error())
        rays, report['ray_hash'] = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                                           ROOT/'reports/ray-modules',
                                           ('SpidyRayStart', 'SpidyRayStop', 'SpidyShooterStart', 'SpidyShooterStop',
                                            'SpidySoundTest', 'SpidySoundData'))
        xr, _ = prepare(game.pid, process, XR_DLL, ROOT/'reports/stereo-modules',
                        ('SpidyPadSubmit', 'SpidyPadSample', 'SpidyPlayerFind', 'SpidyPadRelease'))

        def invoke(address, payload):
            code = call_with_payload(process, address, payload)
            if code:
                raise RuntimeError(f'Native call rejected: {code}')
        invoke(rays['SpidyRayStart'], struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410))
        rays_active = True
        invoke(rays['SpidyShooterStart'], struct.pack('<4IQ', 0x53484f43, 1, 24, game.pid, game.base))
        shooter_active = True

        def test(play=0, hash_=0, hit=0):
            code, raw = call_with_output(game, process, rays['SpidySoundTest'],
                                         sound_test(play, hero, hero if play else 0, hash_, hit), TEST_BYTES)
            return dict(code=code, **answer(raw)) if not code else dict(code=code)

        started = time.monotonic()
        if args.jump:
            pad_taken = True
            pad(process, xr, BUTTONS['a'], 120)
            time.sleep(2)
        # The hero punches the air: each attack fills his impact manager with its impact entries.
        for n in range(args.punches):
            pad_taken = True
            pad(process, xr, BUTTONS['x'], 120)
            for _ in range(8):
                r = dict(test(), label=f'punch {n+1}', seconds=round(time.monotonic()-started, 3))
                report['reads'].append(r)
        kinds = sorted({(e['hit'], e['location'], e['intensity']) for r in report['reads'] if r.get('code') == 0
                        for e in r.get('entries', []) if e['data']}, key=str)
        report['entries_seen'] = [dict(hit=h, location=l, intensity=i) for h, l, i in kinds]
        print(json.dumps(dict(entries_seen=report['entries_seen'])), flush=True)
        # Time-locked plays for a recording: each followed by nothing.
        plan = []
        for hit in (0, 1, 2):
            plan += [(f'hit {hit}', 1, 0, hit), ('nothing', 0, 0, 0)] * args.repeats
        plan += [('spider-sense', 2, SENSE, 0), ('nothing', 0, 0, 0)] * args.repeats
        for label, play, hash_, hit in plan:
            time.sleep(.6)
            asked = time.time()
            row = dict(label=label, asked=round(asked, 3))
            if play:
                r = test(play, hash_, hit)
                row.update({k: r.get(k) for k in ('code', 'result', 'sources', 'switches', 'on_player')})
            report['plays'].append(row)
        time.sleep(1)
        report['data'] = sound_snapshot(game, rays['SpidySoundData'])
    finally:
        if pad_taken and xr:
            report['pad_release'] = call_remote(process, xr['SpidyPadRelease'])
        if shooter_active:
            report['shooter_stop'] = call_remote(process, rays['SpidyShooterStop'])
        if rays_active:
            report['ray_stop'] = call_remote(process, rays['SpidyRayStop'])
        if process:
            close(process)
        try:
            pe = PE(game.path.read_bytes())
            report['entries_restored'] = all(game.read(game.base+rva, 16) == pe.bytes(rva, 16) for rva in ENTRIES)
        except Exception as error:  # the game may have closed
            report['entries_restored'] = str(error)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=1)+'\n')
    blows = [p for p in report['plays'] if p['label'].startswith('hit')]
    played = [p for p in blows if p.get('code') == 0 and p.get('result') == 1]
    summary = dict(blows=len(blows), played=len(played),
                   sources=sorted({p.get('sources') for p in blows}), switches=sorted({p.get('switches') for p in blows}),
                   entries_seen=report.get('entries_seen'), entries_restored=report.get('entries_restored'))
    print(json.dumps(summary))
    return 0 if played and report.get('entries_restored') is True else 1


if __name__ == '__main__':
    raise SystemExit(main())

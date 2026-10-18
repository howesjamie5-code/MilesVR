"""The gadget wheel in the running game, without a headset: the player's gadgets, each equipped and fired from a
scripted hand.

    python tools/probe_gadgets.py

It needs a freshly started game in free roam (Spidy's ray and movement modules start once per process) and the game
window in front, which it brings there. It reads the player's gadgets (SpidyShooterData v4), has the free hand fire
the equipped gadget (SpidyShooterGadgets), and for each gadget with a charge: equips it the way the game's own wheel
does and checks the game took it, pulls the trigger of a hand 1.2 m above the feet aimed 30 degrees down along the
game camera, and checks a shot went out and the gadget gave up a charge. Then the web shooter the same way (its own
web ball). At the end the gadget the player had equipped is equipped again, and the hand's fire set back to the web
ball. Writes reports/gadget-probe.json with a screenshot after each shot.
"""
import argparse
import ctypes as c
import json
import math
import pathlib
import struct
import time
from bridge_game import ROOT, prepare
from capture_game_state import Game, find_game, open_process, close
from inspect_game import PE
from observe_game import call_remote, call_with_payload, modules
from probe_aim import swing_health
from probe_game_grab import ENTRIES, HERO_LOCAL, HERO_MOVERS, camera, hand_command, registry, resolve, shot
from probe_game_screen import user32
from run_game_vr import shooter_snapshot
from vr_launcher import bring_to_front

SHOOTER_ENTRIES = (0x897d30, 0x2150c40)


def gadget_command(fire, equip=0):
    """game_shooter::GadgetCommand: the hand fires the equipped gadget (fire 1), and a weapon id to equip."""
    return struct.pack('<4I2HI', 0x53484f47, 1, 24, fire, equip, 0, 0)


def main():
    try:
        user32.SetProcessDpiAwarenessContext(c.c_void_p(-4))
    except AttributeError:
        user32.SetProcessDPIAware()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=pathlib.Path, default=ROOT/'reports/gadget-probe.json')
    args = parser.parse_args()
    game = Game(find_game())
    process = rays = None
    rays_active = swing_active = shooter_active = False
    report = dict(shots=[])
    entries = (*ENTRIES, *SHOOTER_ENTRIES)
    original = None
    try:
        pe = PE(game.path.read_bytes())
        if any(game.read(game.base+rva, 16) != pe.bytes(rva, 16) for rva in entries):
            raise RuntimeError('A hook entry is already patched: start the game afresh')
        bring_to_front(game.pid)
        components = list(registry(game))
        heroes = [(a, r) for a, v, r, _ in components if v == HERO_LOCAL]
        if len(heroes) != 1:
            raise RuntimeError('Exactly one local hero is required: load free roam first')
        record = heroes[0][1]
        managers = [a for a, v, r, _ in components if v == HERO_MOVERS and r == record]
        mover = resolve(game, struct.unpack('<I', game.read(managers[0]+0xdb4, 4))[0])
        feet = game.transform(game.pointer(record))['position']
        hand = (feet[0], feet[1]+1.2, feet[2])
        process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
        if not process:
            raise c.WinError(c.get_last_error())
        prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_movement_bridge.dll', ROOT/'reports/motion-modules',
                ('SpidyMotionStart', 'SpidyMotionStop'))
        motion_module = next(m['base'] for m in modules(game.pid) if m['name'].lower() == 'spidy_movement_bridge.dll')
        rays, report['ray_hash'] = prepare(game.pid, process, ROOT/'build/windows-ninja/spidy_ray_bridge.dll',
                                           ROOT/'reports/ray-modules',
                                           ('SpidyRayStart', 'SpidyRayStop', 'SpidySwingStart', 'SpidySwingSubmit',
                                            'SpidySwingStop', 'SpidySwingData', 'SpidyShooterStart',
                                            'SpidyShooterStop', 'SpidyShooterGadgets', 'SpidyShooterData'))

        def invoke(address, payload):
            code = call_with_payload(process, address, payload)
            if code:
                raise RuntimeError(f'Native call rejected: {code}')
        invoke(rays['SpidyRayStart'], struct.pack('<4IQ2I', 0x53525943, 1, 32, game.pid, game.base, 30000, 0x410))
        rays_active = True
        invoke(rays['SpidySwingStart'], struct.pack('<4I4QI2fI', 0x53574346, 1, 64, game.pid, game.base, record, mover,
                                                    motion_module, 30000, 32., 6., 0))
        swing_active = True
        invoke(rays['SpidyShooterStart'], struct.pack('<4IQ', 0x53484f43, 1, 24, game.pid, game.base))
        shooter_active = True
        serial = 10
        forward, _ = camera(game)
        yaw = math.atan2(forward[0], forward[2]) if forward else 0.
        down = (math.sin(yaw)*.866, -.5, math.cos(yaw)*.866)

        def hold(trigger, seconds):
            nonlocal serial
            until = time.monotonic()+seconds
            while time.monotonic() < until:
                serial += 1
                invoke(rays['SpidySwingSubmit'], hand_command(serial, hand, down, trigger=trigger))
                time.sleep(.03)

        def sample():
            for _ in range(8):
                shots = shooter_snapshot(game, rays['SpidyShooterData'])
                if shots:
                    return shots
            return None

        # The list, once the shooter has the player from the hand's samples.
        listed = None
        until = time.monotonic()+4
        while time.monotonic() < until:
            hold(0, .2)
            listed = sample()
            if listed and listed['gadgets']:
                break
        report['listed'] = listed
        if not listed or listed['gadget_error'] or not listed['gadgets']:
            raise RuntimeError(f"No gadgets listed: {listed and listed['gadget_error']}")
        original = listed['equipped']
        print(f"equipped {original}; gadgets: " + ', '.join(f"{g['gadget']} {g['ammo']}/{g['most']}"
                                                           for g in listed['gadgets']), flush=True)
        invoke(rays['SpidyShooterGadgets'], gadget_command(1))

        def equip(weapon):
            before = sample()
            invoke(rays['SpidyShooterGadgets'], gadget_command(1, weapon))
            until = time.monotonic()+3
            now = before
            while time.monotonic() < until:
                hold(0, .1)
                now = sample()
                if now['equips'] > before['equips'] and now['equipped'] == weapon:
                    break
            return now

        order = [g for g in listed['gadgets'] if g['gadget'] != 'web_shooter' and g['ammo'] > 0] + \
                [g for g in listed['gadgets'] if g['gadget'] == 'web_shooter']
        for g in order:
            entry = dict(gadget=g['gadget'], weapon=g['weapon'])
            after_equip = equip(g['weapon'])
            entry.update(equip_result=after_equip['equip_result'], equipped=after_equip['equipped'] == g['weapon'])
            hold(0, .2)
            before = sample()
            ammo_before = next((x['ammo'] for x in before['gadgets'] if x['weapon'] == g['weapon']), None)
            hold(1, .25)
            hold(0, .7)  # past the next listing
            after = sample()
            ammo_after = next((x['ammo'] for x in after['gadgets'] if x['weapon'] == g['weapon']), None)
            entry.update(fired=after['fired']-before['fired'], dropped=after['dropped']-before['dropped'],
                         gadget_shots=after['gadget_shots']-before['gadget_shots'],
                         empty=after['empty_pulls']-before['empty_pulls'], ammo=[ammo_before, ammo_after],
                         origin=after['last_origin'], aim_point=after['last_aim_point'],
                         swing=swing_health(game, rays['SpidySwingData']),
                         screenshot=shot(game, f"gadget-{g['gadget']}"))
            report['shots'].append(entry)
            print(f"{g['gadget']:>18}: equipped {entry['equipped']} (code {entry['equip_result']}), fired "
                  f"{entry['fired']} (gadget {entry['gadget_shots']}), dropped {entry['dropped']}, empty "
                  f"{entry['empty']}, charges {ammo_before} -> {ammo_after}", flush=True)
    finally:
        try:
            if shooter_active and original is not None:
                final = None
                call_with_payload(process, rays['SpidyShooterGadgets'], gadget_command(0, original))
                until = time.monotonic()+3
                while time.monotonic() < until:
                    time.sleep(.1)
                    final = shooter_snapshot(game, rays['SpidyShooterData'])
                    if final and final['equipped'] == original:
                        break
                report['restored'] = bool(final and final['equipped'] == original)
                report['final'] = final
        except Exception as error:
            report['restored'] = str(error)
        if shooter_active:
            report['shooter_stop'] = call_remote(process, rays['SpidyShooterStop'])
        if swing_active:
            report['swing_stop'] = call_remote(process, rays['SpidySwingStop'])
        if rays_active:
            report['ray_stop'] = call_remote(process, rays['SpidyRayStop'])
        if process:
            close(process)
        try:
            pe = PE(game.path.read_bytes())
            report['entries_restored'] = all(game.read(game.base+rva, 16) == pe.bytes(rva, 16) for rva in entries)
        except Exception as error:  # the game may have closed
            report['entries_restored'] = str(error)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=1)+'\n')
    print(json.dumps({k: report.get(k) for k in ('restored', 'shooter_stop', 'swing_stop', 'ray_stop',
                                                 'entries_restored')}))
    shots = report['shots']
    ok = shots and all(s['equipped'] and s['fired'] == 1 for s in shots) and report.get('restored') is True and \
        report.get('entries_restored') is True
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())

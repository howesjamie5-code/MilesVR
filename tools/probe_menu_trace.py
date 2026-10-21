"""The game's pause menu as Flash sees it, without a headset: the calls into Flash and the callbacks from it.

    python tools/probe_menu_trace.py [--steps start,down,down,up,up,b]

It needs a started game with a loaded save and the game window in front. It starts the SPIDY VR tab's hooks
(SpidyMenuStart, as tools/probe_menu.py does), records every Flash call and callback they see (SpidyMenuTrace), and
plays the steps on Spidy's virtual Xbox controller: by default it pauses, moves down and up the pause menu's first
page, and resumes with B. After each step it saves a window capture in reports/menu-trace/ and prints what the
menus called. Writes reports/menu-trace.json.
"""
import argparse
import ctypes as c
import json
import pathlib
import struct
import time
from bridge_game import ROOT, prepare
from observe_game import call_remote, call_with_payload
from probe_game_screen import user32
from probe_menu import FLAGS, STICK, capture, settings_payload
import probe_menu_pad as pad

OUTPUT = ROOT/'reports/menu-trace'
EXPORTS = ('SpidyMenuStart', 'SpidyMenuStop', 'SpidyMenuTrace', 'SpidyMenuTraceData', 'SpidyMenuSample')
ENTRY = 224
TYPES = {0: 'undefined', 1: 'null', 2: 'bool', 3: 'int', 4: 'uint', 5: 'number', 6: 'string', 7: 'wstring',
         8: 'object', 9: 'array', 10: 'display'}


def entries(game, address, start):
    """The trace's entries from `start` on: (next, [entry])."""
    head = game.read(address, 24)
    magic, version, size, on, next_ = struct.unpack_from('<5I', head)
    if (magic, version) != (0x54524d53, 1):
        raise RuntimeError('Menu trace protocol mismatch')
    out = []
    for i in range(start, next_):
        raw = game.read(address+24+(i % 256)*ENTRY, ENTRY)
        kind, hash_, count, _, ui, caller = struct.unpack_from('<4I2Q', raw)
        name = raw[32:80].split(b'\0')[0].decode('ascii', 'replace')
        types = struct.unpack_from('<4I', raw, 80)
        values = struct.unpack_from('<4Q', raw, 96)
        texts = [raw[128+k*24:152+k*24].split(b'\0')[0].decode('utf-8', 'replace') for k in range(4)]
        args = []
        for k in range(min(count, 4)):
            t = types[k] & 0x8f
            v = texts[k] if t == 6 else values[k] if t in (2, 3, 4) else \
                struct.unpack('<d', struct.pack('<Q', values[k]))[0] if t == 5 else hex(values[k])
            args.append(f"{TYPES.get(t, t)}:{v}")
        out.append(dict(kind='call' if kind == 1 else 'callback', name=name or None,
                        hash=hex(hash_) if kind == 2 else None, ui=hex(ui), caller=hex(caller), count=count,
                        args=args))
    return next_, out


def main():
    try:
        user32.SetProcessDpiAwarenessContext(c.c_void_p(-4))
    except AttributeError:
        user32.SetProcessDPIAware()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--steps', default='start,down,down,up,up,b')
    parser.add_argument('--output', type=pathlib.Path, default=ROOT/'reports/menu-trace.json')
    args = parser.parse_args()
    game, process, xr, _ = pad.attach()
    exports, _ = prepare(game.pid, process, pad.XR_DLL, ROOT/'reports/stereo-modules', EXPORTS)
    report = dict(pid=game.pid, steps=[])
    OUTPUT.mkdir(parents=True, exist_ok=True)
    seen = 0
    try:
        flags = FLAGS['web_grab'] | FLAGS['punch'] | FLAGS['body'] | FLAGS['air_webs'] | FLAGS['aim_markers']
        code = call_with_payload(process, exports['SpidyMenuStart'], settings_payload(flags, 30, 100, 1, 32.0))
        if code:
            raise RuntimeError(f'SpidyMenuStart: {code}')
        call_remote(process, exports['SpidyMenuTrace'], 1)
        for n, name in enumerate(args.steps.split(',')):
            if name in STICK:
                pad.pad(process, xr, 0, sticks=(*STICK[name], 0, 0))
            else:
                pad.pad(process, xr, pad.BUTTONS[name])
            time.sleep(1.5 if name in ('start', 'a', 'b') else .6)
            seen, calls = entries(game, exports['SpidyMenuTraceData'], seen)
            # VR VIEW: GAME SCREEN, as the menu holds it (SpidyMenuSample's flag 1024).
            code, raw = pad.call_with_output(game, process, exports['SpidyMenuSample'], b'', 80)
            flags = struct.unpack_from('<I', raw, 36)[0] if not code and len(raw) == 80 else 0
            step = dict(step=name, capture=capture(game, f'{n:02d}-{name}'), calls=calls,
                        game_screen=bool(flags & 1024), changes=struct.unpack_from('<Q', raw, 24)[0])
            report['steps'].append(step)
            print(f"--- {name}: {len(calls)} calls, VR VIEW {'GAME SCREEN' if step['game_screen'] else 'VR'}",
                  flush=True)
            for e in calls:
                print('   ', e['kind'], e['name'] or e['hash'], e['caller'], e['count'], ' '.join(e['args']),
                      flush=True)
    finally:
        call_remote(process, exports['SpidyMenuTrace'], 0)
        report['stop'] = call_remote(process, exports['SpidyMenuStop'])
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=1)+'\n')


if __name__ == '__main__':
    main()

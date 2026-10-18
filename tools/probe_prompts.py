"""The game's prompt texts as its Flash screens hand them over for their button icons, no headset.

The hint line ("LB + RB (Hold) Throw"), tutorial cards and other texts with a button in them reach the game's
markup resolver with their markup still in them (src/game_prompts.cpp hooks it). This lists the texts that passed
since the hook went in, newest last, and can set the research rule that rewrites one on its way:

    python tools/probe_prompts.py                         hook (if not yet) and list
    python tools/probe_prompts.py --find TEXT --replace TEXT   texts holding the first pass with the second
    python tools/probe_prompts.py --find ""               end the rule
    python tools/probe_prompts.py --stop                  unhook

Needs the running game (`tools/probe_menu_pad.py start`); a prompt is resolved when it is shown, so list after
the game has shown one (a loaded save near something to interact with).
"""
import argparse
import ctypes as c
import json
import struct
import sys
from bridge_game import ROOT, prepare
from capture_game_state import Game, close, find_game, open_process
from observe_game import call_remote, call_with_payload
from probe_menu_pad import XR_DLL, call_with_output

EXPORTS = ('SpidyPromptsStart', 'SpidyPromptsSample', 'SpidyPromptsStop', 'SpidyPromptsCalls', 'SpidyPromptsTexts')
ENTRIES, CHARS, RULE = 48, 256, 96


def sample(game, process, xr):
    size = 48+ENTRIES*CHARS
    code, raw = call_with_output(game, process, xr['SpidyPromptsSample'], b'', size)
    if code or len(raw) != size:
        raise RuntimeError(f'Prompt trace: {code}')
    magic, version, size_, status, resolved, rewritten, function, count, _ = struct.unpack_from('<4I3Q2I', raw)
    if (magic, version, size_) != (0x53505254, 1, size):
        raise RuntimeError('Prompt trace protocol mismatch')
    texts = [raw[48+i*CHARS:48+(i+1)*CHARS].split(b'\0', 1)[0].decode('utf-8', 'replace') for i in range(count)]
    return dict(hooked=bool(status), function=hex(function), resolved=resolved, rewritten=rewritten, texts=texts)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--find', help='the research rule: texts holding this')
    p.add_argument('--replace', default='', help='pass with it replaced by this')
    p.add_argument('--stop', action='store_true')
    p.add_argument('--calls', action='store_true',
                   help="list the game's calls into Flash too, by name with their first arguments (research)")
    p.add_argument('--texts', action='store_true',
                   help="list the game's localized texts that hold markup too, with their tag's hash (research)")
    a = p.parse_args()
    game = Game(find_game())
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    if not process:
        raise c.WinError(c.get_last_error())
    try:
        xr, _ = prepare(game.pid, process, XR_DLL, ROOT/'reports/stereo-modules', EXPORTS)
        if a.stop:
            print(json.dumps(dict(stop=call_remote(process, xr['SpidyPromptsStop']))))
            return 0
        if a.find is not None:
            rule = a.find.encode()[:RULE-1].ljust(RULE, b'\0')+a.replace.encode()[:RULE-1].ljust(RULE, b'\0')
            code = call_with_payload(process, xr['SpidyPromptsStart'], rule)
        else:
            code = call_remote(process, xr['SpidyPromptsStart'])
        calls = call_remote(process, xr['SpidyPromptsCalls']) if a.calls else None
        texts = call_remote(process, xr['SpidyPromptsTexts']) if a.texts else None
        result = dict(start=code, calls=calls, localized=texts, **sample(game, process, xr))
        sys.stdout.reconfigure(encoding='utf-8')
        print(json.dumps(result, indent=1, ensure_ascii=False))
        return 0 if not code else 1
    finally:
        close(process)
        game.close()


if __name__ == '__main__':
    sys.exit(main())

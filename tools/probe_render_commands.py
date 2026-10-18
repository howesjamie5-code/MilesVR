"""Count the render commands the game queues, by command and by the view whose list takes them.

Run while Spidy's eye views render in the game (a VR session, or tools/probe_vr_load.py in another
console): the stereo module counts for --seconds (SpidyCommandTrace) every command queued through the
engine's 187e130, as "other" (lists that belong to no scene view: models, shadow casters, effects), the
game's own view, and each eye. Printed per frame, with the engine's own command names.

    python tools/probe_render_commands.py --output reports/render-commands.json

It shows what a view costs the render thread and what stops when one does not render: on October 10,
ModelShadowCasters fell from 8,556 to 5,604 a frame when the game's own view stopped rendering, while
Scaleform (the HUD), VisualEffectsCPUReadback and StreamingVideo, queued on that view's own list, went on.
"""
import argparse
import json
import pathlib
import struct
import sys
import time
from capture_game_state import Game, find_game, open_process, close
from inspect_game import PE
from observe_game import call_remote, modules

MODULE = 'spidy_stereo_probe.dll'
OWNERS = ('other', 'game_view', 'left_eye', 'right_eye')
# The engine's table of command names, by command (image offset; 256 pointers).
NAMES = 0x7a2d9b8


def exports(pe):
    """Export name -> image offset of a PE image."""
    data, header = pe.data, pe.u32(0x3c)
    directory = struct.unpack_from('<I', data, header+24+112)[0]
    at = pe.offset(directory, 40)
    count = struct.unpack_from('<I', data, at+24)[0]
    functions, names, ordinals = struct.unpack_from('<III', data, at+28)
    found = {}
    for i in range(count):
        name = pe.offset(pe.u32(pe.offset(names+i*4, 4)))
        ordinal = struct.unpack_from('<H', data, pe.offset(ordinals+i*2, 2))[0]
        found[data[name:data.index(b'\0', name)].decode()] = pe.u32(pe.offset(functions+ordinal*4, 4))
    return found


def trace(game, seconds):
    """Commands per frame: {'frames': n, 'commands': {'0x56 ModelShadowCasters': [other, game, left, right]}}."""
    loaded = [m for m in modules(game.pid) if m['name'].lower() == MODULE]
    if not loaded:
        raise RuntimeError("Spidy's stereo module is not in the game: start VR or a probe's eye views first.")
    module = loaded[-1]
    found = exports(PE(pathlib.Path(module['path']).read_bytes()))
    if 'SpidyCommandTrace' not in found:
        raise RuntimeError('The stereo module in the game is from a build without the command trace.')
    process = open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    try:
        code = call_remote(process, module['base']+found['SpidyCommandTrace'], 1)
        if code:
            raise RuntimeError(f'Command trace: {code} (4201: the eye views have not started in this game)')
        time.sleep(seconds)
        call_remote(process, module['base']+found['SpidyCommandTrace'], 0)
    finally:
        close(process)
    raw = game.read(module['base']+found['SpidyCommandData'], 24+4*256*8)
    if struct.unpack_from('<3I', raw) != (0x53434d44, 1, 24+4*256*8):
        raise RuntimeError('Command trace protocol mismatch')
    frames = struct.unpack_from('<Q', raw, 16)[0]
    counts = [struct.unpack_from('<256Q', raw, 24+owner*256*8) for owner in range(4)]
    commands = {}
    for command in range(256):
        row = [counts[owner][command] for owner in range(4)]
        if any(row):
            name = game.pointer(game.base+NAMES+command*8)
            try:
                text = game.read(name, 48).split(b'\0')[0].decode() if name else '?'
            except (OSError, UnicodeDecodeError):
                text = '?'
            commands[f'{command:#04x} {text}'] = [round(c/max(frames, 1), 2) for c in row]
    return dict(frames=frames, seconds=seconds, commands=commands)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--seconds', type=float, default=3)
    p.add_argument('--output', type=pathlib.Path)
    a = p.parse_args()
    game = Game(find_game())
    try:
        result = trace(game, a.seconds)
    finally:
        game.close()
    print(f"{result['frames']} frames; commands a frame:")
    print(f"{'':42s} " + ' '.join(f'{owner:>10s}' for owner in OWNERS))
    for name, row in result['commands'].items():
        print(f'{name:42s} ' + ' '.join(f'{value:10.2f}' for value in row))
    if a.output:
        a.output.parent.mkdir(parents=True, exist_ok=True)
        a.output.write_text(json.dumps(result, indent=1)+'\n')
        print(f'Report: {a.output.resolve()}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f'Render command probe failed: {error}', file=sys.stderr)
        sys.exit(1)

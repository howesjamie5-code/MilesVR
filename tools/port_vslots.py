"""Virtual functions by their vtable slot: a Spider-Man.exe function stored in a vtable that
port_map.py paired by class name maps to the same slot of the MilesMorales.exe vtable.

    python -I tools/port_vslots.py 0x72d640 ...      (Spider-Man.exe RVAs)
"""
import json, pathlib, struct, sys
from collections import Counter
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from port_map import Image, SM_EXE, MM_EXE  # noqa: E402
ROOT = pathlib.Path(__file__).resolve().parent.parent


def votes(wanted):
    sm, mm = Image(SM_EXE), Image(MM_EXE)
    vt = {int(k, 16): (int(v, 16), n) for k, (v, n) in
          json.loads((ROOT / 'build' / 'port_map.json').read_text())['vtables'].items()}
    want = {sm.image_base + w: w for w in wanted}
    out = {w: Counter() for w in wanted}
    for a, (b, name) in vt.items():
        for slot in range(1024):
            x, y = sm.read(a + 8 * slot, 8), mm.read(b + 8 * slot, 8)
            if not x or not y:
                break
            fa, fb = struct.unpack('<Q', x)[0], struct.unpack('<Q', y)[0]
            if not sm.is_code(fa - sm.image_base):
                break
            if fa in want and mm.is_code(fb - mm.image_base):
                out[want[fa]][(fb - mm.image_base, f'{name} slot {slot}')] += 1
    return out


if __name__ == '__main__':
    for w, c in votes([int(x, 16) for x in sys.argv[1:]]).items():
        targets = Counter()
        for (t, _), n in c.items():
            targets[t] += n
        ex = next(iter(c), (0, ''))[1]
        print(f'{w:#x}: ' + (', '.join(f'{t:#x} x{n}' for t, n in targets.most_common(3)) + f'  (e.g. {ex})'
                             if c else 'in no paired vtable'))

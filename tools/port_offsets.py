"""Field offsets that moved between Spider-Man.exe and MilesMorales.exe.

Spidy reads the game's objects at fixed offsets, and some of those objects grew in Miles Morales
(the view object by 0x1c0 bytes past +0x12000, for one). Every function port_map.py could not pair
exactly is paired here by port_fuzzy's window and instruction similarity; in the aligned pairs, an
instruction that differs only in its memory displacement votes that the offset moved, and one that
is the same votes that it stayed. Votes are kept per displacement and per base register class.

    python -I tools/port_offsets.py [--workers 16] [--out build/port_offsets.json]

build/port_offsets.json: {"0x14e38": {"0x14ff8": 41, "0x14e38": 0}, ...} for displacements >= 0x80.
"""
import argparse, bisect, difflib, json, multiprocessing as mp, pathlib, sys, time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
ROOT = pathlib.Path(__file__).resolve().parent.parent

_state = {}


def _init(map_path, max_candidates=None):
    from port_map import Image, SM_EXE, MM_EXE
    from port_fuzzy import Fuzzy
    sm, mm = Image(SM_EXE), Image(MM_EXE)
    _state['fz'] = Fuzzy(sm, mm, json.loads(pathlib.Path(map_path).read_text()),
                          max_candidates)
    _state['sm'] = sm


def _disp(shape):
    """[(base register, displacement)] of an instruction shape from port_fuzzy.instructions."""
    out = []
    for part in shape.split(' ', 1)[-1].split(','):
        if part.startswith('[') and part != '[rip]':
            base, rest = part[1:-1].split('+', 1)
            disp = int(rest.rsplit('+', 1)[-1], 16)
            out.append((base, disp, rest.rsplit('+', 1)[0]))
    return out


def _work(starts):
    fz = _state['fz']
    moved, kept = {}, {}
    for s in starts:
        try:
            res = fz.pair(s)
        except Exception:
            continue
        fz.pairs.pop(s, None)  # keep memory flat
        if not res or res[0] is None or len(res) < 4 or res[2] is None or res[1] < 0.75:
            continue
        a, b, m = res[2]
        for tag, i1, i2, j1, j2 in m.get_opcodes():
            if tag == 'equal':
                for k in range(i2 - i1):
                    for _, d, _ in _disp(a[i1 + k][2]):
                        if d >= 0x80:
                            kept[d] = kept.get(d, 0) + 1
            elif tag == 'replace' and i2 - i1 == j2 - j1:
                for k in range(i2 - i1):
                    sa, sb = a[i1 + k][2], b[j1 + k][2]
                    if sa.split(' ', 1)[0] != sb.split(' ', 1)[0]:
                        continue
                    da, db = _disp(sa), _disp(sb)
                    if len(da) != len(db) or not da:
                        continue
                    # Same instruction apart from the displacement(s).
                    if [(x[0], x[2]) for x in da] != [(x[0], x[2]) for x in db]:
                        continue
                    if sa.replace(hex(da[0][1]), '') != sb.replace(hex(db[0][1]), ''):
                        continue
                    for (_, x, _), (_, y, _) in zip(da, db):
                        if x >= 0x80 and x != y:
                            moved.setdefault(x, {})
                            moved[x][y] = moved[x].get(y, 0) + 1
    return moved, kept


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--workers', type=int, default=16)
    p.add_argument('--map', default=str(ROOT / 'build' / 'port_map.json'))
    p.add_argument('--out', default=str(ROOT / 'build' / 'port_offsets.json'))
    p.add_argument('--limit', type=int, default=0)
    p.add_argument('--max-size', type=lambda s: int(s, 0), default=0x4000)
    p.add_argument('--max-candidates', type=int, default=0)
    a = p.parse_args()
    from port_map import Image, SM_EXE
    sm = Image(SM_EXE)
    paired = {int(k, 16) for k in json.loads(pathlib.Path(a.map).read_text())['functions']}
    todo = [b for b, e in sm.functions if b not in paired and 24 <= e - b <= a.max_size]
    if a.limit:
        todo = todo[:a.limit]
    print(f'{len(todo)} unpaired functions to align', file=sys.stderr)
    chunks = [todo[i:i + 200] for i in range(0, len(todo), 200)]
    moved, kept = {}, {}
    t = time.time()
    with mp.Pool(a.workers, _init, (a.map, a.max_candidates or None)) as pool:
        for n, (mv, kp) in enumerate(pool.imap_unordered(_work, chunks), 1):
            for d, c in kp.items():
                kept[d] = kept.get(d, 0) + c
            for d, ys in mv.items():
                for y, c in ys.items():
                    moved.setdefault(d, {})
                    moved[d][y] = moved[d].get(y, 0) + c
            if n % 50 == 0:
                print(f'  {n}/{len(chunks)} chunks, {time.time() - t:.0f}s', file=sys.stderr)
    out = {}
    for d in sorted(set(moved) | set(kept)):
        votes = {f'{y:#x}': c for y, c in moved.get(d, {}).items()}
        votes[f'{d:#x}'] = kept.get(d, 0)
        out[f'{d:#x}'] = votes
    pathlib.Path(a.out).write_text(json.dumps(out))
    print(f'wrote {a.out}: {len(moved)} displacements seen moving', file=sys.stderr)


if __name__ == '__main__':
    main()

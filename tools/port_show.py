"""Side-by-side disassembly of a Spider-Man.exe function and its MilesMorales.exe twin, aligned
instruction by instruction, for checking a port by eye.

    python -I tools/port_show.py <Spider-Man.exe RVA> [MilesMorales.exe RVA] [--around RVA] [--context 12]

Without the second RVA, the pair comes from port_map / port_fuzzy. --around shows only the
instructions near one address inside the function.
"""
import argparse, difflib, pathlib, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from port_map import Image, SM_EXE, MM_EXE, _md  # noqa: E402
from port_fuzzy import Fuzzy, instructions  # noqa: E402
import json  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent


def text(img, rva, size):
    return {i.address: f'{i.mnemonic} {i.op_str}' for i in _md.disasm(img.read(rva, size) or b'', rva)}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('sm', type=lambda s: int(s, 16))
    p.add_argument('mm', nargs='?', type=lambda s: int(s, 16))
    p.add_argument('--around', type=lambda s: int(s, 16))
    p.add_argument('--context', type=int, default=12)
    a = p.parse_args()
    sm, mm = Image(SM_EXE), Image(MM_EXE)
    f = sm.function_at(a.sm) or (a.sm, a.sm + 0x100)
    if a.mm is None:
        fz = Fuzzy(sm, mm, json.loads((ROOT / 'build' / 'port_map.json').read_text()))
        res = fz.pair(f[0])
        a.mm = res[0]
        print(f'pair: {f[0]:#x} -> {a.mm:#x}, score {res[1]:.3f}' + (f', runner-up {res[3]:.3f}' if len(res) > 3 else ''))
    g = mm.function_at(a.mm) or (a.mm, a.mm + (f[1] - f[0]))
    ia, ib = instructions(sm, *f), instructions(mm, *g)
    ta, tb = text(sm, f[0], f[1] - f[0]), text(mm, g[0], g[1] - g[0])
    m = difflib.SequenceMatcher(None, [x[2] for x in ia], [x[2] for x in ib], autojunk=False)
    rows = []
    for tag, i1, i2, j1, j2 in m.get_opcodes():
        for k in range(max(i2 - i1, j2 - j1)):
            l = ia[i1 + k] if i1 + k < i2 else None
            r = ib[j1 + k] if j1 + k < j2 else None
            rows.append((tag, l, r))
    if a.around is not None:
        idx = next((n for n, (_, l, _) in enumerate(rows) if l and l[0] <= a.around < l[0] + l[1]), 0)
        rows = rows[max(0, idx - a.context):idx + a.context + 1]
    mark = {'equal': ' ', 'replace': '~', 'delete': '-', 'insert': '+'}
    for tag, l, r in rows:
        left = f'{l[0]:8x}  {ta.get(l[0], "?")[:48]:48}' if l else ' ' * 58
        right = f'{r[0]:8x}  {tb.get(r[0], "?")[:48]}' if r else ''
        print(f'{mark[tag]} {left} | {right}')


if __name__ == '__main__':
    main()

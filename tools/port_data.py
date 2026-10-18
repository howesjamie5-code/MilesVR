"""Checks a global's port by the code that reaches it: every RIP-relative instruction in
Spider-Man.exe that addresses the global (or up to --slack bytes into it) is found, its function's
twin in MilesMorales.exe is aligned (port_fuzzy), and the aligned instruction's address is a vote.

    python -I tools/port_data.py 0x7a34e00 0x7a2d9b8 ...      (Spider-Man.exe RVAs)
"""
import argparse, json, pathlib, struct, sys
from collections import Counter

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from port_map import Image, SM_EXE, MM_EXE, _md  # noqa: E402
from port_fuzzy import Fuzzy  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent


def references(sm, target, slack):
    """(instruction RVA, offset into the global) for RIP-relative uses of [target, target+slack]."""
    import numpy as np
    found = []
    for s in sm.sections:
        if not s['exec']:
            continue
        raw, va, size = s['raw'], s['va'], s['rsize']
        code = np.frombuffer(sm.data, dtype=np.uint8, count=size, offset=raw)
        # Every unaligned little-endian int32: disp[o] reads bytes o..o+3.
        disp = (code[:-3].astype(np.int64) | (code[1:-2].astype(np.int64) << 8) |
                (code[2:-1].astype(np.int64) << 16) | (code[3:].astype(np.int64) << 24))
        disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
        pos = np.arange(len(disp), dtype=np.int64) + va
        for tail in (0, 1, 4):  # disp32 then nothing, an imm8 or an imm32
            into = pos + 4 + tail + disp - target
            for o in np.nonzero((into >= 0) & (into <= slack))[0]:
                end = va + int(o) + 4 + tail
                for back in range(2, 8):
                    start = va + int(o) - back
                    ins = next(_md.disasm(sm.read(start, 16), start, 1), None)
                    if ins and ins.address + ins.size == end and 'rip' in ins.op_str:
                        found.append((start, int(into[o])))
                        break
    return found


def main():
    p = argparse.ArgumentParser()
    p.add_argument('rvas', nargs='+', type=lambda s: int(s, 16))
    p.add_argument('--slack', type=lambda s: int(s, 0), default=0)
    a = p.parse_args()
    sm, mm = Image(SM_EXE), Image(MM_EXE)
    fz = Fuzzy(sm, mm, json.loads((ROOT / 'build' / 'port_map.json').read_text()))
    for target in a.rvas:
        votes = Counter()
        for at, into in references(sm, target, a.slack):
            f = sm.function_at(at)
            if not f:
                continue
            res = fz.pair(f[0])
            if res[0] is None:
                continue
            if len(res) < 4 or res[2] is None:
                b_at = res[0] + at - f[0]
            else:
                ia, ib, m = res[2]
                k = next((n for n, x in enumerate(ia) if x[0] == at), None)
                b_at = None
                for tag, i1, i2, j1, j2 in m.get_opcodes():
                    if tag == 'equal' and k is not None and i1 <= k < i2:
                        b_at = ib[j1 + k - i1][0]
            if b_at is None:
                continue
            ins = next(_md.disasm(mm.read(b_at, 16), b_at, 1), None)
            if ins and 'rip + ' in ins.op_str or (ins and 'rip - ' in ins.op_str):
                part = ins.op_str.split('rip ')[1].split(']')[0]
                disp = int(part.replace(' ', ''), 16)
                votes[ins.address + ins.size + disp - into] += 1
        print(f'{target:#x}: ' + (', '.join(f'{t:#x} x{n}' for t, n in votes.most_common(3)) or 'no references found'))


if __name__ == '__main__':
    main()

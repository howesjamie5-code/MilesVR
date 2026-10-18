"""Call-site votes for functions port_map.py could not pair by their own bodies.

Two exactly paired functions make their calls in the same order, so the k-th call target of one is
the k-th of the other, whatever became of the callee's own code. Each such pair of targets is a
vote; a changed function called from a few paired callers gets a few agreeing votes.

    python -I tools/port_calls.py 0x1873470 0x189c710 ...      (Spider-Man.exe RVAs)
"""
import json, pathlib, sys
from collections import Counter, defaultdict

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from port_map import Image, SM_EXE, MM_EXE, fingerprint  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent


def votes(wanted):
    sm, mm = Image(SM_EXE), Image(MM_EXE)
    funcs = {int(k, 16): int(v, 16) for k, (v, _) in
             json.loads((ROOT / 'build' / 'port_map.json').read_text())['functions'].items()}
    wanted = set(wanted)
    # Callers of the wanted functions: a scan for E8 rel32 calls landing on them.
    callers, fuzzy_callers = defaultdict(set), defaultdict(set)
    for s in sm.sections:
        if not s['exec']:
            continue
        d, raw = sm.data, s['raw']
        end = raw + s['rsize'] - 5
        i = d.find(b'\xe8', raw, end)
        while i != -1:
            target = s['va'] + i - raw + 5 + int.from_bytes(d[i + 1:i + 5], 'little', signed=True)
            if target in wanted:
                f = sm.function_at(s['va'] + i - raw)
                if f:
                    (callers if f[0] in funcs else fuzzy_callers)[target].add(f[0])
            i = d.find(b'\xe8', i + 1, end)
    out = {}
    fz = None
    for w in wanted:
        c = Counter()
        # Callers that changed too: their aligned call instructions.
        for caller in fuzzy_callers[w]:
            if fz is None:
                from port_fuzzy import Fuzzy
                fz = Fuzzy(sm, mm, json.loads((ROOT / 'build' / 'port_map.json').read_text()))
            res = fz.pair(caller)
            if res[0] is None or len(res) < 4 or res[2] is None or res[1] < 0.7:
                continue
            a, b, m = res[2]
            for tag, i1, i2, j1, j2 in m.get_opcodes():
                if tag == 'equal':
                    for k in range(i2 - i1):
                        ra, rb = a[i1 + k][3], b[j1 + k][3]
                        if ra and rb and ra == ('c', w):
                            c[rb[1]] += 1
        for caller in callers[w]:
            fa = sm.function_at(caller)
            fb = mm.function_at(funcs[caller])
            if not fa or not fb:
                continue
            ra, rb = fingerprint(sm, *fa)[1], fingerprint(mm, *fb)[1]
            ca = [(o, t) for k, o, t in ra if k in 'cj']
            cb = [(o, t) for k, o, t in rb if k in 'cj']
            if len(ca) != len(cb):
                continue
            for (oa, ta), (ob, tb) in zip(ca, cb):
                if ta == w:
                    c[tb] += 1
        out[w] = c
    return out


if __name__ == '__main__':
    for w, c in votes(int(x, 16) for x in sys.argv[1:]).items():
        print(f'{w:#x}: ' + (', '.join(f'{t:#x} x{n}' for t, n in c.most_common(3)) or 'no paired callers'))

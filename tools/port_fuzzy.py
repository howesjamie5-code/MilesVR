"""Second pass after port_map.py, for the addresses Spidy uses that it could not pair: functions
whose code changed in MilesMorales.exe, the instructions inside them, and the globals only they
reach.

A changed function still sits between the same neighbours: the nearest paired functions before
and after it bracket a window in MilesMorales.exe, and the candidate there whose instruction
sequence (mnemonics and operand shapes, addresses masked) is most alike is its twin. The two
instruction lists are then aligned, so an address inside the function (a call site, a return
address) maps to the aligned instruction, and RIP-relative addresses in aligned instructions pair
the globals. A global still unpaired takes the offset of the nearest paired global within 4 KB
whose neighbours agree.

    python -I tools/port_fuzzy.py [--audit build/port_audit.csv] [--out build/port_fuzzy.json]

Every result carries a score (1.0: same instruction sequence) and the instruction differences, so
a weak match is visible before it is used.
"""
import argparse, bisect, csv, difflib, json, pathlib, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from port_map import Image, SM_EXE, MM_EXE, _md  # noqa: E402
from capstone import x86  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent


def instructions(img, begin, end):
    code = img.read(begin, end - begin) or b''
    out = []
    for ins in _md.disasm(code, begin):
        shape = []
        ref = None
        for op in ins.operands:
            if op.type == x86.X86_OP_REG:
                shape.append(ins.reg_name(op.reg))
            elif op.type == x86.X86_OP_IMM:
                if ins.group(1) or ins.group(2) or ins.group(7):  # jump, call, relative branch
                    shape.append('rel')
                    ref = ('c', op.imm)
                else:
                    shape.append(hex(op.imm))
            elif op.type == x86.X86_OP_MEM:
                if op.mem.base == x86.X86_REG_RIP:
                    shape.append('[rip]')
                    ref = ('d', ins.address + ins.size + op.mem.disp)
                else:
                    shape.append(f'[{ins.reg_name(op.mem.base) if op.mem.base else ""}'
                                 f'+{ins.reg_name(op.mem.index) if op.mem.index else ""}*{op.mem.scale}'
                                 f'+{op.mem.disp:#x}]')
        out.append((ins.address, ins.size, f'{ins.mnemonic} {",".join(shape)}', ref))
    return out


class Fuzzy:
    def __init__(self, sm, mm, mapping):
        self.sm, self.mm = sm, mm
        self.funcs = {int(k, 16): int(v, 16) for k, (v, _) in mapping['functions'].items()}
        self.data = {int(k, 16): int(v, 16) for k, v in mapping['data'].items()}
        for k, (v, _) in mapping.get('vtables', {}).items():
            self.data[int(k, 16)] = int(v, 16)
        self.taken = set(self.funcs.values())
        self.anchors = sorted(self.funcs)
        self.data_keys = sorted(self.data)
        self.pairs = {}  # SM function start -> (MM start, score, alignment)

    def window(self, rva):
        """MilesMorales.exe range that should hold the twin of the function at `rva`."""
        i = bisect.bisect_left(self.anchors, rva)
        before = [self.anchors[j] for j in range(max(0, i - 8), i)]
        after = [self.anchors[j] for j in range(i, min(len(self.anchors), i + 8)) if self.anchors[j] != rva]
        # The offsets nearby paired functions moved by, taken as a consensus.
        lo_offs = sorted(self.funcs[a] - a for a in before) or [0]
        hi_offs = sorted(self.funcs[b] - b for b in after) or [0]
        lo = rva + lo_offs[len(lo_offs) // 2] - 0x4000
        hi = rva + hi_offs[len(hi_offs) // 2] + 0x4000
        if hi < lo:
            lo, hi = hi, lo
        return lo, hi

    def pair(self, start):
        if start in self.funcs:
            return self.funcs[start], 1.0, None
        if start in self.pairs:
            return self.pairs[start]
        f = self.sm.function_at(start)
        end = f[1] if f and f[0] == start else start + 0x200
        a = instructions(self.sm, start, end)
        sa = [s for _, _, s, _ in a]
        lo, hi = self.window(start)
        i = bisect.bisect_left(self.mm.starts, lo)
        best = (0.0, None, None)
        second = 0.0
        while i < len(self.mm.starts) and self.mm.starts[i] < hi:
            b0, b1 = self.mm.functions[i]
            i += 1
            if b0 in self.taken or not (0.5 < (b1 - b0) / max(1, end - start) < 2.0):
                continue
            b = instructions(self.mm, b0, b1)
            m = difflib.SequenceMatcher(None, sa, [s for _, _, s, _ in b], autojunk=False)
            if m.real_quick_ratio() < max(0.5, second) or m.quick_ratio() < max(0.5, second):
                continue
            r = m.ratio()
            if r > best[0]:
                second, best = best[0], (r, b0, (a, b, m))
            elif r > second:
                second = r
        result = (best[1], best[0], best[2], second)
        self.pairs[start] = result
        return result

    def inside(self, rva):
        f = self.sm.function_at(rva)
        if not f:
            return None
        res = self.pair(f[0])
        target, score = res[0], res[1]
        if target is None:
            return None
        if len(res) < 4 or res[2] is None:  # identical bodies: same offset
            return dict(mm=target + rva - f[0], score=score, function=f[0], how='same body')
        a, b, m = res[2]
        index = next((k for k, (addr, size, _, _) in enumerate(a) if addr <= rva < addr + size), None)
        if index is None:
            return None
        for tag, i1, i2, j1, j2 in m.get_opcodes():
            if i1 <= index < i2:
                if tag == 'equal':
                    addr, size, _, _ = a[index]
                    baddr = b[j1 + index - i1][0]
                    return dict(mm=baddr + rva - addr, score=score, second=res[3], function=f[0],
                                function_mm=target, how='aligned instruction')
                return dict(mm=None, score=score, function=f[0], function_mm=target,
                            how=f'instruction {tag}d in MilesMorales.exe')
        return None

    def data_from_pairs(self):
        """Globals that the aligned instructions of fuzzy pairs reach."""
        found = {}
        for start, res in self.pairs.items():
            if res[0] is None or res[2] is None or res[1] < 0.8:
                continue
            a, b, m = res[2]
            for tag, i1, i2, j1, j2 in m.get_opcodes():
                if tag != 'equal':
                    continue
                for k in range(i2 - i1):
                    ra, rb = a[i1 + k][3], b[j1 + k][3]
                    if ra and rb and ra[0] == rb[0] == 'd':
                        found.setdefault(ra[1], set()).add(rb[1])
        return {k: next(iter(v)) for k, v in found.items() if len(v) == 1}

    def nearby_data(self, rva):
        i = bisect.bisect_right(self.data_keys, rva)
        votes = {}
        for j in range(max(0, i - 6), min(len(self.data_keys), i + 6)):
            k = self.data_keys[j]
            if abs(k - rva) <= 0x1000:
                votes[self.data[k] - k] = votes.get(self.data[k] - k, 0) + 1
        if not votes:
            return None
        off, n = max(votes.items(), key=lambda x: x[1])
        return dict(mm=rva + off, votes=n, of=sum(votes.values()), how='nearby globals')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--map', default=str(ROOT / 'build' / 'port_map.json'))
    p.add_argument('--audit', default=str(ROOT / 'build' / 'port_audit.csv'))
    p.add_argument('--out', default=str(ROOT / 'build' / 'port_fuzzy.json'))
    a = p.parse_args()
    sm, mm = Image(SM_EXE), Image(MM_EXE)
    fz = Fuzzy(sm, mm, json.loads(pathlib.Path(a.map).read_text()))
    wanted = sorted({int(r['sm'], 16) for r in csv.DictReader(open(a.audit, encoding='utf-8'))
                     if r['kind'] == 'unmapped'})
    results = {}
    code = [r for r in wanted if sm.is_code(r)]
    for r in code:
        res = fz.inside(r)
        results[r] = res or dict(mm=None, how='no function or no candidate')
        print(f'{r:#x} -> {res}', file=sys.stderr)
    extra = fz.data_from_pairs()
    for r in wanted:
        if r in results:
            continue
        if r in extra:
            results[r] = dict(mm=extra[r], how='aligned reference in a fuzzy pair')
        else:
            results[r] = fz.nearby_data(r) or dict(mm=None, how='no nearby global')
    out = {f'{k:#x}': {**v, **({'mm': f"{v['mm']:#x}"} if v.get('mm') is not None else {}),
                       **({'function': f"{v['function']:#x}"} if 'function' in v else {}),
                       **({'function_mm': f"{v['function_mm']:#x}"} if 'function_mm' in v else {})}
           for k, v in results.items()}
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1))
    print(f'wrote {a.out}', file=sys.stderr)


if __name__ == '__main__':
    main()

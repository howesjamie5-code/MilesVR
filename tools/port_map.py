"""Maps addresses in Marvel's Spider-Man Remastered (Spider-Man.exe 4.630.0.0) to the same code
and data in Marvel's Spider-Man: Miles Morales (MilesMorales.exe 4.630.0.0).

Both games are the same engine build, so most functions compile to the same instructions and
differ only in where they and their targets sit. Every function the exception table (.pdata)
lists is fingerprinted with its relative branch and RIP-relative fields masked out; a fingerprint
that is unique in both images pairs the two functions. A paired function then pairs everything it
references: the k-th call target and the k-th RIP-relative address in one is the k-th in the
other, which pairs more functions (ones whose fingerprint alone was ambiguous) and the globals
and vtables they use. That repeats until nothing new is learned.

    python -I tools/port_map.py [--sm Spider-Man.exe] [--mm MilesMorales.exe] [--out build/port_map.json]

The JSON maps each Spider-Man.exe RVA (hex) to its MilesMorales.exe RVA, for functions and data;
`lookup(rva)` in this module maps any code RVA inside a paired function by its offset.
"""
import argparse, hashlib, json, pathlib, struct, sys, time
from collections import defaultdict

import capstone
from capstone import x86

STEAM = pathlib.Path(r"C:\Program Files (x86)\Steam\steamapps\common")
SM_EXE = STEAM / "Marvel's Spider-Man Remastered" / "Spider-Man.exe"
MM_EXE = STEAM / "Marvel's Spider-Man Miles Morales" / "MilesMorales.exe"


class Image:
    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.data = self.path.read_bytes()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsize = struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        self.image_base = struct.unpack_from('<Q', d, opt + 24)[0]
        self.image_size = struct.unpack_from('<I', d, opt + 56)[0]
        dirs = opt + 112
        exc_rva, exc_size = struct.unpack_from('<II', d, dirs + 3 * 8)
        self.sections = []
        for i in range(nsec):
            o = opt + optsize + 40 * i
            name = d[o:o + 8].rstrip(b'\0').decode(errors='replace')
            vsize, va, rsize, raw = struct.unpack_from('<IIII', d, o + 8)
            flags = struct.unpack_from('<I', d, o + 36)[0]
            self.sections.append(dict(name=name, va=va, vsize=vsize, raw=raw, rsize=rsize,
                                      exec=bool(flags & 0x20000000)))
        self.functions = []  # (begin, end), sorted, from .pdata
        seen = set()
        off = self.offset(exc_rva)
        for i in range(exc_size // 12):
            b, e, _ = struct.unpack_from('<III', d, off + 12 * i)
            if b and e > b and b not in seen and self.is_code(b):
                seen.add(b)
                self.functions.append((b, e))
        self.functions.sort()
        self.starts = [b for b, _ in self.functions]

    def section(self, rva):
        for s in self.sections:
            if s['va'] <= rva < s['va'] + max(s['vsize'], s['rsize']):
                return s
        return None

    def is_code(self, rva):
        s = self.section(rva)
        return bool(s and s['exec'])

    def offset(self, rva):
        s = self.section(rva)
        if not s or rva - s['va'] >= s['rsize']:
            return None
        return s['raw'] + rva - s['va']

    def read(self, rva, n):
        o = self.offset(rva)
        return None if o is None else self.data[o:o + n]

    def function_at(self, rva):
        import bisect
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and self.functions[i][0] <= rva < self.functions[i][1]:
            return self.functions[i]
        return None


_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
_md.detail = True
_BRANCH = {capstone.CS_GRP_JUMP, capstone.CS_GRP_CALL}


def fingerprint(img, begin, end):
    """(hash of the masked bytes, [(kind, offset in function, target RVA)], instruction count)."""
    code = img.read(begin, end - begin)
    if not code:
        return None
    masked = bytearray(code)
    refs = []
    count = 0
    pos = 0
    for ins in _md.disasm(code, begin):
        count += 1
        pos = ins.address - begin + ins.size
        start = ins.address - begin
        rel = None
        if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL) or ins.group(capstone.CS_GRP_BRANCH_RELATIVE):
            for op in ins.operands:
                if op.type == x86.X86_OP_IMM and ins.imm_size:
                    masked[start + ins.imm_offset:start + ins.imm_offset + ins.imm_size] = b'\0' * ins.imm_size
                    target = op.imm
                    if not (begin <= target < end):  # jumps inside the function stay in the hash
                        rel = ('c' if ins.group(capstone.CS_GRP_CALL) else 'j', target)
                    else:
                        masked[start + ins.imm_offset:start + ins.imm_offset + ins.imm_size] = \
                            code[start + ins.imm_offset:start + ins.imm_offset + ins.imm_size]
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP and ins.disp_size:
                masked[start + ins.disp_offset:start + ins.disp_offset + ins.disp_size] = b'\0' * ins.disp_size
                rel = ('d', ins.address + ins.size + op.mem.disp)
        if rel:
            refs.append((rel[0], ins.address - begin, rel[1]))
    if pos != len(code):
        # Data or padding capstone could not decode: keep the tail as raw bytes.
        pass
    return hashlib.sha1(bytes(masked)).hexdigest(), refs, count


def index(img, label):
    t = time.time()
    by_hash = defaultdict(list)
    info = {}
    for n, (b, e) in enumerate(img.functions):
        fp = fingerprint(img, b, e)
        if fp:
            by_hash[fp[0]].append(b)
            info[b] = (e - b, fp[1], fp[0])
        if n % 50000 == 0:
            print(f'  {label}: {n}/{len(img.functions)} functions, {time.time() - t:.0f}s', file=sys.stderr)
    return by_hash, info


def rtti_vtables(img):
    """{(class name, offset of the subobject), vtable RVA} from MSVC's RTTI: a vtable's slot -1
    points at a complete object locator (signature 1, its own RVA at +20), which names the class."""
    d, base = img.data, img.image_base
    cols = {}
    for s in img.sections:
        if s['exec'] or s['name'] not in ('.rdata', '.data'):
            continue
        raw, size = s['raw'], min(s['rsize'], s['vsize'] or s['rsize'])
        for o in range(raw, raw + size - 24, 4):
            if d[o] == 1 and d[o + 1] == 0 and d[o + 2] == 0 and d[o + 3] == 0:
                rva = s['va'] + o - raw
                if struct.unpack_from('<I', d, o + 20)[0] == rva:
                    cols[base + rva] = rva
    tables = {}
    for s in img.sections:
        if s['exec'] or s['name'] != '.rdata':
            continue
        raw, size = s['raw'], min(s['rsize'], s['vsize'] or s['rsize'])
        for o in range(raw, raw + size - 16, 8):
            v = struct.unpack_from('<Q', d, o)[0]
            col = cols.get(v)
            if col is None:
                continue
            co = img.offset(col)
            subobject, descriptor = struct.unpack_from('<I', d, co + 4)[0], struct.unpack_from('<I', d, co + 12)[0]
            no = img.offset(descriptor + 16)
            if no is None:
                continue
            name = d[no:d.index(b'\0', no, no + 1024)].decode('ascii', 'replace')
            key = (name, subobject)
            tables[key] = None if key in tables else s['va'] + o - raw + 8  # None: ambiguous
    return {k: v for k, v in tables.items() if v is not None}


def build(sm, mm):
    print(f'Spider-Man.exe: {len(sm.functions)} functions; MilesMorales.exe: {len(mm.functions)}', file=sys.stderr)
    sm_hash, sm_info = index(sm, 'Spider-Man.exe')
    mm_hash, mm_info = index(mm, 'MilesMorales.exe')
    funcs, data = {}, {}
    taken = set()
    for h, sms in sm_hash.items():
        mms = mm_hash.get(h)
        if len(sms) == 1 and mms and len(mms) == 1:
            funcs[sms[0]] = mms[0]
            taken.add(mms[0])
    print(f'unique fingerprints: {len(funcs)} functions', file=sys.stderr)

    conflicts = set()
    queue = list(funcs.items())
    while queue:
        a, b = queue.pop()
        ia, ib = sm_info.get(a), mm_info.get(b)
        if not ia or not ib or ia[2] != ib[2] or len(ia[1]) != len(ib[1]):
            continue
        for (ka, oa, ta), (kb, ob, tb) in zip(ia[1], ib[1]):
            if ka != kb or oa != ob:
                break
            if ka in 'cj' and ta in sm_info and tb in mm_info:
                if ta in funcs:
                    if funcs[ta] != tb:
                        conflicts.add(ta)
                    continue
                if tb in taken or sm_info[ta][2] != mm_info[tb][2]:
                    continue
                funcs[ta] = tb
                taken.add(tb)
                queue.append((ta, tb))
            else:
                if ta in data and data[ta] != tb:
                    conflicts.add(ta)
                data.setdefault(ta, tb)
    for c in conflicts:
        funcs.pop(c, None)
        data.pop(c, None)
    print(f'after propagation: {len(funcs)} functions, {len(data)} data, {len(conflicts)} dropped as conflicting',
          file=sys.stderr)
    return funcs, data


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--sm', default=str(SM_EXE))
    p.add_argument('--mm', default=str(MM_EXE))
    p.add_argument('--out', default=str(pathlib.Path(__file__).resolve().parent.parent / 'build' / 'port_map.json'))
    a = p.parse_args()
    sm, mm = Image(a.sm), Image(a.mm)
    funcs, data = build(sm, mm)
    sm_vt, mm_vt = rtti_vtables(sm), rtti_vtables(mm)
    vtables = {}
    for key, rva in sm_vt.items():
        if key in mm_vt:
            vtables[rva] = (mm_vt[key], key[0])
            # A vtable's slots pair the virtual functions, whatever their bodies.
            for i in range(512):
                a_slot, b_slot = sm.read(rva + 8 * i, 8), mm.read(mm_vt[key] + 8 * i, 8)
                if not a_slot or not b_slot:
                    break
                fa = struct.unpack('<Q', a_slot)[0] - sm.image_base
                fb = struct.unpack('<Q', b_slot)[0] - mm.image_base
                if not (sm.is_code(fa) and mm.is_code(fb)):
                    break
                funcs.setdefault(fa, fb)
    print(f'RTTI: {len(sm_vt)} / {len(mm_vt)} vtables, {len(vtables)} paired by class name', file=sys.stderr)
    out = pathlib.Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(dict(
        source=dict(path=str(sm.path), image_size=sm.image_size),
        target=dict(path=str(mm.path), image_size=mm.image_size),
        # [MilesMorales.exe RVA, length in bytes (0: a leaf the exception table does not list)]
        functions={f'{k:#x}': [f'{v:#x}', (sm.function_at(k) or (k, k))[1] - k] for k, v in sorted(funcs.items())},
        data={f'{k:#x}': f'{v:#x}' for k, v in sorted(data.items())},
        vtables={f'{k:#x}': [f'{v:#x}', name] for k, (v, name) in sorted(vtables.items())},
    ), indent=0))
    print(f'wrote {out}', file=sys.stderr)


if __name__ == '__main__':
    main()

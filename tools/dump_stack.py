"""The faulting thread's stack in a minidump, as return addresses into known modules.

No symbols: every qword on the stack that points into a loaded module's image is listed with its
module and offset, nearest the top first. Many are stale, but the game's and Spidy's own frames
show which code reached the fault. With --function, MilesMorales.exe offsets are also named by
the Spider-Man.exe function they were ported from (build/port_map.json).

    python -I tools/dump_stack.py <path.dmp> [--depth 0x3000]
"""
import argparse, bisect, json, pathlib, struct, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
THREAD_LIST, MODULE_LIST, MEMORY_LIST, EXCEPTION, MEMORY64_LIST = 3, 4, 5, 6, 9


def streams(blob):
    count, rva = struct.unpack_from('<II', blob, 8)
    out = {}
    for i in range(count):
        kind, size, at = struct.unpack_from('<III', blob, rva + 12 * i)
        out[kind] = (at, size)
    return out


def utf16(blob, rva):
    n = struct.unpack_from('<I', blob, rva)[0]
    return blob[rva + 4:rva + 4 + n].decode('utf-16-le', 'replace')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('dump')
    p.add_argument('--depth', type=lambda s: int(s, 0), default=0x3000)
    a = p.parse_args()
    blob = pathlib.Path(a.dump).read_bytes()
    s = streams(blob)

    at, _ = s[MODULE_LIST]
    modules = []
    for i in range(struct.unpack_from('<I', blob, at)[0]):
        o = at + 4 + 108 * i
        base, size = struct.unpack_from('<QI', blob, o)
        name = utf16(blob, struct.unpack_from('<I', blob, o + 20)[0]).rsplit('\\', 1)[-1]
        modules.append((base, size, name))
    modules.sort()
    starts = [m[0] for m in modules]

    def module_of(address):
        i = bisect.bisect_right(starts, address) - 1
        if i >= 0 and address < modules[i][0] + modules[i][1]:
            return modules[i]
        return None

    at, _ = s[EXCEPTION]
    thread_id = struct.unpack_from('<I', blob, at)[0]
    ctx_size, ctx_rva = struct.unpack_from('<II', blob, at + 8 + 152)
    rsp = struct.unpack_from('<Q', blob, ctx_rva + 0x98)[0]
    rip = struct.unpack_from('<Q', blob, ctx_rva + 0xf8)[0]
    print(f'thread {thread_id}: rip {rip:#x} rsp {rsp:#x}')

    # Memory ranges in the dump.
    ranges = []
    if MEMORY64_LIST in s:
        at, _ = s[MEMORY64_LIST]
        n, base_rva = struct.unpack_from('<QQ', blob, at)
        o = base_rva
        for i in range(n):
            start, size = struct.unpack_from('<QQ', blob, at + 16 + 16 * i)
            ranges.append((start, size, o))
            o += size
    if MEMORY_LIST in s:
        at, _ = s[MEMORY_LIST]
        for i in range(struct.unpack_from('<I', blob, at)[0]):
            start, size, rva = struct.unpack_from('<QII', blob, at + 4 + 16 * i)
            ranges.append((start, size, rva))

    def read(address, n):
        for start, size, rva in ranges:
            if start <= address and address + n <= start + size:
                return blob[rva + address - start:rva + address - start + n]
        return None

    ported = {}
    mp = ROOT / 'build' / 'port_map.json'
    if mp.exists():
        m = json.loads(mp.read_text())
        ported = sorted((int(v, 16), int(k, 16)) for k, (v, _) in m['functions'].items())
    pkeys = [x for x, _ in ported]

    shown = 0
    for off in range(0, a.depth, 8):
        raw = read(rsp + off, 8)
        if raw is None:
            if off == 0:
                print('the stack is not in this dump')
            break
        v = struct.unpack('<Q', raw)[0]
        mod = module_of(v)
        if not mod or mod[2].lower().startswith(('ntdll', 'kernel', 'ucrtbase', 'msvcp', 'vcruntime')):
            continue
        rva = v - mod[0]
        note = ''
        if mod[2].lower() == 'milesmorales.exe' and pkeys:
            i = bisect.bisect_right(pkeys, rva) - 1
            if i >= 0:
                note = f'   (in the function from Spider-Man.exe {ported[i][1] + rva - ported[i][0]:#x}?' \
                       f' nearest paired start {ported[i][0]:#x})'
        print(f'  rsp+{off:#06x}  {mod[2]}+{rva:#x}{note}')
        shown += 1
        if shown >= 4000:
            break


if __name__ == '__main__':
    sys.exit(main())

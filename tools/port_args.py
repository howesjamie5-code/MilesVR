"""Hooked or called game functions whose arguments changed between Spider-Man.exe and
MilesMorales.exe.

Spidy's detours and calls are written for Spider-Man.exe's signatures. A Miles Morales function
that reads an argument register its Spider-Man.exe original did not (a new flag in rdx, say) gets
whatever the caller left there - the view update's new rdx flag crashed 0.1.0 and 0.1.1. For every
address in the sources that is a function start in both games, this compares the integer argument
registers (rcx, rdx, r8, r9) each version reads before writing them in its first block.

The Spider-Man.exe address of each literal comes from the first commit (the unported import):
lines the port changed only in their literals pair up literal by literal.

    python -I tools/port_args.py
"""
import pathlib, re, subprocess, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from port_map import Image, SM_EXE, MM_EXE, _md  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASE_COMMIT = 'a21cba3'
HEX = re.compile(r'(?<![\w.])0x([0-9a-fA-F]{5,8})\b')
ARGS = {}
for n, regs in enumerate((('rcx', 'ecx', 'cx', 'cl'), ('rdx', 'edx', 'dx', 'dl'),
                          ('r8', 'r8d', 'r8w', 'r8b'), ('r9', 'r9d', 'r9w', 'r9b'))):
    for r in regs:
        ARGS[r] = n


def used_args(img, rva):
    f = img.function_at(rva)
    end = f[1] if f and f[0] == rva else rva + 0x300
    written, used = set(), set()
    for ins in _md.disasm(img.read(rva, min(end - rva, 0x400)) or b'', rva):
        reads, writes = ins.regs_access()
        for x in reads:
            name = ins.reg_name(x)
            if name in ARGS and ARGS[name] not in written:
                used.add(ARGS[name])
        for x in writes:
            name = ins.reg_name(x)
            if name in ARGS:
                written.add(ARGS[name])
        if ins.mnemonic in ('ret', 'jmp') or ins.mnemonic.startswith('j') or ins.mnemonic == 'call':
            break  # the first block only: past a branch or call, registers mean other things
    return sorted(used)


def pairs():
    """{(file, MilesMorales.exe RVA): Spider-Man.exe RVA} from lines changed only in literals."""
    out = {}
    for path in sorted((ROOT / 'src').glob('*.cpp')):
        rel = path.relative_to(ROOT).as_posix()
        old = subprocess.run(['git', '-C', str(ROOT), 'show', f'{BASE_COMMIT}:{rel}'], capture_output=True,
                             text=True, encoding='utf-8').stdout.splitlines()
        new = path.read_text(encoding='utf-8').splitlines()
        old_shapes = {}
        for line in old:
            old_shapes.setdefault(HEX.sub('0x?', line), []).append(line)
        for line in new:
            for candidate in old_shapes.get(HEX.sub('0x?', line), [])[:1]:
                for a, b in zip(HEX.findall(candidate), HEX.findall(line)):
                    if a != b:
                        out[(path.name, int(b, 16))] = int(a, 16)
    return out


def main():
    sm, mm = Image(SM_EXE), Image(MM_EXE)
    flagged = 0
    seen = set()
    for (name, mm_rva), sm_rva in sorted(pairs().items(), key=lambda x: x[0][1]):
        f_sm, f_mm = sm.function_at(sm_rva), mm.function_at(mm_rva)
        if not (f_sm and f_mm and f_sm[0] == sm_rva and f_mm[0] == mm_rva) or mm_rva in seen:
            continue
        seen.add(mm_rva)
        a, b = used_args(sm, sm_rva), used_args(mm, mm_rva)
        if set(b) - set(a):
            flagged += 1
            print(f'{name:24} {sm_rva:#x} -> {mm_rva:#x}: Spider-Man.exe reads args {a}, MilesMorales.exe {b}')
    print(f'{len(seen)} function starts compared, {flagged} read arguments their original did not')


if __name__ == '__main__':
    main()

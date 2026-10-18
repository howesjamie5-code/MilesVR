"""Rewrites the Spider-Man.exe addresses in Spidy's sources as MilesMorales.exe addresses, from
build/port_map.json (exact pairs) and build/port_fuzzy.json (changed functions, scored), and
regenerates the byte checks that guard each hook: a brace list of bytes that equals Spider-Man.exe's
bytes at one of the file's addresses becomes MilesMorales.exe's bytes at the paired address.

    python -I tools/port_apply.py [--dry-run] [--min-score 0.8]

What it cannot do safely it leaves alone and lists in build/port_report.md: addresses with no or
weak pairs, byte lists it could not place, and byte checks whose instructions changed (the hook's
assumptions then need a look, not just new bytes).
"""
import argparse, json, pathlib, re, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from port_map import Image, SM_EXE, MM_EXE, fingerprint  # noqa: E402
from port_audit import Map, literals, HEX, SOURCES, SKIP  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
# Values in address range that are not addresses: capacities, masks, flags, option bits.
NOT_ADDRESSES = {0x100000, 0x100001, 0xffffff, 0x1fffff, 0x1000000, 0x800003, 0x800013}
# Address ranges compared against return addresses: ported by hand (see the report).
RANGES = {('src/native_hud.cpp', 0x72a000), ('src/native_hud.cpp', 0x7c0000),
          ('src/native_hud.cpp', 0x1f00000), ('src/native_hud.cpp', 0x1f10980),
          ('tools/inspect_air_velocity.py', 0xa60000), ('tools/inspect_air_velocity.py', 0xa80000)}
BYTES = re.compile(r'\{\s*((?:0x[0-9a-fA-F]{1,2}|0)\s*(?:,\s*(?:0x[0-9a-fA-F]{1,2}|0)\s*){3,})\}')
PYBYTES = re.compile(r'''(["'])((?:[0-9a-fA-F]{2} ){3,}[0-9a-fA-F]{2})\1''')


def resolve(m, fuzzy, value, min_score):
    kind, target = m.lookup(value)
    if target is not None:
        return target, kind, 1.0
    f = fuzzy.get(f'{value:#x}')
    if f and f.get('mm'):
        score = f.get('score', 0.0 if f.get('how') == 'nearby globals' else 1.0)
        if f.get('how') == 'nearby globals':
            score = 0.85 if f.get('votes', 0) >= 3 and f['votes'] == f.get('of') else 0.5
        clear = f.get('second', 0) < score - 0.1
        if score >= min_score and clear:
            return int(f['mm'], 16), f"fuzzy ({f['how']}, {score:.2f})", score
        return None, f"weak ({f['how']}, {score:.2f}, runner-up {f.get('second', 0):.2f})", score
    return None, 'unmapped', 0.0


def same_shape(a, b):
    """True when two byte strings decode to the same instructions apart from relative fields."""
    fa, fb = fingerprint_bytes(a), fingerprint_bytes(b)
    return fa == fb


def fingerprint_bytes(code):
    from port_map import _md
    from capstone import x86
    out = []
    for ins in _md.disasm(code, 0):
        ops = []
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                ops.append('rip')
            elif op.type == x86.X86_OP_IMM and (ins.group(1) or ins.group(2) or ins.group(7)):
                ops.append('rel')
            elif op.type == x86.X86_OP_REG:
                ops.append(ins.reg_name(op.reg))
            elif op.type == x86.X86_OP_IMM:
                ops.append(op.imm)
            else:
                ops.append((op.mem.base, op.mem.index, op.mem.scale, op.mem.disp, op.size))
        out.append((ins.mnemonic, tuple(ops)))
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--min-score', type=float, default=0.8)
    a = p.parse_args()
    sm, mm = Image(SM_EXE), Image(MM_EXE)
    m = Map(ROOT / 'build' / 'port_map.json', None)
    fuzzy = json.loads((ROOT / 'build' / 'port_fuzzy.json').read_text())
    report = {'weak': [], 'unmapped': [], 'ranges': [], 'bytes_changed': [], 'bytes_unplaced': []}
    changed_files = 0
    counts = {}

    for folder in SOURCES:
        for path in sorted((ROOT / folder).rglob('*')):
            if path.suffix not in ('.cpp', '.hpp', '.py', '.h') or path.name in SKIP:
                continue
            rel = path.relative_to(ROOT).as_posix()
            text = path.read_text(encoding='utf-8')
            mapping = {}

            def sub(match):
                value = int(match.group(1), 16)
                if not (0x1000 <= value < m.image_size) or value in NOT_ADDRESSES:
                    return match.group(0)
                line = text.count('\n', 0, match.start()) + 1
                if (rel, value) in RANGES:
                    report['ranges'].append((rel, line, value))
                    return match.group(0)
                target, how, _ = resolve(m, fuzzy, value, a.min_score)
                counts[how.split(' ')[0]] = counts.get(how.split(' ')[0], 0) + 1
                if target is None:
                    report['weak' if how.startswith('weak') else 'unmapped'].append((rel, line, value, how))
                    return match.group(0)
                mapping[value] = target
                digits = match.group(1)
                new = f'{target:x}'
                if digits.isupper():
                    new = new.upper()
                return match.group(0)[:2] + new

            new_text = HEX.sub(sub, text)

            # Byte checks: a list equal to Spider-Man.exe's bytes at one of this file's addresses.
            def bytes_sub(match, python=False):
                raw = match.group(2) if python else match.group(1)
                try:
                    values = bytes(int(x, 16) for x in (raw.split() if python else re.split(r'\s*,\s*', raw.strip())))
                except ValueError:
                    return match.group(0)
                if len(values) < 4:
                    return match.group(0)
                for sm_rva, mm_rva in mapping.items():
                    for delta in (0,):
                        at = sm.read(sm_rva + delta, len(values))
                        if at == values:
                            target = mm.read(mm_rva + delta, len(values))
                            if target == values:
                                return match.group(0)
                            line = new_text.count('\n', 0, match.start()) + 1
                            if not same_shape(values, target):
                                report['bytes_changed'].append((rel, line, sm_rva, values.hex(' '), target.hex(' ')))
                            if python:
                                return f'{match.group(1)}{target.hex(" ")}{match.group(1)}'
                            body = raw
                            parts = re.split(r'(\s*,\s*)', body.strip())
                            items = parts[0::2]
                            seps = parts[1::2]
                            upper = any(c.isupper() for c in body if c not in 'xX')
                            out = []
                            for k, byte in enumerate(target):
                                tok = items[k]
                                s = '0' if tok == '0' and byte == 0 else (f'0x{byte:02X}' if upper else f'0x{byte:02x}')
                                out.append(s + (seps[k] if k < len(seps) else ''))
                            lead = match.group(1)[:len(match.group(1)) - len(match.group(1).lstrip())]
                            trail = match.group(1)[len(match.group(1).rstrip()):]
                            return '{' + lead + ''.join(out) + trail + '}'
                line = new_text.count('\n', 0, match.start()) + 1
                if mapping and len(values) >= 6 and values[0] in (0x40, 0x41, 0x48, 0x4c, 0x55, 0x53, 0x56, 0x57, 0xf3, 0xf2, 0x0f, 0xe8, 0xe9):
                    report['bytes_unplaced'].append((rel, line, values.hex(' ')))
                return match.group(0)

            new_text = BYTES.sub(bytes_sub, new_text)
            if path.suffix == '.py':
                new_text = PYBYTES.sub(lambda mt: bytes_sub(mt, True), new_text)
            if new_text != text:
                changed_files += 1
                if not a.dry_run:
                    path.write_text(new_text, encoding='utf-8', newline='')

    lines = ['# Port report: Spider-Man.exe -> MilesMorales.exe', '',
             f'Files changed: {changed_files}. Literals: {json.dumps(counts)}', '']
    sections = [('weak', 'Weak matches (left unchanged; need a look)'),
                ('unmapped', 'Not mapped (left unchanged)'),
                ('ranges', 'Address ranges to port by hand'),
                ('bytes_changed', 'Byte checks whose instructions changed (hook needs review)'),
                ('bytes_unplaced', 'Byte lists not matched to an address in their file')]
    for key, title in sections:
        lines += [f'## {title} ({len(report[key])})', '']
        for row in report[key]:
            lines.append('- ' + ' | '.join(f'{x:#x}' if isinstance(x, int) and k != 1 else str(x)
                                           for k, x in enumerate(row)))
        lines.append('')
    out = ROOT / 'build' / 'port_report.md'
    out.write_text('\n'.join(lines), encoding='utf-8')
    print('\n'.join(lines[:3]))
    for key, title in sections:
        print(f'{title}: {len(report[key])}')


if __name__ == '__main__':
    main()

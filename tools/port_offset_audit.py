"""Field offsets in Spidy's sources that build/port_offsets.json says moved in MilesMorales.exe.

A displacement is shared by many unrelated structs, so the votes say where to look, not what to
write: each flagged literal needs its struct identified (port_show.py on the function Spidy
mirrors) before it changes.

    python -I tools/port_offset_audit.py [--min-votes 3]
"""
import argparse, json, pathlib, re

ROOT = pathlib.Path(__file__).resolve().parent.parent
LIT = re.compile(r'(?<![\w.])0x([0-9a-fA-F]{2,5})\b')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--min-votes', type=int, default=3)
    a = p.parse_args()
    votes = json.loads((ROOT / 'build' / 'port_offsets.json').read_text())
    rows = []
    for path in sorted((ROOT / 'src').glob('*.cpp')) + sorted((ROOT / 'include' / 'spidy').glob('*.hpp')):
        for n, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
            code = line.split('//', 1)[0]
            for m in LIT.finditer(code):
                v = int(m.group(1), 16)
                if v < 0x80:
                    continue
                vt = votes.get(f'{v:#x}')
                if not vt:
                    continue
                kept = vt.get(f'{v:#x}', 0)
                moved = sorted(((int(k, 16), c) for k, c in vt.items() if int(k, 16) != v), key=lambda x: -x[1])
                if moved and moved[0][1] >= a.min_votes and moved[0][1] > kept:
                    rows.append((path.name, n, v, moved[0][0], moved[0][1], kept, line.strip()[:110]))
    for f, n, v, to, c, kept, text in rows:
        print(f'{f}:{n}  {v:#x} -> {to:#x} ({c} moved / {kept} kept)  | {text}')
    print(f'{len(rows)} offset literals flagged')


if __name__ == '__main__':
    main()

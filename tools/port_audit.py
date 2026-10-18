"""Lists every Spider-Man.exe address literal in the sources and how build/port_map.json maps it to
MilesMorales.exe: a function start, an instruction inside a paired function (by its offset, when
the two bodies are byte-identical after masking), a global, a vtable, or not mapped yet.

    python -I tools/port_audit.py [--map build/port_map.json] [--csv build/port_audit.csv]
"""
import argparse, bisect, csv, json, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEX = re.compile(r'(?<![\w.])0[xX]([0-9a-fA-F]{6,8})\b')
# Bare hex RVAs written in comments and Python strings ("Spider-Man.exe 1d14c89", "at 7a7fb90").
SOURCES = ['src', 'include', 'apps', 'tools']
SKIP = {'port_map.py', 'port_audit.py', 'port_apply.py'}


class Map:
    def __init__(self, path, image_size):
        m = json.loads(pathlib.Path(path).read_text())
        self.image_size = image_size or m['source']['image_size']
        self.funcs = {int(k, 16): (int(v, 16), n) for k, (v, n) in m['functions'].items()}
        self.data = {int(k, 16): int(v, 16) for k, v in m['data'].items()}
        self.vtables = {int(k, 16): (int(v, 16), name) for k, (v, name) in m.get('vtables', {}).items()}
        self.starts = sorted(self.funcs)

    def lookup(self, rva):
        """(kind, MilesMorales.exe RVA or None)"""
        if rva in self.vtables:
            return 'vtable', self.vtables[rva][0]
        if rva in self.funcs:
            return 'function', self.funcs[rva][0]
        if rva in self.data:
            return 'data', self.data[rva]
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0:
            start = self.starts[i]
            target, length = self.funcs[start]
            if rva < start + length:
                return 'inside', target + rva - start
        return 'unmapped', None


def literals(image_size):
    for folder in SOURCES:
        for path in sorted((ROOT / folder).rglob('*')):
            if path.suffix not in ('.cpp', '.hpp', '.py', '.h') or path.name in SKIP:
                continue
            for n, line in enumerate(path.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
                for m in HEX.finditer(line):
                    value = int(m.group(1), 16)
                    if 0x1000 <= value < image_size:
                        yield path.relative_to(ROOT).as_posix(), n, value, line.strip()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--map', default=str(ROOT / 'build' / 'port_map.json'))
    p.add_argument('--csv', default=str(ROOT / 'build' / 'port_audit.csv'))
    a = p.parse_args()
    m = Map(a.map, None)
    counts, rows = {}, []
    for file, line, value, text in literals(m.image_size):
        kind, target = m.lookup(value)
        counts[kind] = counts.get(kind, 0) + 1
        rows.append(dict(file=file, line=line, sm=f'{value:#x}', kind=kind,
                         mm='' if target is None else f'{target:#x}', text=text[:160]))
    with open(a.csv, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    print(json.dumps(counts, indent=1))
    by_file = {}
    for r in rows:
        if r['kind'] == 'unmapped':
            by_file[r['file']] = by_file.get(r['file'], 0) + 1
    for f, c in sorted(by_file.items(), key=lambda x: -x[1]):
        print(f'{c:5}  {f}')


if __name__ == '__main__':
    sys.exit(main())

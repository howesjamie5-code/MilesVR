"""Runs Spidy's byte checks offline against MilesMorales.exe: every `entry(0xRVA, name, ...)`,
`matches(...)` and `signature(...)` with a named byte list, the same calls with the list written
inline (`entry(rva, {0x48, ...})`), and hook table rows `{0xRVA, ..., {bytes}, n}`, with names and
constants read from the same file. A check that fails here fails in the game too (VR stops with
1002 or the feature stays off), so this is the first test of a port.

    python -I tools/port_verify.py [files...]
"""
import pathlib, re, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from port_map import Image, MM_EXE  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
BYTE = r'(?:0x[0-9a-fA-F]{1,2}|0)'
LIST = BYTE + r'(?:\s*,\s*' + BYTE + r')*'
CHECK = r'\b(?:entry|matches|signature)\s*\(\s*(?:base\s*\+\s*)?(0x[0-9a-fA-F]+|\w+)\s*,\s*'
ARRAY = re.compile(r'(?:unsigned char|uint8_t)\s+(\w+)\s*\[\w*\]\s*=\s*\{\s*(' + LIST + r')\s*,?\s*\}')
CALL = re.compile(CHECK + r'(\w+)\s*,\s*(?:sizeof\(\w+\)|(\d+))')
INLINE = re.compile(CHECK + r'\{\s*(' + LIST + r')\s*\}', re.S)
ROW = re.compile(r'\{\s*(0x[0-9a-fA-F]{5,8}|\w+)\s*,[^{}]*?\{\s*(' + LIST + r')\s*\}\s*,\s*(\d+)\s*\}', re.S)
CONST = re.compile(r'\b(\w+)\s*=\s*(0x[0-9a-fA-F]{5,8})\b')


def parse(raw):
    return bytes(int(x, 16) for x in re.split(r'\s*,\s*', raw.strip()))


def main():
    mm = Image(MM_EXE)
    files = [pathlib.Path(f) for f in sys.argv[1:]] or sorted((ROOT / 'src').glob('*.cpp'))
    total = failed = 0
    for path in files:
        text = path.read_text(encoding='utf-8')
        arrays = {m.group(1): parse(m.group(2)) for m in ARRAY.finditer(text)}
        consts = {m.group(1): int(m.group(2), 16) for m in CONST.finditer(text)}

        def rva_of(token):
            return int(token, 16) if token.startswith('0x') else consts.get(token)

        def line_of(m):
            return text.count('\n', 0, m.start()) + 1

        checks = []
        for m in CALL.finditer(text):
            rva, data = rva_of(m.group(1)), arrays.get(m.group(2))
            if rva is not None and data is not None:
                n = int(m.group(3)) if m.group(3) else len(data)
                checks.append((line_of(m), rva, data[:n], m.group(2)))
        for m in INLINE.finditer(text):
            rva = rva_of(m.group(1))
            if rva is not None:
                checks.append((line_of(m), rva, parse(m.group(2)), m.group(1)))
        for m in ROW.finditer(text):
            rva = rva_of(m.group(1))
            if rva is not None:
                checks.append((line_of(m), rva, parse(m.group(2))[:int(m.group(3))], 'row'))
        for line, rva, data, name in checks:
            total += 1
            actual = mm.read(rva, len(data))
            if actual != data:
                failed += 1
                print(f'FAIL {path.name}:{line} {name} @ {rva:#x}\n     want {data.hex(" ")}\n     have {(actual or b"").hex(" ")}')
    print(f'{total - failed}/{total} byte checks pass against {MM_EXE.name}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

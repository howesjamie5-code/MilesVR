"""What a Spider-Man crash dump says: the exception, and the module it happened in.

The game's own crash reporter leaves minidumps in
%APPDATA%\\Insomniac Games\\Marvel's Spider-Man Remastered\\crs\\reports. This reads the two streams that
answer "did Spidy crash the game": the exception record (code and address) and the module list, so the
faulting address can be named. No debugger needed.

    python tools/read_crash_dump.py                  # the newest dump
    python tools/read_crash_dump.py <path.dmp>
"""
import os
import pathlib
import struct
import sys

REPORTS = pathlib.Path(os.environ['APPDATA'])/"Insomniac Games/Marvel's Spider-Man Remastered/crs/reports"
EXCEPTIONS = {
    0xC0000005: 'access violation', 0xC000001D: 'illegal instruction', 0xC0000025: 'noncontinuable exception',
    0xC0000026: 'invalid disposition', 0xC000008C: 'array bounds exceeded', 0xC0000094: 'integer divide by zero',
    0xC0000096: 'privileged instruction', 0xC00000FD: 'stack overflow', 0xC0000374: 'heap corruption',
    0x80000003: 'breakpoint', 0xE06D7363: 'a C++ exception that nothing caught',
}
THREAD_LIST, MODULE_LIST, EXCEPTION_STREAM = 3, 4, 6


def streams(blob):
    if blob[:4] != b'MDMP':
        raise SystemExit('not a minidump')
    count, rva = struct.unpack_from('<II', blob, 8)
    for i in range(count):
        kind, size, at = struct.unpack_from('<III', blob, rva+i*12)
        yield kind, size, at


def text_at(blob, at):
    """A MINIDUMP_STRING: a byte length then UTF-16."""
    length = struct.unpack_from('<I', blob, at)[0]
    return blob[at+4:at+4+length].decode('utf-16-le', 'replace')


def main():
    if len(sys.argv) > 1:
        path = pathlib.Path(sys.argv[1])
    else:
        dumps = sorted(REPORTS.glob('*.dmp'), key=lambda p: p.stat().st_mtime)
        if not dumps:
            raise SystemExit(f'no dumps in {REPORTS}')
        path = dumps[-1]
    blob = path.read_bytes()
    print(f'{path.name}  {path.stat().st_size/1e6:.1f} MB  written {path.stat().st_mtime}')

    modules = []
    exception = None
    for kind, size, at in streams(blob):
        if kind == MODULE_LIST:
            count = struct.unpack_from('<I', blob, at)[0]
            for i in range(count):
                entry = at+4+i*108
                base, length, _, _, name_rva = struct.unpack_from('<QIIII', blob, entry)
                modules.append((base, length, text_at(blob, name_rva)))
        elif kind == EXCEPTION_STREAM:
            thread, _, code, flags, record, address = struct.unpack_from('<IIIIQQ', blob, at)
            parameters = struct.unpack_from('<I', blob, at+8+24)[0]
            info = struct.unpack_from('<15Q', blob, at+8+32)
            exception = dict(thread=thread, code=code, flags=flags, address=address,
                             parameters=info[:min(parameters, 15)])
    if not exception:
        print('no exception stream: the dump was not written for a fault')
        return 0
    code = exception['code']
    print(f"\nexception {code:#010x}  {EXCEPTIONS.get(code, 'unknown')}")
    print(f"at        {exception['address']:#018x}   on thread {exception['thread']}")
    if code == 0xC0000005 and len(exception['parameters']) >= 2:
        how = {0: 'reading', 1: 'writing', 8: 'executing'}.get(exception['parameters'][0], 'touching')
        print(f"          {how} {exception['parameters'][1]:#018x}")

    where = [m for m in modules if m[0] <= exception['address'] < m[0]+m[1]]
    if where:
        base, length, name = where[0]
        short = name.rsplit('\\', 1)[-1]
        print(f"\nin        {short}  (+{exception['address']-base:#x} of {length:#x})")
        print(f"          {name}")
        print('\nSPIDY' if short.lower().startswith('spidy') else '\nNOT Spidy: the fault is in',
              short if not short.lower().startswith('spidy') else "'s own module")
    else:
        print('\nthe faulting address is in no loaded module (a bad call into freed or unmapped memory)')
    spidy = [m for m in modules if m[2].rsplit('\\', 1)[-1].lower().startswith('spidy')]
    print(f'\nSpidy modules loaded: {len(spidy)}')
    for base, length, name in spidy:
        print(f'  {base:#018x} + {length:#010x}  {name.rsplit(chr(92), 1)[-1]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

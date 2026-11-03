"""A game that stopped answering during VR: a minidump of its threads, and where each of them stood.

A player's report from the mission Straw, Meet Camel (October 23) showed the game stopping for good in mid-swing,
the helicopter's gunfire hitting the building beside him: the headset's loop went on, but no eye image, camera
commit or read of the controller came again, and the left eye's render job had begun and never ended. The report
could not say where in that job the game stood. A minidump written while it hangs can: it holds every thread's
registers and stack. run_game_vr.py writes one beside the report the first time the game stops answering, and puts
each thread's code addresses, by module, in the report's `freeze`.

    python tools/game_freeze.py <dump>     # each thread's instruction pointer and the code addresses on its stack
"""
import ctypes as c
from ctypes import wintypes as w
import pathlib
import struct
import sys

from capture_game_state import open_process, close
from vr_launcher import game_windows

THREAD_LIST, MODULE_LIST = 3, 4
# MiniDumpNormal (every thread's stack and registers, the module list) with MiniDumpWithThreadInfo.
DUMP_TYPE = 0x1000
PROCESS_QUERY_INFORMATION, PROCESS_VM_READ, PROCESS_DUP_HANDLE = 0x400, 0x10, 0x40
GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL = 0x40000000, 2, 0x80
INVALID_HANDLE = c.c_void_p(-1).value
# The AMD64 CONTEXT's stack and instruction pointers.
CONTEXT_RSP, CONTEXT_RIP = 0x98, 0xf8
# Code addresses kept from each thread's stack.
STACK_FRAMES = 16


def window_hung(pid):
    """Whether Windows has marked a window of the process as not responding: its message loop has not run for
    five seconds, which a game whose main thread waits forever does not do. A game behind another window, which
    stops rendering and reading the controller too, still answers."""
    user32 = c.WinDLL('user32')
    user32.IsHungAppWindow.argtypes = (w.HWND,)
    user32.IsHungAppWindow.restype = w.BOOL
    return any(user32.IsHungAppWindow(hwnd) for hwnd in game_windows(pid))


def write_dump(pid, path):
    """A minidump of the process at `path`; True once written."""
    kernel32, dbghelp = c.WinDLL('kernel32', use_last_error=True), c.WinDLL('dbghelp', use_last_error=True)
    kernel32.CreateFileW.argtypes = (w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE)
    kernel32.CreateFileW.restype = w.HANDLE
    dbghelp.MiniDumpWriteDump.argtypes = (w.HANDLE, w.DWORD, w.HANDLE, w.DWORD, c.c_void_p, c.c_void_p, c.c_void_p)
    dbghelp.MiniDumpWriteDump.restype = w.BOOL
    process = open_process(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, False, pid)
    if not process:
        return False
    try:
        pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
        file = kernel32.CreateFileW(str(path), GENERIC_WRITE, 0, None, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, None)
        if not file or file == INVALID_HANDLE:
            return False
        try:
            return bool(dbghelp.MiniDumpWriteDump(process, pid, file, DUMP_TYPE, None, None, None))
        finally:
            close(file)
    finally:
        close(process)


def streams(blob):
    if blob[:4] != b'MDMP':
        raise ValueError('not a minidump')
    count, rva = struct.unpack_from('<II', blob, 8)
    for i in range(count):
        yield struct.unpack_from('<III', blob, rva+i*12)


def text_at(blob, at):
    """A MINIDUMP_STRING: its length in bytes, then UTF-16."""
    length = struct.unpack_from('<I', blob, at)[0]
    return blob[at+4:at+4+length].decode('utf-16-le', 'replace')


def modules(blob):
    """(base, size, file name) of each module in the dump."""
    found = []
    for kind, _, at in streams(blob):
        if kind == MODULE_LIST:
            for i in range(struct.unpack_from('<I', blob, at)[0]):
                base, size, _, _, name = struct.unpack_from('<QIIII', blob, at+4+i*108)
                found.append((base, size, text_at(blob, name).rsplit('\\', 1)[-1]))
    return sorted(found)


def named(address, mods):
    """`module+0xoffset` for an address inside a module, else None."""
    for base, size, name in mods:
        if base <= address < base+size:
            return f'{name}+{address-base:#x}'
    return None


def threads(blob, frames=STACK_FRAMES):
    """Each thread's id, instruction pointer and the first `frames` values on its stack, from its stack pointer
    up, that point into a module: its return addresses among them (and some stale ones and function pointers -
    a scan, not an unwind)."""
    mods = modules(blob)
    found = []
    for kind, _, at in streams(blob):
        if kind != THREAD_LIST:
            continue
        for i in range(struct.unpack_from('<I', blob, at)[0]):
            tid, _, _, _, _, stack_at, stack_size, stack_rva, context_size, context_rva = \
                struct.unpack_from('<IIIIQQIIII', blob, at+4+i*48)
            rsp = rip = 0
            if context_size > CONTEXT_RIP+8:
                rsp, = struct.unpack_from('<Q', blob, context_rva+CONTEXT_RSP)
                rip, = struct.unpack_from('<Q', blob, context_rva+CONTEXT_RIP)
            stack = blob[stack_rva:stack_rva+stack_size]
            start = rsp-stack_at if stack_at <= rsp < stack_at+len(stack) else 0
            start -= start % 8
            calls = []
            for offset in range(start, len(stack)-7, 8):
                where = named(struct.unpack_from('<Q', stack, offset)[0], mods)
                if where and (not calls or calls[-1] != where):
                    calls.append(where)
                    if len(calls) >= frames:
                        break
            found.append(dict(id=tid, rip=named(rip, mods) or f'{rip:#x}', stack=calls))
    return found


def capture(pid, path):
    """The report's `freeze`: the dump written at `path` (or why not), and each thread's place."""
    if not write_dump(pid, path):
        return dict(dump=None, error=c.get_last_error())
    blob = pathlib.Path(path).read_bytes()
    return dict(dump=str(path), dump_mb=round(len(blob)/1e6, 1), threads=threads(blob))


def main():
    blob = pathlib.Path(sys.argv[1]).read_bytes()
    for thread in threads(blob):
        print(f"thread {thread['id']:6}  at {thread['rip']}")
        for where in thread['stack']:
            print(f'                from {where}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

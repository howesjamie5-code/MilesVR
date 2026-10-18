"""Time every frame of the running game and every eye pair it hands to the GPU, from outside.

Run beside a probe that renders Spidy's eye views (tools/probe_vr_load.py in another console) or a VR
session. About a thousand times a second for --seconds it reads two counters in the game: the frames
spidy_render_memory.dll has counted and the eye pairs spidy_stereo_probe.dll has staged (SpidyGpuData), and
notes when each moved. A frame rate is the mean of those intervals; stutter is their spread, which the
probes' half-second samples and a session report's 50 ms samples cannot show.

    python tools/probe_frame_pacing.py --seconds 20 --output reports/pacing.json

For each counter: the intervals' median, 95th and 99th percentile and longest, and how many were over
1.5 and 2 times the median (hitches), in all and for each --window seconds. Its own reads are
about a millisecond apart, so an interval is known to a millisecond; `late_reads` counts reads that came
over 3 ms after the last (this script was held up, and an interval then is not the game's).
"""
import argparse
import ctypes as c
import json
import pathlib
import statistics
import struct
import sys
import time
from capture_game_state import Game, find_game
from inspect_game import PE
from observe_game import modules
from probe_render_commands import exports

# Module, export, the counter's offset in it, and the data's magic and size.
COUNTERS = {
    'frames': ('spidy_render_memory.dll', 'SpidyRenderMemoryData', 24, (0x53524d44, 1, 64)),
    'eye_pairs': ('spidy_stereo_probe.dll', 'SpidyGpuData', 56, (0x53475044, 2, 208)),
}


def locate(game):
    """The address of each counter whose module is in the game."""
    loaded = modules(game.pid)
    found = {}
    for name, (module, export, offset, header) in COUNTERS.items():
        matching = [m for m in loaded if m['name'].lower() == module]
        if not matching:
            continue
        address = matching[-1]['base']+exports(PE(pathlib.Path(matching[-1]['path']).read_bytes()))[export]
        if struct.unpack('<3I', game.read(address, 12)) == header:
            found[name] = address+offset
    return found


def summary(intervals):
    """Milliseconds: the intervals' spread and the hitches among them."""
    if len(intervals) < 4:
        return None
    ordered = sorted(intervals)
    median = statistics.median(ordered)

    def at(share):
        return ordered[min(len(ordered)-1, int(share*len(ordered)))]
    return dict(count=len(ordered), per_second=round(1000*len(ordered)/sum(ordered), 1),
                median_ms=round(median, 2), p95_ms=round(at(.95), 2), p99_ms=round(at(.99), 2),
                longest_ms=round(ordered[-1], 2),
                over_1_5_medians=sum(x > 1.5*median for x in ordered),
                over_2_medians=sum(x > 2*median for x in ordered),
                # The part of the time spent in intervals over 1.5 medians.
                hitch_time_percent=round(100*sum(x for x in ordered if x > 1.5*median)/sum(ordered), 2))


def measure(game, addresses, seconds):
    """For each counter, (time in seconds from the start, milliseconds since its last move) per move."""
    kernel = c.WinDLL('kernel32', use_last_error=True)
    read = kernel.ReadProcessMemory
    read.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    value, count = c.c_uint64(), c.c_size_t()
    names = list(addresses)
    last = {name: None for name in names}
    moved = {name: None for name in names}
    events = {name: [] for name in names}
    late = 0
    began = previous = time.perf_counter()
    while True:
        now = time.perf_counter()
        if now-began >= seconds:
            break
        late += now-previous > .003
        previous = now
        for name in names:
            if not read(game.handle, addresses[name], c.byref(value), 8, c.byref(count)):
                raise c.WinError(c.get_last_error())
            if last[name] is not None and value.value != last[name]:
                if moved[name] is not None:
                    # Several moves between two reads share the time evenly.
                    steps = max(1, value.value-last[name])
                    events[name] += [(now-began, 1000*(now-moved[name])/steps)]*steps
                moved[name] = now
            last[name] = value.value
        time.sleep(.0005)
    return events, late


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--seconds', type=float, default=20)
    p.add_argument('--window', type=float, default=5, help='seconds per window in the report')
    p.add_argument('--output', type=pathlib.Path)
    p.add_argument('--intervals', action='store_true', help='keep every interval in the report')
    a = p.parse_args()
    game = Game(find_game())
    try:
        addresses = locate(game)
        if not addresses:
            raise RuntimeError("Neither Spidy's render memory module nor its stereo module is in the game.")
        events, late = measure(game, addresses, a.seconds)
    finally:
        game.close()
    result = dict(seconds=a.seconds, late_reads=late, counters={})
    for name, moves in events.items():
        windows = []
        for index in range(int(a.seconds//a.window)):
            part = [ms for at, ms in moves if index*a.window <= at < (index+1)*a.window]
            windows.append(summary(part))
        result['counters'][name] = dict(all=summary([ms for _, ms in moves]), windows=windows)
        if a.intervals:
            result['counters'][name]['intervals'] = [[round(at, 4), round(ms, 2)] for at, ms in moves]
        print(name, json.dumps(result['counters'][name]['all']), flush=True)
        for index, window in enumerate(windows):
            print(f'  {index*a.window:5.0f} s', json.dumps(window), flush=True)
    print('late reads:', late, flush=True)
    if a.output:
        a.output.parent.mkdir(parents=True, exist_ok=True)
        a.output.write_text(json.dumps(result, indent=1)+'\n')
        print(f'Report: {a.output.resolve()}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f'Frame pacing probe failed: {error}', file=sys.stderr)
        sys.exit(2)

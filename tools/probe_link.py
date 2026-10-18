"""Time the network link the headset's stream runs over: echo requests to an address, many a second.

A PC that streams to the headset over Wi-Fi shares that link's hiccups with the stream: a scan for
networks, a roam or a busy channel holds every packet for tens or hundreds of milliseconds, and the
headset's picture stalls with them whatever the game's frame rate. This pings the access point (or
the headset) every --interval milliseconds and reports the spikes and when they came.

    python tools/probe_link.py 192.168.1.1 --seconds 60 --output reports/link.json

Without an address it pings the default gateway. No administrator rights are needed (IcmpSendEcho).
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import pathlib
import re
import socket
import struct
import subprocess
import sys
import time

iphlpapi = c.WinDLL('iphlpapi', use_last_error=True)
iphlpapi.IcmpCreateFile.restype = w.HANDLE
iphlpapi.IcmpSendEcho.argtypes = [w.HANDLE, w.DWORD, c.c_void_p, w.WORD, c.c_void_p, c.c_void_p, w.DWORD, w.DWORD]
iphlpapi.IcmpSendEcho.restype = w.DWORD
iphlpapi.IcmpCloseHandle.argtypes = [w.HANDLE]


def wifi():
    """The Wi-Fi connection as netsh reports it: SSID, access point, channel, rates and signal."""
    try:
        text = subprocess.run(['netsh', 'wlan', 'show', 'interfaces'], capture_output=True, text=True,
                              timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return {}
    found = {}
    for key, name in (('SSID', 'ssid'), ('AP BSSID', 'access_point'), ('Band', 'band'), ('Channel', 'channel'),
                      ('Radio type', 'radio'), ('Receive rate (Mbps)', 'receive_mbps'),
                      ('Transmit rate (Mbps)', 'transmit_mbps'), ('Signal', 'signal'), ('State', 'state')):
        match = re.search(rf'^\s*{re.escape(key)}\s*:\s*(.+?)\s*$', text, re.M)
        if match:
            found[name] = match.group(1)
    return found


def gateway():
    text = subprocess.run(['ipconfig'], capture_output=True, text=True, timeout=10).stdout
    match = re.search(r'Default Gateway[ .]*:\s*(\d+\.\d+\.\d+\.\d+)', text)
    return match.group(1) if match else None


def summary(times, lost, seconds):
    ordered = sorted(times)

    def at(share):
        return round(ordered[min(len(ordered)-1, int(share*len(ordered)))], 1) if ordered else None
    return dict(replies=len(times), lost=lost, median_ms=at(.5), p99_ms=at(.99),
                longest_ms=round(ordered[-1], 1) if ordered else None,
                over_20_ms=sum(t > 20 for t in times), over_50_ms=sum(t > 50 for t in times),
                over_100_ms=sum(t > 100 for t in times), seconds=round(seconds, 1))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('address', nargs='?')
    p.add_argument('--seconds', type=float, default=60)
    p.add_argument('--interval', type=float, default=25, help='milliseconds between requests')
    p.add_argument('--timeout', type=int, default=1000, help='milliseconds a reply may take')
    p.add_argument('--output', type=pathlib.Path)
    a = p.parse_args()
    address = a.address or gateway()
    if not address:
        p.error('No address given and no default gateway found')
    target = struct.unpack('<I', socket.inet_aton(address))[0]
    handle = iphlpapi.IcmpCreateFile()
    payload = c.create_string_buffer(b'spidy-link-probe', 16)
    reply = c.create_string_buffer(64)
    events, times, lost = [], [], 0
    link = wifi()
    began = time.perf_counter()
    try:
        while True:
            start = time.perf_counter()
            if start-began >= a.seconds:
                break
            count = iphlpapi.IcmpSendEcho(handle, target, payload, 16, None, reply, len(reply), a.timeout)
            elapsed = 1000*(time.perf_counter()-start)
            status = struct.unpack_from('<I', reply.raw, 4)[0] if count else None
            if count and status == 0:
                times.append(elapsed)
                if elapsed > 20:
                    events.append([round(start-began, 2), round(elapsed, 1)])
            else:
                lost += 1
                events.append([round(start-began, 2), None])
            rest = a.interval/1000-(time.perf_counter()-start)
            if rest > 0:
                time.sleep(rest)
    finally:
        iphlpapi.IcmpCloseHandle(handle)
    result = dict(address=address, wifi=link, wifi_after=wifi(), **summary(times, lost, time.perf_counter()-began),
                  spikes=events)
    print(json.dumps({k: v for k, v in result.items() if k != 'spikes'}), flush=True)
    for at, ms in events[:60]:
        print(f'  {at:7.2f} s  ' + ('lost' if ms is None else f'{ms:.1f} ms'), flush=True)
    if a.output:
        a.output.parent.mkdir(parents=True, exist_ok=True)
        a.output.write_text(json.dumps(result, indent=1)+'\n')
        print(f'Report: {a.output.resolve()}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f'Link probe failed: {error}', file=sys.stderr)
        sys.exit(2)

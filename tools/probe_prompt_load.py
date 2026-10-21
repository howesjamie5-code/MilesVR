"""The prompt reading through a save's load and free roam, no headset: does the game live, and what is named?

    python tools/probe_prompt_load.py [--dll PATH] [--seconds 120]
    python tools/close_game.py

Launches the game as `probe_menu_pad.py start` does (with the given stereo module, the tree's by default),
turns the prompt reading on (SpidyMenuStart + SpidyPromptsWatch) as soon as the menu's hooks take, presses A every
2 s until a player exists (the title, CONTINUE), then watches free roam. The watched data is polled every second and
written out each time, so a death leaves everything before it; every second with something named is listed under
`demands`. Until October 20 the reading ended the game right after CONTINUE: its timeline's line for a long help
text overflowed a buffer (game_prompts.cpp, keepLine). The game is left running.
"""
import argparse
import json
import pathlib
import struct
import sys
import time

import probe_menu_pad as pad
import probe_prompt_watch as watch
from observe_game import call_with_payload
from probe_menu import FLAGS, settings_payload

EXPORTS = watch.EXPORTS + ('SpidyPadSample', 'SpidyPlayerFind')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--dll', default=str(pad.XR_DLL), help='the stereo module to load')
    p.add_argument('--seconds', type=float, default=120, help='free roam watched after the load')
    p.add_argument('--output', default=str(pad.ROOT / 'reports/prompt-load.json'))
    a = p.parse_args()
    try:
        pad.user32.SetProcessDpiAwarenessContext(pad.c.c_void_p(-4))
    except AttributeError:
        pad.user32.SetProcessDPIAware()
    pad.XR_DLL = pathlib.Path(a.dll)
    out = dict(dll=str(pad.XR_DLL), events=[], died=False)
    started = time.monotonic()

    def event(text, **more):
        out['events'].append(dict(seconds=round(time.monotonic() - started, 1), event=text, **more))
        print(json.dumps(out['events'][-1]), flush=True)

    def save():
        pathlib.Path(a.output).parent.mkdir(parents=True, exist_ok=True)
        pathlib.Path(a.output).write_text(json.dumps(out, indent=1), encoding='utf-8')

    loaded = {}

    def early(pid):
        loaded.update(pad.enlarge_render_memory(pid))

    game = pad.wait_for_game(prepare=lambda: pad.vr_display.prepare_launch(small=True), early=early,
                             ready=lambda g: True)
    process = pad.open_process(0x0400 | 0x0010 | 0x0020 | 0x0008 | 0x0002, False, game.pid)
    try:
        pad.wait_for_renderer(game, process, loaded.get('SpidyRenderMemoryData'))
        event('renderer', pid=game.pid, in_front=pad.bring_to_front(game.pid))
        bridge, _ = pad.prepare(game.pid, process, names=pad.BRIDGE_EXPORTS)
        code = call_with_payload(process, bridge['SpidyStart'],
                                 struct.pack('<4I3Q', 0x53424346, 1, 40, game.pid, game.base, 0, 0))
        xr, digest = pad.prepare(game.pid, process, pad.XR_DLL, pad.ROOT / 'reports/stereo-modules', EXPORTS)
        out['sha256'] = digest
        event('modules', bridge_start=code)
        flags = FLAGS['web_grab'] | FLAGS['punch'] | FLAGS['body'] | FLAGS['air_webs'] | FLAGS['stand_on_walls']
        watching = False
        last_press = 0.0
        phase, roam_from = 'load', None
        while True:
            if not pad.alive(game):
                out['died'] = True
                event('the game is gone', phase=phase)
                break
            now = time.monotonic()
            if not watching:
                code = call_with_payload(process, xr['SpidyMenuStart'], settings_payload(flags, 30, 100, 1, 32.0))
                if code in (0, 9301, 9302, 9303):
                    code = call_with_payload(process, xr['SpidyPromptsWatch'], struct.pack('<I', 1))
                    watching = code == 0
                    event('watch on' if watching else 'watch refused', code=code)
            if phase == 'load':
                try:
                    if now - last_press >= 2:
                        last_press = now
                        pad.pad(process, xr, pad.BUTTONS['a'], 150)
                    player, _ = pad.find_player(game, process, xr)
                except OSError as error:
                    event('press refused', error=str(error), alive=pad.alive(game))
                    player = None
                if player:
                    phase, roam_from = 'roam', now
                    event('player', player=player)
                elif now - started > 240:
                    event('no player within four minutes')
                    break
            elif now - roam_from >= a.seconds:
                event('watched free roam to the end')
                break
            if watching:
                try:
                    seen = watch.watched(game, process, xr)
                    out['watched'] = seen
                    if seen.get('demand'):
                        out.setdefault('demands', []).append(dict(
                            seconds=round(time.monotonic() - started, 1), demand=hex(seen['demand']),
                            named=seen.get('texts')))
                except OSError as error:
                    event('watched refused', error=str(error), alive=pad.alive(game))
            out['phase'] = phase
            save()
            time.sleep(1)
        save()
    finally:
        pad.close(process)
    w = out.get('watched', {})
    summary = dict(died=out['died'], phase=out.get('phase'), demands=len(out.get('demands', [])),
                   **{k: w.get(k) for k in ('reading', 'invokes', 'calls', 'texts', 'last_demand', 'by_name')})
    print(json.dumps(summary, indent=1), flush=True)
    print(f'report {a.output}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())

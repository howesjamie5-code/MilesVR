"""Finds the MilesMorales.exe twins of the Spider-Man.exe addresses the first-person cutscenes (src/game_fp.cpp) use.

    python -I tools/find_fp_addresses.py [--sm Spider-Man.exe] [--mm MilesMorales.exe]

Needs both games installed (the paths default to Steam's, as in port_map.py) and `pip install capstone`. It
builds build/port_map.json with tools/port_map.py first if that file is not there (a few minutes), then prints
each address: the twin port_map pairs, how sure it is, and for the three the rest of MilesVR already uses,
whether the twin is the one already in the source. Send the whole output back; getJointRva in game_fp.cpp is
the one that is still to be filled in.
"""
import argparse, json, pathlib, subprocess, sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
from port_map import Image, SM_EXE, MM_EXE  # noqa: E402

# Spider-Man.exe code (functions) and data game_fp.cpp used there, and the MilesMorales.exe address MilesVR's own
# source already has for it (None: none yet).
FUNCTIONS = {
    'getJoint (joint matrix by hash)': (0x1775a80, None),
    'DrawOff (hide an entity)': (0x191afb0, 0x1e26790),
    'DrawOn (show an entity)': (0x191b880, 0x1e27060),
}
DATA = {
    'actor records (pool base)': (0x7a44380, 0x83fa000),
    'actor record count': (0x7a4439c, 0x83fa01c),
}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--sm', default=str(SM_EXE))
    p.add_argument('--mm', default=str(MM_EXE))
    a = p.parse_args()
    mapfile = ROOT / 'build' / 'port_map.json'
    if not mapfile.exists():
        print('building build/port_map.json (a few minutes) ...', flush=True)
        subprocess.run([sys.executable, '-I', str(HERE / 'port_map.py'), '--sm', a.sm, '--mm', a.mm,
                        '--out', str(mapfile)], check=True)
    mapping = json.loads(mapfile.read_text())
    from port_fuzzy import Fuzzy  # noqa: E402
    sm, mm = Image(a.sm), Image(a.mm)
    fuzzy = Fuzzy(sm, mm, mapping)
    print()
    for name, (rva, known) in FUNCTIONS.items():
        twin, score = None, 0.0
        try:
            res = fuzzy.pair(rva)
            twin, score = res[0], res[1]
            runner = f', runner-up {res[3]:.3f}' if len(res) > 3 else ''
        except Exception as error:  # noqa: BLE001
            runner = f' (pairing failed: {error})'
        note = ''
        if known is not None:
            note = '  == the MilesVR source' if twin == known else f'  DIFFERS from the MilesVR source ({known:#x})'
        print(f'{name}: Spider-Man.exe {rva:#x} -> MilesMorales.exe '
              f'{twin:#x}' if twin else f'{name}: Spider-Man.exe {rva:#x} -> not found', end='')
        print(f', score {score:.3f}{runner}{note}')
    for name, (rva, known) in DATA.items():
        twin = fuzzy.data.get(rva)
        note = '' if twin is None else ('  == the MilesVR source' if twin == known else f'  DIFFERS ({known:#x})')
        print(f'{name}: Spider-Man.exe {rva:#x} -> ' + (f'MilesMorales.exe {twin:#x}' if twin else 'not in the map') + note)
    print('\nSend all of this back.')


if __name__ == '__main__':
    main()

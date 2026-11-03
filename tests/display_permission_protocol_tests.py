"""Settings access failures must not abort VR startup or lose the user's backup."""
import argparse
import io
import json
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import ANY, Mock, patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import run_game_vr
import vr_display as display
from launcher_protocol_tests import FakeRegistry, USER


class DisplayPermissionTests(unittest.TestCase):
    def test_partial_window_write_rolls_back_or_keeps_backup_for_retry(self):
        for refuse_restore in (False, True):
            with self.subTest(refuse_restore=refuse_restore), tempfile.TemporaryDirectory() as folder:
                registry = FakeRegistry(USER)
                write = registry.write
                denied = PermissionError(13, 'Window change refused')

                def partial_write(values, key=display.KEY):
                    # SetValueEx can succeed for several values before refusing the next one.
                    write(dict(list(values.items())[:4]), key)
                    registry.write = Mock(side_effect=PermissionError(13, 'Restore refused')) if refuse_restore else write
                    raise denied

                registry.write = partial_write
                backup = pathlib.Path(folder)/'before.json'
                with self.assertRaises(PermissionError) as raised:
                    display.prepare_launch(registry=registry, backup=backup, screen=(3440, 1440))
                self.assertIs(raised.exception, denied)
                self.assertEqual(backup.exists(), refuse_restore)
                if refuse_restore:
                    self.assertEqual(json.loads(backup.read_text())['values'], USER)
                    self.assertEqual(registry.values['Fullscreen'], 0)
                    registry.write = write
                    self.assertTrue(display.restore(registry, backup))
                self.assertEqual(registry.values, USER)
                self.assertFalse(backup.exists())

    def test_unreadable_backup_is_set_aside_and_launch_goes_on(self):
        # spazzmatic_1, October 25: an empty backup ended every start with "Expecting value: line 1 column 1".
        for contents in ('', '{"key": "Software', '\x00\x00'):
            with self.subTest(contents=contents), tempfile.TemporaryDirectory() as folder:
                registry = FakeRegistry(USER)
                backup = pathlib.Path(folder)/'before.json'
                backup.write_text(contents)
                with patch('sys.stdout', io.StringIO()) as shown:
                    self.assertEqual(display.session_changes(backup), {})
                    self.assertFalse(display.restore(registry, backup))
                self.assertIn('WARNING:', shown.getvalue())
                self.assertFalse(backup.exists())
                self.assertEqual((pathlib.Path(folder)/'before.unreadable.json').read_text(), contents)
                backup.write_text(contents)
                with patch('sys.stdout', io.StringIO()):
                    display.prepare_launch(registry=registry, backup=backup, screen=(3440, 1440))
                self.assertEqual(json.loads(backup.read_text())['values'], USER)
                self.assertFalse(list(pathlib.Path(folder).glob('*.tmp')))
                self.assertTrue(display.restore(registry, backup))
                self.assertEqual(registry.values, USER)

    def test_startup_continues_after_settings_failure_and_records_it(self):
        class Prepared(Exception):
            pass

        def wait_for_game(**callbacks):
            callbacks['prepare']()
            raise Prepared()  # Stop before launching or attaching to a real game.

        for full_desktop, pending in ((False, False), (False, True), (True, False), (True, True)):
            with self.subTest(full_desktop=full_desktop, pending=pending):
                args = argparse.Namespace(stop_event=None, xr_runtime='auto', render_scale=100, size=0,
                                          auto_launch=True, full_desktop_view=full_desktop,
                                          game_depth_of_field=False, game_sharpening=full_desktop)
                startup = run_game_vr.Startup(pathlib.Path('unused.json'))
                shown = io.StringIO()
                with patch.object(run_game_vr, 'game_running', return_value=False), \
                     patch.object(run_game_vr, 'preflight', return_value=('runtime.json', {'recommended_eye': None})), \
                     patch.object(run_game_vr, 'wait_for_game', side_effect=wait_for_game), \
                     patch.object(display, 'prepare_launch', side_effect=PermissionError(13, 'Access is denied')) as prepare, \
                     patch.object(display, 'pending', return_value=pending), patch('sys.stdout', shown):
                    with self.assertRaises(Prepared):
                        run_game_vr.session(args, startup)
                prepare.assert_called_once_with(small=not full_desktop,
                                                keep=('sharpening',) if full_desktop else (), comfort=True,
                                                tuned=ANY)
                self.assertIn('Access is denied', startup.fields['game_settings_refused'])
                self.assertIn('WARNING:', shown.getvalue())
                self.assertIn('frame generation off', shown.getvalue())
                self.assertEqual('still need restoring' in shown.getvalue(), pending)
                self.assertNotIn('Game settings for VR:', shown.getvalue())

    def test_auto_tune_game_values_hold_for_the_vr_session_and_come_back_after(self):
        with tempfile.TemporaryDirectory() as folder:
            mine = dict(USER, ShadowQuality=3, LevelOfDetail=3, TextureQuality=2)
            registry = FakeRegistry(mine)
            backup = pathlib.Path(folder)/'before.json'
            # HairQuality was never saved by the game: it is not made.
            display.prepare_launch(registry=registry, backup=backup, screen=(3440, 1440),
                                   tuned=dict(ShadowQuality=0, LevelOfDetail=1, HairQuality=1, Fullscreen=0))
            self.assertEqual((registry.values['ShadowQuality'], registry.values['LevelOfDetail']), (0, 1))
            self.assertNotIn('HairQuality', registry.values)
            self.assertEqual(registry.values['TextureQuality'], 2)
            self.assertTrue(display.restore(registry, backup))
            self.assertEqual({k: registry.values[k] for k in ('ShadowQuality', 'LevelOfDetail', 'TextureQuality')},
                             dict(ShadowQuality=3, LevelOfDetail=3, TextureQuality=2))
            self.assertEqual(registry.values['Fullscreen'], 1)

    def test_wrist_tune_reads_the_mode_resolution_and_game_values_from_spidy_vr_ini(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(run_game_vr.wrist_tune(folder), (None, None, {}))
            (pathlib.Path(folder)/'Spidy').mkdir()
            (pathlib.Path(folder)/'Spidy'/'spidy_vr.ini').write_text(
                'tune_mode=1\ntune_render_scale=85\ntune_game_ShadowQuality=1\ntune_game_HairQuality=-1\n'
                'tune_game_Fullscreen=0\n', encoding='utf-8')
            self.assertEqual(run_game_vr.wrist_tune(folder), ('BALANCED', 85, dict(ShadowQuality=1)))


if __name__ == '__main__':
    unittest.main()

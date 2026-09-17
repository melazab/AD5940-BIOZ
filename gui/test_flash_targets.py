"""Run with python3 -m unittest discover -s gui -p 'test_*.py'."""
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import main


class FlashTargetTests(unittest.TestCase):
    def test_linux_enumerates_both_boards_and_partition_serial(self):
        devices = {"blockdevices": [
            {"name": "/dev/sda", "label": "DAPLINK", "serial": "aaaa",
             "mountpoint": "/media/board a"},
            {"name": "/dev/sdb", "serial": "bbbb", "children": [
                {"name": "/dev/sdb1", "label": "DAPLINK", "mountpoint": None}
            ]},
        ]}
        with patch.object(main.sys, "platform", "linux"), patch.object(
            main.subprocess, "run", return_value=SimpleNamespace(stdout=json.dumps(devices))
        ):
            self.assertEqual(main.list_flash_targets(), [
                main.FlashTarget("/dev/sda", "aaaa", "/media/board a"),
                main.FlashTarget("/dev/sdb1", "bbbb"),
            ])

    def test_identity_survives_device_path_change(self):
        old = main.FlashTarget("/dev/sda", "aaaa")
        moved = main.FlashTarget("/dev/sdc", "aaaa")
        with patch.object(main, "list_flash_targets", return_value=[
            main.FlashTarget("/dev/sda", "bbbb"), moved
        ]):
            self.assertEqual(main.resolve_flash_target(old), moved)

    def test_missing_or_ambiguous_identity_never_falls_back(self):
        chosen = main.FlashTarget("/dev/sda", "aaaa")
        for targets in [[], [main.FlashTarget("/dev/sda", "bbbb")], [chosen, chosen]]:
            with self.subTest(targets=targets), patch.object(
                main, "list_flash_targets", return_value=targets
            ), self.assertRaises(ValueError):
                main.resolve_flash_target(chosen)

    def test_copy_and_reset_only_selected_board(self):
        for selected_index in (0, 1):
            with self.subTest(selected_index=selected_index), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                firmware = root / "measure-2wire-bioz"
                firmware.mkdir()
                binary = "measure_2wire_bioz.bin"
                (firmware / binary).write_bytes(b"firmware")
                targets = []
                for serial in ("aaaa", "bbbb"):
                    mount = root / serial
                    mount.mkdir()
                    (mount / "DETAILS.TXT").write_text(f"Unique ID: {serial}\n")
                    targets.append(main.FlashTarget(f"/dev/{serial}", serial, str(mount)))
                app = SimpleNamespace(_log=Mock())
                with patch.object(main, "PROJECT_ROOT", root), patch.object(
                    main, "list_flash_targets", return_value=targets
                ), patch.object(main.subprocess, "run", return_value=SimpleNamespace(
                    stdout="", stderr="", returncode=0
                )) as run:
                    main.App._build_flash_worker_inner(app, firmware.name, targets[selected_index])
                selected = targets[selected_index]
                self.assertEqual((Path(selected.mount) / binary).read_bytes(), b"firmware")
                self.assertFalse((Path(targets[1 - selected_index].mount) / binary).exists())
                reset = run.call_args_list[-1].args[0]
                self.assertIn(f"adapter serial {selected.serial}", reset)
                self.assertLess(reset.index(f"adapter serial {selected.serial}"), reset.index("init"))

    def test_wrong_mounted_board_rejected_before_copy_or_reset(self):
        app = SimpleNamespace(_log=Mock())
        target = main.FlashTarget("/dev/sda", "aaaa", "/media/board")
        with patch.object(main, "list_flash_targets", return_value=[target]), patch.object(
            main, "read_daplink_serial", return_value="bbbb"
        ), patch.object(main.subprocess, "run", return_value=SimpleNamespace(
            stdout="", stderr="", returncode=0
        )) as run, self.assertRaises(ValueError):
            main.App._build_flash_worker_inner(app, "measure-2wire-bioz", target)
        self.assertEqual(run.call_count, 1)  # make only


if __name__ == "__main__":
    unittest.main()

"""Exercise pair validation and publication rollback with isolated files."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("wing_pair_release", ROOT / "tools/build_paired_release.py")
pair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pair)


class PairReleaseTest(unittest.TestCase):
    def setUp(self):
        parent = (ROOT / "build").resolve()
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="pair-test-", dir=parent)
        self.directory = Path(self.temporary.name).resolve()
        self.assertEqual(self.directory.parent, parent)
        self.addCleanup(self.temporary.cleanup)
        self.stage = self.directory / "stage"
        self.stage.mkdir()
        self.sources = {"product_commit": "a" * 40, "platform_commit": "b" * 40,
                        "product_dirty": None, "platform_dirty": None, "candidate_only": True}
        for board, (suffix, _, mcu, _, _) in pair.BOARDS.items():
            version = "top-flying-wing-9.8.7-" + suffix
            app = bytearray(struct.pack("<II", 0x20010000, 0x08008009))
            app += b"\x00" * 8 + ("TOP_FLYING_WING_MCU=" + mcu).encode() + b"\x00"
            app += ("TOP_FLYING_WING_FW_VERSION=" + version).encode() + b"\x00"
            path = self.directory / (board + "-app.bin")
            path.write_bytes(app)
            pair.create_variant(self.stage, board, path, version, self.sources, "test compiler")

    def refresh(self, board):
        folder = self.stage / board
        path = folder / "manifest.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest["artifacts"] = [pair.artifact(folder / item["name"]) for item in manifest["artifacts"]]
        pair.save_json(path, manifest)
        readme = folder / "README_FLASH.md"
        text = readme.read_text(encoding="utf-8")
        import re
        text = re.sub(r"Full SHA-256：[0-9a-f]{64}", "Full SHA-256：" +
                      pair.sha((folder / "TopFlyingWing_Full.bin").read_bytes()), text)
        readme.write_text(text, encoding="utf-8", newline="\n")

    def test_candidate_pair_and_formal_gate(self):
        pair.validate_pair(self.stage, True)
        with self.assertRaises(ValueError):
            pair.validate_pair(self.stage, False)

    def test_boot_swap_is_rejected_after_manifest_refresh(self):
        folder = self.stage / "v1"
        boot = (self.stage / "v2/ABox_Boot.bin").read_bytes()
        (folder / "ABox_Boot.bin").write_bytes(boot)
        full_path = folder / "TopFlyingWing_Full.bin"
        full = bytearray(full_path.read_bytes())
        full[:pair.APP_OFFSET] = boot
        full_path.write_bytes(full)
        self.refresh("v1")
        with self.assertRaisesRegex(ValueError, "Boot mismatch"):
            pair.validate_pair(self.stage, True)

    def test_wrong_state_is_rejected_after_hashes_are_refreshed(self):
        folder = self.stage / "v1"
        path = folder / "TopFlyingWing_BootState.bin"
        state = bytearray(path.read_bytes())
        struct.pack_into("<I", state, 24, struct.unpack_from("<I", state, 24)[0] ^ 1)
        struct.pack_into("<I", state, 108, zlib.crc32(state[:108]))
        path.write_bytes(state)
        full_path = folder / "TopFlyingWing_Full.bin"
        full = bytearray(full_path.read_bytes())
        full[pair.STATE_OFFSET:] = state
        full_path.write_bytes(full)
        self.refresh("v1")
        with self.assertRaisesRegex(ValueError, "State/App mismatch"):
            pair.validate_pair(self.stage, True)

    def test_full_must_exclude_device_config(self):
        path = self.stage / "v2/TopFlyingWing_Full.bin"
        path.write_bytes(path.read_bytes() + b"\xff" * 2048)
        self.refresh("v2")
        with self.assertRaisesRegex(ValueError, "Full image"):
            pair.validate_pair(self.stage, True)

    def test_failed_replacement_restores_previous_dist(self):
        sources = {"product_commit": "a" * 40, "platform_commit": "b" * 40,
                   "product_dirty": False, "platform_dirty": False, "candidate_only": False}
        for board in pair.BOARDS:
            folder = self.stage / board
            for name in ("manifest.json", "release.manifest.json"):
                path = folder / name
                data = json.loads(path.read_text(encoding="utf-8"))
                data.update(sources)
                if name == "release.manifest.json":
                    data["immutable"] = True
                pair.save_json(path, data)
        destination = self.directory / "dist"
        destination.mkdir()
        (destination / "old.txt").write_text("previous release", encoding="utf-8")
        original_replace = os.replace

        def fail_publish(src, dst):
            if Path(dst) == destination and ".dist-incoming-" in Path(src).name:
                raise OSError("injected publication failure")
            original_replace(src, dst)

        with mock.patch.object(pair, "ROOT", self.directory), \
             mock.patch.object(pair, "formal_sources", return_value=sources), \
             mock.patch.object(pair.os, "replace", side_effect=fail_publish):
            with self.assertRaisesRegex(OSError, "injected"):
                pair.publish_pair(self.stage, sources)
        self.assertEqual((destination / "old.txt").read_text(encoding="utf-8"), "previous release")
        self.assertFalse((self.directory / ".dist-publish.lock").exists())


if __name__ == "__main__":
    unittest.main()

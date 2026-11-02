"""Run after build_release.ps1 to exercise the actual formal package verifier."""

import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from verify_release import crc32, verify
import hashlib


class ReleasePackageTest(unittest.TestCase):
    def setUp(self):
        parent = (ROOT / 'build/host').resolve()
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='release-check-', dir=parent)
        self.directory = Path(self.temporary.name).resolve()
        # Verify the exact cleanup target before TemporaryDirectory removes it.
        self.assertEqual(self.directory.parent, parent)
        self.addCleanup(self.temporary.cleanup)
        shutil.copytree(ROOT / 'dist', self.directory, dirs_exist_ok=True)

    def replace_state(self, state):
        (self.directory / 'TopFlyingWing_BootState.bin').write_bytes(state)
        full_path = self.directory / 'TopFlyingWing_Full.bin'
        full = bytearray(full_path.read_bytes())
        full[0x3E800:] = state
        full_path.write_bytes(full)
        self.refresh_hashes()

    def refresh_hashes(self):
        path = self.directory / 'manifest.json'
        manifest = json.loads(path.read_text(encoding='utf-8'))
        old_hash = next(a['sha256'] for a in manifest['artifacts'] if a['name'] == 'TopFlyingWing_Full.bin')
        for entry in manifest['artifacts']:
            data = (self.directory / entry['name']).read_bytes()
            entry.update(size=len(data), crc32=f'{crc32(data):08X}', sha256=hashlib.sha256(data).hexdigest())
        new_hash = next(a['sha256'] for a in manifest['artifacts'] if a['name'] == 'TopFlyingWing_Full.bin')
        path.write_text(json.dumps(manifest), encoding='utf-8')
        readme = self.directory / 'README_FLASH.md'
        readme.write_text(readme.read_text(encoding='utf-8').replace(old_hash, new_hash), encoding='utf-8')

    def test_actual_formal_package(self):
        self.assertEqual(verify(self.directory)['full_size'], 260096)

    def test_state_for_another_app_is_rejected_even_with_valid_hashes(self):
        state = bytearray((self.directory / 'TopFlyingWing_BootState.bin').read_bytes())
        image_crc = struct.unpack_from('<I', state, 24)[0] ^ 1
        struct.pack_into('<I', state, 24, image_crc)
        struct.pack_into('<I', state, 108, crc32(state[:108]))
        self.replace_state(state)
        with self.assertRaisesRegex(ValueError, 'Boot State App size/CRC mismatch'):
            verify(self.directory)

    def test_configuration_page_in_full_is_rejected(self):
        path = self.directory / 'TopFlyingWing_Full.bin'
        path.write_bytes(path.read_bytes() + b'\xFF' * 2048)
        self.refresh_hashes()
        with self.assertRaisesRegex(ValueError, 'Full image must end before configuration page'):
            verify(self.directory)

    def test_stale_state_b_is_rejected(self):
        state = bytearray((self.directory / 'TopFlyingWing_BootState.bin').read_bytes())
        state[0x800] = 0
        self.replace_state(state)
        with self.assertRaisesRegex(ValueError, 'State padding/B page must be erased'):
            verify(self.directory)

    def test_extra_diagnostic_file_is_rejected(self):
        (self.directory / 'diagnostic.txt').write_text('not a formal artifact')
        with self.assertRaisesRegex(ValueError, 'expected seven files'):
            verify(self.directory)

    def test_ota_metadata_mismatch_is_rejected(self):
        path = self.directory / 'release.manifest.json'
        manifest = json.loads(path.read_text(encoding='utf-8'))
        manifest['size'] += 1
        path.write_text(json.dumps(manifest), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'OTA size mismatch'):
            verify(self.directory)

    def test_failed_staging_preserves_current_release(self):
        release_dir = ROOT / 'dist'
        before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in release_dir.iterdir()}
        version = json.loads((release_dir / 'manifest.json').read_text(encoding='utf-8'))['firmware_version']
        bad_boot = bytearray((release_dir / 'ABox_Boot.bin').read_bytes())
        bad_boot[0x100] ^= 1
        bad_path = self.directory / 'invalid_boot.bin'
        bad_path.write_bytes(bad_boot)
        command = [
            'powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            str(ROOT / 'tools/package_release.ps1'), '-BootPath', str(bad_path),
            '-AppPath', str(release_dir / 'TopFlyingWing_App.bin'), '-FirmwareVersion', version,
        ]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, errors='replace', timeout=60)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Frozen Boot bytes changed', result.stdout + result.stderr)
        after = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in release_dir.iterdir()}
        self.assertEqual(before, after)


if __name__ == '__main__':
    unittest.main()

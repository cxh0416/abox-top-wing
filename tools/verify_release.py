#!/usr/bin/env python3
"""Validate a complete local formal release without touching a target device."""

import argparse
import hashlib
import importlib.util
import json
import re
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_FILES = {
    'ABox_Boot.bin', 'TopFlyingWing_App.bin', 'TopFlyingWing_BootState.bin',
    'TopFlyingWing_Full.bin', 'manifest.json', 'release.manifest.json', 'README_FLASH.md',
}
LOAD_ADDRESSES = {
    'ABox_Boot.bin': '0x08000000',
    'TopFlyingWing_App.bin': '0x08008000',
    'TopFlyingWing_BootState.bin': '0x0803E800',
    'TopFlyingWing_Full.bin': '0x08000000',
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def verify(directory, expected_version=None):
    directory = Path(directory)
    require({p.name for p in directory.iterdir()} == EXPECTED_FILES,
            'Release must contain exactly the expected seven files')
    require(all((directory / name).is_file() for name in EXPECTED_FILES),
            'Release entries must be regular files')
    manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8'))
    release = json.loads((directory / 'release.manifest.json').read_text(encoding='utf-8'))
    version = manifest['firmware_version']
    require(re.fullmatch(r'top-flying-wing-\d+\.\d+\.\d+(?:-st)?', version),
            'A formal STM32 release requires a product version with an optional -st suffix')
    if expected_version is not None:
        require(version == expected_version, 'Unexpected release version')
    require(manifest['product'] == release['product'] == 'top_flying_wing', 'Product mismatch')
    require(release['version'] == version and manifest['boot_version'] == 'abox-boot-2.3.0',
            'Release/Boot version mismatch')
    require(manifest['app_range'] == '0x08008000..0x0803DFFF' and
            manifest['boot_state_pages'] == ['0x0803E800', '0x0803F000'] and
            manifest['config_page'] == '0x0803F800', 'Flash partition contract changed')
    require(manifest['boot_state_initialization'] == {
        'format_version': 3, 'state_a': 'CONFIRMED', 'state_b': 'erased',
    }, 'Boot State initialization metadata mismatch')
    require(manifest['ota_scheme'] == 'boot_v2_ufs' and manifest['ota_downloader'] == 'app',
            'OTA scheme mismatch')
    for obj in (manifest, release):
        require(obj['can_session_mode'] == 'remote_only_idle_silent' and
                obj['idle_status_poll_enabled'] is False, 'CAN session metadata mismatch')

    artifacts = {a['name']: a for a in manifest['artifacts']}
    require(len(manifest['artifacts']) == len(LOAD_ADDRESSES) and
            set(artifacts) == set(LOAD_ADDRESSES), 'Artifact list mismatch')
    images = {}
    for name, address in LOAD_ADDRESSES.items():
        data = (directory / name).read_bytes()
        entry = artifacts[name]
        require(entry['load_address'] == address and entry['size'] == len(data),
                f'Artifact address/size mismatch: {name}')
        require(entry['crc32'] == f'{crc32(data):08X}' and
                entry['sha256'] == hashlib.sha256(data).hexdigest(), f'Artifact hash mismatch: {name}')
        require(entry['includes_device_config'] is False, 'Device configuration must be excluded')
        require(entry['includes_boot_state'] is (name in {
            'TopFlyingWing_Full.bin', 'TopFlyingWing_BootState.bin',
        }), f'Boot State inclusion flag mismatch: {name}')
        images[name] = data

    boot = images['ABox_Boot.bin']
    app = images['TopFlyingWing_App.bin']
    state = images['TopFlyingWing_BootState.bin']
    full = images['TopFlyingWing_Full.bin']
    frozen = ROOT / 'platform/targets/stm32f105_abox_boot'
    spec = importlib.util.spec_from_file_location('frozen_boot_verify', frozen / 'tools/verify_boot_artifact.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.verify(directory / 'ABox_Boot.bin')
    require(boot == (frozen / 'dist/ABox_Boot.bin').read_bytes(), 'Frozen Boot bytes changed')

    require(8 <= len(app) <= 0x36000, 'App exceeds its partition')
    sp, reset = struct.unpack_from('<II', app)
    require(0x20000000 <= sp <= 0x20010000 and sp % 8 == 0 and reset & 1 and
            0x08008000 <= (reset & ~1) < 0x08008000 + len(app), 'Invalid App vectors')
    require(b'TOP_FLYING_WING_FW_VERSION=' + version.encode('ascii') + b'\0' in app,
            'App version marker mismatch')
    require(len(state) == 0x1000, 'State image must cover exactly A/B pages')
    require(struct.unpack_from('<IHHII', state) == (0x42535632, 3, 112, 1, 4),
            'Invalid initial CONFIRMED State record')
    require(struct.unpack_from('<II', state, 20) == struct.unpack_from('<II', state, 60) ==
            (len(app), crc32(app)), 'Boot State App size/CRC mismatch')
    require(state[28:60].split(b'\0', 1)[0] == state[68:100].split(b'\0', 1)[0] ==
            version.encode('ascii'), 'Boot State App version mismatch')
    require(struct.unpack_from('<I', state, 108)[0] == crc32(state[:108]), 'Invalid State record CRC')
    require(state[112:] == b'\xFF' * (0x1000 - 112), 'State padding/B page must be erased')
    require(len(full) == 0x3F800, 'Full image must end before configuration page 0x0803F800')
    require(full[:0x8000] == boot and full[0x8000:0x8000 + len(app)] == app and
            full[0x3E800:] == state, 'Full image components do not match separate artifacts')
    require(full[0x8000 + len(app):0x3E800] == b'\xFF' * (0x36800 - len(app)),
            'App-to-State gap must be erased')
    require(release['artifact'] == 'top_flying_wing_app.bin' and
            release['file'] == 'TopFlyingWing_App.bin' and release['immutable'] is True,
            'OTA artifact metadata mismatch')
    for key in ('size', 'crc32', 'sha256'):
        require(release[key] == artifacts['TopFlyingWing_App.bin'][key], f'OTA {key} mismatch')
    readme = (directory / 'README_FLASH.md').read_text(encoding='utf-8')
    require(version in readme and '{{' not in readme, 'Unexpanded release instructions')
    require(artifacts['TopFlyingWing_Full.bin']['sha256'] in readme, 'README Full hash mismatch')
    return {'product': 'top_flying_wing', 'version': version, 'full_size': len(full),
            'app_size': len(app), 'full_sha256': hashlib.sha256(full).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--version')
    args = parser.parse_args()
    try:
        result = verify(args.directory, args.version)
    except (ValueError, KeyError, OSError, struct.error) as error:
        raise SystemExit(f'Release validation failed: {error}') from error
    print('Formal release verified')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

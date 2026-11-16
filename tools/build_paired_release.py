#!/usr/bin/env python3
"""Build and verify matched V1/GD32 and V2/STM32 wing firmware packages."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = ROOT / "platform"
BOARDS = {
    "v1": ("gd", "gd32f105_abox_boot", "GD32F105RCT6", "abox-boot-2.3.0-gd",
           "3eb4df8bda5c8a99c338dfe93a289acd43a4b94bf432b5785ec10ea90340f237"),
    "v2": ("st", "stm32f105_abox_boot", "STM32F105RCT6", "abox-boot-2.3.0",
           "981409ef107d5f5c56a1a80fce107c0292e2c1165668c0279ba6158f24a61c63"),
}
FILES = {"ABox_Boot.bin", "TopFlyingWing_App.bin", "TopFlyingWing_Full.bin",
         "TopFlyingWing_BootState.bin", "manifest.json", "release.manifest.json", "README_FLASH.md"}
ADDRESSES = {"ABox_Boot.bin": "0x08000000", "TopFlyingWing_App.bin": "0x08008000",
             "TopFlyingWing_Full.bin": "0x08000000", "TopFlyingWing_BootState.bin": "0x0803E800"}
APP_OFFSET, APP_MAX, STATE_OFFSET, FULL_SIZE = 0x8000, 0x36000, 0x3E800, 0x3F800

def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)

def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def crc(data: bytes) -> str:
    return f"{zlib.crc32(data):08X}"

def load_shared():
    path = PLATFORM / "tools/publish_release.py"
    spec = importlib.util.spec_from_file_location("abox_release_paths", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def git(repo: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True,
                                   stderr=subprocess.DEVNULL).strip()

def formal_sources() -> dict:
    for repo in (ROOT, PLATFORM):
        require(Path(git(repo, "rev-parse", "--show-toplevel")).resolve() == repo.resolve(),
                f"not a separate Git worktree: {repo}")
        require(not git(repo, "status", "--porcelain"), f"source worktree is dirty: {repo}")
    product_commit = git(ROOT, "rev-parse", "HEAD")
    platform_commit = git(PLATFORM, "rev-parse", "HEAD")
    require(git(ROOT, "rev-parse", "HEAD:platform") == platform_commit,
            "platform checkout differs from the committed submodule pointer")
    return {"product_commit": product_commit, "platform_commit": platform_commit,
            "product_dirty": False, "platform_dirty": False, "candidate_only": False}

def candidate_sources(product_base: str | None, platform_base: str | None) -> dict:
    values = []
    for repo, declared in ((ROOT, product_base), (PLATFORM, platform_base)):
        try:
            top = Path(git(repo, "rev-parse", "--show-toplevel")).resolve()
            if top != repo.resolve():
                raise ValueError("snapshot is not a separate Git worktree")
            commit = git(repo, "rev-parse", "HEAD")
            dirty = bool(git(repo, "status", "--porcelain"))
        except (subprocess.CalledProcessError, ValueError):
            commit, dirty = declared, None
        else:
            require(not declared or declared == commit, f"declared commit differs from checkout: {repo}")
        values.append((commit, dirty))
    return {"product_commit": values[0][0], "platform_commit": values[1][0],
            "product_dirty": values[0][1], "platform_dirty": values[1][1], "candidate_only": True}

def boot_file(board: str) -> Path:
    _, target, _, _, expected = BOARDS[board]
    folder = PLATFORM / "targets" / target
    image = folder / "dist/ABox_Boot.bin"
    require(sha(image.read_bytes()) == expected, f"{board} frozen Boot SHA-256 mismatch")
    spec = importlib.util.spec_from_file_location(f"boot_verify_{board}", folder / "tools/verify_boot_artifact.py")
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    verifier.verify(image)
    return image

def artifact(path: Path) -> dict:
    data = path.read_bytes()
    return {"name": path.name, "size": len(data), "load_address": ADDRESSES[path.name],
            "crc32": crc(data), "sha256": sha(data), "includes_device_config": False,
            "includes_boot_state": path.name in {"TopFlyingWing_Full.bin", "TopFlyingWing_BootState.bin"}}

def save_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")

def create_variant(stage: Path, board: str, app: Path, version: str, sources: dict, toolchain: str) -> None:
    suffix, _, mcu, boot_version, _ = BOARDS[board]
    require(version.endswith("-" + suffix), "board version suffix mismatch")
    folder = stage / board
    folder.mkdir()
    boot = boot_file(board).read_bytes()
    app_data = app.read_bytes()
    require(8 <= len(app_data) <= APP_MAX, f"{board} App exceeds its Flash region")
    (folder / "ABox_Boot.bin").write_bytes(boot)
    app_out = folder / "TopFlyingWing_App.bin"
    app_out.write_bytes(app_data)
    state_out = folder / "TopFlyingWing_BootState.bin"
    subprocess.run([sys.executable, str(ROOT / "tools/make_bootstate.py"), "--app", str(app_out),
                    "--output", str(state_out), "--version", version], check=True)
    state = state_out.read_bytes()
    full = bytearray(b"\xFF" * FULL_SIZE)
    full[:APP_OFFSET] = boot
    full[APP_OFFSET:APP_OFFSET + len(app_data)] = app_data
    full[STATE_OFFSET:] = state
    (folder / "TopFlyingWing_Full.bin").write_bytes(full)
    artifacts = [artifact(folder / name) for name in
                 ("TopFlyingWing_App.bin", "ABox_Boot.bin", "TopFlyingWing_Full.bin", "TopFlyingWing_BootState.bin")]
    manifest = {"manifest_version": 2, "product": "top_flying_wing", "firmware_version": version,
                "configuration": "Release", "hardware_target": board, "mcu": mcu,
                "boot_version": boot_version, "ota_scheme": "boot_v2_ufs", "ota_downloader": "app",
                "can_session_mode": "remote_only_idle_silent", "idle_status_poll_enabled": False,
                "app_range": "0x08008000..0x0803DFFF", "config_recovery_page": "0x0803E000",
                "boot_state_pages": ["0x0803E800", "0x0803F000"], "config_page": "0x0803F800",
                "boot_state_initialization": {"format_version": 3, "state_a": "CONFIRMED", "state_b": "erased"},
                "toolchain": toolchain, **sources, "artifacts": artifacts}
    save_json(folder / "manifest.json", manifest)
    app_artifact = artifacts[0]
    release = {"schema_version": 1, "product": "top_flying_wing", "version": version,
               "artifact": "top_flying_wing_app.bin", "file": "TopFlyingWing_App.bin",
               "immutable": not sources["candidate_only"], "hardware_target": board,
               "boot_version": boot_version, "can_session_mode": "remote_only_idle_silent",
               "idle_status_poll_enabled": False, **sources,
               **{key: app_artifact[key] for key in ("size", "crc32", "sha256")}}
    save_json(folder / "release.manifest.json", release)
    label = "候选包（不得作为正式发布或 OTA 版本）" if sources["candidate_only"] else "正式烧录包"
    readme = f'''# top_flying_wing {label}

硬件：{mcu}；App：{version}；Boot：{boot_version}。

| 文件 | 地址 | 用途 |
| --- | --- | --- |
| TopFlyingWing_Full.bin | 0x08000000 | Boot、App 和匹配的 State A/B |
| ABox_Boot.bin | 0x08000000 | 冻结公共 Boot |
| TopFlyingWing_App.bin | 0x08008000 | App；OTA 只使用此文件 |
| TopFlyingWing_BootState.bin | 0x0803E800 | 与 App 匹配的 4 KiB State A/B |

Full 长度固定为 260096 字节，结束于 0x0803F7FF，不包含 0x0803F800 配置页。
仅擦除文件覆盖的页并校验；Full 会覆盖 0x0803E000 配置恢复页，先确认不存在待恢复的配置事务。
独立线刷 App 时应同时写入配套 State；OTA 安装由设备管理启动状态。

App 长度：{len(app_data)} 字节；CRC32：{crc(app_data)}。
Full SHA-256：{artifacts[2]["sha256"]}。
文件校验及软件测试不能证明实机运动、OTA 或回滚成功。
'''
    (folder / "README_FLASH.md").write_text(readme, encoding="utf-8", newline="\n")

def validate_variant(folder: Path, board: str, candidate: bool) -> dict:
    require(folder.is_dir() and {p.name for p in folder.iterdir()} == FILES, f"{board} file set mismatch")
    require(all(p.is_file() and not p.is_symlink() for p in folder.iterdir()), "invalid package entry")
    manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    release = json.loads((folder / "release.manifest.json").read_text(encoding="utf-8"))
    suffix, _, mcu, boot_version, boot_hash = BOARDS[board]
    version = manifest["firmware_version"]
    require(re.fullmatch(r"top-flying-wing-\d+\.\d+\.\d+-" + suffix, version) is not None and len(version) < 32,
            f"{board} version invalid")
    require(manifest["product"] == release["product"] == "top_flying_wing" and
            release["version"] == version and manifest["hardware_target"] == release["hardware_target"] == board and
            manifest["mcu"] == mcu and manifest["boot_version"] == release["boot_version"] == boot_version,
            f"{board} identity mismatch")
    require(manifest["candidate_only"] is candidate and release["candidate_only"] is candidate and
            release["immutable"] is (not candidate), f"{board} candidate status mismatch")
    for key in ("product_commit", "platform_commit", "product_dirty", "platform_dirty"):
        require(manifest[key] == release[key], f"{board} source metadata mismatch: {key}")
    if not candidate:
        require(manifest["product_dirty"] is False and manifest["platform_dirty"] is False and
                all(isinstance(manifest[key], str) and re.fullmatch(r"[0-9a-f]{40}", manifest[key])
                    for key in ("product_commit", "platform_commit")), f"{board} formal source metadata invalid")
    require(manifest["configuration"] == "Release" and manifest["app_range"] == "0x08008000..0x0803DFFF" and
            manifest["config_recovery_page"] == "0x0803E000" and manifest["config_page"] == "0x0803F800" and
            manifest["boot_state_pages"] == ["0x0803E800", "0x0803F000"] and
            manifest["boot_state_initialization"] == {"format_version": 3, "state_a": "CONFIRMED", "state_b": "erased"},
            f"{board} Flash layout mismatch")
    for obj in (manifest, release):
        require(obj["can_session_mode"] == "remote_only_idle_silent" and obj["idle_status_poll_enabled"] is False,
                f"{board} CAN session metadata mismatch")
    require(manifest["ota_scheme"] == "boot_v2_ufs" and manifest["ota_downloader"] == "app" and
            release["artifact"] == "top_flying_wing_app.bin" and release["file"] == "TopFlyingWing_App.bin",
            f"{board} OTA metadata mismatch")
    artifacts = manifest["artifacts"]
    require(len(artifacts) == len(ADDRESSES) and {a["name"] for a in artifacts} == set(ADDRESSES),
            f"{board} artifact set mismatch")
    for item in artifacts:
        require(item == artifact(folder / item["name"]), f"{board} artifact mismatch: {item['name']}")
    boot = (folder / "ABox_Boot.bin").read_bytes()
    app = (folder / "TopFlyingWing_App.bin").read_bytes()
    state = (folder / "TopFlyingWing_BootState.bin").read_bytes()
    full = (folder / "TopFlyingWing_Full.bin").read_bytes()
    require(sha(boot) == boot_hash and boot == boot_file(board).read_bytes(), f"{board} Boot mismatch")
    require(8 <= len(app) <= APP_MAX, f"{board} App out of bounds")
    sp, reset = struct.unpack_from("<II", app)
    require(0x20000000 <= sp <= 0x20010000 and sp % 8 == 0 and reset & 1 and
            0x08008000 <= (reset & ~1) < 0x08008000 + len(app), f"{board} App vectors invalid")
    require(("TOP_FLYING_WING_FW_VERSION=" + version).encode() + b"\0" in app and
            ("TOP_FLYING_WING_MCU=" + mcu).encode() + b"\0" in app, f"{board} App marker mismatch")
    other = BOARDS["v2" if board == "v1" else "v1"][2]
    require(("TOP_FLYING_WING_MCU=" + other).encode() + b"\0" not in app, f"{board} wrong MCU marker")
    require(len(state) == 0x1000 and struct.unpack_from("<IHHII", state) == (0x42535632, 3, 112, 1, 4),
            f"{board} State record invalid")
    image_crc = zlib.crc32(app) & 0xFFFFFFFF
    require(struct.unpack_from("<II", state, 20) == struct.unpack_from("<II", state, 60) == (len(app), image_crc) and
            state[28:60].split(b"\0", 1)[0] == state[68:100].split(b"\0", 1)[0] == version.encode() and
            struct.unpack_from("<I", state, 108)[0] == zlib.crc32(state[:108]) and
            state[112:] == b"\xFF" * (0x1000 - 112), f"{board} State/App mismatch")
    require(len(full) == FULL_SIZE and full[:APP_OFFSET] == boot and
            full[APP_OFFSET:APP_OFFSET + len(app)] == app and full[STATE_OFFSET:] == state and
            full[APP_OFFSET + len(app):STATE_OFFSET] == b"\xFF" * (STATE_OFFSET - APP_OFFSET - len(app)),
            f"{board} Full image or reserved pages mismatch")
    for key, expected in (("size", len(app)), ("crc32", crc(app)), ("sha256", sha(app))):
        require(release[key] == expected, f"{board} OTA {key} mismatch")
    readme = (folder / "README_FLASH.md").read_text(encoding="utf-8")
    require(version in readme and sha(full) in readme, f"{board} README mismatch")
    return manifest

def validate_pair(stage: Path, candidate: bool) -> dict:
    shared = load_shared()
    shared._check_tree(ROOT, stage)
    require(stage.is_dir() and {p.name for p in stage.iterdir()} == {"v1", "v2"}, "pair must contain exactly v1 and v2")
    v1 = validate_variant(stage / "v1", "v1", candidate)
    v2 = validate_variant(stage / "v2", "v2", candidate)
    require(v1["firmware_version"][:-3] == v2["firmware_version"][:-3], "pair versions differ")
    for key in ("product_commit", "platform_commit", "product_dirty", "platform_dirty", "toolchain"):
        require(v1[key] == v2[key], f"pair source metadata differs: {key}")
    return {"v1": v1, "v2": v2}

def publish_pair(stage: Path, sources: dict) -> None:
    shared = load_shared()
    target = shared._inside(ROOT, ROOT / "dist")
    stage = shared._inside(ROOT, stage)
    require(not (stage == target or stage.is_relative_to(target) or target.is_relative_to(stage)), "stage overlaps dist")
    releases = validate_pair(stage, False)
    require(formal_sources() == sources, "sources changed during release build")
    for key in sources:
        require(releases["v1"][key] == sources[key], f"formal source mismatch: {key}")
    token = uuid.uuid4().hex
    incoming = shared._inside(ROOT, ROOT / f".dist-incoming-{token}")
    backup = shared._inside(ROOT, ROOT / f".dist-backup-{token}")
    lock = shared._inside(ROOT, ROOT / ".dist-publish.lock")
    handle = lock.open("x")
    try:
        try:
            shutil.copytree(stage, incoming)
            validate_pair(incoming, False)
            if target.exists():
                require(target.is_dir(), "dist is not a directory")
                shared._check_tree(ROOT, target)
                os.replace(target, backup)
            try:
                os.replace(incoming, target)
            except BaseException:
                if backup.exists():
                    os.replace(backup, target)
                raise
            if backup.exists():
                shared._remove(ROOT, backup)
        finally:
            if incoming.exists():
                shared._remove(ROOT, incoming)
            if backup.exists() and not target.exists():
                os.replace(backup, target)
    finally:
        handle.close()
        lock.unlink()

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="V2 version ending in -st; the V1 suffix is -gd")
    destination = parser.add_mutually_exclusive_group(required=True)
    destination.add_argument("--candidate", type=Path, help="new output directory inside the product root, outside dist")
    destination.add_argument("--formal", action="store_true", help="require clean committed sources and replace dist as a pair")
    parser.add_argument("--product-base", help="candidate snapshot base commit, if known")
    parser.add_argument("--platform-base", help="candidate platform snapshot commit, if known")
    args = parser.parse_args()
    require(re.fullmatch(r"top-flying-wing-\d+\.\d+\.\d+-st", args.version) is not None and len(args.version) < 32,
            "invalid V2 version")
    if args.formal:
        require(not args.product_base and not args.platform_base, "snapshot labels are only valid for candidates")
        sources = formal_sources()
    else:
        for value in (args.product_base, args.platform_base):
            require(not value or re.fullmatch(r"[0-9a-f]{40}", value) is not None, "invalid candidate base commit")
        sources = candidate_sources(args.product_base, args.platform_base)
    shared = load_shared()
    stage = shared._inside(ROOT, args.candidate if args.candidate and args.candidate.is_absolute()
                           else ROOT / args.candidate if args.candidate else ROOT / "out" / ("pair-stage-" + uuid.uuid4().hex))
    target = shared._inside(ROOT, ROOT / "dist")
    require(not (stage == target or stage.is_relative_to(target) or target.is_relative_to(stage)), "output overlaps dist")
    require(not stage.exists(), "output directory already exists")
    stage.mkdir(parents=True)
    toolchain = subprocess.check_output(["arm-none-eabi-gcc", "--version"], text=True).splitlines()[0].strip()
    # Keep Windows object paths short while isolating concurrent invocations.
    build_run = shared._inside(ROOT, ROOT / "build" / ("r-" + uuid.uuid4().hex[:12]))
    require(not build_run.exists(), "build directory already exists")
    try:
        for board, (suffix, _, _, _, _) in BOARDS.items():
            version = args.version[:-3] + "-" + suffix
            build = shared._inside(ROOT, build_run / board)
            subprocess.run(["cmake", "-S", str(ROOT / "app"), "-B", str(build), "-G", "Ninja",
                            "-DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake", "-DCMAKE_BUILD_TYPE=Release",
                            "-DABOX_BOARD_VERSION=" + board, "-DTOP_FLYING_WING_APP_FW_VERSION=" + version], check=True)
            subprocess.run(["cmake", "--build", str(build), "--parallel", "4"], check=True)
            create_variant(stage, board, build / "TopFlyingWing_App.bin", version, sources, toolchain)
        validate_pair(stage, not args.formal)
        if args.formal:
            publish_pair(stage, sources)
            validate_pair(ROOT / "dist", False)
            print(f"Formal pair: {ROOT / 'dist'}")
        else:
            print(f"Candidate pair only: {stage}")
    finally:
        if args.formal and stage.exists():
            shared._remove(ROOT, stage)

if __name__ == "__main__":
    main()

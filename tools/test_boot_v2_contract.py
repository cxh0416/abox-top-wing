from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BOOT = ROOT / "platform/targets/stm32f105_abox_boot"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def main() -> None:
    app_cfg = read(ROOT / "app/Core/User/Inc/boot_cfg.h")
    boot_cfg = read(BOOT / "Core/User/Inc/boot_cfg.h")
    app_linker = read(ROOT / "app/STM32F105XX_FLASH.ld")
    boot_linker = read(BOOT / "STM32F105xx_FLASH.ld")
    boot_cmake = read(BOOT / "CMakeLists.txt")
    boot_core = read(BOOT / "Core/User/Src/ota_core.c")
    descriptor = read(BOOT / "Core/User/Src/boot_descriptor.c")
    app_core = read(ROOT / "app/Core/User/Src/ota_service.c")
    at_core = read(ROOT / "platform/components/ec800/abox_ec800_at.c")
    downloader = read(ROOT / "platform/components/ota/abox_https_ufs_downloader.c")
    info = read(ROOT / "app/Core/User/Src/mqtt_app_cmd.c")

    for source in (app_cfg, boot_cfg):
        assert "BOOT_STATE_A_ADDR 0x0803E800U" in source
        assert "BOOT_STATE_B_ADDR 0x0803F000U" in source
        assert "APP_END_ADDR (BOOT_STATE_A_ADDR - 1U)" in source
    assert "LENGTH = 0x36800" in app_linker
    assert "ORIGIN = 0x08000000, LENGTH = 0x7F00" in boot_linker
    assert "ORIGIN = 0x08007F00, LENGTH = 0x100" in boot_linker
    assert "0x2000BC40" in boot_linker
    assert "set(CMAKE_PROJECT_NAME ABox_Boot)" in boot_cmake
    assert 'ABOX_BOOT_FW_VERSION "abox-boot-2.3.0"' in boot_cmake
    assert "abox_platform_attach_boot_v2" in boot_cmake
    assert "ABoxBootV2Boot_Init" in boot_core
    assert "ABoxBootV2Boot_Task" in boot_core
    assert "App owns HTTPS/CA/direct+Range staging" in boot_core
    assert '.product_id="abox_stm32f105"' in descriptor
    assert '.build_version="abox-boot-2.3.0"' in descriptor
    assert "ABoxBootV2App_Init" in app_core
    assert 'c.ota_origin="https://ota.zxwl.ntiov.com:20443"' in app_core
    assert 'c.revision_product="top_flying_wing"' in app_core
    assert 'c.artifact_name="top_flying_wing_app.bin"' in app_core
    assert 'strncmp(command, "AT+QFUPL=\\\"", 10U)' in at_core
    assert "AT+QHTTPREADFILE=" in downloader
    assert "AT+QHTTPGETEX=80" in downloader
    assert '"bootVersion"' in info
    assert '"bootDescriptorValid"' in info
    assert '"bootApiVersion"' in info
    assert '"bootFeatures"' in info
    assert '"otaScheme", "boot_v2_ufs"' in info
    assert '"otaDownloader", "app"' in info
    assert '"otaReady"' in info
    assert '"otaState"' in info

    artifact = BOOT / "dist/ABox_Boot.bin"
    subprocess.run(
        [sys.executable, str(BOOT / "tools/verify_boot_artifact.py"), str(artifact)],
        check=True,
    )
    print("Boot V2 contract tests passed")


if __name__ == "__main__":
    main()

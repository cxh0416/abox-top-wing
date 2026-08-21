from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def main() -> None:
    app_cfg = read("app/Core/User/Inc/boot_cfg.h")
    boot_cfg = read("boot/Core/User/Inc/boot_cfg.h")
    linker = read("app/STM32F105XX_FLASH.ld")
    boot_cmake = read("boot/CMakeLists.txt")
    boot_core = read("boot/Core/User/Src/ota_core.c")
    app_core = read("app/Core/User/Src/ota_service.c")
    at_core = read("app/Core/User/Src/ec800_at.c")
    info = read("app/Core/User/Src/mqtt_app_cmd.c")

    for source in (app_cfg, boot_cfg):
        assert "BOOT_STATE_A_ADDR 0x0803E800U" in source
        assert "BOOT_STATE_B_ADDR 0x0803F000U" in source
        assert "APP_END_ADDR (BOOT_STATE_A_ADDR - 1U)" in source
    assert "LENGTH = 0x36800" in linker
    assert "abox_platform_attach_boot_v2" in boot_cmake
    assert "ABoxBootV2Boot_Init" in boot_core
    assert "ABoxBootV2Boot_Task" in boot_core
    assert "Boot V2.2 never downloads" in boot_core
    assert "ABoxBootV2App_Init" in app_core
    assert 'c.ota_origin="https://ota.zxwl.ntiov.com:20443"' in app_core
    assert 'c.revision_product="top_flying_wing"' in app_core
    assert 'c.artifact_name="top_flying_wing_app.bin"' in app_core
    qfupl_marker = next(line for line in at_core.splitlines() if "strncmp(cmd" in line and "QFUPL=" in line)
    assert qfupl_marker.count("\\") == 1
    assert '"bootVersion"' in info
    assert '"bootDescriptorValid"' in info
    assert '"bootApiVersion"' in info
    assert '"bootFeatures"' in info
    assert '"otaScheme", "boot_v2_ufs"' in info
    assert '"otaDownloader", "app"' in info
    assert '"otaReady"' in info
    assert '"otaState"' in info
    print("Boot V2 contract tests passed")


if __name__ == "__main__":
    main()

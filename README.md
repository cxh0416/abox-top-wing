# A-BOX 顶飞翼固件

产品标识固定为 `top_flying_wing`，首版 App 为 `top-flying-wing-1.0.0`，Boot 为 `top-flying-wing-boot-2.2.0`。

## 结构

- `app/`：STM32F105 App，负责 EC800、MQTT、CAN 控制器状态机和 Boot V2 OTA 下载。
- `boot/`：Boot V2，只负责安装、试运行确认与回滚，不联网下载。
- `docs/vendor/`：厂家 CAN/控制器资料，仅作为设备端帧依据。
- `docs/protocols/`：A-BOX 与平台公共协议归档。
- `dist/`：正式本地构建产物及校验清单。

## CAN

默认 CAN2，可通过 CMake 缓存变量 `TOP_WING_CAN_BUS=1|2` 切换。两路均配置为 500 kbps；业务使用 29 位扩展帧、发送 ID `0x1`、接收 ID `0x2`、DLC 8。应用层仅接受目标 ID 和合法帧。

## 构建与验证

```powershell
cmake --preset Release -S app
cmake --build app/build/Release
cmake --preset Release -S boot
cmake --build boot/build/Release
powershell -ExecutionPolicy Bypass -File .\tools\test_host.ps1
python .\tools\test_boot_v2_contract.py
powershell -ExecutionPolicy Bypass -File .\tools\package_release.ps1 `
  -BootPath .\boot\build\Release\TopFlyingWing_Boot.bin `
  -AppPath .\app\build\Release\TopFlyingWing_App.bin
```

App 烧录地址为 `0x08008000`，Boot/Full 为 `0x08000000`。Full 只拼接到 App 实际末尾，不包含配置页和 Boot State。

本地构建和主机测试不代表 CAN 线束、终端电阻、电机、本地触摸、Boot 跳转或真实 OTA 已通过实物验收。

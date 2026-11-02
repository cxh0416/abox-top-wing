# top_flying_wing 正式烧录包

App 版本：`{{FIRMWARE_VERSION}}`。公共 Boot：`{{BOOT_VERSION}}`。

## 文件与地址

| 文件 | 用途 | 烧录起始地址 |
| --- | --- | --- |
| `TopFlyingWing_Full.bin` | 推荐整包烧录，已包含 Boot、App 和匹配的 State A/B 区域 | `0x08000000` |
| `ABox_Boot.bin` | 独立公共 Boot | `0x08000000` |
| `TopFlyingWing_App.bin` | 独立 App；设备经 OTA 更新时仅使用此文件 | `0x08008000` |
| `TopFlyingWing_BootState.bin` | 与本次 App 匹配的 4 KiB 启动状态 | `0x0803E800` |
| `manifest.json` | 所有二进制文件的地址、长度、CRC32 和 SHA-256 | 不烧录 |
| `release.manifest.json` | App 版本和 OTA 制品校验信息 | 不烧录 |

## 推荐烧录方法

1. 使用烧录软件加载 `TopFlyingWing_Full.bin`，起始地址设为 **`0x08000000`**。
2. 仅擦除文件覆盖的 Flash 页，并启用写入后校验。
3. 文件长度固定为 **260096 字节（254 KiB）**，覆盖 `0x08000000..0x0803F7FF`，不包含配置页。
4. **保留 `0x0803F800..0x0803FFFF` 配置页，不执行整片擦除。**
5. 整包已经包含匹配 State，无需另外叠加其他版本的 State。校验成功后再复位运行。

独立通过调试器更新 App 时，须同时写入本包的 `TopFlyingWing_BootState.bin`，避免旧 State 的长度或 CRC 与新 App 不匹配。不要把 State 文件加载到 `0x08000000`。OTA 安装由设备管理启动状态，不需要上传 Full、Boot 或 State。

## 固定分区

- Boot：`0x08000000..0x08007FFF`。
- App：`0x08008000..0x0803E7FF`。
- State A：`0x0803E800..0x0803EFFF`，初始为匹配本次 App 的 `CONFIRMED` 记录。
- State B：`0x0803F000..0x0803F7FF`，初始为擦除态 `0xFF`，供后续冗余写入使用。
- 配置页：`0x0803F800..0x0803FFFF`，本包不覆盖。

## 制品校验

App 长度：{{APP_SIZE}} 字节；CRC32：`{{APP_CRC32}}`。

Full SHA-256：`{{FULL_SHA256}}`。

其他校验值以 `manifest.json` 为准。烧录软件报告擦除、写入或校验失败时，不能据“运行程序成功”判断烧录已完成。文件校验和软件测试结果不代表真实运动或 OTA 安装已成功。

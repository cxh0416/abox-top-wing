# A-BOX 顶飞翼固件

产品标识固定为 `top_flying_wing`，首版 App 为 `top-flying-wing-1.0.0`，当前构建基线为 `top-flying-wing-1.0.13`。Boot 已归属公共平台，冻结版本为 `abox-boot-2.3.0`。

公共组件、完整 Boot 边界和其他产品迁移状态见 `platform/README.md`、`platform/docs/PLATFORM_CONTRACT.md`、`platform/docs/PRODUCT_MATRIX.md`。

## 结构

- `app/`：STM32F105 App，负责 EC800、MQTT、CAN 控制器状态机和 Boot V2 OTA 下载。
- `platform/targets/stm32f105_abox_boot/`：公共平台完整 Boot 目标，只负责安装、试运行确认与回滚，不联网下载。
- `docs/vendor/`：厂家 CAN/控制器资料，仅作为设备端帧依据。
- `docs/protocols/`：按公共基础、产品业务 Profile 和内部运维接口拆分的协议文档。
- `dist/`：正式本地构建产物及校验清单。

## MQTT 接口边界

- 公共业务：顶飞翼控制、主动状态和心跳，供业务平台对接。
- 内部运维：设备信息、内存日志、受控重启、VID/MQTT 配置和 Boot V2 OTA，只供 OTA 平台使用。
- 不支持：格口映射、试开通道、锁板、RS485 和旧桌面上位机协议。

公共 MQTT 规则以平台仓库唯一维护的 [A-BOX 设备公共 MQTT 协议](https://github.com/cxh0416/abox-platform-firmware/blob/master/docs/protocols/A-BOX设备公共MQTT协议-V3.0.md) 为准，飞翼命令和状态语义见 [A-BOX 飞翼设备业务协议](docs/protocols/A-BOX飞翼设备业务协议-V3.0.md)。`top-flying-wing-1.0.13` 只接受公共 V3 请求；内部 Topic、报文和 OTA 状态语义见 [顶飞翼 A-BOX 内部运维协议](docs/protocols/顶飞翼A-BOX内部运维协议-1.0.md)。

## CAN

默认 CAN2，可通过 CMake 缓存变量 `TOP_WING_CAN_BUS=1|2` 切换。两路均配置为 500 kbps；业务使用 29 位扩展帧、发送 ID `0x1`、接收 ID `0x1`、DLC 8。当前量产控制器采用同 ID 请求/应答，厂家存档说明书中的应答 ID 自动加一规则已经过时；应用层仍会校验 DLC、命令字、状态字段和结果字段。

`top-flying-wing-1.0.13` 使用 `remote_only_idle_silent` 会话模式：完全空闲时不轮询控制器；收到上升或下降请求后执行两次 `RD` 停止预检，中间保持 250 ms 静默，再读取 `RL` 限位并启动。远程运行期间才按 100 ms 周期读取反馈，普通位置样本按 200 ms、QoS 0上报；离散状态、故障和安全停止继续使用 QoS 1。到达目标端点、反馈超时、控制器故障、运行超时或检测到控制冲突时，以单次 `ST` 收口。改变方向前，平台仍必须先明确发送 `stop` 并等待停止确认。

## 构建与验证

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_release.ps1
powershell -ExecutionPolicy Bypass -File .\tools\test_host.ps1
python .\tools\test_boot_v2_contract.py
```

上述脚本默认构建 `top-flying-wing-1.0.13`。日常发布只构建 App，并使用平台中已校验的固定 `ABox_Boot.bin` 生成维修用 Full 包。只有变更公共 Boot ABI 或同一硬件基线时，才在平台目标内单独执行 `tools/build_release.ps1`。

App 烧录地址为 `0x08008000`，Boot/Full 为 `0x08000000`。Full 只拼接到 App 实际末尾，不包含配置页和 Boot State。冻结范围和实物验收门槛见 [Boot 冻结约定](docs/BOOT_FREEZE_CONTRACT.md)。

本地构建和主机测试不代表 CAN 线束、终端电阻、电机、本地触摸、Boot 跳转或真实 OTA 已通过实物验收。

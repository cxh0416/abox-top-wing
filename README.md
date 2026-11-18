# A-BOX 顶飞翼固件

产品标识固定为 `top_flying_wing`，本分支默认 App 版本为 `top-flying-wing-1.1.18-st`；V1 对应 `top-flying-wing-1.1.18-gd`。两种硬件分别使用各自的公共冻结 Boot；已有 `dist/` 的实际版本以其清单为准。

公共组件、完整 Boot 边界和其他产品迁移状态见 `platform/README.md`、`platform/docs/PLATFORM_CONTRACT.md`、`platform/docs/PRODUCT_MATRIX.md`。

## 结构

- `app/`：V1 GD32F105 / V2 STM32F105 App，负责 EC800、MQTT、CAN 控制器状态机和 Boot V2 OTA 下载。
- `platform/targets/stm32f105_abox_boot/`：公共平台完整 Boot 目标，只负责安装、试运行确认与回滚，不联网下载。
- `platform/targets/gd32f105_abox_boot/`：GD32 V1 独立 Boot；不得与 STM32 Boot 混用。
- `docs/vendor/`：厂家 CAN/控制器资料，仅作为设备端帧依据。
- `docs/protocols/`：按公共基础、产品业务 Profile 和内部运维接口拆分的协议文档。
- `dist/`：仅保存最新正式版的 V1/V2 配对包；每次正式打包成功后整体替换，不保存历史版本或诊断包。

## MQTT 接口边界

- 公共业务：顶飞翼控制、主动状态和心跳，供业务平台对接。
- 内部运维：设备信息、内存日志、受控重启、VID/MQTT 配置和 Boot V2 OTA，只供 OTA 平台使用。
- 不支持：格口映射、试开通道、锁板、RS485 和旧桌面上位机协议。

V4 接口定义见 [A-BOX 设备公共 MQTT 协议 V4](https://github.com/cxh0416/abox-mqtt-contracts/blob/release/docs/protocols/A-BOX设备公共MQTT协议-V4.0.md) 和 [A-BOX 飞翼设备业务协议 V4](https://github.com/cxh0416/abox-mqtt-contracts/blob/release/docs/protocols/A-BOX飞翼设备业务协议-V4.0.md)，采用单实例 `wing 1.0`，只定义升降、停止、完整状态和位置采样。

`top-flying-wing-1.1.0` 使用公共 V4；旧 V3 报文和旧控制 Topic 不再作为本构建的业务入口。现有 V3 设备继续按 [飞翼业务 V3](docs/protocols/A-BOX飞翼设备业务协议-V3.0.md) 对接，平台按实际心跳的 `version` 区分。

公共通信直接复用平台的 EC800 bridge、TLS runtime、V4 请求校验/有效期、报文编码、Cloud 启动调度、重连退避和内部授时组件。新升降请求统一使用平台的持久幂等引擎及 EC800 UFS 双副本日志，飞翼只提供业务回调、存储命名空间和共享工作区；旧 Flash 记录保留只读兼容保护。TLS 通过内部 `set_mqtt` 的 `tlsEnabled` 配置，沿用 CA 校验并探测实际模组 TLS 上下文能力；内部授时仅在 TLS 连接上进行。升降需要可信授时，未完成时返回 `4003`，显式 `stop` 不受授时影响。已有 Broker 配置不会因升级被自动改写。

无有效持久配置的空板会使用公共入网服务和 STM32 身份适配，向专用 HTTPS 入网接口提交 `top_flying_wing_stm32f105_ec800_v1` 合同；不连接默认 MQTT 账号。审批后复用公共首次 MQTT 试连，连接、订阅及新心跳发布确认完成后才保存凭据并读回校验，失败则清理候选连接并继续等待入网。已有有效配置的设备直接沿用原配置。当前入网合同保持普通 MQTT 策略，可信内部授时仍要求后续 TLS 连接。

内部 Topic、TLS 配置及 OTA 状态语义见 [顶飞翼 A-BOX 内部运维协议](docs/protocols/顶飞翼A-BOX内部运维协议-1.0.md)。

## CAN

默认 CAN2，可通过 CMake 缓存变量 `TOP_WING_CAN_BUS=1|2` 切换。两路均配置为 500 kbps；业务使用 29 位扩展帧、发送 ID `0x1`、接收 ID `0x1`、DLC 8。当前量产控制器采用同 ID 请求/应答，厂家存档说明书中的应答 ID 自动加一规则已经过时；应用层仍会校验 DLC、命令字、状态字段和结果字段。

`top-flying-wing-1.1.0` 保留 `remote_only_idle_silent` 会话模式：完全空闲时不轮询控制器；收到上升或下降请求后执行两次 `RD` 停止预检，中间保持 250 ms 静默，再读取 `RL` 限位并启动。远程运行期间才按 100 ms 周期读取反馈，普通位置样本按 200 ms、QoS 0 上报；离散状态、故障和安全停止继续使用 QoS 1。到达目标端点、反馈超时、控制器故障、运行超时或检测到控制冲突时，以单次 `ST` 收口。改变方向前，平台仍必须先明确发送 `stop` 并等待停止确认；空闲时的显式 `stop` 也执行停止确认，取得反馈后才返回成功。

## 公共依赖与协议维护

首次检出后，在仓库根目录执行 `git submodule update --init --recursive`，取得本产品已固定提交的 [A-BOX 公共平台](https://github.com/cxh0416/abox-platform)。该命令不会把平台升级到最新提交；构建使用 `platform/` 中的固定版本。

公共固件组件和 Boot 在平台仓库维护；本仓库维护产品实现、CAN 和内部运维。公共 MQTT V4 与产品业务协议统一在 [A-BOX MQTT V4 协议仓](https://github.com/cxh0416/abox-mqtt-contracts) 维护，本项目引用[协议基线 `v4-2026.11.18.1`](https://github.com/cxh0416/abox-mqtt-contracts/tree/c41d04a6074c1a09df4573d741901abe730dee80)。[协议文档中心](https://github.com/cxh0416/protocol-docs) 发布已登记的协议。

## 构建与验证

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\test_host.ps1
python .\tools\build_paired_release.py --version top-flying-wing-1.1.18-st --candidate build/candidate-pair
powershell -ExecutionPolicy Bypass -File .\tools\build_release.ps1
powershell -ExecutionPolicy Bypass -File .\tools\test_v4.ps1 -ServiceRoot D:\work\device-ota-service
```

候选目录必须是 `dist/` 之外的新目录，候选不能作为正式发布或 OTA 版本。正式打包要求产品和平台工作区干净且子模块与提交一致，分别构建 V1/GD32 和 V2/STM32 App，检查各自的冻结 Boot、版本及 MCU 标记，全部通过后将 `dist/` 整体替换为 `v1/` 与 `v2/` 配对包。发布前应确认版本未在 OTA 仓库使用。

每种硬件的 Full 含 Boot、App 和匹配的 Boot State A/B，覆盖到 `0x0803F7FF`，不包含配置页 `0x0803F800`。线刷时参考对应 `dist/v1/README_FLASH.md` 或 `dist/v2/README_FLASH.md`，仅擦写文件覆盖的页；Full 会覆盖配置恢复页 `0x0803E000`，须先确认没有待恢复的配置事务。旧单版 `package_release.ps1` 已停用。

`test_v4.ps1` 使用平台统一预检，检查公共/产品回归以及实际固件报文、入网合同与服务端源码的兼容性。无保留要求的测试板必须使用公共整片擦除入口，从全空 MCU 配置开始入网验收；操作及存储边界见[平台内部接入流程](platform/docs/internal/new_product_onboarding.md#v4-统一预检与空板基线)。保留配置的升级、重启恢复另行验证。

通信主循环由公共 `ABoxAppService` 统一驱动，`wing_app_service.c` 仅绑定 UART、产品轮询和工作区准入，不另写模组恢复流程。`test_wing_app_service` 编译真实薄适配与公共入口，验证 UART 失败、物理复位通知、幂等工作区保护及轮询顺序；它作为统一预检的 `LifecycleTest`，与协议报文、身份合同一起检查。

2026-10-26，标准空板 `BENCH-SW-054363` 已验证公共入口的 HTTP 故障自动恢复，恢复后保留同一入网申请，MCU 不复位；已有入网验收通过后，连续完成 OTA 到 `top-flying-wing-1.1.6`。审计任务 `44b90921-92e6-4431-a2aa-5259029e6c8d` 为 `completed`，独立查询 `ce019a14-6a3b-4726-a0f4-501e305940df` 确认目标版本、Boot `CONFIRMED`、OTA `IDLE`。公共 70 项、产品 26 项主机测试与 ARM Release 通过；本轮验证范围为通信、恢复和 OTA，未发送业务动作命令。后续常规回归保留入网配置，不重复要求审批。

本地构建和主机测试不代表 CAN 线束、终端电阻、电机、本地触摸、Boot 跳转或真实 OTA 已通过实物验收。

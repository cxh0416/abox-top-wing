# 公共授时简化与本产品验证记录

内部资料，不作为业务对接协议对外交付。

## 变更

- 公共授时使用固定默认关联前缀和 RAM 序号；允许重启复用 ID，同次运行内重连和身份切换保留序号。产品侧不再使用备份域、UID 或时间戳构造授时编号。
- 公共时钟提供 `ABoxClock_ReportedUtcMs`，未知、CCLK 启动参考、过期时间均上报 0。产品 UTC 格式化在没有可信时间时正常输出 `0`。
- 保留原采样和重试参数；最大 RTT 调整为 800 ms，并统一检查单样本误差不超过原最坏值 451 ms、时钟总误差不超过 500 ms；非法响应不取消当前等待。V4 接线显式选择清洁 MQTT 会话，旧调用方默认不改模组设置。
- 服务端 0.4.196 按本次入站 trace ID 关联授时回复，不以历史同名 ID 判定本次重复或计算延迟；入站记录失败不猜测历史关联，也不阻断回复。
- OTA 本地稳定槽初始化与 MQTT 启动的独立修复保留在提交 `b9e5c72`，不依赖可信授时。

请求编号仅用于配对，不保证识别跨重启且恰好同 ID 的延迟旧响应。清洁会话和本地连接代次也不提供该保证。

## 构建与自动测试

- 公共平台主机测试：70/70；额外验证旧 transport 接线不发送 session 配置命令。
- Top Flying Wing 主机测试：26/26；Boot V2 分区/描述符及产品与服务端 V4 合同验证通过。
- 服务端相关测试：43 项、11 个子测试通过；Linux 发布目录内授时单元/集成测试 19 项通过。
- 送餐车、巡防底盘、清扫车现有授时适配源文件，使用新公共头文件进行 `gcc -std=c11 -fsyntax-only -Wall -Werror` 检查通过；未更新其平台引用、未烧录其硬件。
- 候选 App：`top-flying-wing-1.1.8-test4`，133884 字节，RAM 58112 字节，SHA-256 `78cfd12882466a540696fafb2ad6c64ff0692727758d837ef32d36e732cd4241`。
- 烧录并回读验证 App 与状态区域 `0x08008000..0x0803F7FF`；Boot `0x08000000`、配置页 `0x0803F800` 保留。正式 `dist/` 未替换。
- 服务端源码提交 `a1d0cf9` 经标准 build/stage/activate 发布到 0.4.196，API 与 MQTT worker 均 active。

## 上板证据

设备 VID `BENCH-SW-054363`，STM32F105RC，EC800；证据与临时工具位于仓库 `build/hardware/time_sync/`。

test3 验证中捕获实际 RTT 478 ms，因旧 400 ms 上限而持续拒绝授时；保留为 `reset-6-initial-timeout.json`。当时五次独立查询覆盖 120 秒，全部返回 200，trustedTime=false、mqttTlsActive=true、otaBusy=false、otaPhase=IDLE，设备在线与 last_seen 持续推进。此证据说明失败不阻塞普通业务，也促成按误差预算接纳较慢样本的调整。

test4 最终验证结果如下；test3 的五次成功复位不计入本表。

| 场景 | 结果与实际证据 |
|---|---|
| 软件复位 | 10/10 重新联网并校时；每次先观察新 App 清除旧时钟状态。有效样本 RTT 435～554 ms，应用时估计误差 250～308 ms。 |
| MQTT 断线恢复 | 5/5 恢复并重新校时；从序号 7 连续推进到 10、13、16、19、22，未重置。 |
| 两分钟授时应用失败 | 设备始终保持 MQTT READY，后台采样序号由 23 推进到 34；期间五次独立 get_info 全部返回 200，trustedTime=false、mqttTlsActive=true、otaBusy=false、otaPhase=IDLE，设备在线且 last_seen 持续推进。 |
| 故障解除 | 恢复原回调后不复位自动校时，序号推进到 37；设备日志 `I TIME platform rtt=432 err=247`。 |
| 真正断电冷启动 | 用户将 MCU 板断电约 5 秒后重新上电，重新联网并校时；设备日志 `I TIME platform rtt=521 err=294`。新同步 monotonic 为 27378 ms，区别于断电前的运行。 |

故障注入仅临时修改此候选 ELF 对应的 RAM apply 回调，令其返回失败，Flash 与服务端均未修改；finally 恢复原回调。断电验证后读回回调仍为原值 `0x0800F0F9`。这项板测验证校时应用失败；丢包、超时、非法响应等路径由公共组件测试覆盖。

服务端候选测试期间的 65 次授时回复全部配对到本次入站记录，未误标历史重复，最大服务端记录延迟 26 ms；这些记录只说明服务端配对/发布，不代表设备接受校时。设备接受结论来自上述时钟状态和 TIME 日志。校时前报文 timestamp=0，最终心跳 timestamp=1793325474576，设备在线。

最终独立回读 operation `56886151-f1d0-4630-a49e-1a11b8a605fb`：fwVersion=top-flying-wing-1.1.8-test4、trustedTime=true、mqttTlsActive=true、otaBusy=false、otaPhase=IDLE、otaReady=true、otaState=CONFIRMED。该 OTA 状态为本地稳定槽状态，本轮未执行 OTA 安装或运动测试。

J-Link V7.94j / ARM V8 探针在 DLL 连接尝试中报告探针堆内存不足，需要 USB 重新上电；此报错不表示 MCU RAM 不足。后续改用无 GUI 的原生 Commander 脚本，每次显式恢复运行。批量复位必须先观察 App 清除旧时钟状态，再判定新的校时成功，避免复位后残留 RAM 造成假阳性。

## 对应源码与产物

公共平台引用 `0911f1e`；产品授时接线提交 `488029a`；独立 OTA 启动修复 `b9e5c72`；服务端修复 `a1d0cf9` / 已部署 0.4.196。候选二进制、回读、复位/重连/故障/冷启动 JSON、服务端只读记录与 CLI 回读保留在 `build/hardware/time_sync/`，正式 dist 未更新。

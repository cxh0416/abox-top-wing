# 顶飞翼项目资料

## 公共业务协议

- [A-BOX 设备公共 MQTT 协议 V4](https://github.com/cxh0416/abox-platform/blob/main/docs/protocols/A-BOX设备公共MQTT协议-V4.0.md)：公共报文、Topic、能力声明、QoS、有效期、幂等和状态同步规则。
- [A-BOX 飞翼设备业务协议 V4](protocols/A-BOX飞翼设备业务协议-V4.0.md)：`wing 1.0` 的升降、停止、完整状态和位置采样接口。
- 现有 V3 对接：[公共 MQTT V3](https://github.com/cxh0416/abox-platform/blob/main/docs/protocols/A-BOX设备公共MQTT协议-V3.0.md)、[飞翼业务 V3](protocols/A-BOX飞翼设备业务协议-V3.0.md)。

飞翼设备公共业务对接方只需阅读对应版本的公共 MQTT 协议和飞翼业务协议。

## 固件与内部维护

- [4G 通信报文与平台迁移代码索引](implementation/4G通信报文与平台迁移代码索引.md)：当前 MQTT/HTTPS 报文、用途、源码位置和平台迁移依赖；仅供内部分析。
- [WL300D 八路超声波报警](implementation/WL300D超声波报警实现.md)：CAN1 数据、共享报警输出、解除延时、构建参数和验证记录。
- [2026-06-15 A-BOX 新板引脚核对](implementation/2026-06-15%20A-BOX新板引脚核对.md)：隔离输入、CAN、电源输出和 LED 与当前 MCU 引脚映射。

本仓库构建基线 `top-flying-wing-1.1.0` 使用公共 V4，并接入公共 TLS 与内部授时。设备现场使用的协议以实际运行固件及心跳版本为准。

- [顶飞翼 A-BOX 内部运维协议](protocols/顶飞翼A-BOX内部运维协议-1.0.md)：设备信息、日志、重启、VID/MQTT 配置和 Boot V2 OTA；文件名为兼容既有引用而保留，正文当前修订为 1.3。
- [飞翼控制实现与安全验收](implementation/飞翼控制实现与安全验收.md)：固件内部控制流程、诊断扩展、控制器故障和硬件验收边界；不属于平台协议。
- [防夹条报警与避让](implementation/防夹条报警实现.md)：PA7 输入、下降全行程防夹、PA0 蜂鸣器、停止确认后受上限约束的 20 mm 避让及释放后 1 秒报警。
- [网页控制与 CAN 现场排查](implementation/2026-10-29网页控制与CAN排查.md)：可信授时、停止未确认和 CH1 只监听的现场证据。
- [厂家 CAN 协议与代码核对](implementation/2026-10-29厂家CAN协议与代码核对.md)：发送字节、应答解析、ID 差异及发送完成语义。
- [CAN 键盘台架控制](../tools/CAN键盘控制说明.md)：分析仪 CH1 的 W/S/空格控制、霍尔位置反馈及限位门禁。
- `vendor/四通道 两通道大功率控制器说明书(MS-04-25A-A).pdf`：同步控制器产品说明书。
- `vendor/CAN2.0 扩展帧通讯协议1.2.pdf`：A-BOX 与同步控制器之间的厂家通信协议。

内部运维协议只供受控 OTA 平台使用。`vendor/` 资料用于设备端实现和硬件联调，其中的底层通信细节不属于平台接口。

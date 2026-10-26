# 顶飞翼项目资料

## 公共业务协议

- [A-BOX 设备公共 MQTT 协议 V4](https://github.com/cxh0416/abox-platform/blob/main/docs/protocols/A-BOX设备公共MQTT协议-V4.0.md)：公共报文、Topic、能力声明、QoS、有效期、幂等和状态同步规则。
- [A-BOX 飞翼设备业务协议 V4](protocols/A-BOX飞翼设备业务协议-V4.0.md)：`wing 1.0` 的升降、停止、完整状态和位置采样接口。
- 现有 V3 对接：[公共 MQTT V3](https://github.com/cxh0416/abox-platform/blob/main/docs/protocols/A-BOX设备公共MQTT协议-V3.0.md)、[飞翼业务 V3](protocols/A-BOX飞翼设备业务协议-V3.0.md)。

飞翼设备公共业务对接方只需阅读对应版本的公共 MQTT 协议和飞翼业务协议。

## 固件与内部维护

本仓库构建基线 `top-flying-wing-1.1.0` 使用公共 V4，并接入公共 TLS 与内部授时。设备现场使用的协议以实际运行固件及心跳版本为准。

- [顶飞翼 A-BOX 内部运维协议](protocols/顶飞翼A-BOX内部运维协议-1.0.md)：设备信息、日志、重启、VID/MQTT 配置和 Boot V2 OTA；文件名为兼容既有引用而保留，正文当前修订为 1.3。
- [飞翼控制实现与安全验收](implementation/飞翼控制实现与安全验收.md)：固件内部控制流程、诊断扩展、控制器故障和硬件验收边界；不属于平台协议。
- `vendor/四通道 两通道大功率控制器说明书(MS-04-25A-A).pdf`：同步控制器产品说明书。
- `vendor/CAN2.0 扩展帧通讯协议1.2.pdf`：A-BOX 与同步控制器之间的厂家通信协议。

内部运维协议只供受控 OTA 平台使用。`vendor/` 资料用于设备端实现和硬件联调，其中的底层通信细节不属于平台接口。

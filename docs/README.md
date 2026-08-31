# 顶飞翼项目资料

- [A-BOX 设备公共 MQTT 协议 V3](https://github.com/cxh0416/abox-platform-firmware/blob/master/docs/protocols/A-BOX设备公共MQTT协议-V3.0.md)：由 ABox_Platform 唯一维护，定义 `deviceType`、QoS、幂等、公共响应码和心跳规则。
- [A-BOX 飞翼设备业务协议 V3](protocols/A-BOX飞翼设备业务协议-V3.0.md)：适用于任何使用飞翼执行设备的项目，定义精简控制响应、主动状态、故障和安全停止语义。
- [顶飞翼 A-BOX 内部运维协议](protocols/顶飞翼A-BOX内部运维协议-1.0.md)：设备信息、日志、重启、VID/MQTT 配置和 Boot V2 OTA；文件名为兼容既有引用而保留，正文当前修订为 1.2。
- `vendor/四通道 两通道大功率控制器说明书(MS-04-25A-A).pdf`：同步控制器产品说明书。
- `vendor/CAN2.0 扩展帧通讯协议1.2.pdf`：A-BOX 与同步控制器之间的厂家通信协议。

飞翼设备公共业务对接方只需阅读设备公共 MQTT 协议和飞翼设备业务协议；内部运维协议只供受控 OTA 平台使用。当前 `top-flying-wing-1.0.13` 已实现公共 V3。`vendor/` 资料用于设备端实现和硬件联调，其中的底层通信细节不属于平台接口。

# A-Box Boot 冻结约定

## 归属与适用范围

完整 Boot 工程归属公共平台目标 `platform/targets/stm32f105_abox_boot`，顶飞翼产品仓库只维护 App、产品业务和发布组合逻辑。

当前通用范围限定为以下硬件契约：

- MCU：STM32F105RCT6，256 KiB Flash、64 KiB RAM。
- EC800 控制：USART1；`PA8=EC_MAIN_RI`、`PC7=EN_3.8V`、`PC8=EC_PWR`、`PC9=EC_DTR`。
- Boot：`0x08000000..0x08007FFF`；App：`0x08008000..0x0803E7FF`。
- Boot State：`0x0803E800`、`0x0803F000`；设备配置：`0x0803F800`。
- Boot/App 故障邮箱：RAM `0x2000BC40`。

不同 MCU、Flash 容量、EC800 UART/电源引脚或上述地址布局，不得直接复用该二进制。

## 冻结边界

`abox-boot-2.3.0` 只从 EC800 UFS 安装 App、维护 Boot State、执行试运行确认和回滚。HTTPS、CA、默认 `QHTTPREADFILE` 直写 UFS、1024 字节 Range 回退和产品 OTA Topic 全部由 App 负责。产品发布默认只重建 App；维修 Full 包必须逐字节使用平台 `dist/ABox_Boot.bin`，并在产品清单中记录其 SHA-256。

Boot 后续变更必须作为公共平台变更处理，同时更新版本、描述符 CRC、冻结二进制和清单。不得在单个产品仓库复制或局部修改 Boot。

## 冻结前实物门槛

当前构建、主机测试和静态校验只证明源码、布局和制品一致性。正式冻结还必须在目标板完成：

1. 擦除后分别烧录 Boot/App，核对 `0x08000000` 和 `0x08008000` 的 Flash 回读。
2. 采集 UART5 Boot 日志，验证正常跳转、无 App、损坏向量和复位原因路径。
3. 完成真实 UFS OTA、试运行确认、连续三次失败回滚，以及擦除/写入中途断电恢复。
4. 确认两页 Boot State 轮换，且 `0x0803F800` 配置页在安装与回滚中保持不变。

以上实物证据齐全后，才可把该 SHA-256 标记为生产冻结版本。

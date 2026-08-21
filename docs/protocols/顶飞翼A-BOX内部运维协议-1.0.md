# 顶飞翼 A-BOX 内部运维协议

版本：1.0  
适用固件：`top-flying-wing-1.0.0`  
产品标识：`top_flying_wing`

## 1. 协议定位

本文档描述 OTA 平台与顶飞翼 A-BOX 固件之间已经实现的内部运维接口，包括设备信息、内存日志、受控重启、VID/MQTT 配置和 Boot V2 OTA。

这些接口使用 `/internal/*` Topic，只供受控运维平台使用，不属于《车载 A-BOX 设备对接协议》中的公共业务接口。业务平台控制顶飞翼时仍只使用 `/ctrl`、`/ctrl/resp`、`/status/report` 和 `/heartbeat`，不得通过内部运维接口替代业务控制。

当前固件没有格口映射、试开通道、锁板、RS485 或旧桌面上位机接口。

## 2. 通用约定

### 2.1 Topic

`{deviceId}` 为设备当前 VID，按平台登记值原样替换。

| 方向 | 用途 | Topic | QoS | Retain |
|---|---|---|---:|---:|
| 平台 → A-BOX | 内部配置和查询 | `/zxwl/abox/{deviceId}/internal/config` | 1 | 否 |
| A-BOX → 平台 | 内部配置响应 | `/zxwl/abox/{deviceId}/internal/config/resp` | 1 | 否 |
| 平台 → A-BOX | Boot V2 OTA | `/zxwl/abox/{deviceId}/internal/ota` | 1 | 否 |
| A-BOX → 平台 | Boot V2 OTA 响应 | `/zxwl/abox/{deviceId}/internal/ota/resp` | 1 | 否 |
| A-BOX → 平台 | 增量运行日志 | `/zxwl/abox/{deviceId}/internal/log` | 0 | 否 |

A-BOX 只订阅自身精确的配置和 OTA 请求 Topic。内部响应优先于主动状态、心跳和日志发送；日志属于最低优先级的尽力上报。

### 2.2 请求结构

```json
{
  "version": "2.0",
  "timestamp": 1787292000000,
  "data": {
    "requestId": "952f0264-08d3-40a3-b557-767c974e23fb",
    "command": "internal_config",
    "params": {
      "action": "get_info"
    }
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---:|---|
| `version` | String | 是 | 固定为 `2.0` |
| `timestamp` | Integer | 是 | Unix 毫秒时间戳 |
| `data.requestId` | String | 是 | 非空请求 ID，长度小于 40 字节 |
| `data.command` | String | 是 | `internal_config` 或 `ota_upgrade` |
| `data.params` | Object | 是 | 命令参数 |
| `data.product` | String | 否 | 当前内部请求为兼容既有 A-BOX V2 平台允许省略；若携带应为 `top_flying_wing` |

内部请求当前不依赖 `data.product` 做设备授权，设备身份由精确 Topic 中的 `{deviceId}` 和受控 Broker 权限确定。公共业务请求仍必须携带并校验 `data.product`。

### 2.3 响应结构

```json
{
  "version": "2.0",
  "timestamp": 1787292000123,
  "data": {
    "product": "top_flying_wing",
    "requestId": "952f0264-08d3-40a3-b557-767c974e23fb",
    "code": 200,
    "msg": "operation accepted",
    "result": null
  }
}
```

所有内部响应必须携带 `data.product=top_flying_wing`。平台必须同时按 Topic、`product` 和 `requestId` 关联响应，避免共享 A-BOX Broker 上不同产品的消息互相串用。

| 响应码 | 含义 |
|---:|---|
| `200` | 操作完成，或 OTA 镜像已校验并写入待安装状态 |
| `202` | MQTT 新配置试用流程已启动，最终是否保存尚未确定 |
| `4001` | 报文格式或 OTA 参数错误 |
| `4002` | 未知的内部命令或动作 |
| `4003` | OTA 下载或相关资源正在占用 |
| `500` | 内部处理失败、OTA 尚未就绪、配置试用无法启动或 OTA 下载校验失败 |

无法解析 JSON、缺少有效 `data` 或缺少有效 `requestId` 时，当前固件直接丢弃报文，不保证生成错误响应。

### 2.4 重复投递

公共顶飞翼控制具有设备侧 `requestId` 去重缓存；内部运维请求当前没有同等级别的设备侧通用去重缓存。平台必须使用自身的幂等键、操作串行化和响应关联避免重复创建内部操作。OTA 处理期间由忙状态阻止并发下载，但不能据此把所有内部命令视为天然幂等。

## 3. 内部配置与查询

请求使用 `command=internal_config`，具体操作由 `params.action` 指定。

| `action` | 用途 | 当前 OTA 平台入口 |
|---|---|---|
| `get_info` | 查询设备、Boot、OTA、MQTT 和 CAN 基本信息 | 已接入 |
| `get_log` | 读取最近的内存日志快照 | 已接入 |
| `clear_log` | 清空设备内存日志 | 固件支持，平台当前不提供按钮 |
| `reboot` | 响应发布成功后受控重启 | 已接入，需二次确认 |
| `set_mqtt` | 试用并保存新的 VID/MQTT 配置 | 由平台“修改 VID”流程受控调用 |

### 3.1 查询设备信息

请求：

```json
{
  "version": "2.0",
  "timestamp": 1787292000000,
  "data": {
    "requestId": "a47f4ad8-0bdc-44f2-9044-106333fc0b15",
    "command": "internal_config",
    "params": {
      "action": "get_info"
    }
  }
}
```

成功响应：

```json
{
  "version": "2.0",
  "timestamp": 1787292000123,
  "data": {
    "product": "top_flying_wing",
    "requestId": "a47f4ad8-0bdc-44f2-9044-106333fc0b15",
    "code": 200,
    "msg": "operation accepted",
    "result": {
      "vid": "TFW000001",
      "product": "top_flying_wing",
      "fwVersion": "top-flying-wing-1.0.0",
      "bootVersion": "top-flying-wing-boot-2.2.0",
      "bootDescriptorValid": true,
      "bootApiVersion": 1,
      "bootFeatures": 15,
      "otaScheme": "boot_v2_ufs",
      "otaDownloader": "app",
      "otaState": "CONFIRMED",
      "otaPhase": "idle",
      "otaReady": true,
      "otaBusy": false,
      "mqttHost": "36.153.162.171",
      "mqttPort": 19683,
      "canBus": 2,
      "canBitrate": 500000,
      "feedbackTimeoutMs": 500,
      "commandTimeoutMs": 1000,
      "maxRuntimeMs": 120000
    }
  }
}
```

`bootVersion` 只有在 Boot descriptor 校验有效时可信；平台应结合 `bootDescriptorValid` 判断。响应不返回 MQTT 用户名、密码或 OTA CA 内容。

### 3.2 获取内存日志

```json
{
  "version": "2.0",
  "timestamp": 1787292000000,
  "data": {
    "requestId": "b827e9f4-e3e0-40e3-bd0b-f280e66c993f",
    "command": "internal_config",
    "params": {
      "action": "get_log"
    }
  }
}
```

成功时 `data.result` 为日志字符串，不是对象或数组：

```json
{
  "version": "2.0",
  "timestamp": 1787292000123,
  "data": {
    "product": "top_flying_wing",
    "requestId": "b827e9f4-e3e0-40e3-bd0b-f280e66c993f",
    "code": 200,
    "msg": "operation accepted",
    "result": "[2026-08-21 14:00:00.000] I APP boot\r\n[2026-08-21 14:00:01.125] I MQTT connected\r\n"
  }
}
```

固件使用 32 行、每行 80 字节的 RAM 环形日志。`get_log` 响应缓冲区最多保留约 639 字节，空间不足时只返回能够完整装入的较新日志；AT 收发等高频噪声会被过滤。设备重启会清空内存日志。

### 3.3 清空内存日志

`params.action=clear_log`。成功返回 `code=200`、`result=null`。该动作只清空 RAM 日志，不删除平台已经保存的历史操作或日志记录。

### 3.4 受控重启

`params.action=reboot`。设备先排队发布 `code=200` 的 QoS 1 响应，确认响应发布完成后延迟约 200 ms 重启；极端情况下最迟约 3 秒强制重启。`200` 表示设备已接受重启，不表示重启后的心跳已经恢复，平台仍需等待新的心跳。

### 3.5 修改 VID/MQTT 配置

```json
{
  "version": "2.0",
  "timestamp": 1787292000000,
  "data": {
    "requestId": "ba4812fb-99cd-4ad5-917d-0fd56ed1357c",
    "command": "internal_config",
    "params": {
      "action": "set_mqtt",
      "vid": "TFW000002",
      "host": "36.153.162.171",
      "port": 19683,
      "username": "configured-by-platform",
      "password": "configured-by-platform"
    }
  }
}
```

未提供的配置项沿用设备当前值。设备先通过旧连接返回 `202`，确认该响应发布成功后才切换到新配置进行最长 60 秒的连接试用：

1. 新连接完成一次有效 MQTT 发布后，设备把新配置写入 Flash。
2. 60 秒内没有完成有效发布时，设备恢复原配置并重新连接。
3. 修改 VID 后，后续 Topic 使用新 VID；平台必须等待新 VID 心跳，不能仅凭 `202` 删除旧设备身份。

用户名和密码属于敏感配置，不得写入日志、审计详情或普通业务文档。平台界面的“修改 VID”只传递新的 VID，其余 Broker 参数由服务器已安装配置补齐。

## 4. 增量运行日志

A-BOX 在没有待发响应、心跳、状态或 OTA 工作时，从 RAM 日志队列读取一行并发布：

```json
{
  "version": "2.0",
  "timestamp": 1787292030000,
  "data": {
    "product": "top_flying_wing",
    "lines": [
      "[2026-08-21 14:00:30.000] W CAN controller feedback timeout\r\n"
    ]
  }
}
```

每次报文当前只包含一行，发送间隔不短于 30 秒，使用 QoS 0 且不 Retain。它用于辅助排障，不保证完整、有序或必达，不能作为控制响应、故障状态或审计记录的替代品。

## 5. Boot V2 OTA

### 5.1 请求

```json
{
  "version": "2.0",
  "timestamp": 1787292000000,
  "data": {
    "requestId": "4aeecdec-71b4-451f-bbc9-2f01c166b4a7",
    "command": "ota_upgrade",
    "params": {
      "url": "https://ota.zxwl.ntiov.com:20443/fw-revisions/top_flying_wing/top-flying-wing-1.0.0/top_flying_wing_app.bin",
      "version": "top-flying-wing-1.0.0",
      "size": 67828,
      "crc32": "F0BE8E81"
    }
  }
}
```

| 参数 | 类型 | 约束 |
|---|---|---|
| `url` | String | 必须精确匹配 `https://ota.zxwl.ntiov.com:20443/fw-revisions/top_flying_wing/{version}/top_flying_wing_app.bin` |
| `version` | String | 必须以 `top-flying-wing-` 开头，长度不超过固件字段上限 |
| `size` | Integer | 大于 0 且不超过 `0x36800` 字节 |
| `crc32` | String | 8 位十六进制字符串，不带 `0x` |

当前 OTA 只安装 App revision。`TopFlyingWing_Boot.bin` 和 `TopFlyingWing_Full.bin` 是工厂烧录或维修产物，不通过本 Topic 下发。

### 5.2 执行和响应语义

设备使用内置 CA、HTTPS 和 EC800 UFS 分段下载镜像，随后校验长度、CRC32 和向量表。只有全部校验通过并把 Boot State 写成 `INSTALL_PENDING` 后，才返回：

```json
{
  "version": "2.0",
  "timestamp": 1787292060000,
  "data": {
    "product": "top_flying_wing",
    "requestId": "4aeecdec-71b4-451f-bbc9-2f01c166b4a7",
    "code": 200,
    "msg": "operation accepted",
    "result": null
  }
}
```

该 `200` 仅表示候选镜像已下载、校验并登记为待安装，设备随后安排重启；不表示 Boot 已完成安装，也不表示新 App 已进入 `CONFIRMED`。

下载或校验失败返回 `500`，`result.error` 携带设备内部错误号。平台应保存原始错误号用于排障，不应把它解释为公共业务响应码。

### 5.3 安装状态

`get_info` 的 `otaState` 可能为：

| 状态 | 含义 |
|---|---|
| `NORMAL` | 没有有效的 Boot V2 状态记录 |
| `INSTALL_PENDING` | App 已下载并等待 Boot 安装 |
| `INSTALLING` | Boot 正在安装候选镜像 |
| `TRIAL` | 新 App 试运行，尚未确认 |
| `CONFIRMED` | 新 App 已确认 |
| `ROLLBACK` | 试运行失败或已进入回滚流程 |

新 App 进入 `TRIAL` 后，固件必须持续运行至少 30 秒、重新连上 MQTT，并成功发送新的心跳，才写入 `CONFIRMED`。平台创建 OTA 任务时必须继续等待设备上报的新版本和 `CONFIRMED`，不得用 `/internal/ota/resp` 的 `200` 提前完成任务。

## 6. 平台安全要求

1. 内部 Topic 只能由受控运维平台和目标 A-BOX 账号访问，不向普通业务调用方开放。
2. 重启、修改 VID 和创建 OTA 任务继续受角色、在线状态、二次确认、幂等、串行化和审计规则约束。
3. 日志 Topic 不用于刷新设备在线时间；A-BOX 在线状态仍只由 `/heartbeat` 判定。
4. OTA revision 必须先在平台发布为不可变文件，再创建设备任务；发布 revision 本身不得自动创建 OTA 任务。
5. 内部接口成功不等于实物动作、Boot 跳转或真实 OTA 已验证，最终结论仍以设备心跳、版本、Boot 状态和必要的现场验收为准。

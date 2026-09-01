# A-BOX 飞翼设备业务协议

文档修订：V3.0

报文版本：`3.0`

设备类型：`wing_actuator`

当前核对固件：`top-flying-wing-1.0.14`

## 1. 协议范围

本文档只定义平台与飞翼设备之间稳定、可观察的公共业务接口，适用于所有使用该设备能力的项目。固件产品名、厂家 CAN、控制器轮询、预检步骤、内部状态机、超时配置和故障恢复算法不属于公共平台协议。

通用报文、QoS、心跳、公共响应码和请求幂等遵循平台仓库的 [A-BOX 设备公共 MQTT 协议](https://github.com/cxh0416/abox-platform-firmware/blob/master/docs/protocols/A-BOX设备公共MQTT协议-V3.0.md)。设备信息、日志、配置和 OTA 见 [顶飞翼 A-BOX 内部运维协议](顶飞翼A-BOX内部运维协议-1.0.md)。

平台只需实现以下 4 个公共 Topic：

| 方向 | 用途 | Topic | QoS | Retain |
|---|---|---|---:|---:|
| 平台 → A-BOX | 飞翼控制 | `/zxwl/abox/{deviceId}/ctrl` | 1 | 否 |
| A-BOX → 平台 | 控制响应 | `/zxwl/abox/{deviceId}/ctrl/resp` | 1 | 否 |
| A-BOX → 平台 | 主动状态 | `/zxwl/abox/{deviceId}/status/report` | 按报告类型为 0 或 1 | 是 |
| A-BOX → 平台 | 公共心跳 | `/zxwl/abox/{deviceId}/heartbeat` | 0 | 否 |

本 Profile 只有 `wing_control` 命令，不定义主动状态查询。

## 2. 平台可见语义

| 动作 | 含义 |
|---|---|
| `ascend` | 请求上升 |
| `descend` | 请求下降 |
| `stop` | 请求停止当前动作，并等待设备确认停止 |

平台必须遵守以下接口规则：

1. 改变运动方向前必须先发送 `stop`；只有收到停止成功响应并观察到 `motionState=stopped`，才能发送反方向请求。
2. `ascend` 或 `descend` 返回 `code=200` 表示设备已接受动作，不表示机械动作已经完成；平台必须继续观察 `/status/report`。
3. `stop` 返回 `code=200` 表示设备已确认停止；未成功响应时，平台不得假定机械已停止。
4. 设备繁忙、状态不足或存在故障时可以拒绝新动作；平台不得绕过错误码连续下发。
5. 相同 `requestId` 的重复投递返回同一业务结果，不代表动作再次执行。

设备如何完成预检、下发控制、确认停止和处理安全异常属于固件实现，不构成平台依赖。

## 3. 控制请求与响应

请求 Topic：`/zxwl/abox/{deviceId}/ctrl`

响应 Topic：`/zxwl/abox/{deviceId}/ctrl/resp`

### 3.1 请求

```json
{
  "version": "3.0",
  "timestamp": 1770186088333,
  "data": {
    "deviceType": "wing_actuator",
    "requestId": "3fa85f64-5717-4562-b3fc-2c96063f8666",
    "command": "wing_control",
    "params": {
      "action": "ascend"
    }
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---:|---|
| `data.command` | String | 是 | 固定为 `wing_control` |
| `data.params.action` | String | 是 | `ascend`、`descend` 或 `stop` |

### 3.2 响应

```json
{
  "version": "3.0",
  "timestamp": 1770186089456,
  "data": {
    "deviceType": "wing_actuator",
    "requestId": "3fa85f64-5717-4562-b3fc-2c96063f8666",
    "code": 200,
    "msg": "operation accepted"
  }
}
```

响应不包含 `result`。平台以 `code` 判断受理结果，`msg` 只用于展示和诊断；当前状态、动作完成和异步故障均从 `/status/report` 获取。

## 4. 响应码

| 响应码 | 业务含义 |
|---:|---|
| `200` | 请求成功；升降表示已接受，停止表示已确认停止 |
| `4001` | 参数、动作非法或 `requestId` 冲突 |
| `4002` | 不支持该命令 |
| `4003` | 设备忙或当前存在互斥动作 |
| `4004` | `deviceType` 不匹配 |
| `5001` | A-BOX 内部异常 |
| `5002` | 下游控制器不可用 |
| `5003` | 当前状态不足以安全接受动作，或确认超时 |
| `5004` | 下游控制器拒绝执行 |
| `5005` | 下游控制器存在故障 |

平台应保留未知响应码和原始 `msg`，不得套用其他设备类型的含义。

## 5. 主动状态报告

Topic：`/zxwl/abox/{deviceId}/status/report`

方向：A-BOX → 平台

Retain：是

### 5.1 状态字段

```json
{
  "version": "3.0",
  "timestamp": 1770186088333,
  "data": {
    "deviceType": "wing_actuator",
    "reportId": "V000001-1770186088333-1024",
    "reportReason": "periodic",
    "motionState": "ascending",
    "activeAction": "ascend",
    "positionMm": 126,
    "topLimitMm": 250,
    "bottomLimitMm": 1,
    "atEndpoint": null,
    "faultCode": null,
    "controllerOnline": true,
    "statusFresh": true,
    "lastRequestId": "3fa85f64-5717-4562-b3fc-2c96063f8666",
    "safetyStopReason": null
  }
}
```

| 字段 | 类型 | 平台含义 |
|---|---|---|
| `reportId` | String | 逻辑报告唯一标识；QoS 重投保持不变 |
| `reportReason` | String | 本次报告的触发原因 |
| `motionState` | String | `unknown`、`stopped`、`ascending`、`descending`、`moving_unknown`、`fault` 或 `offline` |
| `activeAction` | String / null | 当前已接受且尚未结束的 `ascend` 或 `descend` |
| `positionMm` | Integer / null | 设备提供的当前位置；未知时为 `null` |
| `topLimitMm` | Integer / null | 最高限位；未知时为 `null` |
| `bottomLimitMm` | Integer / null | 最低限位；未知时为 `null` |
| `atEndpoint` | String / null | `top`、`bottom` 或 `null` |
| `faultCode` | String / null | 设备原始故障码；无故障时为 `null` |
| `controllerOnline` | Boolean / null | 下游控制器是否在线；无法判断时为 `null` |
| `statusFresh` | Boolean | `false` 表示位置和限位不是当前新鲜反馈 |
| `lastRequestId` | String / null | 与本状态相关的最近一次已接受控制请求 |
| `safetyStopReason` | String / null | 安全停止原因；未触发时为 `null` |

平台不得把 `unknown`、`moving_unknown`、`fault` 或 `offline` 当作已停止，也不得在 `statusFresh=false` 时把位置和限位展示为实时数据。`faultCode` 和未知枚举应原样保存，具体硬件故障解释属于设备维护资料。

固件可能按公共协议的兼容规则附加诊断字段。平台可以保存原始报文，但不得依赖这些字段完成基本控制和状态展示。

### 5.2 报告类型与频率

| `reportReason` | QoS | 触发条件 |
|---|---:|---|
| `periodic` | 0 | 运动期间按 200 ms 目标周期发送最新位置快照 |
| `state_changed` | 1 | 运动状态或动作变化 |
| `controller_offline` | 1 | 下游控制器离线或状态超时 |
| `controller_online` | 1 | 下游控制器恢复在线 |
| `fault_raised` | 1 | 故障产生或故障码变化 |
| `fault_cleared` | 1 | 故障解除 |
| `safety_stop` | 1 | 设备触发安全停止 |
| `endpoint_stop` | 1 | 到达目标端点并停止 |
| `control_conflict` | 1 | 检测到控制冲突 |

平台订阅该 Topic 时请求 QoS 1。平台必须按 `reportId` 去重，并阻止较旧 `timestamp` 覆盖较新状态；QoS 0 周期样本允许丢失和抖动。Retain 报告只表示最近状态，不替代心跳在线判定。

## 6. 平台最小验收

- 只实现本 Profile 声明的 4 个公共 Topic；
- 控制请求以及响应、状态订阅使用 QoS 1；
- 改变方向前明确 `stop`，并等待停止成功响应和停止状态；
- 控制响应只按 `code` 判断，不读取 `result`；
- 不把升降请求的 `code=200` 展示为机械动作完成；
- 按 `reportId` 去重，按 `timestamp` 防止旧状态覆盖；
- 区分 A-BOX 心跳在线和 `controllerOnline`；
- 不依赖厂家 CAN、内部诊断字段或 `/internal/*` Topic。

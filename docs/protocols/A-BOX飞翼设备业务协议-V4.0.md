# A-BOX 飞翼设备业务协议 V4.0

| 项目 | 内容 |
| --- | --- |
| 适用产品 | 飞翼升降设备 A-BOX |
| 文档修订号 | V4.0 |
| 报文版本 | `4.0` |
| Profile 名称及版本 | `wing 1.0` |
| 文档属性 | 业务接口规范 |
| 依赖协议 | [A-BOX 设备公共 MQTT 协议 V4.0](https://github.com/cxh0416/abox-platform/blob/main/docs/protocols/A-BOX设备公共MQTT协议-V4.0.md) |

## 目录

- [1. 范围与引用](#1-范围与引用)
- [2. 通信约定与能力声明](#2-通信约定与能力声明)
- [3. 接口总览与时序约束](#3-接口总览与时序约束)
- [4. 命令定义](#4-命令定义)
  - [4.1 wing_control](#41-wing_control)
  - [4.2 sync_state](#42-sync_state)
- [5. 业务报告](#5-业务报告)
  - [5.1 wing_status](#51-wing_status)
  - [5.2 wing_position](#52-wing_position)
- [6. 交互与异常处理](#6-交互与异常处理)
- [7. 错误与产品约束](#7-错误与产品约束)

## 1. 范围与引用

本协议定义飞翼上升、下降、停止及状态同步接口。公共报文、Topic、QoS、身份、心跳、Manifest、有效期、幂等及兼容规则遵循公共 V4；本文只规定飞翼业务参数、成功边界和状态语义。

设备身份由 Topic 中的 `deviceId` 确定，能力由 Manifest 中的 `wing` 声明；请求和报告不要求携带 `deviceType` 或 `product`。厂家通信、内部控制流程和运维接口不属于本协议。

## 2. 通信约定与能力声明

Profile 名称固定为 `wing`，版本为 `1.0`；单实例，不使用 `instanceId`。

| 方向 | 消息 | Topic | QoS | Retain |
| --- | --- | --- | ---: | --- |
| 平台 → A-BOX | 请求 | `/zxwl/abox/{deviceId}/request` | 1 | false |
| A-BOX → 平台 | 响应 | `/zxwl/abox/{deviceId}/response` | 1 | false |
| A-BOX → 平台 | 完整状态 | `/zxwl/abox/{deviceId}/report` | 1 | false |
| A-BOX → 平台 | 位置采样 | `/zxwl/abox/{deviceId}/report` | 0 | false |
| A-BOX → 平台 | 心跳 | `/zxwl/abox/{deviceId}/heartbeat` | 0 | false |
| A-BOX → 平台 | 能力声明 | `/zxwl/abox/{deviceId}/manifest` | 1 | true |

平台先订阅 Manifest、Heartbeat、Report 和 Response，其中 Report 订阅使用 QoS 1。下发动作前，须按公共 V4 第 7.3～7.5 节将同设备的 Manifest 与当前在线 V4 心跳匹配，并确认支持 `wing 1.0`。心跳周期为 5 秒，连续 15 秒未收到即判定 A-BOX 离线。

**示例 1：Manifest（设备 → 平台；`/zxwl/abox/demo_wing_01/manifest`；完整报文）**

```json
{"version":"4.0","timestamp":1800000000000,"data":{"firmware":"<固件版本字符串>","profiles":[{"name":"wing","version":"1.0"}]}}
```

上例适用于同一 `firmware` 唯一决定能力集合及版本的设备；否则必须按公共协议增加 `manifestId`，并由心跳回显。

## 3. 接口总览与时序约束

| 接口 / 匹配条件 | 分类 | `params` | `expiresAt` / 最大有效期 |
| --- | --- | --- | --- |
| `wing_control`，`action=ascend` | `DISCRETE_ACTION` | `{"action":"ascend"}` | 必填 / 5 秒 |
| `wing_control`，`action=descend` | `DISCRETE_ACTION` | `{"action":"descend"}` | 必填 / 5 秒 |
| `wing_control`，`action=stop` | `SAFETY_STOP` | `{"action":"stop"}` | 不使用 |
| `sync_state` | `QUERY`，Core 保留命令 | `null` | 不使用 |

`action` 区分三种操作，缺失或非法时返回 `4001`。升降请求的原始有效期必须满足 `0 < expiresAt - timestamp <= 5000 ms`；缺失或非法返回 `4001`，已过期返回 `4005`，可信时间不足以判断尚未过期返回 `4003`。`stop` 和 `sync_state` 不依赖授时，携带 `expiresAt` 时返回 `4001`。

升降采用公共 V4 的 `persistent_idempotency` 跨重启保护：重复请求不得重复启动，保护窗口内保留原请求的逻辑内容及首次完整响应。重启后不得自动恢复此前动作；状态必须重新确认。记录保留和内容冲突规则遵循公共 V4 第 4.4 节。

## 4. 命令定义

### 4.1 `wing_control`

请求上升、下降或停止。`params.action`（String）必填，只允许 `ascend`、`descend`、`stop`。

| 动作 | `200` 成功边界 |
| --- | --- |
| `ascend` | 已接受上升请求；不表示到达上端点 |
| `descend` | 已接受下降请求；不表示到达下端点 |
| `stop` | 已取得控制器已停止反馈，当前升降动作已结束 |

三个动作的响应均省略 `result`，业务判断以 `code` 为准；`msg` 仅供展示。升降完成及异步异常通过 `wing_status` 判断。

改变运动方向前，必须发送独立的 `stop` 请求；只有收到该请求的成功响应及关联的 `motionState=stopped`、`statusFresh=true`、`controllerOnline=true` 状态后，才能发送反方向请求。新的换向操作使用新的停止请求标识，历史停止响应不得复用为本次确认。

合法 `stop` 可以抢占尚未完成的升降请求，不因普通动作繁忙或未授时而拒绝；仍须通过身份及参数校验。相同内容的停止请求允许重复执行，每次均以当前停止确认返回结果，不能把历史成功回执当作新的停止反馈。

**示例 2：上升（平台 → 设备；`/zxwl/abox/demo_wing_01/request`；完整请求）**

```json
{
  "version": "4.0",
  "timestamp": 1800000000000,
  "data": {
    "requestId": "wing-up-001",
    "command": "wing_control",
    "params": {"action": "ascend"},
    "expiresAt": 1800000005000
  }
}
```

**示例 3：上升响应（设备 → 平台；`/zxwl/abox/demo_wing_01/response`；完整报文）**

```json
{"version":"4.0","timestamp":1800000000500,"data":{"requestId":"wing-up-001","code":200,"msg":"operation accepted"}}
```

下降使用相同命令，将 `action` 改为 `descend`，并使用新的请求标识和有效期。

**示例 4：停止（平台 → 设备；`/zxwl/abox/demo_wing_01/request`；完整请求）**

```json
{"version":"4.0","timestamp":1800000001000,"data":{"requestId":"wing-stop-001","command":"wing_control","params":{"action":"stop"}}}
```

### 4.2 `sync_state`

请求当前完整状态，不启动、停止或改变升降动作。请求及乱序、重试规则遵循公共 V4 第 6 节。

**示例 5：同步请求（平台 → 设备；`/zxwl/abox/demo_wing_01/request`；完整报文）**

```json
{"version":"4.0","timestamp":1800000002000,"data":{"requestId":"wing-sync-001","command":"sync_state","params":null}}
```

**示例 6：同步响应（设备 → 平台；`/zxwl/abox/demo_wing_01/response`；完整报文）**

```json
{"version":"4.0","timestamp":1800000002010,"data":{"requestId":"wing-sync-001","code":200,"msg":"state synchronization scheduled","result":{"expectedStates":[{"reportName":"wing_status"}]}}}
```

`200` 只表示已安排同步。收到关联同一 `requestId` 的完整 `wing_status` 才算收齐；报告可以先于响应到达。从成功响应起等待 15 秒仍未收齐时，本轮不完整，不得推断已停止或动作完成。位置采样及其他请求的报告不能补齐本轮同步。

## 5. 业务报告

| `reportName` | `reportType` | 完整状态 | 触发 |
| --- | --- | --- | --- |
| `wing_status` | `state` | 是 | 启动、重连、同步，以及动作、停止、端点、故障或控制器可用状态变化 |
| `wing_position` | `telemetry` | 否 | 运动期间按 200 ms 目标周期发送最新位置采样 |

两种报告均为单实例，不使用严格有序流。平台按公共 V4 第 5 节进行 `reportId` 去重和接收顺序处理；不得仅按设备 `timestamp` 排序或把报文到达顺序宣称为生成顺序。

### 5.1 `wing_status`

`payload` 为完整状态，以下字段全部必填；未知或不适用的可空字段显式发送 `null`，不继承历史值。

| 字段 | 类型 | 语义 |
| --- | --- | --- |
| `motionState` | String | `unknown`、`stopped`、`ascending`、`descending`、`moving_unknown`、`fault` 或 `offline` |
| `activeAction` | String / null | 当前已接受且尚未结束的 `ascend` 或 `descend`；无活动动作为 `null` |
| `positionMm` | Integer / null | 当前位置，单位 mm，范围 0～65535；未知为 `null` |
| `topLimitMm` | Integer / null | 上端点位置，单位 mm，范围 0～65535；未知为 `null` |
| `bottomLimitMm` | Integer / null | 下端点位置，单位 mm，范围 0～65535；未知为 `null` |
| `atEndpoint` | String / null | `top`、`bottom`；未确认处于端点时为 `null` |
| `faultCode` | String / null | 设备原始故障码；无已知故障为 `null`，不单独证明正常 |
| `controllerOnline` | Boolean / null | 控制器在线为 `true`，已判定不可用为 `false`，无法判断为 `null` |
| `statusFresh` | Boolean | 本份状态是否基于当前有效反馈；`false` 时运动、位置和端点不能作为实时确认 |
| `safetyStopReason` | String / null | `feedback_timeout`、`max_runtime`、`controller_fault`；未触发安全停止为 `null` |

`ascending` / `descending` 表示已接受动作对应的方向，不能单凭它证明真实机械运动；方向无法确定时使用 `moving_unknown`。`unknown`、`moving_unknown`、`fault`、`offline` 及未知枚举均不得解释为已停止。

动作受理、完成、失败或停止直接触发的状态必须携带对应请求的 `data.requestId`；升降的端点完成或安全停止报告关联原升降请求，显式停止报告关联停止请求。同步报告关联同步请求；启动、重连及无关联请求的自动状态变化省略 `requestId`。同步或重连报告只表达快照，不代表旧动作成功。

**示例 7：停止确认状态（设备 → 平台；`/zxwl/abox/demo_wing_01/report`；完整报文）**

```json
{
  "version": "4.0",
  "timestamp": 1800000001200,
  "data": {
    "reportId": "wing-status-001",
    "reportType": "state",
    "reportName": "wing_status",
    "requestId": "wing-stop-001",
    "payload": {
      "motionState": "stopped",
      "activeAction": null,
      "positionMm": 126,
      "topLimitMm": 250,
      "bottomLimitMm": 1,
      "atEndpoint": null,
      "faultCode": null,
      "controllerOnline": true,
      "statusFresh": true,
      "safetyStopReason": null
    }
  }
}
```

上升完成须由关联报告同时确认新鲜、控制器在线、`motionState=stopped`、`activeAction=null`、`atEndpoint=top`，且无故障或安全停止原因；下降对应 `atEndpoint=bottom`。仅收到 `code=200`、位置值、采样、到达端点但仍运动的状态或失去新鲜度的快照，均不能确认完成。

### 5.2 `wing_position`

`payload` 只包含必填的 `positionMm`（Integer / null）和 `statusFresh`（Boolean），含义同 `wing_status`。允许丢失和抖动；不作为完整状态、停止确认或动作完成依据，也不能刷新 `wing_status` 的接收时间及新鲜度。

**示例 8：位置采样（设备 → 平台；`/zxwl/abox/demo_wing_01/report`；完整报文）**

```json
{"version":"4.0","timestamp":1800000000800,"data":{"reportId":"wing-position-001","reportType":"telemetry","reportName":"wing_position","payload":{"positionMm":126,"statusFresh":true}}}
```

## 6. 交互与异常处理

1. 接入或恢复连接后，先确认当前能力并恢复完整状态；需要明确同步轮次时发送新的 `sync_state`。
2. 升降响应超时表示结果不确定。重试须保留原 `requestId`、参数、`timestamp` 和 `expiresAt`，不得换新 ID 自动重发旧升降意图；需要停止时发送独立 `stop`，并同步状态。
3. 改变方向按“`stop` → 当前停止成功响应及关联新鲜停止状态 → 反方向请求”执行。响应与报告可以乱序，平台必须收齐两者；等待期间若出现新的运动、故障、离线或反馈失效，不得继续换向。
4. 已接受的升降动作不要求平台续租。通信断开不表示动作取消或机械已停止；平台失联期间标记状态不可确认，恢复后不得自动续发旧操作。
5. 故障、控制冲突或安全停止后，由用户根据当前状态重新操作；不得自动反向或重复启动。未取得停止反馈时，设备和平台均不得宣称已停止。

心跳只更新 A-BOX 通信在线状态，不刷新业务状态或实测反馈；历史状态、幂等响应和重复报告均不能作为新的实时观测。

## 7. 错误与产品约束

公共响应码遵循公共 V4 第 4.6 节；同一 `requestId` 内容冲突使用 `4006`，`4004` 为公共保留码。以下补充飞翼业务条件及专用码：

| 响应码 | 飞翼业务含义 |
| ---: | --- |
| `4001` | 动作、参数或有效期非法 |
| `4002` | 不支持该命令 |
| `4003` | 当前存在互斥动作、现场控制占用，或升降所需可信时间不可用 |
| `5001` | A-BOX 内部异常 |
| `5002` | 下游控制器不可用 |
| `5003` | 当前反馈不足以安全接受动作，或未在规定时间内取得确认 |
| `5004` | 下游控制器拒绝执行 |
| `5005` | 下游控制器存在故障 |

平台保留未知响应码、枚举及原始 `msg`，不得套用其他 Profile 的含义。故障码解释属于设备维护资料。整包及 `params` / `result` / `payload` 大小遵循公共 V4 第 3.3 节，不自行分片。

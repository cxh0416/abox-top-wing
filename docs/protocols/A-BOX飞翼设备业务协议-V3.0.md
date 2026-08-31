# A-BOX 飞翼设备业务协议

文档修订：V3.0

报文版本：`3.0`

设备类型：`wing_actuator`

固件产品标识：`top_flying_wing`（仅用于固件发布和内部运维）

当前核对固件：`top-flying-wing-1.0.14`

## 1. 平台最小对接面

本文档定义 A-BOX 飞翼执行设备的公共业务接口。任何客户、车型或项目只要使用该设备能力，都统一按本文档对接，不为项目派生协议分支。公共报文、QoS、心跳、公共响应码和请求幂等统一遵循平台仓库的 [A-BOX 设备公共 MQTT 协议](https://github.com/cxh0416/abox-platform-firmware/blob/master/docs/protocols/A-BOX设备公共MQTT协议-V3.0.md)；设备信息、日志、配置和 OTA 见独立的 [顶飞翼 A-BOX 内部运维协议](顶飞翼A-BOX内部运维协议-1.0.md)；厂家 CAN 协议不属于平台接口。

平台只需实现以下 4 个公共 Topic：

| 方向 | 用途 | Topic | QoS | Retain |
|---|---|---|---:|---:|
| 平台 → A-BOX | 飞翼控制 | `/zxwl/abox/{deviceId}/ctrl` | 1 | 否 |
| A-BOX → 平台 | 控制响应 | `/zxwl/abox/{deviceId}/ctrl/resp` | 1 | 否 |
| A-BOX → 平台 | 主动状态 | `/zxwl/abox/{deviceId}/status/report` | 按报告类型为 0 或 1 | 是 |
| A-BOX → 平台 | 公共心跳 | `/zxwl/abox/{deviceId}/heartbeat` | 0 | 否 |

飞翼设备只有一个公共命令 `wing_control`，其动作只有 `ascend`、`descend` 和 `stop`。本 Profile 不定义 `/status` 查询或其他设备类型的扩展业务。

## 2. 硬件可观测边界

飞翼控制器可向 A-BOX 提供：

- 命令允许或拒绝；
- 多根推杆的平均位置，单位为 mm；
- `moving` 或 `stopped`；
- E01～E16 故障码。

当前硬件不能直接提供上升/下降方向、电流、电压、温度、单根推杆位置或机械动作完成原因。`ascending`、`descending` 由 A-BOX 根据最近一次已接受且未取消的远程动作推断；无法可靠确定方向时使用 `moving_unknown`。

控制响应 `code=200` 只证明控制器接受动作并取得满足本次处理要求的新鲜反馈，不证明机械动作已经完成。平台必须继续观察主动状态。

## 3. 控制规则

| 动作 | 含义 |
|---|---|
| `ascend` | 请求上升，不隐含停止 |
| `descend` | 请求下降，不隐含停止 |
| `stop` | 明确取消当前锁存动作并等待停止确认 |

平台和设备必须遵守以下规则：

1. 改变方向前，平台必须先发送 `stop`，收到停止成功响应并观察到已停止状态后，才能发送反方向请求。
2. 完全空闲时 A-BOX 不轮询控制器，使用 `canSession=idle_silent`、`statusFresh=false`、`controllerOnline=null` 和 `motionState=unknown` 表示；位置和限位可以保留上一次远程会话结果。
3. 新的升降请求会先执行两次运动检查，两次之间静默250 ms，再读取限位。发现现场运动、查询超时、故障或限位非法时拒绝远程动作。
4. 预检通过后设备只发送一次 `UP` 或 `DN`；远程运行期间按设备内部周期读取状态。
5. 到达目标端点、反馈超时、控制器故障、连续运行达到上限、位置反向或未到端点意外停止时，设备只发送一次停止命令收口。
6. `stop` 可以抢占预检、启动和远程运行。空闲静默状态下的 `stop` 按幂等成功处理，不发送底层停止命令。
7. `stop` 只有取得控制器接受和 `stopped` 确认后才返回 `200`，随后才可清除 `activeAction`。
8. 停止未确认时不得宣称机械已停止，也不得清除动作锁存。
9. 已经处于 `stopped`、`activeAction=null` 且无故障时，`stop` 幂等成功。
10. 相同 `requestId` 的 QoS 1 重复投递只返回首次响应，不重复执行底层控制。
11. 设备只接受与本机当前待处理查询匹配的控制器反馈；空闲时其他节点的反馈不得污染状态。
12. 远程会话结束后立即返回 `idle_silent`。当前协议不能可靠区分同方向现场触摸和远程动作，平台必须展示这一限制。

控制准入矩阵：

| 当前条件 | 新 `ascend` / `descend` | `stop` |
|---|---|---|
| `idle_silent` 且 A-BOX 在线 | 进入完整预检 | 幂等成功，不发送底层停止 |
| 预检发现控制器不可达 | `5002` | 不适用 |
| 预检发现现场运动 | `4003`，不干预现场动作 | 可抢占并尝试停止 |
| 预检发现故障 | `5005` | 作为安全动作尝试 |
| `preflight` / `remote_running` | `4003` | 允许抢占 |
| `stopping` | `4003` | 合并现有停止，不重复下发 |

## 4. 状态模型

主动状态报告的 `data` 使用以下快照字段。控制响应不再重复携带状态快照：

| 字段 | 类型 | 说明 |
|---|---|---|
| `motionState` | String | 当前运动状态 |
| `activeAction` | String / null | 已接受且未取消的 `ascend` 或 `descend` |
| `positionMm` | Integer / null | 多推杆平均位置；从未取得时为 `null` |
| `topLimitMm` | Integer / null | 最高限位；未知时为 `null` |
| `bottomLimitMm` | Integer / null | 最低限位；未知时为 `null` |
| `atEndpoint` | String / null | `top`、`bottom` 或 `null` |
| `faultCode` | String / null | `E01`～`E16`；无故障时为 `null` |
| `controllerOnline` | Boolean / null | 当前远程会话的控制器在线状态；空闲静默时为 `null` |
| `statusFresh` | Boolean | 状态是否来自当前活动会话；`false` 时位置和限位属于历史数据 |
| `canSession` | String | `idle_silent`、`preflight`、`remote_running`、`stopping` 或 `fault` |
| `controlOwner` | String | `none`、`remote` 或 `local_suspected` |
| `operationPhase` | String | `idle`、`checking_motion`、`quiet_guard`、`checking_limits`、`starting`、`running` 或 `stopping` |
| `feedbackAgeMs` | Integer / null | 最近有效反馈年龄；无当前会话反馈时为 `null` |
| `lastRequestId` | String / null | 最近一次被接受的远程控制请求 |
| `runElapsedMs` | Integer | 本轮连续运动时间；未运动时为0 |
| `safetyStopReason` | String / null | 安全停止原因；未触发时为 `null` |

`motionState` 枚举：

| 值 | 说明 |
|---|---|
| `unknown` | 当前没有足够信息 |
| `stopped` | 控制器反馈停止 |
| `ascending` | 正在运动，方向由 `activeAction=ascend` 推断 |
| `descending` | 正在运动，方向由 `activeAction=descend` 推断 |
| `moving_unknown` | 控制器反馈运动但方向未知 |
| `fault` | 控制器在线且存在 E01～E16 故障 |
| `offline` | 控制器离线或无法取得有效状态 |

判定优先级为 `offline` > `fault` > `stopped` / 运动状态。

`safetyStopReason` 当前枚举为 `feedback_timeout`、`max_runtime`、`controller_fault`。

## 5. 控制请求与响应

请求 Topic：`/zxwl/abox/{deviceId}/ctrl`

响应 Topic：`/zxwl/abox/{deviceId}/ctrl/resp`

### 5.1 请求

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

### 5.2 控制响应

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

飞翼控制响应统一通过 `code` 表示业务结果，`msg` 只用于展示和诊断，不发送 `result`。请求动作由平台保存在原请求中；当前状态、完成结果和异步故障只通过 `/status/report` 获取，不在控制响应中重复返回。

新的 `requestId` 在动作锁存期间再次请求 `ascend` 或 `descend` 均返回 `4003`。只有相同 `requestId`、相同内容的 MQTT 重复投递才返回首次缓存响应。

停止请求只有在确认 `motionState=stopped` 后返回 `200`，此时 `activeAction=null`、`runElapsedMs=0`。

## 6. 飞翼设备响应码

| 响应码 | 业务含义 |
|---:|---|
| `200` | 动作被接受且取得满足本次处理要求的新鲜反馈；不表示机械动作完成 |
| `4001` | 参数、动作非法或 `requestId` 冲突 |
| `4002` | 当前设备类型不支持该命令 |
| `4003` | 现场运动、动作锁存或控制流程繁忙；除 `stop` 外的新动作被拒绝 |
| `4004` | 请求 `deviceType` 不匹配 |
| `5001` | A-BOX 内部异常 |
| `5002` | 发送控制前已判定控制器离线 |
| `5003` | 当前状态不足以安全接受动作，或等待应答/状态确认超时 |
| `5004` | 控制器明确拒绝执行 |
| `5005` | 控制器存在 E01～E16 故障，无法接受动作或确认安全停止 |

`5002` 表示发送前已有离线结论；`5003` 表示处理本次请求时等待应答或新鲜反馈超时。

## 7. 主动状态报告

Topic：`/zxwl/abox/{deviceId}/status/report`

方向：A-BOX → 平台

Retain：是

### 7.1 报告类型与 QoS

| `reportReason` | QoS | 触发条件 |
|---|---:|---|
| `periodic` | 0 | 运动期间按 200 ms 目标周期发送最新位置快照 |
| `state_changed` | 1 | 会话、运动状态或动作锁存变化 |
| `controller_offline` | 1 | 控制器离线或状态超时 |
| `controller_online` | 1 | 控制器恢复在线 |
| `fault_raised` | 1 | 故障产生或故障码变化 |
| `fault_cleared` | 1 | 故障解除 |
| `safety_stop` | 1 | A-BOX 触发安全停止 |
| `endpoint_stop` | 1 | 到达目标端点并停止或完成停止确认 |
| `control_conflict` | 1 | 位置反向或非端点意外停止，怀疑现场介入 |

平台订阅该 Topic 时请求 QoS 1，从而同时接收 QoS 0 周期样本和 QoS 1 事件。平台必须按 `reportId` 幂等；`lastRequestId` 只用于关联控制请求，不能代替 `reportId` 判断重复。

### 7.2 完整示例

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
    "canSession": "remote_running",
    "controlOwner": "remote",
    "operationPhase": "running",
    "feedbackAgeMs": 32,
    "lastRequestId": "3fa85f64-5717-4562-b3fc-2c96063f8666",
    "runElapsedMs": 4820,
    "safetyStopReason": null
  }
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `reportId` | String | 逻辑报告唯一标识；QoS 重发保持不变 |
| `reportReason` | String | 报告类型 |
其余字段遵循第 4 章状态模型。V3 只使用 `reportReason`，不再发送 V2 的 `reason` 兼容别名。

### 7.3 上报和处理规则

1. MQTT 建立连接后立即上报当前完整状态；空闲时不为取得新状态而发送底层查询。
2. 预检、启动、运行、停止、故障和控制器在线状态发生离散变化时立即用 QoS 1 上报。
3. 远程运动期间普通位置以 200 ms 目标周期、QoS 0 上报；平台必须允许蜂窝网络抖动和中间样本丢失。
4. 停止且没有状态变化时不重复上报，由 Broker 保留最后状态。
5. 平台按 `reportId` 去重，并按 `timestamp` 拒绝覆盖更新状态的过期报文。
6. Retain 报告不改变公共心跳的在线判定。
7. 控制请求已经返回 `200` 后出现的异步故障或安全停止只通过本 Topic 更新状态，不追溯修改原控制响应。

设备发送优先级为：控制/内部响应 > 离散状态和关键安全事件 > 到期心跳 > 周期运动样本 > 增量日志。

## 8. 设备侧安全结果

1. 单个业务请求不得导致底层控制指令重复执行。
2. 运动期间反馈超时，设备尽力停止，并上报 `reportReason=safety_stop`、`safetyStopReason=feedback_timeout`。
3. 连续运动达到运行上限，停止确认后上报 `safetyStopReason=max_runtime`。
4. 运动期间出现 E01～E16，设备尽力停止，并上报故障和安全停止状态。
5. MQTT 断开不取消已接受的动作；控制器监测和运行上限计时继续工作。
6. 安全停止未取得停止确认时，平台不得把机械停止视为已证明。
7. 反馈超时和最长运行配置属于设备配置，不通过本公共业务协议修改。

## 9. E01～E16 故障码

平台必须保存原始 `faultCode`，不得只保留统一的 `5005`。

| 故障码 | 含义 | 处理提示 |
|---|---|---|
| `E01` | 遇阻回退 | 检查阻力和机械行程，错误可能自动消除 |
| `E02` | 同步偏差告警 | 检查多通道同步和推杆反馈 |
| `E03` | 欠压警告 | 检查输入电源和负载 |
| `E04` | 倾斜警告 | 检查同步异常和机械结构 |
| `E05` | 通道1过流 | 检查通道1负载和堵转 |
| `E06` | 通道2过流 | 检查通道2负载和堵转 |
| `E07` | 通道3过流 | 检查通道3负载和堵转 |
| `E08` | 通道4过流 | 检查通道4负载和堵转 |
| `E09` | 电源过流 | 检查总负载和电源容量 |
| `E10` | 通道1霍尔无信号 | 检查推杆、连接和堵转 |
| `E11` | 通道2霍尔无信号 | 检查推杆、连接和堵转 |
| `E12` | 通道3霍尔无信号 | 检查推杆、连接和堵转 |
| `E13` | 通道4霍尔无信号 | 检查推杆、连接和堵转 |
| `E14` | 过热警告 | 停止运行并等待降温 |
| `E15` | 密码或注册错误 | 检查控制器注册信息 |
| `E16` | 异常断电报警 | 按控制器要求复位并检查供电 |

## 10. 平台验收检查表

- 只实现 4 个飞翼设备公共 Topic；
- 控制发布和响应/状态订阅请求 QoS 1；
- 改变方向前明确 `stop`，并等待停止确认；
- 控制响应只按 `code` 判断受理结果，不发送 `result`；
- `code=200` 不展示为机械动作完成，完成状态继续读取 `/status/report`；
- `reportId` 用于去重，`lastRequestId` 只用于请求关联；
- `periodic` 允许丢失和抖动，关键事件完整处理；
- A-BOX 在线和 `controllerOnline` 分别展示；
- `statusFresh=false` 时明确标记位置和限位为历史数据；
- Retain 报告不替代心跳在线判定；
- 未知字段、枚举和故障码保留原始值；
- 公共平台不调用 `/internal/*` Topic，也不依赖厂家 CAN 帧。

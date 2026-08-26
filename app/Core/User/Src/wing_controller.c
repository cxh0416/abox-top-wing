#include "wing_controller.h"

#include <string.h>

#include "app_log.h"
#include "main.h"
#include "wing_protocol.h"

#define RESULT_QUEUE_DEPTH 4U
#define REPORT_QUEUE_DEPTH 8U

typedef enum
{
    SESSION_IDLE = 0,
    SESSION_WAIT_RD1,
    SESSION_QUIET_GUARD,
    SESSION_WAIT_RD2,
    SESSION_WAIT_RL,
    SESSION_WAIT_START_ACK,
    SESSION_WAIT_START_STATUS,
    SESSION_RUNNING,
    SESSION_WAIT_STOP_ACK,
    SESSION_WAIT_STOP_STATUS
} SessionState;

typedef enum { QUERY_NONE = 0, QUERY_RD, QUERY_RL } ExpectedQuery;
typedef enum { STOP_USER = 0, STOP_ENDPOINT, STOP_SAFETY, STOP_CONFLICT } StopCause;

typedef struct
{
    SessionState state;
    ExpectedQuery expected_query;
    WingAction action;
    StopCause stop_cause;
    uint32_t phase_tick;
    uint32_t query_tick;
    uint32_t last_poll_tick;
    uint32_t stop_tick;
    uint16_t best_position_mm;
    uint8_t conflict_samples;
    uint8_t direction_result_sent;
    uint8_t stop_command_sent;
    char direction_request_id[40];
    char stop_request_id[40];
} WingSession;

static WingState g_state;
static WingSession g_session;
static WingControlResult g_results[RESULT_QUEUE_DEPTH];
static uint8_t g_result_head;
static uint8_t g_result_count;
static WingReportReason g_reports[REPORT_QUEUE_DEPTH];
static uint8_t g_report_head;
static uint8_t g_report_count;

static CAN_HandleTypeDef* selected_can(void)
{
#if TOP_WING_CAN_BUS == 1
    return &hcan1;
#else
    return &hcan2;
#endif
}

static void copy_id(char dst[40], const char* src)
{
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, 39U);
    dst[39] = '\0';
}

static void push_report(WingReportReason reason)
{
    uint8_t tail;
    if (reason == WING_REPORT_NONE) return;
    if (g_report_count &&
        g_reports[(uint8_t)((g_report_head + g_report_count - 1U) % REPORT_QUEUE_DEPTH)] == reason)
        return;
    if (g_report_count >= REPORT_QUEUE_DEPTH)
    {
        g_report_head = (uint8_t)((g_report_head + 1U) % REPORT_QUEUE_DEPTH);
        g_report_count--;
    }
    tail = (uint8_t)((g_report_head + g_report_count) % REPORT_QUEUE_DEPTH);
    g_reports[tail] = reason;
    g_report_count++;
}

static void push_result(const char* request_id, WingAction action, uint16_t code)
{
    uint8_t tail;
    if (!request_id || !request_id[0]) return;
    if (g_result_count >= RESULT_QUEUE_DEPTH)
    {
        g_result_head = (uint8_t)((g_result_head + 1U) % RESULT_QUEUE_DEPTH);
        g_result_count--;
    }
    tail = (uint8_t)((g_result_head + g_result_count) % RESULT_QUEUE_DEPTH);
    memset(&g_results[tail], 0, sizeof(g_results[tail]));
    copy_id(g_results[tail].request_id, request_id);
    g_results[tail].action = action;
    g_results[tail].code = code;
    g_results[tail].accepted = code == 200U ? 1U : 0U;
    g_result_count++;
}

static uint8_t send_frame(const uint8_t data[8])
{
    CAN_TxHeaderTypeDef header;
    uint32_t mailbox;
    memset(&header, 0, sizeof(header));
    header.ExtId = WING_CAN_TX_ID;
    header.IDE = CAN_ID_EXT;
    header.RTR = CAN_RTR_DATA;
    header.DLC = 8U;
    header.TransmitGlobalTime = DISABLE;
    return HAL_CAN_AddTxMessage(selected_can(), &header, (uint8_t*)data, &mailbox) == HAL_OK ? 1U : 0U;
}

static uint8_t send_action(WingAction action, const char* request_id)
{
    uint8_t data[8];
    uint8_t ok;
    if (!WingProtocol_EncodeAction(action, data)) return 0U;
    ok = send_frame(data);
    if (ok)
        APP_LOGI("CAN", "tx rid=%s act=%s id=%08lX", request_id && request_id[0] ? request_id : "internal",
                 WingAction_Name(action), (unsigned long)WING_CAN_TX_ID);
    else
        APP_LOGE("CAN", "tx fail rid=%s act=%s", request_id && request_id[0] ? request_id : "internal",
                 WingAction_Name(action));
    return ok;
}

static uint8_t send_query(ExpectedQuery query, uint32_t now)
{
    uint8_t data[8];
    if (query == QUERY_RD) WingProtocol_EncodeStatusQuery(data);
    else if (query == QUERY_RL) WingProtocol_EncodeLimitQuery(data);
    else return 0U;
    if (!send_frame(data)) return 0U;
    g_session.expected_query = query;
    g_session.query_tick = now;
    g_session.last_poll_tick = now;
    APP_LOGI("CAN", "tx query=%s rid=%s", query == QUERY_RD ? "RD" : "RL",
             g_session.direction_request_id[0] ? g_session.direction_request_id : g_session.stop_request_id);
    return 1U;
}

static void clear_session(void)
{
    memset(&g_session, 0, sizeof(g_session));
    g_session.state = SESSION_IDLE;
}

static void finish_idle(WingControlOwner owner)
{
    clear_session();
    WingState_SetIdleSilent(&g_state, owner);
    push_report(WING_REPORT_STATE_CHANGED);
}

static void fail_direction(uint16_t code, WingControlOwner owner)
{
    push_result(g_session.direction_request_id, g_session.action, code);
    APP_LOGW("WING", "preflight/start failed rid=%s act=%s code=%u",
             g_session.direction_request_id, WingAction_Name(g_session.action), (unsigned)code);
    finish_idle(owner);
}

static uint8_t begin_stop(StopCause cause, const char* stop_request_id, uint32_t now)
{
    const char* tx_id = stop_request_id;
    if (g_session.stop_command_sent) return 1U;
    if (!g_session.direction_result_sent && g_session.direction_request_id[0])
    {
        push_result(g_session.direction_request_id, g_session.action, 5003U);
        g_session.direction_result_sent = 1U;
    }
    g_session.expected_query = QUERY_NONE;
    g_session.stop_cause = cause;
    copy_id(g_session.stop_request_id, stop_request_id);
    if (!tx_id || !tx_id[0]) tx_id = g_session.direction_request_id;
    if (!send_action(WING_ACTION_STOP, tx_id))
    {
        if (stop_request_id && stop_request_id[0]) push_result(stop_request_id, WING_ACTION_STOP, 5001U);
        WingState_SetSession(&g_state, WING_CAN_SESSION_FAULT, WING_PHASE_STOPPING,
                             cause == STOP_CONFLICT ? WING_CONTROL_OWNER_LOCAL_SUSPECTED : WING_CONTROL_OWNER_REMOTE);
        finish_idle(cause == STOP_CONFLICT ? WING_CONTROL_OWNER_LOCAL_SUSPECTED : WING_CONTROL_OWNER_NONE);
        return 0U;
    }
    g_session.stop_command_sent = 1U;
    g_session.stop_tick = now;
    g_session.phase_tick = now;
    g_session.state = SESSION_WAIT_STOP_ACK;
    WingState_SetSession(&g_state, WING_CAN_SESSION_STOPPING, WING_PHASE_STOPPING,
                         cause == STOP_CONFLICT ? WING_CONTROL_OWNER_LOCAL_SUSPECTED : WING_CONTROL_OWNER_REMOTE);
    if (cause == STOP_ENDPOINT) push_report(WING_REPORT_ENDPOINT_STOP);
    else if (cause == STOP_CONFLICT) push_report(WING_REPORT_CONTROL_CONFLICT);
    else if (cause == STOP_SAFETY) push_report(WING_REPORT_SAFETY_STOP);
    return 1U;
}

static void finish_stop(void)
{
    WingControlOwner owner = g_session.stop_cause == STOP_CONFLICT ?
                             WING_CONTROL_OWNER_LOCAL_SUSPECTED : WING_CONTROL_OWNER_NONE;
    if (g_session.stop_request_id[0])
        push_result(g_session.stop_request_id, WING_ACTION_STOP, 200U);
    WingState_ConfirmStop(&g_state, g_session.stop_request_id, g_session.stop_request_id[0] ? 1U : 0U);
    finish_idle(owner);
}

static void assess_running_status(uint32_t now)
{
    uint16_t pos = g_state.snapshot.position_mm;
    uint8_t reversed = 0U;
    if (g_state.snapshot.fault_code)
    {
        WingState_LatchSafety(&g_state, WING_SAFETY_CONTROLLER_FAULT);
        (void)begin_stop(STOP_SAFETY, "", now);
        return;
    }
    if (WingState_EndpointReached(&g_state, g_session.action))
    {
        (void)begin_stop(STOP_ENDPOINT, "", now);
        return;
    }
    if (!g_state.raw_moving)
    {
        (void)begin_stop(STOP_CONFLICT, "", now);
        return;
    }
    if (g_session.action == WING_ACTION_ASCEND)
    {
        if (pos > g_session.best_position_mm) g_session.best_position_mm = pos;
        else if ((uint32_t)pos + WING_DIRECTION_CONFLICT_MM < g_session.best_position_mm) reversed = 1U;
    }
    else
    {
        if (pos < g_session.best_position_mm) g_session.best_position_mm = pos;
        else if ((uint32_t)pos > (uint32_t)g_session.best_position_mm + WING_DIRECTION_CONFLICT_MM) reversed = 1U;
    }
    if (reversed)
    {
        if (g_session.conflict_samples < 255U) g_session.conflict_samples++;
        if (g_session.conflict_samples >= WING_DIRECTION_CONFLICT_SAMPLES)
            (void)begin_stop(STOP_CONFLICT, "", now);
    }
    else g_session.conflict_samples = 0U;
}

void WingController_Init(void)
{
    CAN_FilterTypeDef filter;
    WingState_Init(&g_state);
    clear_session();
    g_result_head = g_result_count = 0U;
    g_report_head = g_report_count = 0U;
    memset(&filter, 0, sizeof(filter));
    filter.FilterBank = TOP_WING_CAN_BUS == 1 ? 0U : 14U;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterActivation = ENABLE;
    filter.SlaveStartFilterBank = 14U;
    (void)HAL_CAN_ConfigFilter(selected_can(), &filter);
    (void)HAL_CAN_Start(selected_can());
    (void)HAL_CAN_ActivateNotification(selected_can(), CAN_IT_RX_FIFO0_MSG_PENDING);
    APP_LOGI("WING", "CAN%u remote-session idle=silent tx=0x%08lX rx=0x%08lX",
             (unsigned)TOP_WING_CAN_BUS, (unsigned long)WING_CAN_TX_ID,
             (unsigned long)WING_CAN_RX_ID);
}

uint16_t WingController_Request(WingAction action, const char* request_id)
{
    uint32_t now = HAL_GetTick();
    uint16_t code;
    if (!request_id || !request_id[0] || action == WING_ACTION_NONE) return 4001U;

    if (action == WING_ACTION_STOP)
    {
        if (g_session.state == SESSION_IDLE) return 200U;
        if (g_session.state == SESSION_WAIT_STOP_ACK || g_session.state == SESSION_WAIT_STOP_STATUS)
            return 5003U;
        return begin_stop(STOP_USER, request_id, now) ? 0U : 5001U;
    }

    code = WingState_AdmissionCode(&g_state, action);
    if (code) return code;
    clear_session();
    g_session.state = SESSION_WAIT_RD1;
    g_session.action = action;
    g_session.phase_tick = now;
    copy_id(g_session.direction_request_id, request_id);
    WingState_InvalidateLimits(&g_state);
    WingState_SetSession(&g_state, WING_CAN_SESSION_PREFLIGHT, WING_PHASE_CHECKING_MOTION,
                         WING_CONTROL_OWNER_NONE);
    push_report(WING_REPORT_STATE_CHANGED);
    if (!send_query(QUERY_RD, now))
    {
        finish_idle(WING_CONTROL_OWNER_NONE);
        return 5001U;
    }
    return 0U;
}

void WingController_OnCanRx(CAN_HandleTypeDef* hcan)
{
    CAN_RxHeaderTypeDef header;
    uint8_t data[8];
    WingCanMessage msg;
    uint32_t now;
    if (hcan != selected_can()) return;
    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, data) != HAL_OK) return;
    if (header.IDE != CAN_ID_EXT ||
        !WingProtocol_Decode(header.ExtId, (uint8_t)header.DLC, data, &msg)) return;
    now = HAL_GetTick();

    if (msg.kind == WING_FRAME_ACK)
    {
        if (g_session.state == SESSION_WAIT_START_ACK && msg.action == g_session.action)
        {
            if (!msg.accepted) { fail_direction(5004U, WING_CONTROL_OWNER_NONE); return; }
            g_session.state = SESSION_WAIT_START_STATUS;
            g_session.phase_tick = now;
            if (!send_query(QUERY_RD, now))
                (void)begin_stop(STOP_SAFETY, "", now);
            return;
        }
        if (g_session.state == SESSION_WAIT_STOP_ACK && msg.action == WING_ACTION_STOP)
        {
            if (!msg.accepted)
            {
                if (g_session.stop_request_id[0]) push_result(g_session.stop_request_id, WING_ACTION_STOP, 5004U);
                finish_idle(g_session.stop_cause == STOP_CONFLICT ? WING_CONTROL_OWNER_LOCAL_SUSPECTED : WING_CONTROL_OWNER_NONE);
                return;
            }
            g_session.state = SESSION_WAIT_STOP_STATUS;
            g_session.phase_tick = now;
            g_session.expected_query = QUERY_NONE;
            (void)send_query(QUERY_RD, now);
        }
        return;
    }

    if (msg.kind == WING_FRAME_LIMITS)
    {
        if (g_session.expected_query != QUERY_RL || g_session.state != SESSION_WAIT_RL) return;
        g_session.expected_query = QUERY_NONE;
        if (!WingState_ApplyLimits(&g_state, msg.top_limit_mm, msg.bottom_limit_mm))
        {
            fail_direction(5003U, WING_CONTROL_OWNER_NONE);
            return;
        }
        WingState_SetSession(&g_state, WING_CAN_SESSION_PREFLIGHT, WING_PHASE_STARTING,
                             WING_CONTROL_OWNER_REMOTE);
        g_session.state = SESSION_WAIT_START_ACK;
        g_session.phase_tick = now;
        if (!send_action(g_session.action, g_session.direction_request_id))
            fail_direction(5001U, WING_CONTROL_OWNER_NONE);
        else push_report(WING_REPORT_STATE_CHANGED);
        return;
    }

    if (msg.kind != WING_FRAME_STATUS || g_session.expected_query != QUERY_RD) return;
    if (!msg.accepted) return;
    g_session.expected_query = QUERY_NONE;
    WingState_ApplyFeedback(&g_state, now, msg.position_mm, msg.fault_code, msg.moving);

    if (g_session.state == SESSION_WAIT_RD1 || g_session.state == SESSION_WAIT_RD2)
    {
        uint8_t second = g_session.state == SESSION_WAIT_RD2 ? 1U : 0U;
        if (msg.fault_code) { fail_direction(5005U, WING_CONTROL_OWNER_NONE); return; }
        if (msg.moving) { fail_direction(4003U, WING_CONTROL_OWNER_LOCAL_SUSPECTED); return; }
        if (!second)
        {
            g_session.state = SESSION_QUIET_GUARD;
            g_session.phase_tick = now;
            WingState_SetSession(&g_state, WING_CAN_SESSION_PREFLIGHT, WING_PHASE_QUIET_GUARD,
                                 WING_CONTROL_OWNER_NONE);
            push_report(WING_REPORT_STATE_CHANGED);
        }
        else
        {
            g_session.state = SESSION_WAIT_RL;
            g_session.phase_tick = now;
            WingState_SetSession(&g_state, WING_CAN_SESSION_PREFLIGHT, WING_PHASE_CHECKING_LIMITS,
                                 WING_CONTROL_OWNER_NONE);
            if (!send_query(QUERY_RL, now)) fail_direction(5001U, WING_CONTROL_OWNER_NONE);
            else push_report(WING_REPORT_STATE_CHANGED);
        }
        return;
    }

    if (g_session.state == SESSION_WAIT_START_STATUS)
    {
        if (msg.fault_code)
        {
            if (!g_session.direction_result_sent)
            {
                push_result(g_session.direction_request_id, g_session.action, 5005U);
                g_session.direction_result_sent = 1U;
            }
            WingState_LatchSafety(&g_state, WING_SAFETY_CONTROLLER_FAULT);
            (void)begin_stop(STOP_SAFETY, "", now);
            return;
        }
        if (!msg.moving)
        {
            (void)begin_stop(STOP_CONFLICT, "", now);
            return;
        }
        WingState_AcceptDirection(&g_state, g_session.action, g_session.direction_request_id, now);
        g_session.state = SESSION_RUNNING;
        g_session.phase_tick = now;
        g_session.last_poll_tick = now;
        g_session.best_position_mm = msg.position_mm;
        g_session.direction_result_sent = 1U;
        push_result(g_session.direction_request_id, g_session.action, 200U);
        push_report(WING_REPORT_STATE_CHANGED);
        return;
    }

    if (g_session.state == SESSION_RUNNING)
    {
        assess_running_status(now);
        push_report(WING_REPORT_STATE_CHANGED);
        return;
    }

    if (g_session.state == SESSION_WAIT_STOP_STATUS)
    {
        if (!msg.moving || msg.fault_code) finish_stop();
        return;
    }
}

void WingController_Task(uint32_t now)
{
    WingState_Tick(&g_state, now);

    if (g_session.state == SESSION_IDLE) return;

    if (g_session.state == SESSION_QUIET_GUARD &&
        (now - g_session.phase_tick) >= WING_PREFLIGHT_QUIET_MS)
    {
        g_session.state = SESSION_WAIT_RD2;
        g_session.phase_tick = now;
        WingState_SetSession(&g_state, WING_CAN_SESSION_PREFLIGHT, WING_PHASE_CHECKING_MOTION,
                             WING_CONTROL_OWNER_NONE);
        if (!send_query(QUERY_RD, now)) fail_direction(5001U, WING_CONTROL_OWNER_NONE);
        else push_report(WING_REPORT_STATE_CHANGED);
        return;
    }

    if ((g_session.state == SESSION_WAIT_RD1 || g_session.state == SESSION_WAIT_RD2 ||
         g_session.state == SESSION_WAIT_RL) && g_session.expected_query != QUERY_NONE &&
        (now - g_session.query_tick) >= WING_QUERY_TIMEOUT_MS)
    {
        fail_direction(g_session.expected_query == QUERY_RD ? 5002U : 5003U,
                       WING_CONTROL_OWNER_NONE);
        return;
    }

    if ((g_session.state == SESSION_WAIT_START_ACK || g_session.state == SESSION_WAIT_START_STATUS) &&
        (now - g_session.phase_tick) >= WING_COMMAND_TIMEOUT_MS)
    {
        if (g_session.state == SESSION_WAIT_START_ACK) fail_direction(5003U, WING_CONTROL_OWNER_NONE);
        else (void)begin_stop(STOP_SAFETY, "", now);
        return;
    }

    if (g_session.state == SESSION_RUNNING)
    {
        if (WingState_NeedsSafetyStop(&g_state))
        {
            (void)begin_stop(STOP_SAFETY, "", now);
            return;
        }
        if (g_session.expected_query == QUERY_NONE &&
            (now - g_session.last_poll_tick) >= WING_ACTIVE_STATUS_POLL_MS)
            (void)send_query(QUERY_RD, now);
        return;
    }

    if (g_session.state == SESSION_WAIT_STOP_STATUS &&
        g_session.expected_query == QUERY_NONE &&
        (now - g_session.last_poll_tick) >= WING_ACTIVE_STATUS_POLL_MS)
        (void)send_query(QUERY_RD, now);

    if ((g_session.state == SESSION_WAIT_STOP_ACK || g_session.state == SESSION_WAIT_STOP_STATUS) &&
        (now - g_session.stop_tick) >= WING_COMMAND_TIMEOUT_MS)
    {
        if (g_session.stop_request_id[0])
            push_result(g_session.stop_request_id, WING_ACTION_STOP, 5003U);
        finish_idle(g_session.stop_cause == STOP_CONFLICT ?
                    WING_CONTROL_OWNER_LOCAL_SUSPECTED : WING_CONTROL_OWNER_NONE);
    }
}

const WingSnapshot* WingController_GetSnapshot(void) { return &g_state.snapshot; }

uint8_t WingController_TakeResult(WingControlResult* result)
{
    if (!result || !g_result_count) return 0U;
    *result = g_results[g_result_head];
    g_result_head = (uint8_t)((g_result_head + 1U) % RESULT_QUEUE_DEPTH);
    g_result_count--;
    return 1U;
}

WingReportReason WingController_TakeReportReason(void)
{
    WingReportReason reason;
    if (!g_report_count) return WING_REPORT_NONE;
    reason = g_reports[g_report_head];
    g_report_head = (uint8_t)((g_report_head + 1U) % REPORT_QUEUE_DEPTH);
    g_report_count--;
    return reason;
}

const char* WingReportReason_Name(WingReportReason reason)
{
    if (reason == WING_REPORT_STATE_CHANGED) return "state_changed";
    if (reason == WING_REPORT_CONTROLLER_OFFLINE) return "controller_offline";
    if (reason == WING_REPORT_CONTROLLER_ONLINE) return "controller_online";
    if (reason == WING_REPORT_FAULT_RAISED) return "fault_raised";
    if (reason == WING_REPORT_FAULT_CLEARED) return "fault_cleared";
    if (reason == WING_REPORT_SAFETY_STOP) return "safety_stop";
    if (reason == WING_REPORT_ENDPOINT_STOP) return "endpoint_stop";
    if (reason == WING_REPORT_CONTROL_CONFLICT) return "control_conflict";
    return "periodic";
}

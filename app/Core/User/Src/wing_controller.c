#include "wing_controller.h"

#include <string.h>

#include "app_log.h"
#include "main.h"
#include "wing_protocol.h"

#define RESULT_QUEUE_DEPTH 2U
#define REPORT_QUEUE_DEPTH 4U
#define CAN_DIAG_QUEUE_DEPTH 8U
#define CAN_DIAG_SUMMARY_MS 5000U

typedef struct
{
    uint8_t active;
    uint8_t acked;
    uint8_t safety;
    uint8_t endpoint;
    WingAction action;
    uint32_t started_tick;
    uint32_t ack_tick;
    char request_id[40];
} PendingControl;

typedef struct
{
    WingFrameKind kind;
    WingAction action;
    uint8_t accepted;
    uint8_t data[8];
    uint16_t position_mm;
    uint16_t top_limit_mm;
    uint16_t bottom_limit_mm;
    uint8_t fault_code;
    uint8_t moving;
    char request_id[40];
} CanDiagEvent;

static WingState g_state;
static PendingControl g_pending;
static WingControlResult g_results[RESULT_QUEUE_DEPTH];
static uint8_t g_result_head;
static uint8_t g_result_count;
static WingReportReason g_reports[REPORT_QUEUE_DEPTH];
static uint8_t g_report_head;
static uint8_t g_report_count;
static uint32_t g_last_poll_tick;
static uint32_t g_last_limit_query_tick;
static uint8_t g_safety_sent;
static uint8_t g_endpoint_stop_attempted;
static CanDiagEvent g_can_diag[CAN_DIAG_QUEUE_DEPTH];
static volatile uint8_t g_can_diag_head;
static volatile uint8_t g_can_diag_count;
static volatile uint16_t g_can_diag_dropped;
static volatile uint16_t g_can_invalid_count;
static volatile uint32_t g_can_invalid_last_id;
static volatile uint8_t g_can_invalid_last_dlc;
static uint16_t g_poll_send_fail_count;
static uint32_t g_can_diag_summary_tick;
static volatile uint8_t g_controller_online_event;

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
    if (g_report_count && g_reports[(uint8_t)((g_report_head + g_report_count - 1U) % REPORT_QUEUE_DEPTH)] == reason) return;
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
    g_results[tail].accepted = (code == 200U) ? 1U : 0U;
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
        APP_LOGI("CAN", "tx rid=%s act=%s id=%08lX %02X%02X%02X%02X%02X%02X%02X%02X",
                 (request_id && request_id[0]) ? request_id : "safety", WingAction_Name(action),
                 (unsigned long)WING_CAN_TX_ID, data[0], data[1], data[2], data[3],
                 data[4], data[5], data[6], data[7]);
    else
        APP_LOGE("CAN", "tx fail rid=%s act=%s id=%08lX",
                 (request_id && request_id[0]) ? request_id : "safety", WingAction_Name(action),
                 (unsigned long)WING_CAN_TX_ID);
    return ok;
}

static void send_status_poll(void)
{
    uint8_t data[8];
    WingProtocol_EncodeStatusQuery(data);
    if (!send_frame(data) && g_poll_send_fail_count < 65535U) g_poll_send_fail_count++;
}

static void send_limit_query(void)
{
    uint8_t data[8];
    WingProtocol_EncodeLimitQuery(data);
    if (!send_frame(data) && g_poll_send_fail_count < 65535U) g_poll_send_fail_count++;
}

static uint32_t status_poll_interval(void)
{
    WingAction action = g_state.snapshot.active_action;
    if (!g_state.snapshot.have_feedback || !g_state.snapshot.controller_online ||
        g_pending.active || g_state.safety_stop_latched ||
        action == WING_ACTION_ASCEND || action == WING_ACTION_DESCEND)
        return WING_ACTIVE_STATUS_POLL_MS;
    return WING_IDLE_STATUS_POLL_MS;
}

static void push_can_diag(const WingCanMessage* msg, const uint8_t data[8], const char* request_id)
{
    uint8_t tail;
    CanDiagEvent* event;
    if (!msg || !data) return;
    if (g_can_diag_count >= CAN_DIAG_QUEUE_DEPTH)
    {
        if (g_can_diag_dropped < 65535U) g_can_diag_dropped++;
        return;
    }
    tail = (uint8_t)((g_can_diag_head + g_can_diag_count) % CAN_DIAG_QUEUE_DEPTH);
    event = &g_can_diag[tail];
    memset(event, 0, sizeof(*event));
    event->kind = msg->kind;
    event->action = msg->action;
    event->accepted = msg->accepted;
    event->position_mm = msg->position_mm;
    event->top_limit_mm = msg->top_limit_mm;
    event->bottom_limit_mm = msg->bottom_limit_mm;
    event->fault_code = msg->fault_code;
    event->moving = msg->moving;
    memcpy(event->data, data, sizeof(event->data));
    copy_id(event->request_id, request_id);
    g_can_diag_count++;
}

static void drain_can_diag(void)
{
    CanDiagEvent event;
    uint8_t have_event;
    do
    {
        have_event = 0U;
        __disable_irq();
        if (g_can_diag_count)
        {
            event = g_can_diag[g_can_diag_head];
            g_can_diag_head = (uint8_t)((g_can_diag_head + 1U) % CAN_DIAG_QUEUE_DEPTH);
            g_can_diag_count--;
            have_event = 1U;
        }
        __enable_irq();
        if (have_event && event.kind == WING_FRAME_ACK)
            APP_LOGI("CAN", "rx ack rid=%s act=%s ok=%u %02X%02X%02X%02X%02X%02X%02X%02X",
                     event.request_id, WingAction_Name(event.action), (unsigned)event.accepted,
                     event.data[0], event.data[1], event.data[2], event.data[3],
                     event.data[4], event.data[5], event.data[6], event.data[7]);
        else if (have_event && event.kind == WING_FRAME_STATUS)
            APP_LOGI("CAN", "rx stat rid=%s d=%02X%02X%02X%02X%02X%02X%02X%02X p=%u f=%u m=%u ok=%u",
                     event.request_id,
                     event.data[0], event.data[1], event.data[2], event.data[3],
                     event.data[4], event.data[5], event.data[6], event.data[7],
                     (unsigned)event.position_mm, (unsigned)event.fault_code,
                     (unsigned)event.moving, (unsigned)event.accepted);
        else if (have_event && event.kind == WING_FRAME_LIMITS)
            APP_LOGI("CAN", "rx limit rid=%s top=%u bottom=%u",
                     event.request_id, (unsigned)event.top_limit_mm,
                     (unsigned)event.bottom_limit_mm);
    } while (have_event);
}

static void clear_pending(void) { memset(&g_pending, 0, sizeof(g_pending)); }

static void begin_pending(WingAction action, const char* request_id, uint8_t safety,
                          uint8_t endpoint, uint32_t now)
{
    clear_pending();
    g_pending.active = 1U;
    g_pending.action = action;
    g_pending.safety = safety;
    g_pending.endpoint = endpoint;
    g_pending.started_tick = now;
    copy_id(g_pending.request_id, request_id);
}

void WingController_Init(void)
{
    CAN_FilterTypeDef filter;
    WingState_Init(&g_state);
    clear_pending();
    g_result_head = g_result_count = 0U;
    g_report_head = g_report_count = 0U;
    g_last_poll_tick = HAL_GetTick();
    g_last_limit_query_tick = HAL_GetTick();
    g_safety_sent = 0U;
    g_endpoint_stop_attempted = 0U;
    memset(g_can_diag, 0, sizeof(g_can_diag));
    g_can_diag_head = g_can_diag_count = 0U;
    g_can_diag_dropped = g_can_invalid_count = 0U;
    g_can_invalid_last_id = 0U; g_can_invalid_last_dlc = 0U;
    g_poll_send_fail_count = 0U; g_can_diag_summary_tick = HAL_GetTick();
    g_controller_online_event = 0U;
    memset(&filter, 0, sizeof(filter));
    filter.FilterBank = (TOP_WING_CAN_BUS == 1) ? 0U : 14U;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterActivation = ENABLE;
    filter.SlaveStartFilterBank = 14U;
    /* Hardware bank accepts extended frames; WingProtocol_Decode enforces the configured response ID and DLC 8. */
    (void)HAL_CAN_ConfigFilter(selected_can(), &filter);
    (void)HAL_CAN_Start(selected_can());
    (void)HAL_CAN_ActivateNotification(selected_can(), CAN_IT_RX_FIFO0_MSG_PENDING);
    push_report(WING_REPORT_CONTROLLER_OFFLINE);
    APP_LOGI("WING", "CAN%u ext tx=0x%08lX rx=0x%08lX 500kbps", (unsigned)TOP_WING_CAN_BUS,
             (unsigned long)WING_CAN_TX_ID, (unsigned long)WING_CAN_RX_ID);
}

uint16_t WingController_Request(WingAction action, const char* request_id)
{
    uint16_t code;
    uint32_t now = HAL_GetTick();
    if (!request_id || !request_id[0] || action == WING_ACTION_NONE) return 4001U;

    if (action == WING_ACTION_STOP && g_pending.active && !g_pending.safety && g_pending.action != WING_ACTION_STOP)
    {
        push_result(g_pending.request_id, g_pending.action, 5003U);
        clear_pending();
    }
    else if (g_pending.active)
    {
        APP_LOGW("WING", "reject rid=%s act=%s code=%u", request_id, WingAction_Name(action),
                 (unsigned)(action == WING_ACTION_STOP ? 5003U : 4003U));
        return action == WING_ACTION_STOP ? 5003U : 4003U;
    }

    code = WingState_AdmissionCode(&g_state, action);
    if (code)
    {
        APP_LOGW("WING", "reject rid=%s act=%s code=%u", request_id, WingAction_Name(action), (unsigned)code);
        return code;
    }
    if (action == WING_ACTION_STOP && g_state.snapshot.motion_state == WING_MOTION_STOPPED &&
        g_state.snapshot.active_action == WING_ACTION_NONE && !g_state.snapshot.fault_code)
    {
        WingState_ConfirmStop(&g_state, request_id, 1U);
        push_report(WING_REPORT_STATE_CHANGED);
        return 200U;
    }
    if (!send_action(action, request_id)) return 5001U;
    begin_pending(action, request_id, 0U, 0U, now);
    APP_LOGI("WING", "request action=%s id=%s", WingAction_Name(action), request_id);
    return 0U;
}

static void finalize_from_status(uint32_t now)
{
    if (!g_pending.active || !g_pending.acked || g_state.last_feedback_tick < g_pending.ack_tick) return;
    if (g_pending.action == WING_ACTION_STOP)
    {
        if (g_state.snapshot.motion_state != WING_MOTION_STOPPED && g_state.snapshot.motion_state != WING_MOTION_FAULT) return;
        if (g_state.raw_moving) return;
        WingState_ConfirmStop(&g_state, g_pending.request_id,
                              (g_pending.safety || g_pending.endpoint) ? 0U : 1U);
        if (!g_pending.safety && !g_pending.endpoint)
            push_result(g_pending.request_id, WING_ACTION_STOP, 200U);
        APP_LOGI("WING", "done rid=%s act=stop code=200", g_pending.request_id);
        push_report(g_pending.safety ? WING_REPORT_SAFETY_STOP :
                    (g_pending.endpoint ? WING_REPORT_ENDPOINT_STOP : WING_REPORT_STATE_CHANGED));
        g_safety_sent = 0U;
        clear_pending();
        return;
    }
    WingState_AcceptDirection(&g_state, g_pending.action, g_pending.request_id, now);
    g_endpoint_stop_attempted = 0U;
    push_result(g_pending.request_id, g_pending.action, 200U);
    APP_LOGI("WING", "done rid=%s act=%s code=200", g_pending.request_id, WingAction_Name(g_pending.action));
    push_report(WING_REPORT_STATE_CHANGED);
    clear_pending();
}

static void maybe_start_endpoint_stop(uint32_t now)
{
    WingAction action = g_state.snapshot.active_action;
    if (g_pending.active || g_endpoint_stop_attempted) return;
    if (action != WING_ACTION_ASCEND && action != WING_ACTION_DESCEND) return;
    if (!WingState_EndpointReached(&g_state, action)) return;
    g_endpoint_stop_attempted = 1U;
    if (!send_action(WING_ACTION_STOP, ""))
    {
        APP_LOGE("WING", "endpoint stop tx failed act=%s pos=%u",
                 WingAction_Name(action), (unsigned)g_state.snapshot.position_mm);
        push_report(WING_REPORT_ENDPOINT_STOP);
        return;
    }
    APP_LOGI("WING", "endpoint stop act=%s pos=%u top=%u bottom=%u",
             WingAction_Name(action), (unsigned)g_state.snapshot.position_mm,
             (unsigned)g_state.snapshot.top_limit_mm,
             (unsigned)g_state.snapshot.bottom_limit_mm);
    begin_pending(WING_ACTION_STOP, "", 0U, 1U, now);
    push_report(WING_REPORT_ENDPOINT_STOP);
}

void WingController_OnCanRx(CAN_HandleTypeDef* hcan)
{
    CAN_RxHeaderTypeDef header;
    uint8_t data[8];
    WingCanMessage msg;
    WingMotionState old_motion;
    uint8_t old_online, old_fault;
    uint32_t now;
    if (hcan != selected_can()) return;
    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, data) != HAL_OK)
    {
        if (g_can_invalid_count < 65535U) g_can_invalid_count++;
        return;
    }
    if (header.IDE != CAN_ID_EXT || !WingProtocol_Decode(header.ExtId, (uint8_t)header.DLC, data, &msg))
    {
        if (g_can_invalid_count < 65535U) g_can_invalid_count++;
        g_can_invalid_last_id = header.ExtId;
        g_can_invalid_last_dlc = (uint8_t)header.DLC;
        return;
    }
    now = HAL_GetTick();
    if (g_pending.active) push_can_diag(&msg, data, g_pending.request_id);
    if (msg.kind == WING_FRAME_ACK && g_pending.active && msg.action == g_pending.action)
    {
        if (!msg.accepted)
        {
            if (!g_pending.safety && !g_pending.endpoint)
                push_result(g_pending.request_id, g_pending.action, 5004U);
            clear_pending();
            return;
        }
        g_pending.acked = 1U;
        g_pending.ack_tick = now;
        send_status_poll();
        return;
    }
    if (msg.kind == WING_FRAME_LIMITS)
    {
        uint8_t changed = !g_state.snapshot.have_limits ||
                          g_state.snapshot.top_limit_mm != msg.top_limit_mm ||
                          g_state.snapshot.bottom_limit_mm != msg.bottom_limit_mm;
        if (!WingState_ApplyLimits(&g_state, msg.top_limit_mm, msg.bottom_limit_mm))
        {
            APP_LOGW("WING", "invalid limits top=%u bottom=%u",
                     (unsigned)msg.top_limit_mm, (unsigned)msg.bottom_limit_mm);
            return;
        }
        if (changed) push_report(WING_REPORT_STATE_CHANGED);
        maybe_start_endpoint_stop(now);
        return;
    }
    if (msg.kind != WING_FRAME_STATUS || !msg.accepted) return;
    old_motion = g_state.snapshot.motion_state;
    old_online = g_state.snapshot.controller_online;
    old_fault = g_state.snapshot.fault_code;
    if (!old_online)
    {
        if (g_state.snapshot.have_feedback) WingState_InvalidateLimits(&g_state);
        g_last_limit_query_tick = now - WING_LIMIT_RETRY_MS;
    }
    WingState_ApplyFeedback(&g_state, now, msg.position_mm, msg.fault_code, msg.moving);
    if (!old_online) { g_controller_online_event = 1U; push_report(WING_REPORT_CONTROLLER_ONLINE); }
    if (!old_fault && msg.fault_code) push_report(WING_REPORT_FAULT_RAISED);
    else if (old_fault && !msg.fault_code) push_report(WING_REPORT_FAULT_CLEARED);
    else if (old_motion != g_state.snapshot.motion_state) push_report(WING_REPORT_STATE_CHANGED);
    finalize_from_status(now);
    maybe_start_endpoint_stop(now);
}

void WingController_Task(uint32_t now)
{
    uint8_t was_online = g_state.snapshot.controller_online;
    drain_can_diag();
    if (g_controller_online_event)
    {
        __disable_irq();g_controller_online_event=0U;__enable_irq();
        APP_LOGI("WING", "controller online");
    }
    WingState_Tick(&g_state, now);
    if (was_online && !g_state.snapshot.controller_online)
    {
        APP_LOGW("WING", "controller offline age=%lu", (unsigned long)g_state.snapshot.feedback_age_ms);
        push_report(WING_REPORT_CONTROLLER_OFFLINE);
    }

    if ((now - g_can_diag_summary_tick) >= CAN_DIAG_SUMMARY_MS)
    {
        uint16_t invalid,dropped;
        __disable_irq();
        invalid=g_can_invalid_count;dropped=g_can_diag_dropped;
        g_can_invalid_count=0U;g_can_diag_dropped=0U;
        __enable_irq();
        if(invalid)APP_LOGW("CAN","invalid count=%u lastId=%08lX dlc=%u",(unsigned)invalid,
                           (unsigned long)g_can_invalid_last_id,(unsigned)g_can_invalid_last_dlc);
        if(dropped)APP_LOGW("CAN","diag dropped=%u",(unsigned)dropped);
        if(g_poll_send_fail_count){APP_LOGW("CAN","poll tx fail count=%u",(unsigned)g_poll_send_fail_count);g_poll_send_fail_count=0U;}
        g_can_diag_summary_tick=now;
    }

    if (g_pending.active && (now - g_pending.started_tick) >= WING_COMMAND_TIMEOUT_MS)
    {
        APP_LOGW("WING", "timeout rid=%s act=%s ack=%u", g_pending.request_id,
                 WingAction_Name(g_pending.action), (unsigned)g_pending.acked);
        if (!g_pending.safety && !g_pending.endpoint)
            push_result(g_pending.request_id, g_pending.action, 5003U);
        clear_pending();
    }

    if (WingState_NeedsSafetyStop(&g_state) && !g_safety_sent)
    {
        if (g_pending.active && !g_pending.safety)
        {
            push_result(g_pending.request_id, g_pending.action, 5003U);
            clear_pending();
        }
        if (!g_pending.active && send_action(WING_ACTION_STOP, ""))
        {
            APP_LOGW("WING", "safety stop reason=%s", WingSafetyReason_Name(g_state.snapshot.safety_reason));
            begin_pending(WING_ACTION_STOP, "", 1U, 0U, now);
            g_safety_sent = 1U;
            push_report(WING_REPORT_SAFETY_STOP);
        }
    }

    if ((now - g_last_poll_tick) >= status_poll_interval())
    {
        g_last_poll_tick = now;
        send_status_poll();
    }

    if (g_state.snapshot.controller_online && !g_state.snapshot.have_limits &&
        (now - g_last_limit_query_tick) >= WING_LIMIT_RETRY_MS)
    {
        g_last_limit_query_tick = now;
        send_limit_query();
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
    return "periodic";
}

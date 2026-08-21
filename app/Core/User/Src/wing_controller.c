#include "wing_controller.h"

#include <string.h>

#include "app_log.h"
#include "main.h"
#include "wing_protocol.h"

#define RESULT_QUEUE_DEPTH 2U
#define REPORT_QUEUE_DEPTH 4U

typedef struct
{
    uint8_t active;
    uint8_t acked;
    uint8_t safety;
    WingAction action;
    uint32_t started_tick;
    uint32_t ack_tick;
    char request_id[40];
} PendingControl;

static WingState g_state;
static PendingControl g_pending;
static WingControlResult g_results[RESULT_QUEUE_DEPTH];
static uint8_t g_result_head;
static uint8_t g_result_count;
static WingReportReason g_reports[REPORT_QUEUE_DEPTH];
static uint8_t g_report_head;
static uint8_t g_report_count;
static uint32_t g_last_poll_tick;
static uint8_t g_safety_sent;

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

static uint8_t send_action(WingAction action)
{
    uint8_t data[8];
    return WingProtocol_EncodeAction(action, data) && send_frame(data);
}

static void send_poll(void)
{
    uint8_t data[8];
    WingProtocol_EncodeStatusQuery(data);
    (void)send_frame(data);
}

static void clear_pending(void) { memset(&g_pending, 0, sizeof(g_pending)); }

static void begin_pending(WingAction action, const char* request_id, uint8_t safety, uint32_t now)
{
    clear_pending();
    g_pending.active = 1U;
    g_pending.action = action;
    g_pending.safety = safety;
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
    g_safety_sent = 0U;
    memset(&filter, 0, sizeof(filter));
    filter.FilterBank = (TOP_WING_CAN_BUS == 1) ? 0U : 14U;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterActivation = ENABLE;
    filter.SlaveStartFilterBank = 14U;
    /* Hardware bank accepts extended frames; WingProtocol_Decode enforces exact ID 0x2 and DLC 8. */
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
        return action == WING_ACTION_STOP ? 5003U : 4003U;
    }

    code = WingState_AdmissionCode(&g_state, action);
    if (code) return code;
    if (action == WING_ACTION_STOP && g_state.snapshot.motion_state == WING_MOTION_STOPPED &&
        g_state.snapshot.active_action == WING_ACTION_NONE && !g_state.snapshot.fault_code)
    {
        WingState_ConfirmStop(&g_state, request_id, 1U);
        push_report(WING_REPORT_STATE_CHANGED);
        return 200U;
    }
    if (!send_action(action)) return 5001U;
    begin_pending(action, request_id, 0U, now);
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
        WingState_ConfirmStop(&g_state, g_pending.request_id, g_pending.safety ? 0U : 1U);
        if (!g_pending.safety) push_result(g_pending.request_id, WING_ACTION_STOP, 200U);
        push_report(g_pending.safety ? WING_REPORT_SAFETY_STOP : WING_REPORT_STATE_CHANGED);
        g_safety_sent = 0U;
        clear_pending();
        return;
    }
    WingState_AcceptDirection(&g_state, g_pending.action, g_pending.request_id, now);
    push_result(g_pending.request_id, g_pending.action, 200U);
    push_report(WING_REPORT_STATE_CHANGED);
    clear_pending();
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
    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, data) != HAL_OK) return;
    if (header.IDE != CAN_ID_EXT || !WingProtocol_Decode(header.ExtId, (uint8_t)header.DLC, data, &msg)) return;
    now = HAL_GetTick();
    if (msg.kind == WING_FRAME_ACK && g_pending.active && msg.action == g_pending.action)
    {
        if (!msg.accepted)
        {
            if (!g_pending.safety) push_result(g_pending.request_id, g_pending.action, 5004U);
            clear_pending();
            return;
        }
        g_pending.acked = 1U;
        g_pending.ack_tick = now;
        send_poll();
        return;
    }
    if (msg.kind != WING_FRAME_STATUS || !msg.accepted) return;
    old_motion = g_state.snapshot.motion_state;
    old_online = g_state.snapshot.controller_online;
    old_fault = g_state.snapshot.fault_code;
    WingState_ApplyFeedback(&g_state, now, msg.position_mm, msg.fault_code, msg.moving);
    if (!old_online) push_report(WING_REPORT_CONTROLLER_ONLINE);
    if (!old_fault && msg.fault_code) push_report(WING_REPORT_FAULT_RAISED);
    else if (old_fault && !msg.fault_code) push_report(WING_REPORT_FAULT_CLEARED);
    else if (old_motion != g_state.snapshot.motion_state) push_report(WING_REPORT_STATE_CHANGED);
    finalize_from_status(now);
}

void WingController_Task(uint32_t now)
{
    uint8_t was_online = g_state.snapshot.controller_online;
    WingState_Tick(&g_state, now);
    if (was_online && !g_state.snapshot.controller_online) push_report(WING_REPORT_CONTROLLER_OFFLINE);

    if (g_pending.active && (now - g_pending.started_tick) >= WING_COMMAND_TIMEOUT_MS)
    {
        if (!g_pending.safety) push_result(g_pending.request_id, g_pending.action, 5003U);
        clear_pending();
    }

    if (WingState_NeedsSafetyStop(&g_state) && !g_safety_sent)
    {
        if (g_pending.active && !g_pending.safety)
        {
            push_result(g_pending.request_id, g_pending.action, 5003U);
            clear_pending();
        }
        if (!g_pending.active && send_action(WING_ACTION_STOP))
        {
            begin_pending(WING_ACTION_STOP, "", 1U, now);
            g_safety_sent = 1U;
            push_report(WING_REPORT_SAFETY_STOP);
        }
    }

    if ((now - g_last_poll_tick) >= WING_STATUS_POLL_MS)
    {
        g_last_poll_tick = now;
        send_poll();
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
    return "periodic";
}

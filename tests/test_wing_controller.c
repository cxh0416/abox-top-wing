#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "app_log.h"
#include "wing_controller.h"

CAN_HandleTypeDef hcan1 = { 1U };
CAN_HandleTypeDef hcan2 = { 2U };

static uint32_t g_tick;
static uint8_t g_tx[512][8];
static uint16_t g_tx_count;
static uint8_t g_rx[8];
static uint8_t g_have_rx;

uint32_t HAL_GetTick(void) { return g_tick; }
void AppLog_Printf(AppLogLevel_t level, const char* tag, const char* fmt, ...)
{ (void)level; (void)tag; (void)fmt; }

int HAL_CAN_AddTxMessage(CAN_HandleTypeDef* hcan, CAN_TxHeaderTypeDef* header,
                         uint8_t data[8], uint32_t* mailbox)
{
    (void)hcan;
    assert(header->ExtId == WING_CAN_TX_ID && header->IDE == CAN_ID_EXT && header->DLC == 8U);
    assert(g_tx_count < 512U);
    memcpy(g_tx[g_tx_count++], data, 8U);
    if (mailbox) *mailbox = 0U;
    return HAL_OK;
}

int HAL_CAN_GetRxMessage(CAN_HandleTypeDef* hcan, uint32_t fifo,
                         CAN_RxHeaderTypeDef* header, uint8_t data[8])
{
    (void)hcan; (void)fifo;
    if (!g_have_rx) return HAL_ERROR;
    header->ExtId = WING_CAN_RX_ID;
    header->IDE = CAN_ID_EXT;
    header->DLC = 8U;
    memcpy(data, g_rx, 8U);
    g_have_rx = 0U;
    return HAL_OK;
}

int HAL_CAN_ConfigFilter(CAN_HandleTypeDef* hcan, CAN_FilterTypeDef* filter)
{ (void)hcan; (void)filter; return HAL_OK; }
int HAL_CAN_Start(CAN_HandleTypeDef* hcan) { (void)hcan; return HAL_OK; }
int HAL_CAN_ActivateNotification(CAN_HandleTypeDef* hcan, uint32_t notification)
{ (void)hcan; (void)notification; return HAL_OK; }

static void reset_controller(void)
{
    g_tick = 0U;
    g_tx_count = 0U;
    g_have_rx = 0U;
    WingController_Init();
}

static void receive(const uint8_t data[8], uint32_t tick)
{
    memcpy(g_rx, data, 8U);
    g_have_rx = 1U;
    g_tick = tick;
    WingController_OnCanRx(&hcan2);
    assert(!g_have_rx);
}

static void status_with_fault(uint16_t position, uint8_t moving, uint8_t fault, uint32_t tick)
{
    uint8_t data[8] = { 'R', 'D', (uint8_t)(position >> 8), (uint8_t)position,
                        fault, moving, 0U, 0U };
    receive(data, tick);
}

static void status(uint16_t position, uint8_t moving, uint32_t tick)
{ status_with_fault(position, moving, 0U, tick); }

static void limits(uint16_t top, uint16_t bottom, uint32_t tick)
{
    uint8_t data[8] = { 'R', 'L', (uint8_t)(top >> 8), (uint8_t)top,
                        (uint8_t)(bottom >> 8), (uint8_t)bottom, 0U, 0U };
    receive(data, tick);
}

static void ack(WingAction action, uint32_t tick)
{
    uint8_t data[8] = { 0U };
    if (action == WING_ACTION_ASCEND) { data[0] = 'U'; data[1] = 'P'; }
    else if (action == WING_ACTION_DESCEND) { data[0] = 'D'; data[1] = 'N'; }
    else { data[0] = 'S'; data[1] = 'T'; }
    receive(data, tick);
}

static uint16_t count_command(char first, char second)
{
    uint16_t i, count = 0U;
    for (i = 0U; i < g_tx_count; ++i)
        if (g_tx[i][0] == (uint8_t)first && g_tx[i][1] == (uint8_t)second) count++;
    return count;
}

static WingControlResult take_result(void)
{
    WingControlResult result;
    assert(WingController_TakeResult(&result));
    return result;
}

static void drain_reports(void)
{
    while (WingController_TakeReportReason() != WING_REPORT_NONE) { }
}

static void start_running(WingAction action, const char* rid, uint16_t position)
{
    WingControlResult result;
    g_tick = 0U;
    assert(WingController_Request(action, rid) == 0U);
    assert(count_command('R', 'D') == 1U);
    status(position, 0U, 1U);
    WingController_Task(250U);
    assert(count_command('R', 'D') == 1U);
    WingController_Task(251U);
    assert(count_command('R', 'D') == 2U);
    status(position, 0U, 252U);
    assert(count_command('R', 'L') == 1U);
    limits(100U, 10U, 253U);
    assert(count_command(action == WING_ACTION_ASCEND ? 'U' : 'D',
                         action == WING_ACTION_ASCEND ? 'P' : 'N') == 1U);
    ack(action, 254U);
    assert(count_command('R', 'D') == 3U);
    status(position, 1U, 255U);
    result = take_result();
    assert(result.code == 200U && result.action == action && !strcmp(result.request_id, rid));
    assert(WingController_GetSnapshot()->can_session == WING_CAN_SESSION_REMOTE_RUNNING);
}

static void test_idle_silent_and_correlation(void)
{
    const WingSnapshot* snapshot;
    reset_controller();
    WingController_Task(100U);
    WingController_Task(300000U);
    assert(g_tx_count == 0U);
    status(30U, 1U, 300001U);
    limits(100U, 10U, 300002U);
    assert(g_tx_count == 0U);
    snapshot = WingController_GetSnapshot();
    assert(snapshot->motion_state == WING_MOTION_UNKNOWN);
    assert(!snapshot->status_fresh && !snapshot->controller_online_known);
    assert(!snapshot->have_position && !snapshot->have_limits);
    assert(WingController_Request(WING_ACTION_STOP, "idle-stop") == 0U);
    assert(count_command('S', 'T') == 1U);
    ack(WING_ACTION_STOP, 300003U);
    status(30U, 0U, 300004U);
    WingControlResult result = take_result();
    assert(result.code == 200U && result.stop_confirmed);
}

static void test_preflight_local_motion_rejected(void)
{
    WingControlResult result;
    reset_controller();
    assert(WingController_Request(WING_ACTION_ASCEND, "local") == 0U);
    status(20U, 1U, 1U);
    result = take_result();
    assert(result.code == 4003U);
    assert(count_command('S', 'T') == 0U && count_command('U', 'P') == 0U);
    assert(WingController_GetSnapshot()->control_owner == WING_CONTROL_OWNER_LOCAL_SUSPECTED);
    assert(WingController_GetSnapshot()->can_session == WING_CAN_SESSION_IDLE_SILENT);
}

static void test_preflight_timeouts_and_limits(void)
{
    WingControlResult result;
    reset_controller();
    assert(WingController_Request(WING_ACTION_ASCEND, "no-rd") == 0U);
    WingController_Task(300U);
    result = take_result();
    assert(result.code == 5002U && count_command('U', 'P') == 0U);

    reset_controller();
    assert(WingController_Request(WING_ACTION_ASCEND, "bad-limits") == 0U);
    status(20U, 0U, 1U);
    WingController_Task(251U);
    status(20U, 0U, 252U);
    limits(10U, 20U, 253U);
    result = take_result();
    assert(result.code == 5003U && count_command('U', 'P') == 0U);
}

static void test_endpoint_single_stop(void)
{
    reset_controller();
    start_running(WING_ACTION_ASCEND, "endpoint", 50U);
    WingController_Task(355U);
    status(98U, 1U, 356U);
    assert(count_command('S', 'T') == 1U);
    status(99U, 1U, 357U);
    WingController_Task(500U);
    assert(count_command('S', 'T') == 1U);
    ack(WING_ACTION_STOP, 501U);
    status(100U, 0U, 502U);
    assert(WingController_GetSnapshot()->can_session == WING_CAN_SESSION_IDLE_SILENT);
    assert(!WingController_GetSnapshot()->status_fresh);
}

static void test_running_feedback_uses_periodic_mqtt_policy(void)
{
    reset_controller();
    start_running(WING_ACTION_ASCEND, "periodic", 50U);
    drain_reports();
    WingController_Task(355U);
    status(55U, 1U, 356U);
    assert(WingController_TakeReportReason() == WING_REPORT_NONE);
    assert(!strcmp(WingReportReason_Name(WING_REPORT_PERIODIC), "periodic"));
}

static void test_conflict_and_unexpected_stop(void)
{
    reset_controller();
    start_running(WING_ACTION_ASCEND, "reverse", 50U);
    WingController_Task(355U); status(60U, 1U, 356U);
    WingController_Task(456U); status(55U, 1U, 457U);
    assert(count_command('S', 'T') == 0U);
    WingController_Task(557U); status(54U, 1U, 558U);
    assert(count_command('S', 'T') == 1U);
    assert(WingController_GetSnapshot()->control_owner == WING_CONTROL_OWNER_LOCAL_SUSPECTED);

    reset_controller();
    start_running(WING_ACTION_DESCEND, "released", 50U);
    WingController_Task(355U); status(45U, 0U, 356U);
    assert(count_command('S', 'T') == 1U);
}

static void test_stop_preemption_and_safety(void)
{
    WingControlResult result;
    reset_controller();
    assert(WingController_Request(WING_ACTION_ASCEND, "preempted") == 0U);
    status(20U, 0U, 1U);
    assert(WingController_Request(WING_ACTION_STOP, "stop-now") == 0U);
    assert(count_command('S', 'T') == 1U);
    result = take_result();
    assert(result.code == 5003U && result.action == WING_ACTION_ASCEND);
    ack(WING_ACTION_STOP, 2U);
    status(20U, 0U, 3U);
    result = take_result();
    assert(result.code == 200U && result.action == WING_ACTION_STOP);

    reset_controller();
    start_running(WING_ACTION_ASCEND, "feedback-timeout", 50U);
    WingController_Task(756U);
    assert(count_command('S', 'T') == 1U);
    assert(WingController_GetSnapshot()->safety_reason == WING_SAFETY_FEEDBACK_TIMEOUT);
}

static void test_direction_change_fault_and_max_runtime(void)
{
    uint32_t tick;
    reset_controller();
    start_running(WING_ACTION_ASCEND, "switch-up", 50U);
    assert(WingController_Request(WING_ACTION_DESCEND, "switch-down-early") == 4003U);
    assert(WingController_Request(WING_ACTION_STOP, "switch-stop") == 0U);
    ack(WING_ACTION_STOP, 256U);
    status(50U, 0U, 257U);
    (void)take_result();
    g_tick = 258U;
    assert(WingController_Request(WING_ACTION_DESCEND, "switch-down") == 0U);
    assert(count_command('R', 'D') == 5U);

    reset_controller();
    start_running(WING_ACTION_ASCEND, "fault", 50U);
    WingController_Task(355U);
    status_with_fault(55U, 1U, 1U, 356U);
    assert(count_command('S', 'T') == 1U);
    assert(WingController_GetSnapshot()->safety_reason == WING_SAFETY_CONTROLLER_FAULT);

    reset_controller();
    start_running(WING_ACTION_ASCEND, "max-runtime", 50U);
    for (tick = 355U; tick < 120255U; tick += 400U)
    {
        WingController_Task(tick);
        status(50U, 1U, tick + 1U);
    }
    WingController_Task(120255U);
    assert(count_command('S', 'T') == 1U);
    assert(WingController_GetSnapshot()->safety_reason == WING_SAFETY_MAX_RUNTIME);
}

int main(void)
{
    test_idle_silent_and_correlation();
    test_preflight_local_motion_rejected();
    test_preflight_timeouts_and_limits();
    test_running_feedback_uses_periodic_mqtt_policy();
    test_endpoint_single_stop();
    test_conflict_and_unexpected_stop();
    test_stop_preemption_and_safety();
    test_direction_change_fault_and_max_runtime();
    return 0;
}

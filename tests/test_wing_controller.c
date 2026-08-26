#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "app_log.h"
#include "wing_controller.h"

CAN_HandleTypeDef hcan1 = { 1U };
CAN_HandleTypeDef hcan2 = { 2U };

static uint32_t g_tick;
static uint8_t g_tx[64][8];
static uint8_t g_tx_count;
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
    assert(g_tx_count < 64U);
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

static void receive(const uint8_t data[8], uint32_t tick)
{
    memcpy(g_rx, data, 8U);
    g_have_rx = 1U;
    g_tick = tick;
    WingController_OnCanRx(&hcan2);
    assert(!g_have_rx);
}

static void status(uint16_t position, uint8_t moving, uint32_t tick)
{
    uint8_t data[8] = { 'R', 'D', (uint8_t)(position >> 8), (uint8_t)position, 0U, moving, 0U, 0U };
    receive(data, tick);
}

static void limits(uint16_t top, uint16_t bottom, uint32_t tick)
{
    uint8_t data[8] = { 'R', 'L', (uint8_t)(top >> 8), (uint8_t)top,
                        (uint8_t)(bottom >> 8), (uint8_t)bottom, 0U, 0U };
    receive(data, tick);
}

static void ack(char first, char second, uint32_t tick)
{
    uint8_t data[8] = { (uint8_t)first, (uint8_t)second, 0U, 0U, 0U, 0U, 0U, 0U };
    receive(data, tick);
}

static uint8_t count_command(char first, char second)
{
    uint8_t i, count = 0U;
    for (i = 0U; i < g_tx_count; ++i)
        if (g_tx[i][0] == (uint8_t)first && g_tx[i][1] == (uint8_t)second) count++;
    return count;
}

static void test_status_poll_cadence(void)
{
    g_tick = 0U;
    g_tx_count = 0U;
    WingController_Init();
    status(10U, 0U, 10U);
    WingController_Task(10U);
    assert(count_command('R', 'L') == 1U);
    assert(count_command('R', 'D') == 0U);
    limits(100U, 10U, 11U);
    WingController_Task(999U);
    assert(count_command('R', 'D') == 0U);
    WingController_Task(1000U);
    assert(count_command('R', 'D') == 1U);

    status(20U, 1U, 1001U);
    WingController_Task(1999U);
    assert(count_command('R', 'D') == 1U);
    WingController_Task(2000U);
    assert(count_command('R', 'D') == 1U);

    status(20U, 0U, 2001U);
    g_tick = 2010U;
    assert(WingController_Request(WING_ACTION_ASCEND, "poll-up") == 0U);
    ack('U', 'P', 2011U);
    assert(count_command('R', 'D') == 2U);
    status(21U, 1U, 2012U);
    WingController_Task(2111U);
    assert(count_command('R', 'D') == 3U);
}

static void test_local_motion_query_silence(void)
{
    g_tick = 0U;
    g_tx_count = 0U;
    WingController_Init();
    status(10U, 0U, 10U);
    WingController_Task(10U);
    limits(100U, 10U, 11U);

    status(20U, 1U, 1000U);
    WingController_Task(2000U);
    WingController_Task(4000U);
    WingController_Task(120999U);
    assert(count_command('R', 'D') == 0U);
    assert(count_command('R', 'L') == 1U);
    assert(count_command('S', 'T') == 0U);

    WingController_Task(121000U);
    assert(count_command('R', 'D') == 1U);
    status(25U, 0U, 121001U);
    WingController_Task(122000U);
    assert(count_command('R', 'D') == 1U);
    WingController_Task(122001U);
    assert(count_command('R', 'D') == 2U);
    assert(count_command('S', 'T') == 0U);
}

int main(void)
{
    const WingSnapshot* snapshot;
    test_status_poll_cadence();
    test_local_motion_query_silence();
    g_tick = 0U;
    g_tx_count = 0U;
    WingController_Init();

    status(10U, 0U, 10U);
    WingController_Task(10U);
    assert(count_command('R', 'L') == 1U);
    limits(100U, 10U, 11U);

    g_tick = 20U;
    assert(WingController_Request(WING_ACTION_ASCEND, "up-1") == 0U);
    assert(count_command('U', 'P') == 1U);
    ack('U', 'P', 21U);
    status(50U, 1U, 22U);
    status(97U, 1U, 23U);
    assert(count_command('S', 'T') == 0U);
    status(98U, 1U, 24U);
    assert(count_command('S', 'T') == 1U);
    status(99U, 1U, 25U);
    assert(count_command('S', 'T') == 1U);
    ack('S', 'T', 26U);
    status(100U, 0U, 27U);
    snapshot = WingController_GetSnapshot();
    assert(snapshot->motion_state == WING_MOTION_STOPPED);
    assert(snapshot->active_action == WING_ACTION_NONE);
    status(100U, 0U, 28U);
    g_tick = 29U;
    assert(WingController_Request(WING_ACTION_STOP, "idle-stop") == 200U);
    assert(count_command('S', 'T') == 1U);

    status(50U, 1U, 30U);
    snapshot = WingController_GetSnapshot();
    assert(snapshot->motion_state == WING_MOTION_MOVING_UNKNOWN);
    assert(snapshot->active_action == WING_ACTION_NONE);
    assert(count_command('S', 'T') == 1U);
    status(100U, 0U, 31U);

    g_tick = 40U;
    assert(WingController_Request(WING_ACTION_DESCEND, "down-1") == 0U);
    assert(count_command('D', 'N') == 1U);
    ack('D', 'N', 41U);
    status(50U, 1U, 42U);
    assert(WingController_Request(WING_ACTION_ASCEND, "reverse") == 4003U);
    status(13U, 1U, 43U);
    assert(count_command('S', 'T') == 1U);
    status(12U, 1U, 44U);
    assert(count_command('S', 'T') == 2U);
    status(11U, 1U, 45U);
    assert(count_command('S', 'T') == 2U);

    return 0;
}

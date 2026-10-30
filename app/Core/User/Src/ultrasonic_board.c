#include "ultrasonic_board.h"
#include "main.h"
#include "ultrasonic_alarm.h"
#include <string.h>

static UltrasonicAlarm alarm;
static volatile uint8_t ready;
static volatile uint8_t stop_pending;
static UltrasonicDiagnostics diagnostics;

void UltrasonicBoard_Init(void)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    ready = 0U;
    stop_pending = 0U;
    UltrasonicAlarm_Init(&alarm);
    memset(&diagnostics, 0, sizeof(diagnostics));
    ready = 1U;
    __set_PRIMASK(irq);
}

void UltrasonicBoard_OnCanRx(CAN_HandleTypeDef* hcan)
{
    if (hcan != &hcan1) return;
    while (HAL_CAN_GetRxFifoFillLevel(hcan, CAN_RX_FIFO0))
    {
        CAN_RxHeaderTypeDef header;
        uint8_t data[8];
        if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, data) != HAL_OK) break;
        uint32_t irq = __get_PRIMASK();
        __disable_irq();
        if (ready)
        {
            uint32_t now = HAL_GetTick();
            uint8_t was_active = UltrasonicAlarm_Update(&alarm, now);
            diagnostics.received++;
            diagnostics.last_id = header.IDE == CAN_ID_EXT ? header.ExtId : header.StdId;
            diagnostics.last_frame_tick = now;
            diagnostics.last_dlc = (uint8_t)header.DLC;
            diagnostics.last_extended = header.IDE == CAN_ID_EXT;
            diagnostics.last_remote = header.RTR == CAN_RTR_REMOTE;
            memset(diagnostics.last_data, 0, 8);
            if (header.RTR != CAN_RTR_REMOTE && header.DLC <= 8U)
                memcpy(diagnostics.last_data, data, header.DLC);
            if (UltrasonicAlarm_OnFrame(&alarm, now, header.ExtId,
                header.IDE == CAN_ID_EXT, header.RTR == CAN_RTR_REMOTE, header.DLC, data))
            {
                diagnostics.accepted++;
                diagnostics.last_valid_tick = now;
                memcpy(diagnostics.last_valid_data, data, 8);
                if (!was_active && alarm.active) stop_pending = 1U;
            }
        }
        __set_PRIMASK(irq);
    }
}

uint8_t UltrasonicBoard_TakeTrigger(void)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    uint8_t pending = stop_pending;
    stop_pending = 0U;
    __set_PRIMASK(irq);
    return pending;
}

uint8_t UltrasonicBoard_MotionBlocked(uint32_t now)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    uint8_t blocked = ready && (UltrasonicAlarm_Update(&alarm, now) || stop_pending);
    __set_PRIMASK(irq);
    return blocked;
}

uint8_t UltrasonicBoard_IsAlarm(uint32_t now)
{
    uint32_t irq = __get_PRIMASK();
    uint8_t active;
    __disable_irq();
    active = ready ? UltrasonicAlarm_Update(&alarm, now) : 0U;
    __set_PRIMASK(irq);
    return active;
}

void UltrasonicBoard_CopyDiagnostics(UltrasonicDiagnostics* out)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    *out = diagnostics;
    out->seen = alarm.seen;
    out->mask = alarm.obstacle_mask;
    out->active = alarm.active;
    __set_PRIMASK(irq);
}

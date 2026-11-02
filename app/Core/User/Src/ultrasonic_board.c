#include "ultrasonic_board.h"
#include "main.h"
#include "ultrasonic_alarm.h"
#include <string.h>

static UltrasonicAlarm alarm;
static volatile uint8_t ready;
static volatile uint8_t stop_pending;
static volatile uint8_t alarm_suppressed;
static volatile uint8_t prestart_warning_active;
static volatile uint32_t prestart_warning_started;
static volatile uint8_t alarm_held;
static UltrasonicDiagnostics diagnostics;

static uint8_t prestart_warning_update(uint32_t now)
{
    if (prestart_warning_active &&
        (uint32_t)(now - prestart_warning_started) >= ULTRASONIC_PRESTART_WARNING_MS)
        prestart_warning_active = 0U;
    return prestart_warning_active;
}

void UltrasonicBoard_Init(void)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    ready = 0U;
    stop_pending = 0U;
    alarm_suppressed = 0U;
    prestart_warning_active = 0U;
    prestart_warning_started = 0U;
    alarm_held = 0U;
    UltrasonicAlarm_Init(&alarm);
    memset(&diagnostics, 0, sizeof(diagnostics));
    ready = 1U;
    __set_PRIMASK(irq);
}

void UltrasonicBoard_SetAlarmSuppressed(uint8_t suppressed)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    alarm_suppressed = suppressed ? 1U : 0U;
    if (alarm_suppressed) stop_pending = 0U;
    __set_PRIMASK(irq);
}

void UltrasonicBoard_ClearAlarmHold(void)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    alarm_held = 0U;
    __set_PRIMASK(irq);
}

void UltrasonicBoard_StartPrestartWarning(uint32_t now)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    if (ready && (UltrasonicAlarm_Update(&alarm, now) || stop_pending) &&
        !prestart_warning_update(now))
    {
        /* Prestart warning follows the shared obstacle hold. Continuous
         * obstacle frames keep it active; clearing starts the normal 2 s tail. */
        alarm_held = 1U;
        prestart_warning_started = now;
        prestart_warning_active = 1U;
    }
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
                if (alarm.active && !alarm_suppressed) alarm_held = 1U;
                if (!was_active && alarm.active && !alarm_suppressed) stop_pending = 1U;
            }
        }
        __set_PRIMASK(irq);
    }
}

uint8_t UltrasonicBoard_TakeTrigger(void)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    uint8_t pending = alarm_suppressed ? 0U : stop_pending;
    stop_pending = 0U;
    __set_PRIMASK(irq);
    return pending;
}

uint8_t UltrasonicBoard_MotionBlocked(uint32_t now)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    uint8_t blocked = ready && !alarm_suppressed &&
        (UltrasonicAlarm_Update(&alarm, now) || stop_pending);
    __set_PRIMASK(irq);
    return blocked;
}

uint8_t UltrasonicBoard_ObstacleActive(uint32_t now)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    uint8_t active = ready &&
        (UltrasonicAlarm_Update(&alarm, now) || stop_pending);
    __set_PRIMASK(irq);
    return active;
}

uint8_t UltrasonicBoard_IsAlarm(uint32_t now)
{
    uint32_t irq = __get_PRIMASK();
    uint8_t active, prestart;
    __disable_irq();
    active = ready ? UltrasonicAlarm_Update(&alarm, now) : 0U;
    if (!active) alarm_held = 0U;
    else if (!alarm_suppressed) alarm_held = 1U;
    prestart = prestart_warning_update(now);
    __set_PRIMASK(irq);
    return (active && (!alarm_suppressed || alarm_held)) || prestart;
}

void UltrasonicBoard_CopyDiagnostics(UltrasonicDiagnostics* out)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    *out = diagnostics;
    out->seen = alarm.seen;
    out->mask = alarm.obstacle_mask;
    out->active = alarm.active;
    out->suppressed = alarm_suppressed;
    out->prestart_warning = prestart_warning_active;
    out->alarm_held = alarm_held;
    __set_PRIMASK(irq);
}

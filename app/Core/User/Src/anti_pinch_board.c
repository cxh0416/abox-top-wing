#include "anti_pinch_board.h"

#include "anti_pinch_alarm.h"
#include "main.h"
#include "ultrasonic_board.h"

static AntiPinchAlarm alarm;
static volatile uint8_t ready;
static volatile uint8_t motion_pending, contact_pressed, scope_enabled;
static uint8_t output_active;

void AntiPinchBoard_Init(void)
{
    ready = 0U;
    motion_pending = contact_pressed = 0U;
    scope_enabled = 0U;
    output_active = 0U;
    /* MX_GPIO_Init owns pin configuration. PA0 enables the board's PVIN
     * high-side switch; it does not supply 12 V directly from the MCU. */
    HAL_GPIO_WritePin(ANTI_PINCH_OUTPUT_GPIO_Port, ANTI_PINCH_OUTPUT_Pin, GPIO_PIN_RESET);
    AntiPinchAlarm_Init(&alarm);
    ready = 1U;
}

void AntiPinchBoard_SetScopeEnabled(uint8_t enabled)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    scope_enabled = enabled ? 1U : 0U;
    __set_PRIMASK(irq);
}

void AntiPinchBoard_Tick(uint32_t now)
{
    uint8_t pressed, was_pressed, alarm_active;
    if (!ready) return;
    pressed = HAL_GPIO_ReadPin(ANTI_PINCH_CONTACT_GPIO_Port, ANTI_PINCH_CONTACT_Pin)
              == GPIO_PIN_SET;
    was_pressed = alarm.stable_pressed;
    alarm_active = AntiPinchAlarm_Update(&alarm, now, pressed && scope_enabled);
    contact_pressed = alarm.stable_pressed;
    if (!was_pressed && contact_pressed) motion_pending = 1U;
    /* One writer owns PA0. Either source holds the shared power output on. */
    alarm_active = (uint8_t)(alarm_active | UltrasonicBoard_IsAlarm(now));
    if (alarm_active != output_active)
    {
        output_active = alarm_active;
        HAL_GPIO_WritePin(ANTI_PINCH_OUTPUT_GPIO_Port, ANTI_PINCH_OUTPUT_Pin,
                          alarm_active ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

uint8_t AntiPinchBoard_TakeTrigger(void)
{
    uint32_t irq = __get_PRIMASK();
    uint8_t pending;
    __disable_irq();
    pending = motion_pending;
    motion_pending = 0U;
    __set_PRIMASK(irq);
    return pending;
}

uint8_t AntiPinchBoard_IsPressed(void) { return contact_pressed; }

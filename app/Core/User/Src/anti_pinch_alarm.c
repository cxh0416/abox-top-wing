#include "anti_pinch_alarm.h"

#include <string.h>

void AntiPinchAlarm_Init(AntiPinchAlarm* alarm)
{
    memset(alarm, 0, sizeof(*alarm));
}

uint8_t AntiPinchAlarm_Update(AntiPinchAlarm* alarm, uint32_t now, uint8_t pressed)
{
    pressed = pressed ? 1U : 0U;
    if (pressed != alarm->raw_pressed)
    {
        alarm->raw_pressed = pressed;
        alarm->raw_since = now;
    }
    if (alarm->stable_pressed != alarm->raw_pressed &&
        (uint32_t)(now - alarm->raw_since) >= ANTI_PINCH_DEBOUNCE_MS)
    {
        alarm->stable_pressed = alarm->raw_pressed;
        if (!alarm->stable_pressed) alarm->released_at = now;
    }
    if (alarm->stable_pressed) alarm->alarm_active = 1U;
    else if (alarm->alarm_active && (uint32_t)(now - alarm->released_at) >= ANTI_PINCH_ALARM_TAIL_MS)
        alarm->alarm_active = 0U;
    return alarm->alarm_active;
}

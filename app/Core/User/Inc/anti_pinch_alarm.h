#ifndef ANTI_PINCH_ALARM_H
#define ANTI_PINCH_ALARM_H

#include <stdint.h>

#define ANTI_PINCH_DEBOUNCE_MS 20U
#define ANTI_PINCH_ALARM_TAIL_MS 1000U

typedef struct
{
    uint32_t raw_since;
    uint32_t released_at;
    uint8_t raw_pressed;
    uint8_t stable_pressed;
    uint8_t alarm_active;
} AntiPinchAlarm;

void AntiPinchAlarm_Init(AntiPinchAlarm* alarm);
/* Alarm follows debounced contact, then remains on for 1000 ms from the
 * physical release edge. stable_pressed is independent of the alarm tail. */
uint8_t AntiPinchAlarm_Update(AntiPinchAlarm* alarm, uint32_t now, uint8_t pressed);

#endif

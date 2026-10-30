#ifndef ULTRASONIC_ALARM_H
#define ULTRASONIC_ALARM_H
#include <stdint.h>
#ifndef WL300D_CAN_ID
#define WL300D_CAN_ID 0x18EC0057U
#endif
#define ULTRASONIC_CLEAR_MS 1000U
#define ULTRASONIC_FRESH_MS 500U
#define ULTRASONIC_SILENCE_MS 1000U
typedef struct {
    uint32_t last_rx, clear_since;
    uint8_t seen, obstacle_mask, active, clearing;
} UltrasonicAlarm;
void UltrasonicAlarm_Init(UltrasonicAlarm* state);
uint8_t UltrasonicAlarm_OnFrame(UltrasonicAlarm* state, uint32_t now,
    uint32_t id, uint8_t extended, uint8_t remote, uint8_t length, const uint8_t* data);
uint8_t UltrasonicAlarm_Update(UltrasonicAlarm* state, uint32_t now);
#endif

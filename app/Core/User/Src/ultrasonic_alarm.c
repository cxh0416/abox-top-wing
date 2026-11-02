#include "ultrasonic_alarm.h"
#include <string.h>

void UltrasonicAlarm_Init(UltrasonicAlarm* state) { memset(state, 0, sizeof(*state)); }

uint8_t UltrasonicAlarm_Update(UltrasonicAlarm* state, uint32_t now)
{
    if (!state->seen) return 0U;
    /* User-selected policy: clear frames or silence keep the tail for 2 s.
     * This does not establish that the module is online or the path is clear. */
    if (state->active &&
        ((uint32_t)(now - state->last_rx) >= ULTRASONIC_SILENCE_MS ||
         (state->clearing && (uint32_t)(now - state->clear_since) >= ULTRASONIC_CLEAR_MS)))
    {
        state->active = 0U;
        state->clearing = 0U;
    }
    return state->active;
}

uint8_t UltrasonicAlarm_OnFrame(UltrasonicAlarm* state, uint32_t now,
    uint32_t id, uint8_t extended, uint8_t remote, uint8_t length, const uint8_t* data)
{
    uint8_t mask = 0U;
    if (id != WL300D_CAN_ID || !extended || remote || length != 8U || !data) return 0U;
    (void)UltrasonicAlarm_Update(state, now);
    /* The installed module reports no valid target as either 0x00 or 0xFF.
     * Targets in its blind zone are intentionally treated as no obstacle. */
    for (uint8_t i = 0U; i < 8U; ++i)
        if (data[i] != 0x00U && data[i] != 0xFFU)
            mask |= (uint8_t)(1U << i);
    state->seen = 1U;
    state->last_rx = now;
    state->obstacle_mask = mask;
    if (mask)
    {
        state->active = 1U;
        state->clearing = 0U;
    }
    else if (state->active && !state->clearing)
    {
        state->clear_since = now;
        state->clearing = 1U;
    }
    return 1U;
}

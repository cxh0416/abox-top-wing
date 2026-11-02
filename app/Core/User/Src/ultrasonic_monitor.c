#include "ultrasonic_board.h"
#include "ultrasonic_alarm.h"
#include "app_log.h"

void UltrasonicMonitor_Task(uint32_t now)
{
    static uint32_t last_print;
    static uint8_t logged, last_mask, last_active, last_fresh;
    static uint8_t last_suppressed, last_prestart, last_held;
    UltrasonicDiagnostics d;
    UltrasonicBoard_CopyDiagnostics(&d);
    uint8_t fresh = d.seen && (uint32_t)(now - d.last_valid_tick) <= ULTRASONIC_FRESH_MS;
    if (logged && d.mask == last_mask && d.active == last_active &&
        d.suppressed == last_suppressed && fresh == last_fresh &&
        d.prestart_warning == last_prestart && d.alarm_held == last_held &&
        (uint32_t)(now - last_print) < 5000U) return;
    logged = 1U;
    last_print = now;
    last_mask = d.mask;
    last_active = d.active;
    last_fresh = fresh;
    last_suppressed = d.suppressed;
    last_prestart = d.prestart_warning;
    last_held = d.alarm_held;
    APP_LOGI("SONAR", "rx=%lu valid=%lu id=%08lX dlc=%u ext=%u rtr=%u",
        (unsigned long)d.received, (unsigned long)d.accepted, (unsigned long)d.last_id,
        d.last_dlc, d.last_extended, d.last_remote);
    APP_LOGI("SONAR", "seen=%u fresh=%u age=%lu mask=%02X raw_alarm=%u suppressed=%u "
             "prestart=%u held=%u output=%u",
        d.seen, fresh, d.seen ? (unsigned long)(now - d.last_valid_tick) : 0UL,
        d.mask, d.active, d.suppressed, d.prestart_warning, d.alarm_held,
        (uint8_t)((d.active && (!d.suppressed || d.alarm_held)) || d.prestart_warning));
    if (d.accepted)
        APP_LOGI("SONAR", "last=%02X %02X %02X %02X %02X %02X %02X %02X",
            d.last_valid_data[0], d.last_valid_data[1], d.last_valid_data[2], d.last_valid_data[3],
            d.last_valid_data[4], d.last_valid_data[5], d.last_valid_data[6], d.last_valid_data[7]);
}

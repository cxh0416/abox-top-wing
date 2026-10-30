#include "time_base.h"

#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "main.h"
#include "abox_platform_port.h"
#include "abox_mqtt_v4.h"

static ABoxClock g_clock;
static uint8_t g_cclk_mode_known, g_cclk_local;
static uint64_t g_log_mapping_start;

#define TIMEBASE_BEIJING_OFFSET_SEC (8U * 3600U)

static uint8_t is_leap_year(int y)
{
    return (((y % 4) == 0 && (y % 100) != 0) || ((y % 400) == 0)) ? 1U : 0U;
}

static uint8_t month_days(int y, int mon)
{
    static const uint8_t days[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    if ((mon == 2) && is_leap_year(y)) return 29U;
    if ((mon < 1) || (mon > 12)) return 31U;
    return days[mon - 1];
}

void TimeBase_Init(void)
{
    ABoxClock_Init(&g_clock);
    g_cclk_mode_known = g_cclk_local = 0U;
    g_log_mapping_start = 0U;
}

/*
 * 解析：
 * +CCLK: "25/03/25,11:20:30+32"
 */
void TimeBase_UpdateFromCCLK(const char* line)
{
    uint64_t utc, mono;
    int mode;
    if (!line) return;
    if (!strcmp(line, "RDY")) { g_cclk_mode_known = 0U; return; }
    if (sscanf(line, "+CTZU: %d", &mode) == 1) {
        g_cclk_mode_known = (mode == 1 || mode == 3);
        g_cclk_local = mode == 3;
        return;
    }
    if (!strstr(line, "+CCLK:")) return;
    if (!g_cclk_mode_known || !ABoxClock_ParseCCLK(line, g_cclk_local, &utc)) {
        APP_LOGW("TIME", "CCLK unknown mode or invalid");
        return;
    }
    mono = ABox_PortGetMonotonicMs();
    if (ABoxClock_Bootstrap(&g_clock, utc, mono)) g_log_mapping_start = mono;
}

uint64_t TimeBase_GetMs(void)
{
    return TimeBase_GetSnapshot().utc_ms;
}

uint64_t TimeBase_GetReportedMs(void)
{
    ABoxClockSnapshot snapshot = TimeBase_GetSnapshot();
    return ABoxClock_ReportedUtcMs(&snapshot);
}

ABoxClockSnapshot TimeBase_GetSnapshot(void)
{ return ABoxClock_Snapshot(&g_clock, ABox_PortGetMonotonicMs()); }
uint16_t TimeBase_CheckExpiry(uint64_t expires)
{
    ABoxClockSnapshot snapshot = TimeBase_GetSnapshot();
    return ABoxMqttV4_CheckExpiry(&snapshot, expires);
}
int TimeBase_Synchronize(uint64_t server, uint32_t error, uint64_t sent, uint64_t received)
{
    int ok = ABoxClock_Synchronize(&g_clock, server, error, sent, received);
    if (ok) g_log_mapping_start = ABox_PortGetMonotonicMs();
    return ok;
}

uint8_t TimeBase_FormatUtcMs(char* out, uint16_t out_sz)
{
    if (!out || !out_sz) return 0U;
    out[0] = '\0';
    return (uint8_t)ABoxMqttV4_FormatInteger(out, out_sz, TimeBase_GetReportedMs());
}

uint8_t TimeBase_FormatBeijing(char* out, uint16_t out_sz)
{
    uint32_t sec;
    uint32_t days_total;
    uint32_t day_sec;
    int year = 1970;
    int mon = 1;
    int day;
    int hour;
    int min;
    int second;

    if ((out == 0) || (out_sz == 0U)) return 0U;
    out[0] = '\0';
    if (!TimeBase_IsValid()) return 0U;

    sec = (uint32_t)(TimeBase_GetMs() / 1000ULL);
    sec += TIMEBASE_BEIJING_OFFSET_SEC;
    days_total = sec / 86400U;
    day_sec = sec % 86400U;

    while (days_total >= (is_leap_year(year) ? 366U : 365U))
    {
        days_total -= is_leap_year(year) ? 366U : 365U;
        year++;
    }

    while (days_total >= month_days(year, mon))
    {
        days_total -= month_days(year, mon);
        mon++;
    }

    day = (int)days_total + 1;
    hour = (int)(day_sec / 3600U);
    min = (int)((day_sec % 3600U) / 60U);
    second = (int)(day_sec % 60U);

    snprintf(out, out_sz, "%04d-%02d-%02d %02d:%02d:%02d", year, mon, day, hour, min, second);
    return 1U;
}

uint8_t TimeBase_FormatBeijingFromTick(uint32_t tick, char* out, uint16_t out_sz)
{
    int64_t ms;
    ABoxClockSnapshot snapshot = TimeBase_GetSnapshot();
    uint32_t age = (uint32_t)snapshot.mono_ms - tick;
    uint32_t sec;
    uint32_t days_total;
    uint32_t day_sec;
    int year = 1970;
    int mon = 1;
    int day;
    int hour;
    int min;
    int second;

    if ((out == 0) || (out_sz == 0U)) return 0U;
    out[0] = '\0';
    if (!TimeBase_IsValid() || age > INT32_MAX ||
        snapshot.mono_ms < age || snapshot.mono_ms - age < g_log_mapping_start) return 0U;

    ms = (int64_t)ABoxClock_Snapshot(&g_clock, snapshot.mono_ms - age).utc_ms;
    if (ms < 0) return 0U;

    sec = (uint32_t)(ms / 1000LL);
    sec += TIMEBASE_BEIJING_OFFSET_SEC;
    days_total = sec / 86400U;
    day_sec = sec % 86400U;

    while (days_total >= (is_leap_year(year) ? 366U : 365U))
    {
        days_total -= is_leap_year(year) ? 366U : 365U;
        year++;
    }

    while (days_total >= month_days(year, mon))
    {
        days_total -= month_days(year, mon);
        mon++;
    }

    day = (int)days_total + 1;
    hour = (int)(day_sec / 3600U);
    min = (int)((day_sec % 3600U) / 60U);
    second = (int)(day_sec % 60U);

    snprintf(out, out_sz, "%04d-%02d-%02d %02d:%02d:%02d", year, mon, day, hour, min, second);
    return 1U;
}

uint8_t TimeBase_IsValid(void) { return g_clock.source != ABOX_CLOCK_NONE; }

uint8_t TimeBase_IsTrusted(void) { return TimeBase_GetSnapshot().quality == ABOX_CLOCK_SYNCHRONIZED; }

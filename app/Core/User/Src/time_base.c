#include "time_base.h"

#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "main.h"

static uint32_t g_unix_sec = 0;   // 秒级时间（来自4G）
static uint32_t g_sync_tick = 0;  // 同步时刻 tick
static uint8_t g_valid = 0;

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

/* ===== 简单日期转 unix ===== */
/* 不考虑时区，直接按 UTC 处理 */
static uint32_t ymdhms_to_unix(int y, int mon, int d, int h, int m, int s)
{
    /* 简化版（够用） */
    uint32_t days_total = 0;
    int i;

    for (i = 1970; i < y; i++) { days_total += is_leap_year(i) ? 366U : 365U; }

    for (i = 1; i < mon; i++)
    {
        days_total += month_days(y, i);
    }

    days_total += (d - 1);

    return days_total * 86400 + h * 3600 + m * 60 + s;
}

void TimeBase_Init(void)
{
    g_unix_sec = 0U;
    g_sync_tick = 0U;
    g_valid = 0U;
}

/*
 * 解析：
 * +CCLK: "25/03/25,11:20:30+32"
 */
void TimeBase_UpdateFromCCLK(const char* line)
{
    int yy, MM, dd, hh, mm, ss;

    if (!line) return;

    if (strstr(line, "+CCLK:") == 0) return;

    if (sscanf(line, "+CCLK: \"%2d/%2d/%2d,%2d:%2d:%2d", &yy, &MM, &dd, &hh, &mm, &ss) == 6)
    {
        int year = 2000 + yy;

        if ((year < 2024) || (MM < 1) || (MM > 12) || (dd < 1) || (dd > month_days(year, MM)) || (hh < 0) ||
            (hh > 23) || (mm < 0) || (mm > 59) || (ss < 0) || (ss > 59))
        {
            APP_LOGW("TIME", "invalid CCLK ignored");
            return;
        }

        g_unix_sec = ymdhms_to_unix(year, MM, dd, hh, mm, ss);
        g_sync_tick = HAL_GetTick();
        g_valid = 1U;
    }
}

uint64_t TimeBase_GetMs(void)
{
    if (!g_valid)
    {
        return 0ULL;
    }

    uint32_t now = HAL_GetTick();
    uint32_t delta = now - g_sync_tick;

    return ((uint64_t)g_unix_sec * 1000ULL) + delta;
}

uint8_t TimeBase_FormatUtcMs(char* out, uint16_t out_sz)
{
    char reversed[20];
    uint64_t value;
    uint16_t digits = 0U;
    uint16_t i;

    if ((out == 0) || (out_sz == 0U)) return 0U;
    out[0] = '\0';
    if (!g_valid) return 0U;

    value = TimeBase_GetMs();
    do
    {
        reversed[digits++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    } while ((value != 0ULL) && (digits < (uint16_t)sizeof(reversed)));

    if ((value != 0ULL) || ((uint16_t)(digits + 1U) > out_sz)) return 0U;

    for (i = 0U; i < digits; i++) { out[i] = reversed[digits - i - 1U]; }
    out[digits] = '\0';
    return 1U;
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
    if (!g_valid) return 0U;

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
    if (!g_valid) return 0U;

    ms = ((int64_t)g_unix_sec * 1000LL) + (int32_t)(tick - g_sync_tick);
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

uint8_t TimeBase_IsValid(void) { return g_valid; }

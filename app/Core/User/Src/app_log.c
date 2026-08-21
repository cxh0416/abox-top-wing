#include "app_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lockctrl_diag.h"
#include "main.h"
#include "time_base.h"
#if LOCKCTRL_EC800_AT_DIAG_UART5
#include "usart.h"
#endif

#define APP_LOG_LINE_COUNT 32U
#define APP_LOG_LINE_SIZE 80U

static char g_log_lines[APP_LOG_LINE_COUNT][APP_LOG_LINE_SIZE];
static uint32_t g_log_ticks[APP_LOG_LINE_COUNT];
static uint16_t g_log_wr = 0U;
static uint16_t g_log_count = 0U;
static uint16_t g_log_mqtt_rd = 0U;
#if LOCKCTRL_EC800_AT_DIAG_UART5
static uint16_t g_log_debug_rd = 0U;
#endif
static AppLogLevel_t g_log_level = APP_LOG_LEVEL_INFO;

static const char* level_name(AppLogLevel_t level)
{
    switch (level)
    {
        case APP_LOG_LEVEL_ERROR: return "E";
        case APP_LOG_LEVEL_WARN: return "W";
        case APP_LOG_LEVEL_INFO: return "I";
        case APP_LOG_LEVEL_DEBUG:
        default: return "D";
    }
}

static uint8_t AppLog_IsDumpNoise(const char* line)
{
    if (line == 0) return 1U;
    if (strstr(line, " AT TX owner=") != 0) return 1U;
    if (strstr(line, " AT RX owner=") != 0) return 1U;
    if (strstr(line, " AT PAY owner=") != 0) return 1U;
    if (strstr(line, " MQTT heartbeat publish busy") != 0) return 1U;
    if (strstr(line, " MQTT payload owner mismatch") != 0) return 1U;
    return 0U;
}

static uint16_t AppLog_FormatLine(uint16_t idx, char* out, uint16_t out_sz)
{
    char ts[24];
    uint32_t tick;
    int n;

    if ((out == 0) || (out_sz == 0U)) return 0U;
    out[0] = '\0';
    idx = (uint16_t)(idx % APP_LOG_LINE_COUNT);
    tick = g_log_ticks[idx];

    if (!TimeBase_FormatBeijingFromTick(tick, ts, sizeof(ts)))
    {
        snprintf(ts, sizeof(ts), "BOOT+%lu.%03lus", (unsigned long)(tick / 1000U), (unsigned long)(tick % 1000U));
    }

    n = snprintf(out, out_sz, "[%s] %s", ts, g_log_lines[idx]);
    if (n < 0) return 0U;
    if (n >= out_sz) return (uint16_t)(out_sz - 1U);
    return (uint16_t)n;
}

void AppLog_Init(void)
{
    AppLog_Clear();
    g_log_level = APP_LOG_LEVEL_INFO;
}

void AppLog_Clear(void)
{
    memset(g_log_lines, 0, sizeof(g_log_lines));
    memset(g_log_ticks, 0, sizeof(g_log_ticks));
    g_log_wr = 0U;
    g_log_count = 0U;
    g_log_mqtt_rd = 0U;
#if LOCKCTRL_EC800_AT_DIAG_UART5
    g_log_debug_rd = 0U;
#endif
}

void AppLog_SetLevel(AppLogLevel_t level) { g_log_level = level; }
AppLogLevel_t AppLog_GetLevel(void) { return g_log_level; }

void AppLog_Printf(AppLogLevel_t level, const char* tag, const char* fmt, ...)
{
    char msg[64];
    va_list ap;
    int n;
    uint16_t idx;

    if ((fmt == 0) || (level > g_log_level)) return;

    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    idx = g_log_wr;
    g_log_ticks[idx] = HAL_GetTick();
    snprintf(g_log_lines[idx], APP_LOG_LINE_SIZE, "%s %s %s\r\n", level_name(level), tag ? tag : "APP", msg);
    g_log_wr = (uint16_t)((g_log_wr + 1U) % APP_LOG_LINE_COUNT);
    if (g_log_count < APP_LOG_LINE_COUNT) g_log_count++;
}

uint16_t AppLog_Dump(char* out, uint16_t out_sz)
{
    uint16_t i;
    uint16_t used = 0U;
    uint16_t start;
    uint16_t first = 0U;
    uint16_t lens[APP_LOG_LINE_COUNT];

    if ((out == 0) || (out_sz == 0U)) return 0U;
    out[0] = '\0';
    start = (g_log_wr + APP_LOG_LINE_COUNT - g_log_count) % APP_LOG_LINE_COUNT;

    for (i = 0U; i < g_log_count; i++)
    {
        char line[128];
        lens[i] = AppLog_FormatLine((uint16_t)((start + i) % APP_LOG_LINE_COUNT), line, sizeof(line));
        if (AppLog_IsDumpNoise(line)) lens[i] = 0U;
    }

    first = g_log_count;
    for (i = g_log_count; i > 0U; i--)
    {
        uint16_t n = lens[(uint16_t)(i - 1U)];
        if (n == 0U) continue;
        if ((used + n + 1U) >= out_sz) break;
        used = (uint16_t)(used + n);
        first = (uint16_t)(i - 1U);
    }

    used = 0U;
    for (i = first; i < g_log_count; i++)
    {
        char line[128];
        uint16_t n = lens[i];
        if (n == 0U) continue;

        if ((used + n + 1U) >= out_sz) break;
        (void)AppLog_FormatLine((uint16_t)((start + i) % APP_LOG_LINE_COUNT), line, sizeof(line));
        memcpy(out + used, line, n);
        used += n;
        out[used] = '\0';
    }
    return used;
}

uint16_t AppLog_ReadMqttLine(char* out, uint16_t out_sz)
{
    uint16_t oldest;
    uint16_t newest;
    uint16_t n;

    if ((out == 0) || (out_sz == 0U) || (g_log_count == 0U)) return 0U;

    oldest = (g_log_wr + APP_LOG_LINE_COUNT - g_log_count) % APP_LOG_LINE_COUNT;
    newest = g_log_wr;
    if (g_log_count == APP_LOG_LINE_COUNT)
    {
        uint16_t dist = (g_log_mqtt_rd + APP_LOG_LINE_COUNT - oldest) % APP_LOG_LINE_COUNT;
        if (dist >= g_log_count) g_log_mqtt_rd = oldest;
    }
    if (g_log_mqtt_rd == newest) return 0U;

    do
    {
        n = AppLog_FormatLine(g_log_mqtt_rd, out, out_sz);
        g_log_mqtt_rd = (uint16_t)((g_log_mqtt_rd + 1U) % APP_LOG_LINE_COUNT);
        if (!AppLog_IsDumpNoise(out)) return n;
    } while (g_log_mqtt_rd != newest);

    out[0] = '\0';
    return 0U;
}

#if LOCKCTRL_EC800_AT_DIAG_UART5
void AppLog_DebugTask(void)
{
    char line[128];
    uint16_t n = 0U;
    uint16_t oldest;
    uint16_t newest;

    /* Format in the main loop; AppLog_Printf may be called from UART IRQ. */
    __disable_irq();
    if (g_log_count > 0U)
    {
        oldest = (g_log_wr + APP_LOG_LINE_COUNT - g_log_count) % APP_LOG_LINE_COUNT;
        newest = g_log_wr;
        if (g_log_count == APP_LOG_LINE_COUNT)
        {
            uint16_t dist = (g_log_debug_rd + APP_LOG_LINE_COUNT - oldest) % APP_LOG_LINE_COUNT;
            if (dist >= g_log_count) g_log_debug_rd = oldest;
        }

        if (g_log_debug_rd != newest)
        {
            n = AppLog_FormatLine(g_log_debug_rd, line, sizeof(line));
            g_log_debug_rd = (uint16_t)((g_log_debug_rd + 1U) % APP_LOG_LINE_COUNT);
        }
    }
    __enable_irq();

    if (n > 0U) { (void)HAL_UART_Transmit(&huart5, (uint8_t*)line, n, 1000U); }
}
#endif

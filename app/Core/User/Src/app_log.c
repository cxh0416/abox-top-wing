#include "app_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_diag.h"
#include "main.h"
#include "time_base.h"
#if LOCKCTRL_EC800_AT_DIAG_UART5
#include "usart.h"
#endif

#define APP_LOG_LINE_COUNT 64U
#define APP_LOG_LINE_SIZE 112U

static char g_log_lines[APP_LOG_LINE_COUNT][APP_LOG_LINE_SIZE];
static uint32_t g_log_ticks[APP_LOG_LINE_COUNT];
static uint32_t g_log_sequences[APP_LOG_LINE_COUNT];
static uint16_t g_log_wr = 0U;
static uint16_t g_log_count = 0U;
static uint32_t g_log_legacy_sequence = 1U;
static uint32_t g_log_next_sequence = 1U;
static uint32_t g_log_mqtt_sequence = 1U;
static uint32_t g_log_mqtt_peek_end = 1U;
static uint32_t g_log_mqtt_dropped = 0U;
static uint8_t g_log_mqtt_peek_valid = 0U;
#if LOCKCTRL_EC800_AT_DIAG_UART5
static uint32_t g_log_debug_sequence = 1U;
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

/* Copy one committed slot while interrupts are masked, then format outside the
 * critical section. Sequence arithmetic is unsigned and remains valid across
 * wrap as long as readers are not more than 2^32 records behind. */
static uint8_t AppLog_CopySequence(uint32_t sequence, char* line, uint32_t* tick)
{
    uint32_t irq = __get_PRIMASK();
    uint32_t distance;
    uint16_t idx;
    uint8_t found = 0U;
    __disable_irq();
    distance = g_log_next_sequence - sequence;
    if (distance > 0U && distance <= g_log_count)
    {
        idx = (uint16_t)((g_log_wr + APP_LOG_LINE_COUNT - distance) % APP_LOG_LINE_COUNT);
        if (g_log_sequences[idx] == sequence)
        {
            *tick = g_log_ticks[idx];
            memcpy(line, g_log_lines[idx], APP_LOG_LINE_SIZE);
            found = 1U;
        }
    }
    __set_PRIMASK(irq);
    return found;
}

static uint16_t AppLog_FormatSequence(uint32_t sequence, char* out, uint16_t out_sz)
{
    char line[APP_LOG_LINE_SIZE];
    char ts[24];
    uint32_t tick;
    int n;
    if ((out == 0) || (out_sz == 0U)) return 0U;
    out[0] = '\0';
    if (!AppLog_CopySequence(sequence, line, &tick)) return 0U;
    if (!TimeBase_FormatBeijingFromTick(tick, ts, sizeof(ts)))
    {
        snprintf(ts, sizeof(ts), "BOOT+%lu.%03lus", (unsigned long)(tick / 1000U), (unsigned long)(tick % 1000U));
    }
    n = snprintf(out, out_sz, "[%s] %s", ts, line);
    if (n < 0) return 0U;
    if (n >= out_sz) return (uint16_t)(out_sz - 1U);
    return (uint16_t)n;
}

static void AppLog_Snapshot(uint32_t* next, uint16_t* count)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    *next = g_log_next_sequence;
    *count = g_log_count;
    __set_PRIMASK(irq);
}

void AppLog_Init(void)
{
    AppLog_Clear();
    g_log_level = APP_LOG_LEVEL_INFO;
}

void AppLog_Clear(void)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    /* Invalidate slots by count; no long buffer erase in the critical section.
     * Keep the sequence monotonic so an old snapshot cannot name a new slot. */
    g_log_wr = 0U;
    g_log_count = 0U;
    g_log_legacy_sequence = g_log_next_sequence;
    g_log_mqtt_sequence = g_log_next_sequence;
    g_log_mqtt_peek_end = g_log_next_sequence;
    g_log_mqtt_dropped = 0U;
    g_log_mqtt_peek_valid = 0U;
#if LOCKCTRL_EC800_AT_DIAG_UART5
    g_log_debug_sequence = g_log_next_sequence;
#endif
    __set_PRIMASK(irq);
}

void AppLog_SetLevel(AppLogLevel_t level) { g_log_level = level; }
AppLogLevel_t AppLog_GetLevel(void) { return g_log_level; }

void AppLog_Printf(AppLogLevel_t level, const char* tag, const char* fmt, ...)
{
    char msg[96];
    char line[APP_LOG_LINE_SIZE];
    va_list ap;
    int n;
    uint16_t idx;
    uint32_t tick;
    uint32_t irq;
    size_t line_size;

    if ((fmt == 0) || (level > g_log_level)) return;
    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    n = snprintf(line, sizeof(line), "%s %s %s\r\n", level_name(level), tag ? tag : "APP", msg);
    if (n < 0) return;
    line_size = (n >= (int)sizeof(line)) ? sizeof(line) : (size_t)n + 1U;
    tick = HAL_GetTick();

    irq = __get_PRIMASK();
    __disable_irq();
    idx = g_log_wr;
    memcpy(g_log_lines[idx], line, line_size);
    g_log_ticks[idx] = tick;
    g_log_sequences[idx] = g_log_next_sequence++;
    g_log_wr = (uint16_t)((idx + 1U) % APP_LOG_LINE_COUNT);
    if (g_log_count < APP_LOG_LINE_COUNT) g_log_count++;
    __set_PRIMASK(irq);
}

uint16_t AppLog_Dump(char* out, uint16_t out_sz)
{
    uint32_t next, first;
    uint16_t count, i, used = 0U;
    uint16_t lengths[APP_LOG_LINE_COUNT];
    if ((out == 0) || (out_sz == 0U)) return 0U;
    out[0] = '\0';
    AppLog_Snapshot(&next, &count);
    first = next - count;
    for (i = 0U; i < count; i++)
    {
        char line[128];
        lengths[i] = AppLog_FormatSequence(first + i, line, sizeof(line));
        if (AppLog_IsDumpNoise(line)) lengths[i] = 0U;
    }
    for (i = count; i > 0U; i--)
    {
        uint16_t n = lengths[i - 1U];
        if (n && (uint32_t)used + n + 1U >= out_sz) break;
        used = (uint16_t)(used + n);
    }
    used = 0U;
    for (; i < count; i++)
    {
        char line[128];
        uint16_t n;
        if (lengths[i] == 0U) continue;
        n = AppLog_FormatSequence(first + i, line, sizeof(line));
        if (n == 0U || (uint32_t)used + n + 1U >= out_sz) continue;
        memcpy(out + used, line, n);
        used = (uint16_t)(used + n);
        out[used] = '\0';
    }
    return used;
}

/* These read cursors are used by the main loop. Writers may run in an ISR. */
static uint32_t AppLog_ClampCursor(uint32_t cursor, uint32_t next, uint16_t count)
{
    return (next - cursor > count) ? next - count : cursor;
}

uint16_t AppLog_ReadMqttLine(char* out, uint16_t out_sz)
{
    uint32_t next;
    uint16_t count;
    if ((out == 0) || (out_sz == 0U)) return 0U;
    out[0] = '\0';
    AppLog_Snapshot(&next, &count);
    g_log_legacy_sequence = AppLog_ClampCursor(g_log_legacy_sequence, next, count);
    while (g_log_legacy_sequence != next)
    {
        uint16_t n = AppLog_FormatSequence(g_log_legacy_sequence++, out, out_sz);
        if (n && !AppLog_IsDumpNoise(out)) return n;
    }
    out[0] = '\0';
    return 0U;
}

uint16_t AppLog_GetMqttPendingCount(void)
{
    uint32_t next, pending;
    uint16_t count;
    AppLog_Snapshot(&next, &count);
    pending = next - g_log_mqtt_sequence;
    return pending > count ? count : (uint16_t)pending;
}

uint8_t AppLog_PeekMqttLines(char lines[][APP_LOG_MQTT_LINE_SIZE], uint8_t max_lines)
{
    uint32_t next, sequence, pending;
    uint16_t available;
    uint8_t count = 0U;
    if (!lines || max_lines == 0U) return 0U;
    if (max_lines > APP_LOG_MQTT_BATCH_MAX) max_lines = APP_LOG_MQTT_BATCH_MAX;
    AppLog_Snapshot(&next, &available);
    pending = next - g_log_mqtt_sequence;
    if (pending > available)
    {
        g_log_mqtt_dropped += pending - available;
        g_log_mqtt_sequence = next - available;
    }
    sequence = g_log_mqtt_sequence;
    if (g_log_mqtt_dropped)
    {
        snprintf(lines[count++], APP_LOG_MQTT_LINE_SIZE, "W LOG dropped=%lu\r\n",
                 (unsigned long)g_log_mqtt_dropped);
    }
    while (sequence != next && count < max_lines)
    {
        uint16_t n = AppLog_FormatSequence(sequence, lines[count], APP_LOG_MQTT_LINE_SIZE);
        /* If an ISR overwrote this slot, leave it for the next drop calculation. */
        if (n == 0U) break;
        sequence++;
        if (!AppLog_IsDumpNoise(lines[count])) count++;
    }
    g_log_mqtt_peek_end = sequence;
    g_log_mqtt_peek_valid = (count > 0U);
    if (count == 0U && g_log_mqtt_dropped == 0U) g_log_mqtt_sequence = sequence;
    return count;
}

void AppLog_CommitMqttLines(void)
{
    if (!g_log_mqtt_peek_valid) return;
    g_log_mqtt_sequence = g_log_mqtt_peek_end;
    g_log_mqtt_dropped = 0U;
    g_log_mqtt_peek_valid = 0U;
}

void AppLog_AbortMqttLines(void)
{
    g_log_mqtt_peek_end = g_log_mqtt_sequence;
    g_log_mqtt_peek_valid = 0U;
}

#if LOCKCTRL_EC800_AT_DIAG_UART5
void AppLog_DebugTask(void)
{
    char line[128];
    uint32_t next;
    uint16_t count, n = 0U;
    AppLog_Snapshot(&next, &count);
    g_log_debug_sequence = AppLog_ClampCursor(g_log_debug_sequence, next, count);
    if (g_log_debug_sequence != next)
        n = AppLog_FormatSequence(g_log_debug_sequence++, line, sizeof(line));
    if (n > 0U) { (void)HAL_UART_Transmit(&huart5, (uint8_t*)line, n, 1000U); }
}
#endif

#ifdef TOP_WING_HOST_TEST
/* Place an empty ring near wrap for the real public API regression test. */
void AppLog_TestSetSequence(uint32_t sequence)
{
    AppLog_Clear();
    g_log_next_sequence = sequence;
    g_log_legacy_sequence = sequence;
    g_log_mqtt_sequence = sequence;
    g_log_mqtt_peek_end = sequence;
#if LOCKCTRL_EC800_AT_DIAG_UART5
    g_log_debug_sequence = sequence;
#endif
}
#endif

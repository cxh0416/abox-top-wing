#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_log.h"

static uint32_t g_tick;
static int g_inject_log;
static int g_inject_format;
static uint32_t g_irq_mask;
static int g_pending_irq;
uint32_t AppLogTest_GetPrimask(void) { return g_irq_mask; }
void AppLogTest_DisableIrq(void) { g_irq_mask = 1U; }
void AppLogTest_SetPrimask(uint32_t value)
{
    g_irq_mask = value;
    if (!value && g_pending_irq) {
        g_pending_irq = 0;
        g_irq_mask = 1U;
        APP_LOGW("ISR", "deferred");
        assert(g_irq_mask == 1U);
        g_irq_mask = 0U;
    }
}
void AppLog_TestSetSequence(uint32_t sequence);

uint32_t HAL_GetTick(void)
{
    if (g_inject_log) {
        g_inject_log = 0;
        APP_LOGW("ISR", "nested");
    }
    return g_tick++;
}

uint8_t TimeBase_FormatBeijingFromTick(uint32_t tick, char* out, uint16_t out_sz)
{
    (void)tick;
    if (g_inject_format) {
        unsigned i, n = (unsigned)g_inject_format;
        g_inject_format = 0;
        for (i = 0U; i < n; ++i) APP_LOGW("ISR", "during-read=%u", i);
    }
    if (out != 0 && out_sz > 0U) out[0] = '\0';
    return 0U;
}

static void test_retry_and_commit(void)
{
    char first[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE] = {{0}};
    char retry[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE] = {{0}};
    uint8_t count;

    AppLog_Init();
    APP_LOGI("TEST", "one");
    APP_LOGW("TEST", "two");
    APP_LOGE("TEST", "three");
    assert(AppLog_GetMqttPendingCount() == 3U);

    count = AppLog_PeekMqttLines(first, APP_LOG_MQTT_BATCH_MAX);
    assert(count == 3U);
    AppLog_AbortMqttLines();
    assert(AppLog_PeekMqttLines(retry, APP_LOG_MQTT_BATCH_MAX) == count);
    assert(memcmp(first, retry, sizeof(first[0]) * count) == 0);
    AppLog_CommitMqttLines();
    assert(AppLog_GetMqttPendingCount() == 0U);
}

static void test_overwrite_notice(void)
{
    char lines[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE];
    uint8_t count;
    unsigned i;

    AppLog_Init();
    for (i = 0U; i < 70U; i++) APP_LOGI("TEST", "item=%u", i);
    assert(AppLog_GetMqttPendingCount() == 64U);
    count = AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX);
    assert(count == APP_LOG_MQTT_BATCH_MAX);
    assert(strstr(lines[0], "LOG dropped=6") != 0);
    AppLog_CommitMqttLines();

    while (AppLog_GetMqttPendingCount() > 0U)
    {
        count = AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX);
        assert(count > 0U);
        AppLog_CommitMqttLines();
    }
}

static void test_noise_only_is_consumed(void)
{
    char lines[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE];

    AppLog_Init();
    APP_LOGI("AT", "TX owner=test");
    assert(AppLog_GetMqttPendingCount() == 1U);
    assert(AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX) == 0U);
    assert(AppLog_GetMqttPendingCount() == 0U);
}

static void test_interrupt_during_write(void)
{
    char lines[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE] = {{0}};
    AppLog_Init();
    g_inject_log = 1;
    APP_LOGI("MAIN", "outer");
    assert(AppLog_GetMqttPendingCount() == 2U);
    assert(AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX) == 2U);
    assert(strstr(lines[0], "ISR nested") != 0);
    assert(strstr(lines[1], "MAIN outer") != 0);
}

static void test_critical_section_restores_mask(void)
{
    char lines[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE] = {{0}};
    AppLog_Init();
    g_irq_mask = 1U;
    APP_LOGI("MAIN", "masked");
    assert(g_irq_mask == 1U);
    g_irq_mask = 0U;
    g_pending_irq = 1;
    APP_LOGI("MAIN", "outer");
    assert(g_irq_mask == 0U);
    assert(AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX) == 3U);
    assert(strstr(lines[0], "MAIN masked") != 0);
    assert(strstr(lines[1], "MAIN outer") != 0);
    assert(strstr(lines[2], "ISR deferred") != 0);
}

static void test_read_snapshot_and_overwrite(void)
{
    char lines[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE] = {{0}};
    AppLog_Init();
    APP_LOGI("MAIN", "first");
    APP_LOGI("MAIN", "second");
    g_inject_format = 70;
    assert(AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX) == 1U);
    assert(strstr(lines[0], "MAIN first") != 0);
    AppLog_CommitMqttLines();
    assert(AppLog_GetMqttPendingCount() == 64U);
    assert(AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX) == APP_LOG_MQTT_BATCH_MAX);
    assert(strstr(lines[0], "LOG dropped=7") != 0);
}

static void test_clear_invalidates_inflight_commit(void)
{
    char lines[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE] = {{0}};
    AppLog_Init();
    APP_LOGI("MAIN", "before-clear");
    assert(AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX) == 1U);
    AppLog_Clear();
    APP_LOGI("MAIN", "after-clear");
    AppLog_CommitMqttLines();
    assert(AppLog_GetMqttPendingCount() == 1U);
    assert(AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX) == 1U);
    assert(strstr(lines[0], "after-clear") != 0);
}

static void test_dump_and_clear_interleavings(void)
{
    char dump[512];
    char line[APP_LOG_MQTT_LINE_SIZE];
    AppLog_Init();
    APP_LOGI("MAIN", "old-a");
    APP_LOGI("MAIN", "old-b");
    g_inject_format = 70;
    (void)AppLog_Dump(dump, sizeof(dump));
    assert(strstr(dump, "during-read") == 0); /* snapshot excludes new entries */
    assert(strstr(dump, "old-a") == 0 && strstr(dump, "old-b") == 0);
    assert(AppLog_ReadMqttLine(line, sizeof(line)) > 0U);
    assert(strstr(line, "ISR during-read=6") != 0);
    g_pending_irq = 1;
    AppLog_Clear();
    assert(AppLog_GetMqttPendingCount() == 1U);
    assert(AppLog_ReadMqttLine(line, sizeof(line)) > 0U);
    assert(strstr(line, "ISR deferred") != 0);
}

static void test_sequence_wrap(void)
{
    char lines[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE] = {{0}};
    char dump[512];
    AppLog_TestSetSequence(UINT32_MAX - 1U);
    APP_LOGI("TEST", "a");
    APP_LOGI("TEST", "b");
    APP_LOGI("TEST", "c");
    assert(AppLog_GetMqttPendingCount() == 3U);
    assert(AppLog_Dump(dump, sizeof(dump)) > 0U);
    assert(strstr(dump, "TEST a") && strstr(dump, "TEST b") && strstr(dump, "TEST c"));
    assert(AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX) == 3U);
    AppLog_AbortMqttLines();
    assert(AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX) == 3U);
    AppLog_CommitMqttLines();
    assert(AppLog_GetMqttPendingCount() == 0U);
    AppLog_TestSetSequence(UINT32_MAX - 2U);
    for (unsigned i = 0U; i < 70U; ++i) APP_LOGI("TEST", "wrap=%u", i);
    assert(AppLog_GetMqttPendingCount() == 64U);
    assert(AppLog_PeekMqttLines(lines, APP_LOG_MQTT_BATCH_MAX) == APP_LOG_MQTT_BATCH_MAX);
    assert(strstr(lines[0], "LOG dropped=6") != 0);
}

int main(void)
{
    test_interrupt_during_write();
    test_critical_section_restores_mask();
    test_read_snapshot_and_overwrite();
    test_clear_invalidates_inflight_commit();
    test_dump_and_clear_interleavings();
    test_sequence_wrap();
    test_retry_and_commit();
    test_overwrite_notice();
    test_noise_only_is_consumed();
    puts("app log tests passed");
    return 0;
}

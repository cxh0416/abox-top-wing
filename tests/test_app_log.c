#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_log.h"

static uint32_t g_tick;

uint32_t HAL_GetTick(void)
{
    return g_tick++;
}

uint8_t TimeBase_FormatBeijingFromTick(uint32_t tick, char* out, uint16_t out_sz)
{
    (void)tick;
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

int main(void)
{
    test_retry_and_commit();
    test_overwrite_notice();
    test_noise_only_is_consumed();
    puts("app log tests passed");
    return 0;
}

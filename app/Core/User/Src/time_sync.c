#include "time_sync.h"
#include "time_base.h"
#include "abox_time_client.h"
#include "abox_platform_port.h"
#include "mqtt_core.h"
#include "mqtt_runtime.h"
#include "app_log.h"
#include "main.h"
#include <string.h>

static ABoxTimeClient client;
static uint8_t initialized;

static int send(void* context, const char* topic, const char* packet, size_t length)
{
    (void)context; (void)length;
    return !MQTT_Core_IsPublishActive() && MQTT_Core_PublishEx(topic, packet, 1U, 0U);
}

static int apply(void* context, uint64_t utc, uint32_t error, uint64_t sent, uint64_t received)
{
    (void)context;
    int ok = TimeBase_Synchronize(utc, error, sent, received);
    if (ok) APP_LOGI("TIME", "platform rtt=%lu err=%lu", (unsigned long)(received-sent),
        (unsigned long)TimeBase_GetSnapshot().error_ms);
    return ok;
}

void TimeSync_Init(const char* device_id)
{
    if (initialized)
    {
        if (!ABoxTimeClient_SetDevice(&client, device_id)) APP_LOGE("TIME", "invalid device identity");
        return;
    }
    const ABoxTimeClientPort port = {NULL, send, apply};
    uint64_t seed = ABox_PortGetMonotonicMs() ^ ((uint64_t)HAL_GetUIDw0() << 32U) ^ HAL_GetUIDw1();
    initialized = ABoxTimeClient_Init(&client, device_id, seed ? seed : 1U, &port);
    if (!initialized) APP_LOGE("TIME", "client initialization failed");
}

const char* TimeSync_ResponseTopic(void) { return client.response_topic; }

void TimeSync_Task(void)
{
    if (initialized) ABoxTimeClient_Poll(&client, MQTT_Core_IsReady() && MqttRuntime_IsTlsActive(),
        MQTT_Core_GetReadySeq(), ABox_PortGetMonotonicMs(), TimeBase_GetMs());
}

int TimeSync_OnMessage(const char* topic, const char* payload)
{
    return initialized && ABoxTimeClient_Message(&client, topic, payload, payload ? strlen(payload) : 0U,
        MQTT_Core_IsReady() && MqttRuntime_IsTlsActive(), MQTT_Core_GetReadySeq(), ABox_PortGetMonotonicMs());
}

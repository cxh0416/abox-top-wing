#include "time_sync.h"
#include "time_base.h"
#include "abox_time_sync.h"
#include "abox_platform_port.h"
#include "mqtt_core.h"
#include "mqtt_runtime.h"
#include "app_log.h"
#include "main.h"
#include "cJSON.h"
#include "abox_mqtt_digest.h"
#include <stdio.h>
#include <string.h>

/* Explicitly initialized workspace above the fixed Boot fault mailbox. */
static ABoxTimeSync g_sync;
static char g_request_topic[128];
static char g_response_topic[128];
static uint64_t g_boot_seed;
static uint32_t g_ready_sequence;
static int send(void *context, const char *id)
{
    char packet[192];
    char utc[21];
    (void)context;
    if (MQTT_Core_IsPublishActive()) return 0;
    if (!TimeBase_FormatUtcMs(utc, sizeof(utc))) strcpy(utc, "0");
    snprintf(packet,sizeof(packet),"{\"version\":\"2.0\",\"timestamp\":%s,\"data\":{"
             "\"requestId\":\"%s\",\"command\":\"sync_time\",\"params\":{}}}",utc,id);
    return MQTT_Core_PublishEx(g_request_topic,packet,1U,0U);
}
static int apply(void *context, uint64_t utc, uint32_t error, uint64_t sent, uint64_t received)
{
    int ok;
    (void)context;
    ok = TimeBase_Synchronize(utc,error,sent,received);
    if (ok) {
        ABoxClockSnapshot snapshot = TimeBase_GetSnapshot();
        APP_LOGI("TIME","platform rtt=%lu err=%lu",(unsigned long)(received-sent),
                 (unsigned long)snapshot.error_ms);
    }
    return ok;
}
void TimeSync_Init(const char *device_id)
{
    uint32_t sequence = g_boot_seed ? g_sync.sequence : 0U;
    ABoxTimeSync_Init(&g_sync);
    g_sync.sequence = sequence; /* Reconfiguration cannot reuse an in-boot ID. */
    snprintf(g_request_topic,sizeof(g_request_topic),"/zxwl/abox/%s/internal/time/request",device_id);
    snprintf(g_response_topic,sizeof(g_response_topic),"/zxwl/abox/%s/internal/time/response",device_id);
}
const char *TimeSync_ResponseTopic(void) { return g_response_topic; }
void TimeSync_Task(void)
{
    static const ABoxTimeSyncPort port = {0,send,apply};
    uint64_t mono = ABox_PortGetMonotonicMs();
    uint8_t ready = MQTT_Core_IsReady() && MqttRuntime_IsTlsActive();
    if (ready && g_ready_sequence != MQTT_Core_GetReadySeq()) {
        g_ready_sequence = MQTT_Core_GetReadySeq();
        g_sync.ready = g_sync.pending = g_sync.attempts = g_sync.have_best = 0U;
    }
    /* Platform UTC must remain available even if CCLK bootstrap was rejected. */
    if (!g_boot_seed && ready) {
        g_boot_seed = TimeBase_GetMs() ^ mono ^ ((uint64_t)HAL_GetUIDw0() << 32) ^ HAL_GetUIDw1();
        if (!g_boot_seed) g_boot_seed = 1U;
    }
    ABoxTimeSync_Poll(&g_sync,&port,ready && g_boot_seed != 0,mono,g_boot_seed);
}
static int integer(const cJSON *value, uint64_t *out)
{
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 ||
        value->valuedouble > 9007199254740991.0 ||
        value->valuedouble != (double)(uint64_t)value->valuedouble) return 0;
    *out = (uint64_t)value->valuedouble; return 1;
}
int TimeSync_OnMessage(const char *topic, const char *payload)
{
    cJSON *root, *data, *id, *version, *code, *result;
    uint64_t utc, error;
    uint64_t received = ABox_PortGetMonotonicMs();
    if (strcmp(topic,g_response_topic)) return 0;
    if (!MQTT_Core_IsReady() || !MqttRuntime_IsTlsActive() || g_ready_sequence != MQTT_Core_GetReadySeq()) return 1;
    root = ABoxMqttDigest_Parse(payload, strlen(payload));
    if (!root) return 1;
    version = cJSON_GetObjectItemCaseSensitive(root,"version");
    data = cJSON_GetObjectItemCaseSensitive(root,"data");
    id = cJSON_GetObjectItemCaseSensitive(data,"requestId");
    code = cJSON_GetObjectItemCaseSensitive(data,"code");
    result = cJSON_GetObjectItemCaseSensitive(data,"result");
    if (cJSON_IsString(version) && !strcmp(version->valuestring,"2.0") &&
        cJSON_IsString(id) && cJSON_IsNumber(code) && code->valuedouble == 200 &&
        integer(cJSON_GetObjectItemCaseSensitive(result,"serverUtcMs"),&utc) &&
        integer(cJSON_GetObjectItemCaseSensitive(result,"errorMs"),&error) && error <= ABOX_CLOCK_MAX_REFERENCE_ERROR_MS)
        (void)ABoxTimeSync_Response(&g_sync,id->valuestring,utc,(uint32_t)error,received);
    cJSON_Delete(root);
    return 1;
}

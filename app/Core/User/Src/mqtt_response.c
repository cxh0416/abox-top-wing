#include <string.h>
#include "abox_cloud_queue.h"
#include "abox_cloud_response.h"
#include "app_log.h"
#include "main.h"
#include "mqtt_app_int.h"
#include "mqtt_response_internal.h"
#include "mqtt_core.h"
#include "ota_service.h"
#include "time_base.h"

#define PRODUCT_ID "top_flying_wing"
#define RESPONSE_QUEUE_DEPTH 2U
#define RESP_STALE_MS 8000U
#define OTA_FAILURE_RESP_STALE_MS 45000U
typedef struct
{
    uint8_t ota_failure;
    ResponseTopic topic;
    uint16_t code;
    char request_id[65];
    char payload[MQTT_APP_JSON_BUF_SZ];
} ResponseItem;
static ResponseItem g_queue[RESPONSE_QUEUE_DEPTH];
static const ABoxCloudQueueStorage queue_storage = {g_queue, sizeof(g_queue[0]), RESPONSE_QUEUE_DEPTH};
static ABoxCloudQueueCursor queue_cursor;
static uint64_t response_operation;
static uint8_t pending_ota_failure;

static void copy_id(char dst[65], const char* src)
{
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, 64U);
    dst[64] = '\0';
}

const char* MQTT_Response_Topic(ResponseTopic topic)
{
    if (topic == RESP_INTERNAL_CONFIG) return g_mqtt_app_internal_config_resp_topic;
    if (topic == RESP_INTERNAL_OTA) return g_mqtt_app_internal_ota_resp_topic;
    return g_mqtt_app_ctrl_resp_topic;
}

void MQTT_Response_Reset(void)
{
    g_mqtt_app_resp_pending = 0U;
    g_mqtt_app_resp_publish_active = 0U;
    g_mqtt_app_resp_payload[0] = '\0';
    g_mqtt_app_resp_topic[0] = '\0';
    g_mqtt_app_resp_request_id[0] = '\0';
    g_mqtt_app_resp_code = 0U;
    response_operation = 0U;
    pending_ota_failure = 0U;
    ABoxCloudQueue_Reset(&queue_storage, &queue_cursor);
}

void MQTT_App_PromoteQueuedResponse(void)
{
    ResponseItem* item;
    if (g_mqtt_app_resp_pending) return;
    item = ABoxCloudQueue_Front(&queue_storage, &queue_cursor);
    if (!item) return;
    strncpy(g_mqtt_app_resp_topic, MQTT_Response_Topic(item->topic), sizeof(g_mqtt_app_resp_topic) - 1U);
    strncpy(g_mqtt_app_resp_payload, item->payload, sizeof(g_mqtt_app_resp_payload) - 1U);
    g_mqtt_app_resp_topic[sizeof(g_mqtt_app_resp_topic) - 1U] = '\0';
    g_mqtt_app_resp_payload[sizeof(g_mqtt_app_resp_payload) - 1U] = '\0';
    copy_id(g_mqtt_app_resp_request_id, item->request_id);
    g_mqtt_app_resp_code = item->code;
    pending_ota_failure = item->ota_failure;
    (void)ABoxCloudQueue_Pop(&queue_storage, &queue_cursor);
    g_mqtt_app_resp_pending = 1U;
    g_mqtt_app_resp_tick = HAL_GetTick();
}

uint8_t MQTT_App_DropStalePendingResponse(uint32_t now, uint32_t stale_ms)
{
    if (!g_mqtt_app_resp_pending || (now - g_mqtt_app_resp_tick) < stale_ms) return 0U;
    APP_LOGW("RESP", "stale rid=%s code=%u", g_mqtt_app_resp_request_id,
             (unsigned)g_mqtt_app_resp_code);
    g_mqtt_app_resp_pending = 0U;
    g_mqtt_app_resp_publish_active = 0U;
    pending_ota_failure = 0U;
    MQTT_App_PromoteQueuedResponse();
    return 1U;
}

static uint8_t queue_response(ResponseTopic topic, const char* payload, const char* request_id,
                              uint16_t code, uint8_t ota_failure)
{
    ResponseItem* item;
    if (!payload || strlen(payload) >= MQTT_APP_JSON_BUF_SZ) return 0U;
    if (!g_mqtt_app_resp_pending)
    {
        strncpy(g_mqtt_app_resp_topic, MQTT_Response_Topic(topic), sizeof(g_mqtt_app_resp_topic) - 1U);
        strncpy(g_mqtt_app_resp_payload, payload, sizeof(g_mqtt_app_resp_payload) - 1U);
        g_mqtt_app_resp_topic[sizeof(g_mqtt_app_resp_topic) - 1U] = '\0';
        g_mqtt_app_resp_payload[sizeof(g_mqtt_app_resp_payload) - 1U] = '\0';
        copy_id(g_mqtt_app_resp_request_id, request_id);
        g_mqtt_app_resp_code = code;
        pending_ota_failure = ota_failure;
        g_mqtt_app_resp_pending = 1U;
        g_mqtt_app_resp_tick = HAL_GetTick();
        return 1U;
    }
    item = ABoxCloudQueue_Reserve(&queue_storage, &queue_cursor);
    if (!item) return 0U;
    item->ota_failure = ota_failure;
    item->topic = topic;
    item->code = code;
    copy_id(item->request_id, request_id);
    strncpy(item->payload, payload, sizeof(item->payload) - 1U);
    item->payload[sizeof(item->payload) - 1U] = '\0';
    return ABoxCloudQueue_Commit(&queue_storage, &queue_cursor);
}

uint8_t MQTT_Response_Queue(ResponseTopic topic, const char* payload, const char* request_id, uint16_t code)
{ return queue_response(topic, payload, request_id, code, 0U); }

uint8_t MQTT_Response_QueueOtaFailure(const char* payload, const char* request_id, uint16_t code)
{ return queue_response(RESP_INTERNAL_OTA, payload, request_id, code, 1U); }

uint8_t MQTT_App_QueueMqttTrialResponse(const char* request_id, uint16_t code)
{
    char timestamp[21], output[256];
    const char* message = code == 202U ? "MQTT_CANDIDATE_TESTING" :
        code == 200U ? "MQTT_CONFIG_SAVED" : "MQTT_CANDIDATE_RESTORED";
    if (!TimeBase_FormatUtcMs(timestamp,sizeof(timestamp))) return 0U;
    if (!ABoxCloudResponse_Encode(output,sizeof(output),"2.0",timestamp,PRODUCT_ID,
                                  request_id,code,message,"{}")) return 0U;
    return MQTT_Response_Queue(RESP_INTERNAL_CONFIG,output,request_id,code);
}

void MQTT_App_HandlePendingResponseTask(void)
{
    if (!g_mqtt_app_resp_pending) return;
    if (g_mqtt_app_resp_publish_active)
    {
        ABoxMqttReceiptStatus receipt = MQTT_Core_PublishResult(response_operation);
        if (receipt == ABOX_MQTT_RECEIPT_CONFIRMED)
        {
            MQTT_App_ResponseConfirmed(g_mqtt_app_resp_request_id, g_mqtt_app_resp_code);
            if (strstr(g_mqtt_app_resp_payload, "\"bootDescriptorValid\"")) OtaService_AcknowledgeRollback();
            g_mqtt_app_resp_pending = 0U;
            pending_ota_failure = 0U;
            MQTT_App_ResetResponseConfirmed(g_mqtt_app_resp_topic, g_mqtt_app_resp_request_id,
                                            g_mqtt_app_resp_code);
            MQTT_App_PromoteQueuedResponse();
            return;
        }
        if (receipt != ABOX_MQTT_RECEIPT_PENDING) g_mqtt_app_resp_publish_active = 0U;
    }
    if (!MQTT_App_IsOtaResetResponse(g_mqtt_app_resp_topic, g_mqtt_app_resp_request_id, g_mqtt_app_resp_code) &&
        MQTT_App_DropStalePendingResponse(HAL_GetTick(), pending_ota_failure ? OTA_FAILURE_RESP_STALE_MS : RESP_STALE_MS))
    { response_operation = 0U; return; }
    if (g_mqtt_app_resp_publish_active) return;
    if (MQTT_Core_IsReady() && MQTT_Core_PublishExTracked(g_mqtt_app_resp_topic,
        g_mqtt_app_resp_payload, 1U, 0U, &response_operation)) g_mqtt_app_resp_publish_active = 1U;
}

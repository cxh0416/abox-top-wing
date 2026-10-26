#include <stdio.h>
#include <string.h>
#include "app_log.h"
#include "cJSON.h"
#include "main.h"
#include "mqtt_app_int.h"
#include "mqtt_core.h"
#include "mqtt_v4.h"
#include "ota_service.h"
#include "time_base.h"

#define RESP_STALE_MS 8000U
#define LOG_PERIOD_MS 1000U
static char json_buf[MQTT_APP_JSON_BUF_SZ];
static char log_lines[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE];
static uint8_t log_publish_active;
static uint64_t log_operation;
static uint64_t response_operation;

void MQTT_App_ResetReportState(void)
{
    if (log_publish_active) AppLog_AbortMqttLines();
    log_publish_active = 0U; log_operation = 0U; response_operation = 0U;
    MQTT_V4_ResetReports();
}

void MQTT_App_HandlePendingResponseTask(void)
{
    if (!g_mqtt_app_resp_pending) return;
    if (MQTT_App_DropStalePendingResponse(HAL_GetTick(), RESP_STALE_MS)) { response_operation = 0U; return; }
    if (g_mqtt_app_resp_publish_active)
    {
        ABoxMqttReceiptStatus receipt = MQTT_Core_PublishResult(response_operation);
        if (receipt == ABOX_MQTT_RECEIPT_PENDING) return;
        g_mqtt_app_resp_publish_active = 0U;
        if (receipt == ABOX_MQTT_RECEIPT_CONFIRMED)
        {
            MQTT_App_ResponseConfirmed(g_mqtt_app_resp_request_id, g_mqtt_app_resp_code);
            if (strstr(g_mqtt_app_resp_payload, "\"bootDescriptorValid\"")) OtaService_AcknowledgeRollback();
            g_mqtt_app_resp_pending = 0U;
            if (g_mqtt_app_ota_reset_pending) g_mqtt_app_ota_reset_tick = HAL_GetTick();
            MQTT_App_PromoteQueuedResponse();
            return;
        }
    }
    if (MQTT_Core_IsReady() && MQTT_Core_PublishExTracked(g_mqtt_app_resp_topic,
        g_mqtt_app_resp_payload, 1U, 0U, &response_operation)) g_mqtt_app_resp_publish_active = 1U;
}

void MQTT_App_HandleStatusReportTask(void) { MQTT_V4_ReportTask(); }
void MQTT_App_HandleHeartbeatTask(void) { MQTT_V4_ReportTask(); }

void MQTT_App_HandleLogReportTask(void)
{
    static uint32_t last_tick;
    uint32_t now=HAL_GetTick();
    char ts[21];
    uint8_t i,count;
    cJSON *root=0,*data=0,*lines=0;

    if(log_publish_active)
    {
        ABoxMqttReceiptStatus receipt=MQTT_Core_PublishResult(log_operation);
        if(receipt==ABOX_MQTT_RECEIPT_PENDING)return;
        if(receipt==ABOX_MQTT_RECEIPT_CONFIRMED)
        {
            AppLog_CommitMqttLines();
            last_tick=now;
        }
        else AppLog_AbortMqttLines();
        log_publish_active=0U;
    }
    if(now-last_tick<LOG_PERIOD_MS||!MQTT_App_CanRunBackground(1500U)||
       !TimeBase_FormatUtcMs(ts,sizeof(ts)))return;
    count=AppLog_PeekMqttLines(log_lines,APP_LOG_MQTT_BATCH_MAX);
    if(!count)return;
    root=cJSON_CreateObject();data=cJSON_CreateObject();lines=cJSON_CreateArray();
    if(!root||!data||!lines)goto done;
    cJSON_AddStringToObject(root,"version",MQTT_APP_INTERNAL_VERSION_STR);cJSON_AddRawToObject(root,"timestamp",ts);
    cJSON_AddStringToObject(data,"product","top_flying_wing");
    for(i=0U;i<count;i++)cJSON_AddItemToArray(lines,cJSON_CreateString(log_lines[i]));
    cJSON_AddItemToObject(data,"lines",lines);lines=0;cJSON_AddItemToObject(root,"data",data);data=0;
    if(cJSON_PrintPreallocated(root,json_buf,sizeof(json_buf),0)&&strlen(json_buf)<=1024U&&
       MQTT_Core_PublishExTracked(g_mqtt_app_log_topic,json_buf,0U,0U,&log_operation))
    {
        log_publish_active=1U;
    }
    else AppLog_AbortMqttLines();
done:if(lines)cJSON_Delete(lines);if(data)cJSON_Delete(data);if(root)cJSON_Delete(root);
}

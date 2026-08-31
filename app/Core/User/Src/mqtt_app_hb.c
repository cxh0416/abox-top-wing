#include <stdio.h>
#include <string.h>
#include "app_version.h"
#include "app_log.h"
#include "cJSON.h"
#include "main.h"
#include "mqtt_app_int.h"
#include "mqtt_core.h"
#include "ota_service.h"
#include "time_base.h"
#include "wing_controller.h"

#define RESP_STALE_MS 8000U
#define LOG_PERIOD_MS 1000U
#define STATUS_QOS1_RETRY_MAX 2U
static char json_buf[MQTT_APP_JSON_BUF_SZ];
static char log_lines[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE];
static uint8_t log_publish_active;
static uint32_t log_publish_ok_seq;
static uint32_t status_last_tick;
static uint32_t status_publish_ok_seq;
static WingReportReason status_pending;
static uint8_t status_was_ready;
static uint8_t status_payload_ready;
static uint8_t status_publish_active;
static uint8_t status_publish_qos;
static uint8_t status_retry_count;

static uint8_t status_motion_active(const WingSnapshot* snapshot)
{
    return snapshot &&
           (snapshot->motion_state == WING_MOTION_ASCENDING ||
            snapshot->motion_state == WING_MOTION_DESCENDING ||
            snapshot->motion_state == WING_MOTION_MOVING_UNKNOWN);
}

static uint8_t heartbeat_due(uint32_t now)
{
    uint32_t period = g_mqtt_app_first_hb ? MQTT_APP_FIRST_HEARTBEAT_DELAY_MS :
                                              MQTT_APP_HEARTBEAT_PERIOD_MS;
    return (now - g_mqtt_app_last_hb_tick >= period) ? 1U : 0U;
}

static void status_clear_pending(void)
{
    status_pending = WING_REPORT_NONE;
    status_payload_ready = 0U;
    status_publish_active = 0U;
    status_publish_qos = 0U;
    status_retry_count = 0U;
}

void MQTT_App_ResetReportState(void)
{
    if (log_publish_active) AppLog_AbortMqttLines();
    status_last_tick = HAL_GetTick();
    status_publish_ok_seq = 0U;
    status_was_ready = 0U;
    status_clear_pending();
    log_publish_active = 0U;
    log_publish_ok_seq = 0U;
}

void MQTT_App_HandlePendingResponseTask(void)
{
    if(!g_mqtt_app_resp_pending)return;
    if(MQTT_App_DropStalePendingResponse(HAL_GetTick(),RESP_STALE_MS))return;
    if(g_mqtt_app_resp_publish_active)
    {
        if(MQTT_Core_IsPublishActive())return;
        if(MQTT_Core_GetPubOkSeq()!=g_mqtt_app_resp_pub_ok_seq)
        {
            APP_LOGI("RESP","ok rid=%s code=%u",g_mqtt_app_resp_request_id,(unsigned)g_mqtt_app_resp_code);
            if(strstr(g_mqtt_app_resp_payload,"\"bootDescriptorValid\""))OtaService_AcknowledgeRollback();
            g_mqtt_app_resp_pending=g_mqtt_app_resp_publish_active=0U;
            if(g_mqtt_app_ota_reset_pending)g_mqtt_app_ota_reset_tick=HAL_GetTick();
            MQTT_App_PromoteQueuedResponse();
            return;
        }
        APP_LOGW("RESP","retry rid=%s code=%u",g_mqtt_app_resp_request_id,(unsigned)g_mqtt_app_resp_code);
        g_mqtt_app_resp_publish_active=0U;
    }
    if(MQTT_Core_IsReady()&&MQTT_Core_Publish(g_mqtt_app_resp_topic,g_mqtt_app_resp_payload,1U))
    {
        APP_LOGI("RESP","start rid=%s code=%u",g_mqtt_app_resp_request_id,(unsigned)g_mqtt_app_resp_code);
        g_mqtt_app_resp_publish_active=1U;g_mqtt_app_resp_pub_ok_seq=MQTT_Core_GetPubOkSeq();
    }
}

void MQTT_App_HandleStatusReportTask(void)
{
    uint32_t now = HAL_GetTick();
    const WingSnapshot* snapshot = WingController_GetSnapshot();
    const char* report_reason;
    char ts[21], report_id[96];
    cJSON *root = 0, *data = 0;

    if (!MQTT_Core_IsReady())
    {
        status_was_ready = 0U;
        status_clear_pending();
        return;
    }

    if (status_publish_active)
    {
        if (MQTT_Core_IsPublishActive()) return;
        status_publish_active = 0U;
        if (MQTT_Core_GetPubOkSeq() != status_publish_ok_seq)
        {
            status_clear_pending();
        }
        else if (status_publish_qos == 0U)
        {
            status_clear_pending();
        }
        else if (status_retry_count < STATUS_QOS1_RETRY_MAX)
        {
            status_retry_count++;
            APP_LOGW("STATUS", "retry reason=%s count=%u",
                     WingReportReason_Name(status_pending), (unsigned)status_retry_count);
        }
        else
        {
            APP_LOGW("STATUS", "drop reason=%s after retries",
                     WingReportReason_Name(status_pending));
            status_clear_pending();
        }
    }

    if (!status_payload_ready && status_pending == WING_REPORT_NONE)
        status_pending = WingController_TakeReportReason();
    if (!status_was_ready && status_pending == WING_REPORT_NONE)
        status_pending = WING_REPORT_STATE_CHANGED;
    status_was_ready = 1U;

    if (!status_payload_ready && status_pending == WING_REPORT_NONE &&
        status_motion_active(snapshot) &&
        (now - status_last_tick >= MQTT_APP_MOTION_REPORT_PERIOD_MS))
        status_pending = WING_REPORT_PERIODIC;

    if (status_pending == WING_REPORT_PERIODIC && heartbeat_due(now)) return;
    if (status_pending == WING_REPORT_NONE || MQTT_Core_IsPublishActive() ||
        g_mqtt_app_resp_pending) return;

    if (!status_payload_ready)
    {
        if (!TimeBase_FormatUtcMs(ts, sizeof(ts))) return;
        snprintf(report_id, sizeof(report_id), "%s-%s-%lu", g_mqtt_app_device_id, ts,
                 (unsigned long)g_mqtt_app_seq_no);
        report_reason = WingReportReason_Name(status_pending);
        root = cJSON_CreateObject();
        data = cJSON_CreateObject();
        if (!root || !data) goto done;
        cJSON_AddStringToObject(root, "version", MQTT_APP_PUBLIC_VERSION_STR);
        cJSON_AddRawToObject(root, "timestamp", ts);
        cJSON_AddStringToObject(data, "deviceType", "wing_actuator");
        cJSON_AddStringToObject(data, "reportId", report_id);
        cJSON_AddStringToObject(data, "reportReason", report_reason);
        MQTT_App_AddWingSnapshot(data);
        cJSON_AddItemToObject(root, "data", data);
        data = 0;
        if (!cJSON_PrintPreallocated(root, json_buf, sizeof(json_buf), 0)) goto done;
        status_publish_qos = status_pending == WING_REPORT_PERIODIC ? 0U : 1U;
        status_payload_ready = 1U;
        status_retry_count = 0U;
        g_mqtt_app_seq_no++;
    }

    if (MQTT_Core_PublishEx(g_mqtt_app_status_report_topic, json_buf,
                            status_publish_qos, 1U))
    {
        status_publish_active = 1U;
        status_publish_ok_seq = MQTT_Core_GetPubOkSeq();
        status_last_tick = now;
    }

done:
    if (data) cJSON_Delete(data);
    if (root) cJSON_Delete(root);
}

void MQTT_App_HandleHeartbeatTask(void)
{
    uint32_t now=HAL_GetTick();char ts[21];cJSON *root=0,*data=0;if(!TimeBase_IsValid())return;
    if(!g_mqtt_app_hb_pending&&heartbeat_due(now))g_mqtt_app_hb_pending=1U;
    if(!g_mqtt_app_hb_pending||g_mqtt_app_resp_pending||!MQTT_Core_IsReady()||MQTT_Core_IsPublishActive()||!TimeBase_FormatUtcMs(ts,sizeof(ts)))return;
    root=cJSON_CreateObject();data=cJSON_CreateObject();if(!root||!data)goto done;cJSON_AddStringToObject(root,"version",MQTT_APP_PUBLIC_VERSION_STR);cJSON_AddRawToObject(root,"timestamp",ts);cJSON_AddStringToObject(data,"deviceType","wing_actuator");cJSON_AddStringToObject(data,"firmware",TopFlyingWing_AppFwVersion());cJSON_AddItemToObject(root,"data",data);data=0;
    if(cJSON_PrintPreallocated(root,json_buf,sizeof(json_buf),0)&&MQTT_Core_Publish(g_mqtt_app_heartbeat_topic,json_buf,0U)){g_mqtt_app_hb_pending=0U;g_mqtt_app_first_hb=0U;g_mqtt_app_last_hb_tick=g_mqtt_app_last_hb_ok_tick=now;g_mqtt_app_seq_no++;}
done:if(data)cJSON_Delete(data);if(root)cJSON_Delete(root);
}

void MQTT_App_HandleLogReportTask(void)
{
    static uint32_t last_tick;
    uint32_t now=HAL_GetTick();
    char ts[21];
    uint8_t i,count;
    cJSON *root=0,*data=0,*lines=0;

    if(log_publish_active)
    {
        if(MQTT_Core_IsPublishActive())return;
        if(MQTT_Core_GetPubOkSeq()!=log_publish_ok_seq)
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
    if(cJSON_PrintPreallocated(root,json_buf,sizeof(json_buf),0)&&
       MQTT_Core_Publish(g_mqtt_app_log_topic,json_buf,0U))
    {
        log_publish_active=1U;
        log_publish_ok_seq=MQTT_Core_GetPubOkSeq();
    }
    else AppLog_AbortMqttLines();
done:if(lines)cJSON_Delete(lines);if(data)cJSON_Delete(data);if(root)cJSON_Delete(root);
}

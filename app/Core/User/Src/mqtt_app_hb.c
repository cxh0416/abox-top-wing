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
static char json_buf[MQTT_APP_JSON_BUF_SZ];
static char log_lines[APP_LOG_MQTT_BATCH_MAX][APP_LOG_MQTT_LINE_SIZE];
static uint8_t log_publish_active;
static uint32_t log_publish_ok_seq;

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
    static uint32_t last_tick;static uint8_t was_ready;static WingReportReason pending;
    uint32_t now=HAL_GetTick();const WingSnapshot* s=WingController_GetSnapshot();WingReportReason r;char ts[21],rid[40];cJSON *root=0,*data=0;
    while((r=WingController_TakeReportReason())!=WING_REPORT_NONE)pending=r;
    if(MQTT_Core_IsReady()&&!was_ready)pending=WING_REPORT_STATE_CHANGED;
    was_ready=MQTT_Core_IsReady();
    if(!pending&&s&&(s->motion_state==WING_MOTION_ASCENDING||s->motion_state==WING_MOTION_DESCENDING||s->motion_state==WING_MOTION_MOVING_UNKNOWN)&&now-last_tick>=1000U)pending=WING_REPORT_STATE_CHANGED;
    if(!pending||!MQTT_Core_IsReady()||MQTT_Core_IsPublishActive()||g_mqtt_app_resp_pending||!TimeBase_FormatUtcMs(ts,sizeof(ts)))return;
    snprintf(rid,sizeof(rid),"%.24s-%lu",g_mqtt_app_device_id,(unsigned long)g_mqtt_app_seq_no);root=cJSON_CreateObject();data=cJSON_CreateObject();if(!root||!data)goto done;
    cJSON_AddStringToObject(root,"version","2.0");cJSON_AddRawToObject(root,"timestamp",ts);cJSON_AddStringToObject(data,"product","top_flying_wing");cJSON_AddStringToObject(data,"reportId",rid);cJSON_AddStringToObject(data,"reason",WingReportReason_Name(pending));MQTT_App_AddWingSnapshot(data);cJSON_AddItemToObject(root,"data",data);data=0;
    if(cJSON_PrintPreallocated(root,json_buf,sizeof(json_buf),0)&&MQTT_Core_PublishEx(g_mqtt_app_status_report_topic,json_buf,1U,1U)){pending=WING_REPORT_NONE;last_tick=now;g_mqtt_app_seq_no++;}
done:if(data)cJSON_Delete(data);if(root)cJSON_Delete(root);
}

void MQTT_App_HandleHeartbeatTask(void)
{
    uint32_t now=HAL_GetTick();char ts[21];cJSON *root=0,*data=0;if(!TimeBase_IsValid())return;
    if(!g_mqtt_app_hb_pending&&now-g_mqtt_app_last_hb_tick>=(g_mqtt_app_seq_no==1U?MQTT_APP_FIRST_HEARTBEAT_DELAY_MS:MQTT_APP_HEARTBEAT_PERIOD_MS))g_mqtt_app_hb_pending=1U;
    if(!g_mqtt_app_hb_pending||g_mqtt_app_resp_pending||!MQTT_Core_IsReady()||MQTT_Core_IsPublishActive()||!TimeBase_FormatUtcMs(ts,sizeof(ts)))return;
    root=cJSON_CreateObject();data=cJSON_CreateObject();if(!root||!data)goto done;cJSON_AddStringToObject(root,"version","2.0");cJSON_AddRawToObject(root,"timestamp",ts);cJSON_AddStringToObject(data,"firmware",TopFlyingWing_AppFwVersion());cJSON_AddStringToObject(data,"product","top_flying_wing");cJSON_AddItemToObject(root,"data",data);data=0;
    if(cJSON_PrintPreallocated(root,json_buf,sizeof(json_buf),0)&&MQTT_Core_Publish(g_mqtt_app_heartbeat_topic,json_buf,0U)){g_mqtt_app_hb_pending=0U;g_mqtt_app_last_hb_tick=g_mqtt_app_last_hb_ok_tick=now;g_mqtt_app_seq_no++;}
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
    cJSON_AddStringToObject(root,"version","2.0");cJSON_AddRawToObject(root,"timestamp",ts);
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

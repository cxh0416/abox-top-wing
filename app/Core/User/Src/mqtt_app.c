#include "mqtt_app.h"

#include <stdio.h>
#include <string.h>
#include "app_log.h"
#include "board_config.h"
#include "boot_state.h"
#include "main.h"
#include "mqtt_app_int.h"
#include "mqtt_core.h"
#include "wing_controller.h"
#include "mqtt_v4.h"
#include "time_sync.h"
#include "mqtt_runtime.h"
#include "abox_maintenance_service.h"
#include "ota_service.h"

#define MQTT_APP_OTA_RESET_DELAY_MS 200U
#define MQTT_APP_OTA_FORCE_RESET_MS 15000U

char g_mqtt_app_device_id[64], g_mqtt_app_heartbeat_topic[128], g_mqtt_app_ctrl_topic[128];
char g_mqtt_app_ctrl_resp_topic[128], g_mqtt_app_status_report_topic[128];
char g_mqtt_app_manifest_topic[128];
char g_mqtt_app_internal_config_topic[128], g_mqtt_app_internal_config_resp_topic[128];
char g_mqtt_app_internal_ota_topic[128], g_mqtt_app_internal_ota_resp_topic[128], g_mqtt_app_log_topic[128];
uint32_t g_mqtt_app_last_hb_tick, g_mqtt_app_last_hb_ok_tick, g_mqtt_app_seq_no;
uint8_t g_mqtt_app_first_hb;
volatile uint8_t g_mqtt_app_hb_pending, g_mqtt_app_resp_pending, g_mqtt_app_resp_publish_active;
uint32_t g_mqtt_app_resp_pub_ok_seq, g_mqtt_app_resp_tick;
char g_mqtt_app_resp_topic[128], g_mqtt_app_resp_payload[MQTT_APP_JSON_BUF_SZ];
char g_mqtt_app_resp_request_id[65];
uint16_t g_mqtt_app_resp_code;
volatile uint8_t g_mqtt_app_ota_reset_pending;
uint32_t g_mqtt_app_ota_reset_tick, g_mqtt_app_ota_request_tick;

typedef struct {
    ABoxMqttTrial trial;
    ABoxMqttTrialExecutor executor;
    ABoxMqttConfig active, candidate;
    BoardConfigStore_t stable, next;
    ABoxMqttTrialExecutorAction working;
    uint64_t working_session;
    uint32_t started_tick, heartbeat_baseline;
    char request_id[65];
    uint8_t initialized, response_confirmed, restore_started, flash_restore_failed, response_done;
} MqttTrial;
static MqttTrial g_trial;
static uint32_t g_boot_trial_tick,g_boot_trial_hb;

static uint8_t copy_text(char* dst,uint16_t size,const char* src){size_t n;if(!dst||!size||!src)return 0U;n=strlen(src);if(n>=size)return 0U;memcpy(dst,src,n+1U);return 1U;}
static uint8_t apply_runtime_config(const char* vid,const char* host,uint16_t port,const char* user,const char* password,uint8_t tls)
{
    MQTT_App_Init(vid);
    MQTT_Core_SetDeviceId(vid);
    MQTT_Core_SetTopics(MQTT_App_GetSubTopics(),MQTT_App_GetSubTopicCount(),MQTT_App_GetHeartbeatTopic());
    return MqttRuntime_ApplyEndpoint(host,port,user,password,tls);
}

void MQTT_App_ResponseConfirmed(const char* id, uint16_t code)
{
    if (g_trial.trial.state == ABOX_MQTT_TRIAL_WAIT_ACCEPT && code == 202U && id && !strcmp(id,g_trial.request_id)) g_trial.response_confirmed=1U;
}
static void confirm_boot_trial(uint32_t now){BootStateRecord_t s;if(!BootState_Load(&s)||s.state!=BOOT_STATE_TRIAL)return;if(!g_boot_trial_tick){g_boot_trial_tick=now;g_boot_trial_hb=g_mqtt_app_last_hb_ok_tick;return;}if(MQTT_Core_IsReady()&&g_mqtt_app_last_hb_ok_tick!=g_boot_trial_hb&&now-g_boot_trial_tick>=30000U){if(BootState_Confirm())APP_LOGI("OTA","BootState CONFIRMED after heartbeat");}}

void MQTT_App_Init(const char* id)
{
    if(!MQTT_V4_ValidDeviceId(id)){memset(g_mqtt_app_device_id,0,sizeof(g_mqtt_app_device_id));return;}
    MQTT_V4_IdentityChanged(id);
    strncpy(g_mqtt_app_device_id,id,sizeof(g_mqtt_app_device_id)-1U);
    snprintf(g_mqtt_app_heartbeat_topic,sizeof(g_mqtt_app_heartbeat_topic),"/zxwl/abox/%s/heartbeat",id);
    (void)ABoxMqttV4_BuildTopic(g_mqtt_app_ctrl_topic,sizeof(g_mqtt_app_ctrl_topic),id,ABOX_MQTT_V4_TOPIC_REQUEST);
    (void)ABoxMqttV4_BuildTopic(g_mqtt_app_ctrl_resp_topic,sizeof(g_mqtt_app_ctrl_resp_topic),id,ABOX_MQTT_V4_TOPIC_RESPONSE);
    (void)ABoxMqttV4_BuildTopic(g_mqtt_app_status_report_topic,sizeof(g_mqtt_app_status_report_topic),id,ABOX_MQTT_V4_TOPIC_REPORT);
    (void)ABoxMqttV4_BuildTopic(g_mqtt_app_manifest_topic,sizeof(g_mqtt_app_manifest_topic),id,ABOX_MQTT_V4_TOPIC_MANIFEST);
    TimeSync_Init(id);
    snprintf(g_mqtt_app_internal_config_topic,sizeof(g_mqtt_app_internal_config_topic),"/zxwl/abox/%s/internal/config",id);
    snprintf(g_mqtt_app_internal_config_resp_topic,sizeof(g_mqtt_app_internal_config_resp_topic),"/zxwl/abox/%s/internal/config/resp",id);
    snprintf(g_mqtt_app_internal_ota_topic,sizeof(g_mqtt_app_internal_ota_topic),"/zxwl/abox/%s/internal/ota",id);
    snprintf(g_mqtt_app_internal_ota_resp_topic,sizeof(g_mqtt_app_internal_ota_resp_topic),"/zxwl/abox/%s/internal/ota/resp",id);
    snprintf(g_mqtt_app_log_topic,sizeof(g_mqtt_app_log_topic),"/zxwl/abox/%s/internal/log",id);
    g_mqtt_app_last_hb_tick=g_mqtt_app_last_hb_ok_tick=HAL_GetTick();g_mqtt_app_seq_no=1U;g_mqtt_app_first_hb=1U;g_mqtt_app_hb_pending=0U;
    MQTT_App_ResetResponseState();MQTT_App_ResetReportState();g_mqtt_app_ota_reset_pending=0U;g_mqtt_app_ota_reset_tick=g_mqtt_app_ota_request_tick=0U;g_boot_trial_tick=g_boot_trial_hb=0U;
}

uint8_t MQTT_App_IsMqttConfigTrialActive(void)
{ return (g_trial.request_id[0] && !g_trial.response_done) || ABoxMaintenanceService_Busy(&g_trial.trial); }

uint16_t MQTT_App_MqttTrialReply(const char* id,const char* vid,const char* host,
    uint16_t port,const char* user,const char* password)
{
    if (!id || !g_trial.request_id[0] || strcmp(id,g_trial.request_id)) return 0U;
    if (strcmp(vid,g_trial.next.vid) || strcmp(host,g_trial.next.mqtt_host) ||
        port!=g_trial.next.mqtt_port || strcmp(user,g_trial.next.mqtt_username) ||
        strcmp(password,g_trial.next.mqtt_password)) return 4006U;
    if (g_trial.trial.state==ABOX_MQTT_TRIAL_COMMITTED) return 200U;
    if (g_trial.trial.state==ABOX_MQTT_TRIAL_FAILED) return 500U;
    if (g_trial.trial.state==ABOX_MQTT_TRIAL_RESTORE_FAILED) return 5001U;
    return 202U;
}

static int write_snapshot(void* context, const void* snapshot)
{ (void)context; return BoardConfig_Save((const BoardConfigStore_t*)snapshot); }

uint8_t MQTT_App_ScheduleMqttConfigTrial(const char* request_id,const char* vid,const char* host,uint16_t port,const char* user,const char* pass,uint8_t tls_enabled)
{
    const BoardConfigStore_t* c = BoardConfig_Get();
    if (!g_trial.initialized) {
        if (!ABoxMqttTrialExecutor_Init(&g_trial.executor, &g_trial.trial)) return 0U;
        g_trial.initialized = 1U;
    }
    if (MQTT_App_IsMqttConfigTrialActive() ||
        OtaService_IsBusy() || g_mqtt_app_ota_reset_pending ||
        !request_id || !*request_id || !MqttRuntime_IsTlsActive() || !tls_enabled || !MQTT_V4_ValidDeviceId(vid) ||
        !host || !*host || !user || !*user || !pass || !*pass || !port) return 0U;
    g_trial.stable = *c; g_trial.next = *c;
    if (!copy_text(g_trial.request_id,sizeof(g_trial.request_id),request_id) ||
        !copy_text(g_trial.next.vid,sizeof(g_trial.next.vid),vid) ||
        !copy_text(g_trial.next.mqtt_host,sizeof(g_trial.next.mqtt_host),host) ||
        !copy_text(g_trial.next.mqtt_username,sizeof(g_trial.next.mqtt_username),user) ||
        !copy_text(g_trial.next.mqtt_password,sizeof(g_trial.next.mqtt_password),pass)) {
        g_trial.request_id[0] = '\0'; return 0U;
    }
    g_trial.next.mqtt_port=port; g_trial.next.flags |= BOARD_CONFIG_FLAG_MQTT_TLS;
    g_trial.active=(ABoxMqttConfig){0}; g_trial.candidate=(ABoxMqttConfig){0};
    g_trial.active.host=g_trial.stable.mqtt_host; g_trial.active.port=g_trial.stable.mqtt_port;
    g_trial.active.username=g_trial.stable.mqtt_username; g_trial.active.password=g_trial.stable.mqtt_password;
    g_trial.active.tls_enabled=(g_trial.stable.flags & BOARD_CONFIG_FLAG_MQTT_TLS) != 0U;
    g_trial.active.tls_credentials.mode=ABOX_TLS_AUTH_CA;
    g_trial.candidate.host=g_trial.next.mqtt_host; g_trial.candidate.port=port;
    g_trial.candidate.username=g_trial.next.mqtt_username; g_trial.candidate.password=g_trial.next.mqtt_password;
    g_trial.candidate.tls_enabled=1U; g_trial.candidate.tls_credentials.mode=ABOX_TLS_AUTH_CA;
    g_trial.started_tick=HAL_GetTick(); g_trial.response_confirmed=0U; g_trial.working=ABOX_MQTT_TRIAL_ACTION_NONE;
    g_trial.restore_started=0U; g_trial.flash_restore_failed=0U; g_trial.response_done=0U;
    if (!ABoxMaintenanceService_StartTls(&g_trial.trial,&g_trial.active,&g_trial.candidate,
            1U,g_trial.started_tick,MQTT_APP_MQTT_CONFIG_TRIAL_TIMEOUT_MS,90000U)) {
        g_trial.request_id[0]='\0'; return 0U;
    }
    return 1U;
}

static uint8_t mqtt_trial_task(uint32_t now)
{
    ABoxMqttTrial* trial=&g_trial.trial;
    ABoxMqttTrialEvent event=ABOX_MQTT_TRIAL_ERROR;
    const ABoxMqttConfig* unused;
    uint8_t have_event=1U;
    static const ABoxConfigStorePort store={0,write_snapshot};
    if (!g_trial.initialized || !g_trial.request_id[0] || g_trial.response_done) return 0U;
    if (trial->state==ABOX_MQTT_TRIAL_WAIT_ACCEPT) {
        if (g_trial.response_confirmed) ABoxMqttTrial_Event(trial,trial->session,ABOX_MQTT_TRIAL_ACCEPTED,0U,now);
        else if (now-g_trial.started_tick>=8000U) { ABoxMqttTrial_Cancel(trial,now); g_trial.response_done=1U; }
        return 1U;
    }
    ABoxMqttTrial_Poll(trial,now);
    if (ABoxMqttTrialExecutor_HasPending(&g_trial.executor)) {
        (void)ABoxMqttTrialExecutor_Take(&g_trial.executor,&g_trial.working,&g_trial.working_session,&unused);
        if (g_trial.working==ABOX_MQTT_TRIAL_ACTION_VERIFY) g_trial.heartbeat_baseline=g_mqtt_app_last_hb_ok_tick;
        if (g_trial.working==ABOX_MQTT_TRIAL_ACTION_RESTORE) g_trial.restore_started=0U;
    }
    switch (g_trial.working) {
    case ABOX_MQTT_TRIAL_ACTION_PREPARE:
        event=apply_runtime_config(g_trial.next.vid,g_trial.next.mqtt_host,g_trial.next.mqtt_port,
            g_trial.next.mqtt_username,g_trial.next.mqtt_password,1U) ? ABOX_MQTT_TRIAL_PREPARED : ABOX_MQTT_TRIAL_ERROR; break;
    case ABOX_MQTT_TRIAL_ACTION_CONNECT:
        if (!MQTT_Core_IsConnected()) { have_event=0U; break; }
        event=ABOX_MQTT_TRIAL_CONNECTED; break;
    case ABOX_MQTT_TRIAL_ACTION_SUBSCRIBE:
        if (!MQTT_Core_IsReady() || !MqttRuntime_IsTlsActive()) { have_event=0U; break; }
        event=ABOX_MQTT_TRIAL_SUBSCRIBED; break;
    case ABOX_MQTT_TRIAL_ACTION_VERIFY:
        if (g_trial.heartbeat_baseline==g_mqtt_app_last_hb_ok_tick) {
            MQTT_App_HandleHeartbeatTask(); have_event=0U; break;
        }
        event=ABOX_MQTT_TRIAL_PROVED; break;
    case ABOX_MQTT_TRIAL_ACTION_COMMIT: {
        ABoxConfigStoreResult result=ABoxMaintenanceService_Commit(trial,g_trial.working_session,&store,
            &g_trial.next,&g_trial.stable,now);
        g_trial.flash_restore_failed=result==ABOX_CONFIG_STORE_RECOVERY_FAILED;
        g_trial.working=ABOX_MQTT_TRIAL_ACTION_NONE; have_event=0U; break;
    }
    case ABOX_MQTT_TRIAL_ACTION_RESTORE:
        if (g_trial.flash_restore_failed) { event=ABOX_MQTT_TRIAL_ERROR; break; }
        if (!g_trial.restore_started) {
            if (!apply_runtime_config(g_trial.stable.vid,g_trial.stable.mqtt_host,g_trial.stable.mqtt_port,
                    g_trial.stable.mqtt_username,g_trial.stable.mqtt_password,1U)) { event=ABOX_MQTT_TRIAL_ERROR; break; }
            g_trial.restore_started=1U; have_event=0U; break;
        }
        if (!MQTT_Core_IsReady() || !MqttRuntime_IsTlsActive()) { have_event=0U; break; }
        event=ABOX_MQTT_TRIAL_RESTORED; break;
    default: have_event=0U; break;
    }
    if (have_event) {
        g_trial.working=ABOX_MQTT_TRIAL_ACTION_NONE;
        ABoxMqttTrial_Event(trial,g_trial.working_session,event,trial->proof,now);
    }
    if ((trial->state==ABOX_MQTT_TRIAL_COMMITTED || trial->state==ABOX_MQTT_TRIAL_FAILED) && MQTT_Core_IsReady() &&
        MQTT_App_QueueMqttTrialResponse(g_trial.request_id,trial->state==ABOX_MQTT_TRIAL_COMMITTED ? 200U : 500U))
        g_trial.response_done=1U;
    return trial->state!=ABOX_MQTT_TRIAL_COMMITTED && trial->state!=ABOX_MQTT_TRIAL_FAILED;
}

void MQTT_App_Task(void)
{
    uint32_t now=HAL_GetTick();confirm_boot_trial(now);WingController_Task(now);MQTT_App_HandleOtaServiceTask();MQTT_App_HandleWingResultTask();MQTT_V4_Task();TimeSync_Task();
    if(g_mqtt_app_ota_reset_pending){if(g_mqtt_app_ota_reset_tick&&now-g_mqtt_app_ota_reset_tick>=MQTT_APP_OTA_RESET_DELAY_MS)NVIC_SystemReset();if(MQTT_Core_IsReady()&&g_mqtt_app_resp_pending)MQTT_App_HandlePendingResponseTask();if(g_mqtt_app_ota_request_tick&&now-g_mqtt_app_ota_request_tick>=MQTT_APP_OTA_FORCE_RESET_MS)NVIC_SystemReset();return;}
    if(g_mqtt_app_resp_pending){MQTT_App_HandlePendingResponseTask();if(g_mqtt_app_resp_pending)return;}
    if(mqtt_trial_task(now))return;
    MQTT_App_HandleStatusReportTask();if(MQTT_Core_IsPublishActive())return;MQTT_App_HandleHeartbeatTask();if(g_mqtt_app_hb_pending||MQTT_Core_IsPublishActive())return;MQTT_App_HandleLogReportTask();
}
void MQTT_App_OnMessage(const char* t,const char* p){if(!g_mqtt_app_ota_reset_pending&&!TimeSync_OnMessage(t,p))MQTT_App_HandleCommandMessage(t,p);}
uint8_t MQTT_App_CanRunBackground(uint32_t guard){uint32_t n=HAL_GetTick(),p=g_mqtt_app_first_hb?MQTT_APP_FIRST_HEARTBEAT_DELAY_MS:MQTT_APP_HEARTBEAT_PERIOD_MS;return !MQTT_App_IsMqttConfigTrialActive()&&!g_mqtt_app_ota_reset_pending&&!g_mqtt_app_resp_pending&&!g_mqtt_app_hb_pending&&MQTT_Core_CanRunBackgroundAt()&&(n-g_mqtt_app_last_hb_tick+guard)<p;}
const char* MQTT_App_GetCtrlTopic(void){return g_mqtt_app_ctrl_topic;}const char* MQTT_App_GetHeartbeatTopic(void){return g_mqtt_app_heartbeat_topic;}const char* MQTT_App_GetInternalConfigTopic(void){return g_mqtt_app_internal_config_topic;}const char* MQTT_App_GetInternalOtaTopic(void){return g_mqtt_app_internal_ota_topic;}
const char* const* MQTT_App_GetSubTopics(void){static const char* t[MQTT_APP_SUB_TOPIC_COUNT];t[0]=g_mqtt_app_ctrl_topic;t[1]=g_mqtt_app_internal_config_topic;t[2]=g_mqtt_app_internal_ota_topic;t[3]=TimeSync_ResponseTopic();return t;}uint8_t MQTT_App_GetSubTopicCount(void){return MQTT_APP_SUB_TOPIC_COUNT;}

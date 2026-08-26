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

#define MQTT_APP_OTA_RESET_DELAY_MS 200U
#define MQTT_APP_OTA_FORCE_RESET_MS 15000U

char g_mqtt_app_device_id[64], g_mqtt_app_heartbeat_topic[128], g_mqtt_app_ctrl_topic[128];
char g_mqtt_app_ctrl_resp_topic[128], g_mqtt_app_status_report_topic[128];
char g_mqtt_app_internal_config_topic[128], g_mqtt_app_internal_config_resp_topic[128];
char g_mqtt_app_internal_ota_topic[128], g_mqtt_app_internal_ota_resp_topic[128], g_mqtt_app_log_topic[128];
uint32_t g_mqtt_app_last_hb_tick, g_mqtt_app_last_hb_ok_tick, g_mqtt_app_seq_no;
volatile uint8_t g_mqtt_app_hb_pending, g_mqtt_app_resp_pending, g_mqtt_app_resp_publish_active;
uint32_t g_mqtt_app_resp_pub_ok_seq, g_mqtt_app_resp_tick;
char g_mqtt_app_resp_topic[128], g_mqtt_app_resp_payload[MQTT_APP_JSON_BUF_SZ];
char g_mqtt_app_resp_request_id[40];
uint16_t g_mqtt_app_resp_code;
volatile uint8_t g_mqtt_app_ota_reset_pending;
uint32_t g_mqtt_app_ota_reset_tick, g_mqtt_app_ota_request_tick;

typedef enum { MQTT_TRIAL_IDLE=0, MQTT_TRIAL_WAIT_RESPONSE, MQTT_TRIAL_TESTING } MqttTrialState;
typedef struct { MqttTrialState state; uint32_t started_tick,pub_ok_seq; char vid[32],host[64],username[32],password[32]; uint16_t port; } MqttTrial;
static MqttTrial g_trial;
static uint32_t g_boot_trial_tick,g_boot_trial_hb;

static uint8_t copy_text(char* dst,uint16_t size,const char* src){size_t n;if(!dst||!size||!src)return 0U;n=strlen(src);if(n>=size)return 0U;memcpy(dst,src,n+1U);return 1U;}
static void apply_runtime_config(const char* vid,const char* host,uint16_t port,const char* user,const char* password){MQTT_App_Init(vid);MQTT_Core_UpdateBroker(host,port,user,password);MQTT_Core_SetTopics(MQTT_App_GetSubTopics(),MQTT_App_GetSubTopicCount(),MQTT_App_GetHeartbeatTopic());MQTT_Core_ForceReconnect();}
static void confirm_boot_trial(uint32_t now){BootStateRecord_t s;if(!BootState_Load(&s)||s.state!=BOOT_STATE_TRIAL)return;if(!g_boot_trial_tick){g_boot_trial_tick=now;g_boot_trial_hb=g_mqtt_app_last_hb_ok_tick;return;}if(MQTT_Core_IsReady()&&g_mqtt_app_last_hb_ok_tick!=g_boot_trial_hb&&now-g_boot_trial_tick>=30000U){if(BootState_Confirm())APP_LOGI("OTA","BootState CONFIRMED after heartbeat");}}

void MQTT_App_Init(const char* id)
{
    if(!id){memset(g_mqtt_app_device_id,0,sizeof(g_mqtt_app_device_id));return;}
    strncpy(g_mqtt_app_device_id,id,sizeof(g_mqtt_app_device_id)-1U);
    snprintf(g_mqtt_app_heartbeat_topic,sizeof(g_mqtt_app_heartbeat_topic),"/zxwl/abox/%s/heartbeat",id);
    snprintf(g_mqtt_app_ctrl_topic,sizeof(g_mqtt_app_ctrl_topic),"/zxwl/abox/%s/ctrl",id);
    snprintf(g_mqtt_app_ctrl_resp_topic,sizeof(g_mqtt_app_ctrl_resp_topic),"/zxwl/abox/%s/ctrl/resp",id);
    snprintf(g_mqtt_app_status_report_topic,sizeof(g_mqtt_app_status_report_topic),"/zxwl/abox/%s/status/report",id);
    snprintf(g_mqtt_app_internal_config_topic,sizeof(g_mqtt_app_internal_config_topic),"/zxwl/abox/%s/internal/config",id);
    snprintf(g_mqtt_app_internal_config_resp_topic,sizeof(g_mqtt_app_internal_config_resp_topic),"/zxwl/abox/%s/internal/config/resp",id);
    snprintf(g_mqtt_app_internal_ota_topic,sizeof(g_mqtt_app_internal_ota_topic),"/zxwl/abox/%s/internal/ota",id);
    snprintf(g_mqtt_app_internal_ota_resp_topic,sizeof(g_mqtt_app_internal_ota_resp_topic),"/zxwl/abox/%s/internal/ota/resp",id);
    snprintf(g_mqtt_app_log_topic,sizeof(g_mqtt_app_log_topic),"/zxwl/abox/%s/internal/log",id);
    g_mqtt_app_last_hb_tick=g_mqtt_app_last_hb_ok_tick=HAL_GetTick();g_mqtt_app_seq_no=1U;g_mqtt_app_hb_pending=0U;
    MQTT_App_ResetResponseState();g_mqtt_app_ota_reset_pending=0U;g_mqtt_app_ota_reset_tick=g_mqtt_app_ota_request_tick=0U;g_boot_trial_tick=g_boot_trial_hb=0U;
}

uint8_t MQTT_App_ScheduleMqttConfigTrial(const char* vid,const char* host,uint16_t port,const char* user,const char* pass)
{
    const BoardConfigStore_t* c=BoardConfig_Get();if(g_trial.state!=MQTT_TRIAL_IDLE)return 0U;memset(&g_trial,0,sizeof(g_trial));
    if(!copy_text(g_trial.vid,sizeof(g_trial.vid),vid?vid:c->vid)||!copy_text(g_trial.host,sizeof(g_trial.host),host?host:c->mqtt_host)||!copy_text(g_trial.username,sizeof(g_trial.username),user?user:c->mqtt_username)||!copy_text(g_trial.password,sizeof(g_trial.password),pass?pass:c->mqtt_password))return 0U;
    g_trial.port=port?port:c->mqtt_port;if(!g_trial.vid[0]||!g_trial.host[0]||!g_trial.port)return 0U;g_trial.pub_ok_seq=MQTT_Core_GetPubOkSeq();g_trial.state=MQTT_TRIAL_WAIT_RESPONSE;return 1U;
}

void MQTT_App_Task(void)
{
    uint32_t now=HAL_GetTick();confirm_boot_trial(now);WingController_Task(now);MQTT_App_HandleOtaServiceTask();MQTT_App_HandleWingResultTask();
    if(g_mqtt_app_ota_reset_pending){if(g_mqtt_app_ota_reset_tick&&now-g_mqtt_app_ota_reset_tick>=MQTT_APP_OTA_RESET_DELAY_MS)NVIC_SystemReset();if(MQTT_Core_IsReady()&&g_mqtt_app_resp_pending)MQTT_App_HandlePendingResponseTask();if(g_mqtt_app_ota_request_tick&&now-g_mqtt_app_ota_request_tick>=MQTT_APP_OTA_FORCE_RESET_MS)NVIC_SystemReset();return;}
    if(g_mqtt_app_resp_pending){MQTT_App_HandlePendingResponseTask();if(g_mqtt_app_resp_pending)return;}
    if(g_trial.state==MQTT_TRIAL_WAIT_RESPONSE){if(MQTT_Core_GetPubOkSeq()==g_trial.pub_ok_seq)return;g_trial.state=MQTT_TRIAL_TESTING;g_trial.started_tick=now;g_trial.pub_ok_seq=MQTT_Core_GetPubOkSeq();apply_runtime_config(g_trial.vid,g_trial.host,g_trial.port,g_trial.username,g_trial.password);return;}
    if(g_trial.state==MQTT_TRIAL_TESTING){if(MQTT_Core_GetPubOkSeq()!=g_trial.pub_ok_seq){(void)BoardConfig_SetMqtt(g_trial.vid,g_trial.host,g_trial.port,g_trial.username,g_trial.password);memset(&g_trial,0,sizeof(g_trial));return;}if(now-g_trial.started_tick>=MQTT_APP_MQTT_CONFIG_TRIAL_TIMEOUT_MS){const BoardConfigStore_t* c=BoardConfig_Get();memset(&g_trial,0,sizeof(g_trial));apply_runtime_config(c->vid,c->mqtt_host,c->mqtt_port,c->mqtt_username,c->mqtt_password);return;}MQTT_App_HandleHeartbeatTask();return;}
    MQTT_App_HandleStatusReportTask();if(MQTT_Core_IsPublishActive())return;MQTT_App_HandleHeartbeatTask();if(g_mqtt_app_hb_pending||MQTT_Core_IsPublishActive())return;MQTT_App_HandleLogReportTask();
}
void MQTT_App_OnMessage(const char* t,const char* p){if(!g_mqtt_app_ota_reset_pending)MQTT_App_HandleCommandMessage(t,p);}
uint8_t MQTT_App_CanRunBackground(uint32_t guard){uint32_t n=HAL_GetTick(),p=g_mqtt_app_seq_no==1U?MQTT_APP_FIRST_HEARTBEAT_DELAY_MS:MQTT_APP_HEARTBEAT_PERIOD_MS;return !g_mqtt_app_ota_reset_pending&&!g_mqtt_app_resp_pending&&!g_mqtt_app_hb_pending&&MQTT_Core_CanRunBackgroundAt()&&(n-g_mqtt_app_last_hb_tick+guard)<p;}
const char* MQTT_App_GetCtrlTopic(void){return g_mqtt_app_ctrl_topic;}const char* MQTT_App_GetHeartbeatTopic(void){return g_mqtt_app_heartbeat_topic;}const char* MQTT_App_GetInternalConfigTopic(void){return g_mqtt_app_internal_config_topic;}const char* MQTT_App_GetInternalOtaTopic(void){return g_mqtt_app_internal_ota_topic;}
const char* const* MQTT_App_GetSubTopics(void){static const char* t[MQTT_APP_SUB_TOPIC_COUNT];t[0]=g_mqtt_app_ctrl_topic;t[1]=g_mqtt_app_internal_config_topic;t[2]=g_mqtt_app_internal_ota_topic;return t;}uint8_t MQTT_App_GetSubTopicCount(void){return MQTT_APP_SUB_TOPIC_COUNT;}

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mqtt_app_int.h"
#include "mqtt_core.h"
#include "board_config.h"
#include "boot_state.h"
#include "wing_controller.h"

static uint32_t tick, unrelated_pub;
static BoardConfigStore_t saved;
static unsigned applies, commits;
static uint8_t save_ok = 1U, applied_tls;
static char applied_host[64];
uint32_t HAL_GetTick(void) { return tick; }
void NVIC_SystemReset(void) { assert(!"unexpected reset"); }
uint8_t BootState_Load(BootStateRecord_t* state) { (void)state; return 0U; }
uint8_t BootState_Confirm(void) { return 1U; }
void AppLog_Printf(int level,const char* tag,const char* format,...) { (void)level;(void)tag;(void)format; }
const BoardConfigStore_t* BoardConfig_Get(void) { return &saved; }
uint8_t BoardConfig_SetMqtt(const char* id,const char* host,uint16_t port,const char* user,const char* password,uint8_t tls)
{
    (void)user;(void)password;
    if(!save_ok)return 0;
    ++commits; strcpy(saved.vid,id);strcpy(saved.mqtt_host,host);saved.mqtt_port=port;
    saved.flags=tls ? BOARD_CONFIG_FLAG_MQTT_TLS : 0U;return 1U;
}
uint8_t MqttRuntime_ApplyEndpoint(const char* host,uint16_t port,const char* user,const char* password,uint8_t tls)
{ (void)port;(void)user;(void)password;++applies;strcpy(applied_host,host);applied_tls=tls;return 1U; }
uint8_t MQTT_Core_IsReady(void) { return 1U; }
uint8_t MQTT_Core_IsPublishActive(void) { return 0U; }
uint8_t MQTT_Core_CanRunBackgroundAt(void) { return 1U; }
uint32_t MQTT_Core_GetPubOkSeq(void) { return unrelated_pub; }
void MQTT_Core_SetDeviceId(const char* id) { (void)id; }
void MQTT_Core_SetTopics(const char* const* topics,uint8_t count,const char* heartbeat)
{ assert(count==4U && topics[3] && heartbeat); }
void WingController_Task(uint32_t now) { (void)now; }
uint8_t MQTT_V4_ValidDeviceId(const char* id) { return id && *id && strlen(id)<=32U; }
void MQTT_V4_IdentityChanged(const char* id) { (void)id; }
void MQTT_V4_Task(void) {}
void TimeSync_Init(const char* id) { (void)id; }
void TimeSync_Task(void) {}
int TimeSync_OnMessage(const char* topic,const char* payload) { (void)topic;(void)payload;return 0; }
const char* TimeSync_ResponseTopic(void) { return "/time/response"; }
void MQTT_App_ResetResponseState(void) { g_mqtt_app_resp_pending=0U; }
void MQTT_App_ResetReportState(void) {}
void MQTT_App_HandleCommandMessage(const char* topic,const char* payload) { (void)topic;(void)payload; }
void MQTT_App_HandleWingResultTask(void) {}
void MQTT_App_HandleOtaServiceTask(void) {}
void MQTT_App_HandlePendingResponseTask(void) {}
void MQTT_App_HandleStatusReportTask(void) {}
void MQTT_App_HandleHeartbeatTask(void) {}
void MQTT_App_HandleLogReportTask(void) {}

static void defaults(void)
{
    memset(&saved,0,sizeof(saved));strcpy(saved.vid,"old");strcpy(saved.mqtt_host,"old.example");saved.mqtt_port=1883U;
    tick+=100U;MQTT_App_Init(saved.vid);applies=commits=0U;save_ok=1U;
}
static void schedule(const char* id)
{
    assert(MQTT_App_ScheduleMqttConfigTrial(id,"next","tls.example",8883U,"user","secret",1U));
    MQTT_App_ResponseConfirmed("other",202U);MQTT_App_Task();assert(!applies);
    MQTT_App_ResponseConfirmed(id,202U);MQTT_App_Task();assert(applies==1U && applied_tls);
}
int main(void)
{
    defaults();schedule("tls-change");
    unrelated_pub++;tick++;MQTT_App_Task();assert(!commits); /* Manifest/other PUBACK cannot commit configuration. */
    g_mqtt_app_last_hb_ok_tick++;MQTT_App_Task();assert(commits==1U && saved.flags==BOARD_CONFIG_FLAG_MQTT_TLS);
    defaults();schedule("tls-timeout");tick+=60000U;MQTT_App_Task();
    assert(applies==2U && !commits && !applied_tls && !strcmp(applied_host,"old.example") && !strcmp(g_mqtt_app_device_id,"old"));
    defaults();schedule("save-failed");save_ok=0U;g_mqtt_app_last_hb_ok_tick++;MQTT_App_Task();
    assert(applies==2U && !commits && !strcmp(applied_host,"old.example"));
    defaults();assert(MQTT_App_ScheduleMqttConfigTrial("lost-response",NULL,"tls.example",8883U,"user","secret",1U));
    tick+=8000U;MQTT_App_Task();assert(!applies);
    puts("MQTT trial: matching response, confirmed heartbeat, timeout and save-failure rollback passed");return 0;
}

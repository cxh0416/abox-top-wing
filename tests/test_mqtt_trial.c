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
static unsigned applies, commits, writes;
static uint8_t fail_candidate, fail_restore, applied_tls=1U, ready=1U, connected=1U, ota_busy;
static uint8_t queue_ok=1U;
static uint16_t result_code;
static BoardConfigStore_t flash;
static char applied_host[64];
uint32_t HAL_GetTick(void) { return tick; }
void NVIC_SystemReset(void) { assert(!"unexpected reset"); }
uint8_t BootState_Load(BootStateRecord_t* state) { (void)state; return 0U; }
uint8_t BootState_Confirm(void) { return 1U; }
void AppLog_Printf(int level,const char* tag,const char* format,...) { (void)level;(void)tag;(void)format; }
const BoardConfigStore_t* BoardConfig_Get(void) { return &saved; }
uint8_t BoardConfig_Save(const BoardConfigStore_t* next)
{
    ++writes; flash=*next; /* A failed write may already have changed Flash. */
    if ((!strcmp(next->mqtt_host,"tls.example") && fail_candidate) ||
        (!strcmp(next->mqtt_host,"old.example") && fail_restore)) return 0U;
    saved=*next; ++commits; return 1U;
}
uint8_t MqttRuntime_ApplyEndpoint(const char* host,uint16_t port,const char* user,const char* password,uint8_t tls)
{ (void)port;(void)user;(void)password;assert(tls);++applies;strcpy(applied_host,host);applied_tls=tls;ready=connected=0U;return 1U; }
uint8_t MqttRuntime_IsTlsActive(void) { return ready && applied_tls; }
uint8_t MQTT_Core_IsConnected(void) { return connected; }
uint8_t MQTT_Core_IsReady(void) { return ready; }
uint8_t OtaService_IsBusy(void) { return ota_busy; }
uint8_t MQTT_App_QueueMqttTrialResponse(const char* id,uint16_t code)
{ assert(id && *id); if (!queue_ok) return 0U; result_code=code; return 1U; }
uint8_t MQTT_Core_IsPublishActive(void) { return 0U; }
uint8_t MQTT_Core_CanRunBackgroundAt(void) { return 1U; }
uint32_t MQTT_Core_GetPubOkSeq(void) { return unrelated_pub; }
void MQTT_Core_SetDeviceId(const char* id) { (void)id; }
void MQTT_Core_SetTopics(const char* const* topics,uint8_t count,const char* heartbeat)
{ assert(count==4U && topics[3] && heartbeat); }
void WingController_Task(uint32_t now) { (void)now; }
uint8_t MQTT_V4_ValidDeviceId(const char* id) { return id && *id && strlen(id)<=32U && !strchr(id,'/'); }
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

static void step(unsigned count) { while(count--) { ++tick; MQTT_App_Task(); } }
static void defaults(void)
{
    assert(!MQTT_App_IsMqttConfigTrialActive());
    memset(&saved,0,sizeof(saved));strcpy(saved.vid,"old");strcpy(saved.mqtt_host,"old.example");saved.mqtt_port=8883U;
    strcpy(saved.mqtt_username,"user");strcpy(saved.mqtt_password,"secret");saved.flags=BOARD_CONFIG_FLAG_MQTT_TLS;flash=saved;
    tick+=100U;MQTT_App_Init(saved.vid);applies=commits=writes=0U;fail_candidate=fail_restore=0U;
    applied_tls=ready=connected=queue_ok=1U;result_code=0;
}
static void schedule(const char* id)
{
    assert(MQTT_App_ScheduleMqttConfigTrial(id,"old","tls.example",8883U,"user","secret",1U));
    assert(MQTT_App_MqttTrialReply(id,"old","tls.example",8883U,"user","secret")==202U);
    assert(MQTT_App_MqttTrialReply(id,"old","tls.example",8883U,"user","changed")==4006U);
    MQTT_App_ResponseConfirmed("other",202U);step(1);assert(!applies);
    MQTT_App_ResponseConfirmed(id,202U);step(2);assert(applies==1U && applied_tls && !commits);
    connected=1;step(1);assert(!commits);
    ready=1;step(2);assert(!commits); /* subscribed, now waiting for candidate heartbeat */
}
int main(void)
{
    defaults();
    assert(!MQTT_App_ScheduleMqttConfigTrial("plain","old","tls.example",8883U,"user","secret",0U));
    assert(!MQTT_App_ScheduleMqttConfigTrial("identity","invalid/id","tls.example",8883U,"user","secret",1U));
    ota_busy=1;assert(!MQTT_App_ScheduleMqttConfigTrial("ota","old","tls.example",8883U,"user","secret",1U));ota_busy=0;
    schedule("tls-change");
    unrelated_pub++;step(1);assert(!commits); /* An unrelated PUBACK is not proof. */
    queue_ok=0;g_mqtt_app_last_hb_ok_tick++;step(2);assert(commits==1U && !strcmp(saved.mqtt_host,"tls.example"));
    assert(MQTT_App_IsMqttConfigTrialActive() && !result_code); /* Full queue retries result, never writes again. */
    queue_ok=1;step(1);assert(result_code==200U && !MQTT_App_IsMqttConfigTrialActive() && writes==1);
    assert(MQTT_App_MqttTrialReply("tls-change","old","tls.example",8883U,"user","secret")==200U);

    defaults();
    assert(MQTT_App_ScheduleMqttConfigTrial("rename","next","tls.example",8883U,"user","secret",1U));
    MQTT_App_ResponseConfirmed("rename",202U);step(2);connected=ready=1;step(3);
    g_mqtt_app_last_hb_ok_tick++;step(2);
    assert(result_code==200U && !strcmp(saved.vid,"next") && !strcmp(g_mqtt_app_device_id,"next"));
    defaults();schedule("tls-timeout");tick+=60000U;step(1);
    assert(applies==2U && !writes && applied_tls && !strcmp(applied_host,"old.example") && !result_code);
    connected=ready=1;step(1);assert(result_code==500U && !MQTT_App_IsMqttConfigTrialActive());

    defaults();schedule("save-failed");fail_candidate=1;g_mqtt_app_last_hb_ok_tick++;step(3);
    assert(writes==2U && !strcmp(flash.mqtt_host,"old.example") && applies==2U && !result_code);
    connected=ready=1;step(1);assert(result_code==500U && !MQTT_App_IsMqttConfigTrialActive());

    defaults();assert(MQTT_App_ScheduleMqttConfigTrial("lost-response","old","tls.example",8883U,"user","secret",1U));
    tick+=8000U;step(1);assert(!applies && !writes && !result_code && !MQTT_App_IsMqttConfigTrialActive());

    defaults();schedule("restore-failed");fail_candidate=fail_restore=1;g_mqtt_app_last_hb_ok_tick++;step(4);
    assert(writes==2U && !result_code && MQTT_App_IsMqttConfigTrialActive());
    assert(!MQTT_App_ScheduleMqttConfigTrial("blocked","old","tls.example",8883U,"user","secret",1U));
    puts("Shared MQTT trial: TLS-only, matching acceptance, heartbeat proof, readback, restore and failure latch passed");return 0;
}

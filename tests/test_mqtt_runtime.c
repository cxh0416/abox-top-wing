#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mqtt_runtime.h"
#include "mqtt_core.h"
#include "board_config.h"

static uint32_t tick;
static uint8_t ota_busy;
static uint8_t modem_ready;
static ABoxMqttRuntime* current;
static Ec800AtCore_t modem;
static unsigned initialization, polls, restores;
static BoardConfigStore_t saved;
uint32_t HAL_GetTick(void) { return tick; }
void AppLog_Printf(int level,const char* tag,const char* format,...) { (void)level;(void)tag;(void)format; }
const BoardConfigStore_t* BoardConfig_Get(void) { return &saved; }
uint8_t BoardConfig_HasMqttCredentials(void) { return 1U; }
uint8_t OtaService_IsProvisioned(void) { return 1U; }
uint8_t OtaService_IsBusy(void) { return ota_busy; }
uint8_t TimeBase_IsValid(void) { return 1U; }
void TimeBase_UpdateFromCCLK(const char* text) { (void)text; }
Ec800AtCore_t* MQTT_Core_AtCore(void) { return &modem; }
uint8_t MQTT_Core_TakeModemReady(void) { uint8_t value=modem_ready;modem_ready=0U;return value; }
void MQTT_Core_SetPaused(uint8_t value) { (void)value; }
void MQTT_Core_SetSecurityReady(uint8_t value) { (void)value; }
uint8_t MQTT_Core_ApplyRuntimeConfig(const ABoxMqttConfig* config) { (void)config;return 1U; }
void MQTT_Core_TransportTask(void) {}
void MQTT_Core_Reset(void) {}
void MQTT_Core_ClearBroker(void) {}
int MQTT_Core_Stop(uint32_t now) { (void)now;return 1; }
uint8_t MQTT_Core_IsReady(void) { return 1U; }
uint8_t MQTT_Core_IsConnected(void) { return 1U; }

void ABoxMqttRuntimeOptions_StandardEc800(ABoxMqttRuntimeOptions* options,uint32_t owner,uint8_t profile)
{ memset(options,0,sizeof(*options));options->owner=owner;options->tls_profile_id=profile; }
int ABoxMqttRuntime_Init(ABoxMqttRuntime* runtime,ABoxEc800At* at,const ABoxMqttRuntimePort* port,
    const ABoxMqttRuntimeOptions* options,const ABoxMqttConfig* config,uint32_t now)
{
    (void)now;assert(at==&modem && port->mqtt_stop && port->mqtt_task && port->ca_ready && port->observe_modem_line);
    assert(options->query_clock_mode && options->probe_tls_capability && config->tls_enabled);
    current=runtime;runtime->state=ABOX_MQTT_RUNTIME_READY;runtime->active_tls=1U;++initialization;return 1;
}
int ABoxMqttRuntime_Apply(ABoxMqttRuntime* runtime,const ABoxMqttConfig* config,uint32_t now)
{ (void)now;assert(config->tls_enabled);runtime->state=ABOX_MQTT_RUNTIME_READY;runtime->active_tls=1U;++restores;return 1; }
int ABoxMqttRuntime_Restore(ABoxMqttRuntime* runtime,const ABoxMqttConfig* config,uint32_t now)
{
    if(runtime->state==ABOX_MQTT_RUNTIME_BIND)return 0;
    return ABoxMqttRuntime_Apply(runtime,config,now);
}
int ABoxMqttRuntime_StopFirst(ABoxMqttRuntime* runtime,uint32_t now)
{
    (void)now;
    if(runtime->state==ABOX_MQTT_RUNTIME_UNENROLLED)return 1;
    runtime->state=ABOX_MQTT_RUNTIME_STOP_WAIT_DRAIN;return 0;
}
void ABoxMqttRuntime_Poll(ABoxMqttRuntime* runtime,uint32_t now)
{
    (void)now;++polls;
    if(runtime->state==ABOX_MQTT_RUNTIME_STOP_WAIT_DRAIN)runtime->state=ABOX_MQTT_RUNTIME_UNENROLLED;
}
void ABoxMqttRuntime_OnModemReset(ABoxMqttRuntime* runtime,uint32_t now) { (void)runtime;(void)now;assert(!"RDY must be handled once by Platform"); }
ABoxMqttRuntimeState ABoxMqttRuntime_GetState(const ABoxMqttRuntime* runtime) { return runtime->state; }
uint8_t ABoxMqttRuntime_IsTlsActive(const ABoxMqttRuntime* runtime) { return runtime->active_tls; }
uint8_t ABoxMqttRuntime_IsReady(const ABoxMqttRuntime* runtime) { return runtime->state==ABOX_MQTT_RUNTIME_READY; }
uint8_t ABoxMqttRuntime_NeedsModemRecovery(const ABoxMqttRuntime* runtime) { (void)runtime;return 0U; }
int ABoxMqttRuntime_RecoveryComplete(ABoxMqttRuntime* runtime) { (void)runtime;return 0; }

int main(void)
{
    strcpy(saved.mqtt_host,"tls.example");saved.mqtt_port=8883U;saved.flags=BOARD_CONFIG_FLAG_MQTT_TLS;
    assert(MqttRuntime_Init() && initialization==1U && !polls);
    modem_ready=1U;
    ota_busy=1U;assert(MqttRuntime_StopFirst(tick)==0);MqttRuntime_Task();
    assert(polls==1U && current->state==ABOX_MQTT_RUNTIME_UNENROLLED && !restores);
    assert(MqttRuntime_StopFirst(tick)==1);ota_busy=0U;MqttRuntime_Task();
    assert(restores==1U && MqttRuntime_IsTlsActive());
    current->state=ABOX_MQTT_RUNTIME_BIND;
    assert(MqttRuntime_ApplyEndpoint("rollback.example",8883U,"user","secret",1U));
    MqttRuntime_Task();assert(current->state==ABOX_MQTT_RUNTIME_UNENROLLED);
    MqttRuntime_Task();assert(restores==2U && MqttRuntime_IsTlsActive());
    puts("Runtime adapter: initialize before provisioning, progress stop during OTA, resume TLS and rollback during bind passed");return 0;
}

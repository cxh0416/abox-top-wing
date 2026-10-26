/* Exercise the product lifecycle adapter against the actual Platform runtime. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "mqtt_runtime.h"
#include "mqtt_core.h"
#include "board_config.h"
#include "app_log.h"

static Ec800AtCore_t at;
static BoardConfigStore_t saved;
static uint32_t now;
static uint8_t busy, ready_event, supported = 1U;
static uint8_t saved_available;
static unsigned resets, transport_polls;
static char command[192], endpoint[64];
static uint8_t applied_tls;
const ABoxMqttRuntime* MqttRuntime_TestState(void);
uint32_t HAL_GetTick(void) { return now; }
void AppLog_Printf(AppLogLevel_t l,const char* t,const char* f,...) { (void)l;(void)t;(void)f; }
const BoardConfigStore_t* BoardConfig_Get(void) { return &saved; }
uint8_t BoardConfig_HasMqttCredentials(void) { return saved_available; }
void MQTT_Core_ClearBroker(void) {}
uint8_t OtaService_IsProvisioned(void) { return 1U; }
uint8_t OtaService_IsBusy(void) { return busy; }
uint8_t TimeBase_IsValid(void) { return 1U; }
void TimeBase_UpdateFromCCLK(const char* line) { (void)line; }
Ec800AtCore_t* MQTT_Core_AtCore(void) { return &at; }
uint8_t MQTT_Core_TakeModemReady(void) { uint8_t r=ready_event;ready_event=0U;return r; }
void MQTT_Core_SetPaused(uint8_t value) { (void)value; }
void MQTT_Core_SetSecurityReady(uint8_t value) { (void)value; }
uint8_t MQTT_Core_ApplyRuntimeConfig(const ABoxMqttConfig* config)
{ strcpy(endpoint,config->host);applied_tls=config->tls_enabled;return 1U; }
void MQTT_Core_TransportTask(void) { ++transport_polls; }
void MQTT_Core_Reset(void) { ++resets;ready_event=1U; }
int MQTT_Core_Stop(uint32_t tick) { (void)tick;return 1; }
uint8_t MQTT_Core_IsReady(void) { return 1U; }
uint8_t MQTT_Core_IsConnected(void) { return 1U; }
static uint32_t clock_tick(void* context) { (void)context;return now; }
static int write_at(void* context,const uint8_t* data,uint16_t size,uint32_t timeout)
{ (void)context;(void)timeout;assert(size<sizeof(command));memcpy(command,data,size);command[size]=0;return 1; }
static void step(void)
{
    ++now;MqttRuntime_Task();ABoxEc800At_Task(&at);
    if(at.active_valid)
    {
        const char* reply="OK\r\n";
        if(strstr(command,"QSSLCFG=?"))
            reply=supported ? "+QSSLCFG: \"sslversion\",(0-3)\r\nOK\r\n" : "+QSSLCFG: \"sslversion\",(0-1)\r\nOK\r\n";
        ABoxEc800At_Feed(&at,(const uint8_t*)reply,(uint16_t)strlen(reply));
    }
}
static void reach(ABoxMqttRuntimeState state)
{
    for(unsigned i=0;i<200U && MqttRuntime_TestState()->state!=state;++i)step();
    if(MqttRuntime_TestState()->state!=state)
        fprintf(stderr,"want=%u actual=%u tick=%lu command=%s\n",(unsigned)state,(unsigned)MqttRuntime_TestState()->state,(unsigned long)now,command);
    assert(MqttRuntime_TestState()->state==state);
}
int main(void)
{
    ABoxEc800AtPort port={0};port.tick_ms=clock_tick;port.write=write_at;
    strcpy(saved.mqtt_host,"old.example");strcpy(saved.mqtt_username,"user");strcpy(saved.mqtt_password,"secret");saved.mqtt_port=1883U;
    assert(ABoxEc800At_Init(&at,&port) && MqttRuntime_Init());
    MqttRuntime_Task();
    assert(MqttRuntime_TestState()->state==ABOX_MQTT_RUNTIME_FAILED && !endpoint[0]); /* await-enrollment */
    saved_available=1U;assert(MqttRuntime_ApplyCurrentConfig());reach(ABOX_MQTT_RUNTIME_READY);
    unsigned before=transport_polls;step();assert(transport_polls>before);
    busy=1U;assert(MqttRuntime_StopFirst(now)==0);reach(ABOX_MQTT_RUNTIME_UNENROLLED);
    assert(MqttRuntime_StopFirst(now)==1);busy=0U;reach(ABOX_MQTT_RUNTIME_READY);
    assert(!strcmp(endpoint,"old.example") && !applied_tls);
    assert(MqttRuntime_ApplyEndpoint("tls.example",8883U,"user","secret",1U));reach(ABOX_MQTT_RUNTIME_BIND);
    assert(MqttRuntime_ApplyEndpoint(saved.mqtt_host,saved.mqtt_port,"user","secret",0U));reach(ABOX_MQTT_RUNTIME_READY);
    assert(!strcmp(endpoint,"old.example") && !applied_tls);
    ABoxEc800At_OnModemReset(&at);unsigned expected=resets;reach(ABOX_MQTT_RUNTIME_READY);
    for(unsigned i=0;i<5U;++i)step();assert(resets==expected); /* RDY is not recursively re-fed. */
    supported=0U;
    assert(MqttRuntime_ApplyEndpoint("unsupported.example",8883U,"user","secret",1U));reach(ABOX_MQTT_RUNTIME_FAILED);
    assert(MqttRuntime_TestState()->safety_locked);
    assert(MqttRuntime_ApplyEndpoint(saved.mqtt_host,saved.mqtt_port,"user","secret",0U));reach(ABOX_MQTT_RUNTIME_READY);
    assert(!strcmp(endpoint,"old.example") && !applied_tls);
    puts("Platform runtime integration: OTA stop/resume, TLS-bind rollback, single RDY and unsupported-TLS rollback passed");return 0;
}

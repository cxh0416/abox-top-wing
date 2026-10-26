#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mqtt_core.h"
#include "app_log.h"

static uint32_t now;
static unsigned writes, resets;
static uint8_t storage_busy;
uint32_t HAL_GetTick(void) { return now; }
void AppLog_Printf(AppLogLevel_t l,const char* t,const char* f,...) { (void)l;(void)t;(void)f; }
void MQTT_V4_StoreOnModemReset(void) { ++resets; }
uint8_t MQTT_V4_StoreBusy(void) { return storage_busy; }
static uint32_t tick(void* context) { (void)context;return now; }
static int write_at(void* context,const uint8_t* data,uint16_t size,uint32_t timeout)
{ (void)context;(void)timeout;assert(size && data);++writes;return 1; }
static void message(const char* topic,const char* packet) { (void)topic;(void)packet; }
int main(void)
{
    Ec800AtCore_t at;
    ABoxEc800AtPort port={0};port.tick_ms=tick;port.write=write_at;
    MQTT_CoreConfig_t config={0};
    strcpy(config.client_id,"W1");strcpy(config.server_host,"broker.example");config.server_port=1883;
    strcpy(config.sub_topics[0],"/zxwl/abox/W1/request");config.sub_topic_count=1;
    assert(ABoxEc800At_Init(&at,&port));
    MQTT_Core_Init(&config,message);MQTT_Core_SetAtCore(&at);
    assert(MQTT_Core_Bridge()->initialized);
    MQTT_Core_SetSecurityReady(1);MQTT_Core_SetPaused(0);
    assert(MQTT_Core_Bridge()->transport.v4_backoff);
    /* Repeated runtime polling must not restart or bypass the first delay. */
    for(unsigned i=0;i<20;++i){MQTT_Core_TransportTask();ABoxEc800At_Task(&at);}
    assert(!writes);
    now=4001;
    storage_busy=1;
    assert(MQTT_Core_Stop(now)==0);
    for(unsigned i=0;i<5;++i){MQTT_Core_TransportTask();ABoxEc800At_Task(&at);}
    assert(!writes);
    storage_busy=0;
    for(unsigned i=0;i<5;++i){MQTT_Core_TransportTask();ABoxEc800At_Task(&at);}
    assert(writes==1);
    ABoxEc800At_OnModemReset(&at);
    assert(resets==1 && MQTT_Core_TakeModemReady() && !MQTT_Core_TakeModemReady());
    puts("MQTT core: V4 initial backoff, runtime polling and storage reset notification passed");
    return 0;
}

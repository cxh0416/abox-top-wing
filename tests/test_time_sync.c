#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app_log.h"
#include "time_base.h"
#include "time_sync.h"
#include "cJSON.h"
#include "abox_mqtt_v4.h"

static uint64_t mono;
static uint8_t tls, ready=1U;
static uint32_t generation=1U;
static char last_id[65];
static unsigned sends;
static uint64_t last_timestamp;
void TimeSync_TestReset(void);
uint32_t HAL_GetTick(void) { return (uint32_t)mono; }
uint64_t ABox_PortGetMonotonicMs(void) { return mono; }
void AppLog_Printf(AppLogLevel_t level,const char* tag,const char* fmt,...)
{ (void)level;(void)tag;(void)fmt; }
uint8_t MQTT_Core_IsReady(void) { return ready; }
uint8_t MqttRuntime_IsTlsActive(void) { return tls; }
uint32_t MQTT_Core_GetReadySeq(void) { return generation; }
uint8_t MQTT_Core_IsPublishActive(void) { return 0U; }
uint8_t MQTT_Core_PublishEx(const char* topic,const char* payload,uint8_t qos,uint8_t retain)
{
    cJSON* root=cJSON_Parse(payload);
    assert(strstr(topic,"/internal/time/request") && qos==1 && !retain && root);
    cJSON* data=cJSON_GetObjectItemCaseSensitive(root,"data");
    last_timestamp=(uint64_t)cJSON_GetObjectItemCaseSensitive(root,"timestamp")->valuedouble;
    strcpy(last_id,cJSON_GetObjectItemCaseSensitive(data,"requestId")->valuestring);
    ++sends; cJSON_Delete(root); return 1U;
}

static void reply(void)
{
    char text[512], timestamp[21];
    ABoxMqttV4_FormatInteger(timestamp,sizeof(timestamp),1800000000000ULL+mono);
    snprintf(text,sizeof(text),"{\"version\":\"2.0\",\"timestamp\":1800000000000,\"data\":{\"requestId\":\"%s\",\"code\":200,\"result\":{\"serverUtcMs\":%s,\"errorMs\":10}}}",last_id,timestamp);
    assert(TimeSync_OnMessage(TimeSync_ResponseTopic(),text));
}

int main(void)
{
    TimeBase_Init(); TimeSync_Init("W1");
    char formatted[21];
    assert(TimeBase_FormatUtcMs(formatted,sizeof(formatted)) && !strcmp(formatted,"0"));
    TimeBase_UpdateFromCCLK("+CCLK: \"26/10/26,10:00:00+32\""); assert(!TimeBase_IsValid());
    TimeBase_UpdateFromCCLK("+CTZU: 3");
    TimeBase_UpdateFromCCLK("+CCLK: \"26/10/26,10:00:00+32\"");
    assert(TimeBase_IsValid() && !TimeBase_IsTrusted());
    assert(TimeBase_GetReportedMs()==0U);
    assert(TimeBase_CheckExpiry(TimeBase_GetMs()+5000U)==4003U);
    TimeSync_Task(); assert(!sends);
    tls=1U;
    for(unsigned i=0;i<3U;i++) { TimeSync_Task(); assert(last_timestamp==0U); mono+=100U; reply(); mono+=1000U; }
    TimeSync_Task();
    assert(sends==3U && TimeBase_IsTrusted());
    assert(TimeBase_GetReportedMs() == TimeBase_GetMs());
    assert(TimeBase_CheckExpiry(TimeBase_GetMs()+5000U)==200U);
    mono+=7200001ULL; assert(!TimeBase_IsTrusted() && TimeBase_CheckExpiry(TimeBase_GetMs()+5000U)==4003U);
    TimeBase_Init();TimeSync_Init("W1");++generation;TimeSync_Task();
    char stale[65];strcpy(stale,last_id);
    tls=0;mono+=10U;reply();assert(!TimeBase_IsTrusted());TimeSync_Task();
    tls=1;++generation;TimeSync_Task();assert(strcmp(stale,last_id));
    strcpy(stale,last_id);
    TimeSync_Init("W2");assert(strstr(TimeSync_ResponseTopic(),"/W2/"));
    reply();TimeSync_Task();assert(strcmp(stale,last_id) && !TimeBase_IsTrusted());
    ready=0;TimeSync_Task();reply();assert(!TimeBase_IsTrusted());
    ready=1;++generation;
    for(unsigned i=0;i<3U;++i){TimeSync_Task();mono+=100U;reply();mono+=1000U;}
    TimeSync_Task();assert(TimeBase_IsTrusted());
    /* Restart may reuse IDs; it must not advertise untrusted bootstrap time. */
    TimeSync_TestReset(); TimeBase_Init(); mono=0; tls=ready=1U; generation=1U;
    TimeBase_UpdateFromCCLK("+CTZU: 3");
    TimeBase_UpdateFromCCLK("+CCLK: \"70/01/01,08:00:00+32\"");
    assert(TimeBase_IsValid() && TimeBase_GetReportedMs()==0U);
    TimeSync_Init("W1"); mono=25000U; TimeSync_Task(); assert(last_timestamp==0U);
    strcpy(stale,last_id);
    TimeSync_TestReset(); TimeBase_Init(); mono=0; generation=1U;
    TimeBase_UpdateFromCCLK("+CTZU: 3");
    TimeBase_UpdateFromCCLK("+CCLK: \"70/01/01,08:00:00+32\"");
    TimeSync_Init("W1"); mono=25000U; TimeSync_Task();
    assert(!strcmp(stale,last_id) && last_timestamp==0U);
    puts("Time adapter: CCLK bootstrap, TLS-only sync, three samples and stale-time rejection passed");
    return 0;
}

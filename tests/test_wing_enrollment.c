#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wing_enrollment.h"
#include "abox_enrollment_service.h"
#include "abox_enrollment_identity.h"
#include "abox_stm32f105_ec800_identity.h"
#include "board_config.h"
#include "mqtt_core.h"
#include "app_log.h"

static Ec800AtCore_t at;
static BoardConfigStore_t config;
static uint32_t tick;
static uint8_t saved,connected,subscribed,save_ok=1;
static unsigned applied,commits,clears;
static char current_id[33];
uint32_t g_mqtt_app_last_hb_ok_tick;
ABoxEnrollmentService* WingEnrollment_TestService(void);
uint32_t HAL_GetTick(void) { return tick; }
static uint32_t at_tick(void* user) { (void)user;return tick; }
static int at_write(void* user,const uint8_t* data,uint16_t length,uint32_t timeout)
{ (void)user;(void)data;(void)length;(void)timeout;assert(!"HTTP must not start in this fixture");return 0; }
void AppLog_Printf(AppLogLevel_t l,const char* t,const char* f,...) { (void)l;(void)t;(void)f; }
uint8_t BoardConfig_HasMqttCredentials(void) { return saved; }
const BoardConfigStore_t* BoardConfig_Get(void) { return &config; }
uint8_t BoardConfig_SetMqtt(const char* id,const char* host,uint16_t port,const char* user,const char* password,uint8_t tls)
{ (void)host;(void)port;(void)user;(void)password;(void)tls;++commits;if(save_ok){saved=1;strcpy(config.vid,id);}return save_ok; }
uint8_t EC_Power_IsOnDone(void) { return 1; }
uint8_t OtaService_IsProvisioned(void) { return 0; }
uint8_t OtaService_IsBusy(void) { return 0; }
uint8_t MQTT_V4_StoreBusy(void) { return 0; }
Ec800AtCore_t* MQTT_Core_AtCore(void) { return &at; }
void MQTT_Core_SetPaused(uint8_t value) { (void)value; }
void MQTT_Core_RequestIccid(void) {}
uint8_t MQTT_Core_IsIccidValid(void) { return 1; }
const char* MQTT_Core_GetIccid(void) { return "89860000000000000000"; }
const char* TopFlyingWing_AppFwVersion(void) { return "top-flying-wing-1.1.0"; }
int ABoxStm32F105Ec800Identity_Build(ABoxEnrollmentIdentity* out,ABoxEnrollmentIdentityStorage* storage,
    const char* iccid,const char* id,uint8_t assigned,const char* prefix,const char* hardware,const char* app)
{ return ABoxEnrollmentIdentity_Build(out,storage,1,2,3,iccid,id,assigned,prefix,hardware,"abox-boot-2.3.0",app); }
void MQTT_App_Init(const char* id) { strcpy(current_id,id); }
void MQTT_Core_SetDeviceId(const char* id) { assert(!strcmp(id,current_id)); }
const char* const* MQTT_App_GetSubTopics(void) { static const char* topics[]={"test"};return topics; }
uint8_t MQTT_App_GetSubTopicCount(void) { return 1; }
const char* MQTT_App_GetHeartbeatTopic(void) { return "test/hb"; }
void MQTT_Core_SetTopics(const char* const* topics,uint8_t count,const char* hb) { (void)topics;assert(count==1 && hb); }
uint8_t MqttRuntime_ApplyEndpoint(const char* host,uint16_t port,const char* user,const char* password,uint8_t tls)
{ assert(host && port && user && password && tls);++applied;return 1; }
int MqttRuntime_ClearEnrollmentCandidate(void) { ++clears;connected=subscribed=0;return 1; }
uint8_t MQTT_Core_IsConnected(void) { return connected; }
uint8_t MQTT_Core_IsReady(void) { return subscribed; }

static void boot(void)
{
    ABoxEc800AtPort port={0};port.tick_ms=at_tick;port.write=at_write;
    saved=connected=subscribed=0;tick=0;applied=commits=clears=0;g_mqtt_app_last_hb_ok_tick=0;
    memset(&config,0,sizeof(config));strcpy(config.vid,"V000001");
    assert(ABoxEc800At_Init(&at,&port) && WingEnrollment_Init(&at));
}
static void approve(void)
{
    ABoxEnrollmentService* s=WingEnrollment_TestService();
    const char* packet="{\"status\":\"approved\",\"vid\":\"TFW-APPROVED\",\"mqtt\":{\"host\":\"broker.example\",\"port\":19688,\"username\":\"abox_user\",\"password\":\"demo-enroll-secret\",\"tlsEnabled\":true,\"tlsProfileId\":\"0\"}}";
    strcpy(s->enrollment.buffers.response,packet);s->enrollment.state=ABOX_ENROLLMENT_HTTP;s->enrollment.have_request=1;
    ABoxEnrollment_OnHttp(&s->enrollment,200,strlen(packet),1,tick);
    assert(s->enrollment.state==ABOX_ENROLLMENT_TRIAL);
}
static void step(void) { ++tick;WingEnrollment_Task(); }
int main(int argc, char** argv)
{
    if (argc == 2 && !strcmp(argv[1], "--describe"))
    {
        ABoxEnrollmentIdentity id;
        boot();
        ABoxEnrollmentService* s = WingEnrollment_TestService();
        assert(s->port.identity(s->port.user, &id));
        printf("{\"hardwareContract\":\"%s\",\"tlsProfileId\":%u,\"requirePsk\":%u}\n",
            id.hardware_contract, s->enrollment.expected_tls_profile_id,
            s->enrollment.require_psk);
        return 0;
    }
    boot();assert(!strncmp(WingEnrollment_DeviceId(),"TFW-",4));
    for(unsigned i=0;i<8;++i)step();assert(!applied && !commits);
    approve();step();assert(applied==1 && !commits);
    connected=1;step();subscribed=1;step();step();
    for(unsigned i=0;i<8;++i)step();assert(!commits);
    g_mqtt_app_last_hb_ok_tick=++tick;step();step();assert(saved && commits==1);
    assert(WingEnrollment_TestService()->enrollment.state==ABOX_ENROLLMENT_COMPLETE);
    assert(!strcmp(WingEnrollment_DeviceId(),"TFW-APPROVED"));
    boot();approve();step();tick=120001U;step();assert(clears==1 && !commits && !saved);
    assert(!strncmp(current_id,"TFW-",4) && !connected);
    boot();approve();step();connected=subscribed=1;step();step();step();
    save_ok=0;g_mqtt_app_last_hb_ok_tick=++tick;step();step();step();
    assert(commits==1 && !saved && clears==1 && !connected);
    puts("Enrollment: no default MQTT before approval, first trial proof, saved/readback boundary and rollback passed");
    return 0;
}

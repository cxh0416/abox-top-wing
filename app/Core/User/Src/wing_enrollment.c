#include "wing_enrollment.h"
#include <string.h>
#include "abox_enrollment_service.h"
#include "abox_enrollment_ca_pem.h"
#include "abox_stm32f105_ec800_identity.h"
#include "abox_mqtt_trial_executor.h"
#include "app_version.h"
#include "app_log.h"
#include "board_config.h"
#include "ec_power.h"
#include "mqtt_app_int.h"
#include "mqtt_core.h"
#include "mqtt_runtime.h"
#include "mqtt_v4_store.h"
#include "ota_service.h"
#include "main.h"

static ABoxEnrollmentService service;
static ABoxEnrollmentIdentityStorage identity_storage;
static ABoxMqttTrial trial;
static ABoxMqttTrialExecutor executor;
static ABoxMqttTrialExecutorAction working;
static uint64_t working_session;
static ABoxMqttConfig candidate;
static char url[192] __attribute__((section(".ota_work"), aligned(4)));
static char body[512] __attribute__((section(".ota_work"), aligned(4)));
static char response[1025] __attribute__((section(".ota_work"), aligned(4)));
static char request_id[48], poll_token[64];
static uint32_t proof_heartbeat;

static uint32_t now(void* user) { (void)user; return HAL_GetTick(); }
static int needed(void* user) { (void)user; return !BoardConfig_HasMqttCredentials(); }
static int ready(void* user)
{
    (void)user;
    if (!EC_Power_IsOnDone() || !OtaService_IsProvisioned() || OtaService_IsBusy() ||
        MQTT_V4_StoreBusy() || ABoxEc800At_IsBusy(MQTT_Core_AtCore())) return 0;
    if (!MQTT_Core_IsIccidValid()) { MQTT_Core_RequestIccid(); return 0; }
    return 1;
}
static void pause_mqtt(void* user, uint8_t value) { (void)user; MQTT_Core_SetPaused(value); }
static int identity(void* user, ABoxEnrollmentIdentity* out)
{
    (void)user;
    const BoardConfigStore_t* config = BoardConfig_Get();
    return ABoxStm32F105Ec800Identity_Build(out, &identity_storage, MQTT_Core_GetIccid(),
        config->vid, BoardConfig_HasMqttCredentials(), "TFW-",
        "top_flying_wing_stm32f105_ec800_v1", TopFlyingWing_AppFwVersion());
}
static int start_trial(void* user, const ABoxEnrollmentCredential* c)
{
    (void)user;
    if (!c || c->tls_credentials.mode != ABOX_TLS_AUTH_CA ||
        strlen(c->password) >= sizeof(BoardConfig_Get()->mqtt_password)) return 0;
    candidate = (ABoxMqttConfig){0};
    candidate.host=c->host; candidate.port=c->port; candidate.username=c->username;
    candidate.password=c->password; candidate.tls_enabled=c->tls_enabled;
    candidate.tls_profile_id=c->tls_profile_id;
    APP_LOGI("ENROLL", "approved; starting first MQTT trial");
    return ABoxMqttTrial_StartFirst(&trial, &candidate, 1U, HAL_GetTick());
}
static int result(void* user)
{
    (void)user;
    if (trial.state == ABOX_MQTT_TRIAL_COMMITTED) return 1;
    if (trial.state == ABOX_MQTT_TRIAL_FAILED || trial.state == ABOX_MQTT_TRIAL_RESTORE_FAILED) return 0;
    return -1;
}
static void cancel_trial(void* user) { (void)user; ABoxMqttTrial_Cancel(&trial, HAL_GetTick()); }
const char* WingEnrollment_DeviceId(void)
{ return BoardConfig_HasMqttCredentials() ? BoardConfig_Get()->vid : identity_storage.vid; }

uint8_t WingEnrollment_Init(Ec800AtCore_t* at)
{
    memset(&service,0,sizeof(service));
    memset(url,0,sizeof(url));memset(body,0,sizeof(body));memset(response,0,sizeof(response));
    ABoxEnrollmentIdentity id;
    const ABoxEnrollmentServicePort port = {NULL,now,ready,needed,identity,pause_mqtt,start_trial,result,cancel_trial};
    const ABoxEnrollmentBuffers buffers = {url,body,response,request_id,poll_token,
        sizeof(url),sizeof(body),sizeof(response),sizeof(request_id),sizeof(poll_token)};
    if (!identity(NULL,&id) || !ABoxMqttTrialExecutor_Init(&executor,&trial) ||
        !ABoxEnrollmentService_Init(&service,at,ABOX_EC800_OWNER_PRODUCT_BASE+2U,
            "UFS:enrollment_ca_v1.pem","https://zxwl.ntiov.com:20444",&buffers,0U,&port) ||
        !ABoxEnrollmentEc800Http_SetCaPem(&service.http,abox_enrollment_ca_pem,sizeof(abox_enrollment_ca_pem)-1U))
        return 0U;
    working=ABOX_MQTT_TRIAL_ACTION_NONE;
    return 1U;
}

void WingEnrollment_Task(void)
{
    const ABoxEnrollmentCredential* c = &service.enrollment.credential;
    uint32_t tick = HAL_GetTick();
    ABoxMqttTrial_Poll(&trial,tick);
    const ABoxMqttConfig* queued_config;
    if (ABoxMqttTrialExecutor_HasPending(&executor))
    {
        if (ABoxMqttTrialExecutor_Take(&executor,&working,&working_session,&queued_config) &&
            working==ABOX_MQTT_TRIAL_ACTION_VERIFY) proof_heartbeat=g_mqtt_app_last_hb_ok_tick;
    }
    ABoxMqttTrialEvent event;
    uint8_t have_event=1U;
    switch (working)
    {
    case ABOX_MQTT_TRIAL_ACTION_PREPARE:
        MQTT_App_Init(c->vid); MQTT_Core_SetDeviceId(c->vid);
        MQTT_Core_SetTopics(MQTT_App_GetSubTopics(),MQTT_App_GetSubTopicCount(),MQTT_App_GetHeartbeatTopic());
        event=MqttRuntime_ApplyEndpoint(c->host,c->port,c->username,c->password,c->tls_enabled) ?
            ABOX_MQTT_TRIAL_PREPARED : ABOX_MQTT_TRIAL_ERROR;
        break;
    case ABOX_MQTT_TRIAL_ACTION_CONNECT:
        if (!MQTT_Core_IsConnected()) { have_event=0; break; }
        event=ABOX_MQTT_TRIAL_CONNECTED; break;
    case ABOX_MQTT_TRIAL_ACTION_SUBSCRIBE:
        if (!MQTT_Core_IsReady()) { have_event=0; break; }
        event=ABOX_MQTT_TRIAL_SUBSCRIBED; break;
    case ABOX_MQTT_TRIAL_ACTION_VERIFY:
        if (proof_heartbeat == g_mqtt_app_last_hb_ok_tick) { have_event=0; break; }
        event=ABOX_MQTT_TRIAL_PROVED; break;
    case ABOX_MQTT_TRIAL_ACTION_COMMIT:
        event=BoardConfig_SetMqtt(c->vid,c->host,c->port,c->username,c->password,c->tls_enabled) ?
            ABOX_MQTT_TRIAL_SAVED : ABOX_MQTT_TRIAL_ERROR;
        if (event==ABOX_MQTT_TRIAL_SAVED) APP_LOGI("ENROLL","credentials committed and read back");
        break;
    case ABOX_MQTT_TRIAL_ACTION_RESTORE:
    {
        int stopped=MqttRuntime_ClearEnrollmentCandidate();
        if (!stopped) { have_event=0; break; }
        if (stopped>0) { MQTT_App_Init(identity_storage.vid);MQTT_Core_SetDeviceId(identity_storage.vid); }
        event=stopped>0 ? ABOX_MQTT_TRIAL_RESTORED : ABOX_MQTT_TRIAL_ERROR; break;
    }
    default: have_event=0; break;
    }
    if (have_event)
    {
        working=ABOX_MQTT_TRIAL_ACTION_NONE;
        ABoxMqttTrial_Event(&trial,working_session,event,trial.proof,tick);
    }
    ABoxEnrollmentService_Poll(&service);
}
void WingEnrollment_AfterModemReset(void) { ABoxEnrollmentService_AfterModemReset(&service); }
#ifdef TOP_WING_HOST_TEST
ABoxEnrollmentService* WingEnrollment_TestService(void) { return &service; }
#endif

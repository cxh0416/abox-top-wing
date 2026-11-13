#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ota_service.h"
#include "mqtt_v4_store.h"
#include "abox_boot_v2_app.h"
#include "abox_boot_v2.h"
#include "app_log.h"
#include "boot_state.h"

static uint8_t prepare_allowed = 1U;
static uint8_t boot_matches = 1U;
uint8_t WingOta_TestBootMatches(void) { return boot_matches; }
uint8_t WingOta_TestVersionMatches(const char *version, uint8_t gd32);
uint8_t WingOta_TestDescriptorMatches(const ABoxBootV2Descriptor *, uint8_t gd32);
uint8_t OTA_Info_PrepareExternalWrite(void) { return prepare_allowed; }
static BootStateRecord_t boot_state;
uint8_t BootState_Load(BootStateRecord_t *out) { *out = boot_state; return 1U; }
uint8_t __app_image_end;
static int busy, starts, provisions, polls, credentials, mqtt_ready, config_trial, provisioned=1;
static const char *phase="IDLE";
static const ABoxBootV2AppPort* captured;
uint32_t HAL_GetTick(void) { return 1U; }
const char* TopFlyingWing_AppFwVersion(void) { return "top-flying-wing-1.1.0"; }
void AppLog_Printf(AppLogLevel_t l, const char* t, const char* f, ...) { (void)l;(void)t;(void)f; }
uint8_t MQTT_Core_ClientIndex(void) { return 0U; }
uint8_t MQTT_Core_IsReady(void) { return (uint8_t)mqtt_ready; }
uint8_t MQTT_App_IsMqttConfigTrialActive(void) { return (uint8_t)config_trial; }
uint8_t BoardConfig_HasMqttCredentials(void) { return (uint8_t)credentials; }
int MqttRuntime_StopFirst(uint32_t n) { (void)n;return 1; }
int MQTT_Core_BorrowWorkspace(void) { return 1; }
void MQTT_Core_ReturnWorkspace(void) {}
void MQTT_Core_SetPaused(uint8_t p) { (void)p; }
int ABoxBootV2App_Init(const ABoxBootV2AppPort* port, const ABoxBootV2AppConfig* config)
{ (void)config;captured=port;return 1; }
int ABoxBootV2App_IsBusy(void) { return busy; }
int ABoxBootV2App_Start(const ABoxBootV2AppRequest* request)
{ (void)request;++starts;busy=1;return 1; }
int ABoxBootV2App_BeginProvisioning(void) { ++provisions;busy=1;return 1; }
void ABoxBootV2App_Task(void) { ++polls; }
int ABoxBootV2App_IsProvisioned(void) { return provisioned; }
int ABoxBootV2App_IsReady(void) { return 1; }
const char* ABoxBootV2App_Phase(void) { return phase; }
const char* ABoxBootV2App_TargetVersion(void) { return ""; }
uint32_t ABoxBootV2App_LastError(void) { return 0; }
int ABoxBootV2App_TakeInstallReady(void) { return 0; }
int ABoxBootV2App_TakeFailure(uint32_t* error) { (void)error;return 0; }

int main(void)
{
    Ec800AtCore_t at={0};
    OtaServiceRequest_t request={0};
    ABoxBootV2Descriptor descriptor = {0};
    strcpy(descriptor.product_id, "abox_gd32f105");
    assert(WingOta_TestDescriptorMatches(&descriptor, 1U));
    assert(!WingOta_TestDescriptorMatches(&descriptor, 0U));
    strcpy(descriptor.product_id, "abox_stm32f105");
    assert(WingOta_TestDescriptorMatches(&descriptor, 0U));
    assert(!WingOta_TestDescriptorMatches(&descriptor, 1U));
    strcpy(request.version, "top-flying-wing-1.2.0");
    assert(WingOta_TestVersionMatches("top-flying-wing-1.2.0-gd", 1U));
    assert(!WingOta_TestVersionMatches("top-flying-wing-1.2.0", 1U));
    assert(!WingOta_TestVersionMatches("top-flying-wing-1.2.0-gd", 0U));
    assert(!WingOta_TestVersionMatches("top-flying-wing-1.2.0-gd-wip", 0U));
    assert(!WingOta_TestVersionMatches("top-flying-wing-x.2.0-gd", 1U));
    assert(WingOta_TestVersionMatches("top-flying-wing-1.2.0", 0U));
    assert(!OtaService_AcquireIdemWorkspace());
    OtaService_Init(&at);
    boot_state.state = BOOT_STATE_TRIAL;
    assert(!OtaService_ConfigWriteAllowed());
    boot_state.state = BOOT_STATE_CONFIRMED;
    boot_state.stable_image_size = APP_MAX_SIZE + 1U;
    assert(!OtaService_ConfigWriteAllowed());
    boot_state.stable_image_size = APP_MAX_SIZE;
    assert(OtaService_ConfigWriteAllowed());
    busy = 1; assert(!OtaService_ConfigWriteAllowed()); busy = 0;
    assert(captured && captured->transfer_buffer==OtaService_IdemWorkspace());
    assert(captured->transfer_buffer_size==MQTT_V4_IDEM_WORKSPACE_SIZE);
    assert(captured->may_start_seed && !captured->may_start_seed(captured->context)); /* empty board */
    assert(OtaService_AcquireIdemWorkspace() && !OtaService_AcquireIdemWorkspace());
    memset(OtaService_IdemWorkspace(),0x5a,MQTT_V4_IDEM_WORKSPACE_SIZE);
    assert(!OtaService_Start(&request) && !OtaService_BeginProvisioning());
    OtaService_Task();assert(!starts && !provisions && !polls);
    OtaService_ReleaseIdemWorkspace();
    credentials=1; mqtt_ready=0;
    assert(!captured->may_start_seed(captured->context));
    OtaService_Task(); assert(polls==1); /* polling continues while seed is deferred */
    phase="ERROR"; OtaService_Task(); assert(polls==2); /* retry is not blocked */
    phase="IDLE"; provisioned=0; OtaService_Task(); assert(polls==3); /* provisioning advances */
    provisioned=1; busy=1; OtaService_Task(); assert(polls==4); /* active work advances */
    busy=0; mqtt_ready=1; config_trial=1;
    assert(!captured->may_start_seed(captured->context)); /* maintenance trial */
    config_trial=0; assert(captured->may_start_seed(captured->context));
    boot_matches=0; assert(!captured->may_start_seed(captured->context));
    assert(!OtaService_IsReady() && !OtaService_Start(&request)); boot_matches=1;
    OtaService_Task(); assert(polls==5); /* seed can start */
    credentials=0; assert(!captured->may_start_seed(captured->context));
    mqtt_ready=0; OtaService_Task(); assert(polls==6); /* empty board */
    prepare_allowed=0; assert(!OtaService_Start(&request) && !starts);
    prepare_allowed=1;
    assert(OtaService_Start(&request) && starts==1 && !OtaService_AcquireIdemWorkspace());
    OtaService_Task();assert(polls==7);
    busy=0;assert(OtaService_BeginProvisioning() && !OtaService_AcquireIdemWorkspace());
    busy=0;at.quarantined=1;assert(!OtaService_AcquireIdemWorkspace());
    at.quarantined=0;assert(OtaService_AcquireIdemWorkspace());
    for(size_t i=0;i<MQTT_V4_IDEM_WORKSPACE_SIZE;++i)assert(OtaService_IdemWorkspace()[i]==0x5a);
    OtaService_ReleaseIdemWorkspace();
    puts("OTA workspace: exclusive UFS lease, OTA/provisioning deferral and quarantine passed");
    return 0;
}

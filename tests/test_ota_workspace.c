#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ota_service.h"
#include "mqtt_v4_store.h"
#include "abox_boot_v2_app.h"
#include "app_log.h"

uint8_t __app_image_end;
static int busy, starts, provisions, polls;
static const ABoxBootV2AppPort* captured;
uint32_t HAL_GetTick(void) { return 1U; }
const char* TopFlyingWing_AppFwVersion(void) { return "top-flying-wing-1.1.0"; }
void AppLog_Printf(AppLogLevel_t l, const char* t, const char* f, ...) { (void)l;(void)t;(void)f; }
uint8_t MQTT_Core_ClientIndex(void) { return 0U; }
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
int ABoxBootV2App_IsProvisioned(void) { return 1; }
int ABoxBootV2App_IsReady(void) { return 1; }
const char* ABoxBootV2App_Phase(void) { return "IDLE"; }
const char* ABoxBootV2App_TargetVersion(void) { return ""; }
uint32_t ABoxBootV2App_LastError(void) { return 0; }
int ABoxBootV2App_TakeInstallReady(void) { return 0; }
int ABoxBootV2App_TakeFailure(uint32_t* error) { (void)error;return 0; }

int main(void)
{
    Ec800AtCore_t at={0};
    OtaServiceRequest_t request={0};
    assert(!OtaService_AcquireIdemWorkspace());
    OtaService_Init(&at);
    assert(captured && captured->transfer_buffer==OtaService_IdemWorkspace());
    assert(captured->transfer_buffer_size==MQTT_V4_IDEM_WORKSPACE_SIZE);
    assert(OtaService_AcquireIdemWorkspace() && !OtaService_AcquireIdemWorkspace());
    memset(OtaService_IdemWorkspace(),0x5a,MQTT_V4_IDEM_WORKSPACE_SIZE);
    assert(!OtaService_Start(&request) && !OtaService_BeginProvisioning());
    OtaService_Task();assert(!starts && !provisions && !polls);
    OtaService_ReleaseIdemWorkspace();
    assert(OtaService_Start(&request) && starts==1 && !OtaService_AcquireIdemWorkspace());
    OtaService_Task();assert(polls==1);
    busy=0;assert(OtaService_BeginProvisioning() && !OtaService_AcquireIdemWorkspace());
    busy=0;at.quarantined=1;assert(!OtaService_AcquireIdemWorkspace());
    at.quarantined=0;assert(OtaService_AcquireIdemWorkspace());
    for(size_t i=0;i<MQTT_V4_IDEM_WORKSPACE_SIZE;++i)assert(OtaService_IdemWorkspace()[i]==0x5a);
    OtaService_ReleaseIdemWorkspace();
    puts("OTA workspace: exclusive UFS lease, OTA/provisioning deferral and quarantine passed");
    return 0;
}

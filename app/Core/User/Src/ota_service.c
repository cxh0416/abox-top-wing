#include "ota_service.h"
#include <string.h>
#include "abox_boot_v2.h"
#include "abox_boot_v2_app.h"
#include "abox_boot_v2_ca_pem.h"
#include "app_log.h"
#include "app_version.h"
#include "boot_cfg.h"
#include "mqtt_core.h"
#include "mqtt_core_int.h"

typedef struct { Ec800AtCore_t *at; ABoxBootV2AtDoneFn done; void *done_user; ABoxBootV2AtEventFn events; void *events_user; } OtaAdapter_t;
static OtaAdapter_t g_adapter;
static uint32_t tick(void *c){(void)c;return HAL_GetTick();}
static void at_done(Ec800AtCmdResult_t r,void *u){OtaAdapter_t*a=u;ABoxBootV2AtDoneFn d=a->done;void*du=a->done_user;a->done=0;a->done_user=0;if(d)d((ABoxBootV2AtResult)r,du);}
static int submit(void*c,const char*cmd,uint32_t ms,ABoxBootV2AtDoneFn d,void*u){OtaAdapter_t*a=c;a->done=d;a->done_user=u;if(!EC800_AT_Submit(a->at,cmd,EC800_AT_OWNER_OTA,EC800_AT_PRIORITY_HIGH,ms,at_done,a)){a->done=0;a->done_user=0;return 0;}return 1;}
static int send(void*c,const uint8_t*d,uint16_t n){OtaAdapter_t*a=c;return EC800_AT_SendPayloadForOwner(a->at,EC800_AT_OWNER_OTA,d,n)?1:0;}
static int raw(void*c,uint32_t n){OtaAdapter_t*a=c;return EC800_AT_BeginRawRead(a->at,EC800_AT_OWNER_OTA,n)?1:0;}
static int pending(void*c){OtaAdapter_t*a=c;return EC800_AT_HasPending(a->at,EC800_AT_OWNER_OTA)?1:0;}
static int active(void*c){OtaAdapter_t*a=c;return EC800_AT_IsActiveOwner(a->at,EC800_AT_OWNER_OTA)?1:0;}
static void cancel(void*c){OtaAdapter_t*a=c;EC800_AT_CancelOwner(a->at,EC800_AT_OWNER_OTA);}
static void event(Ec800AtEvent_t e,const uint8_t*d,uint16_t n,void*u){OtaAdapter_t*a=u;if(a->events)a->events((ABoxBootV2AtEvent)e,d,n,a->events_user);}
static void events(void*c,ABoxBootV2AtEventFn f,void*u){OtaAdapter_t*a=c;a->events=f;a->events_user=u;EC800_AT_RegisterUrcHandler(a->at,EC800_AT_OWNER_OTA,event,a);}
static void pause(void*c,uint8_t p){(void)c;MQTT_Core_SetPaused(p);}
static void logline(void*c,uint8_t l,const char*m){(void)c;if(l>=3U)APP_LOGE("OTA","%s",m);else if(l==2U)APP_LOGW("OTA","%s",m);else APP_LOGI("OTA","%s",m);}

void OtaService_Init(Ec800AtCore_t *at)
{
 static ABoxBootV2AppPort p;static ABoxBootV2AppConfig c;static const ABoxBootV2Layout layout={BOOT_STATE_A_ADDR,BOOT_STATE_B_ADDR,BOOT_STATE_PAGE_SIZE,APP_START_ADDR,APP_END_ADDR,0x20000000U,0x20010000U};extern uint8_t __app_image_end;
 memset(&g_adapter,0,sizeof(g_adapter));g_adapter.at=at;memset(&p,0,sizeof(p));p.context=&g_adapter;p.tick_ms=tick;p.submit=submit;p.send_payload=send;p.begin_raw_read=raw;p.has_pending=pending;p.is_active=active;p.cancel=cancel;p.register_events=events;p.mqtt_pause=pause;p.log=logline;p.transfer_buffer=(uint8_t*)g_mqtt_rx_pending_payload;p.transfer_buffer_size=sizeof(g_mqtt_rx_pending_payload);
 memset(&c,0,sizeof(c));c.ota_origin="https://ota.zxwl.ntiov.com:20443";c.revision_product="top_flying_wing";c.artifact_name="top_flying_wing_app.bin";c.version_prefix="top-flying-wing-";c.running_version=TopFlyingWing_AppFwVersion();c.ca_pem=(const uint8_t*)g_ota_ca_pem;c.ca_pem_length=(uint32_t)(sizeof(g_ota_ca_pem)-1U);c.app_start_addr=APP_START_ADDR;c.app_size=(uint32_t)(&__app_image_end-(uint8_t*)APP_START_ADDR);c.app_max_size=APP_MAX_SIZE;
 (void)ABoxBootV2_StateBindLayout(&layout);(void)ABoxBootV2App_Init(&p,&c);
}
uint8_t OtaService_BeginProvisioning(void){return(uint8_t)ABoxBootV2App_BeginProvisioning();}void OtaService_Task(void){ABoxBootV2App_Task();}
uint8_t OtaService_IsProvisioned(void){return(uint8_t)ABoxBootV2App_IsProvisioned();}uint8_t OtaService_IsReady(void){return(uint8_t)ABoxBootV2App_IsReady();}uint8_t OtaService_IsBusy(void){return(uint8_t)ABoxBootV2App_IsBusy();}
const char*OtaService_Phase(void){return ABoxBootV2App_Phase();}const char*OtaService_TargetVersion(void){return ABoxBootV2App_TargetVersion();}uint32_t OtaService_LastError(void){return ABoxBootV2App_LastError();}
uint8_t OtaService_Start(const OtaServiceRequest_t*r){ABoxBootV2AppRequest c;if(!r)return 0U;memcpy(&c,r,sizeof(c));return(uint8_t)ABoxBootV2App_Start(&c);}
uint8_t OtaService_TakeInstallReady(void){return(uint8_t)ABoxBootV2App_TakeInstallReady();}uint8_t OtaService_TakeFailure(uint32_t*e){return(uint8_t)ABoxBootV2App_TakeFailure(e);}void OtaService_AcknowledgeRollback(void){(void)ABoxBootV2_AcknowledgeRollback();}

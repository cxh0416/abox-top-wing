#include "wing_app_service.h"
#include "usart.h"
#include "app_log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

UART_HandleTypeDef huart1;
static uint32_t now;
static int power_done=1, stop_error, start_error, ota_busy, store_busy, lease;
static unsigned stops, starts, restarts, reset_events, provisions, ota_calls, downloads;
static char order[16];
static unsigned order_size;
static void record(char c) { assert(order_size<sizeof(order)-1); order[order_size++]=c; order[order_size]=0; }
uint32_t HAL_GetTick(void) { return now; }
void EC_Power_Task(void) { }
int EC_Power_IsOnDone(void) { return power_done; }
void EC_Power_StartRestartSequence(void) { ++restarts; power_done=0; }
int HAL_UART_AbortReceive(UART_HandleTypeDef *u) { (void)u; ++stops; return stop_error; }
int HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef *u, uint8_t *b, uint16_t n)
{ (void)u; assert(b && n==1088); ++starts; return start_error; }
void AppLog_Printf(AppLogLevel_t l,const char *t,const char *f,...)
{ (void)l; (void)t; (void)f; }
uint8_t OtaService_BeginProvisioning(void) { ++provisions; return 1; }
void OtaService_Task(void) { ++ota_calls; if (!lease) ++downloads; record('O'); }
void ABoxBootV2App_Task(void) { assert(!"shared owner bypassed the OTA lease adapter"); }
int ABoxBootV2App_IsBusy(void) { return ota_busy; }
void MQTT_Core_Task(void) { record('M'); }
void WingEnrollment_Task(void) { record('E'); }
void MQTT_App_Task(void) { record('C'); }
uint8_t MqttRuntime_NeedsModemRecovery(void) { return 0; }
uint8_t MQTT_V4_StoreBusy(void) { return store_busy; }
void MQTT_Core_SetPaused(uint8_t p) { assert(p==1); }
static uint32_t tick(void *c) { (void)c; return now; }
static int write_at(void *c,const uint8_t *d,uint16_t n,uint32_t t)
{ (void)c; (void)d; (void)n; (void)t; return 1; }
static void event(ABoxEc800Event e,const uint8_t *d,uint16_t n,void *c)
{ (void)c; if(e==ABOX_EC800_EVENT_LINE && n==3 && !memcmp(d,"RDY",3)) ++reset_events; }
static void poll(uint32_t time)
{ now=time; order_size=0; order[0]=0; WingAppService_Poll(now); }

int main(void)
{
    ABoxEc800At at;
    ABoxEc800Rx rx;
    uint8_t dma[1088], ring[1152];
    ABoxEc800AtPort wire={.tick_ms=tick,.write=write_at};
    assert(ABoxEc800At_Init(&at,&wire));
    assert(ABoxEc800At_Register(&at,20,event,NULL));
    assert(ABoxEc800Rx_Init(&rx,dma,sizeof(dma),ring,sizeof(ring)));
    assert(WingAppService_Init(&at,&rx));
    rx.head=44; rx.tail=40; rx.dma_position=32;
    stop_error=1; poll(0);
    assert(stops==1 && !starts && rx.head==44 && rx.tail==40 && rx.dma_position==32);
    assert(at.quarantined && !provisions && !ota_calls);
    stop_error=0; start_error=1; poll(128);
    assert(starts==1 && !rx.head && !rx.tail && !rx.dma_position);
    start_error=0; poll(256);
    assert(starts==2 && !rx.restart_requested && at.quarantined && !reset_events);

    poll(8000);
    assert(provisions==1 && !strcmp(order,"OMEC") && restarts==1 && !power_done);
    power_done=1; start_error=1; poll(8001);
    assert(at.quarantined && !reset_events && !order_size);
    start_error=0; poll(8129);
    assert(!at.quarantined && reset_events==1 && !strcmp(order,"OMEC"));
    poll(8130); assert(reset_events==1 && provisions==1);

    /* UART-only restart cannot clear isolation or fabricate a modem reset. */
    ABoxEc800Rx_RequestRestart(&rx); poll(8131);
    assert(at.quarantined && reset_events==1);
    /* An active maintenance lease and OTA both defer physical recovery. */
    store_busy=1; lease=1;
    unsigned old_downloads=downloads;
    poll(40000);
    assert(restarts==1 && downloads==old_downloads && !strcmp(order,"OMEC"));
    store_busy=0; ota_busy=1; poll(40001);
    assert(restarts==1);
    ota_busy=lease=0; poll(40002);
    assert(restarts==2 && downloads==old_downloads+1);
    power_done=1; poll(40003);
    assert(reset_events==2 && !at.quarantined && provisions==1);
    /* A broken startup UART must not acquire OTA resources from the readiness
     * predicate and thereby block its own physical recovery. */
    now=50000; provisions=0; stop_error=1;
    assert(ABoxEc800At_Init(&at,&wire));
    assert(ABoxEc800Rx_Init(&rx,dma,sizeof(dma),ring,sizeof(ring)));
    assert(WingAppService_Init(&at,&rx));
    unsigned old_restarts=restarts;
    poll(58000);
    assert(!provisions && !order_size && restarts==old_restarts+1);
    puts("PASS V4 lifecycle: production wing adapter + shared owner, UART failures, reset, lease, poll order");
}

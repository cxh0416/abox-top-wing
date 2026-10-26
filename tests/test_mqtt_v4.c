/* Reuse the existing CAN harness and its complete control/safety regression. */
#include <stdio.h>
#define main wing_control_regression
#include "test_wing_controller.c"
#undef main
#include "mqtt_v4.h"
#include "mqtt_v4_store.h"
#include "mqtt_app_int.h"
#include "mqtt_core.h"
#include "time_base.h"
#include "ota_info.h"

static union { uint64_t align; uint8_t bytes[2048]; } flash_page;
static uint64_t utc;
static uint8_t trusted, ready, publish_busy;
static uint32_t ready_seq;
static int program_budget;
static uint8_t expire_during_write;
static unsigned response_count, publish_count;
static char response[1030];
typedef struct { char topic[128], payload[1030]; uint8_t qos, retain; } Publish;
static Publish published[64];
static ABoxMqttReceiptStatus receipts[65];

char g_mqtt_app_device_id[64] = "W1";
char g_mqtt_app_manifest_topic[128] = "/zxwl/abox/W1/manifest";
char g_mqtt_app_heartbeat_topic[128] = "/zxwl/abox/W1/heartbeat";
char g_mqtt_app_status_report_topic[128] = "/zxwl/abox/W1/report";
uint32_t g_mqtt_app_last_hb_tick, g_mqtt_app_last_hb_ok_tick;
volatile uint8_t g_mqtt_app_hb_pending, g_mqtt_app_resp_pending;
void MQTT_V4_TestReset(void);

uint8_t* MQTT_V4_TestFlash(void) { return flash_page.bytes; }
int MQTT_V4_TestProgram(uint32_t offset, const void* data, uint32_t length)
{
    const uint8_t* input = data;
    assert(offset >= 512U && offset + length <= sizeof(flash_page.bytes));
    for (uint32_t i = 0; i < length; ++i)
    {
        if (!program_budget) return 0;
        if (program_budget > 0) --program_budget;
        assert((flash_page.bytes[offset+i] & input[i]) == input[i]);
        flash_page.bytes[offset+i] &= input[i];
    }
    if (expire_during_write) utc=1800000005000ULL;
    return 1;
}
void OTA_Info_Load(OTA_Info_t* info) { memcpy(info, flash_page.bytes, sizeof(*info)); }
uint8_t OTA_Info_Save(const OTA_Info_t* info)
{
    if (!MQTT_V4_StoreCanErase()) return 0;
    memset(flash_page.bytes, 255, sizeof(flash_page.bytes));
    memcpy(flash_page.bytes, info, sizeof(*info)); return 1;
}
uint64_t TimeBase_GetMs(void) { return utc; }
ABoxClockSnapshot TimeBase_GetSnapshot(void)
{
    ABoxClockSnapshot s = {0};
    s.utc_ms = utc; s.mono_ms = g_tick; s.error_ms = 10U;
    s.quality = trusted ? ABOX_CLOCK_SYNCHRONIZED : ABOX_CLOCK_BOOTSTRAP;
    return s;
}
uint8_t TimeBase_IsTrusted(void) { return trusted; }
uint16_t TimeBase_CheckExpiry(uint64_t expiry)
{ ABoxClockSnapshot s = TimeBase_GetSnapshot(); return ABoxMqttV4_CheckExpiry(&s, expiry); }
const char* TopFlyingWing_AppFwVersion(void) { return "top-flying-wing-1.1.0"; }
uint8_t MQTT_App_QueuePublicResponse(const char* payload, const char* id, uint16_t code)
{ (void)id; (void)code; assert(strlen(payload) <= 1024U); strcpy(response, payload); ++response_count; return 1U; }
uint8_t MQTT_Core_IsReady(void) { return ready; }
uint32_t MQTT_Core_GetReadySeq(void) { return ready_seq; }
uint8_t MQTT_Core_IsPublishActive(void) { return publish_busy; }
ABoxMqttReceiptStatus MQTT_Core_PublishResult(uint64_t operation)
{ assert(operation <= 64U); return receipts[operation]; }
uint8_t MQTT_Core_PublishExTracked(const char* topic, const char* payload, uint8_t qos, uint8_t retain, uint64_t* operation)
{
    if (publish_busy) return 0;
    assert(publish_count < 64U && strlen(payload) <= 1024U);
    Publish* p = &published[publish_count++];
    strcpy(p->topic, topic); strcpy(p->payload, payload); p->qos = qos; p->retain = retain;
    *operation = publish_count; receipts[publish_count] = ABOX_MQTT_RECEIPT_PENDING;
    publish_busy = 1U; return 1;
}

static void reset_all(void)
{
    memset(flash_page.bytes, 255, sizeof(flash_page.bytes));
    memset(published, 0, sizeof(published)); memset(receipts, 0, sizeof(receipts));
    utc = 1800000000000ULL; trusted = ready = 1U; ready_seq = 1U; publish_busy = 0U;
    program_budget = -1; response_count = publish_count = 0U;
    expire_during_write=0U;
    g_mqtt_app_last_hb_tick = g_mqtt_app_last_hb_ok_tick = 0U;
    reset_controller(); MQTT_V4_TestReset();
}
static int code(void)
{
    cJSON* root = cJSON_Parse(response);
    assert(root);
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "version")->valuestring,"4.0"));
    assert(!cJSON_GetObjectItemCaseSensitive(data,"deviceType") && !cJSON_GetObjectItemCaseSensitive(data,"product"));
    int result = cJSON_GetObjectItemCaseSensitive(data, "code")->valueint;
    cJSON_Delete(root); return result;
}
static void request(const char* id, const char* action)
{
    char packet[512], expiry[21], timestamp[21];
    ABoxMqttV4_FormatInteger(expiry, sizeof(expiry), 1800000005000ULL);
    ABoxMqttV4_FormatInteger(timestamp, sizeof(timestamp), 1800000000000ULL);
    snprintf(packet,sizeof(packet),"{\"version\":\"4.0\",\"timestamp\":%s,\"data\":{\"requestId\":\"%s\",\"command\":\"wing_control\",\"params\":{\"action\":\"%s\"}%s%s}}",
        timestamp,id,action,!strcmp(action,"stop") ? "" : ",\"expiresAt\":",!strcmp(action,"stop") ? "" : expiry);
    MQTT_V4_HandleRequest(packet);
}
static void results(void)
{ WingControlResult result; while(WingController_TakeResult(&result)) MQTT_V4_HandleResult(&result); }
static void finish_start(void)
{
    status(50U,0U,1U); WingController_Task(251U); status(50U,0U,252U);
    limits(100U,10U,253U); ack(WING_ACTION_ASCEND,254U); status(50U,1U,255U); results();
}
static void confirm_publish(void)
{ receipts[publish_count] = ABOX_MQTT_RECEIPT_CONFIRMED; publish_busy = 0U; MQTT_V4_ReportTask(); }

static void test_expiry_and_strict_input(void)
{
    reset_all(); trusted=0U; request("time-1","ascend"); assert(code()==4003 && !g_tx_count);
    trusted=1U; utc=1800000005000ULL; request("expired","ascend"); assert(code()==4005 && !g_tx_count);
    MQTT_V4_HandleRequest("{\"version\":\"4.0\",\"timestamp\":1,\"data\":{\"requestId\":\"x\",\"command\":\"wing_control\",\"expiresAt\":6002,\"params\":{\"action\":\"ascend\"}}}");
    assert(code()==4001 && !g_tx_count);
    unsigned before=response_count;
    MQTT_V4_HandleRequest("{\"version\":\"4.0\",\"timestamp\":1e0,\"data\":{\"requestId\":\"x\"}}");
    MQTT_V4_HandleRequest("{\"version\":\"4.0\",\"timestamp\":-0,\"data\":{\"requestId\":\"x\"}}");
    MQTT_V4_HandleRequest("{\"version\":\"4.0\",\"timestamp\":1,\"data\":{\"requestId\":\"a\",\"requestId\":\"b\"}}");
    assert(response_count==before && !g_tx_count);
    before=response_count; trusted=0U;
    request("unknown-state-stop","stop"); assert(response_count==before && count_command('S','T')==1U);
    ack(WING_ACTION_STOP,1000U); status(50U,0U,1001U); results(); assert(code()==200);
    reset_all();expire_during_write=1U;request("expires-during-reservation","ascend");
    assert(code()==4005 && !g_tx_count);
}

static void test_replay_and_reboot(void)
{
    char original[1030];
    reset_all(); request("up-1","ascend"); assert(count_command('R','D')==1U);
    request("up-1","ascend"); assert(count_command('R','D')==1U && !response_count);
    request("up-1","descend"); assert(code()==4006 && count_command('R','D')==1U);
    finish_start(); assert(code()==200 && count_command('U','P')==1U); strcpy(original,response);
    assert(!MQTT_V4_StoreCanErase());
    OTA_Info_t config = {0}; assert(!OTA_Info_Save(&config));
    request("up-1","ascend"); assert(!strcmp(original,response) && count_command('U','P')==1U);
    reset_controller(); MQTT_V4_TestReset(); trusted=0U; utc=1800000009000ULL;
    request("up-1","ascend"); assert(code()==200 && !strcmp(original,response) && !g_tx_count);
    request("up-1","descend"); assert(code()==4006 && !g_tx_count);
    trusted=1U; utc=1800000606000ULL; assert(MQTT_V4_StoreCanErase());
}

static void test_uncertain_and_torn_records(void)
{
    reset_all(); request("uncertain","ascend"); assert(count_command('R','D')==1U);
    reset_controller(); MQTT_V4_TestReset(); request("uncertain","ascend");
    assert(code()==4003 && !g_tx_count);
    reset_all(); program_budget=20; request("torn","ascend"); assert(code()==5001 && !g_tx_count);
    program_budget=-1; reset_controller(); MQTT_V4_TestReset(); request("torn","ascend");
    assert(code()==5001 && !g_tx_count && !MQTT_V4_StoreCanErase());
    MQTT_V4_HandleRequest("{\"version\":\"4.0\",\"timestamp\":1,\"data\":{\"requestId\":\"diagnose\",\"command\":\"sync_state\",\"params\":null}}");
    assert(code()==200 && !g_tx_count);
    unsigned before=response_count;
    request("still-can-stop","stop"); assert(response_count==before && count_command('S','T')==1U);
    ack(WING_ACTION_STOP,1000U); status(50U,0U,1001U); results(); assert(code()==200);
}

static void test_stop_confirmation_and_long_ids(void)
{
    char id[65]; memset(id,'a',64U); id[64]=0;
    reset_all(); request(id,"ascend"); finish_start(); assert(code()==200);
    assert(!strcmp(WingController_GetSnapshot()->last_request_id,id));
    request("stop-1","stop"); assert(count_command('S','T')==1U);
    ack(WING_ACTION_STOP,256U); status(50U,0U,257U); results(); assert(code()==200);
    assert(!WingController_GetSnapshot()->status_fresh);
    WingReport r; int found=0;
    while(WingController_TakeReport(&r))
        if(!strcmp(r.request_id,"stop-1"))
        { assert(r.snapshot.motion_state==WING_MOTION_STOPPED && r.snapshot.status_fresh && r.snapshot.controller_online); found=1; }
    assert(found);
    unsigned before=response_count;
    request("stop-1","stop"); assert(response_count==before && count_command('S','T')==2U);
    ack(WING_ACTION_STOP,258U); status(50U,0U,259U); results(); assert(code()==200);

    reset_all(); request("fault-up","ascend"); finish_start(); request("fault-stop","stop");
    ack(WING_ACTION_STOP,256U); status_with_fault(50U,1U,1U,257U); results(); assert(code()==5005);
}

static void test_endpoint_turnaround(void)
{
    reset_all(); request("endpoint-up","ascend"); finish_start();
    WingController_Task(355U); status(100U,1U,356U);
    assert(count_command('S','T')==1U);
    ack(WING_ACTION_STOP,357U); status(100U,0U,358U);
    request("endpoint-explicit-stop","stop"); assert(count_command('S','T')==2U);
    ack(WING_ACTION_STOP,359U); status(100U,0U,360U); results(); assert(code()==200);
    request("endpoint-down","descend");
    assert(WingController_GetSnapshot()->can_session==WING_CAN_SESSION_PREFLIGHT);
}

static void test_capacity_preserves_protected_requests(void)
{
    char id[32];
    reset_all();
    for (unsigned i=0;i<MQTT_V4_STORE_SLOTS;++i)
    {
        snprintf(id,sizeof(id),"capacity-%u",i);request(id,"ascend");
        g_tick+=WING_QUERY_TIMEOUT_MS;WingController_Task(g_tick);results();assert(code()==5002);
    }
    unsigned queries=count_command('R','D');
    request("capacity-full","ascend");assert(code()==4003 && count_command('R','D')==queries);
    reset_controller();MQTT_V4_TestReset();request("capacity-0","ascend");
    assert(code()==5002 && !g_tx_count); /* Full storage cannot evict an unexpired record. */
}

static void test_bootstrap_sync_and_report_qos(void)
{
    reset_all(); MQTT_V4_ReportTask(); assert(publish_count==1 && published[0].retain==1U && published[0].qos==1U);
    assert(strstr(published[0].payload,"\"name\":\"wing\""));
    confirm_publish(); assert(publish_count==2 && strstr(published[1].topic,"heartbeat") && !published[1].qos);
    confirm_publish(); assert(publish_count==3 && strstr(published[2].payload,"wing_status") && !published[2].retain && published[2].qos==1);
    confirm_publish(); assert(MQTT_V4_AnnouncementReady());
    MQTT_V4_HandleRequest("{\"version\":\"4.0\",\"timestamp\":1,\"data\":{\"requestId\":\"sync-1\",\"command\":\"sync_state\",\"params\":null}}");
    assert(code()==200 && strstr(response,"expectedStates") && !g_tx_count);
    MQTT_V4_ReportTask(); assert(strstr(published[publish_count-1].payload,"\"requestId\":\"sync-1\""));
    confirm_publish();
    request("position-up","ascend"); finish_start();
    for(unsigned i=0;i<12U;i++) { MQTT_V4_ReportTask(); if(publish_busy) confirm_publish(); }
    g_tick=1000U; MQTT_V4_ReportTask();
    assert(strstr(published[publish_count-1].payload,"wing_position") && published[publish_count-1].qos==0U && !published[publish_count-1].retain);
    ready=0U; MQTT_V4_ReportTask(); ready=1U; ++ready_seq; publish_busy=0;
    MQTT_V4_ReportTask(); assert(strstr(published[publish_count-1].topic,"manifest"));
}

int main(void)
{
    wing_control_regression();
    test_expiry_and_strict_input(); test_replay_and_reboot(); test_uncertain_and_torn_records();
    test_stop_confirmation_and_long_ids(); test_endpoint_turnaround(); test_capacity_preserves_protected_requests(); test_bootstrap_sync_and_report_qos();
    puts("V4 adapter: expiry, canonical replay, reboot/torn flash, confirmed stop, bootstrap/sync/QoS passed");
    return 0;
}

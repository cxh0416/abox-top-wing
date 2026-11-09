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
#include "abox_ec800_ufs.h"
#include "abox_mqtt_request.h"
#include "abox_boot_v2.h"
#include <stddef.h>

static union { uint64_t align; uint8_t bytes[2048]; } flash_page;
static uint64_t utc;
static uint8_t trusted, ready, publish_busy;
static uint32_t ready_seq;
static int program_budget;
static uint8_t expire_during_write;
static unsigned response_count, publish_count;
static uint8_t response_queue_blocked;
static char response_ids[128][65];
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
void MQTT_V4_RetryResponses(void);

uint8_t* MQTT_V4_TestFlash(void) { return flash_page.bytes; }
int MQTT_V4_TestProgram(uint32_t offset, const void* data, uint32_t length)
{
    const uint8_t* input = data;
    assert(offset + length <= sizeof(flash_page.bytes));
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
    uint32_t marker; memcpy(&marker,flash_page.bytes+offsetof(OTA_Info_t,reserved),sizeof(marker));
    memset(flash_page.bytes, 255, sizeof(flash_page.bytes));
    memcpy(flash_page.bytes, info, sizeof(*info));
    memcpy(flash_page.bytes+offsetof(OTA_Info_t,reserved),&marker,sizeof(marker)); return 1;
}
uint64_t TimeBase_GetMs(void) { return utc; }
uint64_t TimeBase_GetReportedMs(void) { return utc; }
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
{
    assert(code && strlen(payload) <= 1024U);
    if (response_queue_blocked) return 0U;
    assert(response_count < 128U);
    strcpy(response_ids[response_count], id); strcpy(response, payload); ++response_count; return 1U;
}
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

/* Durable UFS disk model. The real shared journal/idempotency and product
 * adapter run unchanged; only the modem file boundary is emulated here. */
typedef struct { uint8_t bytes[MQTT_V4_IDEM_WORKSPACE_SIZE]; size_t size; int exists; } File;
static File files[2];
static ABoxUfsRequest file_request;
static uint64_t file_operation;
static int leased, ota_busy, file_error, cut_write = -1;
static unsigned writes;
static uint8_t workspace[MQTT_V4_IDEM_WORKSPACE_SIZE];
void MQTT_V4_TestStoreBoot(const ABoxEc800UfsPort* port);
Ec800AtCore_t* MQTT_Core_AtCore(void) { return NULL; }
uint8_t MQTT_Core_CanRunBackgroundAt(void) { return ready; }
uint8_t* OtaService_IdemWorkspace(void) { return workspace; }
int OtaService_AcquireIdemWorkspace(void) { if (leased || ota_busy) return 0; leased=1; return 1; }
void OtaService_ReleaseIdemWorkspace(void) { assert(leased); leased=0; }
static int file_start(void* context, const ABoxUfsRequest* request, uint64_t* operation)
{
    (void)context; assert(leased); file_request=*request; *operation=++file_operation; return 1;
}
static ABoxAsyncStatus file_poll(void* context, uint64_t operation, uint32_t* transferred)
{
    (void)context; assert(operation==file_operation && leased);
    File* file=&files[strstr(file_request.path,"-b.") ? 1 : 0];
    if (file_error) return ABOX_ASYNC_ERROR;
    if (file_request.operation==ABOX_UFS_UPLOAD)
    {
        size_t size=file_request.size;
        ++writes;
        if (cut_write>=0 && (size_t)cut_write<size) size=(size_t)cut_write;
        memcpy(file->bytes,file_request.data,size); file->size=size; file->exists=1; *transferred=(uint32_t)size;
        if(expire_during_write) utc=1800000005000ULL;
        return size==file_request.size ? ABOX_ASYNC_OK : ABOX_ASYNC_ERROR;
    }
    if (!file->exists) return ABOX_ASYNC_NOT_FOUND;
    size_t size=file->size<file_request.size ? file->size : file_request.size;
    memcpy(file_request.data,file->bytes,size); *transferred=(uint32_t)size;
    return size==file_request.size ? ABOX_ASYNC_OK : ABOX_ASYNC_TRUNCATED;
}
static int file_cancel(void* context, uint64_t operation) { (void)context;(void)operation;return 1; }
static void pump(void) { for(unsigned i=0;i<120U;++i) MQTT_V4_Task(); }
static void boot_storage(void)
{
    const ABoxEc800UfsPort port={NULL,file_start,file_poll,file_cancel};
    leased=0; MQTT_V4_TestReset(); MQTT_V4_TestStoreBoot(&port); pump();
}

static void reset_all(void)
{
    memset(flash_page.bytes, 255, sizeof(flash_page.bytes));
    memset(published, 0, sizeof(published)); memset(receipts, 0, sizeof(receipts));
    utc = 1800000000000ULL; trusted = ready = 1U; ready_seq = 1U; publish_busy = 0U;
    program_budget = -1; response_count = publish_count = 0U; response_queue_blocked = 0U;
    strcpy(g_mqtt_app_device_id,"W1"); expire_during_write=0U; memset(files,0,sizeof(files)); file_operation=writes=0; leased=ota_busy=file_error=0; cut_write=-1;
    g_mqtt_app_last_hb_tick = g_mqtt_app_last_hb_ok_tick = 0U;
    reset_controller(); boot_storage();
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
    MQTT_V4_HandleRequest(packet); pump();
}
static void power_request_with_expiry(const char* id, const char* action, int with_expiry)
{
    char packet[512];
    snprintf(packet, sizeof(packet), "{\"version\":\"4.0\",\"timestamp\":1800000000000,\"data\":{\"requestId\":\"%s\",\"command\":\"power_%s\",\"params\":null%s}}", id, action,
        with_expiry ? ",\"expiresAt\":1800000005000" : "");
    MQTT_V4_HandleRequest(packet); pump();
}
static void power_request(const char* id, const char* action)
{ power_request_with_expiry(id, action, !strcmp(action, "on")); }
static void results(void)
{ WingControlResult result; while(WingController_TakeResult(&result)) MQTT_V4_HandleResult(&result); pump(); }
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
    assert(MQTT_V4_StoreCanErase());
    OTA_Info_t config = {0}; assert(OTA_Info_Save(&config));
    request("up-1","ascend"); assert(!strcmp(original,response) && count_command('U','P')==1U);
    reset_controller(); boot_storage(); trusted=0U; utc=1800000009000ULL;
    request("up-1","ascend"); assert(code()==200 && !strcmp(original,response) && !g_tx_count);
    request("up-1","descend"); assert(code()==4006 && !g_tx_count);
    trusted=1U; utc=1800000606000ULL; assert(MQTT_V4_StoreCanErase());
}

static void test_uncertain_and_torn_records(void)
{
    reset_all(); request("uncertain","ascend"); assert(count_command('R','D')==1U);
    reset_controller(); boot_storage(); request("uncertain","ascend");
    assert(code()==4003 && !g_tx_count);
    reset_all(); cut_write=20; request("ufs-cut","ascend"); assert(code()==5001 && !g_tx_count);
    cut_write=-1; reset_controller(); boot_storage(); request("ufs-recovered","ascend");
    assert(count_command('R','D')==1U); /* No effect preceded the failed preparation. */
    reset_all(); flash_page.bytes[MQTT_V4_STORE_OFFSET]=0; request("legacy-torn","ascend");
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

static void finish_explicit_stop(uint32_t tick)
{
    ack(WING_ACTION_STOP, tick); status(50U, 0U, tick + 1U); results();
}

static void test_completed_stops_do_not_fill_cache(void)
{
    char id[32];
    reset_all();
    for (unsigned i = 0; i < 16U; ++i)
    {
        unsigned before = response_count;
        snprintf(id, sizeof(id), "stop-history-%u", i);
        request(id, "stop");
        assert(response_count == before && count_command('S','T') == i + 1U);
        finish_explicit_stop(10U + i * 2U);
        assert(response_count == before + 1U && code() == 200);
    }
    request("after-stop-history", "ascend");
    assert(count_command('R','D') >= 1U && WingController_GetSnapshot()->can_session == WING_CAN_SESSION_PREFLIGHT);
}

static void test_stop_cache_priority_and_queue_backpressure(void)
{
    char id[32];
    reset_all(); response_queue_blocked = 1U;
    for (unsigned i = 0; i < 11U; ++i)
    {
        snprintf(id, sizeof(id), "pending-stop-%u", i);
        request(id, "stop"); finish_explicit_stop(10U + i * 2U);
    }
    assert(response_count == 0U);
    request("ordinary-when-full", "ascend"); assert(count_command('R','D') == 11U);
    /* The twelfth stop uses the dedicated slot. Its same-ID retry waits;
     * a different stop cannot report success while feedback is pending. */
    request("reserved-stop", "stop");
    unsigned stops = count_command('S','T');
    request("reserved-stop", "stop"); assert(count_command('S','T') == stops);
    response_queue_blocked = 0U;
    request("another-stop", "stop"); assert(code() == 5003 && count_command('S','T') == stops);
    response_queue_blocked = 1U;
    finish_explicit_stop(100U); assert(response_count == 1U);
    /* All slots now hold unsent responses. The fallback still sends ST but
     * must not fabricate a success or overwrite any of those responses. */
    request("untracked-stop", "stop"); assert(count_command('S','T') == stops + 1U);
    finish_explicit_stop(102U); assert(response_count == 1U);
    response_queue_blocked = 0U; MQTT_V4_RetryResponses();
    assert(response_count == 13U);
    for (unsigned i = 0; i < 11U; ++i)
    {
        snprintf(id, sizeof(id), "pending-stop-%u", i);
        assert(!strcmp(response_ids[i + 1U], id));
    }
    assert(!strcmp(response_ids[12], "reserved-stop"));
}

static void test_stop_reclaim_tick_wrap(void)
{
    char id[32];
    reset_all();
    for (unsigned i = 0; i < 11U; ++i)
    {
        uint32_t tick = UINT32_MAX - 20U + i * 3U;
        snprintf(id, sizeof(id), "wrap-stop-%u", i);
        g_tick = tick; request(id, "stop"); finish_explicit_stop(tick + 1U);
    }
    request("wrap-new", "stop"); finish_explicit_stop(20U);
    request("wrap-stop-1", "ascend"); assert(code() == 4006);
    request("wrap-stop-0", "ascend");
    assert(WingController_GetSnapshot()->can_session == WING_CAN_SESSION_PREFLIGHT);
}

static void test_stop_retry_preserves_unqueued_response(void)
{
    reset_all(); response_queue_blocked = 1U;
    request("same-stop-retry", "stop"); finish_explicit_stop(1U);
    assert(response_count == 0U && count_command('S','T') == 1U);
    request("same-stop-retry", "stop");
    assert(response_count == 0U && count_command('S','T') == 2U);
    response_queue_blocked = 0U; MQTT_V4_RetryResponses();
    assert(response_count == 1U && code() == 200 && !strcmp(response_ids[0], "same-stop-retry"));
    finish_explicit_stop(3U);
    assert(response_count == 1U); /* The fallback cannot claim a second success. */
}

static void test_stop_with_full_cache_and_pending_persistence(void)
{
    char id[32];
    reset_all(); response_queue_blocked = 1U;
    for (unsigned i = 0; i < 10U; ++i)
    {
        snprintf(id, sizeof(id), "blocked-response-%u", i);
        request(id, "stop"); finish_explicit_stop(10U + i * 2U);
    }
    ota_busy = 1U;
    request("pending-durable-motion", "ascend");
    unsigned queries = count_command('R','D');
    request("stop-pending-durable", "stop");
    finish_explicit_stop(100U);
    response_queue_blocked = 0U; ota_busy = 0U; pump();
    assert(code() == 4003 && !strcmp(response_ids[response_count - 1U], "pending-durable-motion"));
    assert(count_command('R','D') == queries + 1U && !count_command('U','P'));
    MQTT_V4_RetryResponses();
    assert(!strcmp(response_ids[response_count - 1U], "stop-pending-durable"));
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
    for (unsigned i=0;i<MQTT_V4_IDEM_COUNT;++i)
    {
        snprintf(id,sizeof(id),"capacity-%u",i);request(id,"ascend");
        g_tick+=WING_QUERY_TIMEOUT_MS;WingController_Task(g_tick);results();assert(code()==5002);
    }
    unsigned queries=count_command('R','D');
    request("capacity-full","ascend");assert(code()==4003 && count_command('R','D')==queries);
    request("stop-with-durable-full", "stop");
    finish_explicit_stop(g_tick + 1U); assert(code() == 200);
    reset_controller();boot_storage();request("capacity-0","ascend");
    assert(code()==5002 && !g_tx_count); /* Full storage cannot evict an unexpired record. */
}

static void test_bootstrap_sync_and_report_qos(void)
{
    reset_all(); MQTT_V4_ReportTask(); assert(publish_count==1 && published[0].retain==1U && published[0].qos==1U);
    assert(strstr(published[0].payload,"\"name\":\"wing\"") && strstr(published[0].payload,"\"name\":\"device_power\""));
    confirm_publish(); assert(publish_count==2 && strstr(published[1].topic,"heartbeat") && !published[1].qos);
    confirm_publish(); assert(publish_count==3 && strstr(published[2].payload,"wing_status") && !published[2].retain && published[2].qos==1);
    confirm_publish(); assert(MQTT_V4_AnnouncementReady());
    assert(publish_count==4 && strstr(published[3].payload,"power_status"));
    confirm_publish();
    MQTT_V4_HandleRequest("{\"version\":\"4.0\",\"timestamp\":1,\"data\":{\"requestId\":\"sync-1\",\"command\":\"sync_state\",\"params\":null}}");
    assert(code()==200 && strstr(response,"expectedStates") && strstr(response,"power_status") && !g_tx_count);
    MQTT_V4_ReportTask(); assert(strstr(published[publish_count-1].payload,"\"requestId\":\"sync-1\""));
    confirm_publish();
    assert(strstr(published[publish_count-1].payload,"power_status") && strstr(published[publish_count-1].payload,"sync-1"));
    confirm_publish();
    request("position-up","ascend"); finish_start();
    for(unsigned i=0;i<12U;i++) { MQTT_V4_ReportTask(); if(publish_busy) confirm_publish(); }
    g_tick=1000U; MQTT_V4_ReportTask();
    assert(strstr(published[publish_count-1].payload,"wing_position") && published[publish_count-1].qos==0U && !published[publish_count-1].retain);
    ready=0U; MQTT_V4_ReportTask(); ready=1U; ++ready_seq; publish_busy=0;
    MQTT_V4_ReportTask(); assert(strstr(published[publish_count-1].topic,"manifest"));
}

static void test_async_stop_and_expiry(void)
{
    reset_all(); ota_busy=1;
    request("waiting-for-ufs","ascend"); assert(!g_tx_count && !response_count);
    MQTT_V4_HandleRequest("{\"version\":\"4.0\",\"timestamp\":1,\"data\":{\"requestId\":\"query-during-ufs-busy\",\"command\":\"sync_state\",\"params\":null}}");
    assert(code()==200 && !g_tx_count);
    request("stop-during-ufs-busy","stop"); assert(count_command('S','T')==1U);
    ack(WING_ACTION_STOP,1U); status(50U,0U,2U); results(); assert(code()==200);
    unsigned queries=count_command('R','D');
    ota_busy=0; pump(); assert(code()==4003 && count_command('R','D')==queries && !count_command('U','P'));

    reset_all(); ota_busy=1; request("expires-while-busy","ascend");
    utc=1800000005000ULL; ota_busy=0; pump();
    assert(code()==4005 && !g_tx_count);
}

static void test_durable_response_and_missing_files(void)
{
    char original[1030];
    reset_all(); request("durable-1","ascend");
    ota_busy=1; finish_start(); assert(!response_count); /* No response before FINAL commit. */
    ota_busy=0; pump(); assert(code()==200); strcpy(original,response);
    assert(writes>=6U); /* Initial mirror, preparation and final response. */
    files[0].exists=0; reset_controller(); boot_storage(); utc+=1000U;
    request("durable-1","ascend"); assert(!strcmp(original,response) && !g_tx_count);
    assert(files[0].exists && files[1].exists);
    files[0].exists=files[1].exists=0; reset_controller(); boot_storage();
    request("missing-both","ascend"); assert(code()==5001 && !g_tx_count);
    request("still-stop","stop"); ack(WING_ACTION_STOP,1U); status(50U,0U,2U); results(); assert(code()==200);
    MQTT_V4_StoreOnModemReset(); pump();
    request("reset-does-not-clear-loss","ascend"); assert(code()==5001 && !count_command('U','P'));
}

static void test_identity_scopes(void)
{
    char first[1030], key1[65], key2[65];
    reset_all(); request("scoped-id","ascend"); finish_start(); strcpy(first,response);
    MQTT_V4_StoreKey("W1","scoped-id",key1); MQTT_V4_StoreKey("W2","scoped-id",key2); assert(strcmp(key1,key2));
    reset_controller(); strcpy(g_mqtt_app_device_id,"W2"); MQTT_V4_IdentityChanged("W2");
    request("scoped-id","ascend"); assert(count_command('R','D')==1U);
    reset_controller(); boot_storage(); strcpy(g_mqtt_app_device_id,"W1"); MQTT_V4_IdentityChanged("W1");
    request("scoped-id","ascend"); assert(!strcmp(first,response) && !g_tx_count);

    reset_all();MQTT_V4_IdentityChanged("W1");ota_busy=1;
    request("pending-before-rename","ascend");assert(!g_tx_count);
    MQTT_V4_IdentityChanged("W2");strcpy(g_mqtt_app_device_id,"W2");
    unsigned before=response_count;
    ota_busy=0;pump();assert(!g_tx_count && response_count==before);
    MQTT_V4_IdentityChanged("W1");strcpy(g_mqtt_app_device_id,"W1");
    request("pending-before-rename","ascend");assert(code()==4003 && !g_tx_count);
}

static void test_legacy_flash_compatibility(void)
{
    MqttV4Record record;
    ABoxMqttRequest decoded={0};
    const char* text="{\"version\":\"4.0\",\"timestamp\":1800000000000,\"data\":{\"requestId\":\"legacy-1\",\"command\":\"wing_control\",\"params\":{\"action\":\"ascend\"},\"expiresAt\":1800000005000}}";
    reset_all();
    assert(ABoxMqttRequest_Decode(&decoded,text,strlen(text),NULL,NULL)==200);
    memset(&record,255,sizeof(record)); record.protect_until=1800000605000ULL;
    memset(record.device_id,0,sizeof(record.device_id)); strcpy(record.device_id,"W1");
    memset(record.request_id,0,sizeof(record.request_id)); strcpy(record.request_id,"legacy-1");
    memcpy(record.digest,decoded.digest,32); ABoxMqttRequest_Close(&decoded);
    record.header_crc=ABoxBootV2_Crc32(&record,offsetof(MqttV4Record,header_crc)); record.begin_commit=0x34574247U;
    record.code=200; record.timestamp=1800000000123ULL;
    record.response_crc=ABoxBootV2_Crc32(&record.code,offsetof(MqttV4Record,response_crc)-offsetof(MqttV4Record,code)); record.complete_commit=0x34574350U;
    memcpy(flash_page.bytes+MQTT_V4_STORE_OFFSET,&record,sizeof(record));
    request("legacy-1","ascend"); assert(code()==200 && !g_tx_count);
    request("legacy-1","descend"); assert(code()==4006 && !g_tx_count);
    assert(!MQTT_V4_StoreCanErase());
    record.complete_commit=UINT32_MAX; memcpy(flash_page.bytes+MQTT_V4_STORE_OFFSET,&record,sizeof(record));
    request("legacy-1","ascend"); assert(code()==4003 && !g_tx_count);
}

static void test_pinch_cancels_queued_motion(void)
{
    reset_all();
    ota_busy = 1U;
    request("before-pinch", "descend");
    assert(!g_tx_count);
    MQTT_V4_CancelPendingMotion();
    ota_busy = 0U;
    pump();
    assert(!g_tx_count && code() == 4003U);
}

extern uint8_t test_ultrasonic_blocked;
extern unsigned test_ultrasonic_prestart_warnings;
static void test_ultrasonic_cancels_queued_and_rejects_held_commands(void)
{
    reset_all();ota_busy=1;
    request("queued-before-sonar","descend");
    assert(!g_tx_count);
    test_ultrasonic_blocked=1;
    MQTT_V4_CancelPendingMotion();
    test_ultrasonic_blocked=0; /* Even if alarm clears before persistence finishes. */
    ota_busy=0;pump();
    assert(!g_tx_count && code()==4003);
    reset_all();test_ultrasonic_blocked=1;
    request("during-sonar","ascend");
    assert(!g_tx_count && code()==4003 && test_ultrasonic_prestart_warnings==1);
    test_ultrasonic_blocked=0;pump();
    assert(!g_tx_count); /* A rejected request is never queued for later movement. */
}

static void power_check_to_limits(uint16_t pos)
{
    status(pos, 0U, 1U);
    g_tick = 251U; WingController_Task(g_tick);
    status(pos, 0U, 252U);
}

static void test_power_control(void)
{
    reset_all(); assert(WingPower_IsOn() && g_power_output == 1U);
    power_request("power-off", "off");
    assert(WingPower_IsOn() && count_command('R','D') == 1U && !response_count);
    power_check_to_limits(10U);
    limits(100U, 10U, 253U);
    assert(WingPower_IsOn() && count_command('R','D') == 3U);
    power_request("concurrent-on", "on"); assert(code()==4003 && WingPower_IsOn());
    status(10U, 0U, 254U); results();
    assert(code() == 200 && strstr(response,"\"powerOutputOn\":false") && !WingPower_IsOn() && g_power_output == 0U);
    request("motion-when-off", "ascend"); assert(code() == 4003 && !count_command('U','P'));
    power_request("power-on", "on"); assert(code() == 200 && strstr(response,"\"powerOutputOn\":true") && WingPower_IsOn() && g_power_output == 1U);
    power_request("power-off", "off"); assert(WingPower_IsOn()); /* Safety off checks again. */
    power_check_to_limits(10U); limits(100U,10U,253U); status(10U,0U,254U); results();
    assert(code()==200 && !WingPower_IsOn());

    reset_all(); power_request("not-closed", "off"); power_check_to_limits(50U);
    limits(100U, 10U, 253U); status(50U, 0U, 254U); results();
    assert(code() == 4003 && WingPower_IsOn());
    reset_all(); power_request("moving", "off"); status(10U, 1U, 1U); results();
    assert(code() == 4003 && WingPower_IsOn());
    reset_all(); power_request("no-controller", "off");
    g_tick = WING_QUERY_TIMEOUT_MS; WingController_Task(g_tick); results();
    assert(code() == 5002 && WingPower_IsOn());
    reset_all(); power_request("final-moving", "off"); power_check_to_limits(10U);
    limits(100U, 10U, 253U); status(10U, 1U, 254U); results();
    assert(code() == 4003 && WingPower_IsOn());
    reset_all(); power_request("bad-limits", "off"); power_check_to_limits(10U);
    limits(10U, 10U, 253U); results(); assert(code() == 5003 && WingPower_IsOn());
    reset_all(); power_request("faulted", "off"); power_check_to_limits(10U);
    limits(100U, 10U, 253U); status_with_fault(10U, 0U, 1U, 254U); results();
    assert(code() == 5005 && WingPower_IsOn());
    reset_all(); power_request("stale-feedback", "off"); power_check_to_limits(10U);
    limits(100U, 10U, 253U); status(10U, 0U, 254U);
    g_tick += WING_ACTIVE_FEEDBACK_TIMEOUT_MS + 1U; results();
    assert(code() == 4003 && WingPower_IsOn());
    reset_all(); power_request_with_expiry("expiry-before-cut", "off", 1); power_check_to_limits(10U);
    limits(100U, 10U, 253U); status(10U, 0U, 254U); utc=1800000005000ULL; results();
    assert(code() == 4005 && WingPower_IsOn());

    reset_all(); trusted=0U; power_request("no-time", "on"); assert(code()==4003 && !g_tx_count);
    reset_all(); trusted=0U; power_request("off-no-time", "off"); assert(count_command('R','D')==1U);
    reset_all(); power_request_with_expiry("on-no-expiry", "on", 0); assert(code()==4001 && !g_tx_count);
    MQTT_V4_HandleRequest("{\"version\":\"4.0\",\"timestamp\":1800000000000,\"data\":{\"requestId\":\"bad-params\",\"command\":\"power_off\",\"params\":{}}}");
    assert(code()==4001 && !g_tx_count);
    reset_all(); ota_busy=1U; power_request("queued-on", "on");
    assert(!g_tx_count && !response_count);
    power_request("off-during-storage", "off"); assert(count_command('R','D')==1U);
    power_check_to_limits(10U); limits(100U,10U,253U); status(10U,0U,254U); results();
    assert(code()==200 && !WingPower_IsOn());
    ota_busy=0U; pump(); assert(code()==4003 && !WingPower_IsOn());
    reset_all(); request("running-up", "ascend"); finish_start();
    power_request("running-off", "off"); assert(code()==4003 && WingPower_IsOn());

    reset_all(); power_request("cancel-for-stop", "off");
    request("operator-stop", "stop"); results(); assert(code()==4003 && WingPower_IsOn());
    ack(WING_ACTION_STOP, 1U); status(10U, 0U, 2U); results();
    assert(code()==200 && WingPower_IsOn());
}

static void test_power_report(void)
{
    reset_all(); MQTT_V4_ReportTask();
    confirm_publish(); confirm_publish(); confirm_publish();
    assert(strstr(published[publish_count-1].payload,"power_status"));
    confirm_publish();
    power_request("report-off", "off"); power_check_to_limits(10U);
    limits(100U,10U,253U); status(10U,0U,254U); results();
    MQTT_V4_ReportTask();
    assert(strstr(published[publish_count-1].payload,"\"reportName\":\"power_status\"") &&
           strstr(published[publish_count-1].payload,"\"requestId\":\"report-off\"") &&
           strstr(published[publish_count-1].payload,"\"powerOutputOn\":false"));
    receipts[publish_count]=ABOX_MQTT_RECEIPT_FAILED; publish_busy=0U; MQTT_V4_ReportTask();
    assert(strstr(published[publish_count-1].payload,"report-off"));
    confirm_publish();
}

int main(int argc, char** argv)
{
    if (argc == 2 && !strcmp(argv[1], "--describe-power"))
    {
        reset_all(); MQTT_V4_ReportTask();
        confirm_publish(); confirm_publish(); confirm_publish(); confirm_publish();
        power_request("power-wire", "off"); power_check_to_limits(10U);
        limits(100U,10U,253U); status(10U,0U,254U); results();
        MQTT_V4_ReportTask();
        printf("{\"response\":%s,\"report\":%s}\n", response, published[publish_count-1].payload);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--describe"))
    {
        /* Export packets from the production bootstrap, without exercising CAN. */
        reset_all(); MQTT_V4_ReportTask(); confirm_publish(); confirm_publish();
        assert(publish_count == 3U);
        printf("{\"packets\":[");
        for (unsigned i = 0; i < 3U; ++i)
            printf("%s{\"topic\":\"%s\",\"qos\":%u,\"retain\":%u,\"payload\":%s}",
                i ? "," : "", published[i].topic, published[i].qos,
                published[i].retain, published[i].payload);
        puts("]}");
        return 0;
    }
    wing_control_regression();
    test_expiry_and_strict_input(); test_replay_and_reboot(); test_uncertain_and_torn_records();
    test_stop_confirmation_and_long_ids(); test_completed_stops_do_not_fill_cache();
    test_stop_cache_priority_and_queue_backpressure(); test_stop_reclaim_tick_wrap();
    test_stop_retry_preserves_unqueued_response();
    test_stop_with_full_cache_and_pending_persistence();
    test_endpoint_turnaround(); test_capacity_preserves_protected_requests(); test_bootstrap_sync_and_report_qos();
    test_async_stop_and_expiry(); test_durable_response_and_missing_files(); test_identity_scopes(); test_legacy_flash_compatibility();
    test_pinch_cancels_queued_motion();
    test_ultrasonic_cancels_queued_and_rejects_held_commands();
    test_power_control();
    test_power_report();
    puts("V4 adapter: expiry, canonical replay, reboot/torn flash, confirmed stop, bootstrap/sync/QoS passed");
    return 0;
}

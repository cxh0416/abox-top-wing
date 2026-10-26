#include "mqtt_v4.h"

#include <stdio.h>
#include <string.h>
#include "abox_cloud_v4.h"
#include "abox_mqtt_request.h"
#include "abox_cloud_service.h"
#include "abox_mqtt_digest.h"
#include "abox_platform_port.h"
#include "app_version.h"
#include "main.h"
#include "mqtt_app_int.h"
#include "mqtt_core.h"
#include "mqtt_v4_store.h"
#include "time_base.h"

#define CACHE_COUNT 12U
typedef struct
{
    char id[65];
    char device_id[33];
    uint8_t digest[32];
    uint64_t timestamp;
    uint32_t tick;
    uint16_t code;
    int8_t store_slot;
    uint8_t valid, inflight, sync, retry_response, awaiting_dispatch, completing, cancelled;
    WingAction action;
} Request;
static Request requests[CACHE_COUNT];
static char response_buf[MQTT_APP_JSON_BUF_SZ];
static char report_buf[MQTT_APP_JSON_BUF_SZ];
static char meta_buf[320];
static uint8_t report_is_heartbeat;
static ABoxCloudBootstrap bootstrap;
static uint32_t ready_sequence, report_sequence;
static uint64_t report_operation, boot_seed;
static uint8_t report_pending, report_telemetry;
static uint8_t retry_snapshot;
static char sync_queue[2][65];
static uint8_t sync_count;
static WingReport report;
static uint32_t sample_tick;
static char bound_device_id[33];

static const ABoxMqttCommandDescriptor commands[] = {{"wing_control", "wing", ABOX_MQTT_COMMAND_PARAMETERIZED}};
static const ABoxMqttReportDescriptor reports[] = {{"wing_status", "wing", "state"}, {"wing_position", "wing", "telemetry"}};
static const ABoxMqttProfileDescriptor profile = {"wing", "1.0", commands, 1U, reports, 2U};

uint8_t MQTT_V4_ValidDeviceId(const char* value)
{
    char topic[128];
    return ABoxMqttV4_BuildTopic(topic, sizeof(topic), value, ABOX_MQTT_V4_TOPIC_REQUEST);
}

static int direction(void* context, const cJSON* params)
{
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(params, "action");
    (void)context;
    return cJSON_IsString(action) && (!strcmp(action->valuestring, "ascend") || !strcmp(action->valuestring, "descend"));
}

static int stopping(void* context, const cJSON* params)
{
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(params, "action");
    (void)context;
    return cJSON_IsString(action) && !strcmp(action->valuestring, "stop");
}

static const ABoxMqttOperation operations[] = {
    {"wing_control", "wing", ABOX_MQTT_COMMAND_DISCRETE_ACTION, ABOX_MQTT_EXPIRY_REQUIRED, 5000U, direction, NULL, NULL},
    {"wing_control", "wing", ABOX_MQTT_COMMAND_SAFETY_STOP, ABOX_MQTT_EXPIRY_FORBIDDEN, 0U, stopping, NULL, NULL}
};

void MQTT_V4_IdentityChanged(const char* id)
{
    WingReport old_report;
    WingControlResult old_result;
    if (!strcmp(bound_device_id, id)) return;
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
    {
        Request* r = &requests[i];
        if (r->inflight && r->store_slot >= 0) r->cancelled = 1U;
        else memset(r, 0, sizeof(*r));
    }
    strcpy(bound_device_id, id);
    /* Configuration changes are admitted only while control is idle. */
    while (WingController_TakeReport(&old_report)) { }
    while (WingController_TakeResult(&old_result)) { }
}

static const char* message(uint16_t code)
{
    switch (code)
    {
        case 200U: return "operation accepted";
        case 4001U: return "invalid request";
        case 4002U: return "unsupported command";
        case 4003U: return "busy or trusted time unavailable";
        case 4005U: return "request expired";
        case 4006U: return "requestId conflict";
        case 5002U: return "controller unavailable";
        case 5003U: return "controller feedback not confirmed";
        case 5004U: return "controller rejected command";
        case 5005U: return "controller fault";
        default: return "internal error";
    }
}

static int encode_response(const char* id, uint16_t code, uint64_t timestamp, uint8_t sync)
{
    cJSON* result = sync && code == 200U ? cJSON_Parse("{\"expectedStates\":[{\"reportName\":\"wing_status\"}]}") : NULL;
    if (sync && code == 200U && !result) return 0;
    uint16_t encoded = ABoxCloudV4_Response(response_buf, sizeof(response_buf), timestamp, id, code, message(code), result);
    cJSON_Delete(result);
    return encoded == code;
}

static uint8_t respond(const char* id, uint16_t code, uint64_t timestamp, uint8_t sync)
{
    return encode_response(id, code, timestamp, sync) && MQTT_App_QueuePublicResponse(response_buf, id, code);
}

static void replay_response(const char* response, const char* id)
{
    cJSON* root = ABoxMqttDigest_Parse(response, strlen(response));
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "data"), "code");
    if (cJSON_IsNumber(code) && code->valuedouble > 0 && code->valuedouble <= UINT16_MAX)
        (void)MQTT_App_QueuePublicResponse(response, id, (uint16_t)code->valuedouble);
    cJSON_Delete(root);
}

static Request* find(const char* id)
{
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
        if (requests[i].valid && !strcmp(requests[i].device_id, g_mqtt_app_device_id) && !strcmp(requests[i].id, id)) return &requests[i];
    return NULL;
}

static Request* reserve(void)
{
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
    {
        Request* r = &requests[i];
        if (!r->valid || (!r->inflight && !r->retry_response && HAL_GetTick() - r->tick > 600000U))
        { memset(r, 0, sizeof(*r)); r->store_slot = -1; return r; }
    }
    return NULL;
}

static void complete(Request* r, uint16_t code)
{
    r->timestamp = TimeBase_GetMs();
    r->code = code;
    r->awaiting_dispatch = 0U;
    if (r->store_slot >= 0)
    {
        /* The original complete wire response becomes visible only after the
         * shared engine has committed and verified both UFS copies. */
        r->completing = 1U;
        return;
    }
    r->inflight = 0U;
    r->retry_response = !strcmp(r->device_id, g_mqtt_app_device_id) && !respond(r->id, code, r->timestamp, r->sync);
}

void MQTT_V4_Task(void)
{
    ABoxMqttIdempotency* cache;
    MQTT_V4_StoreTask();
    cache = MQTT_V4_Store();
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
    {
        Request* r = &requests[i];
        if (!r->inflight || r->store_slot < 0 || !cache) continue;
        size_t index = (size_t)r->store_slot;
        if (cache->failed || cache->entries[index].state == ABOX_IDEM_INTERRUPTED)
        {
            r->store_slot = -1;
            complete(r, 5001U);
            continue;
        }
        if (r->awaiting_dispatch && cache->entries[index].state == ABOX_IDEM_PREPARED)
        {
            ABoxClockSnapshot clock = TimeBase_GetSnapshot();
            uint16_t code = r->cancelled || WingController_GetSnapshot()->can_session != WING_CAN_SESSION_IDLE_SILENT ?
                4003U : ABoxMqttIdem_TakeExecutable(cache, index, &clock);
            r->awaiting_dispatch = 0U;
            if (code == 200U) code = WingController_Request(r->action, r->id);
            if (code) complete(r, code);
        }
        if (r->completing)
        {
            if (cache->entries[index].state == ABOX_IDEM_PREPARED || cache->entries[index].state == ABOX_IDEM_DISPATCHED)
            {
                if (encode_response(r->id, r->code, r->timestamp, 0U))
                    (void)ABoxMqttIdem_Complete(cache, index, response_buf, strlen(response_buf));
            }
            const char* response = ABoxMqttIdem_Response(cache, index, NULL);
            if (response && (strcmp(r->device_id, g_mqtt_app_device_id) || MQTT_App_QueuePublicResponse(response, r->id, r->code)))
            { r->completing = r->inflight = r->valid = 0U; }
        }
    }
}

static int queue_sync(const char* id)
{
    for (unsigned i = 0; i < sync_count; ++i) if (!strcmp(sync_queue[i], id)) return 1;
    if (sync_count == 2U) return 0;
    strcpy(sync_queue[sync_count++], id);
    return 1;
}

void MQTT_V4_HandleRequest(const char* text)
{
    ABoxMqttRequest request = {0};
    const ABoxMqttOperation* operation = NULL;
    ABoxMqttIdempotency* cache = MQTT_V4_Store();
    MqttV4Record saved;
    Request* r;
    char key[65];
    size_t index = 0U;
    uint16_t code, operation_error;
    uint8_t sync;
    WingAction action = WING_ACTION_NONE;
    int legacy;
    code = ABoxMqttRequest_Decode(&request, text, text ? strlen(text) : 0U, NULL, NULL);
    if (code != 200U) goto error;
    operation_error = ABoxMqttRequest_Resolve(&request, operations, sizeof(operations)/sizeof(operations[0]), &operation);
    sync = !strcmp(request.command, "sync_state");
    if (operation)
    {
        if (operation->classification == ABOX_MQTT_COMMAND_SAFETY_STOP) action = WING_ACTION_STOP;
        else if (operation->classification == ABOX_MQTT_COMMAND_DISCRETE_ACTION)
            action = !strcmp(cJSON_GetObjectItemCaseSensitive(request.params, "action")->valuestring, "ascend") ?
                WING_ACTION_ASCEND : WING_ACTION_DESCEND;
    }
    r = find(request.request_id);
    if (r && memcmp(r->digest, request.digest, sizeof(request.digest))) { code = 4006U; goto error; }
    if (r && r->inflight) goto done;
    if (r && r->store_slot < 0 && action != WING_ACTION_STOP)
    {
        if (sync && r->code == 200U && !queue_sync(r->id)) { code = 4003U; goto error; }
        r->retry_response = !respond(r->id, r->code, r->timestamp, r->sync);
        goto done;
    }
    /* Compatibility is read-only: the previous Flash reservations retain
     * their original protection; every new reservation uses the shared engine. */
    legacy = MQTT_V4_StoreFind(g_mqtt_app_device_id, request.request_id, &saved, NULL);
    if (legacy > 0)
    {
        if (memcmp(saved.digest, request.digest, sizeof(request.digest))) { code = 4006U; goto error; }
        if (legacy == 2) (void)respond(request.request_id, (uint16_t)saved.code, saved.timestamp, 0U);
        else { code = 4003U; goto error; }
        goto done;
    }
    MQTT_V4_StoreKey(g_mqtt_app_device_id, request.request_id, key);
    ABoxMqttIdemMatch match = ABoxMqttIdem_Find(cache, key, request.digest, &index);
    if (match == ABOX_IDEM_CONFLICT) { code = 4006U; goto error; }
    if (match == ABOX_IDEM_REPLAY)
    {
        const char* response = ABoxMqttIdem_Response(cache, index, NULL);
        /* Release the request tree before reading response metadata. The
         * response itself is queued byte-for-byte, without re-encoding. */
        strcpy(key, request.request_id);
        ABoxMqttRequest_Close(&request);
        replay_response(response, key);
        goto done;
    }
    if (match == ABOX_IDEM_WAIT)
    {
        if (!r) { code = 4003U; goto error; }
        goto done;
    }
    if (match == ABOX_IDEM_UNCERTAIN) { code = 4003U; goto error; }
    if (operation_error != 200U) { code = operation_error; goto error; }
    if (action != WING_ACTION_STOP && !sync && (legacy < 0 || match == ABOX_IDEM_UNAVAILABLE))
    { code = legacy < 0 || !cache || cache->failed ? 5001U : 4003U; goto error; }
    ABoxClockSnapshot clock = TimeBase_GetSnapshot();
    code = ABoxMqttRequest_CheckNew(&request, operation, &clock);
    if (code != 200U) goto error;
    if (!r) r = reserve();
    if (!r && action == WING_ACTION_STOP)
    {
        for (unsigned i = 0; i < CACHE_COUNT; ++i)
            if (!requests[i].inflight) { r = &requests[i]; break; }
    }
    if (!r) { code = 4003U; goto error; }
    memset(r, 0, sizeof(*r));
    strcpy(r->id, request.request_id); memcpy(r->digest, request.digest, sizeof(request.digest));
    strcpy(r->device_id, g_mqtt_app_device_id);
    r->valid = r->inflight = 1U; r->sync = sync; r->action = action; r->store_slot = -1; r->tick = HAL_GetTick();
    if (sync) { complete(r, queue_sync(r->id) ? 200U : 4003U); goto done; }
    if (action == WING_ACTION_STOP)
    {
        /* A stop also cancels an intention still waiting for durable storage.
         * It must never start later after this stop has completed. */
        for (unsigned i = 0; i < CACHE_COUNT; ++i)
            if (requests[i].awaiting_dispatch) requests[i].cancelled = 1U;
        code = WingController_Request(action, r->id);
        if (code) complete(r, code == 200U ? 5003U : code);
    }
    else
    {
        if (WingController_GetSnapshot()->can_session != WING_CAN_SESSION_IDLE_SILENT)
        { complete(r, 4003U); goto done; }
        code = ABoxMqttIdem_Begin(cache, key, request.digest, request.expires_at, &clock, &index);
        if (code != 200U) { complete(r, code); goto done; }
        r->store_slot = (int8_t)index;
        r->awaiting_dispatch = 1U;
    }
    goto done;
error:
    if (request.request_id) (void)respond(request.request_id, code, TimeBase_GetMs(), 0U);
done:
    ABoxMqttRequest_Close(&request);
}

void MQTT_V4_HandleResult(const WingControlResult* result)
{
    Request* r = result ? find(result->request_id) : NULL;
    uint16_t code;
    if (!r || !r->inflight) return;
    code = result->code;
    if (result->action == WING_ACTION_STOP && code == 200U && !result->stop_confirmed)
        code = result->snapshot.fault_code ? 5005U : 5003U;
    complete(r, code);
}

static void nullable(cJSON* object, const char* name, const char* value)
{ if (value && *value) cJSON_AddStringToObject(object, name, value); else cJSON_AddNullToObject(object, name); }

void MQTT_V4_AddSnapshot(cJSON* payload, const WingSnapshot* s)
{
    char fault[8];
    cJSON_AddStringToObject(payload, "motionState", WingMotionState_Name(s->motion_state));
    nullable(payload, "activeAction", WingAction_Name(s->active_action));
    if (s->have_position) cJSON_AddNumberToObject(payload, "positionMm", s->position_mm); else cJSON_AddNullToObject(payload, "positionMm");
    if (s->have_limits)
    {
        cJSON_AddNumberToObject(payload, "topLimitMm", s->top_limit_mm);
        cJSON_AddNumberToObject(payload, "bottomLimitMm", s->bottom_limit_mm);
    }
    else { cJSON_AddNullToObject(payload, "topLimitMm"); cJSON_AddNullToObject(payload, "bottomLimitMm"); }
    nullable(payload, "atEndpoint", WingEndpoint_Name(s->at_endpoint));
    snprintf(fault, sizeof(fault), "E%02u", (unsigned)s->fault_code);
    nullable(payload, "faultCode", s->fault_code ? fault : NULL);
    if (s->controller_online_known) cJSON_AddBoolToObject(payload, "controllerOnline", s->controller_online); else cJSON_AddNullToObject(payload, "controllerOnline");
    cJSON_AddBoolToObject(payload, "statusFresh", s->status_fresh);
    nullable(payload, "safetyStopReason", WingSafetyReason_Name(s->safety_reason));
}

static uint64_t captured_time(uint32_t captured_tick)
{
    uint64_t timestamp = TimeBase_GetMs();
    uint32_t age = HAL_GetTick() - captured_tick;
    return age <= INT32_MAX && timestamp >= age ? timestamp - age : 0U;
}

static int encode_state(const WingSnapshot* snapshot, const char* request_id, uint8_t telemetry, uint32_t captured_tick)
{
    char id[97];
    cJSON* payload = cJSON_CreateObject();
    int ok = 0;
    if (!payload) return 0;
    if (!boot_seed) boot_seed = TimeBase_GetMs() ^ (ABox_PortGetMonotonicMs() << 16U) ^ ((uint64_t)HAL_GetUIDw0() << 32U) ^ HAL_GetUIDw1();
    snprintf(id, sizeof(id), "%08lX%08lX-%lu", (unsigned long)(boot_seed >> 32U), (unsigned long)boot_seed, (unsigned long)++report_sequence);
    const ABoxCloudV4Report header = {.report_id = id, .type = telemetry ? "telemetry" : "state",
        .name = reports[telemetry ? 1U : 0U].name,
        .request_id = request_id && *request_id ? request_id : NULL, .timestamp = captured_time(captured_tick)};
    if (telemetry)
    {
        if (snapshot->have_position) cJSON_AddNumberToObject(payload, "positionMm", snapshot->position_mm); else cJSON_AddNullToObject(payload, "positionMm");
        cJSON_AddBoolToObject(payload, "statusFresh", snapshot->status_fresh);
    }
    else MQTT_V4_AddSnapshot(payload, snapshot);
    if (cJSON_GetArraySize(payload) == (telemetry ? 2 : 10))
        ok = ABoxCloudV4_Report(report_buf, sizeof(report_buf), &header, payload);
    cJSON_Delete(payload);
    return ok;
}

static int publish_meta(ABoxCloudJobKind kind, uint64_t* operation)
{
    const ABoxCloudV4Profile enabled = {profile.name, profile.version, NULL, 0U};
    int encoded = kind == ABOX_CLOUD_JOB_MANIFEST ?
        ABoxCloudV4_Manifest(meta_buf, sizeof(meta_buf), TimeBase_GetMs(), TopFlyingWing_AppFwVersion(), &enabled, 1U, 0) :
        ABoxCloudV4_Heartbeat(meta_buf, sizeof(meta_buf), TimeBase_GetMs(), TopFlyingWing_AppFwVersion(), NULL);
    int ok = encoded && MQTT_Core_PublishExTracked(kind == ABOX_CLOUD_JOB_MANIFEST ? g_mqtt_app_manifest_topic : g_mqtt_app_heartbeat_topic,
        meta_buf, kind == ABOX_CLOUD_JOB_MANIFEST ? 1U : 0U, kind == ABOX_CLOUD_JOB_MANIFEST ? 1U : 0U, operation);
    if (ok && kind == ABOX_CLOUD_JOB_HEARTBEAT) { g_mqtt_app_last_hb_tick = HAL_GetTick(); g_mqtt_app_hb_pending = 0U; }
    return ok;
}

void MQTT_V4_ResetReports(void)
{
    ABoxCloudBootstrap_Init(&bootstrap);
    report_pending = report_telemetry = report_is_heartbeat = retry_snapshot = 0U; report_operation = 0U; ready_sequence = 0U; sync_count = 0U;
}

uint8_t MQTT_V4_AnnouncementReady(void) { return bootstrap.ready && !ABoxCloudBootstrap_Active(&bootstrap); }

void MQTT_V4_ReportTask(void)
{
    ABoxCloudJob job;
    ABoxMqttReceiptStatus receipt;
    uint32_t now = HAL_GetTick();
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
        if (requests[i].retry_response && respond(requests[i].id, requests[i].code, requests[i].timestamp, requests[i].sync)) requests[i].retry_response = 0U;
    if (!MQTT_Core_IsReady())
    {
        ABoxCloudBootstrap_SetReady(&bootstrap, 0);
        report_operation = 0U; report_pending = report_is_heartbeat = retry_snapshot = 0U;
        return;
    }
    if (ready_sequence != MQTT_Core_GetReadySeq())
    {
        if (ready_sequence) ABoxCloudBootstrap_NetworkChanged(&bootstrap);
        ready_sequence = MQTT_Core_GetReadySeq();
        report_operation = 0U; report_pending = report_is_heartbeat = retry_snapshot = 0U;
    }
    ABoxCloudBootstrap_SetReady(&bootstrap, 1);
    if (bootstrap.in_flight)
    {
        receipt = MQTT_Core_PublishResult(bootstrap.operation);
        if (receipt == ABOX_MQTT_RECEIPT_PENDING) return;
        ABoxCloudJobKind completed = ABoxCloudBootstrap_Receipt(&bootstrap, bootstrap.generation, bootstrap.operation, receipt == ABOX_MQTT_RECEIPT_CONFIRMED);
        if (completed == ABOX_CLOUD_JOB_HEARTBEAT) g_mqtt_app_last_hb_ok_tick = now;
    }
    if (ABoxCloudBootstrap_Active(&bootstrap))
    {
        uint64_t operation;
        if (MQTT_Core_IsPublishActive() || !ABoxCloudBootstrap_Next(&bootstrap, &job)) return;
        if (job.kind == ABOX_CLOUD_JOB_STATE)
        {
            WingController_CopySnapshot(&report.snapshot);
            if (!encode_state(&report.snapshot, NULL, 0U, now) ||
                !MQTT_Core_PublishExTracked(g_mqtt_app_status_report_topic, report_buf, 1U, 0U, &operation)) return;
        }
        else if (!publish_meta(job.kind, &operation)) return;
        ABoxCloudBootstrap_Begin(&bootstrap, &job, operation);
        return;
    }
    if (report_operation)
    {
        receipt = MQTT_Core_PublishResult(report_operation);
        if (receipt == ABOX_MQTT_RECEIPT_PENDING) return;
        report_operation = 0U;
        if (report_is_heartbeat) { if (receipt == ABOX_MQTT_RECEIPT_CONFIRMED) g_mqtt_app_last_hb_ok_tick = now; report_is_heartbeat = 0U; }
        else if (receipt == ABOX_MQTT_RECEIPT_CONFIRMED || report_telemetry) report_pending = 0U;
    }
    if (MQTT_Core_IsPublishActive() || g_mqtt_app_resp_pending) return;
    if (now - g_mqtt_app_last_hb_tick >= MQTT_APP_HEARTBEAT_PERIOD_MS)
    {
        if (publish_meta(ABOX_CLOUD_JOB_HEARTBEAT, &report_operation)) report_is_heartbeat = 1U;
        return;
    }
    if (!report_pending)
    {
        const char* id = NULL;
        report_telemetry = 0U;
        if (retry_snapshot)
        {
            if (!encode_state(&report.snapshot, report.request_id, 0U, report.tick)) return;
            retry_snapshot = 0U;
        }
        else if (sync_count)
        {
            memset(&report, 0, sizeof(report)); WingController_CopySnapshot(&report.snapshot);
            strcpy(report.request_id, sync_queue[0]); id = report.request_id;
            if (!encode_state(&report.snapshot, id, 0U, now)) return;
            if (--sync_count) strcpy(sync_queue[0], sync_queue[1]);
        }
        else if (WingController_TakeReport(&report))
        { if (!encode_state(&report.snapshot, report.request_id, 0U, report.tick)) { retry_snapshot = 1U; return; } }
        else
        {
            WingController_CopySnapshot(&report.snapshot);
            const WingSnapshot* s = &report.snapshot;
            if (s->active_action == WING_ACTION_NONE || now - sample_tick < MQTT_APP_MOTION_REPORT_PERIOD_MS) return;
            if (!encode_state(s, NULL, 1U, now)) return;
            report_telemetry = 1U; sample_tick = now;
        }
        report_pending = 1U;
    }
    if (MQTT_Core_PublishExTracked(g_mqtt_app_status_report_topic, report_buf, ABoxMqttV4_ReportQos(report_telemetry ? "telemetry" : "state"), 0U, &report_operation)) return;
}

#ifdef TOP_WING_HOST_TEST
void MQTT_V4_TestReset(void)
{
    memset(requests, 0, sizeof(requests));
    bound_device_id[0] = 0;
    boot_seed = report_sequence = sample_tick = 0U;
    MQTT_V4_ResetReports();
}
#endif

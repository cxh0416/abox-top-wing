#include "mqtt_v4.h"

#include <stdio.h>
#include <string.h>
#include "abox_cloud_response.h"
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
    uint8_t digest[32];
    uint64_t timestamp, protect_until;
    uint32_t tick;
    uint16_t code;
    int8_t store_slot;
    uint8_t valid, inflight, sync, retry_response;
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

static const ABoxMqttCommandDescriptor commands[] = {{"wing_control", "wing", ABOX_MQTT_COMMAND_DISCRETE_ACTION}};
static const ABoxMqttReportDescriptor reports[] = {{"wing_status", "wing", "state"}, {"wing_position", "wing", "telemetry"}};
static const ABoxMqttProfileDescriptor profile = {"wing", "1.0", commands, 1U, reports, 2U};

uint8_t MQTT_V4_ValidDeviceId(const char* value)
{
    char topic[128];
    return ABoxMqttV4_BuildTopic(topic, sizeof(topic), value, ABOX_MQTT_V4_TOPIC_REQUEST);
}

static int request_id_valid(const char* id)
{
    size_t n = id ? strlen(id) : 0U;
    if (!n || n > 64U) return 0;
    for (size_t i = 0; i < n; ++i)
    {
        char c = id[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && (!i || (c != '.' && c != '_' && c != ':' && c != '-'))) return 0;
    }
    return 1;
}

static int integer(const cJSON* node, uint64_t* value)
{
    if (!cJSON_IsNumber(node) || node->valuedouble < 0 || node->valuedouble > 9007199254740991.0) return 0;
    *value = (uint64_t)node->valuedouble;
    return (double)*value == node->valuedouble;
}

static int count_bytes(void* context, const char* bytes, size_t length)
{
    size_t* count = context;
    (void)bytes;
    *count += length;
    return *count <= 768U;
}

void MQTT_V4_IdentityChanged(const char* id)
{
    WingReport old_report;
    WingControlResult old_result;
    if (!strcmp(bound_device_id, id)) return;
    memset(requests, 0, sizeof(requests));
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

static int sync_result(void* context, char* out, size_t capacity)
{
    static const char json[] = "{\"expectedStates\":[{\"reportName\":\"wing_status\"}]}";
    (void)context;
    if (sizeof(json) > capacity) return 0;
    memcpy(out, json, sizeof(json));
    return 1;
}

static uint8_t respond(const char* id, uint16_t code, uint64_t timestamp, uint8_t sync)
{
    char ts[21];
    if (!ABoxMqttV4_FormatInteger(ts, sizeof(ts), timestamp) ||
        !ABoxCloudResponse_EncodeResult(response_buf, sizeof(response_buf), MQTT_APP_PUBLIC_VERSION_STR, ts, NULL,
            id, code, message(code), sync && code == 200U ? sync_result : NULL, NULL)) return 0U;
    return MQTT_App_QueuePublicResponse(response_buf, id, code);
}

static Request* find(const char* id)
{
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
        if (requests[i].valid && !strcmp(requests[i].id, id)) return &requests[i];
    return NULL;
}

static Request* reserve(void)
{
    ABoxClockSnapshot clock = TimeBase_GetSnapshot();
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
    {
        Request* r = &requests[i];
        if (!r->valid || (!r->inflight && !r->retry_response &&
            (r->protect_until ? (clock.quality == ABOX_CLOCK_SYNCHRONIZED && clock.utc_ms > clock.error_ms &&
                clock.utc_ms - clock.error_ms > r->protect_until) : HAL_GetTick() - r->tick > 600000U)))
        { memset(r, 0, sizeof(*r)); r->store_slot = -1; return r; }
    }
    return NULL;
}

static void complete(Request* r, uint16_t code)
{
    r->timestamp = TimeBase_GetMs();
    if (r->store_slot >= 0 && !MQTT_V4_StoreComplete(r->store_slot, code, r->timestamp)) code = 5001U;
    r->code = code; r->inflight = 0U;
    r->retry_response = !respond(r->id, code, r->timestamp, r->sync);
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
    cJSON *root, *data, *id, *version, *command, *params, *expires;
    uint64_t timestamp, expires_at = 0;
    uint8_t digest[32];
    WingAction action = WING_ACTION_NONE;
    uint16_t code = 4001U, operation_error = 0U;
    uint8_t sync;
    Request* r;
    MqttV4Record saved;
    int store_slot = -1, match;
    size_t params_size = 0U;
    if (!text || strlen(text) > 1024U) return;
    root = ABoxMqttDigest_Parse(text, strlen(text));
    if (!root) return;
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    id = cJSON_GetObjectItemCaseSensitive(data, "requestId");
    if (!cJSON_IsString(id) || !request_id_valid(id->valuestring)) goto done;
    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    command = cJSON_GetObjectItemCaseSensitive(data, "command");
    params = cJSON_GetObjectItemCaseSensitive(data, "params");
    expires = cJSON_GetObjectItemCaseSensitive(data, "expiresAt");
    if (!cJSON_IsObject(root) || !cJSON_IsObject(data) || !cJSON_IsString(version) ||
        strcmp(version->valuestring, MQTT_APP_PUBLIC_VERSION_STR) || !integer(cJSON_GetObjectItemCaseSensitive(root, "timestamp"), &timestamp) ||
        !cJSON_IsString(command) || strlen(command->valuestring) > 64U ||
        (!cJSON_IsObject(params) && !cJSON_IsNull(params)) || (expires && !integer(expires, &expires_at)) ||
        !ABoxMqttDigest_Request(data, NULL, digest) ||
        !ABoxMqttDigest_Canonical(params, count_bytes, &params_size)) goto error;
    sync = !strcmp(command->valuestring, "sync_state");
    if (!strcmp(command->valuestring, "wing_control"))
    {
        const cJSON* a = cJSON_GetObjectItemCaseSensitive(params, "action");
        if (!cJSON_IsString(a)) operation_error = 4001U;
        else if (!strcmp(a->valuestring, "ascend")) action = WING_ACTION_ASCEND;
        else if (!strcmp(a->valuestring, "descend")) action = WING_ACTION_DESCEND;
        else if (!strcmp(a->valuestring, "stop")) action = WING_ACTION_STOP;
        else operation_error = 4001U;
    }
    else if (!sync) operation_error = 4002U;
    r = find(id->valuestring);
    if (r && memcmp(r->digest, digest, sizeof(digest))) { code = 4006U; goto error; }
    if (r && r->inflight) goto done;
    if (r && action != WING_ACTION_STOP)
    {
        if (sync && r->code == 200U && !queue_sync(r->id)) { code = 4003U; goto error; }
        r->retry_response = !respond(r->id, r->code, r->timestamp, r->sync);
        goto done;
    }
    match = MQTT_V4_StoreFind(g_mqtt_app_device_id, id->valuestring, &saved, &store_slot);
    if (match > 0)
    {
        if (memcmp(saved.digest, digest, sizeof(digest))) { code = 4006U; goto error; }
        if (match == 2) (void)respond(id->valuestring, (uint16_t)saved.code, saved.timestamp, 0U);
        else { code = 4003U; goto error; }
        goto done;
    }
    if (match < 0 && action != WING_ACTION_STOP && !sync) { code = 5001U; goto error; }
    if (operation_error) { code = operation_error; goto error; }
    if (sync ? (!cJSON_IsNull(params) || expires) :
        (action == WING_ACTION_STOP ? expires != NULL :
            (!expires || !ABoxMqttV4_RequestLifetimeValid(timestamp, expires_at, 5000U)))) goto error;
    if (action == WING_ACTION_ASCEND || action == WING_ACTION_DESCEND)
    {
        code = TimeBase_CheckExpiry(expires_at);
        if (code != 200U) goto error;
    }
    if (!r) r = reserve();
    if (!r && action == WING_ACTION_STOP)
    {
        /* Safety stop never waits for ordinary cache capacity. */
        for (unsigned i = 0; i < CACHE_COUNT; ++i)
            if (!requests[i].inflight && !requests[i].protect_until) { r = &requests[i]; break; }
    }
    if (!r) { code = 4003U; goto error; }
    memset(r, 0, sizeof(*r));
    strcpy(r->id, id->valuestring); memcpy(r->digest, digest, sizeof(digest));
    r->valid = r->inflight = 1U; r->sync = sync; r->action = action; r->store_slot = -1; r->tick = HAL_GetTick();
    if (sync) { complete(r, queue_sync(r->id) ? 200U : 4003U); goto done; }
    if (action != WING_ACTION_STOP)
    {
        if (WingController_GetSnapshot()->can_session != WING_CAN_SESSION_IDLE_SILENT)
        { complete(r, 4003U); goto done; }
        store_slot = MQTT_V4_StoreBegin(g_mqtt_app_device_id, r->id, r->digest, expires_at);
        if (store_slot < 0) { complete(r, store_slot == -1 ? 4003U : 5001U); goto done; }
        r->store_slot = (int8_t)store_slot;
        r->protect_until = ABoxMqttV4_IdempotencyProtectUntil(expires_at);
        code = TimeBase_CheckExpiry(expires_at);
        if (code != 200U) { complete(r, code); goto done; }
    }
    code = WingController_Request(action, r->id);
    if (code)
    {
        if (action == WING_ACTION_STOP && code == 200U) code = 5003U; /* Idle-silent has no new stop feedback. */
        complete(r, code);
    }
    goto done;
error:
    (void)respond(id->valuestring, code, TimeBase_GetMs(), 0U);
done:
    cJSON_Delete(root);
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

static cJSON* envelope(cJSON** data, uint32_t captured_tick)
{
    char ts[21];
    uint64_t timestamp = TimeBase_GetMs();
    uint32_t age = HAL_GetTick() - captured_tick;
    timestamp = age <= INT32_MAX && timestamp >= age ? timestamp - age : 0U;
    cJSON* root = cJSON_CreateObject();
    *data = cJSON_CreateObject();
    if (!root || !*data || !ABoxMqttV4_FormatInteger(ts, sizeof(ts), timestamp))
    { cJSON_Delete(root); cJSON_Delete(*data); *data = NULL; return NULL; }
    if (!cJSON_AddStringToObject(root, "version", MQTT_APP_PUBLIC_VERSION_STR) || !cJSON_AddRawToObject(root, "timestamp", ts) ||
        !cJSON_AddItemToObject(root, "data", *data)) { cJSON_Delete(root); cJSON_Delete(*data); *data = NULL; return NULL; }
    return root;
}

static int encode_state(const WingSnapshot* snapshot, const char* request_id, uint8_t telemetry, uint32_t captured_tick)
{
    char id[97];
    cJSON *data, *root = envelope(&data, captured_tick), *payload = cJSON_CreateObject();
    int ok = 0;
    if (!root || !payload) goto done;
    if (!boot_seed) boot_seed = TimeBase_GetMs() ^ (ABox_PortGetMonotonicMs() << 16U) ^ ((uint64_t)HAL_GetUIDw0() << 32U) ^ HAL_GetUIDw1();
    snprintf(id, sizeof(id), "%08lX%08lX-%lu", (unsigned long)(boot_seed >> 32U), (unsigned long)boot_seed, (unsigned long)++report_sequence);
    cJSON_AddStringToObject(data, "reportId", id);
    cJSON_AddStringToObject(data, "reportType", telemetry ? "telemetry" : "state");
    cJSON_AddStringToObject(data, "reportName", reports[telemetry ? 1U : 0U].name);
    if (request_id && *request_id) cJSON_AddStringToObject(data, "requestId", request_id);
    if (telemetry)
    {
        if (snapshot->have_position) cJSON_AddNumberToObject(payload, "positionMm", snapshot->position_mm); else cJSON_AddNullToObject(payload, "positionMm");
        cJSON_AddBoolToObject(payload, "statusFresh", snapshot->status_fresh);
    }
    else MQTT_V4_AddSnapshot(payload, snapshot);
    if (cJSON_GetArraySize(payload) != (telemetry ? 2 : 10) ||
        !cJSON_AddItemToObject(data, "payload", payload)) goto done;
    payload = NULL;
    if (cJSON_GetArraySize(data) != (request_id && *request_id ? 5 : 4)) goto done;
    ok = cJSON_PrintPreallocated(root, report_buf, sizeof(report_buf), 0) && strlen(report_buf) <= 1024U;
done:
    cJSON_Delete(payload); cJSON_Delete(root);
    return ok;
}

static int publish_meta(ABoxCloudJobKind kind, uint64_t* operation)
{
    cJSON *data, *root = envelope(&data, HAL_GetTick());
    int ok = 0;
    if (!root) return 0;
    cJSON_AddStringToObject(data, "firmware", TopFlyingWing_AppFwVersion());
    if (kind == ABOX_CLOUD_JOB_MANIFEST)
    {
        cJSON* profiles = cJSON_AddArrayToObject(data, "profiles");
        cJSON* entry = cJSON_CreateObject();
        if (!profiles || !entry) { cJSON_Delete(entry); goto done; }
        if (!cJSON_AddStringToObject(entry, "name", profile.name) ||
            !cJSON_AddStringToObject(entry, "version", profile.version) ||
            !cJSON_AddItemToArray(profiles, entry)) { cJSON_Delete(entry); goto done; }
    }
    if (cJSON_GetArraySize(data) != (kind == ABOX_CLOUD_JOB_MANIFEST ? 2 : 1)) goto done;
    ok = cJSON_PrintPreallocated(root, meta_buf, sizeof(meta_buf), 0) &&
        MQTT_Core_PublishExTracked(kind == ABOX_CLOUD_JOB_MANIFEST ? g_mqtt_app_manifest_topic : g_mqtt_app_heartbeat_topic,
            meta_buf, kind == ABOX_CLOUD_JOB_MANIFEST ? 1U : 0U, kind == ABOX_CLOUD_JOB_MANIFEST ? 1U : 0U, operation);
    if (ok && kind == ABOX_CLOUD_JOB_HEARTBEAT) { g_mqtt_app_last_hb_tick = HAL_GetTick(); g_mqtt_app_hb_pending = 0U; }
done:
    cJSON_Delete(root);
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
    boot_seed = report_sequence = sample_tick = 0U;
    MQTT_V4_ResetReports();
}
#endif

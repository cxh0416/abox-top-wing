#include "mqtt_v4.h"
#include "mqtt_v4_report_internal.h"

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
#include "wing_power.h"

#define CACHE_COUNT 12U
#define ORDINARY_CACHE_COUNT (CACHE_COUNT - 1U)
typedef enum { POWER_NONE, POWER_ON, POWER_OFF } PowerAction;
typedef struct
{
    char id[65];
    char device_id[33];
    uint8_t digest[32];
    uint64_t timestamp;
    uint64_t expires_at;
    uint32_t tick;
    uint16_t code;
    int8_t store_slot;
    uint8_t valid, inflight, sync, retry_response, awaiting_dispatch, completing, cancelled, has_expiry;
    WingAction action;
    PowerAction power;
} Request;
static Request requests[CACHE_COUNT];
static char response_buf[MQTT_APP_JSON_BUF_SZ];
static char bound_device_id[33];

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

static int power_action(void* context, const cJSON* params)
{
    (void)context;
    return cJSON_IsNull(params);
}

static const ABoxMqttOperation operations[] = {
    {"wing_control", "wing", ABOX_MQTT_COMMAND_DISCRETE_ACTION, ABOX_MQTT_EXPIRY_REQUIRED, 5000U, direction, NULL, NULL},
    {"wing_control", "wing", ABOX_MQTT_COMMAND_SAFETY_STOP, ABOX_MQTT_EXPIRY_FORBIDDEN, 0U, stopping, NULL, NULL},
    {"power_on", "device_power", ABOX_MQTT_COMMAND_DISCRETE_ACTION, ABOX_MQTT_EXPIRY_REQUIRED, 5000U, power_action, NULL, NULL},
    {"power_off", "device_power", ABOX_MQTT_COMMAND_SAFETY_STOP, ABOX_MQTT_EXPIRY_OPTIONAL, 0U, power_action, NULL, NULL}
};

void MQTT_V4_IdentityChanged(const char* id)
{
    WingControlResult old_result;
    if (!strcmp(bound_device_id, id)) return;
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
    {
        Request* r = &requests[i];
        if (r->inflight && r->store_slot >= 0) r->cancelled = 1U;
        else memset(r, 0, sizeof(*r));
    }
    strcpy(bound_device_id, id);
    MQTT_V4_ReportIdentityChanged();
    /* Configuration changes are admitted only while control is idle. */
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

static int encode_response(const char* id, uint16_t code, uint64_t timestamp, uint8_t sync, PowerAction power)
{
    cJSON* result = sync && code == 200U ? cJSON_Parse("{\"expectedStates\":[{\"reportName\":\"wing_status\"},{\"reportName\":\"power_status\"}]}") : NULL;
    if (code == 200U && power != POWER_NONE)
    {
        result = cJSON_CreateObject();
        if (result && !cJSON_AddBoolToObject(result, "powerOutputOn", power == POWER_ON))
        { cJSON_Delete(result); result = NULL; }
    }
    if (code == 200U && (sync || power != POWER_NONE) && !result) return 0;
    uint16_t encoded = ABoxCloudV4_Response(response_buf, sizeof(response_buf), timestamp, id, code, message(code), result);
    cJSON_Delete(result);
    return encoded == code;
}

static uint8_t respond(const char* id, uint16_t code, uint64_t timestamp, uint8_t sync, PowerAction power)
{
    return encode_response(id, code, timestamp, sync, power) && MQTT_App_QueuePublicResponse(response_buf, id, code);
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
    Request* oldest_stop = NULL;
    uint32_t now = HAL_GetTick();
    for (unsigned i = 0; i < ORDINARY_CACHE_COUNT; ++i)
    {
        Request* r = &requests[i];
        if (!r->valid || (!r->inflight && !r->retry_response && now - r->tick > 600000U))
        { memset(r, 0, sizeof(*r)); r->store_slot = -1; return r; }
        if (!r->inflight && !r->retry_response && !r->awaiting_dispatch && !r->completing &&
            r->store_slot < 0 && r->action == WING_ACTION_STOP &&
            (!oldest_stop || now - r->tick > now - oldest_stop->tick)) oldest_stop = r;
    }
    if (oldest_stop) { memset(oldest_stop, 0, sizeof(*oldest_stop)); oldest_stop->store_slot = -1; }
    return oldest_stop;
}

static void complete(Request* r, uint16_t code)
{
    r->timestamp = TimeBase_GetReportedMs();
    if (r->action == WING_ACTION_STOP) r->tick = HAL_GetTick();
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
    r->retry_response = !strcmp(r->device_id, g_mqtt_app_device_id) && !respond(r->id, code, r->timestamp, r->sync, r->power);
}

void MQTT_V4_CancelPendingMotion(void)
{
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
        if (requests[i].awaiting_dispatch && requests[i].power == POWER_NONE) requests[i].cancelled = 1U;
}

static uint8_t power_off_pending(void)
{
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
        if (requests[i].inflight && !requests[i].completing && requests[i].power == POWER_OFF) return 1U;
    return 0U;
}

static uint8_t power_request_pending(void)
{
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
        if (requests[i].inflight && !requests[i].completing && requests[i].power != POWER_NONE) return 1U;
    return 0U;
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
            uint16_t code = r->cancelled ? 4003U :
                ABoxMqttIdem_TakeExecutable(cache, index, &clock);
            r->awaiting_dispatch = 0U;
            if (code == 200U && r->power == POWER_ON)
            {
                WingPower_Set(1U);
                MQTT_V4_QueuePowerReport(r->id, 1U);
            }
            else if (code == 200U)
            {
                if (power_off_pending() ||
                    WingController_GetSnapshot()->can_session != WING_CAN_SESSION_IDLE_SILENT ||
                    WingController_RejectRemoteStartForUltrasonic()) code = 4003U;
                else code = WingController_Request(r->action, r->id);
            }
            if (code) complete(r, code);
        }
        if (r->completing)
        {
            if (cache->entries[index].state == ABOX_IDEM_PREPARED || cache->entries[index].state == ABOX_IDEM_DISPATCHED)
            {
                if (encode_response(r->id, r->code, r->timestamp, 0U, r->power))
                    (void)ABoxMqttIdem_Complete(cache, index, response_buf, strlen(response_buf));
            }
            const char* response = ABoxMqttIdem_Response(cache, index, NULL);
            if (response && (strcmp(r->device_id, g_mqtt_app_device_id) || MQTT_App_QueuePublicResponse(response, r->id, r->code)))
            { r->completing = r->inflight = r->valid = 0U; }
        }
    }
}

void MQTT_V4_RetryResponses(void)
{
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
        if (requests[i].retry_response && respond(requests[i].id, requests[i].code, requests[i].timestamp, requests[i].sync, requests[i].power)) requests[i].retry_response = 0U;
}

static void untracked_stop(const char* id, uint8_t report_error)
{
    uint16_t code;
    MQTT_V4_CancelPendingMotion();
    code = WingController_RequestUntrackedStop();
    if (code && report_error)
        (void)respond(id, code == 200U ? 5003U : code, TimeBase_GetReportedMs(), 0U, POWER_NONE);
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
    PowerAction power = POWER_NONE;
    int legacy;
    code = ABoxMqttRequest_Decode(&request, text, text ? strlen(text) : 0U, NULL, NULL);
    if (code != 200U) goto error;
    operation_error = ABoxMqttRequest_Resolve(&request, operations, sizeof(operations)/sizeof(operations[0]), &operation);
    sync = !strcmp(request.command, "sync_state");
    if (operation)
    {
        if (!strcmp(request.command, "power_on")) power = POWER_ON;
        else if (!strcmp(request.command, "power_off")) power = POWER_OFF;
        else if (operation->classification == ABOX_MQTT_COMMAND_SAFETY_STOP) action = WING_ACTION_STOP;
        else if (operation->classification == ABOX_MQTT_COMMAND_DISCRETE_ACTION)
            action = !strcmp(cJSON_GetObjectItemCaseSensitive(request.params, "action")->valuestring, "ascend") ?
                WING_ACTION_ASCEND : WING_ACTION_DESCEND;
    }
    r = find(request.request_id);
    if (r && memcmp(r->digest, request.digest, sizeof(request.digest))) { code = 4006U; goto error; }
    if (r && r->inflight) goto done;
    if (r && r->retry_response && action == WING_ACTION_STOP)
    {
        if (!respond(r->id, r->code, r->timestamp, r->sync, r->power))
        { untracked_stop(request.request_id, 0U); goto done; }
        r->retry_response = 0U;
    }
    if (r && r->store_slot < 0 && action != WING_ACTION_STOP && power != POWER_OFF)
    {
        if (sync && r->code == 200U && !MQTT_V4_QueueSync(r->id)) { code = 4003U; goto error; }
        r->retry_response = !respond(r->id, r->code, r->timestamp, r->sync, r->power);
        goto done;
    }
    /* Compatibility is read-only: the previous Flash reservations retain
     * their original protection; every new reservation uses the shared engine. */
    legacy = MQTT_V4_StoreFind(g_mqtt_app_device_id, request.request_id, &saved, NULL);
    if (legacy > 0)
    {
        if (memcmp(saved.digest, request.digest, sizeof(request.digest))) { code = 4006U; goto error; }
        if (legacy == 2) (void)respond(request.request_id, (uint16_t)saved.code, saved.timestamp, 0U, POWER_NONE);
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
    if ((power == POWER_ON && power_request_pending()) || (power == POWER_OFF && power_off_pending()))
    { code = 4003U; goto error; }
    if (action != WING_ACTION_STOP && power != POWER_OFF && !sync && (legacy < 0 || match == ABOX_IDEM_UNAVAILABLE))
    { code = legacy < 0 || !cache || cache->failed ? 5001U : 4003U; goto error; }
    ABoxClockSnapshot clock = TimeBase_GetSnapshot();
    code = ABoxMqttRequest_CheckNew(&request, operation, &clock);
    if (code != 200U) goto error;
    if (!r) r = reserve();
    if (!r && action == WING_ACTION_STOP)
    {
        Request* stop_slot = &requests[ORDINARY_CACHE_COUNT];
        if (!stop_slot->valid || (!stop_slot->inflight && !stop_slot->retry_response &&
            !stop_slot->awaiting_dispatch && !stop_slot->completing && stop_slot->store_slot < 0)) r = stop_slot;
        else { untracked_stop(request.request_id, 1U); goto done; }
    }
    if (!r) { code = 4003U; goto error; }
    memset(r, 0, sizeof(*r));
    strcpy(r->id, request.request_id); memcpy(r->digest, request.digest, sizeof(request.digest));
    strcpy(r->device_id, g_mqtt_app_device_id);
    r->valid = r->inflight = 1U; r->sync = sync; r->action = action; r->power = power;
    r->expires_at = request.expires_at; r->has_expiry = request.has_expires_at; r->store_slot = -1; r->tick = HAL_GetTick();
    if (sync) { complete(r, MQTT_V4_QueueSync(r->id) ? 200U : 4003U); goto done; }
    if (power == POWER_OFF)
    {
        MQTT_V4_CancelPendingMotion();
        for (unsigned i = 0; i < CACHE_COUNT; ++i)
            if (requests[i].power == POWER_ON && requests[i].awaiting_dispatch) requests[i].cancelled = 1U;
        if (!WingPower_IsOn()) { MQTT_V4_QueuePowerReport(r->id, 0U); complete(r, 200U); }
        else { code = WingController_CheckPowerOff(r->id); if (code) complete(r, code); }
    }
    else if (action == WING_ACTION_STOP)
    {
        /* A stop also cancels an intention still waiting for durable storage.
         * It must never start later after this stop has completed. */
        MQTT_V4_CancelPendingMotion();
        code = WingController_Request(action, r->id);
        if (code) complete(r, code == 200U ? 5003U : code);
    }
    else
    {
        if (power == POWER_NONE && (power_off_pending() || !WingPower_IsOn() ||
            WingController_GetSnapshot()->can_session != WING_CAN_SESSION_IDLE_SILENT ||
            WingController_RejectRemoteStartForUltrasonic()))
        { complete(r, 4003U); goto done; }
        code = ABoxMqttIdem_Begin(cache, key, request.digest, request.expires_at, &clock, &index);
        if (code != 200U) { complete(r, code); goto done; }
        r->store_slot = (int8_t)index;
        r->awaiting_dispatch = 1U;
    }
    goto done;
error:
    if (request.request_id) (void)respond(request.request_id, code, TimeBase_GetReportedMs(), 0U, POWER_NONE);
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
    if (r->power == POWER_OFF && code == 200U)
    {
        ABoxClockSnapshot clock = TimeBase_GetSnapshot();
        const WingSnapshot* s = &result->snapshot;
        code = r->cancelled || !s->status_fresh || !s->have_position || !s->have_limits ||
            s->motion_state != WING_MOTION_STOPPED || s->at_endpoint != WING_ENDPOINT_BOTTOM ||
            HAL_GetTick() - result->observed_tick > WING_ACTIVE_FEEDBACK_TIMEOUT_MS ?
            4003U : (r->has_expiry ? ABoxMqttV4_CheckExpiry(&clock, r->expires_at) : 200U);
        if (code == 200U)
        {
            WingPower_Set(0U);
            MQTT_V4_QueuePowerReport(r->id, 0U);
        }
    }
    complete(r, code);
}

#ifdef TOP_WING_HOST_TEST
void MQTT_V4_TestReset(void)
{
    memset(requests, 0, sizeof(requests));
    bound_device_id[0] = 0;
    MQTT_V4_ReportTestReset();
}
#endif

#include "mqtt_v4.h"
#include "mqtt_v4_report_internal.h"
#include <stdio.h>
#include <string.h>
#include "abox_cloud_v4.h"
#include "abox_cloud_service.h"
#include "abox_platform_port.h"
#include "app_version.h"
#include "main.h"
#include "mqtt_app_int.h"
#include "mqtt_core.h"
#include "time_base.h"
#include "wing_power.h"

static char report_buf[MQTT_APP_JSON_BUF_SZ];
static char meta_buf[320];
static uint8_t report_is_heartbeat;
static ABoxCloudBootstrap bootstrap;
static uint32_t ready_sequence, report_sequence;
static uint64_t report_operation, boot_seed;
static uint8_t report_pending, report_telemetry;
static uint8_t retry_snapshot;
typedef struct { char request_id[65]; uint32_t tick; uint8_t on; } PowerReport;
static PowerReport power_reports[8];
static uint8_t power_report_head, power_report_count, report_power_event, report_sync;
static char sync_queue[2][65];
static uint8_t sync_count, sync_stage;
static WingReport report;
static uint32_t sample_tick;
static const ABoxMqttCommandDescriptor commands[] = {{"wing_control", "wing", ABOX_MQTT_COMMAND_PARAMETERIZED}};
static const ABoxMqttReportDescriptor reports[] = {{"wing_status", "wing", "state"}, {"wing_position", "wing", "telemetry"}};
static const ABoxMqttProfileDescriptor profile = {"wing", "1.0", commands, 1U, reports, 2U};

void MQTT_V4_QueuePowerReport(const char* request_id, uint8_t on)
{
    uint8_t tail;
    if (power_report_count == 8U)
    {
        /* Do not overwrite a report currently awaiting its QoS 1 receipt. */
        if (report_power_event) return;
        power_report_head = (uint8_t)((power_report_head + 1U) % 8U);
        --power_report_count;
    }
    tail = (uint8_t)((power_report_head + power_report_count) % 8U);
    power_reports[tail].request_id[0] = '\0';
    if (request_id) strncpy(power_reports[tail].request_id, request_id, 64U);
    power_reports[tail].request_id[64] = '\0';
    power_reports[tail].on = on ? 1U : 0U;
    power_reports[tail].tick = HAL_GetTick();
    ++power_report_count;
}

int MQTT_V4_QueueSync(const char* id)
{
    for (unsigned i = 0; i < sync_count; ++i) if (!strcmp(sync_queue[i], id)) return 1;
    if (sync_count == 2U) return 0;
    strcpy(sync_queue[sync_count++], id);
    return 1;
}

void MQTT_V4_ReportIdentityChanged(void)
{
    WingReport old_report;
    sync_count = sync_stage = power_report_head = power_report_count = 0U;
    if (report_sync || report_power_event)
    {
        report_pending = report_sync = report_power_event = 0U;
        report_operation = 0U;
    }
    while (WingController_TakeReport(&old_report)) { }
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
    uint64_t timestamp = TimeBase_GetReportedMs();
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

static int encode_power_state(const char* request_id, uint8_t on, uint32_t captured_tick)
{
    char id[97];
    cJSON* payload = cJSON_CreateObject();
    int ok = 0;
    if (!payload) return 0;
    if (!boot_seed) boot_seed = TimeBase_GetMs() ^ (ABox_PortGetMonotonicMs() << 16U) ^ ((uint64_t)HAL_GetUIDw0() << 32U) ^ HAL_GetUIDw1();
    snprintf(id, sizeof(id), "%08lX%08lX-%lu", (unsigned long)(boot_seed >> 32U), (unsigned long)boot_seed, (unsigned long)++report_sequence);
    const ABoxCloudV4Report header = {.report_id = id, .type = "state", .name = "power_status",
        .request_id = request_id && *request_id ? request_id : NULL, .timestamp = captured_time(captured_tick)};
    if (cJSON_AddBoolToObject(payload, "powerOutputOn", on))
        ok = ABoxCloudV4_Report(report_buf, sizeof(report_buf), &header, payload);
    cJSON_Delete(payload);
    return ok;
}

static int publish_meta(ABoxCloudJobKind kind, uint64_t* operation)
{
    const ABoxCloudV4Profile enabled[] = {
        {profile.name, profile.version, NULL, 0U}, {"device_power", "1.0", NULL, 0U}
    };
    int encoded = kind == ABOX_CLOUD_JOB_MANIFEST ?
        ABoxCloudV4_Manifest(meta_buf, sizeof(meta_buf), TimeBase_GetReportedMs(), TopFlyingWing_AppFwVersion(), enabled, 2U, 0) :
        ABoxCloudV4_Heartbeat(meta_buf, sizeof(meta_buf), TimeBase_GetReportedMs(), TopFlyingWing_AppFwVersion(), NULL);
    int ok = encoded && MQTT_Core_PublishExTracked(kind == ABOX_CLOUD_JOB_MANIFEST ? g_mqtt_app_manifest_topic : g_mqtt_app_heartbeat_topic,
        meta_buf, kind == ABOX_CLOUD_JOB_MANIFEST ? 1U : 0U, kind == ABOX_CLOUD_JOB_MANIFEST ? 1U : 0U, operation);
    if (ok && kind == ABOX_CLOUD_JOB_HEARTBEAT) { g_mqtt_app_last_hb_tick = HAL_GetTick(); g_mqtt_app_hb_pending = 0U; }
    return ok;
}

void MQTT_V4_ResetReports(void)
{
    ABoxCloudBootstrap_Init(&bootstrap);
    report_pending = report_telemetry = report_is_heartbeat = retry_snapshot = 0U; report_operation = 0U; ready_sequence = 0U; sync_count = sync_stage = 0U;
    power_report_head = power_report_count = report_power_event = report_sync = 0U;
}

uint8_t MQTT_V4_AnnouncementReady(void) { return bootstrap.ready && !ABoxCloudBootstrap_Active(&bootstrap); }

void MQTT_V4_ReportTask(void)
{
    ABoxCloudJob job;
    ABoxMqttReceiptStatus receipt;
    uint32_t now = HAL_GetTick();
    MQTT_V4_RetryResponses();
    if (!MQTT_Core_IsReady())
    {
        ABoxCloudBootstrap_SetReady(&bootstrap, 0);
        report_operation = 0U; report_pending = report_is_heartbeat = retry_snapshot = report_power_event = report_sync = 0U;
        return;
    }
    if (ready_sequence != MQTT_Core_GetReadySeq())
    {
        if (ready_sequence) ABoxCloudBootstrap_NetworkChanged(&bootstrap);
        ready_sequence = MQTT_Core_GetReadySeq();
        report_operation = 0U; report_pending = report_is_heartbeat = retry_snapshot = report_power_event = report_sync = 0U;
    }
    ABoxCloudBootstrap_SetReady(&bootstrap, 1);
    if (bootstrap.in_flight)
    {
        receipt = MQTT_Core_PublishResult(bootstrap.operation);
        if (receipt == ABOX_MQTT_RECEIPT_PENDING) return;
        ABoxCloudJobKind completed = ABoxCloudBootstrap_Receipt(&bootstrap, bootstrap.generation, bootstrap.operation, receipt == ABOX_MQTT_RECEIPT_CONFIRMED);
        if (completed == ABOX_CLOUD_JOB_HEARTBEAT) g_mqtt_app_last_hb_ok_tick = now;
        if (completed == ABOX_CLOUD_JOB_STATE) MQTT_V4_QueuePowerReport(NULL, WingPower_IsOn());
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
        else if (receipt == ABOX_MQTT_RECEIPT_CONFIRMED || report_telemetry)
        {
            report_pending = 0U;
            if (report_power_event && power_report_count)
            { power_report_head = (uint8_t)((power_report_head + 1U) % 8U); --power_report_count; }
            if (report_sync && sync_count)
            {
                if (!sync_stage) sync_stage = 1U;
                else { sync_stage = 0U; if (--sync_count) strcpy(sync_queue[0], sync_queue[1]); }
            }
            report_power_event = report_sync = 0U;
        }
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
            id = sync_queue[0];
            if (sync_stage) { if (!encode_power_state(id, WingPower_IsOn(), now)) return; }
            else
            {
                memset(&report, 0, sizeof(report)); WingController_CopySnapshot(&report.snapshot);
                if (!encode_state(&report.snapshot, id, 0U, now)) return;
            }
            report_sync = 1U;
        }
        else if (power_report_count)
        {
            const PowerReport* p = &power_reports[power_report_head];
            if (!encode_power_state(p->request_id, p->on, p->tick)) return;
            report_power_event = 1U;
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
void MQTT_V4_ReportTestReset(void)
{
    boot_seed = report_sequence = sample_tick = 0U;
    MQTT_V4_ResetReports();
}
#endif

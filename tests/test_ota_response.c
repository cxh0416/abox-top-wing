#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "abox_boot_v2.h"
#include "board_config.h"
#include "boot_state.h"
#include "cJSON.h"
#include "mqtt_app.h"
#include "mqtt_app_int.h"
#include "mqtt_core.h"
#include "ota_service.h"
#include "wing_controller.h"

static uint32_t fake_tick;
static BoardConfigStore_t info_config;
static uint8_t maximal_info;
static uint8_t mqtt_ready;
static uint8_t ota_ready = 1U;
static uint8_t ota_busy;
static uint8_t ota_start_ok = 1U;
static uint8_t ota_install_ready;
static uint8_t ota_failed;
static uint32_t ota_error;
static uint16_t wing_request_code;
static ABoxMqttReceiptStatus publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
static uint64_t publish_operation;
static unsigned stale_logs;

char g_mqtt_app_device_id[64];
char g_mqtt_app_heartbeat_topic[128];
char g_mqtt_app_ctrl_topic[128];
char g_mqtt_app_ctrl_resp_topic[128];
char g_mqtt_app_status_report_topic[128];
char g_mqtt_app_internal_config_topic[128];
char g_mqtt_app_internal_config_resp_topic[128] = "/config/resp";
char g_mqtt_app_internal_ota_topic[128];
char g_mqtt_app_internal_ota_resp_topic[128] = "/ota/resp";
char g_mqtt_app_log_topic[128];
uint32_t g_mqtt_app_last_hb_tick;
uint32_t g_mqtt_app_last_hb_ok_tick;
uint32_t g_mqtt_app_seq_no;
volatile uint8_t g_mqtt_app_hb_pending;
volatile uint8_t g_mqtt_app_resp_pending;
volatile uint8_t g_mqtt_app_resp_publish_active;
uint32_t g_mqtt_app_resp_pub_ok_seq;
uint32_t g_mqtt_app_resp_tick;
char g_mqtt_app_resp_topic[128];
char g_mqtt_app_resp_payload[MQTT_APP_JSON_BUF_SZ];
char g_mqtt_app_resp_request_id[65];
uint16_t g_mqtt_app_resp_code;
volatile uint8_t g_mqtt_app_ota_reset_pending;
uint32_t g_mqtt_app_ota_reset_tick;
uint32_t g_mqtt_app_ota_request_tick;

void MQTT_App_TestHandleConfig(const char* request_id, cJSON* params);
void MQTT_App_TestHandleOta(const char* request_id, cJSON* params);
uint8_t MQTT_App_TestOtaRequestState(void);
void MQTT_App_HandleCommandMessage(const char* topic, const char* payload);

uint32_t HAL_GetTick(void) { return fake_tick; }
uint8_t MQTT_App_IsMqttConfigTrialActive(void) { return 0U; }
uint8_t MQTT_Core_IsReady(void) { return mqtt_ready; }
uint8_t MQTT_Core_PublishExTracked(const char* topic, const char* payload, uint8_t qos,
                                   uint8_t retain, uint64_t* operation)
{
    assert(topic && payload && qos == 1U && retain == 0U);
    *operation = ++publish_operation;
    return 1U;
}
ABoxMqttReceiptStatus MQTT_Core_PublishResult(uint64_t operation)
{
    assert(operation == publish_operation);
    return publish_receipt;
}
void MQTT_App_ResponseConfirmed(const char* request_id, uint16_t code)
{ (void)request_id; (void)code; }
void OtaService_AcknowledgeRollback(void) {}
uint8_t OtaService_IsReady(void) { return ota_ready; }
uint8_t OtaService_IsBusy(void) { return ota_busy; }
uint8_t OtaService_Start(const OtaServiceRequest_t* request)
{
    return (request && request->size == 1234U) ? ota_start_ok : 0U;
}
uint8_t OtaService_TakeInstallReady(void)
{
    uint8_t value = ota_install_ready;
    ota_install_ready = 0U;
    return value;
}
uint8_t OtaService_TakeFailure(uint32_t* error)
{
    uint8_t value = ota_failed;
    ota_failed = 0U;
    if (error) *error = ota_error;
    return value;
}

uint8_t TimeBase_FormatUtcMs(char* out, uint16_t out_sz)
{
    return (uint8_t)(snprintf(out, out_sz, "%s", maximal_info ? "1793251713000" : "0") > 0);
}
void AppLog_Printf(AppLogLevel_t level, const char* tag, const char* fmt, ...)
{
    if (level == APP_LOG_LEVEL_WARN && !strcmp(tag, "RESP") && strstr(fmt, "stale rid=")) ++stale_logs;
}
void AppLog_Clear(void) {}
uint16_t AppLog_Dump(char* out, uint16_t out_sz)
{
    if (out && out_sz) out[0] = '\0';
    return 0U;
}
const BoardConfigStore_t* BoardConfig_Get(void)
{
    return &info_config;
}
uint8_t BootState_Load(BootStateRecord_t* state) { (void)state; return 0U; }
int ABoxBootV2_DescriptorRead(ABoxBootV2Descriptor* descriptor) { if (maximal_info) { memset(descriptor,0,sizeof(*descriptor)); memset(descriptor->build_version,'b',sizeof(descriptor->build_version)-1); return 1; } return 0; }
int ABoxBootV2_DescriptorValid(const ABoxBootV2Descriptor* descriptor) { (void)descriptor; return maximal_info; }
const char* ABoxBootV2App_StableSeedSource(void) { return maximal_info ? "immutable_revision" : "none"; }
const char* TopFlyingWing_AppFwVersion(void) { return "top-flying-wing-1.0.9"; }
const char* OtaService_Phase(void) { return "IDLE"; }
uint16_t MQTT_App_MqttTrialReply(const char* id,const char* vid,const char* host,
    uint16_t port,const char* user,const char* password)
{ (void)id;(void)vid;(void)host;(void)port;(void)user;(void)password;return 0U; }
uint8_t MQTT_App_ScheduleMqttConfigTrial(const char* request_id, const char* vid, const char* host, uint16_t port,
                                         const char* username, const char* password, uint8_t tls_enabled)
{
    (void)request_id; (void)tls_enabled; (void)vid; (void)host; (void)port; (void)username; (void)password;
    return 0U;
}
const WingSnapshot* WingController_GetSnapshot(void)
{
    static WingSnapshot snapshot;
    return &snapshot;
}
const char* WingAction_Name(WingAction action) { (void)action; return "none"; }
const char* WingMotionState_Name(WingMotionState state) { (void)state; return "stopped"; }
const char* WingSafetyReason_Name(WingSafetyReason reason) { (void)reason; return ""; }
const char* WingEndpoint_Name(WingEndpoint endpoint) { (void)endpoint; return ""; }
const char* WingCanSession_Name(WingCanSession session) { (void)session; return "idle_silent"; }
const char* WingControlOwner_Name(WingControlOwner owner) { (void)owner; return "none"; }
const char* WingOperationPhase_Name(WingOperationPhase phase) { (void)phase; return "idle"; }
uint16_t WingController_Request(WingAction action, const char* request_id)
{
    (void)action; (void)request_id; return wing_request_code;
}
uint8_t WingController_TakeResult(WingControlResult* result) { (void)result; return 0U; }
uint8_t MqttRuntime_IsTlsActive(void) { return 0U; }
uint8_t TimeBase_IsTrusted(void) { return 0U; }
uint8_t MQTT_V4_StoreCanErase(void) { return 1U; }
void MQTT_V4_HandleResult(const WingControlResult* result) { (void)result; }
void MQTT_V4_HandleRequest(const char* text) { (void)text; }

static cJSON* ota_params(void)
{
    cJSON* params = cJSON_CreateObject();
    assert(params);
    cJSON_AddStringToObject(params, "url",
        "https://ota.zxwl.ntiov.com:20443/fw-revisions/top_flying_wing/top-flying-wing-1.0.9/top_flying_wing_app.bin");
    cJSON_AddStringToObject(params, "version", "top-flying-wing-1.0.9");
    cJSON_AddNumberToObject(params, "size", 1234);
    cJSON_AddStringToObject(params, "crc32", "1234ABCD");
    return params;
}

static int response_code(void)
{
    cJSON* root = cJSON_Parse(g_mqtt_app_resp_payload);
    cJSON* data;
    cJSON* code;
    int value;
    assert(root);
    data = cJSON_GetObjectItem(root, "data");
    code = cJSON_GetObjectItem(data, "code");
    assert(cJSON_IsNumber(code));
    value = code->valueint;
    cJSON_Delete(root);
    return value;
}

int main(void)
{
    MQTT_App_ResetResponseState();
    cJSON* info_params=cJSON_CreateObject(); cJSON_AddStringToObject(info_params,"action","get_info");
    char long_request[65]; memset(long_request,'r',64); long_request[64]=0;
    memset(info_config.vid,'V',sizeof(info_config.vid)-1);
    memset(info_config.mqtt_host,'a',sizeof(info_config.mqtt_host)-1);
    info_config.mqtt_port=65535;
    maximal_info=1;
    MQTT_App_TestHandleConfig(long_request,info_params); cJSON_Delete(info_params);
    printf("get_info length=%u code=%u\n",(unsigned)strlen(g_mqtt_app_resp_payload),(unsigned)g_mqtt_app_resp_code);
    assert(response_code()==200U);
    cJSON* info_root=cJSON_Parse(g_mqtt_app_resp_payload);
    cJSON* info=cJSON_GetObjectItem(cJSON_GetObjectItem(info_root,"data"),"result");
    assert(cJSON_IsBool(cJSON_GetObjectItem(info,"mqttTlsActive")));
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(info,"maintenanceProtocols"),"mqttConfigTransaction")->valueint==1);
    cJSON_Delete(info_root); maximal_info=0;

    cJSON* params;

    MQTT_App_ResetResponseState();
    params = ota_params();
    fake_tick = 100U;
    MQTT_App_TestHandleOta("ota-ok", params);
    cJSON_Delete(params);
    assert(MQTT_App_TestOtaRequestState() == 1U);
    assert(!g_mqtt_app_resp_pending);

    ota_install_ready = 1U;
    mqtt_ready = 0U;
    MQTT_App_HandleOtaServiceTask();
    assert(MQTT_App_TestOtaRequestState() == 2U);
    assert(!g_mqtt_app_resp_pending);

    fake_tick = 250U;
    mqtt_ready = 1U;
    MQTT_App_HandleOtaServiceTask();
    assert(MQTT_App_TestOtaRequestState() == 0U);
    assert(g_mqtt_app_resp_pending);
    assert(g_mqtt_app_ota_reset_pending);
    assert(g_mqtt_app_ota_request_tick == 250U);
    assert(response_code() == 200);

    MQTT_App_ResetResponseState();
    g_mqtt_app_ota_reset_pending = 0U;
    params = ota_params();
    MQTT_App_TestHandleOta("ota-fail", params);
    cJSON_Delete(params);
    ota_error = 77U;
    ota_failed = 1U;
    mqtt_ready = 0U;
    MQTT_App_HandleOtaServiceTask();
    assert(!g_mqtt_app_resp_pending);
    mqtt_ready = 1U;
    MQTT_App_HandleOtaServiceTask();
    assert(g_mqtt_app_resp_pending);
    assert(!g_mqtt_app_ota_reset_pending);
    assert(response_code() == 500);
    assert(strstr(g_mqtt_app_resp_payload, "\"error\":77"));

    MQTT_App_ResetResponseState();
    g_mqtt_app_ota_reset_pending = 0U;
    g_mqtt_app_ota_reset_tick = 0U;
    params = ota_params();
    MQTT_App_TestHandleOta("ota-full", params);
    cJSON_Delete(params);
    ota_install_ready = 1U;
    mqtt_ready = 0U;
    MQTT_App_HandleOtaServiceTask();
    assert(MQTT_App_QueuePublicResponse("{}", "other-1", 200U));
    assert(MQTT_App_QueuePublicResponse("{}", "other-2", 200U));
    assert(MQTT_App_QueuePublicResponse("{}", "other-3", 200U));
    mqtt_ready = 1U;
    MQTT_App_HandleOtaServiceTask();
    assert(MQTT_App_TestOtaRequestState() == 2U);
    assert(!g_mqtt_app_ota_reset_pending);
    g_mqtt_app_resp_pending = 0U;
    MQTT_App_PromoteQueuedResponse();
    fake_tick = 300U;
    MQTT_App_HandleOtaServiceTask();
    assert(MQTT_App_TestOtaRequestState() == 0U);
    assert(g_mqtt_app_ota_reset_pending);
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_ota_reset_tick);
    publish_receipt = ABOX_MQTT_RECEIPT_CONFIRMED;
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_ota_reset_tick);
    assert(!strcmp(g_mqtt_app_resp_request_id, "other-3"));
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    publish_receipt = ABOX_MQTT_RECEIPT_CONFIRMED;
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_ota_reset_tick);
    assert(!strcmp(g_mqtt_app_resp_request_id, "ota-full"));
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_ota_reset_tick);
    publish_receipt = ABOX_MQTT_RECEIPT_FAILED;
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_ota_reset_tick);
    assert(g_mqtt_app_resp_publish_active);
    publish_receipt = ABOX_MQTT_RECEIPT_CONFIRMED;
    MQTT_App_HandlePendingResponseTask();
    assert(g_mqtt_app_ota_reset_tick == 300U);

    MQTT_App_ResetResponseState();
    g_mqtt_app_ota_reset_pending = 0U;
    g_mqtt_app_ota_reset_tick = 0U;
    params = ota_params();
    MQTT_App_TestHandleOta("ota-failed-full", params);
    cJSON_Delete(params);
    ota_failed = 1U;
    ota_error = 77U;
    mqtt_ready = 0U;
    MQTT_App_HandleOtaServiceTask();
    assert(MQTT_App_QueuePublicResponse("{}", "failure-other-1", 200U));
    assert(MQTT_App_QueuePublicResponse("{}", "failure-other-2", 200U));
    assert(MQTT_App_QueuePublicResponse("{}", "failure-other-3", 200U));
    mqtt_ready = 1U;
    MQTT_App_HandleOtaServiceTask();
    assert(MQTT_App_TestOtaRequestState() == 2U);
    assert(!g_mqtt_app_ota_reset_pending);
    g_mqtt_app_resp_pending = 0U;
    MQTT_App_PromoteQueuedResponse();
    MQTT_App_HandleOtaServiceTask();
    assert(MQTT_App_TestOtaRequestState() == 0U);
    assert(!g_mqtt_app_ota_reset_pending);
    g_mqtt_app_resp_pending = 0U;
    MQTT_App_PromoteQueuedResponse();
    g_mqtt_app_resp_pending = 0U;
    MQTT_App_PromoteQueuedResponse();
    assert(!strcmp(g_mqtt_app_resp_request_id, "ota-failed-full"));
    assert(response_code() == 500);
    assert(strstr(g_mqtt_app_resp_payload, "\"error\":77"));

    /* An overlapping reboot must not interrupt or relabel an OTA transaction. */
    MQTT_App_ResetResponseState();
    g_mqtt_app_ota_reset_pending = 0U;
    g_mqtt_app_ota_reset_tick = 0U;
    fake_tick = 100U;
    mqtt_ready = 0U;
    params = ota_params();
    MQTT_App_TestHandleOta("ota-A", params);
    cJSON_Delete(params);
    ota_install_ready = 1U;
    MQTT_App_HandleOtaServiceTask();
    assert(MQTT_App_TestOtaRequestState() == 2U);
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "action", "reboot");
    MQTT_App_TestHandleConfig("reboot-B", params);
    cJSON_Delete(params);
    assert(response_code() == 4003);
    assert(!g_mqtt_app_ota_reset_pending);
    mqtt_ready = 1U;
    MQTT_App_HandleOtaServiceTask();
    assert(g_mqtt_app_ota_reset_pending);
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    publish_receipt = ABOX_MQTT_RECEIPT_CONFIRMED;
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_ota_reset_tick);
    assert(!strcmp(g_mqtt_app_resp_request_id, "ota-A"));
    assert(strstr(g_mqtt_app_resp_payload, "\"requestId\":\"ota-A\""));

    /* The OTA response survives the ordinary 8 s expiry and retries failures. */
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    fake_tick = 8100U;
    MQTT_App_HandlePendingResponseTask();
    assert(g_mqtt_app_resp_pending && g_mqtt_app_resp_publish_active);
    publish_receipt = ABOX_MQTT_RECEIPT_FAILED;
    MQTT_App_HandlePendingResponseTask();
    assert(g_mqtt_app_resp_pending && g_mqtt_app_resp_publish_active);
    publish_receipt = ABOX_MQTT_RECEIPT_CONFIRMED;
    fake_tick = 9000U;
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_resp_pending && g_mqtt_app_ota_reset_tick == 9000U);

    /* Other responses still expire, but an already-confirmed response wins at the boundary. */
    MQTT_App_ResetResponseState();
    g_mqtt_app_ota_reset_pending = 0U;
    fake_tick = 10000U;
    assert(MQTT_App_QueuePublicResponse("{}", "ordinary", 200U));
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    fake_tick = 18000U;
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_resp_pending);

    MQTT_App_ResetResponseState();
    fake_tick = 20000U;
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "action", "reboot");
    MQTT_App_TestHandleConfig("reboot-C", params);
    cJSON_Delete(params);
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    publish_receipt = ABOX_MQTT_RECEIPT_CONFIRMED;
    fake_tick = 28000U;
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_resp_pending && g_mqtt_app_ota_reset_tick == 28000U);

    /* Failed OTA results retry for 45 s, then release the next response. */
    MQTT_App_ResetResponseState();
    g_mqtt_app_ota_reset_pending = 0U;
    g_mqtt_app_ota_reset_tick = 0U;
    fake_tick = 100000U;
    mqtt_ready = 0U;
    params = ota_params();
    MQTT_App_TestHandleOta("ota-failure-retry", params);
    cJSON_Delete(params);
    ota_failed = 1U;
    ota_error = 77U;
    MQTT_App_HandleOtaServiceTask();
    mqtt_ready = 1U;
    MQTT_App_HandleOtaServiceTask();
    assert(response_code() == 500 && strstr(g_mqtt_app_resp_payload, "\"error\":77"));
    assert(MQTT_App_QueuePublicResponse("{}", "after-failure", 200U));
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    fake_tick = 108000U;
    MQTT_App_HandlePendingResponseTask();
    assert(g_mqtt_app_resp_pending && g_mqtt_app_resp_publish_active);
    fake_tick = 115000U;
    publish_receipt = ABOX_MQTT_RECEIPT_FAILED;
    uint64_t previous_operation = publish_operation;
    MQTT_App_HandlePendingResponseTask();
    assert(publish_operation == previous_operation + 1U);
    assert(g_mqtt_app_resp_pending && g_mqtt_app_resp_publish_active);
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    fake_tick = 144999U;
    MQTT_App_HandlePendingResponseTask();
    assert(!strcmp(g_mqtt_app_resp_request_id, "ota-failure-retry"));
    unsigned logs_before_timeout = stale_logs;
    fake_tick = 145000U;
    MQTT_App_HandlePendingResponseTask();
    assert(!strcmp(g_mqtt_app_resp_request_id, "after-failure"));
    assert(stale_logs == logs_before_timeout + 1U);
    assert(!g_mqtt_app_ota_reset_pending && !g_mqtt_app_ota_reset_tick);
    fake_tick = 153000U;
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_resp_pending);

    /* A queued final failure retains its policy when promoted; confirmation wins at the deadline. */
    MQTT_App_ResetResponseState();
    fake_tick = 200000U;
    params = ota_params();
    MQTT_App_TestHandleOta("ota-failure-queued", params);
    cJSON_Delete(params);
    ota_failed = 1U;
    ota_error = 88U;
    mqtt_ready = 0U;
    MQTT_App_HandleOtaServiceTask();
    assert(MQTT_App_QueuePublicResponse("{}", "before-failure", 200U));
    mqtt_ready = 1U;
    MQTT_App_HandleOtaServiceTask();
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    publish_receipt = ABOX_MQTT_RECEIPT_CONFIRMED;
    MQTT_App_HandlePendingResponseTask();
    assert(!strcmp(g_mqtt_app_resp_request_id, "ota-failure-queued"));
    assert(strstr(g_mqtt_app_resp_payload, "\"error\":88"));
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    fake_tick = 208000U;
    MQTT_App_HandlePendingResponseTask();
    assert(g_mqtt_app_resp_pending);
    publish_receipt = ABOX_MQTT_RECEIPT_CONFIRMED;
    fake_tick = 245000U;
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_resp_pending && !g_mqtt_app_ota_reset_pending);

    /* An immediate OTA rejection remains an ordinary 8 s response. */
    MQTT_App_ResetResponseState();
    fake_tick = 300000U;
    ota_ready = 0U;
    params = ota_params();
    MQTT_App_TestHandleOta("ota-not-ready", params);
    cJSON_Delete(params);
    ota_ready = 1U;
    assert(response_code() == 500);
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    fake_tick = 308000U;
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_resp_pending);

    /* Disconnection and tick rollover do not discard a failure before its deadline. */
    MQTT_App_ResetResponseState();
    fake_tick = UINT32_MAX - 1000U;
    params = ota_params();
    MQTT_App_TestHandleOta("ota-reconnect", params);
    cJSON_Delete(params);
    ota_failed = 1U;
    ota_error = 99U;
    mqtt_ready = 0U;
    MQTT_App_HandleOtaServiceTask();
    mqtt_ready = 1U;
    MQTT_App_HandleOtaServiceTask();
    publish_receipt = ABOX_MQTT_RECEIPT_PENDING;
    MQTT_App_HandlePendingResponseTask();
    previous_operation = publish_operation;
    mqtt_ready = 0U;
    publish_receipt = ABOX_MQTT_RECEIPT_FAILED;
    fake_tick += 44000U;
    MQTT_App_HandlePendingResponseTask();
    assert(g_mqtt_app_resp_pending && !g_mqtt_app_resp_publish_active);
    assert(publish_operation == previous_operation);
    mqtt_ready = 1U;
    MQTT_App_HandlePendingResponseTask();
    assert(publish_operation == previous_operation + 1U);
    publish_receipt = ABOX_MQTT_RECEIPT_CONFIRMED;
    MQTT_App_HandlePendingResponseTask();
    assert(!g_mqtt_app_resp_pending && !g_mqtt_app_ota_reset_pending);
    return 0;
}

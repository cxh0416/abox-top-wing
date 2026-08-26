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
#include "ota_service.h"
#include "wing_controller.h"

static uint32_t fake_tick;
static uint8_t mqtt_ready;
static uint8_t ota_ready = 1U;
static uint8_t ota_busy;
static uint8_t ota_start_ok = 1U;
static uint8_t ota_install_ready;
static uint8_t ota_failed;
static uint32_t ota_error;

char g_mqtt_app_device_id[64];
char g_mqtt_app_heartbeat_topic[128];
char g_mqtt_app_ctrl_topic[128];
char g_mqtt_app_ctrl_resp_topic[128];
char g_mqtt_app_status_report_topic[128];
char g_mqtt_app_internal_config_topic[128];
char g_mqtt_app_internal_config_resp_topic[128];
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
char g_mqtt_app_resp_request_id[40];
uint16_t g_mqtt_app_resp_code;
volatile uint8_t g_mqtt_app_ota_reset_pending;
uint32_t g_mqtt_app_ota_reset_tick;
uint32_t g_mqtt_app_ota_request_tick;

void MQTT_App_TestHandleOta(const char* request_id, cJSON* params);
uint8_t MQTT_App_TestOtaRequestState(void);

uint32_t HAL_GetTick(void) { return fake_tick; }
uint8_t MQTT_Core_IsReady(void) { return mqtt_ready; }
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
    return (uint8_t)(snprintf(out, out_sz, "0") > 0);
}
void AppLog_Printf(AppLogLevel_t level, const char* tag, const char* fmt, ...)
{
    (void)level; (void)tag; (void)fmt;
}
void AppLog_Clear(void) {}
uint16_t AppLog_Dump(char* out, uint16_t out_sz)
{
    if (out && out_sz) out[0] = '\0';
    return 0U;
}
const BoardConfigStore_t* BoardConfig_Get(void)
{
    static BoardConfigStore_t config;
    return &config;
}
uint8_t BootState_Load(BootStateRecord_t* state) { (void)state; return 0U; }
int ABoxBootV2_DescriptorRead(ABoxBootV2Descriptor* descriptor) { (void)descriptor; return 0; }
int ABoxBootV2_DescriptorValid(const ABoxBootV2Descriptor* descriptor) { (void)descriptor; return 0; }
const char* ABoxBootV2App_StableSeedSource(void) { return "none"; }
const char* TopFlyingWing_AppFwVersion(void) { return "top-flying-wing-1.0.8"; }
const char* OtaService_Phase(void) { return "IDLE"; }
uint8_t MQTT_App_ScheduleMqttConfigTrial(const char* vid, const char* host, uint16_t port,
                                         const char* username, const char* password)
{
    (void)vid; (void)host; (void)port; (void)username; (void)password;
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
uint16_t WingController_Request(WingAction action, const char* request_id)
{
    (void)action; (void)request_id; return 0U;
}
uint8_t WingController_TakeResult(WingControlResult* result) { (void)result; return 0U; }

static cJSON* ota_params(void)
{
    cJSON* params = cJSON_CreateObject();
    assert(params);
    cJSON_AddStringToObject(params, "url",
        "https://ota.zxwl.ntiov.com:20443/fw-revisions/top_flying_wing/top-flying-wing-1.0.8/top_flying_wing_app.bin");
    cJSON_AddStringToObject(params, "version", "top-flying-wing-1.0.8");
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
    return 0;
}

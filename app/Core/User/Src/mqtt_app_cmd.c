#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abox_boot_v2.h"
#include "abox_boot_v2_app.h"
#include "app_log.h"
#include "app_version.h"
#include "board_config.h"
#include "boot_state.h"
#include "cJSON.h"
#include "main.h"
#include "mqtt_app_int.h"
#include "mqtt_response_internal.h"
#include "mqtt_core.h"
#include "ota_service.h"
#include "time_base.h"
#include "wing_controller.h"
#include "mqtt_v4.h"
#include "mqtt_v4_store.h"
#include "mqtt_runtime.h"
#include "abox_cloud_queue.h"
#include "abox_cloud_internal_v2.h"
#include "abox_cloud_response.h"

#define PRODUCT_ID "top_flying_wing"
typedef enum { OTA_REQ_IDLE = 0, OTA_REQ_DOWNLOADING, OTA_REQ_WAIT_MQTT } OtaRequestState;
static OtaRequestState g_ota_request_state;
static uint16_t g_ota_response_code;
static uint32_t g_ota_response_error;
static uint32_t g_ota_response_wait_tick;
static char g_ota_request_id[65];
static ResponseTopic g_reset_response_topic;

uint8_t MQTT_App_IsOtaResetResponse(const char* topic, const char* request_id, uint16_t code)
{
    return g_mqtt_app_ota_reset_pending && g_reset_response_topic == RESP_INTERNAL_OTA &&
        code == 200U && topic && request_id &&
        !strcmp(request_id, g_ota_request_id) &&
        !strcmp(topic, MQTT_Response_Topic(RESP_INTERNAL_OTA));
}

void MQTT_App_ResetResponseConfirmed(const char* topic, const char* request_id, uint16_t code)
{
    if (g_mqtt_app_ota_reset_pending && code == 200U && topic && request_id &&
        !strcmp(request_id, g_ota_request_id) &&
        !strcmp(topic, MQTT_Response_Topic(g_reset_response_topic)))
        g_mqtt_app_ota_reset_tick = HAL_GetTick();
}

static void copy_id(char dst[65], const char* src)
{
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, 64U);
    dst[64] = '\0';
}

static const char* command_topic_name(const char* topic)
{
    if (topic && strcmp(topic, g_mqtt_app_ctrl_topic) == 0) return "ctrl";
    if (topic && strcmp(topic, g_mqtt_app_internal_config_topic) == 0) return "config";
    if (topic && strcmp(topic, g_mqtt_app_internal_ota_topic) == 0) return "ota";
    return "unknown";
}

void MQTT_App_ResetResponseState(void)
{
    MQTT_Response_Reset();
    g_ota_request_state = OTA_REQ_IDLE;
    g_ota_response_code = 0U;
    g_ota_response_error = 0U;
    g_ota_response_wait_tick = 0U;
    g_ota_request_id[0] = '\0';
}

static const char* message_for_code(uint16_t code)
{
    if (code == 200U) return "operation accepted";
    if (code == 202U) return "configuration trial started";
    if (code == 4001U) return "invalid request";
    if (code == 4002U) return "unsupported command";
    if (code == 4003U) return "local touch control active or command busy; release touch key and retry";
    if (code == 4006U) return "requestId conflict";
    if (code == 4004U) return "deviceType mismatch";
    if (code == 5002U) return "controller unreachable during preflight";
    if (code == 5003U) return "controller feedback not confirmed";
    if (code == 5004U) return "controller rejected command";
    if (code == 5005U) return "controller fault";
    return "internal error";
}

static uint8_t build_response_with_policy(ResponseTopic topic, const char* request_id, uint16_t code,
                           WingAction action, uint8_t include_wing, cJSON* result, uint8_t ota_failure)
{
    uint8_t queued = 0U;
    char timestamp[21];
    char output[MQTT_APP_JSON_BUF_SZ];
    cJSON* root = cJSON_CreateObject();
    cJSON* data = cJSON_CreateObject();
    if (!root || !data || !TimeBase_FormatUtcMs(timestamp, sizeof(timestamp))) goto done;
    (void)action;
    (void)include_wing;
    cJSON_AddStringToObject(root, "version", MQTT_APP_INTERNAL_VERSION_STR);
    cJSON_AddRawToObject(root, "timestamp", timestamp);
    cJSON_AddStringToObject(data, "product", PRODUCT_ID);
    cJSON_AddStringToObject(data, "requestId", request_id ? request_id : "");
    cJSON_AddNumberToObject(data, "code", code);
    cJSON_AddStringToObject(data, "msg", message_for_code(code));
    if (result) { cJSON_AddItemToObject(data, "result", result); result = 0; }
    else cJSON_AddNullToObject(data, "result");
    cJSON_AddItemToObject(root, "data", data); data = 0;
    if (!cJSON_PrintPreallocated(root, output, sizeof(output), 0) || strlen(output) > 1024U)
    {
            snprintf(output, sizeof(output),
                     "{\"version\":\"2.0\",\"timestamp\":0,\"data\":{\"product\":\"%s\",\"requestId\":\"%s\",\"code\":500,\"msg\":\"response too large\",\"result\":null}}",
                     PRODUCT_ID, request_id ? request_id : "");
    }
    queued = ota_failure ? MQTT_Response_QueueOtaFailure(output, request_id, code) :
        MQTT_Response_Queue(topic, output, request_id, code);
    if (!queued)
        APP_LOGE("RESP", "queue full rid=%s code=%u", request_id ? request_id : "", (unsigned)code);
    else
        APP_LOGI("RESP", "queued rid=%s code=%u", request_id ? request_id : "", (unsigned)code);
done:
    if (result) cJSON_Delete(result);
    if (data) cJSON_Delete(data);
    if (root) cJSON_Delete(root);
    return queued;
}

static uint8_t build_response(ResponseTopic topic, const char* request_id, uint16_t code,
                           WingAction action, uint8_t include_wing, cJSON* result)
{ return build_response_with_policy(topic, request_id, code, action, include_wing, result, 0U); }

static const char* boot_state_name(uint32_t state)
{
    switch ((BootStateCode_t)state)
    {
        case BOOT_STATE_INSTALL_PENDING: return "INSTALL_PENDING";
        case BOOT_STATE_INSTALLING: return "INSTALLING";
        case BOOT_STATE_TRIAL: return "TRIAL";
        case BOOT_STATE_CONFIRMED: return "CONFIRMED";
        case BOOT_STATE_ROLLBACK: return "ROLLBACK";
        default: return "NORMAL";
    }
}

static cJSON* device_info(void)
{
    cJSON* obj = cJSON_CreateObject();
    const BoardConfigStore_t* cfg = BoardConfig_Get();
    BootStateRecord_t state;
    ABoxBootV2Descriptor descriptor;
    uint8_t have_state = BootState_Load(&state);
    uint8_t descriptor_valid = ABoxBootV2_DescriptorRead(&descriptor) && ABoxBootV2_DescriptorValid(&descriptor);
    if (!obj) return 0;
    cJSON_AddStringToObject(obj, "vid", cfg->vid);
    cJSON_AddStringToObject(obj, "product", PRODUCT_ID);
    cJSON_AddStringToObject(obj, "fwVersion", TopFlyingWing_AppFwVersion());
    cJSON_AddStringToObject(obj, "bootVersion", descriptor_valid ? descriptor.build_version : "");
    cJSON_AddBoolToObject(obj, "bootDescriptorValid", descriptor_valid ? 1 : 0);
    cJSON_AddNumberToObject(obj, "bootApiVersion", descriptor_valid ? descriptor.abi_version : 0U);
    cJSON_AddNumberToObject(obj, "bootFeatures", descriptor_valid ? descriptor.feature_flags : 0U);
    cJSON_AddStringToObject(obj, "otaScheme", "boot_v2_ufs");
    cJSON_AddStringToObject(obj, "otaDownloader", "app");
    cJSON_AddStringToObject(obj, "stableSeedSource", ABoxBootV2App_StableSeedSource());
    cJSON_AddBoolToObject(obj, "otaBootstrapNeedsRevision", 0);
    cJSON_AddStringToObject(obj, "otaState", boot_state_name(have_state ? state.state : BOOT_STATE_NORMAL));
    cJSON_AddStringToObject(obj, "otaPhase", OtaService_Phase());
    cJSON_AddBoolToObject(obj, "otaReady", OtaService_IsReady() ? 1 : 0);
    cJSON_AddBoolToObject(obj, "otaBusy", OtaService_IsBusy() ? 1 : 0);
    cJSON_AddStringToObject(obj, "mqttHost", cfg->mqtt_host);
    cJSON_AddNumberToObject(obj, "mqttPort", cfg->mqtt_port);
    cJSON_AddBoolToObject(obj, "mqttTls", (cfg->flags & BOARD_CONFIG_FLAG_MQTT_TLS) != 0U);
    cJSON_AddBoolToObject(obj, "mqttTlsActive", MqttRuntime_IsTlsActive());
    cJSON_AddNumberToObject(obj, "mqttTlsProfileId", 0U);
    cJSON_AddStringToObject(obj, "mqttAuthMode", "ca");
    cJSON* protocols = cJSON_AddObjectToObject(obj, "maintenanceProtocols");
    if (protocols) { cJSON_AddNumberToObject(protocols, "mqttConfigTransaction", 1U); cJSON_AddNumberToObject(protocols, "vidChangeTransaction", 1U); }
    cJSON_AddBoolToObject(obj, "trustedTime", TimeBase_IsTrusted());
    cJSON_AddNumberToObject(obj, "canBus", TOP_WING_CAN_BUS);
    cJSON_AddNumberToObject(obj, "canBitrate", 500000U);
    cJSON_AddNumberToObject(obj, "feedbackTimeoutMs", WING_ACTIVE_FEEDBACK_TIMEOUT_MS);
    cJSON_AddStringToObject(obj, "canSessionMode", "remote_only_idle_silent");
    return obj;
}

static uint8_t parse_crc(const cJSON* item, uint32_t* value)
{
    char* end;
    unsigned long parsed;
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) != 8U) return 0U;
    parsed = strtoul(item->valuestring, &end, 16);
    if (!end || *end) return 0U;
    *value = (uint32_t)parsed;
    return 1U;
}

static void handle_internal_config(const char* request_id, cJSON* params)
{
    cJSON* action = cJSON_GetObjectItem(params, "action");
    const char* name = cJSON_IsString(action) ? action->valuestring : "";
    if (strcmp(name, "get_info") == 0) { build_response(RESP_INTERNAL_CONFIG, request_id, 200U, WING_ACTION_NONE, 0U, device_info()); return; }
    if (strcmp(name, "get_log") == 0)
    {
        char logs[640];
        AppLog_Dump(logs, sizeof(logs));
        build_response(RESP_INTERNAL_CONFIG, request_id, 200U, WING_ACTION_NONE, 0U, cJSON_CreateString(logs));
        return;
    }
    if (strcmp(name, "clear_log") == 0) { AppLog_Clear(); build_response(RESP_INTERNAL_CONFIG, request_id, 200U, WING_ACTION_NONE, 0U, 0); return; }
    if (strcmp(name, "reboot") == 0)
    {
        if (MQTT_App_IsMqttConfigTrialActive() || g_ota_request_state != OTA_REQ_IDLE || OtaService_IsBusy())
        { build_response(RESP_INTERNAL_CONFIG, request_id, 4003U, WING_ACTION_NONE, 0U, 0); return; }
        /* OTA owns this ID until its final response has been queued. */
        copy_id(g_ota_request_id, request_id);
        g_reset_response_topic = RESP_INTERNAL_CONFIG;
        g_mqtt_app_ota_reset_pending = 1U;
        g_mqtt_app_ota_reset_tick = 0U;
        g_mqtt_app_ota_request_tick = HAL_GetTick();
        build_response(RESP_INTERNAL_CONFIG, request_id, 200U, WING_ACTION_NONE, 0U, 0);
        return;
    }
    if (strcmp(name, "set_mqtt") == 0)
    {
        ABoxCloudMqttTlsCandidate candidate;
        if (!MQTT_V4_StoreCanErase() || WingController_GetSnapshot()->can_session != WING_CAN_SESSION_IDLE_SILENT)
        { build_response(RESP_INTERNAL_CONFIG, request_id, 4003U, WING_ACTION_NONE, 0U, 0); return; }
        if (!ABoxCloudInternalV2_MqttTlsCandidate(params, &candidate) || candidate.tls_profile_id != 0U)
        { build_response(RESP_INTERNAL_CONFIG, request_id, 4001U, WING_ACTION_NONE, 0U, 0); return; }
        uint16_t reply=MQTT_App_MqttTrialReply(request_id,candidate.vid,candidate.host,candidate.port,
                                                candidate.username,candidate.password);
        if (reply) {
            if (reply==200U || reply==202U || reply==500U) (void)MQTT_App_QueueMqttTrialResponse(request_id,reply);
            else build_response(RESP_INTERNAL_CONFIG,request_id,reply,WING_ACTION_NONE,0U,0);
            return;
        }
        uint8_t ok = MQTT_App_ScheduleMqttConfigTrial(request_id,candidate.vid,candidate.host,candidate.port,
                                                      candidate.username,candidate.password,1U);
        if (!ok) build_response(RESP_INTERNAL_CONFIG, request_id, 500U, WING_ACTION_NONE, 0U, 0);
        else (void)MQTT_App_QueueMqttTrialResponse(request_id,202U);
        return;
    }
    build_response(RESP_INTERNAL_CONFIG, request_id, 4002U, WING_ACTION_NONE, 0U, 0);
}

static void handle_ota(const char* request_id, cJSON* params)
{
    OtaServiceRequest_t request;
    cJSON *url, *version, *size, *crc;
    uint32_t parsed_crc;
    memset(&request, 0, sizeof(request));
    url = cJSON_GetObjectItem(params, "url"); version = cJSON_GetObjectItem(params, "version");
    size = cJSON_GetObjectItem(params, "size"); crc = cJSON_GetObjectItem(params, "crc32");
    if (!cJSON_IsString(url) || !url->valuestring || strlen(url->valuestring) >= sizeof(request.url) ||
        strncmp(url->valuestring, "https://", 8U) != 0 || !cJSON_IsString(version) || !version->valuestring ||
        strlen(version->valuestring) >= sizeof(request.version) || !cJSON_IsNumber(size) || size->valuedouble <= 0.0 ||
        size->valuedouble > APP_MAX_SIZE || !parse_crc(crc, &parsed_crc))
    { build_response(RESP_INTERNAL_OTA, request_id, 4001U, WING_ACTION_NONE, 0U, 0); return; }
    if (MQTT_App_IsMqttConfigTrialActive()) { build_response(RESP_INTERNAL_OTA, request_id, 4003U, WING_ACTION_NONE, 0U, 0); return; }
    if (!OtaService_IsReady()) { build_response(RESP_INTERNAL_OTA, request_id, 500U, WING_ACTION_NONE, 0U, 0); return; }
    if (g_ota_request_state != OTA_REQ_IDLE || OtaService_IsBusy()) { build_response(RESP_INTERNAL_OTA, request_id, 4003U, WING_ACTION_NONE, 0U, 0); return; }
    snprintf(request.url, sizeof(request.url), "%s", url->valuestring);
    snprintf(request.version, sizeof(request.version), "%s", version->valuestring);
    request.size = (uint32_t)size->valuedouble;
    request.crc32 = parsed_crc;
    if (!OtaService_Start(&request)) { build_response(RESP_INTERNAL_OTA, request_id, 500U, WING_ACTION_NONE, 0U, 0); return; }
    g_ota_request_state = OTA_REQ_DOWNLOADING;
    copy_id(g_ota_request_id, request_id);
    APP_LOGI("OTA", "start rid=%s ver=%s size=%lu", g_ota_request_id, request.version,
             (unsigned long)request.size);
}

void MQTT_App_HandleOtaServiceTask(void)
{
    uint32_t now = HAL_GetTick();
    uint32_t error;
    if (g_ota_request_state == OTA_REQ_WAIT_MQTT)
    {
        if (MQTT_Core_IsReady())
        {
            cJSON* result = 0;
            if (g_ota_response_code != 200U)
            {
                result = cJSON_CreateObject();
                if (result) cJSON_AddNumberToObject(result, "error", g_ota_response_error);
            }
            APP_LOGI("OTA", "respond rid=%s code=%u", g_ota_request_id,
                     (unsigned)g_ota_response_code);
            if (!build_response_with_policy(RESP_INTERNAL_OTA, g_ota_request_id, g_ota_response_code,
                                WING_ACTION_NONE, 0U, result, g_ota_response_code != 200U)) return;
            if (g_ota_response_code == 200U)
            {
                /* Start the reset watchdog only after MQTT has reconnected and
                 * the final response has actually been queued. */
                g_mqtt_app_ota_request_tick = now;
                g_reset_response_topic = RESP_INTERNAL_OTA;
                g_mqtt_app_ota_reset_pending = 1U;
            }
            g_ota_request_state = OTA_REQ_IDLE;
        }
        else if (g_ota_response_code == 200U &&
                 (now - g_ota_response_wait_tick) >= 120000U)
        {
            APP_LOGW("OTA", "response reconnect timeout rid=%s", g_ota_request_id);
            g_mqtt_app_ota_request_tick = now;
            g_mqtt_app_ota_reset_tick = now;
            g_mqtt_app_ota_reset_pending = 1U;
            g_ota_request_state = OTA_REQ_IDLE;
        }
        return;
    }
    if (g_ota_request_state != OTA_REQ_DOWNLOADING) return;
    if (OtaService_TakeInstallReady())
    {
        APP_LOGI("OTA", "verified rid=%s wait=mqtt", g_ota_request_id);
        g_ota_response_code = 200U;
        g_ota_response_error = 0U;
        g_ota_response_wait_tick = now;
        g_ota_request_state = OTA_REQ_WAIT_MQTT;
    }
    else if (OtaService_TakeFailure(&error))
    {
        APP_LOGE("OTA", "failed rid=%s error=%lu wait=mqtt", g_ota_request_id,
                 (unsigned long)error);
        g_ota_response_code = 500U;
        g_ota_response_error = error;
        g_ota_response_wait_tick = now;
        g_ota_request_state = OTA_REQ_WAIT_MQTT;
    }
}

#ifdef TOP_WING_HOST_TEST
void MQTT_App_TestHandleConfig(const char* request_id, cJSON* params)
{ handle_internal_config(request_id, params); }
void MQTT_App_TestHandleOta(const char* request_id, cJSON* params)
{
    handle_ota(request_id, params);
}

uint8_t MQTT_App_TestOtaRequestState(void)
{
    return (uint8_t)g_ota_request_state;
}
#endif

void MQTT_App_HandleWingResultTask(void)
{
    WingControlResult result;
    while (WingController_TakeResult(&result))
        MQTT_V4_HandleResult(&result);
}

void MQTT_App_HandleCommandMessage(const char* topic, const char* payload)
{
    cJSON *root = 0, *data, *version, *timestamp, *request_id, *command, *params;
    const char* request_text;
    uint8_t public_request;
    if (!topic || !payload || g_mqtt_app_ota_reset_pending) return;
    public_request = strcmp(topic, g_mqtt_app_ctrl_topic) == 0 ? 1U : 0U;
    if (public_request) { MQTT_V4_HandleRequest(payload); return; }
    if (!public_request && strcmp(topic, g_mqtt_app_internal_config_topic) != 0 &&
        strcmp(topic, g_mqtt_app_internal_ota_topic) != 0)
    {
        APP_LOGW("CMD", "reject topic=unknown reason=topic");
        return;
    }
    root = cJSON_Parse(payload);
    if (!root)
    {
        APP_LOGW("CMD", "reject topic=%s reason=json", command_topic_name(topic));
        return;
    }
    data = cJSON_GetObjectItem(root, "data");
    version = cJSON_GetObjectItem(root, "version"); timestamp = cJSON_GetObjectItem(root, "timestamp");
    if (!cJSON_IsObject(data))
    {
        APP_LOGW("CMD", "reject topic=%s reason=data", command_topic_name(topic));
        goto done;
    }
    request_id = cJSON_GetObjectItem(data, "requestId");
    if (!cJSON_IsString(request_id) || !request_id->valuestring || !request_id->valuestring[0] || strlen(request_id->valuestring) >= 40U)
    {
        APP_LOGW("CMD", "reject topic=%s reason=requestId", command_topic_name(topic));
        goto done;
    }
    request_text = request_id->valuestring;
    command = cJSON_GetObjectItem(data, "command"); params = cJSON_GetObjectItem(data, "params");
    if (!cJSON_IsString(version) ||
        strcmp(version->valuestring, public_request ? MQTT_APP_PUBLIC_VERSION_STR : MQTT_APP_INTERNAL_VERSION_STR) != 0 ||
        !cJSON_IsNumber(timestamp) ||
        !cJSON_IsString(command) || !cJSON_IsObject(params))
    {
        APP_LOGW("CMD", "reject rid=%s reason=envelope", request_text);
        build_response(public_request ? RESP_CTRL : RESP_INTERNAL_CONFIG, request_text, 4001U, WING_ACTION_NONE, public_request, 0); goto done;
    }
    if (strcmp(topic, g_mqtt_app_internal_config_topic) == 0 && strcmp(command->valuestring, "internal_config") == 0)
    {
        cJSON* action_node = cJSON_GetObjectItem(params, "action");
        APP_LOGI("CMD", "rx rid=%s action=%s", request_text,
                 cJSON_IsString(action_node) ? action_node->valuestring : "invalid");
        handle_internal_config(request_text, params);
    }
    else if (strcmp(topic, g_mqtt_app_internal_ota_topic) == 0 && strcmp(command->valuestring, "ota_upgrade") == 0)
    {
        APP_LOGI("CMD", "rx rid=%s action=ota_upgrade", request_text);
        handle_ota(request_text, params);
    }
    else
    {
        APP_LOGW("CMD", "reject rid=%s reason=unsupported", request_text);
        build_response(strcmp(topic, g_mqtt_app_internal_ota_topic) == 0 ? RESP_INTERNAL_OTA : RESP_INTERNAL_CONFIG,
                       request_text, 4002U, WING_ACTION_NONE, 0U, 0);
    }
done:
    cJSON_Delete(root);
}

uint8_t MQTT_App_QueuePublicResponse(const char* payload, const char* id, uint16_t code)
{ return MQTT_Response_Queue(RESP_CTRL, payload, id, code); }

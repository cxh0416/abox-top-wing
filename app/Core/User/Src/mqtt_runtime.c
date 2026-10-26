#include "mqtt_runtime.h"
#include <string.h>
#include "app_log.h"
#include "board_config.h"
#include "mqtt_core.h"
#include "ota_service.h"
#include "time_base.h"
#include "main.h"

static ABoxMqttRuntime g_runtime;
static uint8_t initialized;
static uint8_t resume_after_ota;
static uint8_t endpoint_pending;
static char pending_host[64], pending_user[32], pending_password[32];
static uint16_t pending_port;
static uint8_t pending_tls;
static uint8_t ca_ready(void* user) { (void)user; return OtaService_IsProvisioned() && !OtaService_IsBusy(); }
static uint8_t time_valid(void* user) { (void)user; return TimeBase_IsValid(); }
static void observe(void* user, const char* line) { (void)user; TimeBase_UpdateFromCCLK(line); }
static void changed(void* user, ABoxMqttRuntimeState state, const char* stage)
{ (void)user; APP_LOGI("MQTT", "runtime=%u stage=%s", (unsigned)state, stage); }

static ABoxMqttConfig config(void)
{
    const BoardConfigStore_t* saved = BoardConfig_Get();
    ABoxMqttConfig c = {0};
    c.host = saved->mqtt_host; c.port = saved->mqtt_port;
    c.username = saved->mqtt_username; c.password = saved->mqtt_password;
    c.tls_enabled = (saved->flags & BOARD_CONFIG_FLAG_MQTT_TLS) != 0U;
    return c;
}

uint8_t MqttRuntime_ApplyCurrentConfig(void)
{
    ABoxMqttConfig c = config();
    return initialized && ABoxMqttRuntime_Apply(&g_runtime, &c, HAL_GetTick());
}

uint8_t MqttRuntime_ApplyEndpoint(const char* host, uint16_t port, const char* user, const char* password, uint8_t tls)
{
    ABoxMqttConfig c = {0};
    if (!initialized || !host || !user || !password || !port ||
        strlen(host) >= sizeof(pending_host) || strlen(user) >= sizeof(pending_user) ||
        strlen(password) >= sizeof(pending_password)) return 0U;
    /* A rejected TLS capability probe must still allow a trial to restore
     * its unchanged, checksummed original plain configuration. No TLS latch
     * is cleared for a TLS target or a different endpoint/credential set. */
    const BoardConfigStore_t* saved = BoardConfig_Get();
    if (!tls && !(saved->flags & BOARD_CONFIG_FLAG_MQTT_TLS) &&
        !strcmp(host, saved->mqtt_host) && port == saved->mqtt_port &&
        !strcmp(user, saved->mqtt_username) && !strcmp(password, saved->mqtt_password))
        (void)ABoxMqttRuntime_RecoveryComplete(&g_runtime);
    c.host = host; c.port = port; c.username = user; c.password = password; c.tls_enabled = tls;
    if (ABoxMqttRuntime_Restore(&g_runtime, &c, HAL_GetTick())) { endpoint_pending = 0U; return 1U; }
    /* A timeout may arrive during TLS preparation rather than CONNECT.
     * Finish the shared stop/drain path before applying the rollback. */
    strcpy(pending_host, host); strcpy(pending_user, user); strcpy(pending_password, password);
    pending_port = port; pending_tls = tls; endpoint_pending = 1U;
    return ABoxMqttRuntime_StopFirst(&g_runtime, HAL_GetTick()) >= 0;
}

uint8_t MqttRuntime_Init(void)
{
    static const ABoxMqttRuntimeCoreApi api = {
        MQTT_Core_SetPaused, MQTT_Core_SetSecurityReady, MQTT_Core_ApplyRuntimeConfig,
        MQTT_Core_TransportTask, MQTT_Core_Reset, MQTT_Core_Stop,
        MQTT_Core_IsReady, MQTT_Core_IsConnected
    };
    uint32_t now = HAL_GetTick();
    if (!initialized)
    {
        ABoxMqttRuntimePort port = {0};
        ABoxMqttRuntimeOptions options;
        ABoxMqttConfig c = config();
        if (!MQTT_Core_AtCore() || !ABoxMqttRuntimePort_BindCore(&port, &api)) return 0U;
        ABoxMqttRuntimeOptions_StandardEc800(&options, ABOX_EC800_OWNER_PRODUCT_BASE + 3U, 0U);
        options.probe_tls_capability = 1U;
        options.query_clock_mode = 1U;
        options.ca_ciphersuite = 0xC02BU;
        port.ca_ready = ca_ready; port.time_valid = time_valid;
        port.observe_modem_line = observe; port.state_changed = changed;
        initialized = ABoxMqttRuntime_Init(&g_runtime, MQTT_Core_AtCore(), &port, &options, &c, now);
        if (!initialized) APP_LOGE("MQTT", "runtime init failed");
    }
    return initialized;
}

void MqttRuntime_Task(void)
{
    uint32_t now = HAL_GetTick();
    if (!initialized && !MqttRuntime_Init()) return;
    /* Both Platform owners already observe RDY. Re-feeding it here would
     * make the runtime's Core_Reset callback trigger an endless reset loop. */
    (void)MQTT_Core_TakeModemReady();
    /* Stop-first must keep progressing while OTA waits for the transport.
     * Once stopped, Platform is inert until OTA returns its workspace. */
    if (endpoint_pending && !OtaService_IsBusy())
        (void)ABoxMqttRuntime_StopFirst(&g_runtime, now);
    if (endpoint_pending && !OtaService_IsBusy() &&
        ABoxMqttRuntime_GetState(&g_runtime) == ABOX_MQTT_RUNTIME_UNENROLLED)
    {
        ABoxMqttConfig c = {0};
        c.host = pending_host; c.port = pending_port; c.username = pending_user;
        c.password = pending_password; c.tls_enabled = pending_tls;
        if (ABoxMqttRuntime_Apply(&g_runtime, &c, now)) endpoint_pending = 0U;
    }
    if (resume_after_ota && !endpoint_pending && !OtaService_IsBusy() &&
        ABoxMqttRuntime_GetState(&g_runtime) == ABOX_MQTT_RUNTIME_UNENROLLED)
    {
        if (MqttRuntime_ApplyCurrentConfig()) resume_after_ota = 0U;
    }
    ABoxMqttRuntime_Poll(&g_runtime, now);
}

void MQTT_Core_Task(void) { MqttRuntime_Task(); }
int MqttRuntime_StopFirst(uint32_t now)
{
    if (!initialized && !MqttRuntime_Init()) return -1;
    resume_after_ota = 1U;
    return ABoxMqttRuntime_StopFirst(&g_runtime, now);
}
uint8_t MqttRuntime_IsTlsActive(void)
{ return initialized && ABoxMqttRuntime_IsReady(&g_runtime) && ABoxMqttRuntime_IsTlsActive(&g_runtime); }
uint8_t MqttRuntime_NeedsModemRecovery(void)
{ return initialized && ABoxMqttRuntime_NeedsModemRecovery(&g_runtime); }

#ifdef TOP_WING_HOST_TEST
const ABoxMqttRuntime* MqttRuntime_TestState(void) { return &g_runtime; }
#endif

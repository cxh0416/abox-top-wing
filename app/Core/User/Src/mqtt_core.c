#include "mqtt_core.h"

#include "abox_mqtt_ec800_bridge.h"
#include "app_log.h"
#include "main.h"
#include "mqtt_runtime.h"
#include "mqtt_v4_store.h"
#include "wing_enrollment.h"
#include "time_base.h"
#include <stdio.h>
#include <string.h>

/* Product identity and Topic storage stay here. Connection, AT command
 * progression and RX framing are owned by ABoxMqttEc800. */
enum { TOPIC_CAPACITY = 128U, PAYLOAD_CAPACITY = 1025U };
static MQTT_CoreConfig_t g_config;
static ABoxMqttEc800Bridge g_bridge;
static ABoxMqttEc800Config g_transport_config;
static const char *g_subscriptions[4];
static Ec800AtCore_t *g_at;
static MQTT_CoreMsgCallback_t g_message;
static MQTT_CorePublishObserver_t g_publish_observer;
static void *g_publish_observer_user;
static uint8_t g_header[192], g_topic[TOPIC_CAPACITY];
static uint8_t g_payload[PAYLOAD_CAPACITY], g_outgoing[PAYLOAD_CAPACITY];
static uint8_t g_modem_ready;
static uint32_t g_ready_seq, g_last_rx_tick, g_last_pub_tick;

static void message(void *user, const char *topic, const char *payload)
{
    (void)user;
    if (!g_message || strlen(payload) > 1024U) return;
    g_last_rx_tick = HAL_GetTick();
    g_message(topic, payload);
}

static void publish_event(void *user, uint64_t operation,
                          ABoxMqttEc800PublishEvent event)
{
    (void)user;
    if (g_publish_observer) g_publish_observer(operation, event, g_publish_observer_user);
    if (event == ABOX_MQTT_EC800_PUBLISH_CONFIRMED) {
        g_last_pub_tick = HAL_GetTick();
    }
}
static void state_changed(void *user, ABoxMqttEc800State state)
{
    (void)user;
    if (state == ABOX_MQTT_EC800_READY) {
        ++g_ready_seq;
        g_last_rx_tick = g_last_pub_tick = HAL_GetTick();
    }
}
static void modem_reset(void *user)
{
    (void)user;
    MQTT_Core_Reset();
}
static void build_config(void)
{
    uint8_t i;
    memset(&g_transport_config, 0, sizeof(g_transport_config));
    g_transport_config.apn = "CMNET";
    g_transport_config.host = g_config.server_host;
    g_transport_config.port = g_config.server_port;
    g_transport_config.client_id = g_config.client_id;
    g_transport_config.username = g_config.username;
    g_transport_config.password = g_config.password;
    g_transport_config.client_index = g_config.client_idx;
    g_transport_config.subscription_count = g_config.sub_topic_count;
    for (i = 0U; i < 4U; ++i)
        g_subscriptions[i] = g_config.sub_topics[i];
    g_transport_config.subscriptions = g_subscriptions;
    g_transport_config.command_timeout_ms = 5000U;
    g_transport_config.open_timeout_ms = 125000U;
    g_transport_config.connect_timeout_ms = 10000U;
    g_transport_config.subscribe_timeout_ms = 5000U;
    g_transport_config.publish_timeout_ms = 15000U;
    g_transport_config.retry_delay_ms = 4000U;
    g_transport_config.legacy_json = 1U;
    g_transport_config.mqtt_version = g_config.tls_enabled ? 4U : 0U;
    g_transport_config.pdp_context_id = g_config.tls_enabled ? 1U : 0U;
    g_transport_config.clean_session = 1U;
}

void MQTT_Core_Init(const MQTT_CoreConfig_t *config, MQTT_CoreMsgCallback_t callback)
{
    memset(&g_config, 0, sizeof(g_config));
    if (config) g_config = *config;
    g_message = callback;
    g_publish_observer = NULL;
    g_publish_observer_user = NULL;
    memset(&g_bridge, 0, sizeof(g_bridge));
    g_modem_ready = 0U;
    g_ready_seq = 0U;
    g_last_rx_tick = g_last_pub_tick = HAL_GetTick();
    build_config();
    if (g_at) MQTT_Core_SetAtCore(g_at);
}

void MQTT_Core_SetPublishObserver(MQTT_CorePublishObserver_t observer, void *user)
{
    g_publish_observer = observer;
    g_publish_observer_user = user;
}

void MQTT_Core_SetAtCore(Ec800AtCore_t *at)
{
    ABoxMqttEc800Buffers buffers;
    static const ABoxMqttEc800BridgeHooks hooks = {
        .message = message,
        .publish_event = publish_event,
        .state_changed = state_changed,
        .modem_reset = modem_reset
    };
    if (!at) return;
    g_at = at;
    if (g_bridge.initialized || !g_message) return;
    memset(&buffers, 0, sizeof(buffers));
    buffers.header = g_header; buffers.header_capacity = sizeof(g_header);
    buffers.topic = g_topic; buffers.topic_capacity = sizeof(g_topic);
    buffers.payload = g_payload; buffers.payload_capacity = sizeof(g_payload);
    buffers.publish_payload = g_outgoing;
    buffers.publish_capacity = sizeof(g_outgoing);
    if (!ABoxMqttEc800Bridge_Init(&g_bridge, at, &g_transport_config,
                                   &buffers, &hooks,
                                   ABOX_EC800_OWNER_PRODUCT_BASE + 5U,
                                   HAL_GetTick())) {
        APP_LOGE("MQTT", "public transport init failed");
        return;
    }
    if (!ABoxMqttEc800_UseV4Backoff(&g_bridge.transport,
        HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2() ^ HAL_GetTick()))
    {
        APP_LOGE("MQTT", "V4 backoff initialization failed");
        g_bridge.initialized = 0U;
    }
}

void MQTT_Core_Reset(void)
{
    if (!g_bridge.initialized) return;
    /* The transport's RDY handler resets its own state once. This callback
     * only invalidates the product-facing receipt and identity caches. */
    g_modem_ready = 1U;
    MQTT_V4_StoreOnModemReset();
    WingEnrollment_AfterModemReset();
    ABoxMqttReceipt_Invalidate(&g_bridge.receipt);
    ABoxMqttEc800Bridge_SetSecurityReady(&g_bridge, 0U);
    ABoxEc800Iccid_OnModemReset(&g_bridge.iccid);
}
uint8_t MQTT_Core_TakeModemReady(void)
{
    uint8_t ready = g_modem_ready;
    g_modem_ready = 0U;
    return ready;
}

void MQTT_Core_TransportTask(void)
{
    /* Finish an already-open file before starting long reconnect commands. */
    if (MQTT_V4_StoreBusy()) return;
    ABoxMqttEc800Bridge_Poll(&g_bridge, HAL_GetTick());
}

void MQTT_Core_RequestReconnect(void)
{
    if (g_bridge.initialized) ABoxMqttEc800_RequestReconnect(&g_bridge.transport, HAL_GetTick());
}
void MQTT_Core_ForceReconnect(void) { MQTT_Core_RequestReconnect(); }

void MQTT_Core_SetPaused(uint8_t paused)
{
    ABoxMqttEc800Bridge_SetPaused(&g_bridge, paused, HAL_GetTick());
}
void MQTT_Core_SetSecurityReady(uint8_t ready)
{
    ABoxMqttEc800Bridge_SetSecurityReady(&g_bridge, ready);
}
uint8_t MQTT_Core_ClientIndex(void) { return g_config.client_idx; }
uint8_t MQTT_Core_IsPaused(void) { return g_bridge.paused; }

int MQTT_Core_Stop(uint32_t now_ms)
{
    if (!g_bridge.initialized) return -1;
    if (MQTT_V4_StoreBusy()) return 0;
    return ABoxMqttEc800_Stop(&g_bridge.transport, now_ms);
}
int MQTT_Core_BorrowWorkspace(void)
{
    return ABoxMqttEc800Bridge_BorrowWorkspace(&g_bridge, 1024U);
}
void MQTT_Core_ReturnWorkspace(void)
{
    ABoxMqttEc800Bridge_ReturnWorkspace(&g_bridge);
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    size_t n;
    if (!source) return;
    n = strlen(source);
    if (n >= capacity) n = capacity - 1U;
    memcpy(destination, source, n);
    destination[n] = 0;
}
void MQTT_Core_UpdateBroker(const char *host, uint16_t port,
                            const char *username, const char *password)
{
    copy_text(g_config.server_host, sizeof(g_config.server_host), host);
    if (port) g_config.server_port = port;
    copy_text(g_config.username, sizeof(g_config.username), username);
    copy_text(g_config.password, sizeof(g_config.password), password);
    build_config();
}
void MQTT_Core_ClearBroker(void)
{
    g_config.server_host[0] = 0;
    g_config.server_port = 0U;
    g_config.username[0] = 0;
    memset(g_config.password, 0, sizeof(g_config.password));
}
uint8_t MQTT_Core_ApplyRuntimeConfig(const ABoxMqttConfig *config)
{
    ABoxMqttEc800State state;
    if (!config || !config->host || !config->username ||
        !config->password || !config->port || !g_bridge.initialized ||
        strlen(config->host) >= sizeof(g_config.server_host) ||
        strlen(config->username) >= sizeof(g_config.username) ||
        strlen(config->password) >= sizeof(g_config.password)) return 0U;
    state = ABoxMqttEc800_GetState(&g_bridge.transport);
    if (state != ABOX_MQTT_EC800_IDLE &&
        state != ABOX_MQTT_EC800_PAUSED) return 0U;
    MQTT_Core_UpdateBroker(config->host, config->port,
                           config->username, config->password);
    g_config.tls_enabled = config->tls_enabled;
    g_config.client_idx = config->tls_enabled ? 1U : 0U;
    g_config.ssl_ctx_id = 2U;
    build_config();
    return (uint8_t)ABoxMqttEc800Bridge_Configure(&g_bridge,
                                                    &g_transport_config);
}
void MQTT_Core_SetTopics(const char *const *topics, uint8_t count,
                         const char *pub_topic)
{
    uint8_t i;
    if (count > 4U) return;
    memset(g_config.sub_topics, 0, sizeof(g_config.sub_topics));
    for (i = 0U; i < count; ++i)
        copy_text(g_config.sub_topics[i], sizeof(g_config.sub_topics[i]),
                  topics[i]);
    g_config.sub_topic_count = count;
    copy_text(g_config.pub_topic, sizeof(g_config.pub_topic), pub_topic);
    build_config();
}
uint8_t MQTT_Core_IsReady(void)
{
    return g_bridge.initialized && ABoxMqttEc800_IsReady(&g_bridge.transport);
}
uint8_t MQTT_Core_IsConnected(void)
{
    return g_bridge.initialized && ABoxMqttEc800_IsConnected(&g_bridge.transport);
}
uint8_t MQTT_Core_CanRunBackgroundAt(void)
{
    return (uint8_t)ABoxMqttEc800Bridge_CanRunBackgroundAt(&g_bridge);
}
MQTT_CoreState_t MQTT_Core_GetState(void)
{
    switch (ABoxMqttEc800_GetState(&g_bridge.transport)) {
    case ABOX_MQTT_EC800_READY: return MQTT_CORE_ST_READY;
    case ABOX_MQTT_EC800_SUBSCRIBING: return MQTT_CORE_ST_WAIT_SUB;
    case ABOX_MQTT_EC800_CONNECTING: return MQTT_CORE_ST_WAIT_CONN;
    case ABOX_MQTT_EC800_OPENING: return MQTT_CORE_ST_WAIT_OPEN;
    case ABOX_MQTT_EC800_CONFIGURING: return MQTT_CORE_ST_CFG;
    case ABOX_MQTT_EC800_CLOSING:
    case ABOX_MQTT_EC800_RETRY_WAIT: return MQTT_CORE_ST_RECONNECT_WAIT;
    case ABOX_MQTT_EC800_BLOCKED: return MQTT_CORE_ST_ERROR;
    default: return MQTT_CORE_ST_IDLE;
    }
}
uint8_t MQTT_Core_IsPublishActive(void)
{
    return g_bridge.initialized && g_bridge.transport.publish_active;
}
uint32_t MQTT_Core_GetPubOkSeq(void) { return g_bridge.publish_ok; }
uint32_t MQTT_Core_GetReadySeq(void) { return g_ready_seq; }
uint32_t MQTT_Core_GetLastRxAgeMs(void) { return HAL_GetTick() - g_last_rx_tick; }
uint32_t MQTT_Core_GetLastPubOkAgeMs(void) { return HAL_GetTick() - g_last_pub_tick; }
uint8_t MQTT_Core_Publish(const char *topic, const char *payload, uint8_t qos)
{
    return MQTT_Core_PublishEx(topic, payload, qos, 0U);
}
uint8_t MQTT_Core_PublishEx(const char *topic, const char *payload,
                           uint8_t qos, uint8_t retain)
{
    uint64_t operation;
    return MQTT_Core_PublishExTracked(topic, payload, qos, retain, &operation);
}
uint8_t MQTT_Core_PublishExTracked(const char *topic, const char *payload,
                                  uint8_t qos, uint8_t retain, uint64_t *operation)
{
    /* A QMT publish can wait for 15 seconds. Keep it out of an open QF
     * transaction; response/report owners retain their packets and retry. */
    if (MQTT_V4_StoreBusy()) return 0U;
    return (uint8_t)ABoxMqttEc800Bridge_Publish(&g_bridge, topic, payload,
                                                 qos, retain, operation);
}
ABoxMqttReceiptStatus MQTT_Core_PublishResult(uint64_t operation)
{
    return ABoxMqttReceipt_Get(&g_bridge.receipt, operation);
}
void MQTT_Core_CancelPendingPublish(void)
{
    ABoxMqttEc800Bridge_CancelPublish(&g_bridge);
}
void MQTT_Core_RequestIccid(void)
{
    if (g_bridge.initialized && !g_bridge.transport.enabled && !ABoxEc800At_IsBusy(g_at))
    {
        (void)ABoxEc800Iccid_Request(&g_bridge.iccid, HAL_GetTick());
        return;
    }
    (void)ABoxMqttEc800Bridge_RequestIccid(&g_bridge, HAL_GetTick());
}
uint8_t MQTT_Core_IsIccidValid(void) { return g_bridge.iccid.valid; }
const char *MQTT_Core_GetIccid(void) { return ABoxEc800Iccid_Value(&g_bridge.iccid); }
uint8_t MQTT_Core_IsIccidPending(void) { return g_bridge.iccid.pending; }

ABoxMqttEc800Bridge *MQTT_Core_Bridge(void) { return &g_bridge; }

uint8_t MQTT_Core_IsRxCollecting(void)
{ return g_bridge.initialized && g_bridge.transport.rx.state != 0; }

void MQTT_Core_SetDeviceId(const char *id)
{ copy_text(g_config.client_id, sizeof(g_config.client_id), id); build_config(); }

Ec800AtCore_t* MQTT_Core_AtCore(void) { return g_at; }

#include "mqtt_core.h"

#include <string.h>

#include "app_log.h"
#include "lockctrl_diag.h"
#include "main.h"
#include "mqtt_core_int.h"

#define MQTT_CORE_READY_LINE_SILENCE_MS 70000U
#define MQTT_CORE_PUB_OK_TIMEOUT_MS 90000U
#define MQTT_CORE_PUB_PROMPT_TIMEOUT_MS 3000U
#define MQTT_CORE_DIAG_SNAPSHOT_PERIOD_MS 60000U
#define MQTT_CORE_DIAG_CMD_TIMEOUT_MS 3000U

MQTT_CoreConfig_t g_mqtt_cfg;
MQTT_CoreRuntime_t g_mqtt_rt;
Ec800AtCore_t* g_mqtt_at_core = 0;
MQTT_CoreMsgCallback_t g_mqtt_msg_cb = 0;
char g_mqtt_line_buf[MQTT_CORE_RX_LINE_BUF_SZ];
uint16_t g_mqtt_line_len = 0;
volatile uint8_t g_mqtt_last_evt_pending = 0;
char g_mqtt_last_evt_line[128];
char g_mqtt_sub_topics[MQTT_CORE_MAX_SUB_TOPICS][MQTT_CORE_TOPIC_BUF_SZ];
uint8_t g_mqtt_sub_topic_count = 0U;
char g_mqtt_pub_topic[MQTT_CORE_TOPIC_BUF_SZ];
char g_mqtt_pub_pending_topic[MQTT_CORE_TOPIC_BUF_SZ];
char g_mqtt_pub_pending_payload[MQTT_CORE_PAYLOAD_BUF_SZ];
uint16_t g_mqtt_pub_pending_msg_id = 0U;
static uint16_t g_mqtt_next_pub_msg_id = 0U;
uint8_t g_mqtt_rx_drop_until_lf = 0;
char g_mqtt_iccid[32];
uint8_t g_mqtt_iccid_valid = 0;
uint8_t g_mqtt_iccid_query_pending = 0;
uint32_t g_mqtt_iccid_query_tick = 0;
volatile uint8_t g_mqtt_pub_prompt_pending = 0;
volatile uint8_t g_mqtt_rx_msg_pending = 0;
// volatile uint8_t g_mqtt_need_full_reinit = 0;
char g_mqtt_rx_pending_topic[MQTT_CORE_TOPIC_BUF_SZ];
char g_mqtt_rx_pending_payload[MQTT_CORE_PAYLOAD_BUF_SZ];
uint32_t g_mqtt_last_pub_ok_tick = 0;
uint32_t g_mqtt_last_mqtt_rx_tick = 0;
uint32_t g_mqtt_pub_ok_seq = 0;
uint8_t g_mqtt_pub_timeout_count = 0U;
static uint8_t g_mqtt_paused = 0U;

#if LOCKCTRL_EC800_AT_DIAG_UART5
static const char* const g_mqtt_diag_names[] = {
    "SIM",
    "ICCID",
    "SIGNAL",
    "REG",
    "ATTACH",
    "PDP",
    "IP",
    "NETWORK"
};

static const char* const g_mqtt_diag_cmds[] = {
    "AT+CPIN?",
    "AT+QCCID",
    "AT+CSQ",
    "AT+CEREG?",
    "AT+CGATT?",
    "AT+QIACT?",
    "AT+CGPADDR=1",
    "AT+QNWINFO"
};

#define MQTT_CORE_DIAG_COUNT ((uint8_t)(sizeof(g_mqtt_diag_cmds) / sizeof(g_mqtt_diag_cmds[0])))

volatile uint8_t g_mqtt_diag_command_active = 0U;
static uint8_t g_mqtt_diag_snapshot_active = 0U;
static uint8_t g_mqtt_diag_snapshot_index = 0U;
static uint32_t g_mqtt_diag_snapshot_next_tick = 0U;

void mqtt_diag_reset_snapshot_state(void)
{
    g_mqtt_diag_snapshot_active = 0U;
    g_mqtt_diag_snapshot_index = 0U;
    g_mqtt_diag_snapshot_next_tick = 0U;
}
#endif

static uint8_t mqtt_pub_flow_active(void)
{ return (g_mqtt_rt.pub_busy || g_mqtt_rt.pub_wait_prompt || g_mqtt_rt.pub_wait_result) ? 1U : 0U; }

static void mqtt_clear_sub_topics(void)
{
    uint8_t i;

    for (i = 0U; i < MQTT_CORE_MAX_SUB_TOPICS; i++) { g_mqtt_sub_topics[i][0] = '\0'; }
    g_mqtt_sub_topic_count = 0U;
}

static void mqtt_copy_config_sub_topics(void)
{
    uint8_t i;
    uint8_t count = g_mqtt_cfg.sub_topic_count;

    mqtt_clear_sub_topics();
    if (count > MQTT_CORE_MAX_SUB_TOPICS) count = MQTT_CORE_MAX_SUB_TOPICS;

    for (i = 0U; i < count; i++)
    {
        if (g_mqtt_cfg.sub_topics[i][0] == '\0') continue;
        mqtt_copy_topic(g_mqtt_sub_topics[g_mqtt_sub_topic_count], sizeof(g_mqtt_sub_topics[g_mqtt_sub_topic_count]),
                        g_mqtt_cfg.sub_topics[i]);
        g_mqtt_sub_topic_count++;
    }
}

static void mqtt_set_ready_after_subscribe(void)
{
    g_mqtt_rt.subscribed = 1U;
    g_mqtt_rt.need_reconnect = 0U;
    g_mqtt_rt.reconnect_count = 0U;
    g_mqtt_rt.retry_in_state = 0U;
    g_mqtt_rt.sub_index = 0U;
    g_mqtt_rt.last_ready_tick = HAL_GetTick();
    g_mqtt_last_pub_ok_tick = g_mqtt_rt.last_ready_tick;
    g_mqtt_last_mqtt_rx_tick = g_mqtt_rt.last_ready_tick;
#if LOCKCTRL_EC800_AT_DIAG_UART5
    g_mqtt_diag_snapshot_active = 0U;
    g_mqtt_diag_snapshot_index = 0U;
    g_mqtt_diag_snapshot_next_tick = g_mqtt_rt.last_ready_tick + MQTT_CORE_DIAG_SNAPSHOT_PERIOD_MS;
#endif
    mqtt_set_state(MQTT_CORE_ST_READY);
}

static void mqtt_clear_publish_state(void)
{
    g_mqtt_rt.pub_busy = 0U;
    g_mqtt_rt.pub_wait_prompt = 0U;
    g_mqtt_rt.pub_wait_result = 0U;
    g_mqtt_pub_prompt_pending = 0U;
    g_mqtt_pub_pending_msg_id = 0U;
    g_mqtt_pub_pending_topic[0] = '\0';
    g_mqtt_pub_pending_payload[0] = '\0';
}

static uint16_t mqtt_allocate_publish_msg_id(uint8_t qos)
{
    if (qos == 0U) return 0U;

    g_mqtt_next_pub_msg_id++;
    if (g_mqtt_next_pub_msg_id == 0U) g_mqtt_next_pub_msg_id = 1U;
    return g_mqtt_next_pub_msg_id;
}

static void mqtt_update_query_timeouts(uint32_t now)
{
    if (g_mqtt_iccid_query_pending && ((now - g_mqtt_iccid_query_tick) > 5000U)) { g_mqtt_iccid_query_pending = 0U; }
}

void MQTT_Core_Init(const MQTT_CoreConfig_t* cfg, MQTT_CoreMsgCallback_t cb)
{
    memset(&g_mqtt_cfg, 0, sizeof(g_mqtt_cfg));
    memset(&g_mqtt_rt, 0, sizeof(g_mqtt_rt));
    memset(g_mqtt_line_buf, 0, sizeof(g_mqtt_line_buf));

    g_mqtt_line_len = 0;
    g_mqtt_rx_drop_until_lf = 0;
    g_mqtt_msg_cb = cb;
    g_mqtt_pub_pending_topic[0] = '\0';
    g_mqtt_pub_pending_payload[0] = '\0';
    g_mqtt_pub_pending_msg_id = 0U;
    g_mqtt_next_pub_msg_id = 0U;
    g_mqtt_iccid[0] = '\0';
    g_mqtt_iccid_valid = 0U;
    g_mqtt_iccid_query_pending = 0U;
    g_mqtt_iccid_query_tick = 0U;
    g_mqtt_pub_prompt_pending = 0U;
    g_mqtt_rx_msg_pending = 0U;
    // g_mqtt_need_full_reinit = 0U;
    g_mqtt_rx_pending_topic[0] = '\0';
    g_mqtt_rx_pending_payload[0] = '\0';
    mqtt_rx_parser_reset();
    g_mqtt_last_pub_ok_tick = HAL_GetTick();
    g_mqtt_last_mqtt_rx_tick = HAL_GetTick();
    g_mqtt_pub_ok_seq = 0U;
    g_mqtt_pub_timeout_count = 0U;
    g_mqtt_paused = 0U;
#if LOCKCTRL_EC800_AT_DIAG_UART5
    g_mqtt_diag_command_active = 0U;
    g_mqtt_diag_snapshot_active = 0U;
    g_mqtt_diag_snapshot_index = 0U;
    g_mqtt_diag_snapshot_next_tick = 0U;
#endif

    if (cfg) { g_mqtt_cfg = *cfg; }

    mqtt_copy_config_sub_topics();
    mqtt_copy_topic(g_mqtt_pub_topic, sizeof(g_mqtt_pub_topic), g_mqtt_cfg.pub_topic);

    g_mqtt_rt.last_rx_tick = HAL_GetTick();
    mqtt_clear_flags();
    mqtt_set_state(MQTT_CORE_ST_IDLE);
}

void MQTT_Core_SetAtCore(Ec800AtCore_t* at_core)
{
    g_mqtt_at_core = at_core;
    if (g_mqtt_at_core) { EC800_AT_RegisterUrcHandler(g_mqtt_at_core, EC800_AT_OWNER_MQTT, MQTT_Core_OnAtEvent, 0); }
}

void MQTT_Core_Reset(void)
{
    memset(&g_mqtt_rt, 0, sizeof(g_mqtt_rt));
    memset(g_mqtt_line_buf, 0, sizeof(g_mqtt_line_buf));

    g_mqtt_line_len = 0;
    g_mqtt_rx_drop_until_lf = 0;
    g_mqtt_pub_pending_topic[0] = '\0';
    g_mqtt_pub_pending_payload[0] = '\0';
    g_mqtt_pub_pending_msg_id = 0U;
    g_mqtt_next_pub_msg_id = 0U;
    g_mqtt_iccid[0] = '\0';
    g_mqtt_iccid_valid = 0U;
    g_mqtt_iccid_query_pending = 0U;
    g_mqtt_iccid_query_tick = 0U;
    g_mqtt_pub_prompt_pending = 0U;
    g_mqtt_rx_msg_pending = 0U;
    // g_mqtt_need_full_reinit = 0U;
    g_mqtt_rx_pending_topic[0] = '\0';
    g_mqtt_rx_pending_payload[0] = '\0';
    mqtt_rx_parser_reset();
    g_mqtt_last_pub_ok_tick = HAL_GetTick();
    g_mqtt_last_mqtt_rx_tick = HAL_GetTick();
    g_mqtt_pub_ok_seq = 0U;
    g_mqtt_pub_timeout_count = 0U;
#if LOCKCTRL_EC800_AT_DIAG_UART5
    g_mqtt_diag_command_active = 0U;
    g_mqtt_diag_snapshot_active = 0U;
    g_mqtt_diag_snapshot_index = 0U;
    g_mqtt_diag_snapshot_next_tick = 0U;
#endif

    mqtt_copy_config_sub_topics();
    mqtt_copy_topic(g_mqtt_pub_topic, sizeof(g_mqtt_pub_topic), g_mqtt_cfg.pub_topic);

    g_mqtt_rt.last_rx_tick = HAL_GetTick();
    mqtt_clear_flags();
    mqtt_set_state(MQTT_CORE_ST_IDLE);
}

void MQTT_Core_SetPaused(uint8_t paused)
{
    if (paused)
    {
        g_mqtt_paused = 1U;
        MQTT_Core_CancelPendingPublish();
        if (g_mqtt_at_core != 0) { EC800_AT_CancelOwner(g_mqtt_at_core, EC800_AT_OWNER_MQTT); }
        g_mqtt_rt.connected = 0U;
        g_mqtt_rt.subscribed = 0U;
        return;
    }

    if (g_mqtt_paused)
    {
        g_mqtt_paused = 0U;
        MQTT_Core_Reset();
    }
}

void MQTT_Core_RequestReconnect(void) { g_mqtt_rt.need_reconnect = 1; }

void MQTT_Core_ForceReconnect(void) { mqtt_enter_reconnect_wait(); }

void MQTT_Core_UpdateBroker(const char* host, uint16_t port, const char* username, const char* password)
{
    if (host)
    {
        strncpy(g_mqtt_cfg.server_host, host, sizeof(g_mqtt_cfg.server_host) - 1U);
        g_mqtt_cfg.server_host[sizeof(g_mqtt_cfg.server_host) - 1U] = '\0';
    }
    if (port != 0U) g_mqtt_cfg.server_port = port;
    if (username)
    {
        strncpy(g_mqtt_cfg.username, username, sizeof(g_mqtt_cfg.username) - 1U);
        g_mqtt_cfg.username[sizeof(g_mqtt_cfg.username) - 1U] = '\0';
    }
    if (password)
    {
        strncpy(g_mqtt_cfg.password, password, sizeof(g_mqtt_cfg.password) - 1U);
        g_mqtt_cfg.password[sizeof(g_mqtt_cfg.password) - 1U] = '\0';
    }
    MQTT_Core_RequestReconnect();
}

void MQTT_Core_RequestIccid(void)
{
    if (g_mqtt_rt.state != MQTT_CORE_ST_READY) return;
    if (mqtt_pub_flow_active()) return;
    if (g_mqtt_iccid_query_pending) return;
    if ((g_mqtt_at_core != 0) && EC800_AT_IsBusy(g_mqtt_at_core)) return;

    if (mqtt_send_cmd("AT+QCCID\r\n"))
    {
        g_mqtt_iccid_query_pending = 1U;
        g_mqtt_iccid_query_tick = HAL_GetTick();
    }
}

uint8_t MQTT_Core_IsIccidValid(void) { return g_mqtt_iccid_valid; }
const char* MQTT_Core_GetIccid(void) { return g_mqtt_iccid_valid ? g_mqtt_iccid : 0; }
uint8_t MQTT_Core_IsIccidPending(void) { return g_mqtt_iccid_query_pending ? 1U : 0U; }

void MQTT_Core_SetTopics(const char* const* sub_topics, uint8_t sub_topic_count, const char* pub_topic)
{
    uint8_t changed = 0;
    uint8_t i;
    uint8_t new_count = sub_topic_count;

    if (sub_topics)
    {
        if (new_count > MQTT_CORE_MAX_SUB_TOPICS) new_count = MQTT_CORE_MAX_SUB_TOPICS;
        if (g_mqtt_sub_topic_count != new_count) changed = 1U;

        for (i = 0U; i < MQTT_CORE_MAX_SUB_TOPICS; i++)
        {
            const char* next = (i < new_count) ? sub_topics[i] : "";
            if (next == 0) next = "";
            if (strcmp(g_mqtt_sub_topics[i], next) != 0)
            {
                changed = 1U;
                break;
            }
        }

        if (changed)
        {
            mqtt_clear_sub_topics();
            g_mqtt_cfg.sub_topic_count = 0U;
            for (i = 0U; i < new_count; i++)
            {
                if ((sub_topics[i] == 0) || (sub_topics[i][0] == '\0')) continue;
                mqtt_copy_topic(g_mqtt_sub_topics[g_mqtt_sub_topic_count],
                                sizeof(g_mqtt_sub_topics[g_mqtt_sub_topic_count]), sub_topics[i]);
                mqtt_copy_topic(g_mqtt_cfg.sub_topics[g_mqtt_cfg.sub_topic_count],
                                sizeof(g_mqtt_cfg.sub_topics[g_mqtt_cfg.sub_topic_count]), sub_topics[i]);
                g_mqtt_sub_topic_count++;
                g_mqtt_cfg.sub_topic_count++;
            }
        }
    }

    if (pub_topic)
    {
        if (strcmp(g_mqtt_pub_topic, pub_topic) != 0)
        {
            mqtt_copy_topic(g_mqtt_pub_topic, sizeof(g_mqtt_pub_topic), pub_topic);
        }
    }

    if (changed)
    {
        g_mqtt_rt.need_resub = 0U;
        g_mqtt_rt.need_reconnect = 1U;
    }
}

uint8_t MQTT_Core_IsReady(void)
{ return (!g_mqtt_paused && (g_mqtt_rt.state == MQTT_CORE_ST_READY)) ? 1U : 0U; }

uint8_t MQTT_Core_CanRunBackgroundAt(void)
{
    mqtt_update_query_timeouts(HAL_GetTick());

    if (g_mqtt_rt.state != MQTT_CORE_ST_READY) return 0U;
    if (mqtt_pub_flow_active()) return 0U;
    if (g_mqtt_rx_msg_pending) return 0U;
    if (g_mqtt_iccid_query_pending) return 0U;
    if ((g_mqtt_at_core != 0) && EC800_AT_IsPayloadBusy(g_mqtt_at_core)) return 0U;
    if ((g_mqtt_at_core != 0) && EC800_AT_HasPending(g_mqtt_at_core, EC800_AT_OWNER_MQTT)) return 0U;
    return 1U;
}

#if LOCKCTRL_EC800_AT_DIAG_UART5
static uint8_t mqtt_diag_can_run_background(void)
{
    if (g_mqtt_rt.state == MQTT_CORE_ST_READY) return MQTT_Core_CanRunBackgroundAt();
    if ((g_mqtt_rt.state != MQTT_CORE_ST_NETWORK_WAIT) && (g_mqtt_rt.state != MQTT_CORE_ST_RECONNECT_WAIT))
        return 0U;
    if ((g_mqtt_at_core != 0) && EC800_AT_IsBusy(g_mqtt_at_core)) return 0U;
    if ((g_mqtt_at_core != 0) && EC800_AT_HasPending(g_mqtt_at_core, EC800_AT_OWNER_MQTT)) return 0U;
    return 1U;
}

static uint8_t mqtt_diag_snapshot_task(uint32_t now)
{
    if (!g_mqtt_diag_snapshot_active)
    {
        if ((g_mqtt_diag_snapshot_next_tick != 0U) &&
            ((int32_t)(now - g_mqtt_diag_snapshot_next_tick) < 0))
        {
            return 0U;
        }
        if (!mqtt_diag_can_run_background()) return 0U;

        g_mqtt_diag_snapshot_active = 1U;
        g_mqtt_diag_snapshot_index = 0U;
        g_mqtt_diag_snapshot_next_tick = now + MQTT_CORE_DIAG_SNAPSHOT_PERIOD_MS;
        APP_LOGI("DIAG", "snapshot start st=%u conn=%u sub=%u retry=%u rx_age=%lu pub_age=%lu",
                 (unsigned)g_mqtt_rt.state, (unsigned)g_mqtt_rt.connected, (unsigned)g_mqtt_rt.subscribed,
                 (unsigned)g_mqtt_rt.reconnect_count,
                 (unsigned long)(now - g_mqtt_rt.last_rx_tick),
                 (unsigned long)(now - g_mqtt_last_pub_ok_tick));
    }

    if (g_mqtt_diag_command_active)
    {
        if ((g_mqtt_at_core != 0) && EC800_AT_IsBusy(g_mqtt_at_core)) return 1U;
        g_mqtt_diag_command_active = 0U;
        if ((g_mqtt_diag_snapshot_index == 1U) && !g_mqtt_iccid_valid)
        {
            APP_LOGW("EC", "ICCID unavailable");
        }
        g_mqtt_rt.flag_cfg_ok = 0U;
        g_mqtt_rt.flag_error = 0U;
        g_mqtt_diag_snapshot_index++;
    }

    if (g_mqtt_diag_snapshot_index >= MQTT_CORE_DIAG_COUNT)
    {
        g_mqtt_diag_snapshot_active = 0U;
        APP_LOGI("DIAG", "snapshot done iccid=%u reg=%u attach=%u pdp=%u",
                 (unsigned)g_mqtt_iccid_valid, (unsigned)g_mqtt_rt.network_registered,
                 (unsigned)g_mqtt_rt.packet_attached, (unsigned)g_mqtt_rt.pdp_active);
        return 1U;
    }

    if (!mqtt_diag_can_run_background()) return 1U;

    g_mqtt_rt.flag_cfg_ok = 0U;
    g_mqtt_rt.flag_error = 0U;
    g_mqtt_diag_command_active = 1U;
    if (!mqtt_send_cmd_timeout(MQTT_CORE_DIAG_CMD_TIMEOUT_MS, "%s\r\n",
                               g_mqtt_diag_cmds[g_mqtt_diag_snapshot_index]))
    {
        g_mqtt_diag_command_active = 0U;
        g_mqtt_diag_snapshot_active = 0U;
        APP_LOGW("DIAG", "snapshot command queue failed idx=%u cmd=%s",
                 (unsigned)g_mqtt_diag_snapshot_index, g_mqtt_diag_cmds[g_mqtt_diag_snapshot_index]);
        return 1U;
    }

    APP_LOGI("DIAG", "snapshot[%u] %s", (unsigned)g_mqtt_diag_snapshot_index,
             g_mqtt_diag_cmds[g_mqtt_diag_snapshot_index]);
    return 1U;
}
#endif

MQTT_CoreState_t MQTT_Core_GetState(void) { return g_mqtt_rt.state; }

uint8_t MQTT_Core_IsPublishActive(void) { return mqtt_pub_flow_active(); }

uint32_t MQTT_Core_GetPubOkSeq(void) { return g_mqtt_pub_ok_seq; }

uint32_t MQTT_Core_GetLastRxAgeMs(void) { return HAL_GetTick() - g_mqtt_rt.last_rx_tick; }

uint32_t MQTT_Core_GetLastPubOkAgeMs(void) { return HAL_GetTick() - g_mqtt_last_pub_ok_tick; }

uint8_t MQTT_Core_Publish(const char* topic, const char* payload, uint8_t qos)
{
    return MQTT_Core_PublishEx(topic, payload, qos, 0U);
}

uint8_t MQTT_Core_PublishEx(const char* topic, const char* payload, uint8_t qos, uint8_t retain)
{
    const char* use_topic;
    uint16_t payload_len;

    if (g_mqtt_rt.state != MQTT_CORE_ST_READY) return 0;
    if (g_mqtt_rt.pub_busy) return 0;
    if ((g_mqtt_at_core != 0) && EC800_AT_IsBusy(g_mqtt_at_core)) return 0;
    if (payload == 0) return 0;
    if (qos > 2U || retain > 1U) return 0;

    use_topic = topic;
    if ((use_topic == 0) || (use_topic[0] == '\0')) { use_topic = g_mqtt_pub_topic; }
    if ((use_topic == 0) || (use_topic[0] == '\0')) return 0;

    payload_len = (uint16_t)strlen(payload);
    if (payload_len == 0) return 0;
    if (payload_len >= sizeof(g_mqtt_pub_pending_payload)) return 0;

    mqtt_copy_topic(g_mqtt_pub_pending_topic, sizeof(g_mqtt_pub_pending_topic), use_topic);
    strncpy(g_mqtt_pub_pending_payload, payload, sizeof(g_mqtt_pub_pending_payload) - 1U);
    g_mqtt_pub_pending_payload[sizeof(g_mqtt_pub_pending_payload) - 1U] = '\0';

    g_mqtt_rt.pub_busy = 1;
    g_mqtt_rt.pub_wait_prompt = 1;
    g_mqtt_rt.pub_wait_result = 0;
    g_mqtt_rt.pub_tick = HAL_GetTick();
    g_mqtt_pub_prompt_pending = 0U;
    g_mqtt_pub_pending_msg_id = mqtt_allocate_publish_msg_id(qos);

    if (!mqtt_send_cmd("AT+QMTPUBEX=%d,%u,%d,%u,\"%s\",%d\r\n", g_mqtt_cfg.client_idx,
                       (unsigned)g_mqtt_pub_pending_msg_id, qos, (unsigned)retain,
                       g_mqtt_pub_pending_topic, payload_len))
    {
        g_mqtt_rt.pub_busy = 0U;
        g_mqtt_rt.pub_wait_prompt = 0U;
        g_mqtt_rt.pub_wait_result = 0U;
        g_mqtt_pub_prompt_pending = 0U;
        g_mqtt_pub_pending_msg_id = 0U;
        g_mqtt_pub_pending_topic[0] = '\0';
        g_mqtt_pub_pending_payload[0] = '\0';
        return 0;
    }
    return 1;
}

void MQTT_Core_CancelPendingPublish(void)
{
    if (g_mqtt_at_core != 0) { EC800_AT_CancelQueuedOwner(g_mqtt_at_core, EC800_AT_OWNER_MQTT); }

    if (g_mqtt_rt.pub_busy || g_mqtt_rt.pub_wait_prompt || g_mqtt_rt.pub_wait_result)
    {
        g_mqtt_rt.pub_busy = 0U;
        g_mqtt_rt.pub_wait_prompt = 0U;
        g_mqtt_rt.pub_wait_result = 0U;
        g_mqtt_pub_prompt_pending = 0U;
        g_mqtt_pub_pending_msg_id = 0U;
        g_mqtt_pub_pending_topic[0] = '\0';
        g_mqtt_pub_pending_payload[0] = '\0';
        APP_LOGW("MQTT", "publish canceled");
    }
}

void MQTT_Core_Task(void)
{
    uint32_t wait_ms;

    if (g_mqtt_paused) return;

    if (g_mqtt_last_evt_pending)
    {
        char evt[128];

        __disable_irq();
        strncpy(evt, g_mqtt_last_evt_line, sizeof(evt) - 1U);
        evt[sizeof(evt) - 1U] = '\0';
        g_mqtt_last_evt_pending = 0U;
        __enable_irq();

        mqtt_dbg_printf("[MQTT] EVT: %s\r\n", evt);
    }

#if LOCKCTRL_EC800_AT_DIAG_UART5
    if ((g_mqtt_rt.state == MQTT_CORE_ST_READY) || (g_mqtt_rt.state == MQTT_CORE_ST_NETWORK_WAIT) ||
        (g_mqtt_rt.state == MQTT_CORE_ST_RECONNECT_WAIT))
    {
        if (mqtt_diag_snapshot_task(HAL_GetTick())) return;
    }
#endif

    switch (g_mqtt_rt.state)
    {
        case MQTT_CORE_ST_IDLE:
            g_mqtt_rt.retry_in_state = 0;
            mqtt_set_state(MQTT_CORE_ST_WAIT_BOOT);
            break;

        case MQTT_CORE_ST_WAIT_BOOT:
            if (mqtt_state_timeout(MQTT_CORE_BOOT_WAIT_MS))
            {
                g_mqtt_rt.retry_in_state = 0;
                mqtt_set_state(MQTT_CORE_ST_ECHO_OFF);
            }
            break;

        case MQTT_CORE_ST_ECHO_OFF:
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_ECHO_OFF);
            mqtt_send_cmd("ATE0\r\n");
            break;

        case MQTT_CORE_ST_WAIT_ECHO_OFF:
            if (g_mqtt_rt.flag_cfg_ok)
            {
                g_mqtt_rt.retry_in_state = 0;
                mqtt_set_state(MQTT_CORE_ST_CFG);
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_CFG_TIMEOUT_MS)) { mqtt_fail_current_step(); }
            break;

        case MQTT_CORE_ST_CFG:
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_CFG);
            mqtt_send_cmd("AT+QMTCFG=\"recv/mode\",%d,0,0\r\n", g_mqtt_cfg.client_idx);
            break;

        case MQTT_CORE_ST_WAIT_CFG:
            if (g_mqtt_rt.flag_cfg_ok)
            {
                g_mqtt_rt.retry_in_state = 0;
#if LOCKCTRL_EC800_AT_DIAG_UART5
                g_mqtt_rt.diag_index = 0U;
                mqtt_set_state(MQTT_CORE_ST_DIAG);
#else
                mqtt_set_state(MQTT_CORE_ST_NETWORK_QUERY);
#endif
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_CFG_TIMEOUT_MS)) { mqtt_fail_current_step(); }
            break;

        case MQTT_CORE_ST_DIAG:
#if LOCKCTRL_EC800_AT_DIAG_UART5
            if (g_mqtt_rt.diag_index >= MQTT_CORE_DIAG_COUNT)
            {
                APP_LOGI("EC", "MQTT pre-open diagnostics complete");
                mqtt_set_state(MQTT_CORE_ST_NETWORK_QUERY);
                break;
            }

            mqtt_clear_flags();
            APP_LOGI("EC", "diag[%u] %s -> %s", (unsigned)g_mqtt_rt.diag_index,
                     g_mqtt_diag_names[g_mqtt_rt.diag_index], g_mqtt_diag_cmds[g_mqtt_rt.diag_index]);
            mqtt_set_state(MQTT_CORE_ST_WAIT_DIAG);
            if (!mqtt_send_cmd("%s\r\n", g_mqtt_diag_cmds[g_mqtt_rt.diag_index]))
            {
                APP_LOGW("EC", "diag[%u] command queue failed", (unsigned)g_mqtt_rt.diag_index);
            }
#else
            mqtt_set_state(MQTT_CORE_ST_NETWORK_QUERY);
#endif
            break;

        case MQTT_CORE_ST_WAIT_DIAG:
#if LOCKCTRL_EC800_AT_DIAG_UART5
            if (g_mqtt_rt.flag_cfg_ok)
            {
                APP_LOGI("EC", "diag[%u] %s terminal=OK", (unsigned)g_mqtt_rt.diag_index,
                         g_mqtt_diag_names[g_mqtt_rt.diag_index]);
                g_mqtt_rt.diag_index++;
                mqtt_set_state(MQTT_CORE_ST_DIAG);
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_DIAG_TIMEOUT_MS))
            {
                APP_LOGW("EC", "diag[%u] %s terminal=%s", (unsigned)g_mqtt_rt.diag_index,
                         g_mqtt_diag_names[g_mqtt_rt.diag_index], g_mqtt_rt.flag_error ? "ERROR" : "TIMEOUT");
                if ((strcmp(g_mqtt_diag_names[g_mqtt_rt.diag_index], "ICCID") == 0) &&
                    !g_mqtt_iccid_valid)
                {
                    APP_LOGW("EC", "ICCID unavailable");
                }
                g_mqtt_rt.diag_index++;
                mqtt_set_state(MQTT_CORE_ST_DIAG);
            }
#else
            mqtt_set_state(MQTT_CORE_ST_NETWORK_QUERY);
#endif
            break;

        case MQTT_CORE_ST_NETWORK_QUERY:
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_NETWORK_QUERY);
            mqtt_send_cmd("AT+CEREG?\r\n");
            break;

        case MQTT_CORE_ST_WAIT_NETWORK_QUERY:
            if (g_mqtt_rt.flag_cfg_ok)
            {
                if (g_mqtt_rt.network_registered)
                {
                    APP_LOGI("MQTT", "packet network registered");
                    mqtt_set_state(MQTT_CORE_ST_ATTACH_QUERY);
                }
                else
                {
                    APP_LOGW("MQTT", "packet network not registered; waiting");
                    mqtt_set_state(MQTT_CORE_ST_NETWORK_WAIT);
                }
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_CFG_TIMEOUT_MS))
            {
                APP_LOGW("MQTT", "network registration query failed; waiting");
                mqtt_set_state(MQTT_CORE_ST_NETWORK_WAIT);
            }
            break;

        case MQTT_CORE_ST_ATTACH_QUERY:
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_ATTACH_QUERY);
            mqtt_send_cmd("AT+CGATT?\r\n");
            break;

        case MQTT_CORE_ST_WAIT_ATTACH_QUERY:
            if (g_mqtt_rt.flag_cfg_ok)
            {
                if (g_mqtt_rt.packet_attached)
                {
                    APP_LOGI("MQTT", "packet service attached");
                    mqtt_set_state(MQTT_CORE_ST_PDP_QUERY);
                }
                else
                {
                    APP_LOGW("MQTT", "packet service not attached; attaching");
                    mqtt_set_state(MQTT_CORE_ST_ATTACH);
                }
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_CFG_TIMEOUT_MS))
            {
                APP_LOGW("MQTT", "packet attach query failed; waiting");
                mqtt_set_state(MQTT_CORE_ST_NETWORK_WAIT);
            }
            break;

        case MQTT_CORE_ST_ATTACH:
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_ATTACH);
            mqtt_send_cmd_timeout(MQTT_CORE_ATTACH_AT_TIMEOUT_MS, "AT+CGATT=1\r\n");
            break;

        case MQTT_CORE_ST_WAIT_ATTACH:
            if (g_mqtt_rt.flag_cfg_ok)
            {
                APP_LOGI("MQTT", "packet attach command accepted");
                mqtt_set_state(MQTT_CORE_ST_PDP_QUERY);
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_ATTACH_TIMEOUT_MS))
            {
                APP_LOGW("MQTT", "packet attach failed; returning to network wait");
                mqtt_set_state(MQTT_CORE_ST_NETWORK_WAIT);
            }
            break;

        case MQTT_CORE_ST_NETWORK_WAIT:
            if (mqtt_state_timeout(MQTT_CORE_NETWORK_RETRY_MS))
            {
                mqtt_set_state(MQTT_CORE_ST_NETWORK_QUERY);
            }
            break;

        case MQTT_CORE_ST_PDP_QUERY:
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_PDP_QUERY);
            mqtt_send_cmd("AT+QIACT?\r\n");
            break;

        case MQTT_CORE_ST_WAIT_PDP_QUERY:
            if (g_mqtt_rt.flag_cfg_ok)
            {
                if (g_mqtt_rt.pdp_active)
                {
                    APP_LOGI("MQTT", "PDP context 1 already active");
                    mqtt_set_state(MQTT_CORE_ST_OPEN);
                }
                else
                {
                    APP_LOGW("MQTT", "PDP context 1 inactive; configuring APN and activating");
                    mqtt_set_state(MQTT_CORE_ST_PDP_CONFIG);
                }
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_CFG_TIMEOUT_MS))
            {
                APP_LOGW("MQTT", "PDP query failed; configuring APN and activating");
                mqtt_set_state(MQTT_CORE_ST_PDP_CONFIG);
            }
            break;

        case MQTT_CORE_ST_PDP_CONFIG:
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_PDP_CONFIG);
            mqtt_send_cmd("AT+QICSGP=1,1,\"%s\",\"\",\"\",1\r\n", MQTT_CORE_PDP_APN);
            break;

        case MQTT_CORE_ST_WAIT_PDP_CONFIG:
            if (g_mqtt_rt.flag_cfg_ok)
            {
                mqtt_set_state(MQTT_CORE_ST_PDP_ACTIVATE);
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_CFG_TIMEOUT_MS))
            {
                APP_LOGW("MQTT", "PDP APN configuration failed");
                mqtt_enter_reconnect_wait();
            }
            break;

        case MQTT_CORE_ST_PDP_ACTIVATE:
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_PDP_ACTIVATE);
            mqtt_send_cmd_timeout(MQTT_CORE_PDP_AT_TIMEOUT_MS, "AT+QIACT=1\r\n");
            break;

        case MQTT_CORE_ST_WAIT_PDP_ACTIVATE:
            if (g_mqtt_rt.flag_cfg_ok)
            {
                mqtt_set_state(MQTT_CORE_ST_PDP_VERIFY);
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_PDP_TIMEOUT_MS))
            {
                APP_LOGW("MQTT", "PDP activation failed");
                mqtt_enter_reconnect_wait();
            }
            break;

        case MQTT_CORE_ST_PDP_VERIFY:
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_PDP_VERIFY);
            mqtt_send_cmd("AT+QIACT?\r\n");
            break;

        case MQTT_CORE_ST_WAIT_PDP_VERIFY:
            if (g_mqtt_rt.flag_cfg_ok)
            {
                if (g_mqtt_rt.pdp_active)
                {
                    APP_LOGI("MQTT", "PDP context 1 active; opening MQTT network");
                    mqtt_set_state(MQTT_CORE_ST_OPEN);
                }
                else
                {
                    APP_LOGW("MQTT", "PDP activation returned OK but no active IP context");
                    mqtt_enter_reconnect_wait();
                }
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_CFG_TIMEOUT_MS))
            {
                APP_LOGW("MQTT", "PDP verification failed");
                mqtt_enter_reconnect_wait();
            }
            break;

        case MQTT_CORE_ST_OPEN:
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_OPEN);
            mqtt_send_cmd("AT+QMTOPEN=%d,\"%s\",%d\r\n", g_mqtt_cfg.client_idx, g_mqtt_cfg.server_host,
                          g_mqtt_cfg.server_port);
            break;

        case MQTT_CORE_ST_WAIT_OPEN:
            if (g_mqtt_rt.flag_open_ok)
            {
                g_mqtt_rt.connected = 0;
                g_mqtt_rt.need_reconnect = 0U;
                g_mqtt_rt.retry_in_state = 0;
                mqtt_set_state(MQTT_CORE_ST_CONN);
            }
            else if (g_mqtt_rt.flag_error)
            {
                mqtt_enter_reconnect_wait();
            }
            else if (mqtt_state_timeout(MQTT_CORE_OPEN_TIMEOUT_MS))
            {
                APP_LOGW("MQTT", "QMTOPEN timeout; closing client before retry");
                mqtt_enter_reconnect_wait();
            }
            break;

        case MQTT_CORE_ST_CONN:
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_CONN);
            if (g_mqtt_cfg.username[0] == '\0')
            {
                mqtt_send_cmd("AT+QMTCONN=%d,\"%s\"\r\n", g_mqtt_cfg.client_idx, g_mqtt_cfg.client_id);
            }
            else
            {
                mqtt_send_cmd("AT+QMTCONN=%d,\"%s\",\"%s\",\"%s\"\r\n", g_mqtt_cfg.client_idx,
                              g_mqtt_cfg.client_id, g_mqtt_cfg.username, g_mqtt_cfg.password);
            }
            break;

        case MQTT_CORE_ST_WAIT_CONN:
            if (g_mqtt_rt.flag_conn_ok)
            {
                g_mqtt_rt.connected = 1;
                g_mqtt_rt.need_reconnect = 0U;
                g_mqtt_rt.retry_in_state = 0;
                g_mqtt_rt.sub_index = 0U;
                mqtt_set_state(MQTT_CORE_ST_SUB);
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_CONN_TIMEOUT_MS))
            {
                mqtt_fail_current_step();
            }
            break;

        case MQTT_CORE_ST_SUB:
            if ((g_mqtt_sub_topic_count == 0U) || (g_mqtt_rt.sub_index >= g_mqtt_sub_topic_count))
            {
                mqtt_set_ready_after_subscribe();
                break;
            }
            mqtt_clear_flags();
            mqtt_set_state(MQTT_CORE_ST_WAIT_SUB);
            mqtt_send_cmd("AT+QMTSUB=%d,1,\"%s\",0\r\n", g_mqtt_cfg.client_idx, g_mqtt_sub_topics[g_mqtt_rt.sub_index]);
            break;

        case MQTT_CORE_ST_WAIT_SUB:
            if (g_mqtt_rt.flag_sub_ok)
            {
                g_mqtt_rt.flag_sub_ok = 0U;
                g_mqtt_rt.sub_index++;
                if (g_mqtt_rt.sub_index < g_mqtt_sub_topic_count)
                {
                    mqtt_set_state(MQTT_CORE_ST_SUB);
                }
                else
                {
                    mqtt_set_ready_after_subscribe();
                }
            }
            else if (g_mqtt_rt.flag_error || mqtt_state_timeout(MQTT_CORE_SUB_TIMEOUT_MS)) { mqtt_fail_current_step(); }
            break;

        case MQTT_CORE_ST_READY:
        {
            static uint32_t last_time_query = 0;
            uint32_t now = HAL_GetTick();

            // if (g_mqtt_need_full_reinit)
            // {
            //     g_mqtt_need_full_reinit = 0U;
            //     g_mqtt_rt.retry_in_state = 0U;
            //     mqtt_clear_flags();
            //     mqtt_set_state(MQTT_CORE_ST_WAIT_BOOT);
            //     break;
            // }

            if (g_mqtt_pub_prompt_pending)
            {
                g_mqtt_pub_prompt_pending = 0U;
                if (g_mqtt_rt.pub_wait_prompt)
                {
                    mqtt_handle_prompt_char();
                    break;
                }
            }

            if (g_mqtt_rt.pub_wait_prompt && (g_mqtt_at_core != 0) &&
                (!EC800_AT_IsActiveOwner(g_mqtt_at_core, EC800_AT_OWNER_MQTT)))
            {
                if (EC800_AT_HasPending(g_mqtt_at_core, EC800_AT_OWNER_MQTT)) { break; }

                APP_LOGW("MQTT", "publish command lost");
                mqtt_clear_publish_state();
                mqtt_set_state(MQTT_CORE_ST_RECOVER_PUB);
                break;
            }

            if (g_mqtt_rt.pub_wait_prompt && ((now - g_mqtt_rt.pub_tick) > MQTT_CORE_PUB_PROMPT_TIMEOUT_MS))
            {
                mqtt_handle_prompt_timeout();
                break;
            }

            if (g_mqtt_rt.pub_busy && ((now - g_mqtt_rt.pub_tick) > MQTT_CORE_PUB_TIMEOUT_MS))
            {
                if (g_mqtt_at_core != 0) { EC800_AT_CancelOwner(g_mqtt_at_core, EC800_AT_OWNER_MQTT); }
                if (g_mqtt_pub_timeout_count < 250U) { g_mqtt_pub_timeout_count++; }
                APP_LOGW("MQTT", "publish timeout cnt=%u st=%u topic=%s", (unsigned)g_mqtt_pub_timeout_count,
                         (unsigned)g_mqtt_rt.state, g_mqtt_pub_pending_topic);
                mqtt_clear_publish_state();
                if (g_mqtt_pub_timeout_count >= MQTT_CORE_PUB_TIMEOUT_RECONNECT_COUNT)
                {
                    mqtt_set_state(MQTT_CORE_ST_RECOVER_PUB);
                }
                break;
            }

            if ((now - g_mqtt_rt.last_rx_tick) > MQTT_CORE_READY_LINE_SILENCE_MS)
            {
                mqtt_dbg_printf("[MQTT] ready silence timeout\r\n");
                mqtt_enter_reconnect_wait();
                break;
            }

            if ((now - g_mqtt_last_pub_ok_tick) > MQTT_CORE_PUB_OK_TIMEOUT_MS)
            {
                mqtt_dbg_printf("[MQTT] no publish success too long\r\n");
                mqtt_enter_reconnect_wait();
                break;
            }

            if (g_mqtt_rx_msg_pending)
            {
                char topic[MQTT_CORE_TOPIC_BUF_SZ];
                char payload[MQTT_CORE_PAYLOAD_BUF_SZ];

                __disable_irq();
                mqtt_copy_topic(topic, sizeof(topic), g_mqtt_rx_pending_topic);
                strncpy(payload, g_mqtt_rx_pending_payload, sizeof(payload) - 1U);
                payload[sizeof(payload) - 1U] = '\0';
                g_mqtt_rx_msg_pending = 0U;
                g_mqtt_rx_pending_topic[0] = '\0';
                g_mqtt_rx_pending_payload[0] = '\0';
                __enable_irq();

                mqtt_dispatch_message(topic, payload);
                break;
            }

            mqtt_update_query_timeouts(now);

            if (mqtt_pub_flow_active())
            {
                if (g_mqtt_rt.flag_closed || g_mqtt_rt.flag_error || g_mqtt_rt.need_reconnect)
                {
                    mqtt_set_state(MQTT_CORE_ST_RECOVER_PUB);
                }
                break;
            }

            if ((last_time_query == 0U) || ((now - last_time_query) > 60000U))
            {
                last_time_query = now;
                mqtt_send_cmd("AT+CCLK?\r\n");
                break;
            }

            if ((!g_mqtt_iccid_valid) && (!g_mqtt_iccid_query_pending))
            {
                MQTT_Core_RequestIccid();
                break;
            }

            if (g_mqtt_rt.flag_closed || g_mqtt_rt.flag_error || g_mqtt_rt.need_reconnect)
            {
                mqtt_enter_reconnect_wait();
            }
            else if (g_mqtt_rt.need_resub)
            {
                g_mqtt_rt.need_resub = 0;
                g_mqtt_rt.subscribed = 0;
                g_mqtt_rt.retry_in_state = 0;
                g_mqtt_rt.sub_index = 0U;
                mqtt_set_state(MQTT_CORE_ST_SUB);
            }
        }
        break;

        case MQTT_CORE_ST_RECOVER_PUB:
            if (mqtt_state_timeout(MQTT_CORE_PUB_RECOVER_MS)) { mqtt_enter_reconnect_wait(); }
            break;

        case MQTT_CORE_ST_RECONNECT_WAIT:
            wait_ms = mqtt_calc_reconnect_delay(g_mqtt_rt.reconnect_count);
            if (mqtt_state_timeout(wait_ms))
            {
                mqtt_dbg_printf("[MQTT] reconnect after %lu ms\r\n", wait_ms);
                g_mqtt_rt.retry_in_state = 0;
                mqtt_clear_flags();
                mqtt_set_state(MQTT_CORE_ST_ECHO_OFF);
            }
            break;

        case MQTT_CORE_ST_ERROR:
        default: mqtt_enter_reconnect_wait(); break;
    }
}

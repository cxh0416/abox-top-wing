#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "main.h"
#include "mqtt_core_int.h"

#define MQTT_AT_TIMEOUT_MS 5000U

void mqtt_dbg_printf(const char* fmt, ...)
{
#if LOCKCTRL_EC800_AT_DIAG_UART5
    char msg[128];
    va_list ap;

    if (fmt == 0) return;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    AppLog_Printf(APP_LOG_LEVEL_DEBUG, "MQTT", "%s", msg);
#else
    if (__get_IPSR() != 0U) return;
    (void)fmt;
#endif
}

void mqtt_uart_send_raw(const uint8_t* data, uint16_t len)
{
    if ((data == 0) || (len == 0)) return;
    if (g_mqtt_at_core == 0) return;
    if (!EC800_AT_SendPayloadForOwner(g_mqtt_at_core, EC800_AT_OWNER_MQTT, data, len))
    {
        g_mqtt_rt.flag_error = 1U;
        APP_LOGW("MQTT", "payload owner mismatch");
    }
}

uint8_t mqtt_uart_force_send_raw(const uint8_t* data, uint16_t len)
{
    if ((data == 0) || (len == 0)) return 0U;
    if (g_mqtt_at_core == 0) return 0U;
    if (!EC800_AT_ForceSendPayloadForOwner(g_mqtt_at_core, EC800_AT_OWNER_MQTT, data, len))
    {
        g_mqtt_rt.flag_error = 1U;
        APP_LOGW("MQTT", "payload force send failed");
        return 0U;
    }
    return 1U;
}

static uint8_t mqtt_submit_formatted_cmd(uint32_t timeout_ms, const char* fmt, va_list ap)
{
    char buf[MQTT_CORE_CMD_BUF_SZ];
    int n;

    n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (n <= 0) return 0U;
    if (n >= (int)sizeof(buf))
    {
        n = sizeof(buf) - 1;
        buf[n] = '\0';
    }

    if ((g_mqtt_at_core == 0) ||
        (!EC800_AT_Submit(g_mqtt_at_core, buf, EC800_AT_OWNER_MQTT, EC800_AT_PRIORITY_HIGH,
                          timeout_ms ? timeout_ms : MQTT_AT_TIMEOUT_MS, 0, 0)))
    {
        g_mqtt_rt.flag_error = 1U;
        APP_LOGW("MQTT", "at queue full");
        return 0U;
    }

    return 1U;
}

uint8_t mqtt_send_cmd_timeout(uint32_t timeout_ms, const char* fmt, ...)
{
    uint8_t ok;
    va_list ap;

    va_start(ap, fmt);
    ok = mqtt_submit_formatted_cmd(timeout_ms, fmt, ap);
    va_end(ap);
    return ok;
}

uint8_t mqtt_send_cmd(const char* fmt, ...)
{
    uint8_t ok;
    va_list ap;

    va_start(ap, fmt);
    ok = mqtt_submit_formatted_cmd(MQTT_AT_TIMEOUT_MS, fmt, ap);
    va_end(ap);
    return ok;
}

void mqtt_clear_flags(void)
{
    g_mqtt_rt.flag_cfg_ok = 0;
    g_mqtt_rt.flag_open_ok = 0;
    g_mqtt_rt.flag_conn_ok = 0;
    g_mqtt_rt.flag_sub_ok = 0;
    g_mqtt_rt.flag_closed = 0;
    g_mqtt_rt.flag_error = 0;
    g_mqtt_rt.network_registered = 0U;
    g_mqtt_rt.packet_attached = 0U;
    g_mqtt_rt.pdp_active = 0U;
}

void mqtt_set_state(MQTT_CoreState_t st)
{
    g_mqtt_rt.state = st;
    g_mqtt_rt.state_tick = HAL_GetTick();
    mqtt_dbg_printf("[MQTT] state -> %d\r\n", (int)st);
    APP_LOGI("MQTT", "state=%d", (int)st);
}

uint8_t mqtt_state_timeout(uint32_t timeout_ms)
{ return ((HAL_GetTick() - g_mqtt_rt.state_tick) >= timeout_ms) ? 1U : 0U; }

uint32_t mqtt_calc_reconnect_delay(uint8_t reconnect_count)
{
    uint32_t delay = MQTT_CORE_RECONNECT_DELAY_MIN_MS;

    while (reconnect_count > 0U)
    {
        if (delay >= (MQTT_CORE_RECONNECT_DELAY_MAX_MS / 2U))
        {
            delay = MQTT_CORE_RECONNECT_DELAY_MAX_MS;
            break;
        }
        delay *= 2U;
        reconnect_count--;
    }

    if (delay > MQTT_CORE_RECONNECT_DELAY_MAX_MS) { delay = MQTT_CORE_RECONNECT_DELAY_MAX_MS; }
    return delay;
}

void mqtt_copy_topic(char* dst, uint16_t dst_sz, const char* src)
{
    if ((dst == 0) || (dst_sz == 0)) return;
    if (src == 0)
    {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_sz - 1U);
    dst[dst_sz - 1U] = '\0';
}

void mqtt_dispatch_message(const char* topic, const char* payload)
{
    if (g_mqtt_msg_cb) { g_mqtt_msg_cb(topic, payload); }
}

void mqtt_enter_reconnect_wait(void)
{
    uint8_t need_close = 0U;
    MQTT_CoreState_t st = g_mqtt_rt.state;
    uint8_t pub_active = (g_mqtt_rt.pub_busy || g_mqtt_rt.pub_wait_prompt || g_mqtt_rt.pub_wait_result) ? 1U : 0U;

    if (g_mqtt_rt.connected || g_mqtt_rt.subscribed || g_mqtt_rt.pub_busy || g_mqtt_rt.pub_wait_prompt ||
        g_mqtt_rt.pub_wait_result)
    {
        need_close = 1U;
    }
    if ((st >= MQTT_CORE_ST_WAIT_OPEN) && (st <= MQTT_CORE_ST_READY)) { need_close = 1U; }

    if (g_mqtt_at_core != 0) { EC800_AT_CancelQueuedOwner(g_mqtt_at_core, EC800_AT_OWNER_MQTT); }

#if LOCKCTRL_EC800_AT_DIAG_UART5
    g_mqtt_diag_command_active = 0U;
    /* A reconnect is a useful boundary for an immediate offline snapshot. */
    mqtt_diag_reset_snapshot_state();
#endif

    g_mqtt_rt.connected = 0;
    g_mqtt_rt.subscribed = 0;
    g_mqtt_rt.need_reconnect = 0;

    g_mqtt_rt.pub_busy = 0;
    g_mqtt_rt.pub_wait_prompt = 0;
    g_mqtt_rt.pub_wait_result = 0;
    g_mqtt_pub_prompt_pending = 0U;
    g_mqtt_pub_pending_msg_id = 0U;
    g_mqtt_pub_pending_topic[0] = '\0';
    g_mqtt_pub_pending_payload[0] = '\0';
    g_mqtt_line_len = 0;
    g_mqtt_rx_drop_until_lf = 0;
    mqtt_rx_parser_reset();

    if (need_close) { mqtt_send_cmd("AT+QMTCLOSE=%d\r\n", g_mqtt_cfg.client_idx); }

    if (g_mqtt_rt.reconnect_count < 250U) { g_mqtt_rt.reconnect_count++; }

    APP_LOGW("MQTT", "reconnect st=%u close=%u err=%u pub=%u", (unsigned)st, (unsigned)g_mqtt_rt.flag_closed,
             (unsigned)g_mqtt_rt.flag_error, (unsigned)pub_active);
    mqtt_clear_flags();
    mqtt_set_state(MQTT_CORE_ST_RECONNECT_WAIT);
}

void mqtt_fail_current_step(void)
{
    g_mqtt_rt.connected = 0;
    g_mqtt_rt.subscribed = 0;

    if (g_mqtt_rt.retry_in_state < MQTT_CORE_MAX_STATE_RETRY)
    {
        g_mqtt_rt.retry_in_state++;
        mqtt_dbg_printf("[MQTT] retry_in_state=%d\r\n", (int)g_mqtt_rt.retry_in_state);

        switch (g_mqtt_rt.state)
        {
            case MQTT_CORE_ST_WAIT_ECHO_OFF: mqtt_set_state(MQTT_CORE_ST_ECHO_OFF); break;
            case MQTT_CORE_ST_WAIT_CFG: mqtt_set_state(MQTT_CORE_ST_CFG); break;
            case MQTT_CORE_ST_WAIT_OPEN: mqtt_set_state(MQTT_CORE_ST_OPEN); break;
            case MQTT_CORE_ST_WAIT_CONN: mqtt_set_state(MQTT_CORE_ST_CONN); break;
            case MQTT_CORE_ST_WAIT_SUB: mqtt_set_state(MQTT_CORE_ST_SUB); break;
            default: mqtt_enter_reconnect_wait(); break;
        }
    }
    else
    {
        mqtt_enter_reconnect_wait();
    }
}

void mqtt_handle_prompt_char(void)
{
    uint16_t payload_len;

    if ((!g_mqtt_rt.pub_busy) || (!g_mqtt_rt.pub_wait_prompt))
    {
        mqtt_dbg_printf("[MQTT] prompt ignored\r\n");
        return;
    }

    payload_len = (uint16_t)strlen(g_mqtt_pub_pending_payload);
    if (payload_len == 0U)
    {
        mqtt_dbg_printf("[MQTT] payload empty\r\n");
        g_mqtt_rt.pub_busy = 0;
        g_mqtt_rt.pub_wait_prompt = 0;
        g_mqtt_rt.pub_wait_result = 0;
        g_mqtt_pub_pending_msg_id = 0U;
        g_mqtt_rt.flag_error = 1;
        return;
    }

    mqtt_dbg_printf("[MQTT] PUB payload len=%u\r\n", payload_len);

    mqtt_uart_send_raw((const uint8_t*)g_mqtt_pub_pending_payload, payload_len);
    mqtt_dbg_printf("[MQTT] payload sent\r\n");

    g_mqtt_rt.pub_wait_prompt = 0;
    g_mqtt_rt.pub_wait_result = 1;
    g_mqtt_rt.pub_tick = HAL_GetTick();
}

void mqtt_handle_prompt_timeout(void)
{
    uint16_t payload_len;

    if ((!g_mqtt_rt.pub_busy) || (!g_mqtt_rt.pub_wait_prompt)) return;

    payload_len = (uint16_t)strlen(g_mqtt_pub_pending_payload);
    if (payload_len == 0U)
    {
        g_mqtt_rt.pub_busy = 0U;
        g_mqtt_rt.pub_wait_prompt = 0U;
        g_mqtt_rt.pub_wait_result = 0U;
        g_mqtt_pub_pending_msg_id = 0U;
        g_mqtt_rt.flag_error = 1U;
        return;
    }

    if (mqtt_uart_force_send_raw((const uint8_t*)g_mqtt_pub_pending_payload, payload_len))
    {
        g_mqtt_rt.pub_wait_prompt = 0U;
        g_mqtt_rt.pub_wait_result = 1U;
        g_mqtt_pub_prompt_pending = 0U;
        g_mqtt_rt.pub_tick = HAL_GetTick();
        APP_LOGD("MQTT", "publish prompt fallback st=%u topic=%s", (unsigned)g_mqtt_rt.state, g_mqtt_pub_pending_topic);
        return;
    }

    APP_LOGW("MQTT", "publish prompt timeout st=%u topic=%s", (unsigned)g_mqtt_rt.state, g_mqtt_pub_pending_topic);
    if (g_mqtt_at_core != 0) { EC800_AT_CancelOwner(g_mqtt_at_core, EC800_AT_OWNER_MQTT); }
    g_mqtt_rt.pub_busy = 0U;
    g_mqtt_rt.pub_wait_prompt = 0U;
    g_mqtt_rt.pub_wait_result = 0U;
    g_mqtt_pub_prompt_pending = 0U;
    g_mqtt_pub_pending_msg_id = 0U;
    g_mqtt_pub_pending_topic[0] = '\0';
    g_mqtt_pub_pending_payload[0] = '\0';
    g_mqtt_rt.flag_error = 1U;
    mqtt_set_state(MQTT_CORE_ST_RECOVER_PUB);
}

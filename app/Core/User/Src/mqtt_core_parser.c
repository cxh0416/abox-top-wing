#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "app_diag.h"
#include "main.h"
#include "mqtt_core_int.h"
#include "time_base.h"

extern volatile uint8_t g_mqtt_pub_prompt_pending;
extern volatile uint8_t g_mqtt_rx_msg_pending;
extern char g_mqtt_rx_pending_topic[MQTT_CORE_TOPIC_BUF_SZ];
extern char g_mqtt_rx_pending_payload[MQTT_CORE_PAYLOAD_BUF_SZ];
extern uint32_t g_mqtt_last_pub_ok_tick;
extern uint32_t g_mqtt_last_mqtt_rx_tick;
extern volatile uint8_t g_mqtt_last_evt_pending;
extern char g_mqtt_last_evt_line[128];

#define MQTT_CORE_RX_MULTILINE_TIMEOUT_MS 3000U

static uint8_t g_mqtt_rx_collecting = 0U;
static uint16_t g_mqtt_rx_collect_len = 0U;
static uint32_t g_mqtt_rx_collect_tick = 0U;

#if LOCKCTRL_EC800_AT_DIAG_UART5
static void mqtt_diag_log_line(const char* line)
{
    if (line == 0) return;
    if (g_mqtt_rx_collecting || (strstr(line, "+QMTRECV:") != 0))
    {
        APP_LOGD("AT", "RX MQTT payload suppressed len=%u", (unsigned)strlen(line));
    }
    else if (strstr(line, "+QCCID:") != 0)
    {
        APP_LOGD("AT", "RX +QCCID value suppressed");
    }
    else if ((line[0] == '{') || (line[0] == '['))
    {
        APP_LOGD("AT", "RX MQTT payload suppressed len=%u", (unsigned)strlen(line));
    }
    else
    {
        APP_LOGD("AT", "RX %s", line);
    }
}
#endif

static uint8_t mqtt_parse_publish_result(const char* line, uint8_t* client_idx, uint16_t* msg_id, int* result)
{
    const char* value;
    unsigned client_value;
    unsigned msg_value;
    int result_value;

    if ((line == 0) || (client_idx == 0) || (msg_id == 0) || (result == 0)) return 0U;
    value = strstr(line, "+QMTPUBEX:");
    if (value == 0) return 0U;
    value += strlen("+QMTPUBEX:");

    if (sscanf(value, " %u,%u,%d", &client_value, &msg_value, &result_value) != 3) return 0U;
    if ((client_value > 255U) || (msg_value > 65535U)) return 0U;

    *client_idx = (uint8_t)client_value;
    *msg_id = (uint16_t)msg_value;
    *result = result_value;
    return 1U;
}

static void mqtt_save_last_evt(const char* line)
{
    if (line == 0) return;

    __disable_irq();
    if (strstr(line, "+QMTRECV:") != 0)
    {
        strncpy(g_mqtt_last_evt_line, "+QMTRECV: payload suppressed", sizeof(g_mqtt_last_evt_line) - 1U);
    }
    else if (strstr(line, "+QCCID:") != 0)
    {
        strncpy(g_mqtt_last_evt_line, "+QCCID: value suppressed", sizeof(g_mqtt_last_evt_line) - 1U);
    }
    else
    {
        strncpy(g_mqtt_last_evt_line, line, sizeof(g_mqtt_last_evt_line) - 1U);
    }
    g_mqtt_last_evt_line[sizeof(g_mqtt_last_evt_line) - 1U] = '\0';
    g_mqtt_last_evt_pending = 1U;
    __enable_irq();
}

static int mqtt_parse_qccid_line(const char* line, char* iccid, uint16_t size)
{
    const char* p;
    uint16_t n = 0;

    if ((line == 0) || (iccid == 0) || (size == 0)) return 0;

    p = strstr(line, "+QCCID:");
    if (p == 0) return 0;

    p += strlen("+QCCID:");
    while ((*p == ' ') || (*p == '\t')) p++;
    while ((*p >= '0') && (*p <= '9') && (n < (size - 1U))) { iccid[n++] = *p++; }

    iccid[n] = '\0';
    return (n > 0U) ? 1 : 0;
}

void mqtt_rx_parser_reset(void)
{
    g_mqtt_rx_collecting = 0U;
    g_mqtt_rx_collect_len = 0U;
    g_mqtt_rx_collect_tick = 0U;
}

uint8_t MQTT_Core_IsRxCollecting(void)
{
    if (!g_mqtt_rx_collecting) return 0U;
    if ((HAL_GetTick() - g_mqtt_rx_collect_tick) <= MQTT_CORE_RX_MULTILINE_TIMEOUT_MS) return 1U;
    mqtt_rx_parser_reset();
    return 0U;
}

static const char* skip_json_ws(const char* s)
{
    if (s == 0) return 0;
    while ((*s == ' ') || (*s == '\t') || (*s == '\n') || (*s == '\r')) s++;
    return s;
}

static uint8_t mqtt_find_json_end(const char* s, uint16_t len, uint16_t* end_len)
{
    uint16_t i;
    uint8_t started = 0U;
    uint8_t in_string = 0U;
    uint8_t escaped = 0U;
    int depth = 0;

    if ((s == 0) || (end_len == 0)) return 0U;

    for (i = 0U; i < len; i++)
    {
        char ch = s[i];

        if (!started)
        {
            if ((ch == ' ') || (ch == '\t') || (ch == '\n') || (ch == '\r')) continue;
            if ((ch != '{') && (ch != '[')) return 0U;
            started = 1U;
            depth = 1;
            continue;
        }

        if (in_string)
        {
            if (escaped)
                escaped = 0U;
            else if (ch == '\\')
                escaped = 1U;
            else if (ch == '"')
                in_string = 0U;
            continue;
        }

        if (ch == '"')
            in_string = 1U;
        else if ((ch == '{') || (ch == '['))
            depth++;
        else if ((ch == '}') || (ch == ']'))
        {
            if (depth > 0) depth--;
            if (depth == 0)
            {
                *end_len = (uint16_t)(i + 1U);
                return 1U;
            }
        }
    }

    return 0U;
}

static void mqtt_store_rx_message(const char* topic, uint16_t topic_len, const char* payload, uint16_t payload_len)
{
    if ((topic == 0) || (topic_len == 0U) || (payload == 0) || (payload_len == 0U)) return;
    if (topic_len >= MQTT_CORE_TOPIC_BUF_SZ) topic_len = MQTT_CORE_TOPIC_BUF_SZ - 1U;
    if (payload_len >= MQTT_CORE_PAYLOAD_BUF_SZ) payload_len = MQTT_CORE_PAYLOAD_BUF_SZ - 1U;

    if (g_mqtt_rx_msg_pending) { APP_LOGW("MQTT", "rx pending overwrite"); }

    __disable_irq();

    memcpy(g_mqtt_rx_pending_topic, topic, topic_len);
    g_mqtt_rx_pending_topic[topic_len] = '\0';

    memcpy(g_mqtt_rx_pending_payload, payload, payload_len);
    g_mqtt_rx_pending_payload[payload_len] = '\0';

    g_mqtt_rx_msg_pending = 1U;
    g_mqtt_last_mqtt_rx_tick = HAL_GetTick();

    __enable_irq();
}

static uint8_t mqtt_append_rx_payload(const char* data, uint16_t len)
{
    uint16_t copy_len;

    if ((data == 0) || (len == 0U)) return 1U;
    if (g_mqtt_rx_collect_len >= (MQTT_CORE_PAYLOAD_BUF_SZ - 1U)) return 0U;

    copy_len = (uint16_t)((MQTT_CORE_PAYLOAD_BUF_SZ - 1U) - g_mqtt_rx_collect_len);
    if (copy_len > len) copy_len = len;

    memcpy(&g_mqtt_rx_pending_payload[g_mqtt_rx_collect_len], data, copy_len);
    g_mqtt_rx_collect_len = (uint16_t)(g_mqtt_rx_collect_len + copy_len);
    g_mqtt_rx_pending_payload[g_mqtt_rx_collect_len] = '\0';
    g_mqtt_rx_collect_tick = HAL_GetTick();

    return (copy_len == len) ? 1U : 0U;
}

static void mqtt_try_finish_rx_collect(void)
{
    uint16_t end_len = 0U;

    if (!g_mqtt_rx_collecting) return;
    if (!mqtt_find_json_end(g_mqtt_rx_pending_payload, g_mqtt_rx_collect_len, &end_len)) return;
    if (end_len == 0U) return;

    g_mqtt_rx_pending_payload[end_len] = '\0';
    g_mqtt_rx_collect_len = end_len;

    if (g_mqtt_rx_msg_pending) { APP_LOGW("MQTT", "rx pending overwrite"); }

    __disable_irq();
    g_mqtt_rx_msg_pending = 1U;
    g_mqtt_last_mqtt_rx_tick = HAL_GetTick();
    __enable_irq();

    mqtt_rx_parser_reset();
}

static void mqtt_start_rx_collect(const char* topic, uint16_t topic_len, const char* payload, uint16_t payload_len)
{
    if ((topic == 0) || (topic_len == 0U)) return;
    if (topic_len >= MQTT_CORE_TOPIC_BUF_SZ) topic_len = MQTT_CORE_TOPIC_BUF_SZ - 1U;

    if (g_mqtt_rx_msg_pending) { APP_LOGW("MQTT", "rx pending overwrite"); }

    __disable_irq();
    memcpy(g_mqtt_rx_pending_topic, topic, topic_len);
    g_mqtt_rx_pending_topic[topic_len] = '\0';
    g_mqtt_rx_pending_payload[0] = '\0';
    g_mqtt_rx_msg_pending = 0U;
    __enable_irq();

    g_mqtt_rx_collecting = 1U;
    g_mqtt_rx_collect_len = 0U;
    g_mqtt_rx_collect_tick = HAL_GetTick();

    if (!mqtt_append_rx_payload(payload, payload_len)) mqtt_rx_parser_reset();
    mqtt_try_finish_rx_collect();
}

static void mqtt_handle_qmtrecv_continuation(const char* line)
{
    if (!MQTT_Core_IsRxCollecting()) return;
    if (line == 0) return;
    if (!mqtt_append_rx_payload(line, (uint16_t)strlen(line))) mqtt_rx_parser_reset();
    mqtt_try_finish_rx_collect();
}

static void mqtt_parse_qmtrecv_line(const char* line)
{
    const char* topic_start;
    const char* topic_end;
    const char* payload_start;
    const char* payload_end;
    const char* comma;
    const char* json_start;
    uint16_t topic_len;
    uint16_t payload_len;
    uint16_t json_end_len;

    if (line == 0) return;
    if (strstr(line, "+QMTRECV:") == 0) return;

    mqtt_rx_parser_reset();

    topic_start = strchr(line, '"');
    if (topic_start == 0) return;
    topic_start++;

    topic_end = strchr(topic_start, '"');
    if (topic_end == 0) return;

    topic_len = (uint16_t)(topic_end - topic_start);
    if (topic_len == 0U) return;
    if (topic_len >= MQTT_CORE_TOPIC_BUF_SZ) topic_len = MQTT_CORE_TOPIC_BUF_SZ - 1U;

    comma = strchr(topic_end, ',');
    if (comma == 0) return;

    payload_start = comma + 1;
    if (*payload_start == '"') payload_start++;
    json_start = skip_json_ws(payload_start);

    if ((json_start != 0) && ((*json_start == '{') || (*json_start == '[')))
    {
        payload_len = (uint16_t)strlen(json_start);
        if (mqtt_find_json_end(json_start, payload_len, &json_end_len))
        {
            mqtt_store_rx_message(topic_start, topic_len, json_start, json_end_len);
            return;
        }

        mqtt_start_rx_collect(topic_start, topic_len, json_start, payload_len);
        return;
    }

    payload_end = strrchr(payload_start, '"');
    if ((payload_end != 0) && (payload_end > payload_start))
    {
        payload_len = (uint16_t)(payload_end - payload_start);
        mqtt_store_rx_message(topic_start, topic_len, payload_start, payload_len);
        return;
    }

    mqtt_start_rx_collect(topic_start, topic_len, payload_start, (uint16_t)strlen(payload_start));
}

void mqtt_handle_line(const char* line)
{
    if (line == 0) return;
    if (line[0] == '\0') return;

    g_mqtt_rt.last_rx_tick = HAL_GetTick();
    TimeBase_UpdateFromCCLK(line);
#if LOCKCTRL_EC800_AT_DIAG_UART5
    mqtt_diag_log_line(line);
#else
    mqtt_dbg_printf("[MQTT] LINE: %s\r\n", line);
#endif

#if LOCKCTRL_EC800_AT_DIAG_UART5
    if ((strncmp(line, "+CPIN:", 6U) == 0) || (strncmp(line, "+CSQ:", 5U) == 0) ||
        (strncmp(line, "+CEREG:", 7U) == 0) || (strncmp(line, "+CGATT:", 7U) == 0) ||
        (strncmp(line, "+CGPADDR:", 9U) == 0) || (strncmp(line, "+QNWINFO:", 9U) == 0))
    {
        APP_LOGI("EC", "AT %s", line);
    }
#endif

    if (g_mqtt_rx_collecting && (strstr(line, "+QMTRECV:") == 0))
    {
        mqtt_handle_qmtrecv_continuation(line);
        return;
    }

    if (strncmp(line, "AT", 2) == 0) { return; }

    if (strstr(line, "RDY"))
    {
        mqtt_save_last_evt(line);

        if ((g_mqtt_rt.state == MQTT_CORE_ST_READY) || g_mqtt_rt.connected || g_mqtt_rt.subscribed ||
            g_mqtt_rt.pub_busy || g_mqtt_rt.pub_wait_prompt || g_mqtt_rt.pub_wait_result)
        {
            g_mqtt_rt.flag_closed = 1U;
            g_mqtt_rt.need_reconnect = 1U;
        }
        return;
    }

    if (strncmp(line, "OK", 2) == 0)
    {
        if ((g_mqtt_rt.state == MQTT_CORE_ST_ECHO_OFF) || (g_mqtt_rt.state == MQTT_CORE_ST_WAIT_ECHO_OFF) ||
            (g_mqtt_rt.state == MQTT_CORE_ST_CFG) || (g_mqtt_rt.state == MQTT_CORE_ST_WAIT_CFG) ||
            (g_mqtt_rt.state == MQTT_CORE_ST_DIAG) || (g_mqtt_rt.state == MQTT_CORE_ST_WAIT_DIAG) ||
            (g_mqtt_rt.state == MQTT_CORE_ST_WAIT_NETWORK_QUERY) ||
            (g_mqtt_rt.state == MQTT_CORE_ST_WAIT_ATTACH_QUERY) ||
            (g_mqtt_rt.state == MQTT_CORE_ST_WAIT_ATTACH) ||
            (g_mqtt_rt.state == MQTT_CORE_ST_WAIT_PDP_QUERY) ||
            (g_mqtt_rt.state == MQTT_CORE_ST_WAIT_PDP_CONFIG) ||
            (g_mqtt_rt.state == MQTT_CORE_ST_WAIT_PDP_ACTIVATE) ||
            (g_mqtt_rt.state == MQTT_CORE_ST_WAIT_PDP_VERIFY))
        {
            g_mqtt_rt.flag_cfg_ok = 1;
        }

        if (g_mqtt_iccid_query_pending) { g_mqtt_iccid_query_pending = 0U; }
        return;
    }

    if (strstr(line, "ERROR") || strstr(line, "+CME ERROR"))
    {
        mqtt_save_last_evt(line);
#if LOCKCTRL_EC800_AT_DIAG_UART5
        if (!g_mqtt_diag_command_active) g_mqtt_rt.flag_error = 1;
#else
        g_mqtt_rt.flag_error = 1;
#endif
        return;
    }

    if (strncmp(line, "+CEREG:", 7U) == 0)
    {
        unsigned first;
        unsigned second;
        unsigned status;
        int count;

        count = sscanf(line + 7U, " %u,%u", &first, &second);
        if (count >= 1)
        {
            status = (count >= 2) ? second : first;
            g_mqtt_rt.network_registered = ((status == 1U) || (status == 5U)) ? 1U : 0U;
#if LOCKCTRL_EC800_AT_DIAG_UART5
            APP_LOGI("EC", "registration status=%u ready=%u", status,
                     (unsigned)g_mqtt_rt.network_registered);
#endif
        }
        return;
    }

    if (strncmp(line, "+CGATT:", 7U) == 0)
    {
        unsigned attached;

        if (sscanf(line + 7U, " %u", &attached) == 1)
        {
            g_mqtt_rt.packet_attached = (attached == 1U) ? 1U : 0U;
#if LOCKCTRL_EC800_AT_DIAG_UART5
            APP_LOGI("EC", "packet attached=%u", (unsigned)g_mqtt_rt.packet_attached);
#endif
        }
        return;
    }

    if (strstr(line, "+QMTPUBEX:"))
    {
        uint8_t client_idx;
        uint16_t msg_id;
        int result;

        mqtt_save_last_evt(line);

        if (!mqtt_parse_publish_result(line, &client_idx, &msg_id, &result))
        {
            APP_LOGW("MQTT", "publish result parse failed");
            return;
        }

        if (g_mqtt_rt.pub_busy &&
            ((client_idx != g_mqtt_cfg.client_idx) || (msg_id != g_mqtt_pub_pending_msg_id)))
        {
            APP_LOGW("MQTT", "publish result mismatch client=%u msg=%u expect=%u", (unsigned)client_idx,
                     (unsigned)msg_id, (unsigned)g_mqtt_pub_pending_msg_id);
            return;
        }

        if (g_mqtt_rt.pub_busy)
        {
            if (result == 0)
            {
                g_mqtt_rt.pub_busy = 0;
                g_mqtt_rt.pub_wait_prompt = 0;
                g_mqtt_rt.pub_wait_result = 0;
                g_mqtt_pub_pending_msg_id = 0U;
                g_mqtt_last_pub_ok_tick = HAL_GetTick();
                g_mqtt_pub_ok_seq++;
                g_mqtt_pub_timeout_count = 0U;
            }
            else
            {
                APP_LOGW("MQTT", "publish result fail topic=%s", g_mqtt_pub_pending_topic);
                g_mqtt_rt.pub_busy = 0;
                g_mqtt_rt.pub_wait_prompt = 0;
                g_mqtt_rt.pub_wait_result = 0;
                g_mqtt_pub_pending_msg_id = 0U;
                g_mqtt_rt.flag_error = 1;
            }
        }
        return;
    }

    if (strstr(line, "+QMTOPEN:"))
    {
        unsigned client_idx;
        int result;

        mqtt_save_last_evt(line);

        if (sscanf(strstr(line, "+QMTOPEN:") + strlen("+QMTOPEN:"), " %u,%d", &client_idx, &result) == 2)
        {
#if LOCKCTRL_EC800_AT_DIAG_UART5
            APP_LOGI("MQTT", "QMTOPEN client=%u result=%d", client_idx, result);
#endif
            if (result == 0) { g_mqtt_rt.flag_open_ok = 1; }
            else
            {
                APP_LOGW("MQTT", "QMTOPEN client=%u failed result=%d", client_idx, result);
                g_mqtt_rt.flag_error = 1;
            }
        }
        else
        {
            APP_LOGW("MQTT", "QMTOPEN parse failed: %s", line);
            g_mqtt_rt.flag_error = 1;
        }
        return;
    }

    if (strstr(line, "+QMTCONN:"))
    {
        mqtt_save_last_evt(line);

#if LOCKCTRL_EC800_AT_DIAG_UART5
        {
            unsigned client_idx;
            int result;
            int ret_code;

            if (sscanf(strstr(line, "+QMTCONN:") + strlen("+QMTCONN:"), " %u,%d,%d", &client_idx, &result,
                       &ret_code) == 3)
            {
                APP_LOGI("MQTT", "QMTCONN client=%u result=%d ret=%d", client_idx, result, ret_code);
            }
        }
#endif

        if (strstr(line, ",0,0")) { g_mqtt_rt.flag_conn_ok = 1; }
        else
        {
            g_mqtt_rt.flag_error = 1;
        }
        return;
    }

    if (strstr(line, "+QMTSUB:"))
    {
        mqtt_save_last_evt(line);

        if (strstr(line, ",1,0") || strstr(line, ",1,1") || strstr(line, ",0,0")) { g_mqtt_rt.flag_sub_ok = 1; }
        else
        {
            g_mqtt_rt.flag_error = 1;
        }
        return;
    }

    if (strstr(line, "+QMTSTAT:") || strstr(line, "+QMTCLOSE:"))
    {
        mqtt_save_last_evt(line);
        g_mqtt_rt.flag_closed = 1;
        return;
    }

    if (strncmp(line, "+QIACT:", 7U) == 0)
    {
        unsigned context_id;
        unsigned context_state;

        if (sscanf(line + 7U, " %u,%u", &context_id, &context_state) >= 2 && context_id == 1U)
        {
            g_mqtt_rt.pdp_active = (context_state == 1U) ? 1U : 0U;
#if LOCKCTRL_EC800_AT_DIAG_UART5
            APP_LOGI("EC", "PDP ctx=%u state=%u active=%u", context_id, context_state,
                     (unsigned)g_mqtt_rt.pdp_active);
#endif
        }
        return;
    }

    if (strstr(line, "+QCCID:"))
    {
        char iccid_buf[32];

        mqtt_save_last_evt(line);

        if (mqtt_parse_qccid_line(line, iccid_buf, sizeof(iccid_buf)))
        {
            strncpy(g_mqtt_iccid, iccid_buf, sizeof(g_mqtt_iccid) - 1U);
            g_mqtt_iccid[sizeof(g_mqtt_iccid) - 1U] = '\0';
            g_mqtt_iccid_valid = 1U;
            g_mqtt_iccid_query_pending = 0U;
#if LOCKCTRL_EC800_AT_DIAG_UART5
            APP_LOGI("EC", "ICCID=%s", g_mqtt_iccid);
#endif
        }
#if LOCKCTRL_EC800_AT_DIAG_UART5
        else
        {
            APP_LOGW("EC", "ICCID unavailable");
        }
#endif
        return;
    }

    if (strstr(line, "+QMTRECV:"))
    {
        mqtt_save_last_evt(line);
        mqtt_parse_qmtrecv_line(line);
        return;
    }

    if ((line[0] == '{') || (line[0] == '[')) { return; }
}

void MQTT_Core_OnUartRx(uint8_t* data, uint16_t len)
{
    uint16_t i;
    char ch;

    if ((data == 0) || (len == 0)) return;

    for (i = 0; i < len; i++)
    {
        ch = (char)data[i];

        if (ch == '>')
        {
            g_mqtt_pub_prompt_pending = 1U;
            continue;
        }

        if (g_mqtt_rx_drop_until_lf)
        {
            if (ch == '\n')
            {
                g_mqtt_rx_drop_until_lf = 0;
                g_mqtt_line_len = 0;
            }
            continue;
        }

        if (ch == '\r') { continue; }
        else if (ch == '\n')
        {
            if (g_mqtt_line_len > 0)
            {
                g_mqtt_line_buf[g_mqtt_line_len] = '\0';
                mqtt_handle_line(g_mqtt_line_buf);
                g_mqtt_line_len = 0;
            }
        }
        else
        {
            if (g_mqtt_line_len < (MQTT_CORE_RX_LINE_BUF_SZ - 1U)) { g_mqtt_line_buf[g_mqtt_line_len++] = ch; }
            else
            {
                g_mqtt_rx_drop_until_lf = 1;
                g_mqtt_line_len = 0;
            }
        }
    }
}

void MQTT_Core_OnAtEvent(Ec800AtEvent_t event, const uint8_t* data, uint16_t len, void* user)
{
    (void)user;
    if ((data == 0) || (len == 0U)) return;

    if (event == EC800_AT_EVENT_LINE)
    {
        if ((len == 1U) && (data[0] == '>'))
        {
            g_mqtt_pub_prompt_pending = 1U;
            return;
        }
        if (len >= MQTT_CORE_RX_LINE_BUF_SZ) len = MQTT_CORE_RX_LINE_BUF_SZ - 1U;
        memcpy(g_mqtt_line_buf, data, len);
        g_mqtt_line_buf[len] = '\0';
        mqtt_handle_line(g_mqtt_line_buf);
    }
}

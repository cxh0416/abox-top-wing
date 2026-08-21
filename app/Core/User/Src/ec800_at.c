#include "ec800_at.h"

#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "app_diag.h"
#include "mqtt_core.h"

#define EC800_AT_ABORT_GUARD_MS 1500U

static uint8_t cmd_expects_payload_prompt(const char* cmd)
{
    if (cmd == 0) return 0U;
    if (strncmp(cmd, "AT+QMTPUBEX=", 12U) == 0) return 1U;
    if (strncmp(cmd, "AT+QISEND=", 10U) == 0) return 1U;
    if (strncmp(cmd, "AT+QHTTPURL=", 12U) == 0) return 1U;
    if ((strncmp(cmd, "AT+QFWRITE=", 11U) == 0) && (cmd[11] != '?')) return 1U;
    if (strncmp(cmd, "AT+QFUPL=\"", 10U) == 0) return 1U;
    return 0U;
}

#if LOCKCTRL_EC800_AT_TRACE
static void trace_command_name(const char* cmd, char* out, uint16_t out_sz)
{
    const char* p;
    uint16_t n = 0U;

    if ((out == 0) || (out_sz == 0U)) return;
    out[0] = '\0';
    if (cmd == 0) return;

    p = (strncmp(cmd, "AT", 2U) == 0) ? (cmd + 2U) : cmd;
    while ((p[n] != '\0') && (p[n] != '=') && (p[n] != '?') && (p[n] != '\r') && (p[n] != '\n') &&
           (n < (out_sz - 1U)))
    {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
}
#endif

static uint8_t line_has_prefix(const uint8_t* line, const char* prefix)
{
    uint16_t prefix_len;

    if ((line == 0) || (prefix == 0)) return 0U;
    prefix_len = (uint16_t)strlen(prefix);
    return (strncmp((const char*)line, prefix, prefix_len) == 0) ? 1U : 0U;
}

static uint8_t line_is_terminal(const uint8_t* line, Ec800AtCmdResult_t* result)
{
    if ((line == 0) || (result == 0)) return 0U;
    if (strcmp((const char*)line, "OK") == 0)
    {
        *result = EC800_AT_CMD_RESULT_OK;
        return 1U;
    }
    if (strcmp((const char*)line, "ERROR") == 0)
    {
        *result = EC800_AT_CMD_RESULT_ERROR;
        return 1U;
    }
    if (strncmp((const char*)line, "+CME ERROR", 10U) == 0)
    {
        *result = EC800_AT_CMD_RESULT_ERROR;
        return 1U;
    }
    if (strcmp((const char*)line, "SEND OK") == 0)
    {
        *result = EC800_AT_CMD_RESULT_SEND_OK;
        return 1U;
    }
    if (strcmp((const char*)line, "SEND FAIL") == 0)
    {
        *result = EC800_AT_CMD_RESULT_SEND_FAIL;
        return 1U;
    }
    return 0U;
}

static uint8_t command_waits_for_result_urc(const char* command)
{
    if (command == 0) return 0U;
    if ((strncmp(command, "AT+QHTTPGETEX=", 14U) == 0) && (command[14] != '?')) return 1U;
    if ((strncmp(command, "AT+QHTTPREAD=", 13U) == 0) && (command[13] != '?')) return 1U;
    if (strncmp(command, "AT+QMTDISC=", 11U) == 0 || strncmp(command, "AT+QMTCLOSE=", 12U) == 0) return 1U;
    return 0U;
}

static uint8_t line_is_result_urc(const Ec800AtCore_t* core, const uint8_t* line, Ec800AtCmdResult_t* result)
{
    unsigned long a, b;
    if (!core || !line || !result || !core->active_valid) return 0U;
    if (strncmp(core->active.cmd, "AT+QHTTPGETEX=", 14U) == 0 && sscanf((const char*)line, "+QHTTPGET: %lu", &a) == 1) { *result = a == 0U ? EC800_AT_CMD_RESULT_OK : EC800_AT_CMD_RESULT_ERROR; return 1U; }
    if (strncmp(core->active.cmd, "AT+QHTTPREAD=", 13U) == 0 && sscanf((const char*)line, "+QHTTPREAD: %lu", &a) == 1) { *result = a == 0U ? EC800_AT_CMD_RESULT_OK : EC800_AT_CMD_RESULT_ERROR; return 1U; }
    if (strncmp(core->active.cmd, "AT+QMTDISC=", 11U) == 0 && sscanf((const char*)line, "+QMTDISC: %lu,%lu", &a, &b) == 2) { *result = b == 0U ? EC800_AT_CMD_RESULT_OK : EC800_AT_CMD_RESULT_ERROR; return 1U; }
    if (strncmp(core->active.cmd, "AT+QMTCLOSE=", 12U) == 0 && sscanf((const char*)line, "+QMTCLOSE: %lu,%lu", &a, &b) == 2) { *result = b == 0U ? EC800_AT_CMD_RESULT_OK : EC800_AT_CMD_RESULT_ERROR; return 1U; }
    return 0U;
}

static void start_abort_guard(Ec800AtCore_t* core)
{
    if (core == 0) return;
    core->abort_guard_tick = HAL_GetTick();
}

static uint8_t abort_guard_active(Ec800AtCore_t* core)
{
    if ((core == 0) || (core->abort_guard_tick == 0U)) return 0U;
    if ((HAL_GetTick() - core->abort_guard_tick) < EC800_AT_ABORT_GUARD_MS) return 1U;
    core->abort_guard_tick = 0U;
    core->raw_owner = EC800_AT_OWNER_NONE;
    core->raw_remaining = 0U;
    return 0U;
}

static void complete_active(Ec800AtCore_t* core, Ec800AtCmdResult_t result)
{
    Ec800AtCmdCallback_t cb;
    void* user;
    Ec800AtOwner_t owner;

    if ((core == 0) || (!core->active_valid)) return;
    owner = core->active.owner;
    cb = core->active.cb;
    user = core->active.user;
    core->active_valid = 0U;
    core->active_payload_cmd = 0U;
    core->active_wait_prompt = 0U;
    core->active_wait_payload_result = 0U;
    core->active_payload_sent = 0U;
    if (core->raw_owner == owner) { core->raw_owner = EC800_AT_OWNER_NONE; core->raw_remaining = 0U; }
    memset(&core->active, 0, sizeof(core->active));
    if (cb) cb(result, user);
}

static void dispatch_to_owner(Ec800AtCore_t* core, Ec800AtOwner_t owner, Ec800AtEvent_t event, const uint8_t* data,
                              uint16_t len)
{
    if ((core == 0) || (data == 0) || (len == 0U)) return;
    if ((owner == EC800_AT_OWNER_MQTT) && core->mqtt_handler)
        core->mqtt_handler(event, data, len, core->mqtt_user);
    else if ((owner == EC800_AT_OWNER_OTA) && core->ota_handler)
        core->ota_handler(event, data, len, core->ota_user);
}

static void dispatch_line(Ec800AtCore_t* core, const uint8_t* data, uint16_t len)
{
    if ((core == 0) || (data == 0) || (len == 0U)) return;

    if (line_has_prefix(data, "+QMT"))
    {
        dispatch_to_owner(core, EC800_AT_OWNER_MQTT, EC800_AT_EVENT_LINE, data, len);
        return;
    }
    if (MQTT_Core_IsRxCollecting())
    {
        dispatch_to_owner(core, EC800_AT_OWNER_MQTT, EC800_AT_EVENT_LINE, data, len);
        return;
    }
    if (core->active_valid)
    {
        dispatch_to_owner(core, core->active.owner, EC800_AT_EVENT_LINE, data, len);
    }
    else if (line_has_prefix(data, "+CCLK") || line_has_prefix(data, "+QCCID") || line_has_prefix(data, "+CPIN") ||
             line_has_prefix(data, "+CSQ") || line_has_prefix(data, "+CEREG") || line_has_prefix(data, "+CGATT") ||
             line_has_prefix(data, "+QIACT") || line_has_prefix(data, "+CGPADDR") || line_has_prefix(data, "+QNWINFO") ||
             line_has_prefix(data, "RDY") || line_has_prefix(data, "OK") || line_has_prefix(data, "ERROR") ||
             line_has_prefix(data, "+CME"))
    {
        dispatch_to_owner(core, EC800_AT_OWNER_MQTT, EC800_AT_EVENT_LINE, data, len);
    }
}

static void emit_line(Ec800AtCore_t* core)
{
    Ec800AtCmdResult_t result;
    uint8_t is_prompt;
    uint8_t* line;
    uint16_t line_len;
#if LOCKCTRL_EC800_AT_TRACE
    Ec800AtOwner_t trace_owner;
#endif

    if ((core == 0) || (core->line_len == 0U)) return;
    core->line_buf[core->line_len] = '\0';
    line = core->line_buf;
    line_len = core->line_len;
    while ((line_len > 0U) && ((*line == ' ') || (*line == '\t')))
    {
        line++;
        line_len--;
    }
    if (line_len == 0U)
    {
        core->line_len = 0U;
        return;
    }
    is_prompt = (((line_len == 1U) && (line[0] == '>')) ||
                 ((line_len == 7U) && (memcmp(line, "CONNECT", 7U) == 0))) ? 1U : 0U;

#if LOCKCTRL_EC800_AT_TRACE
    trace_owner = core->active_valid ? core->active.owner : EC800_AT_OWNER_NONE;
    if (line_has_prefix(line, "ERROR"))
    {
        APP_LOGI("AT", "RX owner=%u %s", (unsigned int)trace_owner, line);
    }
#endif

    if (core->active_valid && is_prompt && !core->active_payload_sent)
    {
        core->active_wait_prompt = 0U;
        core->active_wait_payload_result = 1U;
        core->active_payload_sent = 0U;
        core->active_tick = HAL_GetTick();
    }
    dispatch_line(core, line, line_len);

    if (core->active_valid)
    {
        if (is_prompt && !core->active_payload_sent)
        {
            core->active_wait_prompt = 0U;
            core->active_wait_payload_result = 1U;
            core->active_payload_sent = 0U;
            core->active_tick = HAL_GetTick();
        }
        else if (core->active_wait_payload_result && (core->active.owner == EC800_AT_OWNER_MQTT) &&
                 line_has_prefix(line, "+QMTPUBEX:"))
        {
            complete_active(core, EC800_AT_CMD_RESULT_OK);
        }
        else if (line_is_result_urc(core, line, &result))
        {
            complete_active(core, result);
        }
        else if (line_is_terminal(line, &result))
        {
            if (command_waits_for_result_urc(core->active.cmd) && (result == EC800_AT_CMD_RESULT_OK))
            {
            }
            else if ((result == EC800_AT_CMD_RESULT_SEND_OK) || (result == EC800_AT_CMD_RESULT_SEND_FAIL))
            {
                complete_active(core, result);
            }
            else if (!core->active_payload_cmd)
            {
                if ((result == EC800_AT_CMD_RESULT_OK) || (result == EC800_AT_CMD_RESULT_ERROR))
                {
                    complete_active(core, result);
                }
            }
            else if ((result == EC800_AT_CMD_RESULT_SEND_OK) || (result == EC800_AT_CMD_RESULT_SEND_FAIL) ||
                     (result == EC800_AT_CMD_RESULT_ERROR) ||
                     ((result == EC800_AT_CMD_RESULT_OK) && core->active_payload_sent))
            {
                complete_active(core, result);
            }
        }
    }
    core->line_len = 0U;
}

static void push_char(Ec800AtCore_t* core, uint8_t ch)
{
    if (core == 0) return;
    if (ch == '\r') return;
    if ((ch == '>') && core->active_valid && core->active_payload_cmd && !core->active_payload_sent)
    {
        if (core->line_len != 0U) emit_line(core);
        core->line_buf[core->line_len++] = ch;
        emit_line(core);
        return;
    }
    if (ch == '\n')
    {
        emit_line(core);
        return;
    }
    if ((core->line_len == 0U) && ((ch == ' ') || (ch == '\t'))) return;
    if (core->line_len < (EC800_AT_LINE_BUF_SIZE - 1U))
        core->line_buf[core->line_len++] = ch;
    else
        core->line_len = 0U;
}

static uint8_t queue_empty(Ec800AtCore_t* core)
{
    uint8_t i;

    if (core == 0) return 1U;
    for (i = 0U; i < EC800_AT_QUEUE_SIZE; i++)
        if (core->queue[i].used) return 0U;
    return 1U;
}

static int next_cmd(Ec800AtCore_t* core)
{
    int best = -1;
    uint8_t i;

    if (core == 0) return -1;
    for (i = 0U; i < EC800_AT_QUEUE_SIZE; i++)
    {
        if (!core->queue[i].used) continue;
        if ((best < 0) || (core->queue[i].priority > core->queue[best].priority)) best = (int)i;
    }
    return best;
}

static void start_next(Ec800AtCore_t* core)
{
    int idx;
    uint16_t len;

    if ((core == 0) || core->active_valid || queue_empty(core)) return;
    idx = next_cmd(core);
    if (idx < 0) return;

    core->active = core->queue[idx];
    memset(&core->queue[idx], 0, sizeof(core->queue[idx]));
    core->active_valid = 1U;
    core->active_payload_cmd = cmd_expects_payload_prompt(core->active.cmd);
    core->active_wait_prompt = core->active_payload_cmd;
    core->active_wait_payload_result = 0U;
    core->active_payload_sent = 0U;
    core->active_tick = HAL_GetTick();
    len = (uint16_t)strlen(core->active.cmd);
    if ((len > 0U) && core->uart)
    {
#if LOCKCTRL_EC800_AT_TRACE
        char command_name[32];
        trace_command_name(core->active.cmd, command_name, sizeof(command_name));
        APP_LOGI("AT", "TX owner=%u cmd=%s", (unsigned int)core->active.owner, command_name);
#endif
        (void)HAL_UART_Transmit(core->uart, (uint8_t*)core->active.cmd, len, 300);
    }
}

void EC800_AT_Init(Ec800AtCore_t* core, UART_HandleTypeDef* uart)
{
    if (core == 0) return;
    memset(core, 0, sizeof(*core));
    core->uart = uart;
}

void EC800_AT_Reset(Ec800AtCore_t* core)
{
    if (core == 0) return;
    core->line_len = 0U;
    core->active_valid = 0U;
    core->active_payload_cmd = 0U;
    core->active_wait_prompt = 0U;
    core->active_wait_payload_result = 0U;
    core->active_payload_sent = 0U;
    core->abort_guard_tick = 0U;
    core->raw_owner = EC800_AT_OWNER_NONE;
    core->raw_remaining = 0U;
    memset(core->queue, 0, sizeof(core->queue));
    memset(&core->active, 0, sizeof(core->active));
}

void EC800_AT_FeedRx(Ec800AtCore_t* core, const uint8_t* data, uint16_t len)
{
    uint16_t i;

    if ((core == 0) || (data == 0) || (len == 0U)) return;
    for (i = 0U; i < len; i++)
    {
        if (core->raw_remaining != 0U)
        {
            dispatch_to_owner(core, core->raw_owner, EC800_AT_EVENT_RAW, &data[i], 1U);
            core->raw_remaining--;
            if (core->raw_remaining == 0U) core->raw_owner = EC800_AT_OWNER_NONE;
        }
        else push_char(core, data[i]);
    }
}

void EC800_AT_Task(Ec800AtCore_t* core)
{
    if (core == 0) return;
    if (core->active_valid && ((HAL_GetTick() - core->active_tick) >= core->active.timeout_ms))
    {
#if LOCKCTRL_EC800_AT_TRACE
        {
            char command_name[32];
            trace_command_name(core->active.cmd, command_name, sizeof(command_name));
            APP_LOGW("AT", "TIMEOUT owner=%u cmd=%s", (unsigned int)core->active.owner, command_name);
        }
#endif
        if (core->active_payload_cmd && !core->active_payload_sent && core->uart)
        {
            const uint8_t esc = 0x1BU;
            (void)HAL_UART_Transmit(core->uart, (uint8_t*)&esc, 1U, 100U);
            start_abort_guard(core);
        }
        complete_active(core, EC800_AT_CMD_RESULT_TIMEOUT);
    }
    if (abort_guard_active(core)) return;
    start_next(core);
}

uint8_t EC800_AT_Submit(Ec800AtCore_t* core, const char* cmd, Ec800AtOwner_t owner, Ec800AtPriority_t priority,
                        uint32_t timeout_ms, Ec800AtCmdCallback_t cb, void* user)
{
    uint8_t i;
    uint16_t n;

    if ((core == 0) || (cmd == 0) || (cmd[0] == '\0')) return 0U;
    for (i = 0U; i < EC800_AT_QUEUE_SIZE; i++)
    {
        if (core->queue[i].used) continue;
        memset(&core->queue[i], 0, sizeof(core->queue[i]));
        n = (uint16_t)strlen(cmd);
        if (n > (EC800_AT_CMD_BUF_SIZE - 3U)) n = EC800_AT_CMD_BUF_SIZE - 3U;
        memcpy(core->queue[i].cmd, cmd, n);
        if ((n < 2U) || (core->queue[i].cmd[n - 2U] != '\r') || (core->queue[i].cmd[n - 1U] != '\n'))
        {
            core->queue[i].cmd[n++] = '\r';
            core->queue[i].cmd[n++] = '\n';
        }
        core->queue[i].cmd[n] = '\0';
        core->queue[i].owner = owner;
        core->queue[i].priority = priority;
        core->queue[i].timeout_ms = timeout_ms ? timeout_ms : 5000U;
        core->queue[i].cb = cb;
        core->queue[i].user = user;
        core->queue[i].used = 1U;
        return 1U;
    }
    return 0U;
}

uint8_t EC800_AT_SendPayload(Ec800AtCore_t* core, const uint8_t* data, uint16_t len)
{
    if ((core == 0) || (core->uart == 0) || (data == 0) || (len == 0U)) return 0U;
    return (HAL_UART_Transmit(core->uart, (uint8_t*)data, len, 1000) == HAL_OK) ? 1U : 0U;
}

uint8_t EC800_AT_SendPayloadForOwner(Ec800AtCore_t* core, Ec800AtOwner_t owner, const uint8_t* data, uint16_t len)
{
    if ((core == 0) || (owner == EC800_AT_OWNER_NONE)) return 0U;
    if ((!core->active_valid) || (core->active.owner != owner) || (!core->active_wait_payload_result)) return 0U;
    if (!EC800_AT_SendPayload(core, data, len)) return 0U;
#if LOCKCTRL_EC800_AT_TRACE
    APP_LOGI("AT", "PAY owner=%u len=%u", (unsigned int)owner, len);
#endif
    core->active_wait_prompt = 0U;
    core->active_payload_sent = 1U;
    core->active_tick = HAL_GetTick();
    return 1U;
}

uint8_t EC800_AT_ForceSendPayloadForOwner(Ec800AtCore_t* core, Ec800AtOwner_t owner, const uint8_t* data,
                                          uint16_t len)
{
    if ((core == 0) || (owner == EC800_AT_OWNER_NONE)) return 0U;
    if ((!core->active_valid) || (core->active.owner != owner) || (!core->active_payload_cmd) ||
        core->active_payload_sent)
        return 0U;

    core->active_wait_prompt = 0U;
    core->active_wait_payload_result = 1U;
    return EC800_AT_SendPayloadForOwner(core, owner, data, len);
}

void EC800_AT_RegisterUrcHandler(Ec800AtCore_t* core, Ec800AtOwner_t owner, Ec800AtUrcHandler_t handler, void* user)
{
    if (core == 0) return;
    if (owner == EC800_AT_OWNER_MQTT)
    {
        core->mqtt_handler = handler;
        core->mqtt_user = user;
    }
    else if (owner == EC800_AT_OWNER_OTA)
    {
        core->ota_handler = handler;
        core->ota_user = user;
    }
}

uint8_t EC800_AT_BeginRawRead(Ec800AtCore_t* core, Ec800AtOwner_t owner, uint32_t length)
{
    if (!core || owner == EC800_AT_OWNER_NONE || length == 0U || core->raw_remaining != 0U) return 0U;
    core->raw_owner = owner;
    core->raw_remaining = length;
    return 1U;
}

uint8_t EC800_AT_HasPending(Ec800AtCore_t* core, Ec800AtOwner_t owner)
{
    uint8_t i;

    if (core == 0) return 0U;
    if (core->active_valid && (core->active.owner == owner)) return 1U;
    for (i = 0U; i < EC800_AT_QUEUE_SIZE; i++)
        if (core->queue[i].used && (core->queue[i].owner == owner)) return 1U;
    return 0U;
}

uint8_t EC800_AT_IsBusy(Ec800AtCore_t* core)
{
    if (core == 0) return 0U;
    return (core->active_valid || (!queue_empty(core))) ? 1U : 0U;
}

uint8_t EC800_AT_IsActiveOwner(Ec800AtCore_t* core, Ec800AtOwner_t owner)
{
    if ((core == 0) || (owner == EC800_AT_OWNER_NONE)) return 0U;
    return (core->active_valid && (core->active.owner == owner)) ? 1U : 0U;
}

uint8_t EC800_AT_IsPayloadBusy(Ec800AtCore_t* core)
{
    if (core == 0) return 0U;
    return (core->active_valid && core->active_payload_cmd) ? 1U : 0U;
}

void EC800_AT_CancelQueuedOwner(Ec800AtCore_t* core, Ec800AtOwner_t owner)
{
    uint8_t i;

    if ((core == 0) || (owner == EC800_AT_OWNER_NONE)) return;

    for (i = 0U; i < EC800_AT_QUEUE_SIZE; i++)
    {
        if (core->queue[i].used && (core->queue[i].owner == owner))
        {
            memset(&core->queue[i], 0, sizeof(core->queue[i]));
        }
    }
}

void EC800_AT_CancelOwner(Ec800AtCore_t* core, Ec800AtOwner_t owner)
{
    if ((core == 0) || (owner == EC800_AT_OWNER_NONE)) return;

    if (core->active_valid && (core->active.owner == owner))
    {
        if (core->active_payload_cmd && !core->active_payload_sent && core->uart)
        {
            const uint8_t esc = 0x1BU;
            (void)HAL_UART_Transmit(core->uart, (uint8_t*)&esc, 1U, 100U);
            start_abort_guard(core);
        }
        core->active_valid = 0U;
        core->active_payload_cmd = 0U;
        core->active_wait_prompt = 0U;
        core->active_wait_payload_result = 0U;
        core->active_payload_sent = 0U;
        memset(&core->active, 0, sizeof(core->active));
    }
    if (core->raw_owner == owner) { core->raw_owner = EC800_AT_OWNER_NONE; core->raw_remaining = 0U; }

    EC800_AT_CancelQueuedOwner(core, owner);
}

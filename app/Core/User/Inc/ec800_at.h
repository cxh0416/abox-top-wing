#ifndef EC800_AT_H
#define EC800_AT_H

#include <stdint.h>

#include "main.h"

#define EC800_AT_LINE_BUF_SIZE 704U
#define EC800_AT_CMD_BUF_SIZE 256U
#define EC800_AT_QUEUE_SIZE 10U
#define EC800_AT_RAW_CHUNK_MAX 1024U

typedef enum
{
    EC800_AT_OWNER_NONE = 0,
    EC800_AT_OWNER_MQTT = 1,
    EC800_AT_OWNER_OTA = 2
} Ec800AtOwner_t;

typedef enum
{
    EC800_AT_PRIORITY_NORMAL = 0,
    EC800_AT_PRIORITY_HIGH = 1
} Ec800AtPriority_t;

typedef enum
{
    EC800_AT_EVENT_LINE = 0,
    EC800_AT_EVENT_RAW
} Ec800AtEvent_t;

typedef enum
{
    EC800_AT_CMD_RESULT_OK = 0,
    EC800_AT_CMD_RESULT_ERROR,
    EC800_AT_CMD_RESULT_TIMEOUT,
    EC800_AT_CMD_RESULT_SEND_OK,
    EC800_AT_CMD_RESULT_SEND_FAIL
} Ec800AtCmdResult_t;

typedef void (*Ec800AtUrcHandler_t)(Ec800AtEvent_t event, const uint8_t* data, uint16_t len, void* user);
typedef void (*Ec800AtCmdCallback_t)(Ec800AtCmdResult_t result, void* user);

typedef struct
{
    char cmd[EC800_AT_CMD_BUF_SIZE];
    Ec800AtOwner_t owner;
    Ec800AtPriority_t priority;
    uint32_t timeout_ms;
    Ec800AtCmdCallback_t cb;
    void* user;
    uint8_t used;
} Ec800AtCommand_t;

typedef struct
{
    UART_HandleTypeDef* uart;
    uint8_t line_buf[EC800_AT_LINE_BUF_SIZE];
    uint16_t line_len;
    Ec800AtCommand_t queue[EC800_AT_QUEUE_SIZE];
    Ec800AtCommand_t active;
    uint8_t active_valid;
    uint8_t active_payload_cmd;
    uint8_t active_wait_prompt;
    uint8_t active_wait_payload_result;
    uint8_t active_payload_sent;
    uint32_t active_tick;
    uint32_t abort_guard_tick;
    Ec800AtUrcHandler_t mqtt_handler;
    void* mqtt_user;
    Ec800AtUrcHandler_t ota_handler;
    void* ota_user;
    Ec800AtOwner_t raw_owner;
    uint32_t raw_remaining;
} Ec800AtCore_t;

void EC800_AT_Init(Ec800AtCore_t* core, UART_HandleTypeDef* uart);
void EC800_AT_Reset(Ec800AtCore_t* core);
void EC800_AT_FeedRx(Ec800AtCore_t* core, const uint8_t* data, uint16_t len);
void EC800_AT_Task(Ec800AtCore_t* core);
uint8_t EC800_AT_Submit(Ec800AtCore_t* core, const char* cmd, Ec800AtOwner_t owner, Ec800AtPriority_t priority,
                        uint32_t timeout_ms, Ec800AtCmdCallback_t cb, void* user);
uint8_t EC800_AT_SendPayload(Ec800AtCore_t* core, const uint8_t* data, uint16_t len);
uint8_t EC800_AT_SendPayloadForOwner(Ec800AtCore_t* core, Ec800AtOwner_t owner, const uint8_t* data, uint16_t len);
uint8_t EC800_AT_ForceSendPayloadForOwner(Ec800AtCore_t* core, Ec800AtOwner_t owner, const uint8_t* data,
                                          uint16_t len);
void EC800_AT_RegisterUrcHandler(Ec800AtCore_t* core, Ec800AtOwner_t owner, Ec800AtUrcHandler_t handler, void* user);
uint8_t EC800_AT_HasPending(Ec800AtCore_t* core, Ec800AtOwner_t owner);
uint8_t EC800_AT_IsBusy(Ec800AtCore_t* core);
uint8_t EC800_AT_IsActiveOwner(Ec800AtCore_t* core, Ec800AtOwner_t owner);
uint8_t EC800_AT_IsPayloadBusy(Ec800AtCore_t* core);
void EC800_AT_CancelQueuedOwner(Ec800AtCore_t* core, Ec800AtOwner_t owner);
void EC800_AT_CancelOwner(Ec800AtCore_t* core, Ec800AtOwner_t owner);
uint8_t EC800_AT_BeginRawRead(Ec800AtCore_t* core, Ec800AtOwner_t owner, uint32_t length);

#endif

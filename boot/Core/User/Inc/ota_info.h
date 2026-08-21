#ifndef __OTA_INFO_H__
#define __OTA_INFO_H__

#include <stdint.h>

#include "boot_cfg.h"

#define BOARD_CONFIG_MAGIC 0x42434647U
#define BOARD_CONFIG_VERSION 2U
#define BOARD_CONFIG_BOARD_MAX 32U
#define BOARD_CONFIG_CHANNEL_MAX 24U
#define BOARD_CONFIG_MAX_LOCKERS ((BOARD_CONFIG_BOARD_MAX * BOARD_CONFIG_CHANNEL_MAX) / 2U)

typedef struct
{
    uint8_t board_addr;
    uint8_t channel;
} BoardConfigLockEndpoint_t;

typedef struct
{
    union
    {
        BoardConfigLockEndpoint_t lock1;
        struct { uint8_t board_addr; uint8_t channel; };
    };
    BoardConfigLockEndpoint_t lock2;
} BoardConfigLockerMap_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t checksum;
    uint32_t lock_baud;
    uint16_t map_count;
    uint16_t flags;
    char vid[32];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_username[32];
    char mqtt_password[32];
    uint8_t diag_log_enabled;
    uint8_t diag_log_level;
    uint16_t diag_log_port;
    char diag_log_host[64];
    BoardConfigLockEndpoint_t map[BOARD_CONFIG_BOARD_MAX * BOARD_CONFIG_CHANNEL_MAX];
} BoardConfigStoreV1_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t checksum;
    uint32_t lock_baud;
    uint16_t map_count;
    uint16_t flags;
    char vid[32];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_username[32];
    char mqtt_password[32];
    uint8_t diag_log_enabled;
    uint8_t diag_log_level;
    uint16_t diag_log_port;
    char diag_log_host[64];
    BoardConfigLockerMap_t map[BOARD_CONFIG_MAX_LOCKERS];
} BoardConfigStore_t;

typedef struct
{
    uint32_t ota_req_magic;
    uint32_t app_valid_magic;
    uint32_t fw_size;
    uint32_t fw_crc32;
    uint32_t req_fail_count;  // 请求URL失败次数
    char url[128];
    BoardConfigStore_t board_cfg;
    uint32_t reserved[3];
} OTA_Info_t;

void OTA_Info_Load(OTA_Info_t* info);
uint8_t OTA_Info_Save(const OTA_Info_t* info);
uint8_t OTA_Info_Clear(void);

uint8_t OTA_Info_HasRequest(void);
uint8_t OTA_Info_IsRequestValid(const OTA_Info_t* info);
uint8_t OTA_Info_AppMarkedValid(void);

void OTA_Info_MarkAppValid(void);
void OTA_Info_ClearAppValid(void);
void OTA_Info_SetRequest(uint32_t fw_size, uint32_t fw_crc32, const char* url);
void OTA_Info_ClearRequest(void);

uint32_t OTA_Info_GetReqFailCount(void);
void OTA_Info_ClearReqFailCount(void);
void OTA_Info_IncReqFailCount(void);

#endif

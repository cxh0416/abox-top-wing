#ifndef __OTA_INFO_H__
#define __OTA_INFO_H__

#include <stdint.h>
#include "boot_cfg.h"

#define BOARD_CONFIG_MAGIC 0x57434647U
#define BOARD_CONFIG_VERSION 1U
#define BOARD_CONFIG_FLAG_DIAG_BOOT_ONCE 0x0001U

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t checksum;
    uint16_t flags;
    uint16_t reserved0;
    char vid[32];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_username[32];
    char mqtt_password[32];
    uint8_t diag_log_enabled;
    uint8_t diag_log_level;
    uint16_t diag_log_port;
    char diag_log_host[64];
    uint32_t reserved[8];
} BoardConfigStore_t;

typedef struct
{
    uint32_t ota_req_magic;
    uint32_t app_valid_magic;
    uint32_t fw_size;
    uint32_t fw_crc32;
    uint32_t req_fail_count;
    char url[128];
    BoardConfigStore_t board_cfg;
    uint32_t reserved[3];
} OTA_Info_t;

void OTA_Info_Load(OTA_Info_t* info);
uint8_t OTA_Info_Save(const OTA_Info_t* info);
uint8_t OTA_Info_Clear(void);
uint8_t OTA_Info_LoadBoardConfig(BoardConfigStore_t* cfg);
uint8_t OTA_Info_SaveBoardConfig(const BoardConfigStore_t* cfg);
uint8_t OTA_Info_HasRequest(void);
uint8_t OTA_Info_AppMarkedValid(void);
void OTA_Info_MarkAppValid(void);
void OTA_Info_ClearAppValid(void);
void OTA_Info_SetRequest(uint32_t fw_size, uint32_t fw_crc32, const char* url);
void OTA_Info_ClearRequest(void);
uint32_t OTA_Info_GetReqFailCount(void);
void OTA_Info_ClearReqFailCount(void);
void OTA_Info_IncReqFailCount(void);

#endif

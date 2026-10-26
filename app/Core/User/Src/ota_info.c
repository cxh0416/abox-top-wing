#include "ota_info.h"

#include <string.h>
#include <stddef.h>

#include "main.h"
#include "mqtt_v4_store.h"

static OTA_Info_t g_ota_info_work;

static void OTA_Info_ReadRaw(OTA_Info_t* info)
{
    if (info == 0) return;
    memcpy(info, (const void*)OTA_INFO_ADDR, sizeof(OTA_Info_t));
}

void OTA_Info_Load(OTA_Info_t* info) { OTA_Info_ReadRaw(info); }

static uint8_t OTA_Info_WriteRaw(const OTA_Info_t* info)
{
    HAL_StatusTypeDef st;
    uint32_t page_error = 0;
    FLASH_EraseInitTypeDef erase;
    const uint32_t* p = (const uint32_t*)info;
    uint32_t addr = OTA_INFO_ADDR;
    uint32_t words = (sizeof(OTA_Info_t) + 3U) / 4U;
    uint32_t i;
    const uint32_t marker = *(const uint32_t*)(OTA_INFO_ADDR + offsetof(OTA_Info_t, reserved));

    if (info == 0) return 0;
    /* Never erase protected request reservations while saving configuration. */
    if (!MQTT_V4_StoreCanErase()) return 0;

    HAL_FLASH_Unlock();

    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = OTA_INFO_ADDR;
    erase.NbPages = 1;

    st = HAL_FLASHEx_Erase(&erase, &page_error);
    if (st != HAL_OK)
    {
        HAL_FLASH_Lock();
        return 0;
    }

    for (i = 0; i < words; i++)
    {
        uint32_t word = i * sizeof(uint32_t) == offsetof(OTA_Info_t, reserved) ? marker : p[i];
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word);
        if (st != HAL_OK)
        {
            HAL_FLASH_Lock();
            return 0;
        }
        addr += 4U;
    }

    HAL_FLASH_Lock();
    return *(const uint32_t*)(OTA_INFO_ADDR + offsetof(OTA_Info_t, reserved)) == marker;
}

uint8_t OTA_Info_Save(const OTA_Info_t* info) { return OTA_Info_WriteRaw(info); }

uint8_t OTA_Info_LoadBoardConfig(BoardConfigStore_t* cfg)
{
    if (cfg == 0) return 0U;
    OTA_Info_ReadRaw(&g_ota_info_work);
    *cfg = g_ota_info_work.board_cfg;
    return 1U;
}

uint8_t OTA_Info_SaveBoardConfig(const BoardConfigStore_t* cfg)
{
    if (cfg == 0) return 0U;
    OTA_Info_ReadRaw(&g_ota_info_work);
    g_ota_info_work.board_cfg = *cfg;
    return OTA_Info_WriteRaw(&g_ota_info_work);
}

uint8_t OTA_Info_Clear(void)
{
    memset(&g_ota_info_work, 0xFF, sizeof(g_ota_info_work));
    return OTA_Info_WriteRaw(&g_ota_info_work);
}

uint8_t OTA_Info_HasRequest(void)
{
    OTA_Info_ReadRaw(&g_ota_info_work);
    return (g_ota_info_work.ota_req_magic == OTA_REQ_MAGIC) ? 1U : 0U;
}

uint8_t OTA_Info_AppMarkedValid(void)
{
    OTA_Info_ReadRaw(&g_ota_info_work);
    return (g_ota_info_work.app_valid_magic == APP_VALID_MAGIC) ? 1U : 0U;
}

void OTA_Info_MarkAppValid(void)
{
    OTA_Info_ReadRaw(&g_ota_info_work);

    if ((g_ota_info_work.ota_req_magic == 0xFFFFFFFFU) || (g_ota_info_work.ota_req_magic == 0x00000000U))
        g_ota_info_work.ota_req_magic = 0;

    g_ota_info_work.app_valid_magic = APP_VALID_MAGIC;
    OTA_Info_WriteRaw(&g_ota_info_work);
}

void OTA_Info_ClearAppValid(void)
{
    OTA_Info_ReadRaw(&g_ota_info_work);
    g_ota_info_work.app_valid_magic = 0;
    OTA_Info_WriteRaw(&g_ota_info_work);
}

void OTA_Info_SetRequest(uint32_t fw_size, uint32_t fw_crc32, const char* url)
{
    OTA_Info_ReadRaw(&g_ota_info_work);

    g_ota_info_work.ota_req_magic = OTA_REQ_MAGIC;
    g_ota_info_work.fw_size = fw_size;
    g_ota_info_work.fw_crc32 = fw_crc32;
    g_ota_info_work.req_fail_count = 0;

    memset(g_ota_info_work.url, 0, sizeof(g_ota_info_work.url));
    if (url != 0)
    {
        strncpy(g_ota_info_work.url, url, sizeof(g_ota_info_work.url) - 1U);
        g_ota_info_work.url[sizeof(g_ota_info_work.url) - 1U] = '\0';
    }

    OTA_Info_WriteRaw(&g_ota_info_work);
}

void OTA_Info_ClearRequest(void)
{
    OTA_Info_ReadRaw(&g_ota_info_work);
    g_ota_info_work.ota_req_magic = 0;
    g_ota_info_work.req_fail_count = 0;
    memset(g_ota_info_work.url, 0, sizeof(g_ota_info_work.url));
    OTA_Info_WriteRaw(&g_ota_info_work);
}

uint32_t OTA_Info_GetReqFailCount(void)
{
    OTA_Info_ReadRaw(&g_ota_info_work);

    if (g_ota_info_work.ota_req_magic != OTA_REQ_MAGIC) return 0;
    return g_ota_info_work.req_fail_count;
}

void OTA_Info_ClearReqFailCount(void)
{
    OTA_Info_ReadRaw(&g_ota_info_work);

    g_ota_info_work.req_fail_count = 0;
    OTA_Info_WriteRaw(&g_ota_info_work);
}

void OTA_Info_IncReqFailCount(void)
{
    OTA_Info_ReadRaw(&g_ota_info_work);

    if (g_ota_info_work.ota_req_magic != OTA_REQ_MAGIC) return;

    if (g_ota_info_work.req_fail_count < 0xFFFFFFFFU) g_ota_info_work.req_fail_count++;

    OTA_Info_WriteRaw(&g_ota_info_work);
}

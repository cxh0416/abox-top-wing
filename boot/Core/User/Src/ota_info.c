#include "ota_info.h"

#include <string.h>

#include "main.h"

static void OTA_Info_ReadRaw(OTA_Info_t* info)
{
    if (info == 0) return;
    memcpy(info, (const void*)OTA_INFO_ADDR, sizeof(OTA_Info_t));
}

void OTA_Info_Load(OTA_Info_t* info) { OTA_Info_ReadRaw(info); }

static uint8_t OTA_Info_IsUrlValid(const char* url, uint32_t size)
{
    uint32_t i;

    if ((url == 0) || (size == 0U)) return 0U;
    if ((url[0] == '\0') || ((uint8_t)url[0] == 0xFFU)) return 0U;

    if ((strncmp(url, "http://", 7U) != 0) && (strncmp(url, "https://", 8U) != 0)) return 0U;

    for (i = 0; i < size; i++)
    {
        uint8_t c = (uint8_t)url[i];
        if (c == '\0') return 1U;
        if (c < 0x20U || c > 0x7EU) return 0U;
    }

    return 0U;
}

uint8_t OTA_Info_IsRequestValid(const OTA_Info_t* info)
{
    if (info == 0) return 0U;
    if (info->ota_req_magic != OTA_REQ_MAGIC) return 0U;
    return OTA_Info_IsUrlValid(info->url, (uint32_t)sizeof(info->url));
}

static uint8_t OTA_Info_WriteRaw(const OTA_Info_t* info)
{
    HAL_StatusTypeDef st;
    uint32_t page_error = 0;
    FLASH_EraseInitTypeDef erase;
    const uint32_t* p = (const uint32_t*)info;
    uint32_t addr = OTA_INFO_ADDR;
    uint32_t words = (sizeof(OTA_Info_t) + 3U) / 4U;
    uint32_t i;

    if (info == 0) return 0;

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
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, p[i]);
        if (st != HAL_OK)
        {
            HAL_FLASH_Lock();
            return 0;
        }
        addr += 4U;
    }

    HAL_FLASH_Lock();
    return 1;
}

uint8_t OTA_Info_Save(const OTA_Info_t* info) { return OTA_Info_WriteRaw(info); }

uint8_t OTA_Info_Clear(void)
{
    OTA_Info_t info;
    memset(&info, 0xFF, sizeof(info));
    return OTA_Info_WriteRaw(&info);
}

uint8_t OTA_Info_HasRequest(void)
{
    OTA_Info_t info;
    OTA_Info_ReadRaw(&info);
    return OTA_Info_IsRequestValid(&info);
}

uint8_t OTA_Info_AppMarkedValid(void)
{
    OTA_Info_t info;
    OTA_Info_ReadRaw(&info);
    return (info.app_valid_magic == APP_VALID_MAGIC) ? 1U : 0U;
}

void OTA_Info_MarkAppValid(void)
{
    OTA_Info_t info;
    OTA_Info_ReadRaw(&info);

    if ((info.ota_req_magic == 0xFFFFFFFFU) || (info.ota_req_magic == 0x00000000U)) info.ota_req_magic = 0;

    info.app_valid_magic = APP_VALID_MAGIC;
    OTA_Info_WriteRaw(&info);
}

void OTA_Info_ClearAppValid(void)
{
    OTA_Info_t info;
    OTA_Info_ReadRaw(&info);
    info.app_valid_magic = 0;
    OTA_Info_WriteRaw(&info);
}

void OTA_Info_SetRequest(uint32_t fw_size, uint32_t fw_crc32, const char* url)
{
    OTA_Info_t info;
    OTA_Info_ReadRaw(&info);

    info.ota_req_magic = OTA_REQ_MAGIC;
    info.fw_size = fw_size;
    info.fw_crc32 = fw_crc32;
    info.req_fail_count = 0;

    memset(info.url, 0, sizeof(info.url));
    if (url != 0)
    {
        strncpy(info.url, url, sizeof(info.url) - 1U);
        info.url[sizeof(info.url) - 1U] = '\0';
    }

    if (!OTA_Info_IsRequestValid(&info)) info.ota_req_magic = 0;

    OTA_Info_WriteRaw(&info);
}

void OTA_Info_ClearRequest(void)
{
    OTA_Info_t info;
    OTA_Info_ReadRaw(&info);
    info.ota_req_magic = 0;
    info.req_fail_count = 0;
    memset(info.url, 0, sizeof(info.url));
    OTA_Info_WriteRaw(&info);
}

uint32_t OTA_Info_GetReqFailCount(void)
{
    OTA_Info_t info;
    OTA_Info_ReadRaw(&info);

    if (!OTA_Info_IsRequestValid(&info)) return 0;
    return info.req_fail_count;
}

void OTA_Info_ClearReqFailCount(void)
{
    OTA_Info_t info;
    OTA_Info_ReadRaw(&info);

    info.req_fail_count = 0;
    OTA_Info_WriteRaw(&info);
}

void OTA_Info_IncReqFailCount(void)
{
    OTA_Info_t info;
    OTA_Info_ReadRaw(&info);

    if (!OTA_Info_IsRequestValid(&info)) return;

    if (info.req_fail_count < 0xFFFFFFFFU) info.req_fail_count++;

    OTA_Info_WriteRaw(&info);
}

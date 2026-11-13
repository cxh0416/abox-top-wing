#include "ota_info.h"

#include <stddef.h>
#include <string.h>
#include "abox_platform_port.h"
#include "mqtt_v4_store.h"
#include "ota_service.h"

/* Snapshot the old configuration before touching its erase page. The final
 * commit makes the snapshot authoritative only after it has been verified. */
#define RECOVERY_MAGIC 0x53524331U
#define RECOVERY_COMMIT 0x434F4D31U
typedef struct {
    uint32_t magic, length, old_crc, new_crc, new_crc_inv;
    OTA_Info_t old;
    uint32_t commit;
} RecoveryRecord;
_Static_assert(sizeof(OTA_Info_t) % 4U == 0U, "unaligned configuration");
_Static_assert(sizeof(RecoveryRecord) <= CONFIG_RECOVERY_PAGE_SIZE, "recovery overflow");
static OTA_Info_t g_ota_info_work;
static uint8_t g_config_busy;
#ifdef TOP_WING_CONFIG_RECOVERY_TEST
void OTA_Info_TestReset(void) { g_config_busy = 0U; }
#endif

static uint32_t crc32(const void *data, uint32_t length)
{
    const uint8_t *p = data; uint32_t crc = 0xFFFFFFFFU;
    while (length--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8U; ++i)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xFFFFFFFFU;
}
static uint8_t words(uint32_t address, const void *data, uint32_t length)
{
    const uint8_t *bytes = data;
    for (uint32_t i = 0; i < length; i += 4U)
        if (!ABox_PortFlashWrite(address + i, bytes + i, 4U) ||
            memcmp((const void *)(uintptr_t)(address + i), bytes + i, 4U)) return 0U;
    return 1U;
}
static uint8_t valid_record(const RecoveryRecord *r)
{
    return r->magic == RECOVERY_MAGIC && r->length == sizeof(OTA_Info_t) &&
           r->new_crc == ~r->new_crc_inv && r->old_crc == crc32(&r->old, sizeof(r->old));
}
static uint8_t scratch_blank(void)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)CONFIG_RECOVERY_ADDR;
    for (uint32_t i = 0; i < CONFIG_RECOVERY_PAGE_SIZE; ++i) if (p[i] != 0xFFU) return 0U;
    return 1U;
}
static uint8_t recover_locked(void)
{
    const RecoveryRecord *r = (const RecoveryRecord *)(uintptr_t)CONFIG_RECOVERY_ADDR;
    const uint32_t zero = 0U;
    if (r->commit != RECOVERY_COMMIT) return 1U;
    if (!valid_record(r) || !ABox_PortFlashBegin()) return 0U;
    uint8_t ok = 1U;
    if (crc32((const void *)(uintptr_t)OTA_INFO_ADDR, sizeof(r->old)) != r->new_crc)
        ok = ABox_PortFlashErasePage(OTA_INFO_ADDR) &&
             words(OTA_INFO_ADDR, &r->old, sizeof(r->old)) &&
             !memcmp((const void *)(uintptr_t)OTA_INFO_ADDR, &r->old, sizeof(r->old));
    if (ok) ok = words(CONFIG_RECOVERY_ADDR + offsetof(RecoveryRecord, commit), &zero, 4U);
    if (ok) (void)ABox_PortFlashErasePage(CONFIG_RECOVERY_ADDR);
    ABox_PortFlashEnd();
    return ok;
}
uint8_t OTA_Info_Recover(void)
{
    if (__atomic_test_and_set(&g_config_busy, __ATOMIC_ACQUIRE)) return 0U;
    uint8_t ok = recover_locked();
    __atomic_clear(&g_config_busy, __ATOMIC_RELEASE);
    return ok;
}
/* Before a direct marker write or OTA, no stale committed snapshot may be
 * revived by a later interrupted scratch erase. */
uint8_t OTA_Info_PrepareExternalWrite(void)
{
    if (!OtaService_ConfigWriteAllowed() ||
        __atomic_test_and_set(&g_config_busy, __ATOMIC_ACQUIRE)) return 0U;
    uint8_t ok = recover_locked();
    if (ok && !scratch_blank()) {
        if (!ABox_PortFlashBegin()) ok = 0U;
        else { ok = ABox_PortFlashErasePage(CONFIG_RECOVERY_ADDR) && scratch_blank(); ABox_PortFlashEnd(); }
    }
    __atomic_clear(&g_config_busy, __ATOMIC_RELEASE);
    return ok;
}
static uint8_t OTA_Info_ReadRaw(OTA_Info_t *info)
{
    if (!info) return 0U;
    if (!OTA_Info_Recover()) { memset(info, 0xFF, sizeof(*info)); return 0U; }
    memcpy(info, (const void *)(uintptr_t)OTA_INFO_ADDR, sizeof(*info));
    return 1U;
}
void OTA_Info_Load(OTA_Info_t *info) { (void)OTA_Info_ReadRaw(info); }
static uint8_t OTA_Info_WriteRaw(const OTA_Info_t *info)
{
    const RecoveryRecord *r = (const RecoveryRecord *)(uintptr_t)CONFIG_RECOVERY_ADDR;
    const OTA_Info_t *old = (const OTA_Info_t *)(uintptr_t)OTA_INFO_ADDR;
    OTA_Info_t next;
    uint32_t header[5]; const uint32_t commit = RECOVERY_COMMIT, zero = 0U;
    uint8_t ok = 0U;
    if (!info || !OtaService_ConfigWriteAllowed() ||
        __atomic_test_and_set(&g_config_busy, __ATOMIC_ACQUIRE)) return 0U;
    if (!recover_locked() || !MQTT_V4_StoreCanErase()) goto done;
    next = *info;
    next.reserved[0] = old->reserved[0];
    header[0] = RECOVERY_MAGIC; header[1] = sizeof(*old);
    header[2] = crc32(old, sizeof(*old)); header[3] = crc32(&next, sizeof(next)); header[4] = ~header[3];
    if (!ABox_PortFlashBegin()) goto done;
    if (!ABox_PortFlashErasePage(CONFIG_RECOVERY_ADDR) || !scratch_blank() ||
        !words(CONFIG_RECOVERY_ADDR, header, sizeof(header)) ||
        !words(CONFIG_RECOVERY_ADDR + offsetof(RecoveryRecord, old), old, sizeof(*old)) ||
        !words(CONFIG_RECOVERY_ADDR + offsetof(RecoveryRecord, commit), &commit, 4U) ||
        r->commit != commit || !valid_record(r)) goto end;
    if (!ABox_PortFlashErasePage(OTA_INFO_ADDR) || !words(OTA_INFO_ADDR, &next, sizeof(next)) ||
        memcmp((const void *)(uintptr_t)OTA_INFO_ADDR, &next, sizeof(next)) ||
        !words(CONFIG_RECOVERY_ADDR + offsetof(RecoveryRecord, commit), &zero, 4U)) goto end;
    (void)ABox_PortFlashErasePage(CONFIG_RECOVERY_ADDR);
    ok = 1U;
end:
    ABox_PortFlashEnd();
done:
    __atomic_clear(&g_config_busy, __ATOMIC_RELEASE);
    return ok;
}
uint8_t OTA_Info_Save(const OTA_Info_t *info) { return OTA_Info_WriteRaw(info); }

uint8_t OTA_Info_LoadBoardConfig(BoardConfigStore_t* cfg)
{
    if (cfg == 0) return 0U;
    if (!OTA_Info_ReadRaw(&g_ota_info_work)) return 0U;
    *cfg = g_ota_info_work.board_cfg;
    return 1U;
}

uint8_t OTA_Info_SaveBoardConfig(const BoardConfigStore_t* cfg)
{
    if (cfg == 0) return 0U;
    if (!OTA_Info_ReadRaw(&g_ota_info_work)) return 0U;
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

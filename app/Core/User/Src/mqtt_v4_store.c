#include "mqtt_v4_store.h"

#include <stddef.h>
#include <string.h>
#include "main.h"
#include "ota_info.h"
#include "time_base.h"
#include "abox_mqtt_v4.h"
#include "abox_boot_v2.h"

#define BEGIN_COMMIT 0x34574247U
#define COMPLETE_COMMIT 0x34574350U
#define UFS_INITIALIZED 0x3149344DU
_Static_assert(sizeof(OTA_Info_t) <= MQTT_V4_STORE_OFFSET, "V4 journal overlaps configuration");
_Static_assert(sizeof(MqttV4Record) == 168U, "V4 journal layout changed");
_Static_assert(MQTT_V4_STORE_OFFSET + MQTT_V4_STORE_SLOTS * sizeof(MqttV4Record) <= OTA_INFO_PAGE_SIZE,
               "V4 journal exceeds configuration page");

#ifdef TOP_WING_HOST_TEST
extern uint8_t* MQTT_V4_TestFlash(void);
extern int MQTT_V4_TestProgram(uint32_t offset, const void* data, uint32_t length);
#endif

static const MqttV4Record* records(void)
{
#ifdef TOP_WING_HOST_TEST
    return (const MqttV4Record*)(MQTT_V4_TestFlash() + MQTT_V4_STORE_OFFSET);
#else
    return (const MqttV4Record*)(OTA_INFO_ADDR + MQTT_V4_STORE_OFFSET);
#endif
}

static uint32_t crc(const void* data, size_t length)
{
    return ABoxBootV2_Crc32(data, (uint32_t)length);
}

static int empty(const MqttV4Record* record)
{
    const uint8_t* p = (const uint8_t*)record;
    for (size_t i = 0; i < sizeof(*record); ++i) if (p[i] != 0xFFU) return 0;
    return 1;
}

static int valid(const MqttV4Record* r)
{
    return r->begin_commit == BEGIN_COMMIT && memchr(r->request_id, 0, sizeof(r->request_id)) &&
           memchr(r->device_id, 0, sizeof(r->device_id)) &&
           r->header_crc == crc(r, offsetof(MqttV4Record, header_crc));
}

int MQTT_V4_StoreFind(const char* device_id, const char* id, MqttV4Record* record, int* slot)
{
    const MqttV4Record* entries = records();
    int found = 0;
    for (unsigned i = 0; i < MQTT_V4_STORE_SLOTS; ++i)
    {
        const MqttV4Record* r = &entries[i];
        if (empty(r)) continue;
        if (!valid(r)) return -1; /* Torn reservation must never become a new action. */
        if (strcmp(r->request_id, id) || strcmp(r->device_id, device_id)) continue;
        if (record) *record = *r;
        if (slot) *slot = (int)i;
        found = r->complete_commit == COMPLETE_COMMIT &&
                r->response_crc == crc(&r->code, offsetof(MqttV4Record, response_crc) - offsetof(MqttV4Record, code)) ? 2 : 1;
    }
    return found;
}

uint8_t MQTT_V4_StoreCanErase(void)
{
    const MqttV4Record* entries = records();
    for (unsigned i = 0; i < MQTT_V4_STORE_SLOTS; ++i)
    {
        const MqttV4Record* r = &entries[i];
        if (empty(r)) continue;
        if (!valid(r) || !TimeBase_IsTrusted() || TimeBase_GetMs() <= r->protect_until + TimeBase_GetSnapshot().error_ms) return 0U;
    }
    return 1U;
}

/* Independent first-use marker, outside the checksummed BoardConfig and
 * the legacy records. Ordinary configuration writes preserve this word. */
static uint32_t marker(void)
{
#ifdef TOP_WING_HOST_TEST
    uint32_t value;
    memcpy(&value, MQTT_V4_TestFlash() + offsetof(OTA_Info_t, reserved), sizeof(value));
    return value;
#else
    return *(const uint32_t*)(OTA_INFO_ADDR + offsetof(OTA_Info_t, reserved));
#endif
}

int MQTT_V4_StoreInitialized(void)
{
    uint32_t value = marker();
    return value == UFS_INITIALIZED ? 1 : value == UINT32_MAX ? 0 : -1;
}

int MQTT_V4_StoreMarkInitialized(void)
{
    uint32_t value = UFS_INITIALIZED;
    if (marker() == value) return 1;
    if (marker() != UINT32_MAX) return 0;
#ifdef TOP_WING_HOST_TEST
    if (!MQTT_V4_TestProgram(offsetof(OTA_Info_t, reserved), &value, sizeof(value))) return 0;
#else
    if (HAL_FLASH_Unlock() != HAL_OK) return 0;
    HAL_StatusTypeDef status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
        OTA_INFO_ADDR + offsetof(OTA_Info_t, reserved), value);
    HAL_FLASH_Lock();
    if (status != HAL_OK) return 0;
#endif
    return marker() == value;
}

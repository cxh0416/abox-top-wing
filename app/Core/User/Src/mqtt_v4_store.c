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

static int program(unsigned slot, size_t offset, const void* data, size_t length)
{
    uint32_t address = MQTT_V4_STORE_OFFSET + slot * sizeof(MqttV4Record) + offset;
    if (slot >= MQTT_V4_STORE_SLOTS || offset + length > sizeof(MqttV4Record) || (length & 3U)) return 0;
#ifdef TOP_WING_HOST_TEST
    return MQTT_V4_TestProgram(address, data, (uint32_t)length);
#else
    const uint8_t* bytes = data;
    uint32_t word;
    int ok = HAL_FLASH_Unlock() == HAL_OK;
    for (size_t i = 0; ok && i < length; i += 4U)
    {
        memcpy(&word, bytes + i, sizeof(word));
        ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, OTA_INFO_ADDR + address + i, word) == HAL_OK;
    }
    HAL_FLASH_Lock();
    return ok && !memcmp((const void*)(OTA_INFO_ADDR + address), data, length);
#endif
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

int MQTT_V4_StoreBegin(const char* device_id, const char* id, const uint8_t digest[32], uint64_t expires_at)
{
    MqttV4Record record;
    int slot = -1;
    for (unsigned i = 0; i < MQTT_V4_STORE_SLOTS; ++i)
        if (empty(&records()[i])) { slot = (int)i; break; }
    if (slot < 0 && MQTT_V4_StoreCanErase())
    {
        OTA_Info_t info;
        OTA_Info_Load(&info);
        if (OTA_Info_Save(&info)) slot = 0;
    }
    if (slot < 0) return -1;
    memset(&record, 0xFF, sizeof(record));
    record.protect_until = ABoxMqttV4_IdempotencyProtectUntil(expires_at);
    memcpy(record.digest, digest, sizeof(record.digest));
    memset(record.request_id, 0, sizeof(record.request_id));
    memcpy(record.request_id, id, strlen(id));
    memset(record.device_id, 0, sizeof(record.device_id));
    memcpy(record.device_id, device_id, strlen(device_id));
    record.header_crc = crc(&record, offsetof(MqttV4Record, header_crc));
    record.begin_commit = BEGIN_COMMIT;
    if (!program((unsigned)slot, 0, &record, offsetof(MqttV4Record, begin_commit)) ||
        !program((unsigned)slot, offsetof(MqttV4Record, begin_commit), &record.begin_commit, sizeof(record.begin_commit))) return -2;
    return slot;
}

int MQTT_V4_StoreComplete(int slot, uint16_t code, uint64_t timestamp)
{
    MqttV4Record record;
    if (slot < 0 || slot >= (int)MQTT_V4_STORE_SLOTS || !valid(&records()[slot])) return 0;
    record = records()[slot];
    record.code = code;
    record.timestamp = timestamp;
    record.response_crc = crc(&record.code, offsetof(MqttV4Record, response_crc) - offsetof(MqttV4Record, code));
    record.complete_commit = COMPLETE_COMMIT;
    return program((unsigned)slot, offsetof(MqttV4Record, code), &record.code,
                   offsetof(MqttV4Record, complete_commit) - offsetof(MqttV4Record, code)) &&
           program((unsigned)slot, offsetof(MqttV4Record, complete_commit), &record.complete_commit, sizeof(record.complete_commit));
}

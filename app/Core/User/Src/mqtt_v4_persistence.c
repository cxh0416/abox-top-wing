#include "mqtt_v4_store.h"

#include <stdio.h>
#include <string.h>
#include "abox_sha256.h"
#include "abox_ec800_ufs_at.h"
#include "abox_mqtt_idem_ufs.h"
#include "abox_platform_port.h"
#include "app_log.h"
#include "main.h"
#include "mqtt_core.h"
#include "ota_service.h"

/* Init explicitly clears these NOLOAD objects above the frozen fault mailbox,
 * leaving the existing minimum heap below that mailbox available. */
static ABoxMqttIdemUfs store __attribute__((section(".ota_work"), aligned(4)));
static ABoxEc800UfsAt backend __attribute__((section(".ota_work"), aligned(4)));
static ABoxMqttIdemEntry entries[MQTT_V4_IDEM_COUNT] __attribute__((section(".ota_work"), aligned(4)));
static uint8_t responses[MQTT_V4_IDEM_COUNT * MQTT_V4_IDEM_RESPONSE_SIZE]
    __attribute__((section(".ota_work"), aligned(4)));
static char hardware_id[33];
static uint8_t initialized, attempted;

static int used(void* context) { (void)context; return MQTT_V4_StoreInitialized(); }
static int mark(void* context) { (void)context; return MQTT_V4_StoreMarkInitialized(); }
static int acquire(void* context)
{
    (void)context;
    /* Do not put a 15-second file operation behind a 125-second MQTT open. */
    return MQTT_Core_CanRunBackgroundAt() && OtaService_AcquireIdemWorkspace();
}
static void release(void* context) { (void)context; OtaService_ReleaseIdemWorkspace(); }

ABoxMqttIdempotency* MQTT_V4_Store(void) { return initialized ? &store.cache : NULL; }
uint8_t MQTT_V4_StoreBusy(void) { return initialized && store.journal.leased; }

/* Stable physical journal, logical Topic identity in the key. This keeps
 * protected records through VID trials/rollback without losing their scope. */
void MQTT_V4_StoreKey(const char* device_id, const char* request_id, char key[65])
{
    static const char hex[] = "0123456789abcdef";
    ABoxSha256 sha;
    uint8_t digest[32];
    ABoxSha256_Init(&sha);
    ABoxSha256_Update(&sha, (const uint8_t*)device_id, (uint32_t)strlen(device_id) + 1U);
    ABoxSha256_Update(&sha, (const uint8_t*)request_id, (uint32_t)strlen(request_id));
    ABoxSha256_Final(&sha, digest);
    for (size_t i = 0; i < sizeof(digest); ++i)
    { key[2U*i] = hex[digest[i] >> 4U]; key[2U*i + 1U] = hex[digest[i] & 15U]; }
    key[64] = 0;
}

void MQTT_V4_StoreTask(void)
{
    uint64_t now = ABox_PortGetMonotonicMs();
    if (!attempted && MQTT_Core_AtCore())
    {
        ABoxEc800UfsPort files;
        const ABoxMqttIdemUfsPort resources = {NULL, used, mark, acquire, release};
        attempted = 1U;
        snprintf(hardware_id, sizeof(hardware_id), "%08lX%08lX%08lX",
            (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(), (unsigned long)HAL_GetUIDw2());
        /* This owner is independent of MQTT, TLS preparation and OTA. The
         * Journal holds the one workspace lease for the entire transaction. */
        if (!ABoxEc800UfsAt_Init(&backend, MQTT_Core_AtCore(), ABOX_EC800_OWNER_PRODUCT_BASE + 6U, NULL))
        { APP_LOGE("IDEM", "file backend initialization failed"); return; }
        files = ABoxEc800UfsAt_Port(&backend);
        initialized = ABoxMqttIdemUfs_Init(&store, hardware_id, entries, MQTT_V4_IDEM_COUNT,
            responses, MQTT_V4_IDEM_RESPONSE_SIZE, &files, &resources,
            "UFS:wing-v4-idem-a.bin", "UFS:wing-v4-idem-b.bin",
            OtaService_IdemWorkspace(), MQTT_V4_IDEM_WORKSPACE_SIZE);
        if (!initialized) { APP_LOGE("IDEM", "journal initialization failed"); return; }
    }
    if (!initialized) return;
    uint8_t was_failed = store.cache.failed;
    if (!store.loaded && !store.loading && ABoxUfsJournal_Status(&store.journal) == 0)
        (void)ABoxMqttIdemUfs_Load(&store, now);
    ABoxMqttIdemUfs_Poll(&store, now);
    if (!was_failed && store.cache.failed)
        APP_LOGE("IDEM", "journal unavailable; new motion disabled, stop/query available");
}

void MQTT_V4_StoreOnModemReset(void)
{
    if (initialized) ABoxMqttIdemUfs_OnModemReset(&store);
}

#ifdef TOP_WING_HOST_TEST
void MQTT_V4_TestStoreBoot(const ABoxEc800UfsPort* files)
{
    const ABoxMqttIdemUfsPort resources = {NULL, used, mark, acquire, release};
    attempted = 1U;
    strcpy(hardware_id, "123456789ABCDEF013579BDF");
    initialized = ABoxMqttIdemUfs_Init(&store, hardware_id, entries, MQTT_V4_IDEM_COUNT,
        responses, MQTT_V4_IDEM_RESPONSE_SIZE, files, &resources,
        "UFS:wing-v4-idem-a.bin", "UFS:wing-v4-idem-b.bin",
        OtaService_IdemWorkspace(), MQTT_V4_IDEM_WORKSPACE_SIZE);
}
#endif

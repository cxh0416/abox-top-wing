#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "ota_info.h"

void OTA_Info_TestReset(void);
static jmp_buf reset_point;
static int event_count, cut_at, cut_mode, write_allowed = 1, erase_allowed = 1;

uint8_t OtaService_ConfigWriteAllowed(void) { return (uint8_t)write_allowed; }
uint8_t MQTT_V4_StoreCanErase(void) { return (uint8_t)erase_allowed; }
int ABox_PortFlashBegin(void) { return 1; }
void ABox_PortFlashEnd(void) {}

int ABox_PortFlashErasePage(uint32_t address)
{
    assert(address == OTA_INFO_ADDR || address == CONFIG_RECOVERY_ADDR);
    int cut = ++event_count == cut_at;
    uint8_t *p = (uint8_t *)(uintptr_t)address;
    if (cut && cut_mode == 0) longjmp(reset_point, 1);
    if (cut && cut_mode == 1) {
        /* A partial erase can affect any bits, including the final commit. */
        for (unsigned i = 0; i < OTA_FLASH_PAGE_SIZE; ++i) p[i] |= 0x55U;
        longjmp(reset_point, 1);
    }
    if (cut && cut_mode == 3) {
        /* Worst-case torn erase: revive only the previous commit word. */
        if (address == CONFIG_RECOVERY_ADDR) {
            const uint32_t old_commit = 0x434F4D31U;
            uint32_t *word = (uint32_t *)(void *)(p + 5U * sizeof(uint32_t) + sizeof(OTA_Info_t));
            *word |= old_commit;
        }
        longjmp(reset_point, 1);
    }
    memset(p, 0xff, OTA_FLASH_PAGE_SIZE);
    if (cut) longjmp(reset_point, 1);
    return 1;
}

int ABox_PortFlashWrite(uint32_t address, const uint8_t *bytes, uint32_t length)
{
    assert(length == 4);
    assert((address >= OTA_INFO_ADDR && address + length <= OTA_INFO_END_ADDR + 1U) ||
           (address >= CONFIG_RECOVERY_ADDR && address + length <= CONFIG_RECOVERY_ADDR + OTA_FLASH_PAGE_SIZE));
    int cut = ++event_count == cut_at;
    uint8_t *p = (uint8_t *)(uintptr_t)address;
    if (cut && cut_mode == 0) longjmp(reset_point, 1);
    for (unsigned i = 0; i < (cut && cut_mode == 1 ? 2U : length); ++i) {
        assert((p[i] & bytes[i]) == bytes[i]);
        p[i] &= bytes[i];
    }
    if (cut) longjmp(reset_point, 1);
    return 1;
}

static OTA_Info_t old_config, new_config;
static void setup(void)
{
    memset((void *)(uintptr_t)0x08030000U, 0xff, 65536U);
    memcpy((void *)(uintptr_t)OTA_INFO_ADDR, &old_config, sizeof(old_config));
    OTA_Info_TestReset();
    event_count = cut_at = 0;
    write_allowed = 1;
    erase_allowed = 1;
}

static void assert_old_or_new(void)
{
    OTA_Info_t result;
    assert(OTA_Info_Recover());
    OTA_Info_Load(&result);
    assert(!memcmp(&result, &old_config, sizeof(result)) ||
           !memcmp(&result, &new_config, sizeof(result)));
    for (unsigned i = 0; i < 2U * OTA_FLASH_PAGE_SIZE; ++i)
        assert(*((uint8_t *)(uintptr_t)BOOT_STATE_A_ADDR + i) == 0xff);
}

int main(void)
{
#ifdef _WIN32
    assert(VirtualAlloc((void *)0x08030000, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    assert(mmap((void *)0x08030000, 65536, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void *)-1);
#endif
    memset(&old_config, 0x35, sizeof(old_config));
    memset(&new_config, 0xa6, sizeof(new_config));
    old_config.reserved[0] = 0x3149344DU;
    new_config.reserved[0] = 0x3149344DU;
    setup();
    assert(OTA_Info_Save(&new_config));
    int save_events = event_count;
    assert_old_or_new();
    for (int n = 1; n <= save_events; ++n) {
        for (int mode = 0; mode < 4; ++mode) {
            setup(); cut_at = n; cut_mode = mode;
            if (!setjmp(reset_point)) { (void)OTA_Info_Save(&new_config); assert(0); }
            OTA_Info_TestReset(); cut_at = 0;
            assert_old_or_new();
        }
    }
    /* A cut during the final scratch erase must preserve the complete new page. */
    setup(); cut_at = save_events; cut_mode = 3;
    if (!setjmp(reset_point)) { (void)OTA_Info_Save(&new_config); assert(0); }
    OTA_Info_TestReset(); cut_at = 0;
    assert(OTA_Info_Recover());
    assert(!memcmp((void *)(uintptr_t)OTA_INFO_ADDR, &new_config, sizeof(new_config)));

    /* Reboot repeatedly during restoration of a committed old snapshot. */
    setup();
    /* Locate the first write to the primary page by cutting at each event. */
    int recovery_events = 0, interrupted_event = 0;
    for (int n = 1; n <= save_events && !interrupted_event; ++n) {
        setup(); cut_at = n; cut_mode = 1;
        if (!setjmp(reset_point)) (void)OTA_Info_Save(&new_config);
        OTA_Info_TestReset(); cut_at = 0; event_count = 0;
        if (memcmp((void *)(uintptr_t)OTA_INFO_ADDR, &old_config, sizeof(old_config)) &&
            memcmp((void *)(uintptr_t)OTA_INFO_ADDR, &new_config, sizeof(new_config))) {
            assert(OTA_Info_Recover());
            recovery_events = event_count; interrupted_event = n;
        }
    }
    assert(recovery_events && interrupted_event);
    for (int n = 1; n <= recovery_events; ++n) {
        for (int mode = 0; mode < 4; ++mode) {
            setup(); cut_at = interrupted_event; cut_mode = 1;
            if (!setjmp(reset_point)) { (void)OTA_Info_Save(&new_config); assert(0); }
            OTA_Info_TestReset(); event_count = 0; cut_at = n; cut_mode = mode;
            if (!setjmp(reset_point)) { (void)OTA_Info_Recover(); assert(0); }
            OTA_Info_TestReset(); cut_at = 0;
            assert_old_or_new();
        }
    }
    setup(); write_allowed = 0;
    assert(!OTA_Info_Save(&new_config) && event_count == 0);
    assert(!memcmp((void *)(uintptr_t)OTA_INFO_ADDR, &old_config, sizeof(old_config)));
    assert(!OTA_Info_PrepareExternalWrite() && event_count == 0);
    setup(); erase_allowed = 0;
    assert(!OTA_Info_Save(&new_config) && event_count == 0);
    assert(!memcmp((void *)(uintptr_t)OTA_INFO_ADDR, &old_config, sizeof(old_config)));
    /* A committed but corrupt recovery record cannot authorize a new write or
     * cause a damaged page to be interpreted as a virgin replay marker. */
    setup(); cut_at = 9 + sizeof(OTA_Info_t) / 4U; cut_mode = 0;
    if (!setjmp(reset_point)) { (void)OTA_Info_Save(&new_config); assert(0); }
    OTA_Info_TestReset(); cut_at = 0;
    *((uint32_t *)(uintptr_t)(CONFIG_RECOVERY_ADDR + 8U)) ^= 1U;
    assert(!OTA_Info_Recover());
    assert(!OTA_Info_PrepareExternalWrite());
    assert(!OTA_Info_Save(&new_config));
    printf("config recovery: %d save events, %d recovery events, four cut modes\n", save_events, recovery_events);
    return 0;
}

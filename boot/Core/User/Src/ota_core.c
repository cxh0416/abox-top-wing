#include "ota_core.h"

#include <stdio.h>
#include <string.h>

#include "boot_cfg.h"
#include "abox_boot_v2_boot.h"
#include "boot_dbg.h"
#include "boot_diag.h"
#include "boot_jump.h"
#include "boot_state.h"
#include "boot_test.h"
#include "boot_version.h"
#include "fault_mailbox.h"
#include "main.h"
#include "ota_image.h"
#include "usart.h"

#define UFS_CHUNK 1024U
#define RESET_REASON_IWDG 3U

static BootStateRecord_t g_record;
static uint8_t g_started;
static uint8_t g_reset_reason;
static uint32_t g_reset_flags;
/* Deliberately shared by UFS verification and programming: Boot's stack stays small. */
static uint8_t g_ufs_buffer[UFS_CHUNK + 1U];

typedef enum {
    INSTALL_RESULT_OK = 0U,
    INSTALL_RESULT_PRE_ERASE_FAILED = 1U,
    INSTALL_RESULT_FLASH_FAILED = 2U,
} InstallResult_t;

const char g_sweeper_boot_version_marker[] __attribute__((used, section(".fw_version"))) =
    SWEEPER_BOOT_FW_VERSION_MARKER;

static uint32_t crc_update(uint32_t crc, const uint8_t *data, uint16_t length)
{
    while (length-- != 0U) {
        uint8_t bit;
        crc ^= *data++;
        for (bit = 0U; bit < 8U; ++bit) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc;
}
static const char *file_name(uint8_t slot) { return slot == 0U ? "fw_slot0.bin" : "fw_slot1.bin"; }
static uint8_t ufs_read(uint32_t handle, uint8_t *data, uint16_t wanted, uint16_t *got);

static uint8_t at_line(char *out, uint16_t size, uint32_t timeout_ms)
{
    uint16_t length = 0U;
    uint8_t ch;
    uint32_t started = HAL_GetTick();
    if (out == 0 || size < 2U) return 0U;
    while ((HAL_GetTick() - started) < timeout_ms) {
        if (HAL_UART_Receive(&huart1, &ch, 1U, 25U) != HAL_OK) continue;
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (length != 0U) {
                out[length] = '\0';
                return 1U;
            }
            continue;
        }
        if (length < size - 1U) out[length++] = (char)ch;
    }
    return 0U;
}

static uint8_t at_send(const char *command)
{
    char line[128];
    char tx[160];
    uint32_t started = HAL_GetTick();
    int length = snprintf(tx, sizeof(tx), "%s\r\n", command);
    if (length <= 0 || HAL_UART_Transmit(&huart1, (uint8_t *)tx, (uint16_t)length, 1000U) != HAL_OK) return 0U;
    while ((HAL_GetTick() - started) < 10000U) {
        if (!at_line(line, sizeof(line), 1000U)) continue;
        if (strcmp(line, "OK") == 0) return 1U;
        if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR", 10U) == 0) return 0U;
    }
    return 0U;
}

static void ufs_close_leaked_handles(void)
{
    uint32_t handles[8];
    uint8_t count = 0U;
    char line[128];
    static const char query[] = "AT+QFOPEN?\r\n";
    uint32_t started = HAL_GetTick();

    if (HAL_UART_Transmit(&huart1, (uint8_t *)query, sizeof(query) - 1U, 1000U) != HAL_OK) return;
    while ((HAL_GetTick() - started) < 5000U) {
        unsigned long handle;
        char name[64];
        if (!at_line(line, sizeof(line), 1000U)) continue;
        if (strncmp(line, "+QFOPEN:", 8U) == 0)
            boot_printf("[BOOT2] evt=UFS_OPEN_LEAK line=%s\r\n", line);
        if (sscanf(line, "+QFOPEN: \"%63[^\"]\",%lu", name, &handle) == 2) {
            if (count < (uint8_t)(sizeof(handles) / sizeof(handles[0]))) handles[count++] = (uint32_t)handle;
            continue;
        }
        if (strcmp(line, "OK") == 0 || strstr(line, "ERROR") != 0) break;
    }

    for (uint8_t index = 0U; index < count; ++index) {
        char command[48];
        (void)snprintf(command, sizeof(command), "AT+QFCLOSE=%lu", (unsigned long)handles[index]);
        (void)at_send(command);
    }
    if (count != 0U) boot_printf("[BOOT2] evt=UFS_CLOSE_LEAK count=%u\r\n", (unsigned)count);
}

static uint8_t ufs_open(uint8_t slot, uint32_t *handle)
{
    char command[96];
    char line[128];
    uint32_t started = HAL_GetTick();
    int length = snprintf(command, sizeof(command), "AT+QFOPEN=\"%s\",0\r\n", file_name(slot));
    if (handle == 0 || length <= 0 ||
        HAL_UART_Transmit(&huart1, (uint8_t *)command, (uint16_t)length, 1000U) != HAL_OK)
        return 0U;
    while ((HAL_GetTick() - started) < 5000U) {
        if (!at_line(line, sizeof(line), 1000U)) continue;
        if (sscanf(line, "+QFOPEN: %lu", (unsigned long *)handle) == 1) {
            boot_printf("[BOOT2] evt=UFS_OPEN slot=%u file=%s handle=%lu\r\n",
                        (unsigned)slot, file_name(slot), (unsigned long)*handle);
            return 1U;
        }
        if (strstr(line, "ERROR") != 0) {
            boot_printf("[BOOT2] evt=UFS_OPEN_FAIL slot=%u file=%s line=%s\r\n",
                        (unsigned)slot, file_name(slot), line);
            return 0U;
        }
    }
    return 0U;
}

static uint8_t ufs_close(uint32_t handle)
{
    char command[48];
    (void)snprintf(command, sizeof(command), "AT+QFCLOSE=%lu", (unsigned long)handle);
    if (!at_send(command)) {
        boot_printf("[BOOT2] evt=UFS_CLOSE_FAIL handle=%lu\r\n", (unsigned long)handle);
        return 0U;
    }
    HAL_Delay(100U);
    return 1U;
}

static uint8_t ufs_delete(uint8_t slot)
{
    char command[96];
    uint8_t result;
    (void)snprintf(command, sizeof(command), "AT+QFDEL=\"%s\"", file_name(slot));
    result = at_send(command);
    boot_printf("[BOOT2] evt=TEST_UFS_DELETE testBuild=1 testPoint=%s slot=%u file=%s result=%s\r\n",
                SWEEPER_BOOT_TEST_POINT, (unsigned)slot, file_name(slot), result ? "PASS" : "FAIL");
    return result;
}

#if 0 /* Boot V2.2 never downloads: App owns HTTPS/CA/Range staging. */
/* Stage the existing public OTA request into UFS before BootState is changed.
 * This keeps the public URL-only OTA command compatible while ensuring Boot
 * never installs a partially received image. */
static uint8_t http_set_url(const char *url)
{
    char command[96];
    char line[128];
    uint32_t started = HAL_GetTick();
    int length;
    if (url == 0 || url[0] == '\0') return 0U;
    length = snprintf(command, sizeof(command), "AT+QHTTPURL=%lu,80\r\n", (unsigned long)strlen(url));
    if (length <= 0 || HAL_UART_Transmit(&huart1, (uint8_t *)command, (uint16_t)length, 1000U) != HAL_OK) return 0U;
    while ((HAL_GetTick() - started) < 15000U) {
        if (!at_line(line, sizeof(line), 1000U)) continue;
        if (strcmp(line, "CONNECT") == 0) {
            if (HAL_UART_Transmit(&huart1, (uint8_t *)url, (uint16_t)strlen(url), 5000U) != HAL_OK) return 0U;
            continue;
        }
        if (strcmp(line, "OK") == 0) return 1U;
        if (strstr(line, "ERROR") != 0 || strncmp(line, "+CME ERROR", 10U) == 0) return 0U;
    }
    return 0U;
}

static uint8_t http_get(uint32_t *content_length)
{
    char line[128];
    uint32_t started = HAL_GetTick();
    uint8_t result_seen = 0U;
    if (HAL_UART_Transmit(&huart1, (uint8_t *)"AT+QHTTPGET=80\r\n", (uint16_t)strlen("AT+QHTTPGET=80\r\n"), 1000U) != HAL_OK) return 0U;
    while ((HAL_GetTick() - started) < 90000U) {
        unsigned long result, status, length;
        if (!at_line(line, sizeof(line), 1000U)) continue;
        if (sscanf(line, "+QHTTPGET: %lu,%lu,%lu", &result, &status, &length) == 3) {
            result_seen = 1U;
            if (content_length != 0) *content_length = (uint32_t)length;
            return (uint8_t)(result == 0U && status >= 200U && status < 300U);
        }
        if (strstr(line, "ERROR") != 0 || strncmp(line, "+CME ERROR", 10U) == 0) return 0U;
        (void)result_seen;
    }
    return 0U;
}

static uint8_t http_read_file(uint8_t slot)
{
    char command[96];
    char line[128];
    uint32_t started = HAL_GetTick();
    int length = snprintf(command, sizeof(command), "AT+QHTTPREADFILE=\"%s\",80\r\n", file_name(slot));
    if (length <= 0 || HAL_UART_Transmit(&huart1, (uint8_t *)command, (uint16_t)length, 1000U) != HAL_OK) return 0U;
    while ((HAL_GetTick() - started) < 90000U) {
        unsigned long result;
        if (!at_line(line, sizeof(line), 1000U)) continue;
        if (sscanf(line, "+QHTTPREADFILE: %lu", &result) == 1) return result == 0U ? 1U : 0U;
        if (strstr(line, "ERROR") != 0 || strncmp(line, "+CME ERROR", 10U) == 0) return 0U;
    }
    return 0U;
}

static uint8_t ufs_list_size(uint8_t slot, uint32_t *size)
{
    char command[96];
    char line[128];
    uint32_t started = HAL_GetTick();
    int length = snprintf(command, sizeof(command), "AT+QFLST=\"%s\"\r\n", file_name(slot));
    if (size == 0 || length <= 0 || HAL_UART_Transmit(&huart1, (uint8_t *)command, (uint16_t)length, 1000U) != HAL_OK) return 0U;
    while ((HAL_GetTick() - started) < 10000U) {
        char name[64];
        unsigned long value;
        if (!at_line(line, sizeof(line), 1000U)) continue;
        if (sscanf(line, "+QFLST: \"%63[^\"]\",%lu", name, &value) == 2 &&
            (strcmp(name, file_name(slot)) == 0 || (strncmp(name, "UFS:", 4U) == 0 && strcmp(name + 4U, file_name(slot)) == 0))) {
            *size = (uint32_t)value;
            return 1U;
        }
        if (strstr(line, "ERROR") != 0) return 0U;
    }
    return 0U;
}

static uint8_t ufs_measure(uint8_t slot, uint32_t size, uint32_t *crc, uint8_t vector[8])
{
    uint32_t handle;
    uint32_t total = 0U;
    uint32_t calculated = 0xFFFFFFFFU;
    uint16_t got;
    if (crc == 0 || vector == 0 || size == 0U || size > APP_MAX_SIZE || !ufs_open(slot, &handle)) return 0U;
    memset(vector, 0, 8U);
    while (total < size) {
        uint16_t wanted = (uint16_t)(((size - total) > UFS_CHUNK) ? UFS_CHUNK : (size - total));
        if (!ufs_read(handle, g_ufs_buffer, wanted, &got) || got != wanted) {
            (void)ufs_close(handle);
            return 0U;
        }
        if (total < 8U) {
            uint16_t copy = (uint16_t)(((8U - total) < got) ? (8U - total) : got);
            memcpy(&vector[total], g_ufs_buffer, copy);
        }
        calculated = crc_update(calculated, g_ufs_buffer, got);
        total += got;
    }
    if (!ufs_close(handle)) return 0U;
    *crc = calculated ^ 0xFFFFFFFFU;
    return SweeperOta_VectorValid(vector);
}

static uint8_t stage_ota_request(void)
{
    OTA_Info_t info;
    BootStateRecord_t state;
    uint8_t have_state = BootState_Load(&state);
    uint8_t slot;
    uint32_t size = 0U;
    uint32_t crc = 0U;
    uint32_t http_length = 0U;
    uint8_t vector[8];
    OTA_Info_Load(&info);
    if (!OTA_Info_IsRequestValid(&info)) return 0U;
    if (have_state && state.state != BOOT_STATE_NORMAL && state.state != BOOT_STATE_CONFIRMED) return 0U;
    slot = have_state ? (state.stable_slot == 0U ? 1U : 0U) : 0U;
    (void)ufs_delete(slot);
    if (!http_set_url(info.url) || !http_get(&http_length) || !http_read_file(slot) || !ufs_list_size(slot, &size)) return 0U;
    if (http_length != 0U && http_length != size) return 0U;
    if (info.fw_size != 0U && info.fw_size != size) return 0U;
    if (!ufs_measure(slot, size, &crc, vector) || (info.fw_crc32 != 0U && info.fw_crc32 != crc)) return 0U;
    if (!have_state) memset(&state, 0, sizeof(state));
    state.state = BOOT_STATE_INSTALL_PENDING;
    state.candidate_slot = slot;
    state.image_size = size;
    state.image_crc32 = crc;
    (void)snprintf(state.image_version, sizeof(state.image_version), "ota-request");
    if (!BootState_Save(&state)) return 0U;
    OTA_Info_ClearRequest();
    boot_printf("[BOOT2] evt=OTA_STAGE result=PASS slot=%u size=%lu crc=%08lX\r\n",
                (unsigned)slot, (unsigned long)size, (unsigned long)crc);
    return 1U;
}
#endif

static uint8_t ufs_cleanup_both(void)
{
    uint8_t slot0 = ufs_delete(0U);
    uint8_t slot1 = ufs_delete(1U);
    uint8_t result = (uint8_t)(slot0 && slot1);
    boot_printf("[BOOT2] evt=TEST_UFS_CLEANUP testBuild=1 testPoint=%s action=DELETE_BOTH result=%s\r\n",
                SWEEPER_BOOT_TEST_POINT, result ? "PASS" : "FAIL");
    return result;
}

/* QFREAD's payload is opaque.  Read exactly the modem-reported CONNECT size. */
static uint8_t ufs_read(uint32_t handle, uint8_t *data, uint16_t wanted, uint16_t *got)
{
    char command[48];
    char line[96];
    unsigned long actual = 0U;
    uint16_t received = 0U;
    uint32_t started = HAL_GetTick();
    if (data == 0 || got == 0) return 0U;
    *got = 0U;
    (void)snprintf(command, sizeof(command), "AT+QFREAD=%lu,%u\r\n", (unsigned long)handle, (unsigned)wanted);
    if (HAL_UART_Transmit(&huart1, (uint8_t *)command, (uint16_t)strlen(command), 1000U) != HAL_OK) return 0U;
    while ((HAL_GetTick() - started) < 5000U) {
        if (!at_line(line, sizeof(line), 1000U)) continue;
        if (sscanf(line, "CONNECT %lu", &actual) == 1) {
            if (actual == 0U || actual > wanted ||
                HAL_UART_Receive(&huart1, data, (uint16_t)actual, 5000U) != HAL_OK)
                return 0U;
            received = (uint16_t)actual;
            break;
        }
        if (strstr(line, "ERROR") != 0) return 0U;
    }
    if (received == 0U) return 0U;
    while ((HAL_GetTick() - started) < 10000U) {
        if (!at_line(line, sizeof(line), 1000U)) continue;
        if (strcmp(line, "OK") == 0) {
            *got = received;
            return 1U;
        }
        if (strstr(line, "ERROR") != 0) return 0U;
    }
    return 0U;
}

static uint8_t ufs_verify(uint8_t slot, uint32_t size, uint32_t crc, uint8_t vector[8])
{
    uint32_t handle;
    uint32_t total = 0U;
    uint32_t calculated = 0xFFFFFFFFU;
    uint16_t got;
    if (size == 0U || size > APP_MAX_SIZE) {
        boot_printf("[BOOT2] evt=UFS_PREFLIGHT slot=%u result=FAIL reason=SIZE size=%lu\r\n",
                    (unsigned)slot, (unsigned long)size);
        return 0U;
    }
    if (!ufs_open(slot, &handle)) {
        boot_printf("[BOOT2] evt=UFS_PREFLIGHT slot=%u result=FAIL reason=OPEN size=%lu crc=%08lX\r\n",
                    (unsigned)slot, (unsigned long)size, (unsigned long)crc);
        return 0U;
    }
    while (total < size) {
        uint16_t wanted = (uint16_t)(((size - total) > UFS_CHUNK) ? UFS_CHUNK : (size - total));
        if (!ufs_read(handle, g_ufs_buffer, wanted, &got) || got != wanted) {
            (void)ufs_close(handle);
            boot_printf("[BOOT2] evt=UFS_PREFLIGHT slot=%u result=FAIL reason=READ offset=%lu wanted=%u got=%u\r\n",
                        (unsigned)slot, (unsigned long)total, (unsigned)wanted, (unsigned)got);
            return 0U;
        }
        if (total < 8U) {
            uint16_t copy = (uint16_t)(((8U - total) < got) ? (8U - total) : got);
            memcpy(&vector[total], g_ufs_buffer, copy);
        }
        calculated = crc_update(calculated, g_ufs_buffer, got);
        total += got;
    }
    if (!ufs_close(handle)) return 0U;
    calculated ^= 0xFFFFFFFFU;
    {
        uint8_t vector_valid = SweeperOta_VectorValid(vector);
        uint32_t sp = SweeperOta_ReadLe32(vector);
        uint32_t reset = SweeperOta_ReadLe32(vector + 4U);
        if (BootTest_ForceVectorInvalid(size, crc)) vector_valid = 0U;
        boot_printf("[BOOT2] evt=UFS_PREFLIGHT slot=%u file=%s size=%lu crc=%08lX expectedCrc=%08lX vectorValid=%u sp=0x%08lX reset=0x%08lX\r\n",
                    (unsigned)slot, file_name(slot), (unsigned long)total, (unsigned long)calculated,
                    (unsigned long)crc, (unsigned)vector_valid, (unsigned long)sp, (unsigned long)reset);
        if (calculated != crc || !vector_valid) {
            boot_printf("[BOOT2] evt=UFS_PREFLIGHT_RESULT slot=%u result=FAIL reason=%s\r\n",
                        (unsigned)slot, calculated != crc ? "CRC" : "VECTOR");
            return 0U;
        }
    }
    boot_printf("[BOOT2] evt=UFS_PREFLIGHT_RESULT slot=%u result=PASS\r\n", (unsigned)slot);
    return 1U;
}

static uint8_t flash_image_is_valid(uint32_t size, uint32_t crc)
{
    return (size != 0U && size <= APP_MAX_SIZE &&
            BootState_Crc32((const void *)APP_START_ADDR, size) == crc && Boot_IsAppValid(APP_START_ADDR)) ? 1U : 0U;
}

static uint8_t save_state(const char *reason, uint32_t test_phase)
{
    uint8_t ok;
    BootTest_SetPhase(test_phase);
    ok = BootState_Save(&g_record);
    BootTest_SetPhase(BOOT_TEST_PHASE_NONE);
    boot_printf("[BOOT2] evt=STATE_SAVE reason=%s result=%s\r\n",
                reason == 0 ? "unspecified" : reason, ok ? "PASS" : "FAIL");
    BootDiag_LogStatePages("STATE_PAGES");
    if (ok) BootDiag_LogStateRecord("STATE_SNAPSHOT", &g_record);
    return ok;
}

static uint8_t flash_unlock_for(const char *reason)
{
    HAL_StatusTypeDef status = HAL_FLASH_Unlock();
    boot_printf("[BOOT2] evt=FLASH_UNLOCK reason=%s result=%s\r\n",
                reason == 0 ? "unspecified" : reason, status == HAL_OK ? "PASS" : "FAIL");
    return status == HAL_OK ? 1U : 0U;
}

static InstallResult_t flash_program_image(uint8_t slot, uint32_t size, uint32_t crc)
{
    uint8_t vector[8] = {0};
    uint32_t handle;
    uint32_t total = 0U;
    uint32_t calculated = 0xFFFFFFFFU;
    uint32_t page_error = 0U;
    uint16_t got;
    uint32_t next_progress = 10U;
    FLASH_EraseInitTypeDef erase = {0};
    if (BootTest_UfsCandidateMissingRequested(size, crc, g_record.image_version) &&
        !ufs_delete(slot)) return INSTALL_RESULT_PRE_ERASE_FAILED;
    if (!ufs_verify(slot, size, crc, vector)) return INSTALL_RESULT_PRE_ERASE_FAILED;
    if (!save_state("install_pre_erase", BOOT_TEST_PHASE_STATE_WRITE)) return INSTALL_RESULT_PRE_ERASE_FAILED;
    if (!ufs_open(slot, &handle)) return INSTALL_RESULT_PRE_ERASE_FAILED;
    boot_printf("[BOOT2] evt=APP_ERASE_START address=0x%08lX pages=%lu\r\n",
                (unsigned long)APP_START_ADDR, (unsigned long)(APP_MAX_SIZE / OTA_FLASH_PAGE_SIZE));
    if (!flash_unlock_for("app_erase")) {
        (void)ufs_close(handle);
        return INSTALL_RESULT_PRE_ERASE_FAILED;
    }
    /* From this point the current App may be partially erased; never fall back to it. */
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = APP_START_ADDR;
    erase.NbPages = APP_MAX_SIZE / OTA_FLASH_PAGE_SIZE;
    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
        boot_printf("[BOOT2] evt=APP_ERASE_DONE result=FAIL pageError=0x%08lX\r\n", (unsigned long)page_error);
        HAL_FLASH_Lock();
        (void)ufs_close(handle);
        return INSTALL_RESULT_FLASH_FAILED;
    }
    boot_printf("[BOOT2] evt=APP_ERASE_DONE result=PASS pages=%lu\r\n",
                (unsigned long)(APP_MAX_SIZE / OTA_FLASH_PAGE_SIZE));
#ifdef SWEEPER_BOOT_FAULT_INJECTION_TEST
    if (!save_state("test_after_erase", BOOT_TEST_PHASE_POST_ERASE_STATE)) {
        HAL_FLASH_Lock();
        (void)ufs_close(handle);
        return INSTALL_RESULT_FLASH_FAILED;
    }
    if (!flash_unlock_for("after_test_state")) {
        (void)ufs_close(handle);
        return INSTALL_RESULT_FLASH_FAILED;
    }
#endif
    boot_printf("[BOOT2] evt=APP_PROGRAM_START address=0x%08lX size=%lu crc=%08lX\r\n",
                (unsigned long)APP_START_ADDR, (unsigned long)size, (unsigned long)crc);
    boot_printf("[BOOT2] evt=APP_PROGRAM_PROGRESS percent=0 total=0 size=%lu\r\n", (unsigned long)size);
    while (total < size) {
        uint16_t wanted = (uint16_t)(((size - total) > UFS_CHUNK) ? UFS_CHUNK : (size - total));
        uint16_t flash_length;
        if (!ufs_read(handle, g_ufs_buffer, wanted, &got) || got != wanted) {
            HAL_FLASH_Lock();
            (void)ufs_close(handle);
            boot_printf("[BOOT2] UFS program read failed offset=%lu wanted=%u got=%u\r\n",
                        (unsigned long)total, (unsigned)wanted, (unsigned)got);
            return INSTALL_RESULT_FLASH_FAILED;
        }
        calculated = crc_update(calculated, g_ufs_buffer, got);
        flash_length = got;
        if ((flash_length & 1U) != 0U) g_ufs_buffer[flash_length++] = 0xFFU;
        for (uint16_t offset = 0U; offset < flash_length; offset += 2U) {
            BootTest_ProgramCheckpoint(total + offset, size, crc);
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, APP_START_ADDR + total + offset,
                                  *(uint16_t *)&g_ufs_buffer[offset]) != HAL_OK) {
                boot_printf("[BOOT2] evt=APP_PROGRAM_HALFWORD result=FAIL address=0x%08lX offset=%lu\r\n",
                            (unsigned long)(APP_START_ADDR + total + offset),
                            (unsigned long)(total + offset));
                HAL_FLASH_Lock();
                (void)ufs_close(handle);
                return INSTALL_RESULT_FLASH_FAILED;
            }
        }
        if (memcmp((const void *)(APP_START_ADDR + total), g_ufs_buffer, got) != 0) {
            boot_printf("[BOOT2] evt=APP_PROGRAM_CHUNK_VERIFY result=FAIL address=0x%08lX offset=%lu length=%u\r\n",
                        (unsigned long)(APP_START_ADDR + total), (unsigned long)total, (unsigned)got);
            HAL_FLASH_Lock();
            (void)ufs_close(handle);
            return INSTALL_RESULT_FLASH_FAILED;
        }
        total += got;
        while (next_progress <= 100U && total * 100U >= size * next_progress) {
            boot_printf("[BOOT2] evt=APP_PROGRAM_PROGRESS percent=%lu total=%lu size=%lu\r\n",
                        (unsigned long)next_progress, (unsigned long)total, (unsigned long)size);
            next_progress += 10U;
        }
    }
    HAL_FLASH_Lock();
    if (!ufs_close(handle)) return INSTALL_RESULT_FLASH_FAILED;
    calculated ^= 0xFFFFFFFFU;
    {
        uint32_t flash_crc = BootState_Crc32((const void *)APP_START_ADDR, size);
        uint8_t vector_valid = Boot_IsAppValid(APP_START_ADDR);
        uint8_t result = (uint8_t)(calculated == crc && flash_crc == crc && vector_valid);
        boot_printf("[BOOT2] evt=APP_VERIFY result=%s calculatedCrc=%08lX flashCrc=%08lX expectedCrc=%08lX vectorValid=%u\r\n",
                    result ? "PASS" : "FAIL", (unsigned long)calculated, (unsigned long)flash_crc,
                    (unsigned long)crc, (unsigned)vector_valid);
        return result ? INSTALL_RESULT_OK : INSTALL_RESULT_FLASH_FAILED;
    }
}

#if 0 /* Product-local lifecycle migrated to abox::boot_v2_boot. */
static uint8_t restore_stable_app_after_pre_erase_failure(void)
{
    uint8_t app_valid = flash_image_is_valid(g_record.stable_image_size, g_record.stable_image_crc32);
    boot_printf("[BOOT2] evt=INSTALL_FALLBACK phase=PRE_ERASE appValid=%u action=KEEP_STABLE_APP\r\n",
                (unsigned)app_valid);
    if (!app_valid) return 0U;

    g_record.state = BOOT_STATE_CONFIRMED;
    g_record.candidate_slot = g_record.stable_slot;
    g_record.image_size = g_record.stable_image_size;
    g_record.image_crc32 = g_record.stable_image_crc32;
    (void)snprintf(g_record.image_version, sizeof(g_record.image_version), "%s", g_record.stable_image_version);
    g_record.trial_fail_count = 0U;
    g_record.rollback_report_pending = 0U;
    g_record.rollback_installed = 0U;
    g_record.last_error = BOOT_ERROR_INSTALL_IO;
    if (!save_state("install_pre_erase_fallback", BOOT_TEST_PHASE_NONE)) {
        boot_printf("[BOOT2] evt=INSTALL_FALLBACK result=FAIL reason=STATE_SAVE\r\n");
        return 0U;
    }
    boot_printf("[BOOT2] evt=INSTALL_FALLBACK result=PASS state=CONFIRMED appVersion=%s\r\n",
                g_record.stable_image_version);
    Boot_JumpToApp(APP_START_ADDR);
    return 1U;
}
#endif

static uint8_t reset_reason(uint32_t *raw_flags)
{
    uint32_t flags = RCC->CSR;
    /* Software reset also leaves PINRSTF set on STM32F1.  Check SFTRSTF
       before PINRSTF or a HardFault handler that records its mailbox and
       calls NVIC_SystemReset() is misclassified as a normal NRST. */
    uint8_t reason = (flags & RCC_CSR_IWDGRSTF) ? RESET_REASON_IWDG :
                     (flags & RCC_CSR_PORRSTF) ? 2U :
                     (flags & RCC_CSR_SFTRSTF) ? 5U :
                     (flags & RCC_CSR_PINRSTF) ? 1U : 0U;
    if (raw_flags != 0) *raw_flags = flags;
    __HAL_RCC_CLEAR_RESET_FLAGS();
    return reason;
}

#if 0 /* Product-local trial accounting migrated to abox::boot_v2_boot. */
static void start_rollback(void)
{
    if (g_record.stable_image_size == 0U || g_record.stable_slot > 1U) {
        boot_printf("[BOOT2] evt=ROLLBACK_START result=FAIL reason=NO_STABLE_IMAGE\r\n");
        g_record.last_error = BOOT_ERROR_INSTALL_IO;
        (void)save_state("rollback_no_stable", BOOT_TEST_PHASE_NONE);
        return;
    }
    BootDiag_LogStateTransition(g_record.state, BOOT_STATE_ROLLBACK, "trial_fail_limit");
    g_record.state = BOOT_STATE_ROLLBACK;
    g_record.rollback_installed = 0U;
    boot_printf("[BOOT2] evt=ROLLBACK_START stableSlot=%u stableVersion=%s stableSize=%lu stableCrc=%08lX\r\n",
                (unsigned)g_record.stable_slot, g_record.stable_image_version,
                (unsigned long)g_record.stable_image_size, (unsigned long)g_record.stable_image_crc32);
    (void)save_state("rollback_start", BOOT_TEST_PHASE_NONE);
}

static void record_trial_failure(const char *reason)
{
    uint8_t before = g_record.trial_fail_count;
    if (g_record.trial_fail_count < 3U) g_record.trial_fail_count++;
    g_record.last_error = BOOT_ERROR_TRIAL_FAULT;
    SweeperFault_Clear();
    boot_printf("[BOOT2] evt=TRIAL_FAILURE reason=%s countBefore=%u countAfter=%u limit=3\r\n",
                reason == 0 ? "unspecified" : reason, (unsigned)before,
                (unsigned)g_record.trial_fail_count);
    if (g_record.trial_fail_count >= 3U) start_rollback();
    else (void)save_state("trial_failure", BOOT_TEST_PHASE_NONE);
}
#endif

static uint8_t common_flash_valid(void *context,uint32_t size,uint32_t crc){(void)context;return flash_image_is_valid(size,crc);}
static ABoxBootV2InstallResult common_install(void *context,uint8_t slot,uint32_t size,uint32_t crc){InstallResult_t r;(void)context;(void)BootState_Load(&g_record);r=flash_program_image(slot,size,crc);return r==INSTALL_RESULT_OK?ABOX_BOOT_V2_INSTALL_OK:r==INSTALL_RESULT_PRE_ERASE_FAILED?ABOX_BOOT_V2_INSTALL_PRE_ERASE_FAILED:ABOX_BOOT_V2_INSTALL_FAILED;}
static uint8_t common_app_valid(void *context){(void)context;return Boot_IsAppValid(APP_START_ADDR);}
static void common_jump(void *context){(void)context;Boot_JumpToApp(APP_START_ADDR);}
static uint8_t common_reset(void *context){(void)context;return g_reset_reason;}
static void common_delay(void *context,uint32_t ms){(void)context;HAL_Delay(ms);}
static void common_log(void *context,const char *line){(void)context;boot_printf("%s",line);}

void OTA_Core_Init(void)
{
    static const ABoxBootV2Layout layout={BOOT_STATE_A_ADDR,BOOT_STATE_B_ADDR,BOOT_STATE_PAGE_SIZE,APP_START_ADDR,APP_END_ADDR,0x20000000U,0x20010000U};
    static const ABoxBootV2BootPort port={0,common_flash_valid,common_install,common_app_valid,common_jump,common_reset,common_delay,common_log};
    boot_printf("[BOOT2] evt=BOOT_START version=%s testBuild=%u testPoint=%s\r\n",
                SWEEPER_BOOT_FW_VERSION, (unsigned)SWEEPER_BOOT_TEST_BUILD, SWEEPER_BOOT_TEST_POINT);
    g_reset_reason = reset_reason(&g_reset_flags);
    BootDiag_LogReset(g_reset_flags, g_reset_reason);
    HAL_UART_AbortReceive(&huart1);
    HAL_GPIO_WritePin(EC_PWR_GPIO_Port, EC_PWR_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(EN_3_8V_GPIO_Port, EN_3_8V_Pin, GPIO_PIN_RESET);
    HAL_Delay(1000U);
    HAL_GPIO_WritePin(EN_3_8V_GPIO_Port, EN_3_8V_Pin, GPIO_PIN_SET);
    HAL_Delay(1000U);
    HAL_GPIO_WritePin(EC_PWR_GPIO_Port, EC_PWR_Pin, GPIO_PIN_SET);
    HAL_Delay(2000U);
    HAL_GPIO_WritePin(EC_PWR_GPIO_Port, EC_PWR_Pin, GPIO_PIN_RESET);
    HAL_Delay(2500U);
    (void)at_send("ATE0");
    ufs_close_leaked_handles();
    g_started = 1U;
    (void)ABoxBootV2_StateBindLayout(&layout);
    (void)ABoxBootV2Boot_Init(&port);
}

void OTA_Core_Task(void)
{
    if (!g_started) return;
    if (BootTest_UfsCleanupRequested()) {
        (void)ufs_cleanup_both();
        HAL_Delay(5000U);
        return;
    }
    ABoxBootV2Boot_Task();
}

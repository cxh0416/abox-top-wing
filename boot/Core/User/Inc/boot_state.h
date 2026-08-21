#ifndef LOCKCTRL_BOOT_STATE_H
#define LOCKCTRL_BOOT_STATE_H

#include <stdint.h>
#include "boot_cfg.h"

#define BOOT_STATE_MAGIC 0x42535632U
#define BOOT_STATE_VERSION 3U
#define BOOT_SLOT_NONE 0xFFU
#define BOOT_ERROR_NONE 0U
#define BOOT_ERROR_INSTALL_IO 2U
#define BOOT_ERROR_TRIAL_FAULT 4U
#define BOOT_ERROR_ROLLED_BACK 5U

typedef enum { BOOT_STATE_NORMAL = 0, BOOT_STATE_INSTALL_PENDING, BOOT_STATE_INSTALLING, BOOT_STATE_TRIAL, BOOT_STATE_CONFIRMED, BOOT_STATE_ROLLBACK } BootStateCode_t;

typedef struct {
    uint32_t magic; uint16_t version; uint16_t length; uint32_t sequence; uint32_t state;
    uint8_t stable_slot; uint8_t candidate_slot; uint8_t trial_fail_count; uint8_t last_reset_reason;
    uint32_t image_size; uint32_t image_crc32; char image_version[32];
    uint32_t stable_image_size; uint32_t stable_image_crc32; char stable_image_version[32];
    uint32_t last_error; uint8_t rollback_report_pending; uint8_t rollback_installed; uint8_t reserved[2];
    uint32_t record_crc32;
} BootStateRecord_t;

typedef struct { uint8_t page_a_valid, page_b_valid, selected_page, selected_valid; uint32_t page_a_sequence, page_b_sequence, selected_sequence; } BootStateDiagnostics_t;
uint32_t BootState_Crc32(const void *data, uint32_t length);
uint8_t BootState_Load(BootStateRecord_t *record);
uint8_t BootState_GetDiagnostics(BootStateDiagnostics_t *diagnostics);
uint8_t BootState_Save(const BootStateRecord_t *record);
uint8_t BootState_Confirm(void);

#endif


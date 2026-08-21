#ifndef LOCKCTRL_BOOT_STATE_H
#define LOCKCTRL_BOOT_STATE_H
#include <stdint.h>
#include "boot_cfg.h"
#define BOOT_STATE_MAGIC 0x42535632U
#define BOOT_STATE_VERSION 3U
typedef enum { BOOT_STATE_NORMAL=0, BOOT_STATE_INSTALL_PENDING, BOOT_STATE_INSTALLING, BOOT_STATE_TRIAL, BOOT_STATE_CONFIRMED, BOOT_STATE_ROLLBACK } BootStateCode_t;
typedef struct { uint32_t magic; uint16_t version; uint16_t length; uint32_t sequence; uint32_t state; uint8_t stable_slot,candidate_slot,trial_fail_count,last_reset_reason; uint32_t image_size,image_crc32; char image_version[32]; uint32_t stable_image_size,stable_image_crc32; char stable_image_version[32]; uint32_t last_error; uint8_t rollback_report_pending,rollback_installed,reserved[2]; uint32_t record_crc32; } BootStateRecord_t;
uint32_t BootState_Crc32(const void*, uint32_t); uint8_t BootState_Load(BootStateRecord_t*); uint8_t BootState_Save(const BootStateRecord_t*); uint8_t BootState_Confirm(void);
void BootState_EnsureSeeded(const char* version);
#endif


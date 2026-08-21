#ifndef __BOOT_CFG_H__
#define __BOOT_CFG_H__

#include <stdint.h>

/* STM32F105RCT6: 256KB Flash */
#define FLASH_BASE_ADDR 0x08000000U
#define FLASH_TOTAL_SIZE (256U * 1024U)

/* Flash page size */
#define OTA_FLASH_PAGE_SIZE (2U * 1024U)

/* Boot region */
#define BOOT_START_ADDR 0x08000000U
#define BOOT_SIZE (32U * 1024U)
#define BOOT_END_ADDR (BOOT_START_ADDR + BOOT_SIZE - 1U)

/* Boot V2 state pages are isolated from the existing configuration page. */
#define BOOT_STATE_A_ADDR 0x0803E800U
#define BOOT_STATE_B_ADDR 0x0803F000U
#define BOOT_STATE_PAGE_SIZE OTA_FLASH_PAGE_SIZE

/* OTA request and device configuration remain in the last page. */
#define OTA_INFO_ADDR 0x0803F800U
#define OTA_INFO_PAGE_SIZE OTA_FLASH_PAGE_SIZE
#define OTA_INFO_END_ADDR (OTA_INFO_ADDR + OTA_INFO_PAGE_SIZE - 1U)

/* App region */
#define APP_START_ADDR 0x08008000U
#define APP_END_ADDR (BOOT_STATE_A_ADDR - 1U)
#define APP_MAX_SIZE (APP_END_ADDR - APP_START_ADDR + 1U)

/* Magic */
#define APP_VALID_MAGIC 0xA55A55AAU
#define OTA_REQ_MAGIC 0x5A5AA5A5U

#define OTA_DEFAULT_URL "https://ota.zxwl.ntiov.com:20443/fw/top_flying_wing_app.bin"

#define OTA_REQ_FAIL_MAX 3U

#endif

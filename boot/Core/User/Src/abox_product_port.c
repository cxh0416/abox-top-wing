#include "abox_product_port.h"

#include <string.h>

#include "abox_platform_port.h"
#include "ec_power.h"
#include "boot_cfg.h"
#include "boot_dbg.h"
#include "main.h"
#include "usart.h"

static const ABoxFlashLayout g_flash_layout = {
    APP_START_ADDR,
    OTA_INFO_ADDR,
    OTA_FLASH_PAGE_SIZE
};

static uint32_t product_get_tick(void *context)
{
    (void)context;
    return HAL_GetTick();
}

static int product_uart_write(void *context, const uint8_t *data, uint32_t length)
{
    (void)context;
    if ((data == 0) || (length == 0U) || (length > 0xFFFFU)) return 0;
    return (HAL_UART_Transmit(&huart1, (uint8_t *)data, (uint16_t)length, 1000U) == HAL_OK) ? 1 : 0;
}

static int product_flash_begin(void *context)
{
    (void)context;
    return (HAL_FLASH_Unlock() == HAL_OK) ? 1 : 0;
}

static int product_flash_write(void *context, uint32_t address, const uint8_t *data, uint32_t length)
{
    uint32_t word;

    (void)context;
    if ((data == 0) || (length != 4U)) return 0;
    memcpy(&word, data, sizeof(word));
    return (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, word) == HAL_OK) ? 1 : 0;
}

static int product_flash_erase_page(void *context, uint32_t address)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error = 0U;

    (void)context;
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = address;
    erase.NbPages = 1U;
    return (HAL_FLASHEx_Erase(&erase, &page_error) == HAL_OK) ? 1 : 0;
}

static void product_flash_end(void *context)
{
    (void)context;
    (void)HAL_FLASH_Lock();
}

static void product_enter_critical(void *context)
{
    (void)context;
    __disable_irq();
}

static void product_exit_critical(void *context)
{
    (void)context;
    __enable_irq();
}

static void product_log_write(void *context, const char *line)
{
    (void)context;
    if (line) boot_printf("%s", line);
}

static void product_ec_power_write(void *context, uint8_t level)
{
    (void)context;
    HAL_GPIO_WritePin(EN_3_8V_GPIO_Port, EN_3_8V_Pin, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void product_ec_pwrkey_write(void *context, uint8_t level)
{
    (void)context;
    HAL_GPIO_WritePin(EC_PWR_GPIO_Port, EC_PWR_Pin, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void ABox_ProductPlatform_Init(void)
{
    static const ABoxPlatformPort port = {
        0,
        product_get_tick,
        product_uart_write,
        product_flash_begin,
        product_flash_write,
        product_flash_erase_page,
        product_flash_end,
        product_enter_critical,
        product_exit_critical,
        product_log_write,
        0,
        &g_flash_layout,
        product_ec_power_write,
        product_ec_pwrkey_write
    };
    static const ABoxEcPowerConfig ec_power_config = {
        1000U, 1000U, 2000U, 3000U, 0U
    };

    if (!ABox_PlatformPortBind(&port)) boot_printf("[PLATFORM] port bind failed\r\n");
    if (!ABox_EcPower_SetConfig(&ec_power_config)) boot_printf("[PLATFORM] ec power config failed\r\n");
}

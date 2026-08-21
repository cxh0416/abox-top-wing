#include "boot_jump.h"

#include "boot_cfg.h"
#include "boot_dbg.h"
#include "main.h"

typedef void (*pFunction)(void);

uint8_t Boot_IsAppValid(uint32_t app_addr)
{
    uint32_t app_sp = *(volatile uint32_t*)app_addr;
    uint32_t app_reset = *(volatile uint32_t*)(app_addr + 4U);

    boot_printf("[BOOT] app check: addr=0x%08lX sp=0x%08lX reset=0x%08lX\r\n", app_addr, app_sp, app_reset);

    if ((app_sp & 0x2FFE0000U) != 0x20000000U)
    {
        boot_printf("[BOOT] invalid sp\r\n");
        return 0;
    }

    if (app_reset < app_addr || app_reset > APP_END_ADDR)
    {
        boot_printf("[BOOT] invalid reset\r\n");
        return 0;
    }

    return 1;
}

void Boot_JumpToApp(uint32_t app_addr)
{
    uint32_t app_sp = *(volatile uint32_t*)app_addr;
    uint32_t app_reset = *(volatile uint32_t*)(app_addr + 4U);
    pFunction app_entry = (pFunction)app_reset;

    __disable_irq();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    for (uint32_t i = 0; i < 8; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    HAL_RCC_DeInit();
    HAL_DeInit();

    SCB->VTOR = app_addr;
    __DSB();
    __ISB();

    __set_MSP(app_sp);

    /* 鍏抽敭锛氳烦杞墠鎭㈠鍏ㄥ眬涓柇 */
    // __enable_irq();

    app_entry();

    while (1) {}
}

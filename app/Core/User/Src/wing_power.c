#include "wing_power.h"
#include "main.h"

static uint8_t power_on;

void WingPower_Set(uint8_t enabled)
{
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    /* Both outputs belong to GPIOA; update them together with one write. */
    HAL_GPIO_WritePin(PVIN_OUT2_GPIO_Port, PVIN_OUT2_Pin | PVIN_OUT3_Pin,
                      enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
    power_on = enabled ? 1U : 0U;
    __set_PRIMASK(irq);
}

void WingPower_Init(void) { WingPower_Set(1U); }
uint8_t WingPower_IsOn(void) { return power_on; }

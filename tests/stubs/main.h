#ifndef TEST_MAIN_H
#define TEST_MAIN_H

#include <stdint.h>

#define PVIN_OUT2_GPIO_Port ((void *)1)
#define PVIN_OUT2_Pin (1U << 1)
#define PVIN_OUT3_Pin (1U << 2)
#define GPIO_PIN_RESET 0U
#define GPIO_PIN_SET 1U
void HAL_GPIO_WritePin(void *port, uint16_t pin, uint8_t value);

uint32_t HAL_GetTick(void);
void NVIC_SystemReset(void);
#define HAL_GetUIDw0() (0x12345678U)
#define HAL_GetUIDw1() (0x9ABCDEF0U)
#define HAL_GetUIDw2() (0x13579BDFU)
#define __disable_irq() ((void)0)
#define __enable_irq() ((void)0)
#define __get_PRIMASK() (0U)
#define __set_PRIMASK(value) ((void)(value))

#endif

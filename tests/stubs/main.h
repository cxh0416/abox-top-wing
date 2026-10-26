#ifndef TEST_MAIN_H
#define TEST_MAIN_H

#include <stdint.h>

uint32_t HAL_GetTick(void);
void NVIC_SystemReset(void);
#define HAL_GetUIDw0() (0x12345678U)
#define HAL_GetUIDw1() (0x9ABCDEF0U)
#define __disable_irq() ((void)0)
#define __enable_irq() ((void)0)
#define __get_PRIMASK() (0U)
#define __set_PRIMASK(value) ((void)(value))

#endif

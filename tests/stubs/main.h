#ifndef TEST_MAIN_H
#define TEST_MAIN_H

#include <stdint.h>

uint32_t HAL_GetTick(void);
#define __disable_irq() ((void)0)
#define __enable_irq() ((void)0)

#endif

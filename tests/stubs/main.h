#ifndef TEST_MAIN_H
#define TEST_MAIN_H

#include <stdint.h>

uint32_t HAL_GetTick(void);
void NVIC_SystemReset(void);
#define HAL_GetUIDw0() (0x12345678U)
#define HAL_GetUIDw1() (0x9ABCDEF0U)
#define HAL_GetUIDw2() (0x13579BDFU)
#ifdef WING_TEST_IRQ_TRACKING
uint32_t WingTest_GetPrimask(void);
void WingTest_DisableIrq(void);
void WingTest_SetPrimask(uint32_t value);
#define __disable_irq() WingTest_DisableIrq()
#define __enable_irq() WingTest_SetPrimask(0U)
#define __get_PRIMASK() WingTest_GetPrimask()
#define __set_PRIMASK(value) WingTest_SetPrimask(value)
#else
#define __disable_irq() ((void)0)
#define __enable_irq() ((void)0)
#define __get_PRIMASK() (0U)
#define __set_PRIMASK(value) ((void)(value))
#endif

#endif

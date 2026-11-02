#ifndef ANTI_PINCH_TEST_HAL_H
#define ANTI_PINCH_TEST_HAL_H

#include <stdint.h>
#define __get_PRIMASK() (0U)
#define __disable_irq() ((void)0)
#define __set_PRIMASK(value) ((void)(value))

typedef struct { uint32_t unused; } GPIO_TypeDef;
extern GPIO_TypeDef test_gpioa, test_gpiob, test_gpioc, test_gpiod;
#define GPIOA (&test_gpioa)
#define GPIOB (&test_gpiob)
#define GPIOC (&test_gpioc)
#define GPIOD (&test_gpiod)
#define GPIO_PIN_0 (1U << 0)
#define GPIO_PIN_1 (1U << 1)
#define GPIO_PIN_2 (1U << 2)
#define GPIO_PIN_3 (1U << 3)
#define GPIO_PIN_4 (1U << 4)
#define GPIO_PIN_5 (1U << 5)
#define GPIO_PIN_6 (1U << 6)
#define GPIO_PIN_7 (1U << 7)
#define GPIO_PIN_8 (1U << 8)
#define GPIO_PIN_9 (1U << 9)
#define GPIO_PIN_13 (1U << 13)
#define GPIO_PIN_14 (1U << 14)
#define GPIO_PIN_15 (1U << 15)
#define GPIO_MODE_INPUT 0U
#define GPIO_MODE_OUTPUT_PP 1U
#define GPIO_NOPULL 0U
#define GPIO_PULLUP 1U
#define GPIO_SPEED_FREQ_LOW 0U

typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;
typedef struct {
    uint32_t Pin, Mode, Pull, Speed;
} GPIO_InitTypeDef;

void Test_EnableGpioClock(GPIO_TypeDef* port);
#define __HAL_RCC_GPIOA_CLK_ENABLE() Test_EnableGpioClock(GPIOA)
#define __HAL_RCC_GPIOB_CLK_ENABLE() Test_EnableGpioClock(GPIOB)
#define __HAL_RCC_GPIOC_CLK_ENABLE() Test_EnableGpioClock(GPIOC)
#define __HAL_RCC_GPIOD_CLK_ENABLE() Test_EnableGpioClock(GPIOD)
void HAL_GPIO_Init(GPIO_TypeDef* port, GPIO_InitTypeDef* config);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef* port, uint16_t pin);
void HAL_GPIO_WritePin(GPIO_TypeDef* port, uint16_t pin, GPIO_PinState value);

#endif

#ifndef TOP_WING_GD_GPIO_TEST_MAIN_H
#define TOP_WING_GD_GPIO_TEST_MAIN_H
#include <stdint.h>
#define GPIOA 1U
#define RESET 0U
uint32_t gpio_input_bit_get(uint32_t port, uint32_t pin);
void gpio_bit_set(uint32_t port, uint32_t pin);
void gpio_bit_reset(uint32_t port, uint32_t pin);
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t state);
#endif

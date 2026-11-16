#ifndef TOP_WING_GD_BOARD_CONFIG_H
#define TOP_WING_GD_BOARD_CONFIG_H
#include "abox_mcu_clock.h"
/* 8 MHz crystal / 2 * 27 = 108 MHz; APB1 = 54 MHz. */
#define TOP_WING_GD_CLOCK_CONFIG {ABOX_MCU_CLOCK_PLL_HSE, 8000000U, 2U, 27U, 1U, 2U, 1U, 0U, 0U}
#endif

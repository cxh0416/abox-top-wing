#ifndef TOP_WING_GD32_MAIN_H
#define TOP_WING_GD32_MAIN_H
#include "gd32f10x.h"
#include "abox_platform_port.h"
#include "abox_mcu.h"
static inline uint32_t HAL_GetTick(void) { return ABox_PortGetTickMs(); }
static inline uint32_t HAL_GetUIDw0(void) { return ABoxMcu_UidWord(0U); }
static inline uint32_t HAL_GetUIDw1(void) { return ABoxMcu_UidWord(1U); }
static inline uint32_t HAL_GetUIDw2(void) { return ABoxMcu_UidWord(2U); }
void Error_Handler(void);
#endif

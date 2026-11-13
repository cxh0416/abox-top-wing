#ifndef TOP_WING_GD32_CAN_H
#define TOP_WING_GD32_CAN_H
#include "abox_gd32_can_legacy.h"
int WingGdCan_Init(uint32_t first_bitrate);
uint32_t HAL_CAN_GetRxFifoFillLevel(CAN_HandleTypeDef *h, uint32_t fifo);
uint32_t HAL_CAN_GetState(CAN_HandleTypeDef *h);
#define HAL_CAN_STATE_LISTENING 1U
#endif

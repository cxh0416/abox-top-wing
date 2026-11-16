#ifndef TOP_WING_GD32_CAN_H
#define TOP_WING_GD32_CAN_H
#include <stdint.h>
#include "gd32f10x.h"

/* Product-local compatibility surface for the existing wing controller. */
typedef struct { void *Instance; uint32_t ErrorCode; } CAN_HandleTypeDef;
typedef struct { uint32_t StdId, ExtId, IDE, RTR, DLC, TransmitGlobalTime; } CAN_TxHeaderTypeDef;
typedef struct { uint32_t StdId, ExtId, IDE, RTR, DLC; } CAN_RxHeaderTypeDef;
#undef CAN1
#define CAN1 ((void *)(uintptr_t)CAN_BASE)
#define CAN2 ((void *)(uintptr_t)(CAN_BASE + 0x400U))
#define HAL_OK 0U
#define HAL_ERROR 1U
#define CAN_ID_STD 0U
#define CAN_ID_EXT 4U
#define CAN_RTR_DATA 0U
#define CAN_RTR_REMOTE 2U
#define CAN_RX_FIFO0 0U
#define CAN_TX_MAILBOX0 1U
#define CAN_TX_MAILBOX1 2U
#define CAN_TX_MAILBOX2 4U
#define HAL_CAN_STATE_LISTENING 1U
extern CAN_HandleTypeDef hcan1, hcan2;
int WingGdCan_Init(uint32_t first_bitrate);
uint32_t HAL_CAN_GetRxFifoFillLevel(CAN_HandleTypeDef *h, uint32_t fifo);
uint32_t HAL_CAN_GetState(CAN_HandleTypeDef *h);
uint32_t HAL_CAN_GetRxMessage(CAN_HandleTypeDef *h, uint32_t fifo,
                              CAN_RxHeaderTypeDef *header, uint8_t *data);
uint32_t HAL_CAN_AddTxMessage(CAN_HandleTypeDef *h, const CAN_TxHeaderTypeDef *header,
                              const uint8_t *data, uint32_t *mailbox);
uint32_t HAL_CAN_AbortTxRequest(CAN_HandleTypeDef *h, uint32_t mask);
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *h);
#endif

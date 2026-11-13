#ifndef WING_GD_CAN_TEST_PORT_H
#define WING_GD_CAN_TEST_PORT_H
#include <stdint.h>
typedef struct { void *Instance; uint32_t ErrorCode; } CAN_HandleTypeDef;
typedef struct { uint32_t StdId, ExtId, IDE, RTR, DLC, TransmitGlobalTime; } CAN_TxHeaderTypeDef;
typedef struct { uint32_t StdId, ExtId, IDE, RTR, DLC; } CAN_RxHeaderTypeDef;
#define CAN1 ((void *)1)
#define CAN2 ((void *)2)
#define CAN_ID_STD 0U
#define CAN_ID_EXT 4U
#define CAN_RTR_DATA 0U
#define CAN_RTR_REMOTE 2U
#define CAN_RX_FIFO0 0U
#define HAL_OK 0U
#define HAL_ERROR 1U
#define HAL_CAN_STATE_LISTENING 1U
extern CAN_HandleTypeDef hcan1, hcan2;
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t state);
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *h);
uint32_t HAL_CAN_GetRxMessage(CAN_HandleTypeDef *, uint32_t, CAN_RxHeaderTypeDef *, uint8_t *);
uint32_t HAL_CAN_GetRxFifoFillLevel(CAN_HandleTypeDef *, uint32_t);
uint32_t HAL_CAN_GetState(CAN_HandleTypeDef *);
uint32_t HAL_CAN_AddTxMessage(CAN_HandleTypeDef *, const CAN_TxHeaderTypeDef *, const uint8_t *, uint32_t *);
uint32_t HAL_CAN_AbortTxRequest(CAN_HandleTypeDef *, uint32_t);
int WingGdCan_Init(uint32_t first_bitrate);
#endif

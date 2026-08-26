#ifndef TEST_CAN_H
#define TEST_CAN_H

#include <stdint.h>

typedef struct { uint8_t instance; } CAN_HandleTypeDef;
typedef struct
{
    uint32_t ExtId;
    uint32_t IDE;
    uint32_t RTR;
    uint32_t DLC;
    uint32_t TransmitGlobalTime;
} CAN_TxHeaderTypeDef;
typedef struct
{
    uint32_t ExtId;
    uint32_t IDE;
    uint32_t DLC;
} CAN_RxHeaderTypeDef;
typedef struct
{
    uint32_t FilterBank;
    uint32_t FilterMode;
    uint32_t FilterScale;
    uint32_t FilterFIFOAssignment;
    uint32_t FilterActivation;
    uint32_t SlaveStartFilterBank;
} CAN_FilterTypeDef;

#define HAL_OK 0
#define HAL_ERROR 1
#define CAN_ID_EXT 1U
#define CAN_RTR_DATA 0U
#define CAN_FILTERMODE_IDMASK 0U
#define CAN_FILTERSCALE_32BIT 0U
#define CAN_FILTER_FIFO0 0U
#define CAN_RX_FIFO0 0U
#define CAN_IT_RX_FIFO0_MSG_PENDING 1U
#define ENABLE 1U
#define DISABLE 0U

extern CAN_HandleTypeDef hcan1;
extern CAN_HandleTypeDef hcan2;

int HAL_CAN_AddTxMessage(CAN_HandleTypeDef* hcan, CAN_TxHeaderTypeDef* header,
                         uint8_t data[8], uint32_t* mailbox);
int HAL_CAN_GetRxMessage(CAN_HandleTypeDef* hcan, uint32_t fifo,
                         CAN_RxHeaderTypeDef* header, uint8_t data[8]);
int HAL_CAN_ConfigFilter(CAN_HandleTypeDef* hcan, CAN_FilterTypeDef* filter);
int HAL_CAN_Start(CAN_HandleTypeDef* hcan);
int HAL_CAN_ActivateNotification(CAN_HandleTypeDef* hcan, uint32_t notification);

#endif

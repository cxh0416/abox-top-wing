#include <assert.h>
#include <string.h>
#include "main.h"
#include "ultrasonic_board.h"
#include "ultrasonic_alarm.h"
CAN_HandleTypeDef hcan1={1},hcan2={2};
static uint32_t tick;
static unsigned count, consumed;
static CAN_RxHeaderTypeDef headers[5];
static uint8_t payloads[5][8];
uint32_t HAL_GetTick(void) {return tick;}
uint32_t HAL_CAN_GetRxFifoFillLevel(CAN_HandleTypeDef* h, uint32_t fifo)
{assert(h==&hcan1 && fifo==CAN_RX_FIFO0);return count-consumed;}
int HAL_CAN_GetRxMessage(CAN_HandleTypeDef* h,uint32_t fifo,CAN_RxHeaderTypeDef* head,uint8_t* d)
{assert(h==&hcan1 && fifo==CAN_RX_FIFO0);*head=headers[consumed];memcpy(d,payloads[consumed++],8);return HAL_OK;}
int main(void)
{
    assert(!UltrasonicBoard_IsAlarm(0));
    UltrasonicBoard_Init();
    count=6;
    for(unsigned i=0;i<count;++i)
    {headers[i]=(CAN_RxHeaderTypeDef){.ExtId=WL300D_CAN_ID,.IDE=CAN_ID_EXT,.DLC=8};memset(payloads[i],0xFF,8);}
    headers[0].ExtId=1; headers[1].IDE=0; headers[2].RTR=CAN_RTR_REMOTE;headers[3].DLC=7;
    for(unsigned i=0;i<4;++i)payloads[i][0]=20;
    memset(payloads[4],0,8);
    UltrasonicBoard_OnCanRx(&hcan2);assert(!consumed); /* Preserve CAN2. */
    UltrasonicBoard_OnCanRx(&hcan1);assert(consumed==count && !UltrasonicBoard_IsAlarm(0));
    UltrasonicDiagnostics diag;
    UltrasonicBoard_CopyDiagnostics(&diag);
    assert(diag.received==6 && diag.accepted==2 && diag.seen && !diag.active);
    assert(diag.last_id==WL300D_CAN_ID && diag.last_dlc==8 && diag.last_extended);
    assert(!diag.last_remote && diag.last_valid_data[7]==0xFF);
    consumed=0;count=1;headers[0]=headers[4];memset(payloads[0],0xFF,8);payloads[0][7]=30;tick=100;
    UltrasonicBoard_OnCanRx(&hcan1);assert(UltrasonicBoard_IsAlarm(101));
    UltrasonicBoard_CopyDiagnostics(&diag);
    assert(diag.received==7 && diag.accepted==3 && diag.mask==128 && diag.active);
    assert(diag.last_valid_tick==100 && diag.last_valid_data[7]==30);
    consumed=0;headers[0].IDE=0;headers[0].StdId=0x321;tick=200;
    UltrasonicBoard_OnCanRx(&hcan1);
    UltrasonicBoard_CopyDiagnostics(&diag);
    assert(diag.received==8 && diag.accepted==3 && diag.last_id==0x321 && !diag.last_extended);
    assert(diag.last_valid_tick==100 && diag.last_valid_data[7]==30);
    assert(UltrasonicBoard_IsAlarm(1099));
    assert(!UltrasonicBoard_IsAlarm(1100)); /* Silence clears after exactly one second. */
    assert(UltrasonicBoard_MotionBlocked(1100)); /* A short alarm cannot lose its stop event. */
    assert(UltrasonicBoard_TakeTrigger());
    assert(!UltrasonicBoard_TakeTrigger() && !UltrasonicBoard_MotionBlocked(1100));
    return 0;
}

#include <assert.h>
#include "can.h"
uint8_t test_ultrasonic_blocked;
unsigned test_ultrasonic_aborts;
uint8_t UltrasonicBoard_MotionBlocked(uint32_t now)
{ (void)now; return test_ultrasonic_blocked; }
int HAL_CAN_AbortTxRequest(CAN_HandleTypeDef* hcan, uint32_t mailboxes)
{
    assert(hcan==&hcan2 && mailboxes==(CAN_TX_MAILBOX0|CAN_TX_MAILBOX1|CAN_TX_MAILBOX2));
    ++test_ultrasonic_aborts;
    return HAL_OK;
}

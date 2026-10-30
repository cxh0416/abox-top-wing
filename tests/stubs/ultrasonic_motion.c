#include <assert.h>
#include "can.h"
uint8_t test_ultrasonic_blocked;
uint8_t test_ultrasonic_suppressed;
uint8_t test_anti_pinch_scope;
unsigned test_ultrasonic_aborts;
void AntiPinchBoard_SetScopeEnabled(uint8_t enabled)
{ test_anti_pinch_scope = enabled ? 1U : 0U; }
void UltrasonicBoard_SetAlarmSuppressed(uint8_t suppressed)
{ test_ultrasonic_suppressed = suppressed ? 1U : 0U; }
uint8_t UltrasonicBoard_MotionBlocked(uint32_t now)
{ (void)now; return test_ultrasonic_blocked && !test_ultrasonic_suppressed; }
int HAL_CAN_AbortTxRequest(CAN_HandleTypeDef* hcan, uint32_t mailboxes)
{
    assert(hcan==&hcan2 && mailboxes==(CAN_TX_MAILBOX0|CAN_TX_MAILBOX1|CAN_TX_MAILBOX2));
    ++test_ultrasonic_aborts;
    return HAL_OK;
}

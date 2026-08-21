#ifndef WING_PROTOCOL_H
#define WING_PROTOCOL_H

#include <stdint.h>

#define WING_CAN_TX_ID 0x00000001U
#define WING_CAN_RX_ID 0x00000002U
#define WING_CAN_DLC 8U

typedef enum { WING_ACTION_NONE = 0, WING_ACTION_ASCEND, WING_ACTION_DESCEND, WING_ACTION_STOP } WingAction;
typedef enum { WING_FRAME_INVALID = 0, WING_FRAME_ACK, WING_FRAME_STATUS } WingFrameKind;

typedef struct
{
    WingFrameKind kind;
    WingAction action;
    uint8_t accepted;
    uint16_t position_mm;
    uint8_t fault_code;
    uint8_t moving;
} WingCanMessage;

uint8_t WingProtocol_EncodeAction(WingAction action, uint8_t out[WING_CAN_DLC]);
void WingProtocol_EncodeStatusQuery(uint8_t out[WING_CAN_DLC]);
uint8_t WingProtocol_Decode(uint32_t ext_id, uint8_t dlc, const uint8_t data[WING_CAN_DLC], WingCanMessage* out);
const char* WingAction_Name(WingAction action);

#endif

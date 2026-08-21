#include "wing_protocol.h"

#include <string.h>

static uint8_t bytes_for_action(WingAction action, uint8_t* b0, uint8_t* b1)
{
    if (action == WING_ACTION_ASCEND) { *b0 = 'U'; *b1 = 'P'; return 1U; }
    if (action == WING_ACTION_DESCEND) { *b0 = 'D'; *b1 = 'N'; return 1U; }
    if (action == WING_ACTION_STOP) { *b0 = 'S'; *b1 = 'T'; return 1U; }
    return 0U;
}

uint8_t WingProtocol_EncodeAction(WingAction action, uint8_t out[WING_CAN_DLC])
{
    uint8_t b0, b1;
    if (!out || !bytes_for_action(action, &b0, &b1)) return 0U;
    memset(out, 0, WING_CAN_DLC);
    out[0] = b0;
    out[1] = b1;
    return 1U;
}

void WingProtocol_EncodeStatusQuery(uint8_t out[WING_CAN_DLC])
{
    if (!out) return;
    memset(out, 0, WING_CAN_DLC);
    out[0] = 'R';
    out[1] = 'D';
}

uint8_t WingProtocol_Decode(uint32_t ext_id, uint8_t dlc, const uint8_t data[WING_CAN_DLC], WingCanMessage* out)
{
    WingAction action = WING_ACTION_NONE;
    uint8_t b0, b1;
    if (!data || !out || ext_id != WING_CAN_RX_ID || dlc != WING_CAN_DLC) return 0U;
    memset(out, 0, sizeof(*out));
    if (data[0] == 'R' && data[1] == 'D')
    {
        if (data[4] > 16U || data[5] > 1U || data[7] > 1U) return 0U;
        out->kind = WING_FRAME_STATUS;
        out->position_mm = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
        out->fault_code = data[4];
        out->moving = data[5];
        out->accepted = (data[7] == 0U) ? 1U : 0U;
        return 1U;
    }
    for (action = WING_ACTION_ASCEND; action <= WING_ACTION_STOP; action = (WingAction)(action + 1))
    {
        if (bytes_for_action(action, &b0, &b1) && data[0] == b0 && data[1] == b1)
        {
            if (data[7] > 1U) return 0U;
            out->kind = WING_FRAME_ACK;
            out->action = action;
            out->accepted = (data[7] == 0U) ? 1U : 0U;
            return 1U;
        }
    }
    return 0U;
}

const char* WingAction_Name(WingAction action)
{
    if (action == WING_ACTION_ASCEND) return "ascend";
    if (action == WING_ACTION_DESCEND) return "descend";
    if (action == WING_ACTION_STOP) return "stop";
    return "";
}

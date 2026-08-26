#include <assert.h>
#include <string.h>
#include "wing_protocol.h"
#include "wing_state.h"

int main(void)
{
    uint8_t b[8];
    WingCanMessage m;
    WingState s;

    assert(WingProtocol_EncodeAction(WING_ACTION_ASCEND, b) && b[0] == 'U' && b[1] == 'P');
    assert(WingProtocol_EncodeAction(WING_ACTION_DESCEND, b) && b[0] == 'D' && b[1] == 'N');
    assert(WingProtocol_EncodeAction(WING_ACTION_STOP, b) && b[0] == 'S' && b[1] == 'T');
    WingProtocol_EncodeStatusQuery(b); assert(b[0] == 'R' && b[1] == 'D');
    WingProtocol_EncodeLimitQuery(b); assert(b[0] == 'R' && b[1] == 'L');

    memset(b, 0, sizeof(b)); b[0] = 'R'; b[1] = 'D'; b[2] = 1; b[3] = 2; b[5] = 1;
    assert(!WingProtocol_Decode(2, 8, b, &m));
    assert(!WingProtocol_Decode(1, 7, b, &m));
    assert(WingProtocol_Decode(1, 8, b, &m) && m.position_mm == 258 && m.moving);
    memset(b, 0, sizeof(b)); b[0] = 'R'; b[1] = 'L'; b[3] = 100; b[5] = 10;
    assert(WingProtocol_Decode(1, 8, b, &m) && m.top_limit_mm == 100 && m.bottom_limit_mm == 10);

    WingState_Init(&s);
    assert(s.snapshot.can_session == WING_CAN_SESSION_IDLE_SILENT);
    assert(s.snapshot.motion_state == WING_MOTION_UNKNOWN);
    assert(!s.snapshot.controller_online_known && !s.snapshot.status_fresh);
    assert(WingState_AdmissionCode(&s, WING_ACTION_ASCEND) == 0U);

    WingState_SetSession(&s, WING_CAN_SESSION_PREFLIGHT, WING_PHASE_CHECKING_MOTION,
                         WING_CONTROL_OWNER_NONE);
    WingState_ApplyFeedback(&s, 100U, 10U, 0U, 0U);
    assert(s.snapshot.controller_online_known && s.snapshot.controller_online);
    assert(s.snapshot.status_fresh && s.snapshot.motion_state == WING_MOTION_STOPPED);
    assert(!WingState_ApplyLimits(&s, 9U, 10U));
    assert(WingState_ApplyLimits(&s, 100U, 10U));
    assert(s.snapshot.at_endpoint == WING_ENDPOINT_BOTTOM);
    assert(WingState_EndpointReached(&s, WING_ACTION_DESCEND));

    WingState_AcceptDirection(&s, WING_ACTION_ASCEND, "r1", 100U);
    WingState_ApplyFeedback(&s, 120U, 97U, 0U, 1U);
    assert(s.snapshot.motion_state == WING_MOTION_ASCENDING);
    assert(!WingState_EndpointReached(&s, WING_ACTION_ASCEND));
    WingState_ApplyFeedback(&s, 121U, 98U, 0U, 1U);
    assert(WingState_EndpointReached(&s, WING_ACTION_ASCEND));
    assert(!strcmp(WingEndpoint_Name(s.snapshot.at_endpoint), "top"));
    assert(WingState_AdmissionCode(&s, WING_ACTION_DESCEND) == 4003U);
    WingState_Tick(&s, 622U);
    assert(s.snapshot.safety_reason == WING_SAFETY_FEEDBACK_TIMEOUT);

    WingState_SetIdleSilent(&s, WING_CONTROL_OWNER_NONE);
    assert(!s.snapshot.status_fresh && !s.snapshot.controller_online_known);
    assert(s.snapshot.motion_state == WING_MOTION_UNKNOWN);
    assert(s.snapshot.have_position && s.snapshot.have_limits);
    assert(WingState_AdmissionCode(&s, WING_ACTION_ASCEND) == 0U);

    WingState_SetSession(&s, WING_CAN_SESSION_PREFLIGHT, WING_PHASE_CHECKING_MOTION,
                         WING_CONTROL_OWNER_NONE);
    WingState_ApplyFeedback(&s, 0xFFFFFFF0U, 30U, 0U, 0U);
    WingState_Tick(&s, 0x20U);
    assert(s.snapshot.feedback_age_ms == 48U);

    WingState_AcceptDirection(&s, WING_ACTION_ASCEND, "max", 1U);
    WingState_ApplyFeedback(&s, 1U, 30U, 0U, 1U);
    WingState_ApplyFeedback(&s, 120001U, 90U, 0U, 1U);
    WingState_Tick(&s, 120001U);
    assert(s.snapshot.safety_reason == WING_SAFETY_MAX_RUNTIME);
    return 0;
}

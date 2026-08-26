#include <assert.h>
#include <string.h>
#include "wing_protocol.h"
#include "wing_state.h"

int main(void)
{
    uint8_t b[8]; WingCanMessage m; WingState s;
    assert(WingProtocol_EncodeAction(WING_ACTION_ASCEND,b)&&b[0]=='U'&&b[1]=='P');
    assert(WingProtocol_EncodeAction(WING_ACTION_DESCEND,b)&&b[0]=='D'&&b[1]=='N');
    assert(WingProtocol_EncodeAction(WING_ACTION_STOP,b)&&b[0]=='S'&&b[1]=='T');
    WingProtocol_EncodeStatusQuery(b);assert(b[0]=='R'&&b[1]=='D');
    WingProtocol_EncodeLimitQuery(b);assert(b[0]=='R'&&b[1]=='L');
    memset(b,0,sizeof(b));b[0]='R';b[1]='D';b[2]=1;b[3]=2;b[5]=1;
    assert(!WingProtocol_Decode(2,8,b,&m));assert(!WingProtocol_Decode(1,7,b,&m));
    assert(WingProtocol_Decode(1,8,b,&m)&&m.position_mm==258&&m.moving);
    memset(b,0,sizeof(b));b[0]='R';b[1]='L';b[2]=0;b[3]=100;b[4]=0;b[5]=10;
    assert(WingProtocol_Decode(1,8,b,&m)&&m.kind==WING_FRAME_LIMITS&&m.top_limit_mm==100&&m.bottom_limit_mm==10);
    WingState_Init(&s);assert(WingState_AdmissionCode(&s,WING_ACTION_ASCEND)==5002);
    WingState_ApplyFeedback(&s,100,10,0,0);assert(WingState_AdmissionCode(&s,WING_ACTION_ASCEND)==0);
    assert(!WingState_ApplyLimits(&s,9,10));assert(!s.snapshot.have_limits);
    assert(WingState_ApplyLimits(&s,100,10));assert(s.snapshot.have_limits&&s.snapshot.at_endpoint==WING_ENDPOINT_BOTTOM);
    assert(WingState_EndpointReached(&s,WING_ACTION_DESCEND));assert(!WingState_EndpointReached(&s,WING_ACTION_ASCEND));
    WingState_AcceptDirection(&s,WING_ACTION_ASCEND,"r1",100);assert(s.snapshot.active_action==WING_ACTION_ASCEND);
    WingState_ApplyFeedback(&s,120,97,0,1);assert(s.snapshot.motion_state==WING_MOTION_ASCENDING);
    assert(!WingState_EndpointReached(&s,WING_ACTION_ASCEND));
    WingState_ApplyFeedback(&s,121,98,0,1);assert(WingState_EndpointReached(&s,WING_ACTION_ASCEND));
    assert(s.snapshot.at_endpoint==WING_ENDPOINT_TOP&&!strcmp(WingEndpoint_Name(s.snapshot.at_endpoint),"top"));
    assert(WingState_AdmissionCode(&s,WING_ACTION_DESCEND)==4003);
    WingState_Tick(&s,622);assert(!s.snapshot.controller_online&&s.snapshot.safety_reason==WING_SAFETY_FEEDBACK_TIMEOUT);
    WingState_Init(&s);WingState_ApplyFeedback(&s,100,20,0,1);
    assert(WingState_FeedbackTimeoutMs(&s)==WING_IDLE_FEEDBACK_TIMEOUT_MS);
    WingState_Tick(&s,601);assert(s.snapshot.controller_online&&s.snapshot.motion_state==WING_MOTION_MOVING_UNKNOWN);
    WingState_Tick(&s,2601);assert(!s.snapshot.controller_online&&s.snapshot.safety_reason==WING_SAFETY_NONE);
    s.motion_started_tick=1;WingState_Tick(&s,120001);assert(s.snapshot.safety_reason==WING_SAFETY_NONE);
    WingState_ApplyFeedback(&s,700,12,0,0);WingState_ConfirmStop(&s,"r2",1);assert(s.snapshot.active_action==WING_ACTION_NONE);
    WingState_InvalidateLimits(&s);assert(!s.snapshot.have_limits&&s.snapshot.at_endpoint==WING_ENDPOINT_NONE);
    WingState_ApplyFeedback(&s,800,13,1,0);assert(WingState_AdmissionCode(&s,WING_ACTION_ASCEND)==5005);
    WingState_ApplyFeedback(&s,900,14,0,0);WingState_AcceptDirection(&s,WING_ACTION_ASCEND,"r3",900);
    assert(WingState_FeedbackTimeoutMs(&s)==WING_ACTIVE_FEEDBACK_TIMEOUT_MS);
    WingState_ApplyFeedback(&s,901,14,0,1);s.motion_started_tick=1;WingState_ApplyFeedback(&s,120001,14,0,1);WingState_Tick(&s,120001);assert(s.snapshot.safety_reason==WING_SAFETY_MAX_RUNTIME);

    /* A CAN ISR may update feedback after the task sampled its `now` tick. */
    WingState_Init(&s);WingState_ApplyFeedback(&s,101,20,0,1);WingState_AcceptDirection(&s,WING_ACTION_ASCEND,"race",101);
    WingState_Tick(&s,100);assert(s.snapshot.controller_online);assert(s.snapshot.feedback_age_ms==0U);
    assert(s.snapshot.run_elapsed_ms==0U);assert(s.snapshot.safety_reason==WING_SAFETY_NONE);

    /* Normal 32-bit HAL tick wrap must continue to produce a small age. */
    WingState_Init(&s);WingState_ApplyFeedback(&s,0xFFFFFFF0U,30,0,0);WingState_Tick(&s,0x20U);
    assert(s.snapshot.controller_online);assert(s.snapshot.feedback_age_ms==48U);
    return 0;
}

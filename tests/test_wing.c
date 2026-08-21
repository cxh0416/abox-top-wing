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
    memset(b,0,sizeof(b));b[0]='R';b[1]='D';b[2]=1;b[3]=2;b[5]=1;
    assert(!WingProtocol_Decode(1,8,b,&m));assert(!WingProtocol_Decode(2,7,b,&m));
    assert(WingProtocol_Decode(2,8,b,&m)&&m.position_mm==258&&m.moving);
    WingState_Init(&s);assert(WingState_AdmissionCode(&s,WING_ACTION_ASCEND)==5002);
    WingState_ApplyFeedback(&s,100,10,0,0);assert(WingState_AdmissionCode(&s,WING_ACTION_ASCEND)==0);
    WingState_AcceptDirection(&s,WING_ACTION_ASCEND,"r1",100);assert(s.snapshot.active_action==WING_ACTION_ASCEND);
    WingState_ApplyFeedback(&s,120,11,0,1);assert(s.snapshot.motion_state==WING_MOTION_ASCENDING);
    assert(WingState_AdmissionCode(&s,WING_ACTION_DESCEND)==4003);
    WingState_Tick(&s,621);assert(!s.snapshot.controller_online&&s.snapshot.safety_reason==WING_SAFETY_FEEDBACK_TIMEOUT);
    WingState_ApplyFeedback(&s,700,12,0,0);WingState_ConfirmStop(&s,"r2",1);assert(s.snapshot.active_action==WING_ACTION_NONE);
    WingState_ApplyFeedback(&s,800,13,1,0);assert(WingState_AdmissionCode(&s,WING_ACTION_ASCEND)==5005);
    WingState_ApplyFeedback(&s,900,14,0,0);WingState_AcceptDirection(&s,WING_ACTION_ASCEND,"r3",900);
    WingState_ApplyFeedback(&s,901,14,0,1);s.motion_started_tick=1;WingState_ApplyFeedback(&s,120001,14,0,1);WingState_Tick(&s,120001);assert(s.snapshot.safety_reason==WING_SAFETY_MAX_RUNTIME);
    return 0;
}

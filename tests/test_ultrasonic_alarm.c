#include <assert.h>
#include <string.h>
#include "ultrasonic_alarm.h"
static void frame(UltrasonicAlarm* s, uint32_t t, uint8_t* d)
{ assert(UltrasonicAlarm_OnFrame(s,t,WL300D_CAN_ID,1,0,8,d)); }
int main(void)
{
    UltrasonicAlarm s;
    uint8_t d[8];
    UltrasonicAlarm_Init(&s);
    assert(!UltrasonicAlarm_Update(&s,10000));
    memset(d,0xFF,8);
    frame(&s,0,d);
    assert(!s.active);
    /*
     * The module reports 0x00 for no valid obstacle, including during its
     * continuous heartbeat. Exercise that live wire value before testing
     * each channel with a positive distance.
     */
    memset(d,0x00,8); frame(&s,5,d);
    assert(!s.active && s.obstacle_mask==0);
    for (unsigned i=0;i<8;++i)
    {
        UltrasonicAlarm_Init(&s);
        memset(d,0xFF,8); d[i]=(uint8_t)(i+1U);
        frame(&s,10,d);
        assert(s.active && s.obstacle_mask==(1U<<i));
    }
    memset(d,30,8); frame(&s,20,d); assert(s.obstacle_mask==255);
    memset(d,0xFF,8); frame(&s,100,d);
    for(uint32_t t=200;t<=1000;t+=100) { frame(&s,t,d); assert(s.active); }
    assert(UltrasonicAlarm_Update(&s,1099));
    assert(!UltrasonicAlarm_Update(&s,1100));
    d[1]=40; frame(&s,1200,d);
    d[1]=0xFF; frame(&s,1300,d);
    d[2]=50; frame(&s,1500,d); /* Obstacle reappears during tail. */
    assert(s.active && !s.clearing);
    d[2]=0xFF; frame(&s,1600,d);
    assert(!UltrasonicAlarm_Update(&s,3000)); /* Explicit silence-timeout policy. */
    d[2]=50; frame(&s,3050,d); d[2]=0xFF;
    frame(&s,3100,d);
    for(uint32_t t=3200;t<=4000;t+=100) frame(&s,t,d);
    assert(UltrasonicAlarm_Update(&s,4099));
    assert(!UltrasonicAlarm_Update(&s,4100));
    /* Invalid/unrelated frames neither change state nor refresh timestamps. */
    uint32_t last=s.last_rx;
    assert(!UltrasonicAlarm_OnFrame(&s,4200,WL300D_CAN_ID+1,1,0,8,d));
    assert(!UltrasonicAlarm_OnFrame(&s,4200,WL300D_CAN_ID,0,0,8,d));
    assert(!UltrasonicAlarm_OnFrame(&s,4200,WL300D_CAN_ID,1,1,8,d));
    assert(!UltrasonicAlarm_OnFrame(&s,4200,WL300D_CAN_ID,1,0,7,d));
    assert(!UltrasonicAlarm_OnFrame(&s,4200,WL300D_CAN_ID,1,0,8,0));
    assert(s.last_rx==last && !s.active);
    /* Nonmatching traffic does not extend the one-second sonar alarm. */
    d[0]=30;frame(&s,5000,d);
    assert(!UltrasonicAlarm_OnFrame(&s,5900,WL300D_CAN_ID+1,1,0,8,d));
    assert(UltrasonicAlarm_Update(&s,5999));
    assert(!UltrasonicAlarm_Update(&s,6000));
    assert(s.obstacle_mask==1); /* Last observation remains available for diagnostics. */
    frame(&s,6100,d);
    assert(s.active); /* New obstacle data rearms without a power cycle. */
    frame(&s,7000,d);
    assert(UltrasonicAlarm_Update(&s,7999));
    assert(!UltrasonicAlarm_Update(&s,8000));
    /* Timers retain their semantics at uint32 wrap. */
    UltrasonicAlarm_Init(&s);
    uint32_t base=UINT32_MAX-400U;
    d[0]=30; frame(&s,base,d);
    d[0]=0xFF; frame(&s,base+100,d);
    for(uint32_t dt=200;dt<=1000;dt+=100) frame(&s,base+dt,d);
    assert(UltrasonicAlarm_Update(&s,base+1099));
    assert(!UltrasonicAlarm_Update(&s,base+1100));
    d[0]=30;frame(&s,base,d);
    assert(UltrasonicAlarm_Update(&s,base+999));
    assert(!UltrasonicAlarm_Update(&s,base+1000));
    return 0;
}

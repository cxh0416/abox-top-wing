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
    /* Both vendor no-target values and the installed module's zero distance
     * represent no reported obstacle. The blind zone is explicitly ignored. */
    memset(d,0x00,8); frame(&s,5,d);
    assert(!s.active && s.obstacle_mask==0);
    for(uint32_t t=105;t<=2005;t+=100) { frame(&s,t,d); assert(!s.active); }
    for (unsigned i=0;i<8;++i)
    {
        UltrasonicAlarm_Init(&s);
        memset(d,0xFF,8); d[i]=(uint8_t)(i+1U);
        frame(&s,10,d);
        assert(s.active && s.obstacle_mask==(1U<<i));
    }
    UltrasonicAlarm held;
    UltrasonicAlarm_Init(&held);
    memset(d,30,8);
    frame(&held,0,d);
    for (uint32_t t=100;t<=5000;t+=100)
    {
        frame(&held,t,d);
        assert(held.active);
    }
    memset(d,30,8); frame(&s,20,d); assert(s.obstacle_mask==255);
    memset(d,0xFF,8); frame(&s,100,d);
    for(uint32_t t=200;t<=1000;t+=100) { frame(&s,t,d); assert(s.active); }
    assert(UltrasonicAlarm_Update(&s,2099));
    assert(!UltrasonicAlarm_Update(&s,2100));
    memset(d,0x00,8); frame(&s,2100,d);
    d[1]=40; frame(&s,2200,d);
    memset(d,0x00,8); frame(&s,2300,d);
    d[2]=50; frame(&s,2500,d); /* Obstacle reappears during tail. */
    assert(s.active && !s.clearing);
    memset(d,0x00,8); frame(&s,2600,d);
    assert(UltrasonicAlarm_Update(&s,4599));
    assert(!UltrasonicAlarm_Update(&s,4600)); /* Two seconds after first clear frame. */
    memset(d,0xFF,8); d[2]=50; frame(&s,4700,d);
    memset(d,0x00,8); frame(&s,4800,d);
    for(uint32_t t=4900;t<=5700;t+=100) frame(&s,t,d);
    assert(UltrasonicAlarm_Update(&s,6799));
    assert(!UltrasonicAlarm_Update(&s,6800));
    /* Invalid/unrelated frames neither change state nor refresh timestamps. */
    uint32_t last=s.last_rx;
    assert(!UltrasonicAlarm_OnFrame(&s,6800,WL300D_CAN_ID+1,1,0,8,d));
    assert(!UltrasonicAlarm_OnFrame(&s,6800,WL300D_CAN_ID,0,0,8,d));
    assert(!UltrasonicAlarm_OnFrame(&s,6800,WL300D_CAN_ID,1,1,8,d));
    assert(!UltrasonicAlarm_OnFrame(&s,6800,WL300D_CAN_ID,1,0,7,d));
    assert(!UltrasonicAlarm_OnFrame(&s,6800,WL300D_CAN_ID,1,0,8,0));
    assert(s.last_rx==last && !s.active);
    /* Nonmatching traffic does not extend the two-second silence timeout. */
    d[0]=30;frame(&s,7000,d);
    assert(!UltrasonicAlarm_OnFrame(&s,7900,WL300D_CAN_ID+1,1,0,8,d));
    assert(UltrasonicAlarm_Update(&s,8999));
    assert(!UltrasonicAlarm_Update(&s,9000));
    assert(s.obstacle_mask==1); /* Last observation remains available for diagnostics. */
    frame(&s,9100,d);
    assert(s.active); /* New obstacle data rearms without a power cycle. */
    assert(UltrasonicAlarm_Update(&s,11099));
    assert(!UltrasonicAlarm_Update(&s,11100));
    /* Timers retain their semantics at uint32 wrap. */
    UltrasonicAlarm_Init(&s);
    uint32_t base=UINT32_MAX-400U;
    d[0]=30; frame(&s,base,d);
    d[0]=0x00; frame(&s,base+100,d);
    for(uint32_t dt=200;dt<=1000;dt+=100) frame(&s,base+dt,d);
    assert(UltrasonicAlarm_Update(&s,base+2099));
    assert(!UltrasonicAlarm_Update(&s,base+2100));
    d[0]=30;frame(&s,base+2200,d);
    assert(UltrasonicAlarm_Update(&s,base+4199));
    assert(!UltrasonicAlarm_Update(&s,base+4200));
    return 0;
}

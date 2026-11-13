#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wing_gd_can_test_port.h"
#include "abox_can_events.h"

static ABoxCanEventCallbacks cb[2];
static ABoxCanEvents *bound[2];
static int ready[2], fail_bus = -1, fail_bind = -1, stopped[2];
static uint32_t rates[2], irq_state;
static ABoxCanOptions options[2];
static ABoxCanFrame sent;
static uint8_t aborted;
static unsigned received[2];
uint32_t __get_PRIMASK(void) { return irq_state; }
void __disable_irq(void) { irq_state = 1U; }
void __set_PRIMASK(uint32_t state) { irq_state = state; }
int ABoxCanPort_InitWithOptions(unsigned bus, uint32_t rate, ABoxCanMode mode, const ABoxCanOptions *opts)
{ assert(bus < 2 && mode == ABOX_CAN_NORMAL); if ((int)bus == fail_bus) return 0; ready[bus]=1; rates[bus]=rate; options[bus]=*opts; return 1; }
int ABoxCanPort_Stop(unsigned bus) { assert(bus < 2); ready[bus]=0; ++stopped[bus]; return 1; }
int ABoxCanPort_IsReady(unsigned bus) { return bus < 2 && ready[bus]; }
int ABoxCanPort_BindEvents(unsigned bus, ABoxCanEvents *events, const ABoxCanEventCallbacks *callbacks)
{ if ((int)bus == fail_bind) return 0; assert(callbacks->receive && callbacks->complete); cb[bus]=*callbacks; bound[bus]=events; return 1; }
int ABoxCanPort_EnableEventIrq(unsigned bus, ABoxCanEvents *events) { return bound[bus] == events; }
int ABoxCanEvents_Send(ABoxCanEvents *events, const ABoxCanFrame *frame, uint8_t *mailbox)
{ assert(events == bound[1] && irq_state); sent=*frame; *mailbox=2U; return 1; }
int ABoxCanEvents_Abort(ABoxCanEvents *events, uint8_t mask)
{ assert(events == bound[1] && irq_state); aborted=mask; return 1; }
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *h)
{
    CAN_RxHeaderTypeDef header; uint8_t data[8];
    unsigned bus = h == &hcan1 ? 0U : 1U;
    assert(HAL_CAN_GetRxFifoFillLevel(h, CAN_RX_FIFO0) == 1U);
    assert(HAL_CAN_GetRxMessage(h, CAN_RX_FIFO0, &header, data) == HAL_OK);
    assert(header.IDE == CAN_ID_EXT && header.ExtId == 0x18ec0057U && header.DLC == 2U);
    assert(data[0] == 0x11 && data[1] == 0x22);
    assert(!HAL_CAN_GetRxFifoFillLevel(h, CAN_RX_FIFO0)); ++received[bus];
}
int main(void)
{
    fail_bus=1; assert(!WingGdCan_Init(250000U)); assert(stopped[0] == 1 && !ready[0]);
    fail_bus=-1; fail_bind=1; assert(!WingGdCan_Init(250000U)); assert(!ready[0] && !ready[1]);
    fail_bind=-1; assert(WingGdCan_Init(250000U));
    assert(rates[0] == 250000U && rates[1] == 500000U);
    assert(options[0].auto_retransmit && options[0].auto_bus_off_recovery && options[0].rx_fifo_overwrite);
    assert(options[1].auto_retransmit && !options[1].auto_bus_off_recovery && options[1].rx_fifo_overwrite);
    assert(HAL_CAN_GetState(&hcan1) == HAL_CAN_STATE_LISTENING);
    ABoxCanFrame frame = {.id=0x18ec0057U,.length=2U,.extended=1U,.data={0x11,0x22}};
    cb[0].receive(cb[0].context, &frame); cb[1].receive(cb[1].context, &frame);
    assert(received[0] == 1 && received[1] == 1);
    CAN_TxHeaderTypeDef header = {.ExtId=0x1234567U,.IDE=CAN_ID_EXT,.RTR=CAN_RTR_DATA,.DLC=2U};
    uint32_t mailbox=0; uint8_t data[2]={1,2};
    assert(HAL_CAN_AddTxMessage(&hcan2, &header, data, &mailbox) == HAL_OK && mailbox == 4U);
    assert(sent.id == header.ExtId && sent.extended && sent.length == 2U && !memcmp(sent.data,data,2));
    assert(!irq_state && HAL_CAN_AbortTxRequest(&hcan2, 7U) == HAL_OK && aborted == 7U && !irq_state);
    header.DLC=9U; assert(HAL_CAN_AddTxMessage(&hcan2,&header,data,&mailbox) == HAL_ERROR);
    puts("GD CAN: init, failure cleanup, RX, TX and abort passed");
    return 0;
}

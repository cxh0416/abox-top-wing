#ifdef TOP_WING_GD_CAN_TEST
#include "wing_gd_can_test_port.h"
#else
#include "can.h"
#endif
#include "abox_can_events.h"
#include <string.h>

CAN_HandleTypeDef hcan1 = {CAN1, 0U}, hcan2 = {CAN2, 0U};
typedef struct { unsigned bus; CAN_HandleTypeDef *handle; ABoxCanEvents events; ABoxCanFrame rx; uint8_t pending; } WingCan;
static WingCan ports[2] = {{.bus=0U,.handle=&hcan1},{.bus=1U,.handle=&hcan2}};
static WingCan *port(CAN_HandleTypeDef *h) { return h == &hcan1 ? &ports[0] : h == &hcan2 ? &ports[1] : 0; }
static void complete(void *context, uint8_t mailbox, ABoxCanTxResult result)
{ (void)context; (void)mailbox; (void)result; }
static void receive(void *context, const ABoxCanFrame *frame)
{
    WingCan *p = context;
    p->rx = *frame; p->pending = 1U;
    HAL_CAN_RxFifo0MsgPendingCallback(p->handle);
    p->pending = 0U;
}
int WingGdCan_Init(uint32_t first_bitrate)
{
    const ABoxCanOptions first = {1U, 1U, 1U}, second = {1U, 0U, 1U};
    if (!ABoxCanPort_InitWithOptions(0U, first_bitrate, ABOX_CAN_NORMAL, &first)) return 0;
    if (!ABoxCanPort_InitWithOptions(1U, 500000U, ABOX_CAN_NORMAL, &second)) {
        (void)ABoxCanPort_Stop(0U); return 0;
    }
    for (unsigned i = 0; i < 2U; ++i) {
        ABoxCanEventCallbacks callbacks = {0};
        callbacks.context = &ports[i]; callbacks.receive = receive; callbacks.complete = complete;
        if (!ABoxCanPort_BindEvents(i, &ports[i].events, &callbacks) ||
            !ABoxCanPort_EnableEventIrq(i, &ports[i].events)) {
            (void)ABoxCanPort_Stop(1U); (void)ABoxCanPort_Stop(0U); return 0;
        }
    }
    return 1;
}
uint32_t HAL_CAN_GetState(CAN_HandleTypeDef *h)
{ WingCan *p = port(h); return p && ABoxCanPort_IsReady(p->bus) ? HAL_CAN_STATE_LISTENING : 0U; }
uint32_t HAL_CAN_GetRxFifoFillLevel(CAN_HandleTypeDef *h, uint32_t fifo)
{ WingCan *p = port(h); return p && fifo == CAN_RX_FIFO0 ? p->pending : 0U; }
uint32_t HAL_CAN_GetRxMessage(CAN_HandleTypeDef *h, uint32_t fifo, CAN_RxHeaderTypeDef *header, uint8_t *data)
{
    WingCan *p = port(h);
    if (!p || !p->pending || fifo != CAN_RX_FIFO0 || !header || !data) return HAL_ERROR;
    memset(header, 0, sizeof(*header));
    header->IDE = p->rx.extended ? CAN_ID_EXT : CAN_ID_STD;
    header->RTR = p->rx.remote ? CAN_RTR_REMOTE : CAN_RTR_DATA;
    header->ExtId = p->rx.extended ? p->rx.id : 0U;
    header->StdId = p->rx.extended ? 0U : p->rx.id;
    header->DLC = p->rx.length; memcpy(data, p->rx.data, p->rx.length);
    p->pending = 0U; return HAL_OK;
}
uint32_t HAL_CAN_AddTxMessage(CAN_HandleTypeDef *h, const CAN_TxHeaderTypeDef *header, const uint8_t *data, uint32_t *mailbox)
{
    WingCan *p = port(h); ABoxCanFrame frame = {0}; uint8_t slot;
    if (!p || !header || !data || !mailbox || header->DLC > 8U ||
        (header->IDE != CAN_ID_STD && header->IDE != CAN_ID_EXT) ||
        (header->RTR != CAN_RTR_DATA && header->RTR != CAN_RTR_REMOTE)) return HAL_ERROR;
    frame.extended = header->IDE == CAN_ID_EXT; frame.remote = header->RTR == CAN_RTR_REMOTE;
    frame.id = frame.extended ? header->ExtId : header->StdId; frame.length = (uint8_t)header->DLC;
    memcpy(frame.data, data, frame.length);
    uint32_t key = __get_PRIMASK(); __disable_irq();
    int queued = ABoxCanEvents_Send(&p->events, &frame, &slot);
    __set_PRIMASK(key);
    if (queued != 1) return HAL_ERROR;
    *mailbox = 1U << slot; return HAL_OK;
}
uint32_t HAL_CAN_AbortTxRequest(CAN_HandleTypeDef *h, uint32_t mask)
{
    WingCan *p = port(h);
    if (!p || (mask & ~7U)) return HAL_ERROR;
    uint32_t key = __get_PRIMASK(); __disable_irq();
    int ok = ABoxCanEvents_Abort(&p->events, (uint8_t)mask);
    __set_PRIMASK(key);
    return ok ? HAL_OK : HAL_ERROR;
}

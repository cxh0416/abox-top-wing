#ifndef WING_CONTROLLER_H
#define WING_CONTROLLER_H

#include <stdint.h>
#include "can.h"
#include "wing_state.h"

#ifndef TOP_WING_CAN_BUS
#define TOP_WING_CAN_BUS 2
#endif

#define WING_ACTIVE_STATUS_POLL_MS 100U
#define WING_IDLE_STATUS_POLL_MS 1000U
#define WING_LOCAL_MOTION_PROBE_MS WING_MAX_RUNTIME_MS
#define WING_LIMIT_RETRY_MS 1000U
#define WING_COMMAND_TIMEOUT_MS 1000U

typedef enum
{
    WING_REPORT_NONE = 0,
    WING_REPORT_STATE_CHANGED,
    WING_REPORT_CONTROLLER_OFFLINE,
    WING_REPORT_CONTROLLER_ONLINE,
    WING_REPORT_FAULT_RAISED,
    WING_REPORT_FAULT_CLEARED,
    WING_REPORT_SAFETY_STOP,
    WING_REPORT_ENDPOINT_STOP
} WingReportReason;

typedef struct
{
    char request_id[40];
    WingAction action;
    uint16_t code;
    uint8_t accepted;
} WingControlResult;

void WingController_Init(void);
void WingController_Task(uint32_t now);
void WingController_OnCanRx(CAN_HandleTypeDef* hcan);
uint16_t WingController_Request(WingAction action, const char* request_id);
const WingSnapshot* WingController_GetSnapshot(void);
uint8_t WingController_TakeResult(WingControlResult* result);
WingReportReason WingController_TakeReportReason(void);
const char* WingReportReason_Name(WingReportReason reason);

#endif

#ifndef TEST_WING_CONTROLLER_H
#define TEST_WING_CONTROLLER_H

#include <stdint.h>
#include "wing_protocol.h"

#define WING_ACTIVE_FEEDBACK_TIMEOUT_MS 500U
#define WING_IDLE_FEEDBACK_TIMEOUT_MS 2500U
#define WING_COMMAND_TIMEOUT_MS 3000U
#define WING_MAX_RUNTIME_MS 120000U

typedef enum
{
    WING_MOTION_UNKNOWN = 0,
    WING_MOTION_STOPPED,
    WING_MOTION_ASCENDING,
    WING_MOTION_DESCENDING,
    WING_MOTION_MOVING_UNKNOWN,
    WING_MOTION_FAULT,
    WING_MOTION_OFFLINE
} WingMotionState;

typedef enum
{
    WING_SAFETY_NONE = 0,
    WING_SAFETY_FEEDBACK_TIMEOUT,
    WING_SAFETY_MAX_RUNTIME,
    WING_SAFETY_CONTROLLER_FAULT
} WingSafetyReason;

typedef enum
{
    WING_ENDPOINT_NONE = 0,
    WING_ENDPOINT_TOP,
    WING_ENDPOINT_BOTTOM
} WingEndpoint;

typedef struct
{
    WingMotionState motion_state;
    WingAction active_action;
    uint16_t position_mm;
    uint8_t have_position;
    uint16_t top_limit_mm;
    uint16_t bottom_limit_mm;
    uint8_t have_limits;
    WingEndpoint at_endpoint;
    uint8_t fault_code;
    uint8_t controller_online;
    uint32_t feedback_age_ms;
    uint8_t have_feedback;
    char last_request_id[40];
    uint32_t run_elapsed_ms;
    WingSafetyReason safety_reason;
} WingSnapshot;

typedef struct
{
    char request_id[40];
    WingAction action;
    uint16_t code;
} WingControlResult;

const WingSnapshot* WingController_GetSnapshot(void);
uint16_t WingController_Request(WingAction action, const char* request_id);
uint8_t WingController_TakeResult(WingControlResult* result);
uint8_t WingController_TakeReport(uint8_t* reason);
const char* WingController_ReportReasonName(uint8_t reason);
const char* WingMotionState_Name(WingMotionState state);
const char* WingSafetyReason_Name(WingSafetyReason reason);
const char* WingEndpoint_Name(WingEndpoint endpoint);

#endif

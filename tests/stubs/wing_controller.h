#ifndef TEST_WING_CONTROLLER_H
#define TEST_WING_CONTROLLER_H

#include <stdint.h>
#include "wing_protocol.h"

#define WING_ACTIVE_FEEDBACK_TIMEOUT_MS 500U
#define WING_QUERY_TIMEOUT_MS 300U
#define WING_PREFLIGHT_QUIET_MS 250U
#define WING_COMMAND_TIMEOUT_MS 1000U
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

typedef enum
{
    WING_CAN_SESSION_IDLE_SILENT = 0,
    WING_CAN_SESSION_PREFLIGHT,
    WING_CAN_SESSION_REMOTE_RUNNING,
    WING_CAN_SESSION_STOPPING,
    WING_CAN_SESSION_FAULT
} WingCanSession;

typedef enum
{
    WING_CONTROL_OWNER_NONE = 0,
    WING_CONTROL_OWNER_REMOTE,
    WING_CONTROL_OWNER_LOCAL_SUSPECTED
} WingControlOwner;

typedef enum
{
    WING_PHASE_IDLE = 0,
    WING_PHASE_CHECKING_MOTION,
    WING_PHASE_QUIET_GUARD,
    WING_PHASE_CHECKING_LIMITS,
    WING_PHASE_STARTING,
    WING_PHASE_RUNNING,
    WING_PHASE_STOPPING
} WingOperationPhase;

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
    uint8_t controller_online_known;
    uint8_t status_fresh;
    uint32_t feedback_age_ms;
    uint8_t have_feedback;
    char last_request_id[40];
    uint32_t run_elapsed_ms;
    WingSafetyReason safety_reason;
    WingCanSession can_session;
    WingControlOwner control_owner;
    WingOperationPhase operation_phase;
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
const char* WingCanSession_Name(WingCanSession session);
const char* WingControlOwner_Name(WingControlOwner owner);
const char* WingOperationPhase_Name(WingOperationPhase phase);

#endif

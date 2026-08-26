#ifndef WING_STATE_H
#define WING_STATE_H

#include <stdint.h>
#include "wing_protocol.h"

#define WING_FEEDBACK_TIMEOUT_MS 500U
#define WING_MAX_RUNTIME_MS 120000U
#define WING_ENDPOINT_TOLERANCE_MM 2U

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
    WingSnapshot snapshot;
    uint32_t last_feedback_tick;
    uint32_t motion_started_tick;
    uint8_t raw_moving;
    uint8_t safety_stop_latched;
} WingState;

void WingState_Init(WingState* state);
void WingState_ApplyFeedback(WingState* state, uint32_t now, uint16_t position_mm, uint8_t fault_code, uint8_t moving);
uint8_t WingState_ApplyLimits(WingState* state, uint16_t top_limit_mm, uint16_t bottom_limit_mm);
void WingState_InvalidateLimits(WingState* state);
uint8_t WingState_EndpointReached(const WingState* state, WingAction action);
void WingState_Tick(WingState* state, uint32_t now);
uint16_t WingState_AdmissionCode(const WingState* state, WingAction action);
void WingState_AcceptDirection(WingState* state, WingAction action, const char* request_id, uint32_t now);
void WingState_ConfirmStop(WingState* state, const char* request_id, uint8_t remote);
void WingState_LatchSafety(WingState* state, WingSafetyReason reason);
uint8_t WingState_NeedsSafetyStop(const WingState* state);
const char* WingMotionState_Name(WingMotionState value);
const char* WingSafetyReason_Name(WingSafetyReason value);
const char* WingEndpoint_Name(WingEndpoint value);

#endif

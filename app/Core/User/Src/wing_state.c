#include "wing_state.h"

#include <string.h>

static void copy_request_id(char dst[40], const char* src)
{
    if (!src) return;
    strncpy(dst, src, 39U);
    dst[39] = '\0';
}

/*
 * CAN feedback is written from an interrupt while the main loop supplies its
 * previously sampled tick.  Treat a timestamp a few ticks in the future as
 * zero elapsed time.  The signed delta still preserves normal HAL tick wrap.
 */
static uint32_t elapsed_ms(uint32_t now, uint32_t since)
{
    int32_t delta = (int32_t)(now - since);
    return delta < 0 ? 0U : (uint32_t)delta;
}

static void refresh_motion(WingState* state)
{
    WingSnapshot* s = &state->snapshot;
    if (!s->controller_online) s->motion_state = WING_MOTION_OFFLINE;
    else if (s->fault_code) s->motion_state = WING_MOTION_FAULT;
    else if (!state->raw_moving) s->motion_state = WING_MOTION_STOPPED;
    else if (s->active_action == WING_ACTION_ASCEND) s->motion_state = WING_MOTION_ASCENDING;
    else if (s->active_action == WING_ACTION_DESCEND) s->motion_state = WING_MOTION_DESCENDING;
    else s->motion_state = WING_MOTION_MOVING_UNKNOWN;
}

static void refresh_endpoint(WingState* state)
{
    WingSnapshot* s = &state->snapshot;
    s->at_endpoint = WING_ENDPOINT_NONE;
    if (!s->have_position || !s->have_limits) return;
    if ((uint32_t)s->position_mm + WING_ENDPOINT_TOLERANCE_MM >= s->top_limit_mm)
        s->at_endpoint = WING_ENDPOINT_TOP;
    else if ((uint32_t)s->position_mm <= (uint32_t)s->bottom_limit_mm + WING_ENDPOINT_TOLERANCE_MM)
        s->at_endpoint = WING_ENDPOINT_BOTTOM;
}

void WingState_Init(WingState* state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->snapshot.motion_state = WING_MOTION_OFFLINE;
}

void WingState_ApplyFeedback(WingState* state, uint32_t now, uint16_t position_mm, uint8_t fault_code, uint8_t moving)
{
    uint8_t was_moving;
    if (!state) return;
    was_moving = state->raw_moving;
    state->last_feedback_tick = now;
    state->snapshot.have_feedback = 1U;
    state->snapshot.controller_online = 1U;
    state->snapshot.feedback_age_ms = 0U;
    state->snapshot.position_mm = position_mm;
    state->snapshot.have_position = 1U;
    state->snapshot.fault_code = fault_code;
    state->raw_moving = moving ? 1U : 0U;
    if (state->raw_moving && !was_moving) state->motion_started_tick = now;
    if (!state->raw_moving)
    {
        state->motion_started_tick = 0U;
        state->snapshot.run_elapsed_ms = 0U;
    }
    if (fault_code && (state->raw_moving || state->snapshot.active_action != WING_ACTION_NONE))
        WingState_LatchSafety(state, WING_SAFETY_CONTROLLER_FAULT);
    refresh_motion(state);
    refresh_endpoint(state);
}

uint8_t WingState_ApplyLimits(WingState* state, uint16_t top_limit_mm, uint16_t bottom_limit_mm)
{
    if (!state || bottom_limit_mm > top_limit_mm) return 0U;
    state->snapshot.top_limit_mm = top_limit_mm;
    state->snapshot.bottom_limit_mm = bottom_limit_mm;
    state->snapshot.have_limits = 1U;
    refresh_endpoint(state);
    return 1U;
}

void WingState_InvalidateLimits(WingState* state)
{
    if (!state) return;
    state->snapshot.top_limit_mm = 0U;
    state->snapshot.bottom_limit_mm = 0U;
    state->snapshot.have_limits = 0U;
    state->snapshot.at_endpoint = WING_ENDPOINT_NONE;
}

uint8_t WingState_EndpointReached(const WingState* state, WingAction action)
{
    const WingSnapshot* s;
    if (!state) return 0U;
    s = &state->snapshot;
    if (!s->have_position || !s->have_limits) return 0U;
    if (action == WING_ACTION_ASCEND)
        return (uint32_t)s->position_mm + WING_ENDPOINT_TOLERANCE_MM >= s->top_limit_mm ? 1U : 0U;
    if (action == WING_ACTION_DESCEND)
        return (uint32_t)s->position_mm <= (uint32_t)s->bottom_limit_mm + WING_ENDPOINT_TOLERANCE_MM ? 1U : 0U;
    return 0U;
}

void WingState_Tick(WingState* state, uint32_t now)
{
    uint32_t age;
    if (!state) return;
    if (!state->snapshot.have_feedback)
    {
        state->snapshot.controller_online = 0U;
        state->snapshot.feedback_age_ms = 0U;
        return;
    }
    age = elapsed_ms(now, state->last_feedback_tick);
    state->snapshot.feedback_age_ms = age;
    if (age > WING_FEEDBACK_TIMEOUT_MS)
    {
        if (state->raw_moving || state->snapshot.active_action != WING_ACTION_NONE)
            WingState_LatchSafety(state, WING_SAFETY_FEEDBACK_TIMEOUT);
        state->snapshot.controller_online = 0U;
        refresh_motion(state);
        return;
    }
    state->snapshot.controller_online = 1U;
    if (state->raw_moving && state->motion_started_tick)
    {
        state->snapshot.run_elapsed_ms = elapsed_ms(now, state->motion_started_tick);
        if (state->snapshot.run_elapsed_ms >= WING_MAX_RUNTIME_MS)
            WingState_LatchSafety(state, WING_SAFETY_MAX_RUNTIME);
    }
    refresh_motion(state);
}

uint16_t WingState_AdmissionCode(const WingState* state, WingAction action)
{
    const WingSnapshot* s;
    if (!state || action == WING_ACTION_NONE) return 4001U;
    s = &state->snapshot;
    if (action == WING_ACTION_STOP)
    {
        if (!s->controller_online) return 5002U;
        return 0U;
    }
    if (!s->controller_online) return 5002U;
    if (s->fault_code) return 5005U;
    if (s->motion_state == WING_MOTION_UNKNOWN) return 5003U;
    if (s->motion_state != WING_MOTION_STOPPED || s->active_action != WING_ACTION_NONE) return 4003U;
    return 0U;
}

void WingState_AcceptDirection(WingState* state, WingAction action, const char* request_id, uint32_t now)
{
    if (!state || (action != WING_ACTION_ASCEND && action != WING_ACTION_DESCEND)) return;
    state->snapshot.active_action = action;
    copy_request_id(state->snapshot.last_request_id, request_id);
    state->snapshot.safety_reason = WING_SAFETY_NONE;
    state->safety_stop_latched = 0U;
    if (state->raw_moving && !state->motion_started_tick) state->motion_started_tick = now;
    refresh_motion(state);
}

void WingState_ConfirmStop(WingState* state, const char* request_id, uint8_t remote)
{
    if (!state) return;
    state->snapshot.active_action = WING_ACTION_NONE;
    state->raw_moving = 0U;
    state->motion_started_tick = 0U;
    state->snapshot.run_elapsed_ms = 0U;
    state->safety_stop_latched = 0U;
    if (remote) copy_request_id(state->snapshot.last_request_id, request_id);
    refresh_motion(state);
}

void WingState_LatchSafety(WingState* state, WingSafetyReason reason)
{
    if (!state || reason == WING_SAFETY_NONE || state->safety_stop_latched) return;
    state->snapshot.safety_reason = reason;
    state->safety_stop_latched = 1U;
}

uint8_t WingState_NeedsSafetyStop(const WingState* state) { return state ? state->safety_stop_latched : 0U; }

const char* WingMotionState_Name(WingMotionState value)
{
    switch (value)
    {
        case WING_MOTION_STOPPED: return "stopped";
        case WING_MOTION_ASCENDING: return "ascending";
        case WING_MOTION_DESCENDING: return "descending";
        case WING_MOTION_MOVING_UNKNOWN: return "moving_unknown";
        case WING_MOTION_FAULT: return "fault";
        case WING_MOTION_OFFLINE: return "offline";
        default: return "unknown";
    }
}

const char* WingSafetyReason_Name(WingSafetyReason value)
{
    if (value == WING_SAFETY_FEEDBACK_TIMEOUT) return "feedback_timeout";
    if (value == WING_SAFETY_MAX_RUNTIME) return "max_runtime";
    if (value == WING_SAFETY_CONTROLLER_FAULT) return "controller_fault";
    return "";
}

const char* WingEndpoint_Name(WingEndpoint value)
{
    if (value == WING_ENDPOINT_TOP) return "top";
    if (value == WING_ENDPOINT_BOTTOM) return "bottom";
    return "";
}

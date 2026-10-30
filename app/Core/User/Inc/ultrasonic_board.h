#ifndef ULTRASONIC_BOARD_H
#define ULTRASONIC_BOARD_H
#include <stdint.h>
#include "can.h"
void UltrasonicBoard_Init(void);
void UltrasonicBoard_OnCanRx(CAN_HandleTypeDef* hcan);
/* Suppression hides the sonar output/stop path but keeps raw frame state current. */
void UltrasonicBoard_SetAlarmSuppressed(uint8_t suppressed);
uint8_t UltrasonicBoard_IsAlarm(uint32_t now);
uint8_t UltrasonicBoard_TakeTrigger(void);
uint8_t UltrasonicBoard_MotionBlocked(uint32_t now);
typedef struct {
    uint32_t received, accepted, last_id, last_frame_tick, last_valid_tick;
    uint8_t last_dlc, last_extended, last_remote, seen, mask, active, suppressed;
    uint8_t last_data[8], last_valid_data[8];
} UltrasonicDiagnostics;
void UltrasonicBoard_CopyDiagnostics(UltrasonicDiagnostics* out);
void UltrasonicMonitor_Task(uint32_t now);
#endif

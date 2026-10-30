#ifndef __TIME_BASE_H__
#define __TIME_BASE_H__

#include <stdint.h>
#include "abox_clock.h"

void TimeBase_Init(void);

/* 喂入 AT+CCLK? 返回的字符串 */
void TimeBase_UpdateFromCCLK(const char* line);

/* 获取毫秒时间戳 */
uint64_t TimeBase_GetMs(void);
/* Outbound timestamps use zero until platform time is trusted. */
uint64_t TimeBase_GetReportedMs(void);
uint8_t TimeBase_FormatUtcMs(char* out, uint16_t out_sz);
uint8_t TimeBase_FormatBeijing(char* out, uint16_t out_sz);
uint8_t TimeBase_FormatBeijingFromTick(uint32_t tick, char* out, uint16_t out_sz);

/* 是否已经同步 */
uint8_t TimeBase_IsValid(void);
ABoxClockSnapshot TimeBase_GetSnapshot(void);
uint16_t TimeBase_CheckExpiry(uint64_t expires_at);
int TimeBase_Synchronize(uint64_t server, uint32_t error, uint64_t sent, uint64_t received);
uint8_t TimeBase_IsTrusted(void);

#endif

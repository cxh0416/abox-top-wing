#ifndef __TIME_BASE_H__
#define __TIME_BASE_H__

#include <stdint.h>

void TimeBase_Init(void);

/* 喂入 AT+CCLK? 返回的字符串 */
void TimeBase_UpdateFromCCLK(const char* line);

/* 获取毫秒时间戳 */
uint64_t TimeBase_GetMs(void);
uint8_t TimeBase_FormatUtcMs(char* out, uint16_t out_sz);
uint8_t TimeBase_FormatBeijing(char* out, uint16_t out_sz);
uint8_t TimeBase_FormatBeijingFromTick(uint32_t tick, char* out, uint16_t out_sz);

/* 是否已经同步 */
uint8_t TimeBase_IsValid(void);

#endif

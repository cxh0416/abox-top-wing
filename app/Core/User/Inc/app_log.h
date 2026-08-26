#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

typedef enum
{
    APP_LOG_LEVEL_ERROR = 0,
    APP_LOG_LEVEL_WARN = 1,
    APP_LOG_LEVEL_INFO = 2,
    APP_LOG_LEVEL_DEBUG = 3
} AppLogLevel_t;

#define APP_LOG_MQTT_BATCH_MAX 6U
#define APP_LOG_MQTT_LINE_SIZE 144U

void AppLog_Init(void);
void AppLog_Clear(void);
void AppLog_Printf(AppLogLevel_t level, const char* tag, const char* fmt, ...);
uint16_t AppLog_Dump(char* out, uint16_t out_sz);
uint16_t AppLog_ReadMqttLine(char* out, uint16_t out_sz);
uint8_t AppLog_PeekMqttLines(char lines[][APP_LOG_MQTT_LINE_SIZE], uint8_t max_lines);
void AppLog_CommitMqttLines(void);
void AppLog_AbortMqttLines(void);
uint16_t AppLog_GetMqttPendingCount(void);
void AppLog_DebugTask(void);
void AppLog_SetLevel(AppLogLevel_t level);
AppLogLevel_t AppLog_GetLevel(void);

#define APP_LOGE(tag, fmt, ...) AppLog_Printf(APP_LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)
#define APP_LOGW(tag, fmt, ...) AppLog_Printf(APP_LOG_LEVEL_WARN, tag, fmt, ##__VA_ARGS__)
#define APP_LOGI(tag, fmt, ...) AppLog_Printf(APP_LOG_LEVEL_INFO, tag, fmt, ##__VA_ARGS__)
#define APP_LOGD(tag, fmt, ...) AppLog_Printf(APP_LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)

#endif

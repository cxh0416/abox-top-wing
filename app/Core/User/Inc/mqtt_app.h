#ifndef __MQTT_APP_H__
#define __MQTT_APP_H__

#include <stdint.h>
#include "abox_mqtt_v4.h"

#define MQTT_APP_PUBLIC_VERSION_STR ABOX_MQTT_V4_VERSION
#define MQTT_APP_INTERNAL_VERSION_STR "2.0"
#define MQTT_APP_JSON_BUF_SZ 1030U
#define MQTT_APP_SUB_TOPIC_COUNT 4U
#define MQTT_APP_FIRST_HEARTBEAT_DELAY_MS 3000U
#define MQTT_APP_HEARTBEAT_PERIOD_MS 5000U
#define MQTT_APP_MOTION_REPORT_PERIOD_MS 200U
#define MQTT_APP_MQTT_CONFIG_TRIAL_TIMEOUT_MS 60000U

void MQTT_App_Init(const char* device_id);
void MQTT_App_Task(void);
void MQTT_App_OnMessage(const char* topic, const char* payload);
uint8_t MQTT_App_CanRunBackground(uint32_t guard_ms);
const char* MQTT_App_GetCtrlTopic(void);
const char* MQTT_App_GetHeartbeatTopic(void);
const char* MQTT_App_GetInternalConfigTopic(void);
const char* MQTT_App_GetInternalOtaTopic(void);
const char* const* MQTT_App_GetSubTopics(void);
uint8_t MQTT_App_GetSubTopicCount(void);

#endif

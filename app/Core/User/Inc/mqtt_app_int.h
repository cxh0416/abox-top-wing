#ifndef __MQTT_APP_INT_H__
#define __MQTT_APP_INT_H__

#include <stdint.h>
#include "cJSON.h"

#include "mqtt_app.h"

extern char g_mqtt_app_device_id[64];
extern char g_mqtt_app_heartbeat_topic[128];
extern char g_mqtt_app_ctrl_topic[128];
extern char g_mqtt_app_ctrl_resp_topic[128];
extern char g_mqtt_app_status_report_topic[128];
extern char g_mqtt_app_internal_config_topic[128];
extern char g_mqtt_app_internal_config_resp_topic[128];
extern char g_mqtt_app_internal_ota_topic[128];
extern char g_mqtt_app_internal_ota_resp_topic[128];
extern char g_mqtt_app_log_topic[128];
extern uint32_t g_mqtt_app_last_hb_tick;
extern uint32_t g_mqtt_app_last_hb_ok_tick;
extern uint32_t g_mqtt_app_seq_no;
extern uint8_t g_mqtt_app_first_hb;
extern volatile uint8_t g_mqtt_app_hb_pending;
extern volatile uint8_t g_mqtt_app_resp_pending;
extern volatile uint8_t g_mqtt_app_resp_publish_active;
extern uint32_t g_mqtt_app_resp_pub_ok_seq;
extern uint32_t g_mqtt_app_resp_tick;
extern char g_mqtt_app_resp_topic[128];
extern char g_mqtt_app_resp_payload[MQTT_APP_JSON_BUF_SZ];
extern char g_mqtt_app_resp_request_id[40];
extern uint16_t g_mqtt_app_resp_code;

extern volatile uint8_t g_mqtt_app_ota_reset_pending;
extern uint32_t g_mqtt_app_ota_reset_tick;
extern uint32_t g_mqtt_app_ota_request_tick;

void MQTT_App_ResetResponseState(void);
void MQTT_App_PromoteQueuedResponse(void);
uint8_t MQTT_App_DropStalePendingResponse(uint32_t now, uint32_t stale_ms);
uint8_t MQTT_App_ScheduleMqttConfigTrial(const char* vid, const char* host, uint16_t port,
                                         const char* username, const char* password);
void MQTT_App_HandleCommandMessage(const char* topic, const char* payload);
void MQTT_App_HandleWingResultTask(void);
void MQTT_App_HandlePendingResponseTask(void);
void MQTT_App_HandleOtaServiceTask(void);
void MQTT_App_HandleHeartbeatTask(void);
void MQTT_App_HandleStatusReportTask(void);
void MQTT_App_HandleLogReportTask(void);
void MQTT_App_ResetReportState(void);
void MQTT_App_AddWingSnapshot(cJSON* object);

#endif

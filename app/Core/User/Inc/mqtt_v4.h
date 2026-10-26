#ifndef MQTT_V4_H
#define MQTT_V4_H

#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "abox_mqtt_v4.h"
#include "wing_controller.h"

#define MQTT_V4_REQUEST_ID_SIZE 65U

uint8_t MQTT_V4_ValidDeviceId(const char* value);
void MQTT_V4_Task(void);
void MQTT_V4_HandleRequest(const char* text);
void MQTT_V4_IdentityChanged(const char* device_id);
void MQTT_V4_HandleResult(const WingControlResult* result);
void MQTT_V4_ResetReports(void);
void MQTT_V4_ReportTask(void);
uint8_t MQTT_V4_AnnouncementReady(void);
void MQTT_V4_AddSnapshot(cJSON* payload, const WingSnapshot* snapshot);

#endif

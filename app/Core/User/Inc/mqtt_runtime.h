#ifndef MQTT_RUNTIME_H
#define MQTT_RUNTIME_H
#include "abox_mqtt_runtime.h"
uint8_t MqttRuntime_Init(void);
int MqttRuntime_ClearEnrollmentCandidate(void);
void MqttRuntime_Task(void);
int MqttRuntime_StopFirst(uint32_t now);
uint8_t MqttRuntime_IsTlsActive(void);
uint8_t MqttRuntime_NeedsModemRecovery(void);
uint8_t MqttRuntime_ApplyCurrentConfig(void);
uint8_t MqttRuntime_ApplyEndpoint(const char* host, uint16_t port, const char* user, const char* password, uint8_t tls);
#endif

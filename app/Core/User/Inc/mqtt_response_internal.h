#ifndef MQTT_RESPONSE_INTERNAL_H
#define MQTT_RESPONSE_INTERNAL_H
#include <stdint.h>
typedef enum { RESP_CTRL = 0, RESP_INTERNAL_CONFIG, RESP_INTERNAL_OTA } ResponseTopic;
const char* MQTT_Response_Topic(ResponseTopic topic);
void MQTT_Response_Reset(void);
uint8_t MQTT_Response_Queue(ResponseTopic topic, const char* payload, const char* request_id, uint16_t code);
uint8_t MQTT_Response_QueueOtaFailure(const char* payload, const char* request_id, uint16_t code);
#endif

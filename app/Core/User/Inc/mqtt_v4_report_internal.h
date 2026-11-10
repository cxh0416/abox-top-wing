#ifndef MQTT_V4_REPORT_INTERNAL_H
#define MQTT_V4_REPORT_INTERNAL_H
#include <stdint.h>
void MQTT_V4_QueuePowerReport(const char* request_id, uint8_t on);
int MQTT_V4_QueueSync(const char* id);
void MQTT_V4_ReportIdentityChanged(void);
void MQTT_V4_RetryResponses(void);
#ifdef TOP_WING_HOST_TEST
void MQTT_V4_ReportTestReset(void);
#endif
#endif

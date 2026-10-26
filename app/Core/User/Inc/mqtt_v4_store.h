#ifndef MQTT_V4_STORE_H
#define MQTT_V4_STORE_H

#include <stdint.h>
#include "abox_mqtt_idempotency.h"

#define MQTT_V4_IDEM_COUNT 9U
#define MQTT_V4_IDEM_RESPONSE_SIZE 256U
#define MQTT_V4_IDEM_WORKSPACE_SIZE (60U + MQTT_V4_IDEM_COUNT * (108U + MQTT_V4_IDEM_RESPONSE_SIZE))

/* Shared UFS engine. The product only supplies resources and the namespace. */
ABoxMqttIdempotency* MQTT_V4_Store(void);
void MQTT_V4_StoreTask(void);
void MQTT_V4_StoreOnModemReset(void);
uint8_t MQTT_V4_StoreBusy(void);
void MQTT_V4_StoreKey(const char* device_id, const char* request_id, char key[65]);
int MQTT_V4_StoreInitialized(void);
int MQTT_V4_StoreMarkInitialized(void);

/* Existing configuration page tail only; no Boot/App boundary changes. */
#define MQTT_V4_STORE_OFFSET 512U
#define MQTT_V4_STORE_SLOTS 9U

typedef struct
{
    uint64_t protect_until;
    uint8_t digest[32];
    char request_id[65];
    char device_id[33];
    uint8_t padding[2];
    uint32_t header_crc;
    uint32_t begin_commit;
    uint32_t code;
    uint64_t timestamp;
    uint32_t response_crc;
    uint32_t complete_commit;
} MqttV4Record;

/* Read-only compatibility with the previous Flash journal. No new entries. */
/* 0 absent, 1 reserved/uncertain, 2 completed, -1 damaged storage. */
int MQTT_V4_StoreFind(const char* device_id, const char* id, MqttV4Record* record, int* slot);
uint8_t MQTT_V4_StoreCanErase(void);

#endif

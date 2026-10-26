#ifndef MQTT_V4_STORE_H
#define MQTT_V4_STORE_H

#include <stdint.h>

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

/* 0 absent, 1 reserved/uncertain, 2 completed, -1 damaged storage. */
int MQTT_V4_StoreFind(const char* device_id, const char* id, MqttV4Record* record, int* slot);
int MQTT_V4_StoreBegin(const char* device_id, const char* id, const uint8_t digest[32], uint64_t expires_at);
int MQTT_V4_StoreComplete(int slot, uint16_t code, uint64_t timestamp);
uint8_t MQTT_V4_StoreCanErase(void);

#endif

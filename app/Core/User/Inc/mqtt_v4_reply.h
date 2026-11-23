#ifndef MQTT_V4_REPLY_H
#define MQTT_V4_REPLY_H

#include <stddef.h>
#include <stdint.h>

typedef enum { POWER_NONE = 0, POWER_ON = 1, POWER_OFF = 2 } PowerAction;

typedef struct
{
    uint64_t timestamp;
    uint16_t code;
    PowerAction power;
} MqttV4Reply;

#define MQTT_V4_REPLY_LENGTH 21U
#define MQTT_V4_REPLY_CAPACITY (MQTT_V4_REPLY_LENGTH + 1U)

const char* MQTT_V4_ReplyMessage(uint16_t code);
int MQTT_V4_ReplyEncode(char* out, size_t capacity, const char* id, const MqttV4Reply* reply);
int MQTT_V4_ReplyPack(char out[MQTT_V4_REPLY_CAPACITY], const MqttV4Reply* reply);
int MQTT_V4_ReplyUnpack(MqttV4Reply* reply, const char* record, size_t length);
int MQTT_V4_ReplyConvertLegacy(char out[MQTT_V4_REPLY_CAPACITY], const char* response);

#endif

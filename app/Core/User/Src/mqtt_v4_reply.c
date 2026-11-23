#include "mqtt_v4_reply.h"

#include <string.h>
#include "abox_cloud_v4.h"
#include "abox_mqtt_digest.h"

#define TIMESTAMP_DIGITS 16U
#define CODE_DIGITS 4U
#define MAX_TIMESTAMP 9007199254740991ULL
#define LEGACY_REPLY_CAPACITY 256U

_Static_assert(TIMESTAMP_DIGITS + CODE_DIGITS + 1U == MQTT_V4_REPLY_LENGTH, "V4 reply layout");

const char* MQTT_V4_ReplyMessage(uint16_t code)
{
    switch (code)
    {
        case 200U: return "operation accepted";
        case 4001U: return "invalid request";
        case 4002U: return "unsupported command";
        case 4003U: return "busy or trusted time unavailable";
        case 4005U: return "request expired";
        case 4006U: return "requestId conflict";
        case 5002U: return "controller unavailable";
        case 5003U: return "controller feedback not confirmed";
        case 5004U: return "controller rejected command";
        case 5005U: return "controller fault";
        default: return "internal error";
    }
}

int MQTT_V4_ReplyEncode(char* out, size_t capacity, const char* id, const MqttV4Reply* reply)
{
    cJSON* result = NULL;
    if (reply->code == 200U && reply->power != POWER_NONE)
    {
        result = cJSON_CreateObject();
        if (!result || !cJSON_AddBoolToObject(result, "powerOutputOn", reply->power == POWER_ON))
        { cJSON_Delete(result); return 0; }
    }
    const uint16_t code = ABoxCloudV4_Response(out, capacity, reply->timestamp, id, reply->code,
                                             MQTT_V4_ReplyMessage(reply->code), result);
    cJSON_Delete(result);
    return code == reply->code;
}

static void write_hex(char* out, size_t width, uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    while (width)
    {
        out[--width] = digits[value & 0xFU];
        value >>= 4U;
    }
}

static int read_hex(const char* text, size_t width, uint64_t* value)
{
    *value = 0U;
    for (size_t i = 0; i < width; ++i)
    {
        const char c = text[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a') + 10U;
        else return 0;
        *value = (*value << 4U) | digit;
    }
    return 1;
}

/* Fixed on-disk fields: timestamp hex, code hex, and result (0 none, 1 on, 2 off).
 * The response requestId comes from the request that matched the durable key. */
int MQTT_V4_ReplyPack(char out[MQTT_V4_REPLY_CAPACITY], const MqttV4Reply* reply)
{
    if (reply->timestamp > MAX_TIMESTAMP || !reply->code) return 0;
    write_hex(out, TIMESTAMP_DIGITS, reply->timestamp);
    write_hex(out + TIMESTAMP_DIGITS, CODE_DIGITS, reply->code);
    out[MQTT_V4_REPLY_LENGTH - 1U] = (char)('0' + (reply->code == 200U ? reply->power : POWER_NONE));
    out[MQTT_V4_REPLY_LENGTH] = '\0';
    return 1;
}

int MQTT_V4_ReplyUnpack(MqttV4Reply* reply, const char* record, size_t length)
{
    uint64_t timestamp, code;
    if (!record || length != MQTT_V4_REPLY_LENGTH ||
        !read_hex(record, TIMESTAMP_DIGITS, &timestamp) || timestamp > MAX_TIMESTAMP ||
        !read_hex(record + TIMESTAMP_DIGITS, CODE_DIGITS, &code) || !code) return 0;
    const char power = record[length - 1U];
    if (power < '0' || power > '2' || (code != 200U && power != '0')) return 0;
    *reply = (MqttV4Reply){timestamp, (uint16_t)code, (PowerAction)(power - '0')};
    return 1;
}

int MQTT_V4_ReplyConvertLegacy(char out[MQTT_V4_REPLY_CAPACITY], const char* response)
{
    cJSON* root = ABoxMqttDigest_Parse(response, strlen(response));
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const char* id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "requestId"));
    const cJSON* timestamp = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(data, "code");
    const cJSON* result = cJSON_GetObjectItemCaseSensitive(data, "result");
    const cJSON* power = cJSON_GetObjectItemCaseSensitive(result, "powerOutputOn");
    int converted = 0;
    if (id && cJSON_IsNumber(timestamp) && timestamp->valuedouble >= 0 &&
        timestamp->valuedouble <= (double)MAX_TIMESTAMP && cJSON_IsNumber(code) &&
        code->valuedouble >= 1 && code->valuedouble <= UINT16_MAX && (!power || cJSON_IsBool(power)))
    {
        const MqttV4Reply reply = {(uint64_t)timestamp->valuedouble, (uint16_t)code->valuedouble,
                                  power ? (cJSON_IsTrue(power) ? POWER_ON : POWER_OFF) : POWER_NONE};
        char rendered[LEGACY_REPLY_CAPACITY];
        converted = MQTT_V4_ReplyEncode(rendered, sizeof(rendered), id, &reply) &&
                    !strcmp(rendered, response) && MQTT_V4_ReplyPack(out, &reply);
    }
    cJSON_Delete(root);
    return converted;
}

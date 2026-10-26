#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <stdint.h>
#include "ota_info.h"

#define BOARD_CONFIG_DEFAULT_VID "V000001"
#define BOARD_CONFIG_DEFAULT_MQTT_HOST "36.153.162.171"
#define BOARD_CONFIG_DEFAULT_MQTT_PORT 19683U
#define BOARD_CONFIG_DEFAULT_MQTT_USER "zxwl"
#define BOARD_CONFIG_DEFAULT_MQTT_PASS "zxwl1234@"
#define BOARD_CONFIG_DEFAULT_DIAG_HOST "36.153.162.171"
#define BOARD_CONFIG_DEFAULT_DIAG_PORT 30080U
#define BOARD_CONFIG_DEFAULT_DIAG_ENABLED 0U
#define BOARD_CONFIG_DEFAULT_DIAG_LEVEL 2U

void BoardConfig_Load(void);
const BoardConfigStore_t* BoardConfig_Get(void);
uint8_t BoardConfig_Save(const BoardConfigStore_t* cfg);
void BoardConfig_ResetDefault(BoardConfigStore_t* cfg);
uint8_t BoardConfig_SetMqtt(const char* vid, const char* host, uint16_t port,
                            const char* username, const char* password, uint8_t tls_enabled);
uint8_t BoardConfig_SetDiagLog(uint8_t enabled, const char* host, uint16_t port, uint8_t level);
uint8_t BoardConfig_SetDiagBootOnce(uint8_t enabled);

#endif

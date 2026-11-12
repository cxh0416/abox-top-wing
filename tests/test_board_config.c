#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "board_config.h"

static BoardConfigStore_t stored;

uint8_t OTA_Info_LoadBoardConfig(BoardConfigStore_t *cfg)
{
    *cfg = stored;
    return 1U;
}

uint8_t OTA_Info_SaveBoardConfig(const BoardConfigStore_t *cfg)
{
    stored = *cfg;
    return 1U;
}

void AppLog_Printf(AppLogLevel_t level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    (void)format;
}

int main(void)
{
    BoardConfigStore_t cfg;

    memset(&stored, 0, sizeof(stored));
    BoardConfig_Load();
    assert(!BoardConfig_HasMqttCredentials());

    BoardConfig_ResetDefault(&cfg);
    strcpy(cfg.vid, "ZL63538");
    strcpy(cfg.mqtt_host, "broker.example");
    strcpy(cfg.mqtt_username, "test-user");
    strcpy(cfg.mqtt_password, "test-password");
    cfg.mqtt_port = 19683U;
    cfg.flags &= (uint16_t)~BOARD_CONFIG_FLAG_MQTT_TLS;
    assert(BoardConfig_Save(&cfg));
    BoardConfig_Load();
    assert(BoardConfig_HasMqttCredentials());
    assert(!strcmp(BoardConfig_Get()->vid, "ZL63538"));
    assert(!(BoardConfig_Get()->flags & BOARD_CONFIG_FLAG_MQTT_TLS));

    cfg.flags |= BOARD_CONFIG_FLAG_MQTT_TLS;
    assert(BoardConfig_Save(&cfg));
    BoardConfig_Load();
    assert(BoardConfig_HasMqttCredentials());
    assert(BoardConfig_Get()->flags & BOARD_CONFIG_FLAG_MQTT_TLS);

    cfg.mqtt_password[0] = '\0';
    assert(BoardConfig_Save(&cfg));
    BoardConfig_Load();
    assert(!BoardConfig_HasMqttCredentials());

    stored.checksum ^= 1U;
    BoardConfig_Load();
    assert(!BoardConfig_HasMqttCredentials());
    puts("Saved plaintext and TLS configurations retain identity; invalid or empty credentials require enrollment");
    return 0;
}

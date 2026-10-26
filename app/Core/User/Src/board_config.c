#include "board_config.h"

#include <stddef.h>
#include <string.h>

#include "app_log.h"

static BoardConfigStore_t g_board_cfg;
static BoardConfigStore_t g_board_cfg_work;

static void copy_text(char* dst, uint16_t size, const char* src)
{
    if (!dst || size == 0U) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, size - 1U);
    dst[size - 1U] = '\0';
}

static uint32_t checksum(const BoardConfigStore_t* cfg)
{
    const uint8_t* bytes = (const uint8_t*)cfg;
    uint32_t value = 0x13572468U;
    uint16_t i;
    if (!cfg) return 0U;
    for (i = 0U; i < sizeof(*cfg); ++i)
    {
        if (i >= offsetof(BoardConfigStore_t, checksum) &&
            i < offsetof(BoardConfigStore_t, checksum) + sizeof(cfg->checksum)) continue;
        value = (value << 5) ^ (value >> 27) ^ bytes[i];
    }
    return value;
}

void BoardConfig_ResetDefault(BoardConfigStore_t* cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = BOARD_CONFIG_MAGIC;
    cfg->version = BOARD_CONFIG_VERSION;
    cfg->length = sizeof(*cfg);
    copy_text(cfg->vid, sizeof(cfg->vid), BOARD_CONFIG_DEFAULT_VID);
    copy_text(cfg->mqtt_host, sizeof(cfg->mqtt_host), BOARD_CONFIG_DEFAULT_MQTT_HOST);
    cfg->mqtt_port = BOARD_CONFIG_DEFAULT_MQTT_PORT;
    copy_text(cfg->mqtt_username, sizeof(cfg->mqtt_username), BOARD_CONFIG_DEFAULT_MQTT_USER);
    copy_text(cfg->mqtt_password, sizeof(cfg->mqtt_password), BOARD_CONFIG_DEFAULT_MQTT_PASS);
    cfg->diag_log_enabled = BOARD_CONFIG_DEFAULT_DIAG_ENABLED;
    cfg->diag_log_level = BOARD_CONFIG_DEFAULT_DIAG_LEVEL;
    cfg->diag_log_port = BOARD_CONFIG_DEFAULT_DIAG_PORT;
    copy_text(cfg->diag_log_host, sizeof(cfg->diag_log_host), BOARD_CONFIG_DEFAULT_DIAG_HOST);
    cfg->checksum = checksum(cfg);
}

static uint8_t valid(const BoardConfigStore_t* cfg)
{
    return cfg && cfg->magic == BOARD_CONFIG_MAGIC && cfg->version == BOARD_CONFIG_VERSION &&
           cfg->length == sizeof(*cfg) && cfg->vid[0] && cfg->mqtt_host[0] && cfg->mqtt_port &&
           cfg->diag_log_level <= 3U && cfg->diag_log_port && cfg->checksum == checksum(cfg);
}

void BoardConfig_Load(void)
{
    OTA_Info_LoadBoardConfig(&g_board_cfg_work);
    if (valid(&g_board_cfg_work))
    {
        g_board_cfg = g_board_cfg_work;
        APP_LOGI("CFG", "loaded vid=%s", g_board_cfg.vid);
    }
    else
    {
        BoardConfig_ResetDefault(&g_board_cfg);
        APP_LOGW("CFG", "default config");
    }
}

const BoardConfigStore_t* BoardConfig_Get(void) { return &g_board_cfg; }

uint8_t BoardConfig_Save(const BoardConfigStore_t* cfg)
{
    if (!cfg) return 0U;
    g_board_cfg_work = *cfg;
    g_board_cfg_work.magic = BOARD_CONFIG_MAGIC;
    g_board_cfg_work.version = BOARD_CONFIG_VERSION;
    g_board_cfg_work.length = sizeof(g_board_cfg_work);
    if (g_board_cfg_work.diag_log_level > 3U) g_board_cfg_work.diag_log_level = BOARD_CONFIG_DEFAULT_DIAG_LEVEL;
    if (!g_board_cfg_work.diag_log_port) g_board_cfg_work.diag_log_port = BOARD_CONFIG_DEFAULT_DIAG_PORT;
    if (!g_board_cfg_work.diag_log_host[0])
        copy_text(g_board_cfg_work.diag_log_host, sizeof(g_board_cfg_work.diag_log_host), BOARD_CONFIG_DEFAULT_DIAG_HOST);
    g_board_cfg_work.checksum = checksum(&g_board_cfg_work);
    if (!OTA_Info_SaveBoardConfig(&g_board_cfg_work)) return 0U;
    g_board_cfg = g_board_cfg_work;
    APP_LOGI("CFG", "saved");
    return 1U;
}

uint8_t BoardConfig_SetMqtt(const char* vid, const char* host, uint16_t port,
                            const char* username, const char* password, uint8_t tls_enabled)
{
    g_board_cfg_work = g_board_cfg;
    if (vid) copy_text(g_board_cfg_work.vid, sizeof(g_board_cfg_work.vid), vid);
    if (host) copy_text(g_board_cfg_work.mqtt_host, sizeof(g_board_cfg_work.mqtt_host), host);
    if (port) g_board_cfg_work.mqtt_port = port;
    if (username) copy_text(g_board_cfg_work.mqtt_username, sizeof(g_board_cfg_work.mqtt_username), username);
    if (password) copy_text(g_board_cfg_work.mqtt_password, sizeof(g_board_cfg_work.mqtt_password), password);
    if (tls_enabled) g_board_cfg_work.flags |= BOARD_CONFIG_FLAG_MQTT_TLS;
    else g_board_cfg_work.flags &= (uint16_t)~BOARD_CONFIG_FLAG_MQTT_TLS;
    return BoardConfig_Save(&g_board_cfg_work);
}

uint8_t BoardConfig_SetDiagLog(uint8_t enabled, const char* host, uint16_t port, uint8_t level)
{
    g_board_cfg_work = g_board_cfg;
    g_board_cfg_work.diag_log_enabled = enabled ? 1U : 0U;
    if (host) copy_text(g_board_cfg_work.diag_log_host, sizeof(g_board_cfg_work.diag_log_host), host);
    if (port) g_board_cfg_work.diag_log_port = port;
    if (level <= 3U) g_board_cfg_work.diag_log_level = level;
    return BoardConfig_Save(&g_board_cfg_work);
}

uint8_t BoardConfig_SetDiagBootOnce(uint8_t enabled)
{
    g_board_cfg_work = g_board_cfg;
    if (enabled) g_board_cfg_work.flags |= BOARD_CONFIG_FLAG_DIAG_BOOT_ONCE;
    else g_board_cfg_work.flags &= (uint16_t)~BOARD_CONFIG_FLAG_DIAG_BOOT_ONCE;
    return BoardConfig_Save(&g_board_cfg_work);
}

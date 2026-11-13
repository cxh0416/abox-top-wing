#include "main.h"
#include "can.h"
#include "abox_board.h"
#include "abox_modem_port.h"
#include "app_log.h"
#include "app_version.h"
#include "board_config.h"
#include "boot_cfg.h"
#include "ec800_at.h"
#include "ec_power.h"
#include "mqtt_app.h"
#include "mqtt_core.h"
#include "mqtt_runtime.h"
#include "ota_service.h"
#include "time_base.h"
#include "wing_app_service.h"
#include "wing_controller.h"
#include "wing_enrollment.h"
#ifdef TOP_WING_ZHONGTIAN
#include "anti_pinch_board.h"
#include "ultrasonic_board.h"
#include "wing_power.h"
#include "mqtt_v4.h"
#endif
#include <stdio.h>
#include <string.h>
#ifndef WL300D_CAN_BITRATE
#define WL300D_CAN_BITRATE 500000U
#endif
#ifndef WL300D_CAN_BITRATE
#define WL300D_CAN_BITRATE 500000U
#endif

static MQTT_CoreConfig_t mqtt_config;
static Ec800AtCore_t at;
static uint8_t dma[1088], ring[1152];
static ABoxEc800Rx rx;
const char g_top_flying_wing_fw_version_marker[] = TOP_FLYING_WING_APP_FW_VERSION_MARKER;
const char g_top_wing_mcu_marker[] = "TOP_FLYING_WING_MCU=GD32F105RCT6";
const char *TopFlyingWing_AppFwVersion(void) { return TOP_FLYING_WING_APP_FW_VERSION; }
int fputc(int ch, FILE *f) { (void)f; return ch; }
static uint32_t tick(void *context) { (void)context; return HAL_GetTick(); }
static int write_at(void *context, const uint8_t *data, uint16_t length, uint32_t timeout)
{ (void)context; return ABoxModemPort_Write(data, length, timeout); }
static void modem_rx(uint16_t position) { (void)ABoxEc800Rx_OnDmaEvent(&rx, position); }
static void modem_error(void) { ABoxEc800Rx_RequestRestart(&rx); }
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *h)
{
#ifdef TOP_WING_ZHONGTIAN
    if (h == &hcan1) UltrasonicBoard_OnCanRx(h);
    else WingController_OnCanRx(h);
#else
    WingController_OnCanRx(h);
#endif
}
void WingGd_SysTick(void)
{
#ifdef TOP_WING_ZHONGTIAN
    AntiPinchBoard_Tick(HAL_GetTick());
#endif
}
void Error_Handler(void) { __disable_irq(); for (;;) {} }

int main(void)
{
    static const ABoxModemPortEvents modem_events = {modem_rx, modem_error};
    const ABoxEc800AtPort at_port = {0, tick, write_at, 0};
    if (!ABoxBoard_Init(APP_START_ADDR)) Error_Handler();
    AppLog_Init(); TimeBase_Init();
    APP_LOGI("APP", "%s", g_top_flying_wing_fw_version_marker);
    APP_LOGI("APP", "%s", g_top_wing_mcu_marker);
#ifdef TOP_WING_ZHONGTIAN
    UltrasonicBoard_Init(); AntiPinchBoard_Init();
#endif
    EC_Power_Init(); EC_Power_StartRestartSequence();
    int communication_ok = ABoxEc800Rx_Init(&rx, dma, sizeof(dma), ring, sizeof(ring));
    ABoxModemPort_SetEvents(&modem_events);
    BoardConfig_Load(); AppLog_SetLevel((AppLogLevel_t)BoardConfig_Get()->diag_log_level);
    /* Bind both CAN IRQ owners only after board and safety state are ready. */
    __disable_irq();
    if (!WingGdCan_Init(WL300D_CAN_BITRATE)) Error_Handler();
#ifdef TOP_WING_ZHONGTIAN
    if (!WingController_Init()) Error_Handler();
#else
    WingController_Init();
#endif
    __enable_irq();
    if (SysTick_Config(SystemCoreClock / 1000U)) Error_Handler();
    NVIC_SetPriority(SysTick_IRQn, 0U);
    const BoardConfigStore_t *cfg = BoardConfig_Get();
    memset(&mqtt_config, 0, sizeof(mqtt_config));
    mqtt_config.client_idx = 0U;
    strncpy(mqtt_config.server_host, cfg->mqtt_host, sizeof(mqtt_config.server_host)-1U);
    mqtt_config.server_port = cfg->mqtt_port;
    strncpy(mqtt_config.client_id, cfg->vid, sizeof(mqtt_config.client_id)-1U);
    strncpy(mqtt_config.username, cfg->mqtt_username, sizeof(mqtt_config.username)-1U);
    strncpy(mqtt_config.password, cfg->mqtt_password, sizeof(mqtt_config.password)-1U);
    communication_ok = EC800_AT_Init(&at, &at_port) && communication_ok;
    MQTT_Core_SetAtCore(&at); MQTT_Core_Init(&mqtt_config, MQTT_App_OnMessage);
    OtaService_Init(&at);
    if (!WingEnrollment_Init(&at)) APP_LOGE("ENROLL", "initialization failed");
    MQTT_App_Init(WingEnrollment_DeviceId());
    MQTT_Core_SetTopics(MQTT_App_GetSubTopics(), MQTT_App_GetSubTopicCount(), MQTT_App_GetHeartbeatTopic());
    communication_ok = MqttRuntime_Init() && communication_ok;
    communication_ok = communication_ok && WingAppService_Init(&at, &rx);
    if (!communication_ok) APP_LOGE("APP", "communication initialization failed");
#ifdef TOP_WING_ZHONGTIAN
    WingPower_Init();
#endif
    for (;;) {
#ifdef TOP_WING_ZHONGTIAN
        if (UltrasonicBoard_TakeTrigger()) { MQTT_V4_CancelPendingMotion(); WingController_UltrasonicStop(); }
        UltrasonicMonitor_Task(HAL_GetTick());
        uint8_t trigger = AntiPinchBoard_TakeTrigger();
        if (trigger) MQTT_V4_CancelPendingMotion();
        WingController_AntiPinch(AntiPinchBoard_IsPressed(), trigger);
        WingController_Task(HAL_GetTick());
        WingAppService_Poll(HAL_GetTick());
#else
        WingAppService_Poll(HAL_GetTick());
        if (!communication_ok) WingController_Task(HAL_GetTick());
#endif
    }
}

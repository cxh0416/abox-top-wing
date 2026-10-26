#include "wing_app_service.h"
#include "app_log.h"
#include "ec_power.h"
#include "main.h"
#include "mqtt_app.h"
#include "mqtt_core.h"
#include "mqtt_runtime.h"
#include "mqtt_v4_store.h"
#include "ota_service.h"
#include "usart.h"
#include "wing_enrollment.h"

static ABoxAppService service;
static ABoxEc800Recovery recovery;
static uint32_t started_at, reported_overflow;
static uint8_t enabled;

static int restart_rx(void *context)
{
    (void)context;
    if (HAL_UART_AbortReceive(&huart1) != HAL_OK) return 0;
    ABoxEc800Rx_AfterRestart(service.rx);
    return HAL_UARTEx_ReceiveToIdle_DMA(&huart1, service.rx->dma_buffer,
                                      service.rx->dma_size) == HAL_OK;
}
static int communication_enabled(void *context)
{
    (void)context;
    return enabled || (EC_Power_IsOnDone() && HAL_GetTick() - started_at >= 8000U);
}
static void after_at(void *context, uint32_t now)
{
    (void)context; (void)now;
    /* This callback runs only after RX rearm. The enabled predicate is also
     * queried on RX failures and must not acquire an OTA workspace there. */
    if (!enabled) {
        enabled = 1U;
        (void)OtaService_BeginProvisioning();
    }
}
static void ota(void *context, uint32_t now)
{ (void)context; (void)now; OtaService_Task(); }
static void before_enrollment(void *context, uint32_t now)
{ (void)context; (void)now; MQTT_Core_Task(); }
static void enrollment(void *context, uint32_t now)
{ (void)context; (void)now; WingEnrollment_Task(); }
static void cloud(void *context, uint32_t now)
{ (void)context; (void)now; MQTT_App_Task(); }
static int transport_error(void *context)
{ (void)context; return MqttRuntime_NeedsModemRecovery(); }
static int maintenance_busy(void *context)
{ (void)context; return MQTT_V4_StoreBusy(); }
static void event(void *context, ABoxAppServiceEvent value)
{
    (void)context;
    if (value == ABOX_APP_NETWORK_CHANGED) MQTT_Core_SetPaused(1U);
}
static const ABoxAppServicePort port = {
    .restart_rx = restart_rx, .enabled = communication_enabled, .after_at = after_at,
    .ota = ota, .before_enrollment = before_enrollment,
    .enrollment = enrollment, .cloud = cloud, .event = event,
    .transport_error = transport_error, .maintenance_busy = maintenance_busy,
    .recovery = &recovery, .recovery_after_cloud = 1U
};

int WingAppService_Init(ABoxEc800At *at, ABoxEc800Rx *rx)
{
    started_at = HAL_GetTick();
    enabled = 0U;
    reported_overflow = 0U;
    ABoxEc800Recovery_Init(&recovery, 30000U);
    return ABoxAppService_Init(&service, at, rx, &port);
}
void WingAppService_Poll(uint32_t now)
{
    ABoxAppService_Poll(&service, now);
    if (service.rx && ABoxEc800Rx_OverflowCount(service.rx) != reported_overflow) {
        reported_overflow = ABoxEc800Rx_OverflowCount(service.rx);
        APP_LOGE("AT", "USART1 deferred RX overflow=%lu", (unsigned long)reported_overflow);
    }
}

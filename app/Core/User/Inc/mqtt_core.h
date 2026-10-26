#ifndef __MQTT_CORE_H__
#define __MQTT_CORE_H__

#include <stdint.h>

#include "ec800_at.h"
#include "abox_mqtt_ec800_bridge.h"
#include "abox_mqtt_runtime.h"

typedef enum
{
    MQTT_CORE_ST_IDLE = 0,
    MQTT_CORE_ST_WAIT_BOOT,
    MQTT_CORE_ST_ECHO_OFF,
    MQTT_CORE_ST_WAIT_ECHO_OFF,
    MQTT_CORE_ST_CFG,
    MQTT_CORE_ST_WAIT_CFG,
    MQTT_CORE_ST_DIAG,
    MQTT_CORE_ST_WAIT_DIAG,
    MQTT_CORE_ST_NETWORK_QUERY,
    MQTT_CORE_ST_WAIT_NETWORK_QUERY,
    MQTT_CORE_ST_ATTACH_QUERY,
    MQTT_CORE_ST_WAIT_ATTACH_QUERY,
    MQTT_CORE_ST_ATTACH,
    MQTT_CORE_ST_WAIT_ATTACH,
    MQTT_CORE_ST_NETWORK_WAIT,
    MQTT_CORE_ST_PDP_QUERY,
    MQTT_CORE_ST_WAIT_PDP_QUERY,
    MQTT_CORE_ST_PDP_CONFIG,
    MQTT_CORE_ST_WAIT_PDP_CONFIG,
    MQTT_CORE_ST_PDP_ACTIVATE,
    MQTT_CORE_ST_WAIT_PDP_ACTIVATE,
    MQTT_CORE_ST_PDP_VERIFY,
    MQTT_CORE_ST_WAIT_PDP_VERIFY,
    MQTT_CORE_ST_OPEN,
    MQTT_CORE_ST_WAIT_OPEN,
    MQTT_CORE_ST_CONN,
    MQTT_CORE_ST_WAIT_CONN,
    MQTT_CORE_ST_SUB,
    MQTT_CORE_ST_WAIT_SUB,
    MQTT_CORE_ST_READY,
    MQTT_CORE_ST_RECOVER_PUB,
    MQTT_CORE_ST_RECONNECT_WAIT,
    MQTT_CORE_ST_ERROR
} MQTT_CoreState_t;

typedef struct
{
    uint8_t client_idx;
    uint8_t tls_enabled;
    uint8_t ssl_ctx_id;
    char server_host[64];
    uint16_t server_port;
    char client_id[64];
    char username[32];
    char password[32];
    char sub_topics[4][128];
    uint8_t sub_topic_count;
    char pub_topic[128];
} MQTT_CoreConfig_t;

typedef void (*MQTT_CoreMsgCallback_t)(const char* topic, const char* payload);
typedef void (*MQTT_CorePublishObserver_t)(uint64_t operation, ABoxMqttEc800PublishEvent event, void* user);

typedef struct
{
    MQTT_CoreState_t state;
    uint32_t state_tick;
    uint32_t last_rx_tick;
    uint32_t last_ready_tick;
    uint32_t pub_tick;
    uint8_t connected;
    uint8_t subscribed;
    uint8_t need_reconnect;
    uint8_t need_resub;
    uint8_t sub_index;
    uint8_t retry_in_state;
    uint8_t reconnect_count;
    uint8_t flag_cfg_ok;
    uint8_t flag_open_ok;
    uint8_t flag_conn_ok;
    uint8_t flag_sub_ok;
    uint8_t flag_closed;
    uint8_t flag_error;
    uint8_t diag_index;
    uint8_t network_registered;
    uint8_t packet_attached;
    uint8_t pdp_active;
    uint8_t pub_busy;
    uint8_t pub_wait_prompt;
    uint8_t pub_wait_result;
} MQTT_CoreRuntime_t;

void MQTT_Core_Init(const MQTT_CoreConfig_t* cfg, MQTT_CoreMsgCallback_t cb);
void MQTT_Core_SetAtCore(Ec800AtCore_t* at_core);
void MQTT_Core_Reset(void);
void MQTT_Core_SetPaused(uint8_t paused);
void MQTT_Core_Task(void);
void MQTT_Core_OnUartRx(uint8_t* data, uint16_t len);
void MQTT_Core_OnAtEvent(Ec800AtEvent_t event, const uint8_t* data, uint16_t len, void* user);
void MQTT_Core_RequestReconnect(void);
void MQTT_Core_ForceReconnect(void);
void MQTT_Core_UpdateBroker(const char* host, uint16_t port, const char* username, const char* password);
void MQTT_Core_RequestIccid(void);
void MQTT_Core_SetTopics(const char* const* sub_topics, uint8_t sub_topic_count, const char* pub_topic);
uint8_t MQTT_Core_IsReady(void);
uint8_t MQTT_Core_CanRunBackgroundAt(void);
MQTT_CoreState_t MQTT_Core_GetState(void);
uint8_t MQTT_Core_IsPublishActive(void);
uint32_t MQTT_Core_GetPubOkSeq(void);
uint32_t MQTT_Core_GetLastRxAgeMs(void);
uint32_t MQTT_Core_GetLastPubOkAgeMs(void);
uint8_t MQTT_Core_Publish(const char* topic, const char* payload, uint8_t qos);
uint8_t MQTT_Core_PublishEx(const char* topic, const char* payload, uint8_t qos, uint8_t retain);
void MQTT_Core_CancelPendingPublish(void);
uint8_t MQTT_Core_IsIccidValid(void);
const char* MQTT_Core_GetIccid(void);
uint8_t MQTT_Core_IsIccidPending(void);
uint8_t MQTT_Core_IsRxCollecting(void);
void MQTT_Core_TransportTask(void);
void MQTT_Core_SetSecurityReady(uint8_t ready);
uint8_t MQTT_Core_ApplyRuntimeConfig(const ABoxMqttConfig* config);
uint8_t MQTT_Core_IsConnected(void);
uint8_t MQTT_Core_IsPaused(void);
uint8_t MQTT_Core_TakeModemReady(void);
uint8_t MQTT_Core_ClientIndex(void);
void MQTT_Core_SetDeviceId(const char* device_id);
int MQTT_Core_Stop(uint32_t now);
int MQTT_Core_BorrowWorkspace(void);
void MQTT_Core_ReturnWorkspace(void);
void MQTT_Core_ClearBroker(void);
uint32_t MQTT_Core_GetReadySeq(void);
uint8_t MQTT_Core_PublishExTracked(const char* topic, const char* payload, uint8_t qos, uint8_t retain, uint64_t* operation);
ABoxMqttReceiptStatus MQTT_Core_PublishResult(uint64_t operation);
void MQTT_Core_SetPublishObserver(MQTT_CorePublishObserver_t observer, void* user);
ABoxMqttEc800Bridge* MQTT_Core_Bridge(void);
Ec800AtCore_t* MQTT_Core_AtCore(void);

#endif

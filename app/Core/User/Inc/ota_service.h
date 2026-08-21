#ifndef LOCKCTRL_OTA_SERVICE_H
#define LOCKCTRL_OTA_SERVICE_H
#include <stdint.h>
#include "ec800_at.h"
typedef struct { char url[256]; char version[32]; uint32_t size; uint32_t crc32; } OtaServiceRequest_t;
void OtaService_Init(Ec800AtCore_t *at);
uint8_t OtaService_BeginProvisioning(void);
void OtaService_Task(void);
uint8_t OtaService_IsProvisioned(void);
uint8_t OtaService_IsReady(void);
uint8_t OtaService_IsBusy(void);
const char *OtaService_Phase(void);
const char *OtaService_TargetVersion(void);
uint32_t OtaService_LastError(void);
uint8_t OtaService_Start(const OtaServiceRequest_t *request);
uint8_t OtaService_TakeInstallReady(void);
uint8_t OtaService_TakeFailure(uint32_t *error);
void OtaService_AcknowledgeRollback(void);
#endif

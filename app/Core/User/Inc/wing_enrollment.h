#ifndef WING_ENROLLMENT_H
#define WING_ENROLLMENT_H
#include "ec800_at.h"
uint8_t WingEnrollment_Init(Ec800AtCore_t* at);
void WingEnrollment_Task(void);
void WingEnrollment_AfterModemReset(void);
const char* WingEnrollment_DeviceId(void);
#endif

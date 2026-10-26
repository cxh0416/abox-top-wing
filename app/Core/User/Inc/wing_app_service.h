#ifndef WING_APP_SERVICE_H
#define WING_APP_SERVICE_H
#include "abox_app_service.h"
int WingAppService_Init(ABoxEc800At *at, ABoxEc800Rx *rx);
void WingAppService_Poll(uint32_t now);
#endif

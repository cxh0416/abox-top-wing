#ifndef WING_POWER_H
#define WING_POWER_H

#include <stdint.h>

/* State reflects commanded outputs, not sensed voltage or relay feedback. */
void WingPower_Init(void);
void WingPower_Set(uint8_t enabled);
uint8_t WingPower_IsOn(void);

#endif

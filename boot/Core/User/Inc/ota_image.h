#ifndef LOCKCTRL_OTA_IMAGE_H
#define LOCKCTRL_OTA_IMAGE_H
#include <stdint.h>
#include "boot_cfg.h"
static inline uint32_t TopWingOta_ReadLe32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8U)|((uint32_t)p[2]<<16U)|((uint32_t)p[3]<<24U);}
static inline uint8_t TopWingOta_VectorValid(const uint8_t *p){uint32_t sp,reset;if(!p)return 0U;sp=TopWingOta_ReadLe32(p);reset=TopWingOta_ReadLe32(p+4U);return(sp>=0x20000000U&&sp<=0x20010000U&&(reset&1U)&&reset>=APP_START_ADDR&&reset<=APP_END_ADDR)?1U:0U;}
#define SweeperOta_ReadLe32 TopWingOta_ReadLe32
#define SweeperOta_VectorValid TopWingOta_VectorValid
#endif

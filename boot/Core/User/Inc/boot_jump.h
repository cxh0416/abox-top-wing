#ifndef __BOOT_JUMP_H__
#define __BOOT_JUMP_H__

#include <stdint.h>

uint8_t Boot_IsAppValid(uint32_t app_addr);
void Boot_JumpToApp(uint32_t app_addr);

#endif
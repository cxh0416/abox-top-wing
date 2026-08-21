#ifndef LOCKCTRL_BOOT_TEST_H
#define LOCKCTRL_BOOT_TEST_H
#include <stdint.h>
#define BOOT_TEST_PHASE_NONE 0U
#define BOOT_TEST_PHASE_STATE_WRITE 1U
#define BOOT_TEST_PHASE_POST_ERASE_STATE 2U
static inline void BootTest_SetPhase(uint32_t p){(void)p;}
static inline void BootTest_BootStateCheckpoint(uint32_t a,uint32_t o,uint32_t s,uint32_t z,uint32_t c,const char*v){(void)a;(void)o;(void)s;(void)z;(void)c;(void)v;}
static inline uint8_t BootTest_ForceVectorInvalid(uint32_t s,uint32_t c){(void)s;(void)c;return 0U;}
static inline uint8_t BootTest_UfsCandidateMissingRequested(uint32_t s,uint32_t c,const char*v){(void)s;(void)c;(void)v;return 0U;}
static inline uint8_t BootTest_CorruptBothStatePages(uint32_t s,uint32_t c,const char*v){(void)s;(void)c;(void)v;return 0U;}
static inline uint8_t BootTest_UfsCleanupRequested(void){return 0U;}
static inline void BootTest_ProgramCheckpoint(uint32_t o,uint32_t s,uint32_t c){(void)o;(void)s;(void)c;}
#endif


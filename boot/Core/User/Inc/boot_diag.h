#ifndef LOCKCTRL_BOOT_DIAG_H
#define LOCKCTRL_BOOT_DIAG_H
#include "boot_state.h"
static inline void BootDiag_LogStatePages(const char *e){(void)e;}
static inline void BootDiag_LogStateRecord(const char *e,const BootStateRecord_t*r){(void)e;(void)r;}
static inline void BootDiag_LogStateTransition(uint32_t a,uint32_t b,const char*r){(void)a;(void)b;(void)r;}
static inline void BootDiag_LogReset(uint32_t f,uint8_t r){(void)f;(void)r;}
#endif


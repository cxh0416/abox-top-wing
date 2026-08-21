#ifndef LOCKCTRL_FAULT_MAILBOX_H
#define LOCKCTRL_FAULT_MAILBOX_H
#include <stdint.h>
#define SWEEPER_FAULT_MAGIC 0x46544355U
#define SWEEPER_FAULT_HARDFAULT 1U
typedef struct {uint32_t magic,reason,pc,lr,crc32;} SweeperFaultMailbox_t;
extern volatile SweeperFaultMailbox_t g_sweeper_fault_mailbox;
uint8_t SweeperFault_Valid(void); void SweeperFault_Clear(void); void SweeperFault_Record(uint32_t,uint32_t,uint32_t);
#endif


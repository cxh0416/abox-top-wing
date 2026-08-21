#include "fault_mailbox.h"
#include "boot_state.h"
volatile SweeperFaultMailbox_t g_sweeper_fault_mailbox;
void SweeperFault_Record(uint32_t r,uint32_t pc,uint32_t lr){g_sweeper_fault_mailbox.magic=SWEEPER_FAULT_MAGIC;g_sweeper_fault_mailbox.reason=r;g_sweeper_fault_mailbox.pc=pc;g_sweeper_fault_mailbox.lr=lr;g_sweeper_fault_mailbox.crc32=BootState_Crc32((const void*)&g_sweeper_fault_mailbox,16U);}
uint8_t SweeperFault_Valid(void){return(g_sweeper_fault_mailbox.magic==SWEEPER_FAULT_MAGIC&&g_sweeper_fault_mailbox.crc32==BootState_Crc32((const void*)&g_sweeper_fault_mailbox,16U))?1U:0U;}
void SweeperFault_Clear(void){g_sweeper_fault_mailbox.magic=0U;g_sweeper_fault_mailbox.crc32=0U;}


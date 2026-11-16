#include <stdint.h>
#include "gd32f10x.h"
#include "abox_boot_v2.h"
#include "abox_uart.h"
#include "abox_gd32_monotonic.h"
#ifdef ABOX_CAN_EVENTS_ENABLED
#include "abox_can_events.h"
#ifdef ABOX_GD_FREERTOS
#include "FreeRTOS.h"
#include "task.h"
#endif
static void can_irq(unsigned bus)
{
#ifdef ABOX_GD_FREERTOS
    UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
    ABoxCanPort_DispatchEventIrq(bus);
    taskEXIT_CRITICAL_FROM_ISR(saved);
#else
    uint32_t saved = __get_PRIMASK();
    __disable_irq();
    ABoxCanPort_DispatchEventIrq(bus);
    __set_PRIMASK(saved);
#endif
}
void CAN0_TX_IRQHandler(void) { can_irq(0U); }
void CAN0_RX0_IRQHandler(void) { can_irq(0U); }
void CAN0_RX1_IRQHandler(void) { can_irq(0U); }
void CAN0_EWMC_IRQHandler(void) { can_irq(0U); }
void CAN1_TX_IRQHandler(void) { can_irq(1U); }
void CAN1_RX0_IRQHandler(void) { can_irq(1U); }
void CAN1_RX1_IRQHandler(void) { can_irq(1U); }
void CAN1_EWMC_IRQHandler(void) { can_irq(1U); }
#endif

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);
extern int main(void);
extern void WingGd_SysTick(void);
void Reset_Handler(void);
void Default_Handler(void) { for (;;) {} }
void HardFault_Handler(void) { ABoxBootV2_FaultRecord(1U, 0U, 0U); NVIC_SystemReset(); }
#ifdef ABOX_DMA_API_ENABLED
#include "abox_dma.h"
#endif
static void dma_irq(unsigned controller, unsigned channel)
{
    ABoxUart_DmaIrq(controller, channel);
#ifdef ABOX_DMA_API_ENABLED
    ABoxDma_Irq(controller, channel);
#endif
}
void DMA0_Channel0_IRQHandler(void) { dma_irq(1U, 1U); }
void DMA0_Channel1_IRQHandler(void) { dma_irq(1U, 2U); }
void DMA0_Channel2_IRQHandler(void) { dma_irq(1U, 3U); }
void DMA0_Channel3_IRQHandler(void) { dma_irq(1U, 4U); }
void DMA0_Channel4_IRQHandler(void) { dma_irq(1U, 5U); }
void DMA0_Channel5_IRQHandler(void) { dma_irq(1U, 6U); }
void DMA0_Channel6_IRQHandler(void) { dma_irq(1U, 7U); }
void DMA1_Channel0_IRQHandler(void) { dma_irq(2U, 1U); }
void DMA1_Channel1_IRQHandler(void) { dma_irq(2U, 2U); }
void DMA1_Channel2_IRQHandler(void) { dma_irq(2U, 3U); }
void DMA1_Channel3_IRQHandler(void) { dma_irq(2U, 4U); }
void DMA1_Channel4_IRQHandler(void) { dma_irq(2U, 5U); }
void TIMER1_IRQHandler(void) { ABoxGdMonotonic_IrqHandler(); }
void USART0_IRQHandler(void) { ABoxUart_Irq(1U); }
void USART1_IRQHandler(void) { ABoxUart_Irq(2U); }
void USART2_IRQHandler(void) { ABoxUart_Irq(3U); }
void UART3_IRQHandler(void) { ABoxUart_Irq(4U); }
void UART4_IRQHandler(void) { ABoxUart_Irq(5U); }
#ifdef ABOX_I2C_API_ENABLED
#include "abox_i2c.h"
void I2C0_EV_IRQHandler(void) { ABoxI2c_Irq(1U); }
void I2C0_ER_IRQHandler(void) { ABoxI2c_Irq(1U); }
void I2C1_EV_IRQHandler(void) { ABoxI2c_Irq(2U); }
void I2C1_ER_IRQHandler(void) { ABoxI2c_Irq(2U); }
#endif
#ifdef ABOX_I2S_API_ENABLED
#include "abox_i2s.h"
void SPI1_IRQHandler(void) { ABoxI2s_Irq(2U); }
void SPI2_IRQHandler(void) { ABoxI2s_Irq(3U); }
#endif
#ifdef ABOX_WWDG_API_ENABLED
#include "abox_wwdg.h"
void WWDGT_IRQHandler(void) { ABoxWwdg_Irq(); }
#endif
#ifdef ABOX_ADC_API_ENABLED
#include "abox_adc.h"
void ADC0_1_IRQHandler(void) { ABoxAdc_Irq(1U);ABoxAdc_Irq(2U); }
#endif
#ifdef ABOX_USB_API_ENABLED
#include "abox_usb.h"
void USBFS_IRQHandler(void) { ABoxUsb_Irq(); }
#endif
#ifdef ABOX_PVD_API_ENABLED
#include "abox_pvd.h"
void LVD_IRQHandler(void) { ABoxPvd_Irq(); }
#endif
#if defined(ABOX_GD_FREERTOS)
/* The product must link the FreeRTOS port and CMSIS-RTOS tick handler. */
extern void SVC_Handler(void);
extern void PendSV_Handler(void);
extern void SysTick_Handler(void);
#endif

/* GD32F10x_CL IRQ indices come from the pinned vendor device header. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".isr_vector"), used))
const uintptr_t g_pfnVectors[16U + 68U] = {
    [0] = (uintptr_t)&_estack, [1] = (uintptr_t)Reset_Handler,
    [2 ... 16U + 67U] = (uintptr_t)Default_Handler,
    [3] = (uintptr_t)HardFault_Handler,
    [15] = (uintptr_t)WingGd_SysTick,
#if defined(ABOX_GD_FREERTOS)
    [11] = (uintptr_t)SVC_Handler,
    [14] = (uintptr_t)PendSV_Handler,
    [15] = (uintptr_t)SysTick_Handler,
#endif
    [16U + DMA0_Channel0_IRQn] = (uintptr_t)DMA0_Channel0_IRQHandler,
    [16U + DMA0_Channel1_IRQn] = (uintptr_t)DMA0_Channel1_IRQHandler,
    [16U + DMA0_Channel2_IRQn] = (uintptr_t)DMA0_Channel2_IRQHandler,
    [16U + DMA0_Channel3_IRQn] = (uintptr_t)DMA0_Channel3_IRQHandler,
    [16U + DMA0_Channel4_IRQn] = (uintptr_t)DMA0_Channel4_IRQHandler,
    [16U + DMA0_Channel5_IRQn] = (uintptr_t)DMA0_Channel5_IRQHandler,
    [16U + DMA0_Channel6_IRQn] = (uintptr_t)DMA0_Channel6_IRQHandler,
    [16U + DMA1_Channel0_IRQn] = (uintptr_t)DMA1_Channel0_IRQHandler,
    [16U + DMA1_Channel1_IRQn] = (uintptr_t)DMA1_Channel1_IRQHandler,
    [16U + DMA1_Channel2_IRQn] = (uintptr_t)DMA1_Channel2_IRQHandler,
    [16U + DMA1_Channel3_IRQn] = (uintptr_t)DMA1_Channel3_IRQHandler,
    [16U + DMA1_Channel4_IRQn] = (uintptr_t)DMA1_Channel4_IRQHandler,
    [16U + TIMER1_IRQn] = (uintptr_t)TIMER1_IRQHandler,
    [16U + USART0_IRQn] = (uintptr_t)USART0_IRQHandler,
    [16U + USART1_IRQn] = (uintptr_t)USART1_IRQHandler,
    [16U + USART2_IRQn] = (uintptr_t)USART2_IRQHandler,
    [16U + UART3_IRQn] = (uintptr_t)UART3_IRQHandler,
    [16U + UART4_IRQn] = (uintptr_t)UART4_IRQHandler
#ifdef ABOX_I2C_API_ENABLED
    , [16U + I2C0_EV_IRQn] = (uintptr_t)I2C0_EV_IRQHandler
    , [16U + I2C0_ER_IRQn] = (uintptr_t)I2C0_ER_IRQHandler
    , [16U + I2C1_EV_IRQn] = (uintptr_t)I2C1_EV_IRQHandler
    , [16U + I2C1_ER_IRQn] = (uintptr_t)I2C1_ER_IRQHandler
#endif
#ifdef ABOX_I2S_API_ENABLED
    , [16U + SPI1_IRQn] = (uintptr_t)SPI1_IRQHandler
    , [16U + SPI2_IRQn] = (uintptr_t)SPI2_IRQHandler
#endif
#ifdef ABOX_WWDG_API_ENABLED
    , [16U + WWDGT_IRQn] = (uintptr_t)WWDGT_IRQHandler
#endif
#ifdef ABOX_ADC_API_ENABLED
    , [16U + ADC0_1_IRQn] = (uintptr_t)ADC0_1_IRQHandler
#endif
#ifdef ABOX_USB_API_ENABLED
    , [16U + USBFS_IRQn] = (uintptr_t)USBFS_IRQHandler
#endif
#ifdef ABOX_PVD_API_ENABLED
    , [16U + LVD_IRQn] = (uintptr_t)LVD_IRQHandler
#endif
#ifdef ABOX_CAN_EVENTS_ENABLED
    , [16U + CAN0_TX_IRQn] = (uintptr_t)CAN0_TX_IRQHandler
    , [16U + CAN0_RX0_IRQn] = (uintptr_t)CAN0_RX0_IRQHandler
    , [16U + CAN0_RX1_IRQn] = (uintptr_t)CAN0_RX1_IRQHandler
    , [16U + CAN0_EWMC_IRQn] = (uintptr_t)CAN0_EWMC_IRQHandler
    , [16U + CAN1_TX_IRQn] = (uintptr_t)CAN1_TX_IRQHandler
    , [16U + CAN1_RX0_IRQn] = (uintptr_t)CAN1_RX0_IRQHandler
    , [16U + CAN1_RX1_IRQn] = (uintptr_t)CAN1_RX1_IRQHandler
    , [16U + CAN1_EWMC_IRQn] = (uintptr_t)CAN1_EWMC_IRQHandler
#endif
};
#pragma GCC diagnostic pop

void Reset_Handler(void)
{
    uint32_t *source = &_sidata;
    for (uint32_t *p = &_sdata; p < &_edata; ++p) *p = *source++;
    for (uint32_t *p = &_sbss; p < &_ebss; ++p) *p = 0U;
    __libc_init_array();
    (void)main();
    for (;;) {}
}

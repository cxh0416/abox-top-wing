#ifndef TEST_USART_H
#define TEST_USART_H
#include <stdint.h>
typedef int UART_HandleTypeDef;
#define HAL_OK 0
extern UART_HandleTypeDef huart1;
int HAL_UART_AbortReceive(UART_HandleTypeDef *uart);
int HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef *uart, uint8_t *buffer, uint16_t size);
#endif

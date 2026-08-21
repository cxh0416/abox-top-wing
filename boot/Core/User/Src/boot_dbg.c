#include "boot_dbg.h"

#include <stdarg.h>
#include <stdio.h>

#include "usart.h"

void boot_printf(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0) return;
    if (n > (int)sizeof(buf)) n = sizeof(buf);

    HAL_UART_Transmit(&huart5, (uint8_t*)buf, (uint16_t)n, 200);
}
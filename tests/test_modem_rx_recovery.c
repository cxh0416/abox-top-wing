#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* Run the production main-loop service with failing/successful HAL boundaries. */
#define HAL_OK 0
#define ABOX_EC800_RESULT_ERROR 1
#define UART1_RX_BUF_SZ 1088U
static int g_ec800_rx, g_ec800_at, huart1;
static uint8_t uart1_rx_buf[UART1_RX_BUF_SZ], g_modem_restarting;
static int powered, requested, abort_result, arm_result, quarantined;
static unsigned step, aborted_at, cleared_at, armed_at, notified_at;
static int EC_Power_IsOnDone(void) { return powered; }
static void Ec800RxSink(void *c, const uint8_t *d, uint16_t n)
{ (void)c; (void)d; (void)n; }
static void ABoxEc800Rx_RequestRestart(int *r) { (void)r; requested = 1; }
static int ABoxEc800Rx_Drain(int *r, void (*sink)(void *, const uint8_t *, uint16_t),
                           int *at, unsigned limit)
{ (void)r; (void)sink; (void)at; (void)limit; return 0; }
static unsigned ABoxEc800Rx_OverflowCount(int *r) { (void)r; return 0; }
static void EC800_AT_SetRxOverflowCount(int *at, unsigned n) { (void)at; (void)n; }
static int ABoxEc800Rx_TakeRestartRequest(int *r)
{ int value = requested; (void)r; requested = 0; return value; }
static void ABoxEc800At_AbortAll(int *at, int reason)
{ (void)at; (void)reason; quarantined = 1; ++step; }
static int HAL_UART_AbortReceive(int *uart)
{ (void)uart; aborted_at = ++step; return abort_result; }
static void ABoxEc800Rx_AfterRestart(int *r)
{ (void)r; cleared_at = ++step; }
static int HAL_UARTEx_ReceiveToIdle_DMA(int *uart, uint8_t *buf, unsigned size)
{ (void)uart; (void)buf; (void)size; armed_at = ++step; return arm_result; }
static void ABoxEc800At_OnModemReset(int *at)
{ (void)at; notified_at = ++step; quarantined = 0; }

#include "modem_rx_service.inc"

static void reset_case(void)
{
    g_modem_restarting = powered = quarantined = 1;
    requested = abort_result = arm_result = 0;
    step = aborted_at = cleared_at = armed_at = notified_at = 0;
}

int main(void)
{
    reset_case();
    Ec800RxService();
    assert(!quarantined && !g_modem_restarting);
    assert(aborted_at < cleared_at && cleared_at < armed_at && armed_at < notified_at);
    unsigned completed = step;
    Ec800RxService();
    assert(step == completed);

    reset_case(); abort_result = 1;
    Ec800RxService();
    assert(quarantined && g_modem_restarting && !cleared_at && !notified_at);
    abort_result = 0;
    Ec800RxService();
    assert(!quarantined && !g_modem_restarting);

    reset_case(); arm_result = 1;
    Ec800RxService();
    assert(quarantined && g_modem_restarting && !notified_at);
    arm_result = 0;
    Ec800RxService();
    assert(!quarantined && !g_modem_restarting);

    reset_case(); g_modem_restarting = 0; requested = 1;
    Ec800RxService();
    assert(quarantined && armed_at && !notified_at);

    reset_case(); powered = 0;
    Ec800RxService();
    assert(quarantined && g_modem_restarting && !notified_at && !armed_at);
    puts("modem RX recovery: PASS");
}

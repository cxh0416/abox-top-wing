#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "main.h"
#include "anti_pinch_board.h"
#include "anti_pinch_alarm.h"
#include "wing_power.h"

static uint8_t contact, sonar;
static uint32_t outputs, irq;
uint32_t __get_PRIMASK(void) { return irq; }
void __disable_irq(void) { irq = 1U; }
void __set_PRIMASK(uint32_t state) { irq = state; }
uint32_t gpio_input_bit_get(uint32_t port, uint32_t pin)
{ assert(port == GPIOA && pin == (1U << 4)); return contact; }
void gpio_bit_set(uint32_t port, uint32_t pin) { assert(port == GPIOA); outputs |= pin; }
void gpio_bit_reset(uint32_t port, uint32_t pin) { assert(port == GPIOA); outputs &= ~pin; }
uint8_t UltrasonicBoard_IsAlarm(uint32_t now) { (void)now; return sonar; }
int main(void)
{
    outputs = 7U; AntiPinchBoard_Init(); assert(outputs == 6U);
    WingPower_Init(); assert(outputs == 6U);
    AntiPinchBoard_SetScopeEnabled(1U); contact = 1U;
    AntiPinchBoard_Tick(100U); assert(!AntiPinchBoard_TakeTrigger());
    AntiPinchBoard_Tick(100U + ANTI_PINCH_DEBOUNCE_MS);
    assert(AntiPinchBoard_IsPressed() && AntiPinchBoard_TakeTrigger() && (outputs & 1U));
    assert(!AntiPinchBoard_TakeTrigger());
    contact = 0U; AntiPinchBoard_Tick(200U);
    AntiPinchBoard_Tick(200U + ANTI_PINCH_DEBOUNCE_MS);
    assert(!AntiPinchBoard_IsPressed());
    AntiPinchBoard_Tick(200U + ANTI_PINCH_ALARM_TAIL_MS);
    assert(!(outputs & 1U));
    sonar = 1U; AntiPinchBoard_Tick(5000U); assert(outputs & 1U);
    WingPower_Set(0U); assert(outputs == 1U);
    sonar = 0U; AntiPinchBoard_Tick(5001U); assert(!outputs);
    puts("GD V1 B8/PA4 debounce, alarm and power output passed");
    return 0;
}

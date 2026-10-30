#include <assert.h>

#include "main.h"
#include "gpio.h"
#include "anti_pinch_board.h"
#include "ultrasonic_alarm.h"

static UltrasonicAlarm sonar;
uint8_t UltrasonicBoard_IsAlarm(uint32_t now) { return UltrasonicAlarm_Update(&sonar, now); }

GPIO_TypeDef test_gpioa, test_gpiob, test_gpioc, test_gpiod;
static uint8_t clock_a, clock_b, clock_c, clock_d, configured_input, configured_output;
static unsigned configurations;
static unsigned writes, reads, output_changes;
static GPIO_PinState contact = GPIO_PIN_RESET, output = GPIO_PIN_SET;

void Test_EnableGpioClock(GPIO_TypeDef* port)
{
    if (port == GPIOA) clock_a = 1U;
    else if (port == GPIOB) clock_b = 1U;
    else if (port == GPIOC) clock_c = 1U;
    else { assert(port == GPIOD); clock_d = 1U; }
}

void HAL_GPIO_Init(GPIO_TypeDef* port, GPIO_InitTypeDef* config)
{
    ++configurations;
    if (port == GPIOA && (config->Pin & GPIO_PIN_0))
    {
        assert(clock_a);
        assert(config->Mode == GPIO_MODE_OUTPUT_PP && config->Pull == GPIO_NOPULL);
        assert(output == GPIO_PIN_RESET); /* Off before switching pin mode. */
        configured_output = 1U;
    }
    else if (port == GPIOA && (config->Pin & GPIO_PIN_7))
    {
        assert(clock_a);
        assert(config->Mode == GPIO_MODE_INPUT && config->Pull == GPIO_NOPULL);
        configured_input = 1U;
    }
    else if (port == GPIOB)
    {
        assert(!(config->Pin & GPIO_PIN_0)); /* Alarm must not configure PB0. */
    }
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef* port, uint16_t pin)
{
    assert(port == GPIOA && pin == GPIO_PIN_7 && configured_input);
    ++reads;
    return contact;
}

void HAL_GPIO_WritePin(GPIO_TypeDef* port, uint16_t pin, GPIO_PinState value)
{
    if (port == GPIOC) { assert(clock_c); return; }
    if (port == GPIOB) { assert(clock_b && !(pin & GPIO_PIN_0)); return; }
    assert(port == GPIOA && clock_a && pin == GPIO_PIN_0);
    if (output != value) ++output_changes;
    output = value;
    ++writes;
}

int main(void)
{
    AntiPinchBoard_Tick(0U);
    assert(!reads && !writes); /* Early TIM1 interrupts must not touch GPIO. */
    MX_GPIO_Init(); /* Exercise the actual generated pin setup. */
    assert(clock_d && configured_input && configured_output);
    unsigned gpio_configurations = configurations;
    AntiPinchBoard_Init();
    assert(configurations == gpio_configurations); /* No duplicate pin setup. */
    assert(configured_input && configured_output);
    assert(output == GPIO_PIN_RESET && output_changes == 1U);
    AntiPinchBoard_Tick(1U);
    assert(output == GPIO_PIN_RESET && output_changes == 1U);
    unsigned initial_writes = writes;

    /* External contact through the optocoupler drives PA7 HIGH. */
    contact = GPIO_PIN_SET;
    AntiPinchBoard_Tick(10U);
    AntiPinchBoard_Tick(29U);
    assert(output == GPIO_PIN_RESET);
    AntiPinchBoard_Tick(30U);
    assert(output == GPIO_PIN_SET && output_changes == 2U);
    assert(AntiPinchBoard_IsPressed());
    assert(AntiPinchBoard_TakeTrigger());
    assert(!AntiPinchBoard_TakeTrigger());
    AntiPinchBoard_Tick(5000U);
    assert(output == GPIO_PIN_SET);
    assert(output_changes == 2U); /* Held input keeps PWR12V_OUT1 enabled. */
    assert(!AntiPinchBoard_TakeTrigger());
    assert(writes == initial_writes + 1U); /* No redundant writes or PWM. */

    contact = GPIO_PIN_RESET;
    AntiPinchBoard_Tick(5010U);
    AntiPinchBoard_Tick(5029U);
    assert(output == GPIO_PIN_SET);
    AntiPinchBoard_Tick(5030U);
    assert(!AntiPinchBoard_IsPressed() && output == GPIO_PIN_SET);
    AntiPinchBoard_Tick(6029U);
    assert(output == GPIO_PIN_SET);
    AntiPinchBoard_Tick(6030U);
    assert(output == GPIO_PIN_RESET && output_changes == 3U);
    contact = GPIO_PIN_SET;
    AntiPinchBoard_Tick(6040U);
    AntiPinchBoard_Tick(6060U);
    assert(output == GPIO_PIN_SET && output_changes == 4U);
    assert(AntiPinchBoard_TakeTrigger());
    contact = GPIO_PIN_RESET;
    AntiPinchBoard_Tick(7060U);
    AntiPinchBoard_Tick(7080U);
    assert(output == GPIO_PIN_SET);
    AntiPinchBoard_Tick(8080U);
    assert(output == GPIO_PIN_RESET && output_changes == 5U);

    /* An open strip / optocoupler LOW output must keep power off. */
    for (uint32_t now = 8081U; now <= 38081U; ++now)
    {
        AntiPinchBoard_Tick(now);
        assert(output == GPIO_PIN_RESET && output_changes == 5U);
    }
    assert(writes == initial_writes + 4U);

    /* A contact closed at startup also works across uint32 tick wrap. */
    AntiPinchBoard_Init();
    contact = GPIO_PIN_SET;
    const uint32_t start = UINT32_MAX - 10U;
    AntiPinchBoard_Tick(start);
    AntiPinchBoard_Tick(start + 19U);
    assert(output == GPIO_PIN_RESET);
    AntiPinchBoard_Tick(start + 20U);
    assert(output == GPIO_PIN_SET);
    assert(AntiPinchBoard_TakeTrigger());
    contact = GPIO_PIN_RESET;
    AntiPinchBoard_Tick(start + 21U);
    AntiPinchBoard_Tick(start + 41U);
    assert(output == GPIO_PIN_SET);
    AntiPinchBoard_Tick(start + 1040U);
    assert(output == GPIO_PIN_SET);
    AntiPinchBoard_Tick(start + 1041U);
    assert(output == GPIO_PIN_RESET);
    /* Both sources share PA0; clearing one must not override the other. */
    AntiPinchBoard_Init();
    UltrasonicAlarm_Init(&sonar);
    contact = GPIO_PIN_RESET;
    uint8_t distances[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,30};
    assert(UltrasonicAlarm_OnFrame(&sonar, 10U, WL300D_CAN_ID, 1U, 0U, 8U, distances));
    AntiPinchBoard_Tick(10U);
    assert(output == GPIO_PIN_SET && !AntiPinchBoard_TakeTrigger());
    contact = GPIO_PIN_SET;
    AntiPinchBoard_Tick(20U);
    AntiPinchBoard_Tick(40U);
    assert(AntiPinchBoard_TakeTrigger());
    distances[7] = 0xFF;
    for (uint32_t t = 100; t <= 1100; t += 100)
    {
        UltrasonicAlarm_OnFrame(&sonar,t,WL300D_CAN_ID,1,0,8,distances);
        AntiPinchBoard_Tick(t);
        assert(output == GPIO_PIN_SET); /* Contact still pressed after sonar clears. */
    }
    contact = GPIO_PIN_RESET;
    AntiPinchBoard_Tick(1200U);
    AntiPinchBoard_Tick(1220U);
    distances[0] = 20;
    UltrasonicAlarm_OnFrame(&sonar,1300U,WL300D_CAN_ID,1,0,8,distances);
    AntiPinchBoard_Tick(2220U);
    assert(output == GPIO_PIN_SET); /* Sonar holds output after contact tail. */
    AntiPinchBoard_Tick(2300U);
    assert(output == GPIO_PIN_RESET); /* Both sources are now expired. */
    contact = GPIO_PIN_SET;
    AntiPinchBoard_Tick(2400U);
    AntiPinchBoard_Tick(2420U);
    UltrasonicAlarm_OnFrame(&sonar,2500U,WL300D_CAN_ID,1,0,8,distances);
    AntiPinchBoard_Tick(3500U);
    assert(output == GPIO_PIN_SET); /* Sonar silence must not clear a held strip. */
    return 0;
}

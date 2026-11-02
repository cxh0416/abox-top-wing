#include <assert.h>
#include <stdint.h>

#include "anti_pinch_alarm.h"

int main(void)
{
    AntiPinchAlarm alarm;
    const uint32_t wrap = UINT32_MAX - 10U;

    AntiPinchAlarm_Init(&alarm);
    assert(!AntiPinchAlarm_Update(&alarm, 0U, 0U));

    /* A held assertion becomes active after debounce and stays active. */
    assert(!AntiPinchAlarm_Update(&alarm, 10U, 1U));
    assert(!AntiPinchAlarm_Update(&alarm, 29U, 1U));
    assert(AntiPinchAlarm_Update(&alarm, 30U, 1U));
    assert(AntiPinchAlarm_Update(&alarm, 1000U, 1U));
    assert(AntiPinchAlarm_Update(&alarm, 60000U, 1U));

    /* Release is debounced, then the active state follows the input. */
    assert(AntiPinchAlarm_Update(&alarm, 60010U, 0U));
    assert(AntiPinchAlarm_Update(&alarm, 60029U, 0U));
    assert(AntiPinchAlarm_Update(&alarm, 60030U, 0U));
    assert(!alarm.stable_pressed);
    assert(AntiPinchAlarm_Update(&alarm, 61009U, 0U));
    assert(!AntiPinchAlarm_Update(&alarm, 61010U, 0U));
    assert(!AntiPinchAlarm_Update(&alarm, 70000U, 0U));

    /* Short glitches do not change the stable state. */
    assert(!AntiPinchAlarm_Update(&alarm, 70010U, 1U));
    assert(!AntiPinchAlarm_Update(&alarm, 70029U, 0U));
    assert(!AntiPinchAlarm_Update(&alarm, 70050U, 0U));

    /* An input already high at startup also produces continuous alarm. */
    AntiPinchAlarm_Init(&alarm);
    assert(!AntiPinchAlarm_Update(&alarm, wrap, 1U));
    assert(AntiPinchAlarm_Update(&alarm, wrap + 20U, 1U));
    assert(AntiPinchAlarm_Update(&alarm, wrap + 1000U, 1U));
    assert(AntiPinchAlarm_Update(&alarm, wrap + 1010U, 0U));
    assert(AntiPinchAlarm_Update(&alarm, wrap + 1030U, 0U));
    assert(!alarm.stable_pressed);
    assert(AntiPinchAlarm_Update(&alarm, wrap + 2009U, 0U));
    assert(!AntiPinchAlarm_Update(&alarm, wrap + 2010U, 0U));

    /* Repress during the tail keeps alarm on; the next release gets a full tail. */
    AntiPinchAlarm_Init(&alarm);
    AntiPinchAlarm_Update(&alarm, 0U, 1U);
    assert(AntiPinchAlarm_Update(&alarm, 20U, 1U));
    AntiPinchAlarm_Update(&alarm, 30U, 0U);
    assert(AntiPinchAlarm_Update(&alarm, 50U, 0U));
    AntiPinchAlarm_Update(&alarm, 100U, 1U);
    assert(AntiPinchAlarm_Update(&alarm, 120U, 1U));
    assert(AntiPinchAlarm_Update(&alarm, 2000U, 1U));
    AntiPinchAlarm_Update(&alarm, 2010U, 0U);
    assert(AntiPinchAlarm_Update(&alarm, 2030U, 0U));
    assert(AntiPinchAlarm_Update(&alarm, 3009U, 0U));
    assert(!AntiPinchAlarm_Update(&alarm, 3010U, 0U));
    return 0;
}

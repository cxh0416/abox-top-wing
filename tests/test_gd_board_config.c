#include "wing_gd_board_config.h"
#include "abox_product_config.h"
#include <assert.h>
#include <string.h>

ABoxHalStatus ABoxMcuClockHw_Apply(const ABoxMcuClockConfig *c, const ABoxMcuClockFrequencies *f)
{ (void)c; (void)f; assert(!"hardware clock apply in host test"); return ABOX_HAL_ERROR; }
ABoxHalStatus ABoxMcuClockHw_Get(ABoxMcuClockFrequencies *f)
{ (void)f; assert(!"hardware clock read in host test"); return ABOX_HAL_ERROR; }

int main(void)
{
    const ABoxMcuClockConfig clock = TOP_WING_GD_CLOCK_CONFIG;
    ABoxMcuClockFrequencies hz;
    assert(ABox_ProductConfigIsValid(&ABOX_PRODUCT_CONFIG));
    assert(!strcmp(ABOX_PRODUCT_CONFIG.product_id, "top_flying_wing"));
    assert(ABOX_PRODUCT_CONFIG.scheduler == ABOX_SCHEDULER_BAREMETAL);
    assert(ABoxMcuClock_CalculateFor(ABOX_MCU_CLOCK_GD32F105, &clock, &hz) == ABOX_HAL_OK);
    assert(hz.system_hz == 108000000U && hz.ahb_hz == 108000000U);
    assert(hz.apb1_hz == 54000000U && hz.apb2_hz == 108000000U);
    assert(hz.apb1_timer_hz == 108000000U);
    /* The product's 18-quanta CAN timing must represent both first-bus options
       and its 500 kbit/s wing controller bus without rounding. */
    assert(hz.apb1_hz / (500000U * 18U) == 6U && hz.apb1_hz % (500000U * 18U) == 0U);
    assert(hz.apb1_hz / (250000U * 18U) == 12U && hz.apb1_hz % (250000U * 18U) == 0U);
    return 0;
}

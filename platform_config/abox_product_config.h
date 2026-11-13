#ifndef ABOX_PRODUCT_CONFIG_H
#define ABOX_PRODUCT_CONFIG_H
#include "abox_platform_config.h"
static const ABoxProductConfig ABOX_PRODUCT_CONFIG = {
    "top_flying_wing", 0x08000000U, 0x40000U, 0x08000000U, 0x8000U,
    0x08008000U, 0x0803F800U, 0x800U, 250000U, 500000U, 115200U,
    ABOX_SCHEDULER_BAREMETAL
};
#endif

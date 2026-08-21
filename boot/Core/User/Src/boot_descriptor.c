#include "abox_boot_v2.h"
const ABoxBootV2Descriptor g_boot_v2_descriptor __attribute__((used,section(".boot_descriptor")))={
 .magic=ABOX_BOOT_V2_DESCRIPTOR_MAGIC,.abi_version=ABOX_BOOT_V2_DESCRIPTOR_ABI,.length=sizeof(ABoxBootV2Descriptor),
 .feature_flags=ABOX_BOOT_V2_REQUIRED_FEATURES,.semver_major=2U,.semver_minor=2U,.semver_patch=0U,
 .product_id="top_flying_wing",.build_version="top-flying-wing-boot-2.2.0",.crc32=0xEA33B7EEU};

#ifndef __APP_VERSION_H__
#define __APP_VERSION_H__

#ifndef TOP_FLYING_WING_APP_FW_VERSION
#define TOP_FLYING_WING_APP_FW_VERSION "top-flying-wing-1.1.0"
#endif
#define TOP_FLYING_WING_APP_FW_VERSION_MARKER \
    "TOP_FLYING_WING_FW_VERSION=" TOP_FLYING_WING_APP_FW_VERSION

extern const char g_top_flying_wing_fw_version_marker[];
const char *TopFlyingWing_AppFwVersion(void);

#endif

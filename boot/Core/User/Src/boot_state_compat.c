#include "boot_state.h"

#include <string.h>

#include "abox_boot_v2.h"

_Static_assert(sizeof(BootStateRecord_t) == sizeof(ABoxBootV2Record), "Boot State V3 ABI mismatch");
_Static_assert(sizeof(BootStateDiagnostics_t) == sizeof(ABoxBootV2Diagnostics), "Boot diagnostics ABI mismatch");

uint32_t BootState_Crc32(const void *data, uint32_t length) { return ABoxBootV2_Crc32(data, length); }

uint8_t BootState_Load(BootStateRecord_t *record)
{
    ABoxBootV2Record common;
    if (record == 0) return 0U;
    if (!ABoxBootV2_StateLoad(&common)) { memset(record, 0, sizeof(*record)); return 0U; }
    memcpy(record, &common, sizeof(*record));
    return 1U;
}

uint8_t BootState_GetDiagnostics(BootStateDiagnostics_t *diagnostics)
{
    ABoxBootV2Diagnostics common;
    int valid;
    if (diagnostics == 0) return 0U;
    valid = ABoxBootV2_StateDiagnostics(&common);
    memcpy(diagnostics, &common, sizeof(*diagnostics));
    return (uint8_t)valid;
}

uint8_t BootState_Save(const BootStateRecord_t *record)
{
    ABoxBootV2Record common;
    if (record == 0) return 0U;
    memcpy(&common, record, sizeof(common));
    return (uint8_t)ABoxBootV2_StateSave(&common);
}

uint8_t BootState_Confirm(void) { return (uint8_t)ABoxBootV2_Confirm(); }
uint8_t BootState_AcknowledgeRollback(void) { return (uint8_t)ABoxBootV2_AcknowledgeRollback(); }

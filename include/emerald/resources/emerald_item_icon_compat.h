#ifndef EMERALD_RESOURCES_EMERALD_ITEM_ICON_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_ITEM_ICON_COMPAT_H

/* R15 Phase 5: Emerald item-icon graphics native compatibility
 * publication. Publishes the gItemIconTable from the ROM_BASE pack.
 *
 * The table is mutable on native (u32 *gItemIconTable[ITEMS_COUNT+1][2])
 * with NULL rows; the compat seam publishes the session pointers at init.
 */

#include <stdint.h>
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"

enum EmeraldResourceCompatStatus
EmeraldItemIconCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

void EmeraldItemIconCompat_ClearMigratedEntries(void);

const void *ItemIcon_Get(const char *id);

#endif
#ifndef EMERALD_RESOURCES_EMERALD_UI_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_UI_COMPAT_H

/* R15: Emerald UI graphics native compatibility publication.
 * Publishes all 379 UI graphics resources from the ROM_BASE pack.
 * Additive-degrade: if the pack cannot provide the resources, consumers
 * fall back to empty/zero graphics (session is not refused). */

#include <stdint.h>
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"

enum EmeraldResourceCompatStatus
EmeraldUICompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

void EmeraldUICompat_ClearMigratedEntries(void);

const void *UI_Get(const char *id);

#endif
#ifndef EMERALD_RESOURCES_EMERALD_EASY_CHAT_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_EASY_CHAT_COMPAT_H

/* R15: Emerald easy-chat word native compatibility publication.
 * Publishes all 1,008 easy-chat word payloads from the ROM_BASE pack.
 * Additive-degrade: if the pack cannot provide the resources, consumers
 * fall back to empty strings (session is not refused). */

#include <stdint.h>
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"

enum EmeraldResourceCompatStatus
EmeraldEasyChatCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

void EmeraldEasyChatCompat_ClearMigratedEntries(void);

const void *EC_GetWord(const char *id);

#endif
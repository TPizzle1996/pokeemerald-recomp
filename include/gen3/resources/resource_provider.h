#ifndef GEN3_RESOURCES_RESOURCE_PROVIDER_H
#define GEN3_RESOURCES_RESOURCE_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_diagnostics.h"
#include "gen3/resources/resource_types.h"

enum Gen3ResourceProviderKind
{
    GEN3_PROVIDER_BOOTSTRAP = 0,
    GEN3_PROVIDER_LEGACY_COMPILED,
    GEN3_PROVIDER_ROM_BASE,
    GEN3_PROVIDER_MOD,
    GEN3_PROVIDER_SYNTHETIC,
};

struct Gen3ResourceProvider;

struct Gen3ResourceProviderMetadata
{
    const char *id;
    const char *version;
    enum Gen3ResourceProviderKind kind;
    uint32_t precedence;
};

struct Gen3ResourceProvider *Gen3ResourceProvider_Create(
    const struct Gen3ResourceProviderMetadata *metadata);
void Gen3ResourceProvider_Destroy(struct Gen3ResourceProvider *provider);

/* Read-only metadata accessor. The returned id/version pointers reference
 * storage owned by the provider and remain valid until it is destroyed; the
 * caller must not free them. */
bool Gen3ResourceProvider_GetMetadata(
    const struct Gen3ResourceProvider *provider,
    struct Gen3ResourceProviderMetadata *outMetadata);
bool Gen3ResourceProvider_Add(struct Gen3ResourceProvider *provider,
                              const char *canonicalName,
                              enum Gen3ResourceType declaredType,
                              uint32_t declaredSchema,
                              const void *payload,
                              size_t payloadSize,
                              bool requiredForProvider,
                              struct Gen3ResourceDiagnosticList *diagnostics);
bool Gen3ResourceProvider_Finalize(struct Gen3ResourceProvider *provider,
                                   struct Gen3ResourceDiagnosticList *diagnostics);

#endif

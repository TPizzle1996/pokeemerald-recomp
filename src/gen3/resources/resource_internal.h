#ifndef GEN3_RESOURCES_RESOURCE_INTERNAL_H
#define GEN3_RESOURCES_RESOURCE_INTERNAL_H

#include "gen3/resources/resource_resolver.h"

typedef void (*Gen3ResourceKeyDeriverForTest)(const char *canonicalName,
                                               Gen3ResourceKey *outKey,
                                               void *context);

struct Gen3CatalogEntryOwned
{
    struct Gen3ResourceContract contract;
    char *name;
};

struct Gen3ResourceCatalog
{
    struct Gen3CatalogEntryOwned *entries;
    size_t count;
    size_t capacity;
    bool finalized;
    Gen3ResourceKeyDeriverForTest keyDeriver;
    void *keyDeriverContext;
};

struct Gen3ProviderEntryOwned
{
    char *canonicalName;
    enum Gen3ResourceType declaredType;
    uint32_t declaredSchema;
    uint8_t *payload;
    size_t payloadSize;
    bool requiredForProvider;
};

struct Gen3ResourceProvider
{
    char *id;
    char *version;
    enum Gen3ResourceProviderKind kind;
    uint32_t precedence;
    struct Gen3ProviderEntryOwned *entries;
    size_t count;
    size_t capacity;
    bool finalized;
};

struct Gen3ResourceCandidate
{
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceProvider **providers;
    size_t providerCount;
    size_t providerCapacity;
};

struct Gen3ResolvedOwned
{
    bool resolved;
    size_t providerIndex;
    size_t providerEntryIndex;
};

struct Gen3ResourceSnapshot
{
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceProvider **providers;
    size_t providerCount;
    struct Gen3ResolvedOwned *resolved;
};

void Gen3ResourceCatalog_SetKeyDeriverForTest(
    struct Gen3ResourceCatalog *catalog,
    Gen3ResourceKeyDeriverForTest deriver,
    void *context);

struct Gen3ResourceCatalog *Gen3ResourceCatalog_Clone(
    const struct Gen3ResourceCatalog *source);
struct Gen3ResourceProvider *Gen3ResourceProvider_Clone(
    const struct Gen3ResourceProvider *source);
const struct Gen3ProviderEntryOwned *Gen3ResourceProvider_FindEntry(
    const struct Gen3ResourceProvider *provider,
    const char *canonicalName,
    size_t *outIndex);
enum Gen3ResourceReason Gen3ResourceProvider_ValidateEntry(
    const struct Gen3ProviderEntryOwned *entry,
    const struct Gen3ResourceContract *contract);

#endif

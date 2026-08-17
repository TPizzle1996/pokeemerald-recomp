#include "resource_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char *CopyString(const char *source)
{
    size_t length;
    char *copy;
    if (source == NULL)
        return NULL;
    length = strlen(source);
    copy = malloc(length + 1u);
    if (copy != NULL)
        memcpy(copy, source, length + 1u);
    return copy;
}

static bool GrowArray(void **items, size_t itemSize, size_t *capacity, size_t required)
{
    size_t next;
    void *resized;
    if (*capacity >= required)
        return true;
    next = *capacity == 0 ? 8u : *capacity;
    while (next < required)
    {
        if (next > SIZE_MAX / 2u)
            return false;
        next *= 2u;
    }
    if (itemSize != 0 && next > SIZE_MAX / itemSize)
        return false;
    resized = realloc(*items, next * itemSize);
    if (resized == NULL)
        return false;
    *items = resized;
    *capacity = next;
    return true;
}

static void CopyBounded(char *destination, size_t capacity, const char *source)
{
    size_t length;
    if (capacity == 0)
        return;
    if (source == NULL)
        source = "";
    length = strlen(source);
    if (length >= capacity)
        length = capacity - 1u;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

void Gen3ResourceDiagnostics_Init(struct Gen3ResourceDiagnosticList *list)
{
    if (list != NULL)
        memset(list, 0, sizeof(*list));
}

void Gen3ResourceDiagnostics_Destroy(struct Gen3ResourceDiagnosticList *list)
{
    if (list == NULL)
        return;
    free(list->items);
    memset(list, 0, sizeof(*list));
}

bool Gen3ResourceDiagnostics_Append(
    struct Gen3ResourceDiagnosticList *list,
    enum Gen3ResourceDiagnosticSeverity severity,
    enum Gen3ResourceReason reason,
    const char *resourceName,
    const char *providerId,
    enum Gen3ResourceType expectedType,
    enum Gen3ResourceType actualType,
    uint32_t expectedSchema,
    uint32_t actualSchema,
    bool providerEntryRequired,
    bool catalogRequiredForBase)
{
    struct Gen3ResourceDiagnostic *diagnostic;
    if (list == NULL)
        return true;
    if (!GrowArray((void **)&list->items, sizeof(*list->items),
                   &list->capacity, list->count + 1u))
        return false;
    diagnostic = &list->items[list->count++];
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->severity = severity;
    diagnostic->reason = reason;
    CopyBounded(diagnostic->resourceName, sizeof(diagnostic->resourceName), resourceName);
    CopyBounded(diagnostic->providerId, sizeof(diagnostic->providerId), providerId);
    diagnostic->expectedType = expectedType;
    diagnostic->actualType = actualType;
    diagnostic->expectedSchema = expectedSchema;
    diagnostic->actualSchema = actualSchema;
    diagnostic->providerEntryRequired = providerEntryRequired;
    diagnostic->catalogRequiredForBase = catalogRequiredForBase;
    return true;
}

const char *Gen3ResourceReason_Describe(enum Gen3ResourceReason reason)
{
    static const char *const descriptions[] =
    {
        "no error",
        "invalid argument",
        "out of memory",
        "invalid canonical resource name",
        "duplicate catalog resource name",
        "resource key collision",
        "catalog is not finalized",
        "provider is not finalized",
        "duplicate provider identity",
        "duplicate provider precedence",
        "duplicate provider entry",
        "provider entry is not declared by the catalog",
        "resource type does not match the catalog contract",
        "resource schema does not match the catalog contract",
        "resource payload is invalid for its type",
        "required base resource has no valid base implementation",
        "resource has no valid resolved implementation",
    };
    if ((size_t)reason >= sizeof(descriptions) / sizeof(descriptions[0]))
        return "unknown resource diagnostic";
    return descriptions[reason];
}

const char *Gen3ResourceType_Name(enum Gen3ResourceType type)
{
    static const char *const names[] =
    {
        "invalid", "bitmap", "tile-graphics", "palette", "sprite-sheet",
        "sprite-metadata", "tileset", "tilemap", "font", "text",
        "audio-sample", "music-sequence", "sound-effect", "cry", "binary",
        "instrument-bank",
    };
    if ((size_t)type >= sizeof(names) / sizeof(names[0]))
        return "invalid";
    return names[type];
}

static void StandardKeyDeriver(const char *canonicalName,
                               Gen3ResourceKey *outKey,
                               void *context)
{
    (void)context;
    Gen3ResourceId_DeriveKey(canonicalName, outKey);
}

struct Gen3ResourceCatalog *Gen3ResourceCatalog_Create(void)
{
    struct Gen3ResourceCatalog *catalog = calloc(1, sizeof(*catalog));
    if (catalog != NULL)
        catalog->keyDeriver = StandardKeyDeriver;
    return catalog;
}

void Gen3ResourceCatalog_Destroy(struct Gen3ResourceCatalog *catalog)
{
    size_t i;
    if (catalog == NULL)
        return;
    for (i = 0; i < catalog->count; i++)
        free(catalog->entries[i].name);
    free(catalog->entries);
    free(catalog);
}

void Gen3ResourceCatalog_SetKeyDeriverForTest(
    struct Gen3ResourceCatalog *catalog,
    Gen3ResourceKeyDeriverForTest deriver,
    void *context)
{
    if (catalog == NULL || catalog->count != 0 || catalog->finalized)
        return;
    catalog->keyDeriver = deriver != NULL ? deriver : StandardKeyDeriver;
    catalog->keyDeriverContext = context;
}

bool Gen3ResourceCatalog_Add(struct Gen3ResourceCatalog *catalog,
                             const char *canonicalName,
                             enum Gen3ResourceType type,
                             uint32_t schema,
                             bool requiredForBase,
                             struct Gen3ResourceDiagnosticList *diagnostics)
{
    struct Gen3CatalogEntryOwned *entry;
    Gen3ResourceKey key;
    size_t i;
    enum Gen3ResourceReason reason;

    if (catalog == NULL || catalog->finalized || canonicalName == NULL
     || type <= GEN3_RESOURCE_TYPE_INVALID || type >= GEN3_RESOURCE_TYPE_COUNT
     || schema == 0)
    {
        Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
            GEN3_RESOURCE_REASON_INVALID_ARGUMENT, canonicalName, NULL,
            type, type, schema, schema, false, requiredForBase);
        return false;
    }
    if (Gen3ResourceId_ValidateCanonicalName(canonicalName) != GEN3_RESOURCE_NAME_VALID)
    {
        Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
            GEN3_RESOURCE_REASON_INVALID_CANONICAL_NAME, canonicalName, NULL,
            type, type, schema, schema, false, requiredForBase);
        return false;
    }
    catalog->keyDeriver(canonicalName, &key, catalog->keyDeriverContext);
    for (i = 0; i < catalog->count; i++)
    {
        if (strcmp(catalog->entries[i].name, canonicalName) == 0)
            reason = GEN3_RESOURCE_REASON_DUPLICATE_CATALOG_NAME;
        else if (Gen3ResourceId_KeyEqual(&catalog->entries[i].contract.key, &key))
            reason = GEN3_RESOURCE_REASON_RESOURCE_KEY_COLLISION;
        else
            continue;
        Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
            reason, canonicalName, NULL, type, type, schema, schema, false,
            requiredForBase);
        return false;
    }
    if (!GrowArray((void **)&catalog->entries, sizeof(*catalog->entries),
                   &catalog->capacity, catalog->count + 1u))
    {
        Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
            GEN3_RESOURCE_REASON_OUT_OF_MEMORY, canonicalName, NULL,
            type, type, schema, schema, false, requiredForBase);
        return false;
    }
    entry = &catalog->entries[catalog->count];
    memset(entry, 0, sizeof(*entry));
    entry->name = CopyString(canonicalName);
    if (entry->name == NULL)
        return false;
    entry->contract.key = key;
    entry->contract.canonicalName = entry->name;
    entry->contract.type = type;
    entry->contract.schema = schema;
    entry->contract.requiredForBase = requiredForBase;
    catalog->count++;
    return true;
}

static int CompareCatalogEntries(const void *left, const void *right)
{
    const struct Gen3CatalogEntryOwned *a = left;
    const struct Gen3CatalogEntryOwned *b = right;
    return strcmp(a->name, b->name);
}

bool Gen3ResourceCatalog_Finalize(struct Gen3ResourceCatalog *catalog,
                                  struct Gen3ResourceDiagnosticList *diagnostics)
{
    size_t i;
    (void)diagnostics;
    if (catalog == NULL || catalog->finalized || catalog->count > UINT32_MAX)
        return false;
    qsort(catalog->entries, catalog->count, sizeof(*catalog->entries), CompareCatalogEntries);
    for (i = 0; i < catalog->count; i++)
    {
        catalog->entries[i].contract.handle = (Gen3ResourceHandle)i;
        catalog->entries[i].contract.canonicalName = catalog->entries[i].name;
    }
    catalog->finalized = true;
    return true;
}

size_t Gen3ResourceCatalog_Count(const struct Gen3ResourceCatalog *catalog)
{
    return catalog != NULL && catalog->finalized ? catalog->count : 0;
}

const struct Gen3ResourceContract *Gen3ResourceCatalog_At(
    const struct Gen3ResourceCatalog *catalog, size_t index)
{
    if (catalog == NULL || !catalog->finalized || index >= catalog->count)
        return NULL;
    return &catalog->entries[index].contract;
}

const struct Gen3ResourceContract *Gen3ResourceCatalog_Find(
    const struct Gen3ResourceCatalog *catalog, const char *canonicalName)
{
    size_t low = 0;
    size_t high;
    if (catalog == NULL || !catalog->finalized || canonicalName == NULL)
        return NULL;
    high = catalog->count;
    while (low < high)
    {
        size_t middle = low + (high - low) / 2u;
        int comparison = strcmp(canonicalName, catalog->entries[middle].name);
        if (comparison == 0)
            return &catalog->entries[middle].contract;
        if (comparison < 0)
            high = middle;
        else
            low = middle + 1u;
    }
    return NULL;
}

struct Gen3ResourceCatalog *Gen3ResourceCatalog_Clone(
    const struct Gen3ResourceCatalog *source)
{
    struct Gen3ResourceCatalog *copy;
    size_t i;
    if (source == NULL || !source->finalized)
        return NULL;
    copy = Gen3ResourceCatalog_Create();
    if (copy == NULL)
        return NULL;
    for (i = 0; i < source->count; i++)
    {
        const struct Gen3ResourceContract *contract = &source->entries[i].contract;
        if (!GrowArray((void **)&copy->entries, sizeof(*copy->entries),
                       &copy->capacity, copy->count + 1u))
            goto fail;
        copy->entries[copy->count].name = CopyString(source->entries[i].name);
        if (copy->entries[copy->count].name == NULL)
            goto fail;
        copy->entries[copy->count].contract = *contract;
        copy->entries[copy->count].contract.canonicalName = copy->entries[copy->count].name;
        copy->count++;
    }
    copy->finalized = true;
    return copy;
fail:
    Gen3ResourceCatalog_Destroy(copy);
    return NULL;
}

struct Gen3ResourceProvider *Gen3ResourceProvider_Create(
    const struct Gen3ResourceProviderMetadata *metadata)
{
    struct Gen3ResourceProvider *provider;
    if (metadata == NULL || metadata->id == NULL || metadata->version == NULL
     || metadata->id[0] == '\0' || metadata->version[0] == '\0'
     || strlen(metadata->id) > GEN3_PROVIDER_ID_MAX
     || strlen(metadata->version) > GEN3_PROVIDER_VERSION_MAX)
        return NULL;
    provider = calloc(1, sizeof(*provider));
    if (provider == NULL)
        return NULL;
    provider->id = CopyString(metadata->id);
    provider->version = CopyString(metadata->version);
    if (provider->id == NULL || provider->version == NULL)
    {
        Gen3ResourceProvider_Destroy(provider);
        return NULL;
    }
    provider->kind = metadata->kind;
    provider->precedence = metadata->precedence;
    return provider;
}

bool Gen3ResourceProvider_GetMetadata(
    const struct Gen3ResourceProvider *provider,
    struct Gen3ResourceProviderMetadata *outMetadata)
{
    if (provider == NULL || outMetadata == NULL)
        return false;
    outMetadata->id = provider->id;
    outMetadata->version = provider->version;
    outMetadata->kind = provider->kind;
    outMetadata->precedence = provider->precedence;
    return true;
}

void Gen3ResourceProvider_Destroy(struct Gen3ResourceProvider *provider)
{
    size_t i;
    if (provider == NULL)
        return;
    free(provider->id);
    free(provider->version);
    for (i = 0; i < provider->count; i++)
    {
        free(provider->entries[i].canonicalName);
        free(provider->entries[i].payload);
    }
    free(provider->entries);
    free(provider);
}

bool Gen3ResourceProvider_Add(struct Gen3ResourceProvider *provider,
                              const char *canonicalName,
                              enum Gen3ResourceType declaredType,
                              uint32_t declaredSchema,
                              const void *payload,
                              size_t payloadSize,
                              bool requiredForProvider,
                              struct Gen3ResourceDiagnosticList *diagnostics)
{
    struct Gen3ProviderEntryOwned *entry;
    if (provider == NULL || provider->finalized || canonicalName == NULL
     || (payload == NULL && payloadSize != 0)
     || Gen3ResourceId_ValidateCanonicalName(canonicalName) != GEN3_RESOURCE_NAME_VALID)
    {
        Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
            GEN3_RESOURCE_REASON_INVALID_ARGUMENT, canonicalName,
            provider != NULL ? provider->id : NULL,
            declaredType, declaredType, declaredSchema, declaredSchema,
            requiredForProvider, false);
        return false;
    }
    if (!GrowArray((void **)&provider->entries, sizeof(*provider->entries),
                   &provider->capacity, provider->count + 1u))
        return false;
    entry = &provider->entries[provider->count];
    memset(entry, 0, sizeof(*entry));
    entry->canonicalName = CopyString(canonicalName);
    if (entry->canonicalName == NULL)
        return false;
    if (payloadSize != 0)
    {
        entry->payload = malloc(payloadSize);
        if (entry->payload == NULL)
        {
            free(entry->canonicalName);
            memset(entry, 0, sizeof(*entry));
            return false;
        }
        memcpy(entry->payload, payload, payloadSize);
    }
    entry->declaredType = declaredType;
    entry->declaredSchema = declaredSchema;
    entry->payloadSize = payloadSize;
    entry->requiredForProvider = requiredForProvider;
    provider->count++;
    return true;
}

static int CompareProviderEntries(const void *left, const void *right)
{
    const struct Gen3ProviderEntryOwned *a = left;
    const struct Gen3ProviderEntryOwned *b = right;
    return strcmp(a->canonicalName, b->canonicalName);
}

bool Gen3ResourceProvider_Finalize(struct Gen3ResourceProvider *provider,
                                   struct Gen3ResourceDiagnosticList *diagnostics)
{
    size_t i;
    if (provider == NULL || provider->finalized)
        return false;
    qsort(provider->entries, provider->count, sizeof(*provider->entries),
          CompareProviderEntries);
    for (i = 1; i < provider->count; i++)
    {
        if (strcmp(provider->entries[i - 1u].canonicalName,
                   provider->entries[i].canonicalName) == 0)
        {
            Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
                GEN3_RESOURCE_REASON_DUPLICATE_PROVIDER_ENTRY,
                provider->entries[i].canonicalName, provider->id,
                provider->entries[i].declaredType,
                provider->entries[i].declaredType,
                provider->entries[i].declaredSchema,
                provider->entries[i].declaredSchema,
                provider->entries[i].requiredForProvider, false);
            return false;
        }
    }
    provider->finalized = true;
    return true;
}

struct Gen3ResourceProvider *Gen3ResourceProvider_Clone(
    const struct Gen3ResourceProvider *source)
{
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *copy;
    size_t i;
    if (source == NULL || !source->finalized)
        return NULL;
    metadata.id = source->id;
    metadata.version = source->version;
    metadata.kind = source->kind;
    metadata.precedence = source->precedence;
    copy = Gen3ResourceProvider_Create(&metadata);
    if (copy == NULL)
        return NULL;
    for (i = 0; i < source->count; i++)
    {
        const struct Gen3ProviderEntryOwned *entry = &source->entries[i];
        if (!Gen3ResourceProvider_Add(copy, entry->canonicalName,
             entry->declaredType, entry->declaredSchema, entry->payload,
             entry->payloadSize, entry->requiredForProvider, NULL))
        {
            Gen3ResourceProvider_Destroy(copy);
            return NULL;
        }
    }
    copy->finalized = true;
    return copy;
}

const struct Gen3ProviderEntryOwned *Gen3ResourceProvider_FindEntry(
    const struct Gen3ResourceProvider *provider,
    const char *canonicalName,
    size_t *outIndex)
{
    size_t low = 0;
    size_t high;
    if (provider == NULL || !provider->finalized || canonicalName == NULL)
        return NULL;
    high = provider->count;
    while (low < high)
    {
        size_t middle = low + (high - low) / 2u;
        int comparison = strcmp(canonicalName, provider->entries[middle].canonicalName);
        if (comparison == 0)
        {
            if (outIndex != NULL)
                *outIndex = middle;
            return &provider->entries[middle];
        }
        if (comparison < 0)
            high = middle;
        else
            low = middle + 1u;
    }
    return NULL;
}

static bool PayloadValid(enum Gen3ResourceType type, const uint8_t *payload, size_t size)
{
    if (payload == NULL || size == 0)
        return false;
    if (type == GEN3_RESOURCE_TYPE_TILE_GRAPHICS)
        return size % 32u == 0;
    if (type == GEN3_RESOURCE_TYPE_PALETTE)
        return size % 2u == 0 && size <= 512u;
    return true;
}

enum Gen3ResourceReason Gen3ResourceProvider_ValidateEntry(
    const struct Gen3ProviderEntryOwned *entry,
    const struct Gen3ResourceContract *contract)
{
    if (entry == NULL || contract == NULL)
        return GEN3_RESOURCE_REASON_INVALID_ARGUMENT;
    if (entry->declaredType != contract->type)
        return GEN3_RESOURCE_REASON_TYPE_MISMATCH;
    if (entry->declaredSchema != contract->schema)
        return GEN3_RESOURCE_REASON_SCHEMA_MISMATCH;
    if (!PayloadValid(contract->type, entry->payload, entry->payloadSize))
        return GEN3_RESOURCE_REASON_INVALID_PAYLOAD;
    return GEN3_RESOURCE_REASON_NONE;
}

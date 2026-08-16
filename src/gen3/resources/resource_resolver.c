#include "resource_internal.h"

#include <stdlib.h>
#include <string.h>

static bool GrowPointerArray(void ***items, size_t *capacity, size_t required)
{
    size_t next;
    void **resized;
    if (*capacity >= required)
        return true;
    next = *capacity == 0 ? 4u : *capacity;
    while (next < required)
    {
        if (next > SIZE_MAX / 2u)
            return false;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(**items))
        return false;
    resized = realloc(*items, next * sizeof(**items));
    if (resized == NULL)
        return false;
    *items = resized;
    *capacity = next;
    return true;
}

struct Gen3ResourceCandidate *Gen3ResourceCandidate_Create(
    const struct Gen3ResourceCatalog *catalog,
    struct Gen3ResourceDiagnosticList *diagnostics)
{
    struct Gen3ResourceCandidate *candidate;
    if (catalog == NULL || !catalog->finalized)
    {
        Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
            GEN3_RESOURCE_REASON_CATALOG_NOT_FINALIZED, NULL, NULL,
            GEN3_RESOURCE_TYPE_INVALID, GEN3_RESOURCE_TYPE_INVALID,
            0, 0, false, false);
        return NULL;
    }
    candidate = calloc(1, sizeof(*candidate));
    if (candidate == NULL)
        return NULL;
    candidate->catalog = Gen3ResourceCatalog_Clone(catalog);
    if (candidate->catalog == NULL)
    {
        free(candidate);
        return NULL;
    }
    return candidate;
}

void Gen3ResourceCandidate_Destroy(struct Gen3ResourceCandidate *candidate)
{
    size_t i;
    if (candidate == NULL)
        return;
    Gen3ResourceCatalog_Destroy(candidate->catalog);
    for (i = 0; i < candidate->providerCount; i++)
        Gen3ResourceProvider_Destroy(candidate->providers[i]);
    free(candidate->providers);
    free(candidate);
}

bool Gen3ResourceCandidate_AddProvider(
    struct Gen3ResourceCandidate *candidate,
    const struct Gen3ResourceProvider *provider,
    struct Gen3ResourceDiagnosticList *diagnostics)
{
    struct Gen3ResourceProvider *copy;
    if (candidate == NULL || provider == NULL || !provider->finalized)
    {
        Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
            GEN3_RESOURCE_REASON_PROVIDER_NOT_FINALIZED, NULL,
            provider != NULL ? provider->id : NULL,
            GEN3_RESOURCE_TYPE_INVALID, GEN3_RESOURCE_TYPE_INVALID,
            0, 0, false, false);
        return false;
    }
    copy = Gen3ResourceProvider_Clone(provider);
    if (copy == NULL)
        return false;
    if (!GrowPointerArray((void ***)&candidate->providers,
                          &candidate->providerCapacity,
                          candidate->providerCount + 1u))
    {
        Gen3ResourceProvider_Destroy(copy);
        return false;
    }
    candidate->providers[candidate->providerCount++] = copy;
    return true;
}

static int CompareProviderPointers(const void *left, const void *right)
{
    const struct Gen3ResourceProvider *a = *(const struct Gen3ResourceProvider *const *)left;
    const struct Gen3ResourceProvider *b = *(const struct Gen3ResourceProvider *const *)right;
    if (a->precedence < b->precedence)
        return -1;
    if (a->precedence > b->precedence)
        return 1;
    return strcmp(a->id, b->id);
}

static bool IsBaseProvider(const struct Gen3ResourceProvider *provider)
{
    return provider->kind == GEN3_PROVIDER_BOOTSTRAP
        || provider->kind == GEN3_PROVIDER_LEGACY_COMPILED
        || provider->kind == GEN3_PROVIDER_ROM_BASE;
}

static bool AppendEntryDiagnostic(struct Gen3ResourceDiagnosticList *diagnostics,
                                  const struct Gen3ResourceContract *contract,
                                  const struct Gen3ResourceProvider *provider,
                                  const struct Gen3ProviderEntryOwned *entry,
                                  enum Gen3ResourceReason reason)
{
    return Gen3ResourceDiagnostics_Append(diagnostics,
        entry->requiredForProvider ? GEN3_DIAGNOSTIC_ERROR : GEN3_DIAGNOSTIC_WARNING,
        reason, entry->canonicalName, provider->id,
        contract != NULL ? contract->type : GEN3_RESOURCE_TYPE_INVALID,
        entry->declaredType,
        contract != NULL ? contract->schema : 0,
        entry->declaredSchema,
        entry->requiredForProvider,
        contract != NULL && contract->requiredForBase);
}

static struct Gen3ResourceSnapshot *CloneCandidateToSnapshot(
    const struct Gen3ResourceCandidate *candidate)
{
    struct Gen3ResourceSnapshot *snapshot;
    size_t i;
    snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL)
        return NULL;
    snapshot->catalog = Gen3ResourceCatalog_Clone(candidate->catalog);
    if (snapshot->catalog == NULL)
        goto fail;
    if (candidate->providerCount != 0)
    {
        snapshot->providers = calloc(candidate->providerCount, sizeof(*snapshot->providers));
        if (snapshot->providers == NULL)
            goto fail;
    }
    for (i = 0; i < candidate->providerCount; i++)
    {
        snapshot->providers[i] = Gen3ResourceProvider_Clone(candidate->providers[i]);
        if (snapshot->providers[i] == NULL)
            goto fail;
        snapshot->providerCount++;
    }
    if (snapshot->catalog->count != 0)
    {
        snapshot->resolved = calloc(snapshot->catalog->count, sizeof(*snapshot->resolved));
        if (snapshot->resolved == NULL)
            goto fail;
    }
    /* qsort on a NULL base (zero providers) is UB under UBSan; sorting fewer
     * than two providers is a no-op anyway. */
    if (snapshot->providerCount > 1)
        qsort(snapshot->providers, snapshot->providerCount,
              sizeof(*snapshot->providers), CompareProviderPointers);
    return snapshot;
fail:
    Gen3ResourceSnapshot_Destroy(snapshot);
    return NULL;
}

bool Gen3ResourceCandidate_Build(
    const struct Gen3ResourceCandidate *candidate,
    struct Gen3ResourceSnapshot **outSnapshot,
    struct Gen3ResourceDiagnosticList *diagnostics)
{
    struct Gen3ResourceSnapshot *snapshot;
    bool valid = true;
    size_t i;
    size_t j;

    if (outSnapshot != NULL)
        *outSnapshot = NULL;
    if (candidate == NULL || outSnapshot == NULL)
        return false;
    snapshot = CloneCandidateToSnapshot(candidate);
    if (snapshot == NULL)
        return false;

    for (i = 0; i < snapshot->providerCount; i++)
    {
        if (i != 0 && snapshot->providers[i - 1u]->precedence == snapshot->providers[i]->precedence)
        {
            Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
                GEN3_RESOURCE_REASON_DUPLICATE_PROVIDER_PRECEDENCE, NULL,
                snapshot->providers[i]->id,
                GEN3_RESOURCE_TYPE_INVALID, GEN3_RESOURCE_TYPE_INVALID,
                0, 0, false, false);
            valid = false;
        }
        for (j = 0; j < i; j++)
        {
            if (strcmp(snapshot->providers[j]->id, snapshot->providers[i]->id) == 0)
            {
                Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
                    GEN3_RESOURCE_REASON_DUPLICATE_PROVIDER_ID, NULL,
                    snapshot->providers[i]->id,
                    GEN3_RESOURCE_TYPE_INVALID, GEN3_RESOURCE_TYPE_INVALID,
                    0, 0, false, false);
                valid = false;
                break;
            }
        }
    }

    for (i = 0; i < snapshot->providerCount; i++)
    {
        const struct Gen3ResourceProvider *provider = snapshot->providers[i];
        for (j = 0; j < provider->count; j++)
        {
            const struct Gen3ProviderEntryOwned *entry = &provider->entries[j];
            const struct Gen3ResourceContract *contract =
                Gen3ResourceCatalog_Find(snapshot->catalog, entry->canonicalName);
            enum Gen3ResourceReason reason = contract == NULL
                ? GEN3_RESOURCE_REASON_UNKNOWN_RESOURCE
                : Gen3ResourceProvider_ValidateEntry(entry, contract);
            if (reason != GEN3_RESOURCE_REASON_NONE)
            {
                AppendEntryDiagnostic(diagnostics, contract, provider, entry, reason);
                if (entry->requiredForProvider)
                    valid = false;
            }
        }
    }

    for (i = 0; i < snapshot->catalog->count; i++)
    {
        const struct Gen3ResourceContract *contract = &snapshot->catalog->entries[i].contract;
        bool validBase = false;
        if (!contract->requiredForBase)
            continue;
        for (j = snapshot->providerCount; j > 0; j--)
        {
            const struct Gen3ResourceProvider *provider = snapshot->providers[j - 1u];
            const struct Gen3ProviderEntryOwned *entry;
            if (!IsBaseProvider(provider))
                continue;
            entry = Gen3ResourceProvider_FindEntry(provider, contract->canonicalName, NULL);
            if (entry != NULL
             && Gen3ResourceProvider_ValidateEntry(entry, contract) == GEN3_RESOURCE_REASON_NONE)
            {
                validBase = true;
                break;
            }
        }
        if (!validBase)
        {
            Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
                GEN3_RESOURCE_REASON_MISSING_REQUIRED_BASE_RESOURCE,
                contract->canonicalName, NULL, contract->type,
                GEN3_RESOURCE_TYPE_INVALID, contract->schema, 0, false, true);
            valid = false;
        }
    }

    if (!valid)
    {
        Gen3ResourceSnapshot_Destroy(snapshot);
        return false;
    }

    for (i = 0; i < snapshot->catalog->count; i++)
    {
        const struct Gen3ResourceContract *contract = &snapshot->catalog->entries[i].contract;
        for (j = snapshot->providerCount; j > 0; j--)
        {
            size_t entryIndex;
            const struct Gen3ProviderEntryOwned *entry =
                Gen3ResourceProvider_FindEntry(snapshot->providers[j - 1u],
                                                contract->canonicalName,
                                                &entryIndex);
            if (entry != NULL
             && Gen3ResourceProvider_ValidateEntry(entry, contract) == GEN3_RESOURCE_REASON_NONE)
            {
                snapshot->resolved[i].resolved = true;
                snapshot->resolved[i].providerIndex = j - 1u;
                snapshot->resolved[i].providerEntryIndex = entryIndex;
                break;
            }
        }
    }
    *outSnapshot = snapshot;
    return true;
}

void Gen3ResourceSnapshot_Destroy(struct Gen3ResourceSnapshot *snapshot)
{
    size_t i;
    if (snapshot == NULL)
        return;
    Gen3ResourceCatalog_Destroy(snapshot->catalog);
    for (i = 0; i < snapshot->providerCount; i++)
        Gen3ResourceProvider_Destroy(snapshot->providers[i]);
    free(snapshot->providers);
    free(snapshot->resolved);
    free(snapshot);
}

size_t Gen3ResourceSnapshot_Count(const struct Gen3ResourceSnapshot *snapshot)
{
    return snapshot != NULL ? snapshot->catalog->count : 0;
}

enum Gen3ResourceResult Gen3ResourceSnapshot_FindHandle(
    const struct Gen3ResourceSnapshot *snapshot,
    const char *canonicalName,
    Gen3ResourceHandle *outHandle)
{
    const struct Gen3ResourceContract *contract;
    if (snapshot == NULL || canonicalName == NULL || outHandle == NULL)
        return GEN3_RESOURCE_NOT_FOUND;
    contract = Gen3ResourceCatalog_Find(snapshot->catalog, canonicalName);
    if (contract == NULL)
        return GEN3_RESOURCE_NOT_FOUND;
    *outHandle = contract->handle;
    return GEN3_RESOURCE_OK;
}

enum Gen3ResourceResult Gen3ResourceSnapshot_Resolve(
    const struct Gen3ResourceSnapshot *snapshot,
    Gen3ResourceHandle handle,
    enum Gen3ResourceType expectedType,
    uint32_t expectedSchema,
    struct Gen3ResourceView *outView)
{
    const struct Gen3ResourceContract *contract;
    const struct Gen3ResolvedOwned *resolved;
    const struct Gen3ResourceProvider *provider;
    const struct Gen3ProviderEntryOwned *entry;
    if (snapshot == NULL || outView == NULL || handle >= snapshot->catalog->count)
        return GEN3_RESOURCE_INVALID_HANDLE;
    contract = &snapshot->catalog->entries[handle].contract;
    if (contract->type != expectedType)
        return GEN3_RESOURCE_WRONG_TYPE;
    if (contract->schema != expectedSchema)
        return GEN3_RESOURCE_WRONG_SCHEMA;
    resolved = &snapshot->resolved[handle];
    if (!resolved->resolved)
        return GEN3_RESOURCE_NOT_FOUND;
    provider = snapshot->providers[resolved->providerIndex];
    entry = &provider->entries[resolved->providerEntryIndex];
    memset(outView, 0, sizeof(*outView));
    outView->handle = handle;
    outView->key = contract->key;
    outView->canonicalName = contract->canonicalName;
    outView->type = contract->type;
    outView->schema = contract->schema;
    outView->payload = entry->payload;
    outView->payloadSize = entry->payloadSize;
    outView->winningProviderId = provider->id;
    outView->winningProviderVersion = provider->version;
    outView->winningProviderPrecedence = provider->precedence;
    return GEN3_RESOURCE_OK;
}

enum Gen3ResourceResult Gen3ResourceSnapshot_GetTileGraphics(
    const struct Gen3ResourceSnapshot *snapshot,
    Gen3ResourceHandle handle,
    uint32_t expectedSchema,
    struct Gen3TileGraphicsView *outView)
{
    struct Gen3ResourceView view;
    enum Gen3ResourceResult result;
    if (outView == NULL)
        return GEN3_RESOURCE_INVALID_HANDLE;
    result = Gen3ResourceSnapshot_Resolve(snapshot, handle,
        GEN3_RESOURCE_TYPE_TILE_GRAPHICS, expectedSchema, &view);
    if (result == GEN3_RESOURCE_OK)
    {
        outView->bytes = view.payload;
        outView->size = view.payloadSize;
    }
    return result;
}

enum Gen3ResourceResult Gen3ResourceSnapshot_GetPalette(
    const struct Gen3ResourceSnapshot *snapshot,
    Gen3ResourceHandle handle,
    uint32_t expectedSchema,
    struct Gen3PaletteView *outView)
{
    struct Gen3ResourceView view;
    enum Gen3ResourceResult result;
    if (outView == NULL)
        return GEN3_RESOURCE_INVALID_HANDLE;
    result = Gen3ResourceSnapshot_Resolve(snapshot, handle,
        GEN3_RESOURCE_TYPE_PALETTE, expectedSchema, &view);
    if (result == GEN3_RESOURCE_OK)
    {
        outView->bytes = view.payload;
        outView->size = view.payloadSize;
        outView->colorCount = view.payloadSize / 2u;
    }
    return result;
}

void Gen3ResourceTrace_Init(struct Gen3ResourceTrace *trace)
{
    if (trace != NULL)
        memset(trace, 0, sizeof(*trace));
}

void Gen3ResourceTrace_Destroy(struct Gen3ResourceTrace *trace)
{
    if (trace == NULL)
        return;
    free(trace->items);
    memset(trace, 0, sizeof(*trace));
}

static bool TraceAppend(struct Gen3ResourceTrace *trace,
                        const struct Gen3ResourceProvider *provider,
                        enum Gen3ResourceTraceDisposition disposition,
                        enum Gen3ResourceReason reason)
{
    struct Gen3ResourceTraceRecord *resized;
    struct Gen3ResourceTraceRecord *record;
    size_t next;
    if (trace->count == trace->capacity)
    {
        next = trace->capacity == 0 ? 4u : trace->capacity * 2u;
        resized = realloc(trace->items, next * sizeof(*trace->items));
        if (resized == NULL)
            return false;
        trace->items = resized;
        trace->capacity = next;
    }
    record = &trace->items[trace->count++];
    memset(record, 0, sizeof(*record));
    strncpy(record->providerId, provider->id, sizeof(record->providerId) - 1u);
    strncpy(record->providerVersion, provider->version,
            sizeof(record->providerVersion) - 1u);
    record->precedence = provider->precedence;
    record->disposition = disposition;
    record->reason = reason;
    return true;
}

bool Gen3ResourceSnapshot_Trace(
    const struct Gen3ResourceSnapshot *snapshot,
    Gen3ResourceHandle handle,
    struct Gen3ResourceTrace *trace)
{
    const struct Gen3ResourceContract *contract;
    bool foundWinner = false;
    size_t i;
    if (snapshot == NULL || trace == NULL || handle >= snapshot->catalog->count)
        return false;
    Gen3ResourceTrace_Destroy(trace);
    contract = &snapshot->catalog->entries[handle].contract;
    for (i = snapshot->providerCount; i > 0; i--)
    {
        const struct Gen3ResourceProvider *provider = snapshot->providers[i - 1u];
        const struct Gen3ProviderEntryOwned *entry =
            Gen3ResourceProvider_FindEntry(provider, contract->canonicalName, NULL);
        enum Gen3ResourceReason reason;
        enum Gen3ResourceTraceDisposition disposition;
        if (entry == NULL)
            continue;
        reason = Gen3ResourceProvider_ValidateEntry(entry, contract);
        if (reason != GEN3_RESOURCE_REASON_NONE)
            disposition = GEN3_TRACE_REJECTED;
        else if (!foundWinner)
        {
            disposition = GEN3_TRACE_WINNER;
            foundWinner = true;
        }
        else
            disposition = GEN3_TRACE_FALLBACK;
        if (!TraceAppend(trace, provider, disposition, reason))
        {
            Gen3ResourceTrace_Destroy(trace);
            return false;
        }
    }
    return true;
}

void Gen3ResourceRegistry_Init(struct Gen3ResourceRegistry *registry)
{
    if (registry != NULL)
        registry->active = NULL;
}

void Gen3ResourceRegistry_Destroy(struct Gen3ResourceRegistry *registry)
{
    if (registry == NULL)
        return;
    Gen3ResourceSnapshot_Destroy(registry->active);
    registry->active = NULL;
}

void Gen3ResourceRegistry_Publish(struct Gen3ResourceRegistry *registry,
                                  struct Gen3ResourceSnapshot *snapshot)
{
    struct Gen3ResourceSnapshot *previous;
    if (registry == NULL || snapshot == NULL)
        return;
    previous = registry->active;
    registry->active = snapshot;
    Gen3ResourceSnapshot_Destroy(previous);
}

const struct Gen3ResourceSnapshot *Gen3ResourceRegistry_Active(
    const struct Gen3ResourceRegistry *registry)
{
    return registry != NULL ? registry->active : NULL;
}

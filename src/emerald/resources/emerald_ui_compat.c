/* R15: UI graphics native compatibility publication.
 * See emerald_ui_compat.h for the contract. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include "global.h"
#include "data.h"

#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/resource_lz.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_ui_compat.h"
#include "emerald/resources/ui_slots.generated.h"
#include "emerald/resources/ui_struct_population.h"

static struct EmeraldResourceCompatibilityImage *sImage;

static int32_t FindResourceIndex(const char *id)
{
    size_t low = 0u;
    size_t high = UI_RESOURCE_COUNT;
    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        int cmp = strcmp(kUIResources[mid].id, id);
        if (cmp == 0)
            return (int32_t)mid;
        if (cmp < 0)
            low = mid + 1u;
        else
            high = mid;
    }
    return -1;
}

const void *UI_Get(const char *id)
{
    int32_t ri;
    if (id == NULL || sImage == NULL)
        return NULL;
    ri = FindResourceIndex(id);
    if (ri < 0)
        return NULL;
    return EmeraldResourceCompatImage_GetStream(sImage, (size_t)ri);
}

static void ClearDiagnostics(struct EmeraldResourceCompatDiagnostics *diag)
{
    if (diag != NULL)
        memset(diag, 0, sizeof(*diag));
}

static enum EmeraldResourceCompatStatus
ResolveResource(const struct Gen3ResourceSnapshot *snapshot,
                size_t resourceIndex,
                struct Gen3ResourceView *outView,
                struct EmeraldResourceCompatDiagnostics *diag)
{
    const struct UIResource *r = &kUIResources[resourceIndex];
    Gen3ResourceHandle handle;
    enum Gen3ResourceResult result;

    ClearDiagnostics(diag);
    if (snapshot == NULL || outView == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
    if (diag != NULL)
    {
        snprintf(diag->canonicalName, sizeof(diag->canonicalName),
                 "%s", r->id);
        diag->expectedSchema = r->schema;
        diag->expectedSize = r->expectedSize;
    }
    result = Gen3ResourceSnapshot_FindHandle(snapshot, r->id, &handle);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    result = Gen3ResourceSnapshot_Resolve(snapshot, handle, r->type,
                                          r->schema, outView);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    if (outView->payloadSize != r->expectedSize)
    {
        if (diag != NULL)
            diag->actualSize = (uint32_t)outView->payloadSize;
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    }
    return EMERALD_COMPAT_OK;
}

static enum EmeraldResourceCompatStatus
EncodeStreams(const struct Gen3ResourceView *views,
              uint8_t **outArena,
              struct EmeraldResourceCompatSourceEntry *entries)
{
    size_t total = 0u;
    size_t offset = 0u;
    uint8_t *arena;
    size_t i;

    for (i = 0u; i < UI_RESOURCE_COUNT; i++)
    {
        size_t encoded =
            Gen3LzLiteral_EncodedSize((uint32_t)views[i].payloadSize);
        if (encoded == SIZE_MAX)
            return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
        total += encoded;
    }
    arena = (uint8_t *)malloc(total != 0u ? total : 1u);
    if (arena == NULL)
        return EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
    for (i = 0u; i < UI_RESOURCE_COUNT; i++)
    {
        size_t encoded;
        enum Gen3LzResult lz;
        lz = Gen3LzLiteral_Encode(views[i].payload, (uint32_t)views[i].payloadSize,
                                  arena + offset, total - offset, &encoded);
        if (lz != GEN3_LZ_OK)
        {
            free(arena);
            return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
        }
        entries[i].payload = arena + offset;
        entries[i].payloadSize = (uint32_t)encoded;
        offset += encoded;
    }
    *outArena = arena;
    return EMERALD_COMPAT_OK;
}

enum EmeraldResourceCompatStatus
EmeraldUICompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diag)
{
    struct EmeraldResourceCompatSourceEntry *entries;
    struct Gen3ResourceView *views;
    struct EmeraldResourceCompatibilityImage *image = NULL;
    uint8_t *encodedArena = NULL;
    enum EmeraldResourceCompatStatus status;
    size_t i;

    ClearDiagnostics(diag);
    if (snapshot == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;

    entries = (struct EmeraldResourceCompatSourceEntry *)calloc(
        UI_RESOURCE_COUNT, sizeof(entries[0]));
    views = (struct Gen3ResourceView *)calloc(
        UI_RESOURCE_COUNT, sizeof(views[0]));
    if (entries == NULL || views == NULL)
    {
        free(entries);
        free(views);
        return EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
    }

    for (i = 0u; i < UI_RESOURCE_COUNT; i++)
    {
        status = ResolveResource(snapshot, i, &views[i], diag);
        if (status != EMERALD_COMPAT_OK)
            goto done;
    }
    status = EncodeStreams(views, &encodedArena, entries);
    if (status != EMERALD_COMPAT_OK)
        goto done;
    for (i = 0u; i < UI_RESOURCE_COUNT; i++)
    {
        const struct UIResource *r = &kUIResources[i];
        entries[i].canonicalName = r->id;
        entries[i].type = r->type;
        entries[i].schema = r->schema;
        entries[i].expectedSize = r->expectedSize;
        entries[i].encoding = EMERALD_COMPAT_ENTRY_GBA_LZ;
    }
    status = EmeraldResourceCompatImage_CreateFamily(entries,
        UI_RESOURCE_COUNT, &image, diag);
    if (status != EMERALD_COMPAT_OK)
        goto done;
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = image;
    /* R15: republish every mutable native UI struct from the new image.
     * A NULL image degrades every struct's pointer fields to NULL (the
     * scalar metadata is static and never lost). */
    EmeraldUICompat_PopulateStructs();
    status = EMERALD_COMPAT_OK;
done:
    free(encodedArena);
    free(entries);
    free(views);
    return status;
}

void EmeraldUICompat_ClearMigratedEntries(void)
{
    /* Null the mutable native struct pointers, then drop the image;
     * UI_Get returns NULL after clear. */
    EmeraldUICompat_ClearStructPointers();
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = NULL;
}

#endif /* defined(PLATFORM_SDL2) && defined(NATIVE_LINUX) */
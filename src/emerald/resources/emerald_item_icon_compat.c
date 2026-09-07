/* R15 Phase 5: item-icon graphics native compatibility publication.
 * See emerald_item_icon_compat.h for the contract. */

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
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_item_icon_compat.h"
#include "emerald/resources/item_icon_slots.generated.h"

/* gItemIconTable is non-const on native (declared in item_icon_table.h). */
extern u32 *gItemIconTable[][2];

static struct EmeraldResourceCompatibilityImage *sImage;

static void ClearDiagnostics(struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    if (diagnostics != NULL)
        memset(diagnostics, 0, sizeof(*diagnostics));
}

static enum EmeraldResourceCompatStatus
ResolveIconResource(const struct Gen3ResourceSnapshot *snapshot,
                    size_t resourceIndex,
                    struct Gen3ResourceView *outView,
                    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    const struct ItemIconResource *r =
        &kItemIconResources[resourceIndex];
    Gen3ResourceHandle handle;
    enum Gen3ResourceResult result;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || outView == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
    if (diagnostics != NULL)
    {
        snprintf(diagnostics->canonicalName, sizeof(diagnostics->canonicalName),
                 "%s", r->id);
        snprintf(diagnostics->stage, sizeof(diagnostics->stage), "resolve");
        diagnostics->expectedSchema = r->schema;
        diagnostics->expectedSize = r->expectedSize;
    }

    result = Gen3ResourceSnapshot_FindHandle(snapshot, r->id, &handle);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    result = Gen3ResourceSnapshot_Resolve(snapshot, handle, r->type,
                                          r->schema, outView);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    if (outView->winningProviderId == NULL
     || strcmp(outView->winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0
     || outView->payloadSize != r->expectedSize)
    {
        if (diagnostics != NULL)
            diagnostics->actualSize = (uint32_t)outView->payloadSize;
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    }
    return EMERALD_COMPAT_OK;
}

static enum EmeraldResourceCompatStatus
EncodeIconStreams(const struct Gen3ResourceView *views,
                  uint8_t **outArena,
                  struct EmeraldResourceCompatSourceEntry *entries)
{
    size_t total = 0u;
    size_t offset = 0u;
    uint8_t *arena;
    size_t i;

    for (i = 0u; i < ITEM_ICON_RESOURCE_COUNT; i++)
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
    for (i = 0u; i < ITEM_ICON_RESOURCE_COUNT; i++)
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

/* Publish the item-icon table from the validated image. */
static enum EmeraldResourceCompatStatus
PublishIconTable(const struct EmeraldResourceCompatibilityImage *image,
                 struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    size_t i;

    ClearDiagnostics(diagnostics);
    if (image == NULL
     || EmeraldResourceCompatImage_GetEntryCount(image)
            != ITEM_ICON_RESOURCE_COUNT)
        return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
    for (i = 0u; i < ITEM_ICON_TABLE_ROWS; i++)
    {
        int32_t riIcon = kItemIconSlots[i * 2];
        int32_t riPal = kItemIconSlots[i * 2 + 1];
        if (riIcon >= 0 && riPal >= 0)
        {
            gItemIconTable[i][0] = (u32 *)(const void *)
                EmeraldResourceCompatImage_GetStream(image, (size_t)riIcon);
            gItemIconTable[i][1] = (u32 *)(const void *)
                EmeraldResourceCompatImage_GetStream(image, (size_t)riPal);
        }
    }
    return EMERALD_COMPAT_OK;
}

static int32_t FindResourceIndex(const char *id)
{
    size_t low = 0u;
    size_t high = ITEM_ICON_RESOURCE_COUNT;
    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        int cmp = strcmp(kItemIconResources[mid].id, id);
        if (cmp == 0)
            return (int32_t)mid;
        if (cmp < 0)
            low = mid + 1u;
        else
            high = mid;
    }
    return -1;
}

const void *ItemIcon_Get(const char *id)
{
    int32_t ri;
    if (id == NULL || sImage == NULL)
        return NULL;
    ri = FindResourceIndex(id);
    if (ri < 0)
        return NULL;
    return EmeraldResourceCompatImage_GetStream(sImage, (size_t)ri);
}

enum EmeraldResourceCompatStatus
EmeraldItemIconCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    struct EmeraldResourceCompatSourceEntry *entries;
    struct Gen3ResourceView *views;
    struct EmeraldResourceCompatibilityImage *image = NULL;
    uint8_t *encodedArena = NULL;
    enum EmeraldResourceCompatStatus status;
    size_t i;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;

    entries = (struct EmeraldResourceCompatSourceEntry *)calloc(
        ITEM_ICON_RESOURCE_COUNT, sizeof(entries[0]));
    views = (struct Gen3ResourceView *)calloc(
        ITEM_ICON_RESOURCE_COUNT, sizeof(views[0]));
    if (entries == NULL || views == NULL)
    {
        free(entries);
        free(views);
        return EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
    }

    for (i = 0u; i < ITEM_ICON_RESOURCE_COUNT; i++)
    {
        status = ResolveIconResource(snapshot, i, &views[i], diagnostics);
        if (status != EMERALD_COMPAT_OK)
            goto done;
    }
    status = EncodeIconStreams(views, &encodedArena, entries);
    if (status != EMERALD_COMPAT_OK)
        goto done;
    for (i = 0u; i < ITEM_ICON_RESOURCE_COUNT; i++)
    {
        const struct ItemIconResource *r =
            &kItemIconResources[i];
        entries[i].canonicalName = r->id;
        entries[i].type = r->type;
        entries[i].schema = r->schema;
        entries[i].expectedSize = r->expectedSize;
        entries[i].encoding = EMERALD_COMPAT_ENTRY_GBA_LZ;
    }
    status = EmeraldResourceCompatImage_CreateFamily(entries,
        ITEM_ICON_RESOURCE_COUNT, &image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
        goto done;
    status = PublishIconTable(image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
    {
        EmeraldResourceCompatImage_Destroy(image);
        goto done;
    }
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = image;
    status = EMERALD_COMPAT_OK;
done:
    free(encodedArena);
    free(entries);
    free(views);
    return status;
}

void EmeraldItemIconCompat_ClearMigratedEntries(void)
{
    size_t i;
    for (i = 0u; i < ITEM_ICON_TABLE_ROWS; i++)
    {
        gItemIconTable[i][0] = NULL;
        gItemIconTable[i][1] = NULL;
    }
}

#endif /* defined(PLATFORM_SDL2) && defined(NATIVE_LINUX) */
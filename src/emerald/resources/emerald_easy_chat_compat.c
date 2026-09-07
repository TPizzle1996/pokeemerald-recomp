/* R15: Easy-chat word native compatibility publication.
 * See emerald_easy_chat_compat.h for the contract. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include "global.h"
#include "data.h"

#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_easy_chat_compat.h"
#include "emerald/resources/easy_chat_slots.generated.h"

static struct EmeraldResourceCompatibilityImage *sImage;

static int32_t FindResourceIndex(const char *id)
{
    size_t low = 0u;
    size_t high = EC_RESOURCE_COUNT;
    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        int cmp = strcmp(kECResources[mid].id, id);
        if (cmp == 0)
            return (int32_t)mid;
        if (cmp < 0)
            low = mid + 1u;
        else
            high = mid;
    }
    return -1;
}

const void *EC_GetWord(const char *id)
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
    const struct ECResource *r = &kECResources[resourceIndex];
    Gen3ResourceHandle handle;
    enum Gen3ResourceResult result;

    ClearDiagnostics(diag);
    if (snapshot == NULL || outView == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
    if (diag != NULL)
    {
        snprintf(diag->canonicalName, sizeof(diag->canonicalName),
                 "%s", r->id);
        diag->expectedSchema = 1u;
        diag->expectedSize = r->expectedSize;
    }
    result = Gen3ResourceSnapshot_FindHandle(snapshot, r->id, &handle);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    result = Gen3ResourceSnapshot_Resolve(snapshot, handle,
                                          GEN3_RESOURCE_TYPE_BINARY,
                                          1u, outView);
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

enum EmeraldResourceCompatStatus
EmeraldEasyChatCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diag)
{
    struct EmeraldResourceCompatSourceEntry *entries;
    struct Gen3ResourceView *views;
    struct EmeraldResourceCompatibilityImage *image = NULL;
    enum EmeraldResourceCompatStatus status;
    size_t i;

    ClearDiagnostics(diag);
    if (snapshot == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;

    entries = (struct EmeraldResourceCompatSourceEntry *)calloc(
        EC_RESOURCE_COUNT, sizeof(entries[0]));
    views = (struct Gen3ResourceView *)calloc(
        EC_RESOURCE_COUNT, sizeof(views[0]));
    if (entries == NULL || views == NULL)
    {
        free(entries);
        free(views);
        return EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
    }

    for (i = 0u; i < EC_RESOURCE_COUNT; i++)
    {
        status = ResolveResource(snapshot, i, &views[i], diag);
        if (status != EMERALD_COMPAT_OK)
            goto done;
    }
    /* Easy-chat words are raw binary payloads (no LZ encoding) */
    for (i = 0u; i < EC_RESOURCE_COUNT; i++)
    {
        const struct ECResource *r = &kECResources[i];
        entries[i].canonicalName = r->id;
        entries[i].type = GEN3_RESOURCE_TYPE_BINARY;
        entries[i].schema = 1u;
        entries[i].expectedSize = r->expectedSize;
        entries[i].payload = views[i].payload;
        entries[i].payloadSize = (uint32_t)views[i].payloadSize;
        entries[i].encoding = EMERALD_COMPAT_ENTRY_RAW;
    }
    status = EmeraldResourceCompatImage_CreateFamily(entries,
        EC_RESOURCE_COUNT, &image, diag);
    if (status != EMERALD_COMPAT_OK)
        goto done;
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = image;
    status = EMERALD_COMPAT_OK;
done:
    free(entries);
    free(views);
    return status;
}

void EmeraldEasyChatCompat_ClearMigratedEntries(void)
{
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = NULL;
}

#endif /* defined(PLATFORM_SDL2) && defined(NATIVE_LINUX) */
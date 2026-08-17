/* R11-D: raw map blockdata native compatibility publication.
 * See emerald_layout_compat.h for the contract. */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include "global.h"

#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_layout_compat.h"

/* The tileset structs are compiled STRUCTURAL metadata (their .tiles/
 * .palettes/.metatiles members were published by the R11-C seam); the
 * layout records reference them by symbol. The include-once toggle stays
 * undefined here so this TU sees the extern declarations. */
#include "emerald/resources/tileset_native.generated.h"

/* Exactly one TU (this one) defines the session-published native slots. */
#define LAYOUT_NATIVE_DEFINE
#include "emerald/resources/layout_native.generated.h"
#undef LAYOUT_NATIVE_STRUCT

/* ------------------------------------------------------------------ */
/* Row table: the frames header is re-included once per row kind.     */
/* (Rows carry no trailing ';' - the includer supplies the separator, */
/* the object-event precedent.)                                       */
/* ------------------------------------------------------------------ */

struct LayoutRow
{
    const char *symbol;
    const char *blockdataId;
    u32 blockdataSize;
    const char *borderId;
    u32 borderSize;
};

static const struct LayoutRow sLayoutRows[] =
{
#define LAYOUT_RECORD(symbol, blockdataId, blockdataSize, borderId, borderSize) \
    { #symbol, blockdataId, blockdataSize, borderId, borderSize },
#include "emerald/resources/layout_frames.generated.h"
#undef LAYOUT_RECORD
#undef LAYOUT_FRAMES_GENERATED_H
};

#define LAYOUT_ROW_COUNT   (sizeof(sLayoutRows) / sizeof(sLayoutRows[0]))
#define LAYOUT_ENTRY_COUNT 882u

/* One image entry: identity + verification + encoding invariants. */
struct LayoutImageEntry
{
    const char *id;
    enum Gen3ResourceType type;
    u32 schema;
    u32 expectedSize;
    enum EmeraldResourceCompatEntryEncoding encoding;
};

static struct LayoutImageEntry sEntries[LAYOUT_ROW_COUNT * 2u];
static size_t sEntryCount;

/* Session-global state: the published image is retained for the process
 * session and released only by EmeraldLayoutCompat_Shutdown. */
static struct EmeraldResourceCompatibilityImage *sImage;

static void ClearDiagnostics(struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    if (diagnostics != NULL)
        memset(diagnostics, 0, sizeof(*diagnostics));
}

static bool32 ClearDiagnosticsForResolve(
    struct EmeraldResourceCompatDiagnostics *diagnostics, const char *id)
{
    ClearDiagnostics(diagnostics);
    if (diagnostics != NULL)
    {
        snprintf(diagnostics->canonicalName, sizeof(diagnostics->canonicalName),
                 "%s", id != NULL ? id : "");
        snprintf(diagnostics->stage, sizeof(diagnostics->stage), "resolve");
    }
    return TRUE;
}

/* Resolve one resource through the normal M0/M1 snapshot and verify
 * identity/winner/size. */
static enum EmeraldResourceCompatStatus ResolveResource(
    const struct Gen3ResourceSnapshot *snapshot, const char *id,
    enum Gen3ResourceType type, u32 schema, u32 expectedSize,
    struct Gen3ResourceView *outView,
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    Gen3ResourceHandle handle;
    enum Gen3ResourceResult result;

    ClearDiagnosticsForResolve(diagnostics, id);
    if (snapshot == NULL || id == NULL || outView == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
    result = Gen3ResourceSnapshot_FindHandle(snapshot, id, &handle);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    result = Gen3ResourceSnapshot_Resolve(snapshot, handle, type, schema, outView);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    if (outView->payloadSize != expectedSize)
    {
        if (diagnostics != NULL)
        {
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "size");
            diagnostics->expectedSize = expectedSize;
            diagnostics->actualSize = (u32)outView->payloadSize;
        }
        return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
    }
    if (outView->winningProviderId == NULL
     || strcmp(outView->winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
    {
        if (diagnostics != NULL)
        {
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "winner");
            snprintf(diagnostics->winningProviderId,
                     sizeof(diagnostics->winningProviderId), "%s",
                     outView->winningProviderId != NULL
                         ? outView->winningProviderId : "");
        }
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    }
    return EMERALD_COMPAT_OK;
}

/* Build the image-entry table from the row table: two entries per layout
 * (blockdata, then border), no dedup (441 layouts, 441 of each leaf). The
 * 32 distinct border byte patterns are byte-level aliases inside the pack,
 * not shared resource ids. Idempotent: rebuilt on every TryInitialize. */
static void BuildEntries(void)
{
    size_t i;

    sEntryCount = 0u;
    for (i = 0u; i < LAYOUT_ROW_COUNT; i++)
    {
        const struct LayoutRow *r = &sLayoutRows[i];
        sEntries[sEntryCount].id = r->blockdataId;
        sEntries[sEntryCount].type = GEN3_RESOURCE_TYPE_TILEMAP;
        sEntries[sEntryCount].schema = 1u;
        sEntries[sEntryCount].expectedSize = r->blockdataSize;
        sEntries[sEntryCount].encoding = EMERALD_COMPAT_ENTRY_RAW;
        sEntryCount++;
        sEntries[sEntryCount].id = r->borderId;
        sEntries[sEntryCount].type = GEN3_RESOURCE_TYPE_TILEMAP;
        sEntries[sEntryCount].schema = 1u;
        sEntries[sEntryCount].expectedSize = r->borderSize;
        sEntries[sEntryCount].encoding = EMERALD_COMPAT_ENTRY_RAW;
        sEntryCount++;
    }
}

/* Index of the image entry for a canonical id, or SIZE_MAX. */
static size_t FindEntryIndex(const char *id)
{
    size_t i;

    for (i = 0u; i < sEntryCount; i++)
    {
        if (strcmp(sEntries[i].id, id) == 0)
            return i;
    }
    return (size_t)-1;
}

static const uint8_t *FindStream(const char *id)
{
    size_t index = FindEntryIndex(id);
    if (sImage == NULL || index == (size_t)-1)
        return NULL;
    return EmeraldResourceCompatImage_GetStream(sImage, index);
}

/* Publish every migrated record pointer from the retained image. Runs
 * both at init (fresh image) and at republish (idempotent,
 * allocation-free). */
static void PublishFromImage(void)
{
#define LAYOUT_RECORD(symbol, blockdataId, blockdataSize, borderId, borderSize) \
    do                                                                          \
    {                                                                           \
        const uint8_t *map = FindStream(blockdataId);                           \
        const uint8_t *border = FindStream(borderId);                           \
        if (map != NULL)                                                        \
            symbol.map = (const u16 *)map;                                      \
        if (border != NULL)                                                     \
            symbol.border = (const u16 *)border;                                \
    } while (0);
#include "emerald/resources/layout_frames.generated.h"
#undef LAYOUT_RECORD
#undef LAYOUT_FRAMES_GENERATED_H
}

enum EmeraldResourceCompatStatus
EmeraldLayoutCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    struct EmeraldResourceCompatSourceEntry *entries = NULL;
    struct Gen3ResourceView *views = NULL;
    struct EmeraldResourceCompatibilityImage *image = NULL;
    enum EmeraldResourceCompatStatus status = EMERALD_COMPAT_OK;
    size_t i;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;

    BuildEntries();
    if (sEntryCount != LAYOUT_ENTRY_COUNT)
    {
        if (diagnostics != NULL)
        {
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "build");
            diagnostics->expectedSize = LAYOUT_ENTRY_COUNT;
            diagnostics->actualSize = (u32)sEntryCount;
        }
        return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
    }

    entries = calloc(sEntryCount, sizeof(*entries));
    views = calloc(sEntryCount, sizeof(*views));
    if (entries == NULL || views == NULL)
    {
        status = EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
        goto done;
    }

    /* Phase 1: resolve + verify every resource before anything is built. */
    for (i = 0u; i < sEntryCount; i++)
    {
        status = ResolveResource(snapshot, sEntries[i].id, sEntries[i].type,
                                 sEntries[i].schema, sEntries[i].expectedSize,
                                 &views[i], diagnostics);
        if (status != EMERALD_COMPAT_OK)
            goto done;
    }

    /* Phase 2: source entries. Blockdata/border are raw canonical bytes on
     * both targets - every entry serves the view payload verbatim. */
    for (i = 0u; i < sEntryCount; i++)
    {
        entries[i].canonicalName = sEntries[i].id;
        entries[i].type = sEntries[i].type;
        entries[i].schema = sEntries[i].schema;
        entries[i].expectedSize = sEntries[i].expectedSize;
        entries[i].encoding = sEntries[i].encoding;
        entries[i].payload = views[i].payload;
        entries[i].payloadSize = (uint32_t)views[i].payloadSize;
    }

    /* Phase 3: transactional image build (the stream IS the canonical
     * payload bytes, validated against expectedSize by the image). */
    status = EmeraldResourceCompatImage_CreateFamily(
        entries, sEntryCount, &image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
        goto done;

    /* Phase 4: publish from the validated image, then adopt it. The
     * publication writes only migrated record .map/.border pointers; every
     * other field stays identical. */
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = image;
    PublishFromImage();
    ClearDiagnostics(diagnostics);
    status = EMERALD_COMPAT_OK;
done:
    free(entries);
    free(views);
    if (status != EMERALD_COMPAT_OK)
    {
        EmeraldResourceCompatImage_Destroy(image);
        /* Fail closed: nothing was mutated (publication happens only after
         * the image was built and adopted on the success path). */
    }
    return status;
}

enum EmeraldResourceCompatStatus
EmeraldLayoutCompat_Republish(
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    ClearDiagnostics(diagnostics);
    if (sImage == NULL)
    {
        if (diagnostics != NULL)
            snprintf(diagnostics->stage, sizeof(diagnostics->stage),
                     "republish");
        return EMERALD_COMPAT_ERR_UNAVAILABLE;
    }
    PublishFromImage();
    return EMERALD_COMPAT_OK;
}

void EmeraldLayoutCompat_ClearMigratedEntries(void)
{
#define LAYOUT_RECORD(symbol, blockdataId, blockdataSize, borderId, borderSize) \
    do                                                                          \
    {                                                                           \
        symbol.map = NULL;                                                      \
        symbol.border = NULL;                                                   \
    } while (0);
#include "emerald/resources/layout_frames.generated.h"
#undef LAYOUT_RECORD
#undef LAYOUT_FRAMES_GENERATED_H
}

void EmeraldLayoutCompat_Shutdown(void)
{
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = NULL;
}

const struct EmeraldResourceCompatibilityImage *EmeraldLayoutCompat_GetImage(void)
{
    return sImage;
}

size_t EmeraldLayoutCompat_GetEntryCount(void)
{
    return sImage != NULL ? EmeraldResourceCompatImage_GetEntryCount(sImage)
                          : 0u;
}

uint32_t EmeraldLayoutCompat_GetEntrySchema(size_t i)
{
    if (i >= sEntryCount)
        return 0u;
    return sEntries[i].schema;
}

uint32_t EmeraldLayoutCompat_GetEntryRole(size_t i)
{
    if (i >= sEntryCount)
        return EMERALD_RESOURCE_ROLE_CANONICAL;
    return sEntries[i].encoding == EMERALD_COMPAT_ENTRY_RAW
        ? EMERALD_RESOURCE_ROLE_CANONICAL
        : EMERALD_RESOURCE_ROLE_LEGACY_LZ;
}

#endif /* PLATFORM_SDL2 && NATIVE_LINUX */

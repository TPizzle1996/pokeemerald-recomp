/* R11-B: object-event graphics native compatibility publication.
 * See emerald_object_event_compat.h for the contract. */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include "global.h"
#include "sprite.h"

#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_object_event_compat.h"

/* Exactly one TU (this one) defines the session-published native arrays. */
#define OBJECT_EVENT_NATIVE_DEFINE
#include "emerald/resources/object_event_pic_tables.native.generated.h"
#undef OE_NATIVE_ARRAY

/* Session-global state: the published image is retained for the process
 * session and released only by EmeraldObjectEventCompat_Shutdown. */
static struct EmeraldResourceCompatibilityImage *sImage;

/* Generated static tables (bytewise-sorted, deterministic). */
struct ObjectEventSheetEntry
{
    const char *id;
    u32 expectedSize;
};
static const struct ObjectEventSheetEntry sSheetEntries[] =
{
#define OBJECT_EVENT_SHEET(symbol, id, size) { id, size },
#include "emerald/resources/object_event_sheets.generated.h"
#undef OBJECT_EVENT_SHEET
};

struct ObjectEventPaletteEntry
{
    const char *symbol;
    const char *id;
};
static const struct ObjectEventPaletteEntry sPaletteEntries[] =
{
#define OBJECT_EVENT_PALETTE(symbol, id, reflection) { #symbol, id },
#include "emerald/resources/object_event_palettes.generated.h"
#undef OBJECT_EVENT_PALETTE
};

#define OBJECT_EVENT_SHEET_COUNT \
    (sizeof(sSheetEntries) / sizeof(sSheetEntries[0]))
#define OBJECT_EVENT_PALETTE_COUNT \
    (sizeof(sPaletteEntries) / sizeof(sPaletteEntries[0]))
#define OBJECT_EVENT_ENTRY_COUNT \
    (OBJECT_EVENT_SHEET_COUNT + OBJECT_EVENT_PALETTE_COUNT)

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

/* Index of the image entry for a canonical id, or SIZE_MAX. */
static size_t FindEntryIndex(const char *id)
{
    size_t i;

    for (i = 0; i < OBJECT_EVENT_ENTRY_COUNT; i++)
    {
        if (i < OBJECT_EVENT_SHEET_COUNT)
        {
            if (strcmp(sSheetEntries[i].id, id) == 0)
                return i;
        }
        else
        {
            if (strcmp(sPaletteEntries[i - OBJECT_EVENT_SHEET_COUNT].id, id) == 0)
                return i;
        }
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

static size_t FindStreamSize(const char *id)
{
    size_t index = FindEntryIndex(id);
    if (sImage == NULL || index == (size_t)-1)
        return 0u;
    return EmeraldResourceCompatImage_GetStreamSize(sImage, index);
}

/* Publish every migrated frame/palette from the retained image. Runs both
 * at init (fresh image) and at republish (idempotent, allocation-free). */
static void PublishFromImage(void)
{
#define OBJECT_EVENT_FRAME(array, index, sheetId, offset, frameSize, whole) \
    do                                                                      \
    {                                                                       \
        const uint8_t *stream = FindStream(sheetId);                        \
        if (stream != NULL)                                                 \
        {                                                                   \
            (array)[(index)].data = (u8 *)stream + (offset);                \
            if (whole)                                                      \
                (array)[(index)].size = (u16)FindStreamSize(sheetId);       \
        }                                                                   \
    } while (0)
#include "emerald/resources/object_event_frames.generated.h"
#undef OBJECT_EVENT_FRAME

#define OBJECT_EVENT_PALETTE(symbol, id, reflection) \
    do                                               \
    {                                                \
        const uint8_t *stream = FindStream(id);      \
        if (stream != NULL)                          \
            memcpy(symbol, stream, 32);              \
    } while (0);
#include "emerald/resources/object_event_palettes.generated.h"
#undef OBJECT_EVENT_PALETTE
}

enum EmeraldResourceCompatStatus
EmeraldObjectEventCompat_TryInitialize(
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

    entries = malloc(OBJECT_EVENT_ENTRY_COUNT * sizeof(*entries));
    views = malloc(OBJECT_EVENT_ENTRY_COUNT * sizeof(*views));
    if (entries == NULL || views == NULL)
    {
        status = EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
        goto done;
    }
    memset(entries, 0, OBJECT_EVENT_ENTRY_COUNT * sizeof(*entries));

    /* Phase 1: resolve + verify every resource before anything is built. */
    for (i = 0; i < OBJECT_EVENT_SHEET_COUNT; i++)
    {
        status = ResolveResource(snapshot, sSheetEntries[i].id,
                                 GEN3_RESOURCE_TYPE_SPRITE_SHEET, 1u,
                                 sSheetEntries[i].expectedSize, &views[i],
                                 diagnostics);
        if (status != EMERALD_COMPAT_OK)
            goto done;
        entries[i].canonicalName = sSheetEntries[i].id;
        entries[i].type = GEN3_RESOURCE_TYPE_SPRITE_SHEET;
        entries[i].schema = 1u;
        entries[i].payload = views[i].payload;
        entries[i].payloadSize = (u32)views[i].payloadSize;
        entries[i].expectedSize = sSheetEntries[i].expectedSize;
        entries[i].encoding = EMERALD_COMPAT_ENTRY_RAW;
    }
    for (i = 0; i < OBJECT_EVENT_PALETTE_COUNT; i++)
    {
        status = ResolveResource(snapshot, sPaletteEntries[i].id,
                                 GEN3_RESOURCE_TYPE_PALETTE, 1u, 32u,
                                 &views[OBJECT_EVENT_SHEET_COUNT + i],
                                 diagnostics);
        if (status != EMERALD_COMPAT_OK)
            goto done;
        entries[OBJECT_EVENT_SHEET_COUNT + i].canonicalName =
            sPaletteEntries[i].id;
        entries[OBJECT_EVENT_SHEET_COUNT + i].type = GEN3_RESOURCE_TYPE_PALETTE;
        entries[OBJECT_EVENT_SHEET_COUNT + i].schema = 1u;
        entries[OBJECT_EVENT_SHEET_COUNT + i].payload =
            views[OBJECT_EVENT_SHEET_COUNT + i].payload;
        entries[OBJECT_EVENT_SHEET_COUNT + i].payloadSize =
            (u32)views[OBJECT_EVENT_SHEET_COUNT + i].payloadSize;
        entries[OBJECT_EVENT_SHEET_COUNT + i].expectedSize = 32u;
        entries[OBJECT_EVENT_SHEET_COUNT + i].encoding =
            EMERALD_COMPAT_ENTRY_RAW;
    }

    /* Phase 2: transactional image build (RAW entries: the stream IS the
     * canonical payload bytes). */
    status = EmeraldResourceCompatImage_CreateFamily(
        entries, OBJECT_EVENT_ENTRY_COUNT, &image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
        goto done;

    /* Phase 3: publish from the validated image, then adopt it. The
     * publication writes only migrated frame .data/.size fields and
     * palette bytes; every other field stays identical. */
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
EmeraldObjectEventCompat_Republish(
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

void EmeraldObjectEventCompat_ClearMigratedEntries(void)
{
#define OBJECT_EVENT_FRAME(array, index, sheetId, offset, frameSize, whole) \
    do                                                                      \
    {                                                                       \
        (array)[(index)].data = NULL;                                       \
        if (whole)                                                          \
            (array)[(index)].size = 0;                                      \
    } while (0)
#include "emerald/resources/object_event_frames.generated.h"
#undef OBJECT_EVENT_FRAME

#define OBJECT_EVENT_PALETTE(symbol, id, reflection) \
    memset(symbol, 0, 32);
#include "emerald/resources/object_event_palettes.generated.h"
#undef OBJECT_EVENT_PALETTE
}

void EmeraldObjectEventCompat_Shutdown(void)
{
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = NULL;
}

const struct EmeraldResourceCompatibilityImage *EmeraldObjectEventCompat_GetImage(void)
{
    return sImage;
}

size_t EmeraldObjectEventCompat_GetEntryCount(void)
{
    return sImage != NULL ? EmeraldResourceCompatImage_GetEntryCount(sImage)
                          : 0u;
}

#endif /* PLATFORM_SDL2 && NATIVE_LINUX */

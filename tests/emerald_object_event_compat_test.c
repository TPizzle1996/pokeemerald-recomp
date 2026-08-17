/* R11-B: object-event compatibility seam tests.
 *
 * Builds a synthetic snapshot carrying ALL 288 object-event resources (253
 * raw sheets with their generated sizes + 35 palettes, deterministic pattern
 * payloads), drives the real seam, and pins:
 *
 *   - transactional init: resolve/verify all 288, build, publish;
 *   - frame hydration: every one of the 1,788 generated frame rows gets
 *     frame.data == the seam image stream for its sheet + the generated
 *     offset (whole-sheet rows additionally get .size == sheet size);
 *   - palette publication: all 35 arrays receive the exact 32 canonical
 *     bytes;
 *   - byte parity: stream bytes equal the synthetic payload patterns;
 *   - lifecycle: clear -> NULL sentinels, republish (idempotent), shutdown;
 *   - failure: a wrong-sized sheet refuses without mutating anything.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "sprite.h"
#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_provider.h"
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_object_event_compat.h"

/* Declaration mode (the seam TU defines the arrays). The generated row
 * headers are included only with their row macros defined (below). */
#include "emerald/resources/object_event_pic_tables.native.generated.h"

static int sFailures = 0;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            sFailures++;                                                \
        }                                                               \
    } while (0)

#define OBJECT_EVENT_SHEET_COUNT 253u
#define OBJECT_EVENT_PALETTE_COUNT 35u

/* Deterministic payload pattern: byte k = (seed + k) & 0xFF. */
static void FillPattern(uint8_t *bytes, size_t size, uint8_t seed)
{
    size_t i;
    for (i = 0; i < size; i++)
        bytes[i] = (uint8_t)(seed + i);
}

struct SheetPayload
{
    char id[128];
    size_t size;
    uint8_t seed;
};

struct PalPayload
{
    char id[128];
    uint8_t seed;
};

static struct SheetPayload sSheets[OBJECT_EVENT_SHEET_COUNT];
static struct PalPayload sPals[OBJECT_EVENT_PALETTE_COUNT];
static size_t sSheetCount;
static size_t sPalCount;

/* Collect the generated ids/sizes (deterministic order). */
static void CollectGeneratedTables(void)
{
#define OBJECT_EVENT_SHEET(symbol, sheetIdArg, sheetSizeArg)              \
    do                                                                    \
    {                                                                     \
        snprintf(sSheets[sSheetCount].id, sizeof(sSheets[sSheetCount].id), \
                 "%s", sheetIdArg);                                       \
        sSheets[sSheetCount].size = (size_t)(sheetSizeArg);               \
        sSheets[sSheetCount].seed = (uint8_t)(sSheetCount * 3u + 1u);     \
        sSheetCount++;                                                    \
    } while (0);
#include "emerald/resources/object_event_sheets.generated.h"
#undef OBJECT_EVENT_SHEET

#define OBJECT_EVENT_PALETTE(symbol, palId, reflection)                   \
    do                                                                    \
    {                                                                     \
        snprintf(sPals[sPalCount].id, sizeof(sPals[sPalCount].id),        \
                 "%s", palId);                                            \
        sPals[sPalCount].seed = (uint8_t)(sPalCount * 7u + 2u);           \
        sPalCount++;                                                      \
    } while (0);
#include "emerald/resources/object_event_palettes.generated.h"
#undef OBJECT_EVENT_PALETTE
}

static bool SnapshotCarriesFamily(const struct Gen3ResourceSnapshot *snapshot)
{
    struct Gen3ResourceView view;
    Gen3ResourceHandle handle;
    return Gen3ResourceSnapshot_FindHandle(snapshot, sSheets[0].id, &handle)
               == GEN3_RESOURCE_OK
        && Gen3ResourceSnapshot_Resolve(snapshot, handle,
                                        GEN3_RESOURCE_TYPE_SPRITE_SHEET, 1u,
                                        &view) == GEN3_RESOURCE_OK;
}

/* Build a synthetic snapshot over the full family (or over a variant where
 * one sheet's payload size is wrong). */
static struct Gen3ResourceSnapshot *BuildSnapshot(bool wrongSizeSheet)
{
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList diag;
    size_t i;
    uint8_t paletteBytes[32];

    Gen3ResourceDiagnostics_Init(&diag);
    metadata.id = "emerald.rom-base.bpee01";
    metadata.version = "v1";
    metadata.kind = GEN3_PROVIDER_ROM_BASE;
    metadata.precedence = 300u;
    provider = Gen3ResourceProvider_Create(&metadata);
    catalog = Gen3ResourceCatalog_Create();
    if (provider == NULL || catalog == NULL)
        return NULL;

    for (i = 0; i < sSheetCount; i++)
    {
        size_t size = wrongSizeSheet && i == 0u
                        ? sSheets[i].size + 4u : sSheets[i].size;
        uint8_t *payload = malloc(size);
        if (payload == NULL)
            goto done;
        FillPattern(payload, size, sSheets[i].seed);
        Gen3ResourceProvider_Add(provider, sSheets[i].id,
                                 GEN3_RESOURCE_TYPE_SPRITE_SHEET, 1u,
                                 payload, (uint32_t)size, false, NULL);
        Gen3ResourceCatalog_Add(catalog, sSheets[i].id,
                                GEN3_RESOURCE_TYPE_SPRITE_SHEET, 1u, true, NULL);
        free(payload);
    }
    for (i = 0; i < sPalCount; i++)
    {
        FillPattern(paletteBytes, sizeof(paletteBytes), sPals[i].seed);
        Gen3ResourceProvider_Add(provider, sPals[i].id,
                                 GEN3_RESOURCE_TYPE_PALETTE, 1u,
                                 paletteBytes, sizeof(paletteBytes), false, NULL);
        Gen3ResourceCatalog_Add(catalog, sPals[i].id,
                                GEN3_RESOURCE_TYPE_PALETTE, 1u, true, NULL);
    }
    Gen3ResourceProvider_Finalize(provider, NULL);
    Gen3ResourceCatalog_Finalize(catalog, NULL);

    candidate = Gen3ResourceCandidate_Create(catalog, &diag);
    if (candidate != NULL)
    {
        Gen3ResourceCandidate_AddProvider(candidate, provider, &diag);
        if (!Gen3ResourceCandidate_Build(candidate, &snapshot, &diag))
            snapshot = NULL;
        Gen3ResourceCandidate_Destroy(candidate);
    }
done:
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourceProvider_Destroy(provider);
    Gen3ResourceDiagnostics_Destroy(&diag);
    return snapshot;
}

static const struct EmeraldResourceCompatibilityImage *Image(void)
{
    return EmeraldObjectEventCompat_GetImage();
}

static size_t SheetEntryIndex(const char *id)
{
    size_t i;
    for (i = 0; i < sSheetCount; i++)
    {
        if (strcmp(sSheets[i].id, id) == 0)
            return i;
    }
    return (size_t)-1;
}

/* Verify every generated frame row against the published image. */
static void VerifyFrameHydration(void)
{
#define OBJECT_EVENT_FRAME(array, index, sheetId, offset, frameSize, whole) \
    do                                                                      \
    {                                                                       \
        size_t entryIndex = SheetEntryIndex(sheetId);                       \
        const uint8_t *stream = NULL;                                       \
        size_t streamSize = 0;                                              \
        size_t j;                                                           \
        if (entryIndex != (size_t)-1)                                       \
        {                                                                   \
            stream = EmeraldResourceCompatImage_GetStream(Image(),          \
                                                          entryIndex);      \
            streamSize = EmeraldResourceCompatImage_GetStreamSize(Image(),  \
                                                                  entryIndex); \
        }                                                                   \
        CHECK(stream != NULL);                                              \
        CHECK((array)[(index)].data == (const u32 *)(const void *)(stream + (offset))); \
        if (whole)                                                          \
            CHECK((array)[(index)].size == (u16)streamSize);                \
        else                                                                \
            CHECK((array)[(index)].size == (u16)(frameSize));               \
        /* Byte parity: the stream bytes equal the synthetic pattern. */    \
        for (j = 0; stream != NULL && j < streamSize; j++)                  \
        {                                                                   \
            if (stream[j] != (uint8_t)(sSheets[entryIndex].seed + j))       \
            {                                                               \
                fprintf(stderr, "FAIL parity: %s byte %zu\n", sheetId, j);  \
                sFailures++;                                                \
                break;                                                      \
            }                                                               \
        }                                                                   \
    } while (0)
#include "emerald/resources/object_event_frames.generated.h"
#undef OBJECT_EVENT_FRAME
}

static void VerifyPalettes(void)
{
#define OBJECT_EVENT_PALETTE(symbol, palId, reflection)                    \
    do                                                                      \
    {                                                                       \
        size_t i;                                                           \
        size_t palIndex = (size_t)-1;                                       \
        for (i = 0; i < sPalCount; i++)                                     \
        {                                                                   \
            if (strcmp(sPals[i].id, palId) == 0)                            \
                palIndex = i;                                               \
        }                                                                   \
        CHECK(palIndex != (size_t)-1);                                      \
        for (i = 0; i < 16; i++)                                            \
        {                                                                   \
            uint16_t expected = (uint16_t)(((sPals[palIndex].seed + i * 2) \
                                                & 0xFF)                     \
                                | (uint16_t)(((sPals[palIndex].seed        \
                                               + i * 2 + 1) & 0xFF) << 8));          \
            CHECK((symbol)[i] == expected);                                 \
        }                                                                   \
    } while (0);
#include "emerald/resources/object_event_palettes.generated.h"
#undef OBJECT_EVENT_PALETTE
}

static void VerifyAllCleared(void)
{
#define OBJECT_EVENT_FRAME(array, index, sheetId, offset, frameSize, whole) \
    CHECK((array)[(index)].data == NULL);
#include "emerald/resources/object_event_frames.generated.h"
#undef OBJECT_EVENT_FRAME

#define OBJECT_EVENT_PALETTE(symbol, palId, reflection)                    \
    do                                                                      \
    {                                                                       \
        size_t i;                                                           \
        for (i = 0; i < 16; i++)                                            \
            CHECK((symbol)[i] == 0);                                        \
    } while (0);
#include "emerald/resources/object_event_palettes.generated.h"
#undef OBJECT_EVENT_PALETTE
}

int main(void)
{
    struct Gen3ResourceSnapshot *snapshot;
    struct Gen3ResourceSnapshot *badSnapshot;
    struct EmeraldResourceCompatDiagnostics diagnostics;
    enum EmeraldResourceCompatStatus status;

    CollectGeneratedTables();
    CHECK(sSheetCount == OBJECT_EVENT_SHEET_COUNT);
    CHECK(sPalCount == OBJECT_EVENT_PALETTE_COUNT);

    /* Transactional failure: a wrong-sized sheet refuses without mutating. */
    badSnapshot = BuildSnapshot(true);
    CHECK(badSnapshot != NULL);
    if (badSnapshot != NULL)
    {
        status = EmeraldObjectEventCompat_TryInitialize(badSnapshot, &diagnostics);
        CHECK(status != EMERALD_COMPAT_OK);
        CHECK(EmeraldObjectEventCompat_GetImage() == NULL);
        VerifyAllCleared();
        Gen3ResourceSnapshot_Destroy(badSnapshot);
    }

    /* Success path. */
    snapshot = BuildSnapshot(false);
    CHECK(snapshot != NULL);
    if (snapshot == NULL)
        return 1;
    CHECK(SnapshotCarriesFamily(snapshot));
    status = EmeraldObjectEventCompat_TryInitialize(snapshot, &diagnostics);
    CHECK(status == EMERALD_COMPAT_OK);
    CHECK(diagnostics.canonicalName[0] == '\0');
    CHECK(EmeraldObjectEventCompat_GetImage() != NULL);
    CHECK(EmeraldObjectEventCompat_GetEntryCount() == 288u);

    VerifyFrameHydration();
    VerifyPalettes();

    /* Clear -> NULL sentinels; republish restores; idempotent. */
    EmeraldObjectEventCompat_ClearMigratedEntries();
    VerifyAllCleared();
    status = EmeraldObjectEventCompat_Republish(&diagnostics);
    CHECK(status == EMERALD_COMPAT_OK);
    VerifyFrameHydration();
    VerifyPalettes();
    status = EmeraldObjectEventCompat_Republish(&diagnostics);
    CHECK(status == EMERALD_COMPAT_OK);

    /* Re-init with the same snapshot replaces the image (idempotent). */
    status = EmeraldObjectEventCompat_TryInitialize(snapshot, &diagnostics);
    CHECK(status == EMERALD_COMPAT_OK);
    VerifyFrameHydration();

    /* Shutdown -> unavailable, cleared state. */
    EmeraldObjectEventCompat_Shutdown();
    CHECK(EmeraldObjectEventCompat_GetImage() == NULL);
    CHECK(EmeraldObjectEventCompat_GetEntryCount() == 0u);
    status = EmeraldObjectEventCompat_Republish(&diagnostics);
    CHECK(status == EMERALD_COMPAT_ERR_UNAVAILABLE);

    Gen3ResourceSnapshot_Destroy(snapshot);
    if (sFailures != 0)
    {
        fprintf(stderr, "emerald_object_event_compat_test: %d failure(s)\n",
                sFailures);
        return 1;
    }
    printf("emerald_object_event_compat_test: all checks passed "
           "(%zu sheets, %zu palettes, 1788 frames)\n", sSheetCount, sPalCount);
    return 0;
}

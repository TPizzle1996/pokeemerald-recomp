/* R11-D: layout family native compatibility test.
 *
 * Builds a synthetic snapshot carrying ALL 882 layout-family resources
 * (pattern payloads, sizes straight from the generated
 * layout_frames.generated.h rows), then drives the REAL
 * emerald_layout_compat.c seam and verifies the published native records:
 *
 *   - all 441 struct MapLayout records: .map/.border land inside the
 *     session arena and the streams ARE the pattern payload bytes (raw
 *     canonical both targets - no LZ representation);
 *   - the compiled structural fields survive publication: width/height
 *     match the pinned facts and the tileset references stay as compiled
 *     (UnusedOutdoorArea_Layout's secondaryTileset stays NULL, the
 *     layouts.json "0" case);
 *   - transactional fail-closed: a wrong-size blockdata refuses the whole
 *     family, leaving the records at their NULL sentinels and the
 *     structural fields untouched;
 *   - Republish is allocation-free and idempotent; ClearMigratedEntries
 *     NULLs only the .map/.border pointers; Shutdown drops the image;
 *     re-init on a fresh snapshot succeeds.
 *
 * The entry-count cross-check (the seam's BuildEntries count over the
 * same generated rows) pins seam == generator == test at 882.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include "global.h"

#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_layout_compat.h"

/* The gTileset_* references in the record initializers resolve to the
 * definitions in emerald_tileset_compat.c (linked by this harness). */
#include "emerald/resources/tileset_native.generated.h"

/* Declaration mode: the record DEFINITIONS live in
 * emerald_layout_compat.c. */
#include "emerald/resources/layout_native.generated.h"

/* ------------------------------------------------------------------ */
/* Row table from the generated frames header.                        */
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

#define LAYOUT_ROW_COUNT (sizeof(sLayoutRows) / sizeof(sLayoutRows[0]))

/* Per-row symbol pointers (the same pass, emitting the identifiers
 * instead of their stringized names). */
static const struct MapLayout *const sLayouts[LAYOUT_ROW_COUNT] =
{
#define LAYOUT_RECORD(symbol, blockdataId, blockdataSize, borderId, borderSize) \
    &symbol,
#include "emerald/resources/layout_frames.generated.h"
#undef LAYOUT_RECORD
#undef LAYOUT_FRAMES_GENERATED_H
};

/* Pinned facts (layouts.json): the compiled structural fields must
 * survive publication untouched. */
struct LayoutPin
{
    const char *symbol;
    s32 width;
    s32 height;
    bool secondaryNull;
};

static const struct LayoutPin sPins[] =
{
    { "AbandonedShip_CaptainsOffice_Layout", 9, 7, FALSE },
    { "PetalburgCity_Layout", 30, 30, FALSE },
    { "UnusedOutdoorArea_Layout", 58, 26, TRUE },
    { "Underwater_Route127_Layout", 80, 80, FALSE },
};

static int sFailures;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",              \
                    __FILE__, __LINE__, #cond);                         \
            sFailures++;                                                \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* Deterministic pattern bytes for an id.                              */
/* ------------------------------------------------------------------ */

static uint32_t IdHash(const char *id)
{
    uint32_t h = 2166136261u;
    while (*id != '\0')
    {
        h ^= (uint8_t)*id++;
        h *= 16777619u;
    }
    return h;
}

static void FillPattern(uint8_t *buf, size_t size, const char *id)
{
    uint32_t x = IdHash(id);
    size_t i;

    for (i = 0u; i < size; i++)
    {
        x ^= x << 13u;
        x ^= x >> 17u;
        x ^= x << 5u;
        buf[i] = (uint8_t)x;
    }
}

/* ------------------------------------------------------------------ */
/* Synthetic full-family snapshot (or the wrong-size variant).        */
/* ------------------------------------------------------------------ */

static struct Gen3ResourceSnapshot *BuildSnapshot(bool wrongSizeBlockdata)
{
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList diag;
    size_t i;

    Gen3ResourceDiagnostics_Init(&diag);
    metadata.id = "emerald.rom-base.bpee01";
    metadata.version = "v1";
    metadata.kind = GEN3_PROVIDER_ROM_BASE;
    metadata.precedence = 300u;
    provider = Gen3ResourceProvider_Create(&metadata);
    catalog = Gen3ResourceCatalog_Create();
    if (provider == NULL || catalog == NULL)
        return NULL;

    for (i = 0u; i < LAYOUT_ROW_COUNT; i++)
    {
        const struct LayoutRow *r = &sLayoutRows[i];
        /* +2 keeps the payload even (blockdata is raw u16 words); the
         * SEAM's resolve (expectedSize check) must be the one to reject
         * it. */
        size_t blockdataSize = wrongSizeBlockdata && i == 0u
                               ? r->blockdataSize + 2u : r->blockdataSize;
        uint8_t *payload;

        payload = malloc(blockdataSize);
        if (payload == NULL)
            goto done;
        FillPattern(payload, blockdataSize, r->blockdataId);
        Gen3ResourceProvider_Add(provider, r->blockdataId,
                                 GEN3_RESOURCE_TYPE_TILEMAP, 1u,
                                 payload, (uint32_t)blockdataSize, false, NULL);
        Gen3ResourceCatalog_Add(catalog, r->blockdataId,
                                GEN3_RESOURCE_TYPE_TILEMAP, 1u, true, NULL);
        free(payload);

        payload = malloc(r->borderSize);
        if (payload == NULL)
            goto done;
        FillPattern(payload, r->borderSize, r->borderId);
        Gen3ResourceProvider_Add(provider, r->borderId,
                                 GEN3_RESOURCE_TYPE_TILEMAP, 1u,
                                 payload, (uint32_t)r->borderSize, false, NULL);
        Gen3ResourceCatalog_Add(catalog, r->borderId,
                                GEN3_RESOURCE_TYPE_TILEMAP, 1u, true, NULL);
        free(payload);
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

/* Replicate the seam's BuildEntries count over the same generated rows:
 * the seam, the generator and this test must agree on 882 entries (two
 * per layout, no dedup). */
static size_t ExpectedEntryCount(void)
{
    return LAYOUT_ROW_COUNT * 2u;
}

/* ------------------------------------------------------------------ */
/* Checks over the published records.                                 */
/* ------------------------------------------------------------------ */

static bool InArena(const void *ptr, const uint8_t *base, size_t size)
{
    const uint8_t *p = (const uint8_t *)ptr;
    return p >= base && p < base + size;
}

static void VerifyPatternAt(const uint8_t *ptr, size_t size, const char *id)
{
    uint8_t *expect = malloc(size);
    if (expect == NULL)
    {
        sFailures++;
        return;
    }
    FillPattern(expect, size, id);
    if (memcmp(ptr, expect, size) != 0)
    {
        fprintf(stderr, "CHECK failed at %s:%d: pattern mismatch %s\n",
                __FILE__, __LINE__, id);
        sFailures++;
    }
    free(expect);
}

static void VerifyPublishedRecords(void)
{
    const struct EmeraldResourceCompatibilityImage *image =
        EmeraldLayoutCompat_GetImage();
    const uint8_t *arenaBase;
    size_t arenaSize;
    size_t i;

    CHECK(image != NULL);
    CHECK(EmeraldResourceCompatImage_GetArenaSpan(image, &arenaBase, &arenaSize));
    for (i = 0u; i < LAYOUT_ROW_COUNT; i++)
    {
        const struct LayoutRow *r = &sLayoutRows[i];
        const struct MapLayout *layout = sLayouts[i];

        CHECK(InArena(layout->map, arenaBase, arenaSize));
        CHECK(InArena(layout->border, arenaBase, arenaSize));
        if (layout->map == NULL || layout->border == NULL)
            continue;
        VerifyPatternAt((const uint8_t *)layout->map, r->blockdataSize,
                        r->blockdataId);
        VerifyPatternAt((const uint8_t *)layout->border, r->borderSize,
                        r->borderId);
    }
    for (i = 0u; i < sizeof(sPins) / sizeof(sPins[0]); i++)
    {
        const struct LayoutPin *pin = &sPins[i];
        size_t j;
        for (j = 0u; j < LAYOUT_ROW_COUNT; j++)
        {
            if (strcmp(sLayoutRows[j].symbol, pin->symbol) == 0)
                break;
        }
        CHECK(j != LAYOUT_ROW_COUNT);
        if (j != LAYOUT_ROW_COUNT)
        {
            const struct MapLayout *layout = sLayouts[j];
            CHECK(layout->width == pin->width);
            CHECK(layout->height == pin->height);
            CHECK((layout->secondaryTileset == NULL) == pin->secondaryNull);
            CHECK(layout->primaryTileset != NULL);
        }
    }
    CHECK(sFailures == 0);
}

int main(void)
{
    struct Gen3ResourceSnapshot *badSnapshot;
    struct Gen3ResourceSnapshot *snapshot;
    struct EmeraldResourceCompatDiagnostics diagnostics;
    enum EmeraldResourceCompatStatus status;
    const struct EmeraldResourceCompatibilityImage *image;
    const uint8_t *arenaBase;
    size_t arenaSize;

    /* 1: transactional fail-closed on a wrong-size blockdata. */
    badSnapshot = BuildSnapshot(true);
    CHECK(badSnapshot != NULL);
    if (badSnapshot != NULL)
    {
        status = EmeraldLayoutCompat_TryInitialize(badSnapshot, &diagnostics);
        CHECK(status == EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH);
        CHECK(AbandonedShip_CaptainsOffice_Layout.map == NULL);
        CHECK(AbandonedShip_CaptainsOffice_Layout.border == NULL);
        CHECK(AbandonedShip_CaptainsOffice_Layout.width == 9);
        CHECK(AbandonedShip_CaptainsOffice_Layout.height == 7);
        CHECK(AbandonedShip_CaptainsOffice_Layout.primaryTileset != NULL);
        Gen3ResourceSnapshot_Destroy(badSnapshot);
    }

    /* 2: full-family init. */
    snapshot = BuildSnapshot(false);
    CHECK(snapshot != NULL);
    if (snapshot == NULL)
        return sFailures != 0;
    status = EmeraldLayoutCompat_TryInitialize(snapshot, &diagnostics);
    CHECK(status == EMERALD_COMPAT_OK);
    CHECK(diagnostics.canonicalName[0] == '\0');
    CHECK(diagnostics.stage[0] == '\0');
    CHECK(sFailures == 0);

    /* 3: entry-count cross-check (seam == generator == test). */
    CHECK(EmeraldLayoutCompat_GetEntryCount() == ExpectedEntryCount());
    CHECK(ExpectedEntryCount() == 882u);
    CHECK(sFailures == 0);

    /* 4: published records (arena residency, pattern bytes, pins). */
    VerifyPublishedRecords();

    /* 5: Republish is idempotent - same arena, same bytes. */
    image = EmeraldLayoutCompat_GetImage();
    CHECK(image != NULL);
    CHECK(EmeraldResourceCompatImage_GetArenaSpan(image, &arenaBase, &arenaSize));
    status = EmeraldLayoutCompat_Republish(&diagnostics);
    CHECK(status == EMERALD_COMPAT_OK);
    CHECK(EmeraldLayoutCompat_GetImage() == image);
    CHECK(InArena(AbandonedShip_CaptainsOffice_Layout.map, arenaBase, arenaSize));
    CHECK(AbandonedShip_CaptainsOffice_Layout.width == 9);
    CHECK(sFailures == 0);

    /* 6: ClearMigratedEntries NULLs the .map/.border pointers and leaves
     * the structural fields untouched. */
    EmeraldLayoutCompat_ClearMigratedEntries();
    CHECK(AbandonedShip_CaptainsOffice_Layout.map == NULL);
    CHECK(AbandonedShip_CaptainsOffice_Layout.border == NULL);
    CHECK(AbandonedShip_CaptainsOffice_Layout.width == 9);
    CHECK(AbandonedShip_CaptainsOffice_Layout.height == 7);
    CHECK(AbandonedShip_CaptainsOffice_Layout.primaryTileset != NULL);
    CHECK(sFailures == 0);

    /* 7: Shutdown drops the image; Republish now fails closed. */
    EmeraldLayoutCompat_Shutdown();
    CHECK(EmeraldLayoutCompat_GetImage() == NULL);
    CHECK(EmeraldLayoutCompat_GetEntryCount() == 0u);
    status = EmeraldLayoutCompat_Republish(&diagnostics);
    CHECK(status == EMERALD_COMPAT_ERR_UNAVAILABLE);
    CHECK(sFailures == 0);

    /* 8: a fresh snapshot re-initializes the seam. */
    status = EmeraldLayoutCompat_TryInitialize(snapshot, &diagnostics);
    CHECK(status == EMERALD_COMPAT_OK);
    CHECK(AbandonedShip_CaptainsOffice_Layout.map != NULL);
    CHECK(AbandonedShip_CaptainsOffice_Layout.border != NULL);
    EmeraldLayoutCompat_Shutdown();
    Gen3ResourceSnapshot_Destroy(snapshot);

    if (sFailures != 0)
    {
        fprintf(stderr, "emerald layout compat test: %d FAILURES\n",
                sFailures);
        return 1;
    }
    printf("emerald layout compat test: ALL PASSED "
           "(%zu layouts, %zu entries)\n", LAYOUT_ROW_COUNT,
           ExpectedEntryCount());
    return 0;
}

#else /* !(PLATFORM_SDL2 && NATIVE_LINUX) */

int main(void)
{
    return 0;
}

#endif

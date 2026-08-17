/* R10-C: reverse resource-range index unit tests.
 *
 * Builds a real EmeraldResourceCompatibilityImage with a small synthetic
 * family (the same literal-only LZ77 path the seams use), registers the
 * entry streams, and pins the lookup contract:
 *
 *   - beginning of range / interior / last valid byte all hit with the
 *     correct key/type/schema/role and range offset;
 *   - one byte before the range and one byte after the range miss;
 *   - overlapping registration is rejected;
 *   - hull arithmetic overflow, zero-length and full-table cases reject;
 *   - keys derive deterministically from canonical names.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "gen3/resources/resource_id.h"

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

static void FillPattern(uint8_t *bytes, size_t size, uint8_t seed)
{
    size_t i;
    for (i = 0; i < size; i++)
        bytes[i] = (uint8_t)(seed + (uint8_t)i);
}

static bool BuildImage(struct EmeraldResourceCompatibilityImage **outImage)
{
    static const char *names[3] = {
        "emerald:trainer/hiker/battle/front/sheet",
        "emerald:trainer/hiker/battle/front/palette",
        "emerald:pokemon/battle/front/species/0001",
    };
    static uint8_t sheetBytes[2048];
    static uint8_t paletteBytes[32];
    static uint8_t otherBytes[2048];
    struct EmeraldResourceCompatSourceEntry entries[3];
    enum EmeraldResourceCompatStatus status;
    size_t i;

    FillPattern(sheetBytes, sizeof(sheetBytes), 0x10);
    FillPattern(paletteBytes, sizeof(paletteBytes), 0x20);
    FillPattern(otherBytes, sizeof(otherBytes), 0x30);
    for (i = 0; i < 3; i++)
    {
        entries[i].canonicalName = names[i];
        entries[i].schema = 1u;
        entries[i].encoding = EMERALD_COMPAT_ENTRY_LZ;
        entries[i].expectedSize = 0u;
    }
    entries[0].type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    entries[0].payload = sheetBytes;
    entries[0].payloadSize = sizeof(sheetBytes);
    entries[1].type = GEN3_RESOURCE_TYPE_PALETTE;
    entries[1].payload = paletteBytes;
    entries[1].payloadSize = sizeof(paletteBytes);
    entries[2].type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    entries[2].payload = otherBytes;
    entries[2].payloadSize = sizeof(otherBytes);
    status = EmeraldResourceCompatImage_CreateFamily(entries, 3, outImage, NULL);
    return status == EMERALD_COMPAT_OK;
}

int main(void)
{
    struct EmeraldResourceRangeIndex index;
    struct EmeraldResourceCompatibilityImage *image = NULL;
    const uint8_t *stream0;
    const uint8_t *stream1;
    const uint8_t *stream2;
    size_t size0;
    size_t size1;
    size_t size2;
    Gen3ResourceKey expectedKey0;
    struct EmeraldResourceRangeHit hit;

    CHECK(BuildImage(&image));
    CHECK(image != NULL);
    stream0 = EmeraldResourceCompatImage_GetStream(image, 0);
    stream1 = EmeraldResourceCompatImage_GetStream(image, 1);
    stream2 = EmeraldResourceCompatImage_GetStream(image, 2);
    size0 = EmeraldResourceCompatImage_GetStreamSize(image, 0);
    size1 = EmeraldResourceCompatImage_GetStreamSize(image, 1);
    size2 = EmeraldResourceCompatImage_GetStreamSize(image, 2);
    CHECK(stream0 != NULL && stream1 != NULL && stream2 != NULL);
    CHECK(size0 > 0 && size1 > 0 && size2 > 0);

    EmeraldResourceRangeIndex_Init(&index);
    CHECK(EmeraldResourceRangeIndex_GetRangeCount(&index) == 0);
    CHECK(!EmeraldResourceRangeIndex_Lookup(&index, (uintptr_t)stream0, &hit));

    /* Registration: two trainer entries + one pokemon entry. */
    CHECK(EmeraldResourceRangeIndex_RegisterStream(&index, image, 0, 1u,
                                                   EMERALD_RESOURCE_ROLE_LEGACY_LZ));
    CHECK(EmeraldResourceRangeIndex_RegisterStream(&index, image, 1, 1u,
                                                   EMERALD_RESOURCE_ROLE_LEGACY_LZ));
    CHECK(EmeraldResourceRangeIndex_RegisterStream(&index, image, 2, 7u,
                                                   EMERALD_RESOURCE_ROLE_LEGACY_LZ));
    CHECK(EmeraldResourceRangeIndex_GetRangeCount(&index) == 3);

    /* Identity: the derived key matches DeriveKey on the canonical name. */
    Gen3ResourceId_DeriveKey("emerald:trainer/hiker/battle/front/sheet",
                             &expectedKey0);
    CHECK(Gen3ResourceId_KeyEqual(&index.ranges[0].key, &expectedKey0));

    /* Beginning of range. */
    CHECK(EmeraldResourceRangeIndex_Lookup(&index, (uintptr_t)stream0, &hit));
    CHECK(Gen3ResourceId_KeyEqual(&hit.key, &expectedKey0));
    CHECK(hit.type == GEN3_RESOURCE_TYPE_TILE_GRAPHICS);
    CHECK(hit.schema == 1u);
    CHECK(hit.role == EMERALD_RESOURCE_ROLE_LEGACY_LZ);
    CHECK(hit.rangeOffset == 0u);

    /* Interior pointer. */
    CHECK(size0 > 3);
    CHECK(EmeraldResourceRangeIndex_Lookup(&index, (uintptr_t)stream0 + 3, &hit));
    CHECK(hit.rangeOffset == 3u);

    /* Last valid byte. */
    CHECK(EmeraldResourceRangeIndex_Lookup(&index,
                                           (uintptr_t)stream0 + size0 - 1, &hit));
    CHECK(hit.rangeOffset == size0 - 1);

    /* One byte before the first range and one byte after the last range
     * miss (the streams are packed adjacently, so only the arena ends are
     * uncovered). */
    CHECK(!EmeraldResourceRangeIndex_Lookup(&index,
                                            (uintptr_t)stream0 - 1, &hit));
    CHECK(!EmeraldResourceRangeIndex_Lookup(&index,
                                            (uintptr_t)stream2 + size2, &hit));

    /* Adjacency semantics: the byte after range 0 is range 1's base, and so
     * on - the arena lays the streams out contiguously. Pin that. */
    Gen3ResourceId_DeriveKey("emerald:trainer/hiker/battle/front/palette",
                             &hit.key);
    CHECK(EmeraldResourceRangeIndex_Lookup(&index,
                                           (uintptr_t)stream0 + size0, &hit));
    CHECK(hit.rangeOffset == 0u);
    CHECK(Gen3ResourceId_KeyEqual(&hit.key, &index.ranges[1].key));
    CHECK(EmeraldResourceRangeIndex_Lookup(&index,
                                           (uintptr_t)stream1 - 1, &hit));
    CHECK(hit.rangeOffset == size0 - 1);
    CHECK(Gen3ResourceId_KeyEqual(&hit.key, &index.ranges[0].key));
    CHECK(EmeraldResourceRangeIndex_Lookup(&index,
                                           (uintptr_t)stream1 + size1, &hit));
    CHECK(Gen3ResourceId_KeyEqual(&hit.key, &index.ranges[2].key));

    /* Hulls: a hull spanning all three streams contains them, but hull
     * membership alone is not a range hit. */
    CHECK(EmeraldResourceRangeIndex_AddHull(&index, (uintptr_t)stream0,
                                            (size_t)(stream2 + size2 - stream0)));
    CHECK(EmeraldResourceRangeIndex_InHull(&index, (uintptr_t)stream0));
    CHECK(EmeraldResourceRangeIndex_InHull(&index, (uintptr_t)stream2 + size2 - 1));
    CHECK(!EmeraldResourceRangeIndex_InHull(&index, (uintptr_t)stream0 - 1));
    CHECK(!EmeraldResourceRangeIndex_InHull(&index,
                                            (uintptr_t)stream2 + size2));
    /* Resource-owned span with no registered range: inside its hull, but a
     * lookup misses (the capture path turns this combination into an
     * error). */
    CHECK(EmeraldResourceRangeIndex_AddHull(&index,
                                            (uintptr_t)stream0 - 0x100, 0x100));
    CHECK(EmeraldResourceRangeIndex_InHull(&index, (uintptr_t)stream0 - 1));
    CHECK(!EmeraldResourceRangeIndex_Lookup(&index,
                                            (uintptr_t)stream0 - 1, &hit));

    /* Overlapping registration is rejected. */
    {
        struct EmeraldResourceRangeIndex overlapIndex;
        EmeraldResourceRangeIndex_Init(&overlapIndex);
        CHECK(EmeraldResourceRangeIndex_RegisterStream(
                  &overlapIndex, image, 0, 1u, EMERALD_RESOURCE_ROLE_LEGACY_LZ));
        /* A range with the same base is an overlap: synthesize one by
         * registering a fake image entry whose stream is stream0 + 2
         * (inside range 0) - so use a second image built from the same
         * payloads is overkill; instead register a hull-adjacent fake via
         * a direct range insertion is not part of the API. Overlap via a
         * second image entry sharing bytes is impossible (distinct
         * allocations). The API-level overlap check is exercised with the
         * second index: stream1 - 1 and stream1 both belong to no range, so
         * registration of the SAME entry twice must fail as a duplicate
         * (same base). */
        CHECK(!EmeraldResourceRangeIndex_RegisterStream(
                  &overlapIndex, image, 0, 1u, EMERALD_RESOURCE_ROLE_LEGACY_LZ));
        CHECK(EmeraldResourceRangeIndex_GetRangeCount(&overlapIndex) == 1);
    }

    /* Overflow-safe registration and hull arithmetic. */
    {
        struct EmeraldResourceRangeIndex edgeIndex;
        EmeraldResourceRangeIndex_Init(&edgeIndex);
        CHECK(!EmeraldResourceRangeIndex_AddHull(&edgeIndex,
                                                 UINTPTR_MAX - 4, 8));
        CHECK(!EmeraldResourceRangeIndex_AddHull(&edgeIndex, 0, 0));
        CHECK(!EmeraldResourceRangeIndex_RegisterStream(
                  &edgeIndex, image, 999u, 1u, EMERALD_RESOURCE_ROLE_LEGACY_LZ));
        CHECK(!EmeraldResourceRangeIndex_RegisterStream(
                  &edgeIndex, image, 0, 1u, (enum EmeraldResourceRangeRole)99));
        CHECK(EmeraldResourceRangeIndex_GetRangeCount(&edgeIndex) == 0);
        CHECK(!EmeraldResourceRangeIndex_RegisterStream(NULL, image, 0, 1u,
                                                        EMERALD_RESOURCE_ROLE_LEGACY_LZ));
        CHECK(!EmeraldResourceRangeIndex_RegisterStream(&edgeIndex, NULL, 0, 1u,
                                                        EMERALD_RESOURCE_ROLE_LEGACY_LZ));
    }

    /* Hull full-table bound. */
    {
        struct EmeraldResourceRangeIndex hullIndex;
        size_t i;
        EmeraldResourceRangeIndex_Init(&hullIndex);
        for (i = 0; i < EMERALD_RESOURCE_RANGE_INDEX_MAX_HULLS; i++)
            CHECK(EmeraldResourceRangeIndex_AddHull(&hullIndex,
                                                    0x1000u + i * 0x100, 0x10));
        CHECK(!EmeraldResourceRangeIndex_AddHull(&hullIndex, 0x90000, 0x10));
    }

    /* Lookup robustness. */
    CHECK(!EmeraldResourceRangeIndex_Lookup(NULL, 0, &hit));
    CHECK(!EmeraldResourceRangeIndex_Lookup(&index, 0, NULL));
    CHECK(!EmeraldResourceRangeIndex_InHull(NULL, 0));
    CHECK(!EmeraldResourceRangeIndex_InHull(&index, 0));

    /* ResolveByKey (load direction): exact identity + offset. */
    {
        uintptr_t resolved;
        Gen3ResourceKey key0;
        Gen3ResourceKey bogus;
        Gen3ResourceId_DeriveKey("emerald:trainer/hiker/battle/front/sheet", &key0);
        Gen3ResourceId_DeriveKey("emerald:no/such/resource", &bogus);
        CHECK(EmeraldResourceRangeIndex_ResolveByKey(
                  &index, &key0, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u,
                  EMERALD_RESOURCE_ROLE_LEGACY_LZ, 0u, &resolved)
              && resolved == (uintptr_t)stream0);
        CHECK(EmeraldResourceRangeIndex_ResolveByKey(
                  &index, &key0, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u,
                  EMERALD_RESOURCE_ROLE_LEGACY_LZ, 3u, &resolved)
              && resolved == (uintptr_t)stream0 + 3);
        /* Identity mismatches refuse, never guess. */
        CHECK(!EmeraldResourceRangeIndex_ResolveByKey(
                  &index, &key0, GEN3_RESOURCE_TYPE_PALETTE, 1u,
                  EMERALD_RESOURCE_ROLE_LEGACY_LZ, 0u, &resolved));
        CHECK(!EmeraldResourceRangeIndex_ResolveByKey(
                  &index, &key0, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 9u,
                  EMERALD_RESOURCE_ROLE_LEGACY_LZ, 0u, &resolved));
        CHECK(!EmeraldResourceRangeIndex_ResolveByKey(
                  &index, &key0, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u,
                  EMERALD_RESOURCE_ROLE_CANONICAL, 0u, &resolved));
        /* Out-of-range offsets refuse. */
        CHECK(!EmeraldResourceRangeIndex_ResolveByKey(
                  &index, &key0, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u,
                  EMERALD_RESOURCE_ROLE_LEGACY_LZ, size0, &resolved));
        /* Unknown keys refuse. */
        CHECK(!EmeraldResourceRangeIndex_ResolveByKey(
                  &index, &bogus, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u,
                  EMERALD_RESOURCE_ROLE_LEGACY_LZ, 0u, &resolved));
        CHECK(!EmeraldResourceRangeIndex_ResolveByKey(NULL, &key0,
                  GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u,
                  EMERALD_RESOURCE_ROLE_LEGACY_LZ, 0u, &resolved));
        CHECK(!EmeraldResourceRangeIndex_ResolveByKey(&index, NULL,
                  GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u,
                  EMERALD_RESOURCE_ROLE_LEGACY_LZ, 0u, &resolved));
    }

    /* Reset clears everything. */
    EmeraldResourceRangeIndex_Reset(&index);
    CHECK(EmeraldResourceRangeIndex_GetRangeCount(&index) == 0);
    CHECK(!EmeraldResourceRangeIndex_InHull(&index, (uintptr_t)stream0));
    CHECK(!EmeraldResourceRangeIndex_Lookup(&index, (uintptr_t)stream0, &hit));

    EmeraldResourceCompatImage_Destroy(image);
    if (sFailures != 0)
    {
        fprintf(stderr, "emerald_resource_ranges_test: %d failure(s)\n", sFailures);
        return 1;
    }
    printf("emerald_resource_ranges_test: all checks passed\n");
    return 0;
}

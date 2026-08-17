/* R11-C: tileset family native compatibility test.
 *
 * Builds a synthetic snapshot carrying ALL 1,544 tileset-family resources
 * (pattern payloads, sizes straight from the generated
 * tileset_frames.generated.h rows), then drives the REAL
 * emerald_tileset_compat.c seam and verifies the published native objects:
 *
 *   - all 75 structs: the four payload pointers land inside the session
 *     arena, the LZ-served compressed tile streams decode back to the
 *     pattern, the raw tilesets' streams ARE the payload bytes, metatile/
 *     attribute/palette streams are byte-identical to the patterns,
 *     isCompressed/isSecondary match the generated rows, and every
 *     .callback survives publication;
 *   - the 1,200 palette-row array bytes, the 125 anim-frame arrays and the
 *     4 floor-light arrays are byte-parity with the patterns;
 *   - the shared SecretBase metatile/attribute aliasing: the 6 variants
 *     publish the SAME arena streams (detected dynamically from the rows);
 *   - transactional fail-closed: a wrong-size tile refuses the whole
 *     family, leaving the structs at their NULL sentinels, the arrays
 *     zeroed and the callbacks untouched;
 *   - Republish is allocation-free and idempotent; ClearMigratedEntries
 *     NULLs/zeroes only the migrated slots; Shutdown drops the image;
 *     re-init on a fresh snapshot succeeds.
 *
 * The entry-count cross-check (the seam's BuildEntries dedup logic
 * replicated over the same generated rows) pins seam == generator == test.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include "global.h"
#include "tileset_anims.h"

#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_tileset_compat.h"

/* Declaration mode: the DEFINITIONS live in emerald_tileset_compat.c. */
#include "emerald/resources/tileset_native.generated.h"

/* ------------------------------------------------------------------ */
/* Row tables from the generated frames header (one pass per kind).   */
/* ------------------------------------------------------------------ */

struct TilesetStructRow
{
    const char *symbol;
    const char *tilesId;
    u32 tilesSize;
    const char *metId;
    u32 metSize;
    const char *attrId;
    u32 attrSize;
    bool8 compressed;
    bool8 secondary;
};

static const struct TilesetStructRow sStructRows[] =
{
#define TILESET_STRUCT(symbol, tilesId, tilesSize, metId, metSize, attrId, attrSize, compressed, secondary) \
    { #symbol, tilesId, tilesSize, metId, metSize, attrId, attrSize, compressed, secondary },
#define TILESET_PALETTE_ROW(symbol, row, id)
#define TILESET_ANIM_FRAME(symbol, id, size)
#define TILESET_FLOOR_LIGHT(s0, s1, s2, s3, i0, i1, i2, i3)
#include "emerald/resources/tileset_frames.generated.h"
#undef TILESET_STRUCT
#undef TILESET_PALETTE_ROW
#undef TILESET_ANIM_FRAME
#undef TILESET_FLOOR_LIGHT
#undef TILESET_FRAMES_GENERATED_H
};

struct TilesetPaletteRow
{
    const char *symbol;
    u32 row;
    const char *id;
};

static const struct TilesetPaletteRow sPaletteRows[] =
{
#define TILESET_STRUCT(symbol, tilesId, tilesSize, metId, metSize, attrId, attrSize, compressed, secondary)
#define TILESET_PALETTE_ROW(symbol, row, id) { #symbol, row, id },
#define TILESET_ANIM_FRAME(symbol, id, size)
#define TILESET_FLOOR_LIGHT(s0, s1, s2, s3, i0, i1, i2, i3)
#include "emerald/resources/tileset_frames.generated.h"
#undef TILESET_STRUCT
#undef TILESET_PALETTE_ROW
#undef TILESET_ANIM_FRAME
#undef TILESET_FLOOR_LIGHT
#undef TILESET_FRAMES_GENERATED_H
};

struct TilesetAnimRow
{
    const char *symbol;
    const char *id;
    u32 size;
};

static const struct TilesetAnimRow sAnimRows[] =
{
#define TILESET_STRUCT(symbol, tilesId, tilesSize, metId, metSize, attrId, attrSize, compressed, secondary)
#define TILESET_PALETTE_ROW(symbol, row, id)
#define TILESET_ANIM_FRAME(symbol, id, size) { #symbol, id, size },
#define TILESET_FLOOR_LIGHT(s0, s1, s2, s3, i0, i1, i2, i3)
#include "emerald/resources/tileset_frames.generated.h"
#undef TILESET_STRUCT
#undef TILESET_PALETTE_ROW
#undef TILESET_ANIM_FRAME
#undef TILESET_FLOOR_LIGHT
#undef TILESET_FRAMES_GENERATED_H
};

struct TilesetFloorRow
{
    const char *symbol;
    const char *id;
};

static const struct TilesetFloorRow sFloorRows[] =
{
#define TILESET_STRUCT(symbol, tilesId, tilesSize, metId, metSize, attrId, attrSize, compressed, secondary)
#define TILESET_PALETTE_ROW(symbol, row, id)
#define TILESET_ANIM_FRAME(symbol, id, size)
#define TILESET_FLOOR_LIGHT(s0, s1, s2, s3, i0, i1, i2, i3) { #s0, i0 }, { #s1, i1 }, { #s2, i2 }, { #s3, i3 },
#include "emerald/resources/tileset_frames.generated.h"
#undef TILESET_STRUCT
#undef TILESET_PALETTE_ROW
#undef TILESET_ANIM_FRAME
#undef TILESET_FLOOR_LIGHT
#undef TILESET_FRAMES_GENERATED_H
};

#define TILESET_STRUCT_COUNT   (sizeof(sStructRows) / sizeof(sStructRows[0]))
#define TILESET_PALETTE_COUNT  (sizeof(sPaletteRows) / sizeof(sPaletteRows[0]))
#define TILESET_ANIM_COUNT     (sizeof(sAnimRows) / sizeof(sAnimRows[0]))
#define TILESET_FLOOR_COUNT    (sizeof(sFloorRows) / sizeof(sFloorRows[0]))

/* Per-row symbol pointers (the same passes, emitting the identifiers
 * instead of their stringized names). */
static const struct Tileset *const sStructs[TILESET_STRUCT_COUNT] =
{
#define TILESET_STRUCT(symbol, tilesId, tilesSize, metId, metSize, attrId, attrSize, compressed, secondary) \
    &symbol,
#define TILESET_PALETTE_ROW(symbol, row, id)
#define TILESET_ANIM_FRAME(symbol, id, size)
#define TILESET_FLOOR_LIGHT(s0, s1, s2, s3, i0, i1, i2, i3)
#include "emerald/resources/tileset_frames.generated.h"
#undef TILESET_STRUCT
#undef TILESET_PALETTE_ROW
#undef TILESET_ANIM_FRAME
#undef TILESET_FLOOR_LIGHT
#undef TILESET_FRAMES_GENERATED_H
};

static const uint8_t *const sPaletteBlocks[TILESET_PALETTE_COUNT] =
{
#define TILESET_STRUCT(symbol, tilesId, tilesSize, metId, metSize, attrId, attrSize, compressed, secondary)
#define TILESET_PALETTE_ROW(symbol, row, id) (const uint8_t *)&symbol[(row)],
#define TILESET_ANIM_FRAME(symbol, id, size)
#define TILESET_FLOOR_LIGHT(s0, s1, s2, s3, i0, i1, i2, i3)
#include "emerald/resources/tileset_frames.generated.h"
#undef TILESET_STRUCT
#undef TILESET_PALETTE_ROW
#undef TILESET_ANIM_FRAME
#undef TILESET_FLOOR_LIGHT
#undef TILESET_FRAMES_GENERATED_H
};

static const uint8_t *const sAnimBlocks[TILESET_ANIM_COUNT] =
{
#define TILESET_STRUCT(symbol, tilesId, tilesSize, metId, metSize, attrId, attrSize, compressed, secondary)
#define TILESET_PALETTE_ROW(symbol, row, id)
#define TILESET_ANIM_FRAME(symbol, id, size) (const uint8_t *)symbol,
#define TILESET_FLOOR_LIGHT(s0, s1, s2, s3, i0, i1, i2, i3)
#include "emerald/resources/tileset_frames.generated.h"
#undef TILESET_STRUCT
#undef TILESET_PALETTE_ROW
#undef TILESET_ANIM_FRAME
#undef TILESET_FLOOR_LIGHT
#undef TILESET_FRAMES_GENERATED_H
};

static const uint8_t *const sFloorBlocks[TILESET_FLOOR_COUNT] =
{
#define TILESET_STRUCT(symbol, tilesId, tilesSize, metId, metSize, attrId, attrSize, compressed, secondary)
#define TILESET_PALETTE_ROW(symbol, row, id)
#define TILESET_ANIM_FRAME(symbol, id, size)
#define TILESET_FLOOR_LIGHT(s0, s1, s2, s3, i0, i1, i2, i3) \
    (const uint8_t *)s0, (const uint8_t *)s1, \
    (const uint8_t *)s2, (const uint8_t *)s3,
#include "emerald/resources/tileset_frames.generated.h"
#undef TILESET_STRUCT
#undef TILESET_PALETTE_ROW
#undef TILESET_ANIM_FRAME
#undef TILESET_FLOOR_LIGHT
#undef TILESET_FRAMES_GENERATED_H
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
/* Literal-only GBA LZ77 decode (the seam's synthesized stream shape:  */
/* 4-byte header with the DECODED size, then [0xFF flag, 8 literals]   */
/* groups, zero-padded final group). Returns the decoded size, or 0   */
/* on a malformed stream.                                              */
/* ------------------------------------------------------------------ */

static size_t DecodeLiteralLz(const uint8_t *stream, size_t streamSize,
                              uint8_t *out, size_t outCapacity)
{
    uint32_t decoded;
    size_t offset = 4u;
    size_t outPos = 0u;

    if (streamSize < 4u || stream[0] != 0x10u)
        return 0u;
    decoded = (uint32_t)stream[1] | ((uint32_t)stream[2] << 8u)
            | ((uint32_t)stream[3] << 16u);
    if (decoded > outCapacity)
        return 0u;
    while (outPos < decoded)
    {
        size_t groupLen = decoded - outPos < 8u ? decoded - outPos : 8u;
        if (offset + 1u + groupLen > streamSize)
            return 0u;
        /* Literal-only GBA LZ77: every flag byte is 0x00 (each bit 0 =
         * literal; an all-literal group). The real GBA decompressor reads
         * the same stream, so this mirrors it, not 0xFF. */
        if (stream[offset] != 0x00u)
            return 0u;
        memcpy(out + outPos, stream + offset + 1u, groupLen);
        outPos += groupLen;
        offset += 9u;
    }
    return outPos;
}

/* ------------------------------------------------------------------ */
/* Synthetic full-family snapshot (or the wrong-size-tile variant).   */
/* ------------------------------------------------------------------ */

static struct Gen3ResourceSnapshot *BuildSnapshot(bool wrongSizeTile)
{
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList diag;
    uint8_t paletteBytes[32];
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

    for (i = 0u; i < TILESET_STRUCT_COUNT; i++)
    {
        const struct TilesetStructRow *r = &sStructRows[i];
        /* +32 keeps the payload TILE_GRAPHICS-valid (multiple of 32) so the
         * candidate build still constructs the snapshot; the SEAM's resolve
         * (expectedSize check) must be the one to reject it. */
        size_t tilesSize = wrongSizeTile && i == 0u
                            ? r->tilesSize + 32u : r->tilesSize;
        uint8_t *payload = malloc(tilesSize);
        if (payload == NULL)
            goto done;
        FillPattern(payload, tilesSize, r->tilesId);
        Gen3ResourceProvider_Add(provider, r->tilesId,
                                 GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u,
                                 payload, (uint32_t)tilesSize, false, NULL);
        Gen3ResourceCatalog_Add(catalog, r->tilesId,
                                GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u,
                                true, NULL);
        free(payload);
    }
    /* SecretBase 6-variant aliasing: the shared metatiles/attributes ids
     * repeat across struct rows, but the snapshot carries each unique id
     * once - exactly like the pack (70 unique each) and the seam's own
     * BuildEntries dedup. The provider finalizer rejects duplicates, and
     * ResolveResource resolves each unique id once anyway. */
    for (i = 0u; i < TILESET_STRUCT_COUNT; i++)
    {
        const struct TilesetStructRow *r = &sStructRows[i];
        uint8_t *payload;
        size_t j;
        for (j = 0u; j < i; j++)
        {
            if (strcmp(sStructRows[j].metId, r->metId) == 0)
                break;
        }
        if (j != i)
            continue;
        payload = malloc(r->metSize);
        if (payload == NULL)
            goto done;
        FillPattern(payload, r->metSize, r->metId);
        Gen3ResourceProvider_Add(provider, r->metId,
                                 GEN3_RESOURCE_TYPE_TILESET, 1u,
                                 payload, (uint32_t)r->metSize, false, NULL);
        Gen3ResourceCatalog_Add(catalog, r->metId,
                                GEN3_RESOURCE_TYPE_TILESET, 1u, true, NULL);
        free(payload);
    }
    for (i = 0u; i < TILESET_STRUCT_COUNT; i++)
    {
        const struct TilesetStructRow *r = &sStructRows[i];
        uint8_t *payload;
        size_t j;
        for (j = 0u; j < i; j++)
        {
            if (strcmp(sStructRows[j].attrId, r->attrId) == 0)
                break;
        }
        if (j != i)
            continue;
        payload = malloc(r->attrSize);
        if (payload == NULL)
            goto done;
        FillPattern(payload, r->attrSize, r->attrId);
        Gen3ResourceProvider_Add(provider, r->attrId,
                                 GEN3_RESOURCE_TYPE_TILESET, 2u,
                                 payload, (uint32_t)r->attrSize, false, NULL);
        Gen3ResourceCatalog_Add(catalog, r->attrId,
                                GEN3_RESOURCE_TYPE_TILESET, 2u, true, NULL);
        free(payload);
    }
    for (i = 0u; i < TILESET_PALETTE_COUNT; i++)
    {
        FillPattern(paletteBytes, sizeof(paletteBytes), sPaletteRows[i].id);
        Gen3ResourceProvider_Add(provider, sPaletteRows[i].id,
                                 GEN3_RESOURCE_TYPE_PALETTE, 1u,
                                 paletteBytes, sizeof(paletteBytes), false, NULL);
        Gen3ResourceCatalog_Add(catalog, sPaletteRows[i].id,
                                GEN3_RESOURCE_TYPE_PALETTE, 1u, true, NULL);
    }
    for (i = 0u; i < TILESET_ANIM_COUNT; i++)
    {
        uint8_t *payload = malloc(sAnimRows[i].size);
        if (payload == NULL)
            goto done;
        FillPattern(payload, sAnimRows[i].size, sAnimRows[i].id);
        Gen3ResourceProvider_Add(provider, sAnimRows[i].id,
                                 GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u,
                                 payload, (uint32_t)sAnimRows[i].size,
                                 false, NULL);
        Gen3ResourceCatalog_Add(catalog, sAnimRows[i].id,
                                GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u,
                                true, NULL);
        free(payload);
    }
    for (i = 0u; i < TILESET_FLOOR_COUNT; i++)
    {
        FillPattern(paletteBytes, sizeof(paletteBytes), sFloorRows[i].id);
        Gen3ResourceProvider_Add(provider, sFloorRows[i].id,
                                 GEN3_RESOURCE_TYPE_PALETTE, 1u,
                                 paletteBytes, sizeof(paletteBytes), false, NULL);
        Gen3ResourceCatalog_Add(catalog, sFloorRows[i].id,
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

/* Replicate the seam's BuildEntries dedup over the same generated rows:
 * the seam, the generator and this test must agree on 1,544 entries. */
static size_t ExpectedEntryCount(void)
{
    size_t count = 0u;
    size_t i, j;

    for (i = 0u; i < TILESET_STRUCT_COUNT; i++)
    {
        count++; /* tiles (always unique) */
        for (j = 0u; j < i; j++)
        {
            if (strcmp(sStructRows[j].metId, sStructRows[i].metId) == 0)
                break;
        }
        if (j == i)
            count++;
        for (j = 0u; j < i; j++)
        {
            if (strcmp(sStructRows[j].attrId, sStructRows[i].attrId) == 0)
                break;
        }
        if (j == i)
            count++;
    }
    count += TILESET_PALETTE_COUNT + TILESET_ANIM_COUNT + TILESET_FLOOR_COUNT;
    return count;
}

/* ------------------------------------------------------------------ */
/* Checks over the published objects.                                 */
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

/* The 25 tilesets with a compiled InitTilesetAnim_* entry point (pinned
 * identity; the native header assigns exactly these - the other 50
 * structs carry .callback = NULL). The callbacks stay compiled and
 * publication must never touch them. */
struct TilesetCallbackRow
{
    const char *symbol;
    void (*cb)(void);
};

static const struct TilesetCallbackRow sCallbacks[] =
{
    { "gTileset_General", InitTilesetAnim_General },
    { "gTileset_Petalburg", InitTilesetAnim_Petalburg },
    { "gTileset_Rustboro", InitTilesetAnim_Rustboro },
    { "gTileset_Dewford", InitTilesetAnim_Dewford },
    { "gTileset_Slateport", InitTilesetAnim_Slateport },
    { "gTileset_Mauville", InitTilesetAnim_Mauville },
    { "gTileset_MauvilleGym", InitTilesetAnim_MauvilleGym },
    { "gTileset_Lavaridge", InitTilesetAnim_Lavaridge },
    { "gTileset_Fallarbor", InitTilesetAnim_Fallarbor },
    { "gTileset_Fortree", InitTilesetAnim_Fortree },
    { "gTileset_Lilycove", InitTilesetAnim_Lilycove },
    { "gTileset_Mossdeep", InitTilesetAnim_Mossdeep },
    { "gTileset_Sootopolis", InitTilesetAnim_Sootopolis },
    { "gTileset_SootopolisGym", InitTilesetAnim_SootopolisGym },
    { "gTileset_Pacifidlog", InitTilesetAnim_Pacifidlog },
    { "gTileset_EverGrande", InitTilesetAnim_EverGrande },
    { "gTileset_EliteFour", InitTilesetAnim_EliteFour },
    { "gTileset_Building", InitTilesetAnim_Building },
    { "gTileset_BikeShop", InitTilesetAnim_BikeShop },
    { "gTileset_Cave", InitTilesetAnim_Cave },
    { "gTileset_BattlePyramid", InitTilesetAnim_BattlePyramid },
    { "gTileset_BattleFrontierOutsideWest", InitTilesetAnim_BattleFrontierOutsideWest },
    { "gTileset_BattleFrontierOutsideEast", InitTilesetAnim_BattleFrontierOutsideEast },
    { "gTileset_BattleDome", InitTilesetAnim_BattleDome },
    { "gTileset_Underwater", InitTilesetAnim_Underwater },
};

static void VerifyPublishedStructs(void)
{
    const struct EmeraldResourceCompatibilityImage *image =
        EmeraldTilesetCompat_GetImage();
    const uint8_t *arenaBase;
    size_t arenaSize;
    size_t i;

    CHECK(image != NULL);
    CHECK(EmeraldResourceCompatImage_GetArenaSpan(image, &arenaBase, &arenaSize));
    for (i = 0u; i < TILESET_STRUCT_COUNT; i++)
    {
        const struct TilesetStructRow *r = &sStructRows[i];
        const struct Tileset *ts = sStructs[i];
        uint8_t *decoded;
        size_t decodedSize;

        CHECK(InArena(ts->tiles, arenaBase, arenaSize));
        CHECK(InArena(ts->metatiles, arenaBase, arenaSize));
        CHECK(InArena(ts->metatileAttributes, arenaBase, arenaSize));
        CHECK(InArena(ts->palettes, arenaBase, arenaSize));
        if (ts->tiles == NULL || ts->metatiles == NULL
         || ts->metatileAttributes == NULL || ts->palettes == NULL)
            continue;
        CHECK(ts->isCompressed == r->compressed);
        CHECK(ts->isSecondary == r->secondary);
        {
            const struct TilesetCallbackRow *row = NULL;
            size_t k;
            for (k = 0u; k < sizeof(sCallbacks) / sizeof(sCallbacks[0]); k++)
            {
                if (strcmp(sCallbacks[k].symbol, r->symbol) == 0)
                {
                    row = &sCallbacks[k];
                    break;
                }
            }
            CHECK((row != NULL) == (ts->callback != NULL));
            if (row != NULL)
                CHECK(ts->callback == row->cb);
        }

        VerifyPatternAt((const uint8_t *)ts->metatiles, r->metSize, r->metId);
        VerifyPatternAt((const uint8_t *)ts->metatileAttributes,
                        r->attrSize, r->attrId);
        if (r->compressed)
        {
            /* The published .tiles stream is the synthesized literal-only
             * GBA LZ stream; it must decode to the pattern. */
            decoded = malloc(r->tilesSize);
            if (decoded == NULL)
            {
                sFailures++;
                continue;
            }
            decodedSize = DecodeLiteralLz((const uint8_t *)ts->tiles,
                                          arenaBase + arenaSize
                                              - (const uint8_t *)ts->tiles,
                                          decoded, r->tilesSize);
            CHECK(decodedSize == r->tilesSize);
            if (decodedSize == r->tilesSize)
                VerifyPatternAt(decoded, r->tilesSize, r->tilesId);
            free(decoded);
        }
        else
        {
            /* The 8 uncompressed tilesets are served raw: the stream IS
             * the canonical bytes. */
            VerifyPatternAt((const uint8_t *)ts->tiles, r->tilesSize,
                            r->tilesId);
        }
    }
    CHECK(sFailures == 0);
}

static void VerifyAliases(void)
{
    size_t i, j;

    for (i = 0u; i < TILESET_STRUCT_COUNT; i++)
    {
        for (j = 0u; j < i; j++)
        {
            if (strcmp(sStructRows[j].metId, sStructRows[i].metId) == 0)
            {
                CHECK(sStructs[j]->metatiles == sStructs[i]->metatiles);
                CHECK(sStructs[j]->metatileAttributes
                          == sStructs[i]->metatileAttributes);
            }
            if (strcmp(sStructRows[j].tilesId, sStructRows[i].tilesId) == 0)
                CHECK(sStructs[j]->tiles == sStructs[i]->tiles);
        }
    }
    CHECK(sFailures == 0);
}

static void VerifyPaletteArrays(void)
{
    size_t i;

    for (i = 0u; i < TILESET_PALETTE_COUNT; i++)
        VerifyPatternAt(sPaletteBlocks[i], 32u, sPaletteRows[i].id);
    CHECK(sFailures == 0);
}

static void VerifyAnimArrays(void)
{
    size_t i;

    for (i = 0u; i < TILESET_ANIM_COUNT; i++)
        VerifyPatternAt(sAnimBlocks[i], sAnimRows[i].size, sAnimRows[i].id);
    CHECK(sFailures == 0);
}

static void VerifyFloorArrays(void)
{
    size_t i;

    for (i = 0u; i < TILESET_FLOOR_COUNT; i++)
        VerifyPatternAt(sFloorBlocks[i], 32u, sFloorRows[i].id);
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

    /* 1: transactional fail-closed on a wrong-size tile. */
    badSnapshot = BuildSnapshot(true);
    CHECK(badSnapshot != NULL);
    if (badSnapshot != NULL)
    {
        status = EmeraldTilesetCompat_TryInitialize(badSnapshot, &diagnostics);
        CHECK(status == EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH);
        CHECK(gTileset_General.tiles == NULL);
        CHECK(gTileset_General.palettes == NULL);
        CHECK(gTileset_General.metatiles == NULL);
        CHECK(gTileset_General.metatileAttributes == NULL);
        CHECK(gTilesetPalettes_General[0][0] == 0);
        CHECK(gTileset_General.callback == InitTilesetAnim_General);
        Gen3ResourceSnapshot_Destroy(badSnapshot);
    }

    /* 2: full-family init. */
    snapshot = BuildSnapshot(false);
    CHECK(snapshot != NULL);
    if (snapshot == NULL)
        return sFailures != 0;
    status = EmeraldTilesetCompat_TryInitialize(snapshot, &diagnostics);
    CHECK(status == EMERALD_COMPAT_OK);
    CHECK(diagnostics.canonicalName[0] == '\0');
    CHECK(diagnostics.stage[0] == '\0');
    CHECK(sFailures == 0);

    /* 3: entry-count cross-check (seam == generator == test). */
    CHECK(EmeraldTilesetCompat_GetEntryCount() == ExpectedEntryCount());
    CHECK(ExpectedEntryCount() == 1544u);
    CHECK(sFailures == 0);

    /* 4-7: published structs, aliases, palette/anim/floor array bytes. */
    VerifyPublishedStructs();
    VerifyAliases();
    VerifyPaletteArrays();
    VerifyAnimArrays();
    VerifyFloorArrays();

    /* 8: Republish is idempotent - same arena, same bytes. */
    image = EmeraldTilesetCompat_GetImage();
    CHECK(image != NULL);
    CHECK(EmeraldResourceCompatImage_GetArenaSpan(image, &arenaBase, &arenaSize));
    status = EmeraldTilesetCompat_Republish(&diagnostics);
    CHECK(status == EMERALD_COMPAT_OK);
    CHECK(EmeraldTilesetCompat_GetImage() == image);
    CHECK(InArena(gTileset_General.tiles, arenaBase, arenaSize));
    CHECK(gTileset_General.callback == InitTilesetAnim_General);
    CHECK(sFailures == 0);

    /* 9: ClearMigratedEntries NULLs the payload pointers, zeroes the
     * arrays, and leaves the structural fields untouched. */
    EmeraldTilesetCompat_ClearMigratedEntries();
    CHECK(gTileset_General.tiles == NULL);
    CHECK(gTileset_General.palettes == NULL);
    CHECK(gTileset_General.metatiles == NULL);
    CHECK(gTileset_General.metatileAttributes == NULL);
    CHECK(gTilesetPalettes_General[0][0] == 0);
    CHECK(gTileset_General.callback == InitTilesetAnim_General);
    CHECK(sFailures == 0);

    /* 10: Shutdown drops the image; Republish now fails closed. */
    EmeraldTilesetCompat_Shutdown();
    CHECK(EmeraldTilesetCompat_GetImage() == NULL);
    CHECK(EmeraldTilesetCompat_GetEntryCount() == 0u);
    status = EmeraldTilesetCompat_Republish(&diagnostics);
    CHECK(status == EMERALD_COMPAT_ERR_UNAVAILABLE);
    CHECK(sFailures == 0);

    /* 11: a fresh snapshot re-initializes the seam. */
    status = EmeraldTilesetCompat_TryInitialize(snapshot, &diagnostics);
    CHECK(status == EMERALD_COMPAT_OK);
    CHECK(gTileset_General.tiles != NULL);
    CHECK(gTileset_General.callback == InitTilesetAnim_General);
    EmeraldTilesetCompat_Shutdown();
    Gen3ResourceSnapshot_Destroy(snapshot);

    if (sFailures != 0)
    {
        fprintf(stderr, "emerald tileset compat test: %d FAILURES\n",
                sFailures);
        return 1;
    }
    printf("emerald tileset compat test: ALL PASSED "
           "(75 structs, %zu entries)\n", ExpectedEntryCount());
    return 0;
}

#else /* !(PLATFORM_SDL2 && NATIVE_LINUX) */

int main(void)
{
    return 0;
}

#endif

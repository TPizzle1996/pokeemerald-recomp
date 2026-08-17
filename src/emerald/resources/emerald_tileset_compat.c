/* R11-C: tileset graphics native compatibility publication.
 * See emerald_tileset_compat.h for the contract. */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include "global.h"
#include "tileset_anims.h"

#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/resource_lz.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_tileset_compat.h"

/* Exactly one TU (this one) defines the session-published native slots. */
#define TILESET_NATIVE_DEFINE
#include "emerald/resources/tileset_native.generated.h"
#undef TILESET_NATIVE_STRUCT
#undef TILESET_NATIVE_ARRAY

/* ------------------------------------------------------------------ */
/* Row tables: the frames header is re-included once per row kind.    */
/* (Rows carry no trailing ';' — the includer supplies the separator, */
/* the object-event precedent.)                                       */
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

/* Worst-case entry count (no metatile/attribute aliasing); the image is
 * deduped to exactly 1544 (75 tiles + 70 metatiles + 70 attributes +
 * 1,200 palette rows + 125 anim frames + 4 floor-light palettes). */
#define TILESET_MAX_ENTRIES \
    (TILESET_STRUCT_COUNT * 3u + TILESET_PALETTE_COUNT \
     + TILESET_ANIM_COUNT + TILESET_FLOOR_COUNT)
#define TILESET_ENTRY_COUNT 1544u

/* One image entry: identity + verification + encoding invariants. */
struct TilesetImageEntry
{
    const char *id;
    enum Gen3ResourceType type;
    u32 schema;
    u32 expectedSize;
    enum EmeraldResourceCompatEntryEncoding encoding;
};

static struct TilesetImageEntry sEntries[TILESET_MAX_ENTRIES];
static size_t sEntryCount;

/* Session-global state: the published image is retained for the process
 * session and released only by EmeraldTilesetCompat_Shutdown. */
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

/* Build the deduped image-entry table from the row tables. The 6-variant
 * SecretBase aliases share one metatiles + one attributes resource, so the
 * per-struct-row met/attr ids are deduped (the R9 alias rule). Idempotent:
 * rebuilt on every TryInitialize. */
static void BuildEntries(void)
{
    size_t i;

    sEntryCount = 0u;
    for (i = 0u; i < TILESET_STRUCT_COUNT; i++)
    {
        const struct TilesetStructRow *r = &sStructRows[i];
        size_t j;
        sEntries[sEntryCount].id = r->tilesId;
        sEntries[sEntryCount].type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
        sEntries[sEntryCount].schema = 1u;
        sEntries[sEntryCount].expectedSize = r->tilesSize;
        sEntries[sEntryCount].encoding = r->compressed
            ? EMERALD_COMPAT_ENTRY_GBA_LZ : EMERALD_COMPAT_ENTRY_RAW;
        sEntryCount++;
        for (j = 0u; j < sEntryCount - 1u; j++)
        {
            if (strcmp(sEntries[j].id, r->metId) == 0)
                break;
        }
        if (j == sEntryCount - 1u)
        {
            sEntries[sEntryCount].id = r->metId;
            sEntries[sEntryCount].type = GEN3_RESOURCE_TYPE_TILESET;
            sEntries[sEntryCount].schema = 1u;
            sEntries[sEntryCount].expectedSize = r->metSize;
            sEntries[sEntryCount].encoding = EMERALD_COMPAT_ENTRY_RAW;
            sEntryCount++;
        }
        for (j = 0u; j < sEntryCount - 1u; j++)
        {
            if (strcmp(sEntries[j].id, r->attrId) == 0)
                break;
        }
        if (j == sEntryCount - 1u)
        {
            sEntries[sEntryCount].id = r->attrId;
            sEntries[sEntryCount].type = GEN3_RESOURCE_TYPE_TILESET;
            sEntries[sEntryCount].schema = 2u;
            sEntries[sEntryCount].expectedSize = r->attrSize;
            sEntries[sEntryCount].encoding = EMERALD_COMPAT_ENTRY_RAW;
            sEntryCount++;
        }
    }
    for (i = 0u; i < TILESET_PALETTE_COUNT; i++)
    {
        sEntries[sEntryCount].id = sPaletteRows[i].id;
        sEntries[sEntryCount].type = GEN3_RESOURCE_TYPE_PALETTE;
        sEntries[sEntryCount].schema = 1u;
        sEntries[sEntryCount].expectedSize = 32u;
        sEntries[sEntryCount].encoding = EMERALD_COMPAT_ENTRY_RAW;
        sEntryCount++;
    }
    for (i = 0u; i < TILESET_ANIM_COUNT; i++)
    {
        sEntries[sEntryCount].id = sAnimRows[i].id;
        sEntries[sEntryCount].type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
        sEntries[sEntryCount].schema = 1u;
        sEntries[sEntryCount].expectedSize = sAnimRows[i].size;
        sEntries[sEntryCount].encoding = EMERALD_COMPAT_ENTRY_RAW;
        sEntryCount++;
    }
    for (i = 0u; i < TILESET_FLOOR_COUNT; i++)
    {
        sEntries[sEntryCount].id = sFloorRows[i].id;
        sEntries[sEntryCount].type = GEN3_RESOURCE_TYPE_PALETTE;
        sEntries[sEntryCount].schema = 1u;
        sEntries[sEntryCount].expectedSize = 32u;
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

/* The 16 palette rows of a tileset share its canonical id prefix
 * (emerald:tileset/<canonical>/...), so the row-0 stream locates the
 * tileset's 512-byte contiguous palette region in the arena. */
static const uint8_t *FindPaletteStream(const char *tilesId)
{
    const char *slash;
    size_t prefixLen;
    size_t i;

    if (tilesId == NULL || (slash = strrchr(tilesId, '/')) == NULL)
        return NULL;
    prefixLen = (size_t)(slash - tilesId) + 1u;
    for (i = 0u; i < TILESET_PALETTE_COUNT; i++)
    {
        const char *id = sPaletteRows[i].id;
        if (sPaletteRows[i].row != 0u
         || strncmp(id, tilesId, prefixLen) != 0
         || strncmp(id + prefixLen, "palette/00", 10) != 0)
            continue;
        return FindStream(id);
    }
    return NULL;
}

/* The .palettes pointer dereferences 512 contiguous bytes (16 rows), so
 * every tileset's row-0..15 entries must be consecutive in the image (they
 * are by construction: palette rows are appended in emission order, each
 * tileset's rows contiguous). Fails closed if the invariant is violated. */
static bool32 VerifyPaletteRegions(void)
{
    char buf[96];
    size_t i;

    for (i = 0u; i < TILESET_STRUCT_COUNT; i++)
    {
        const char *tilesId = sStructRows[i].tilesId;
        const char *slash = strrchr(tilesId, '/');
        size_t prefixLen = (size_t)(slash - tilesId) + 1u;
        size_t rowIndex;
        u32 j;

        snprintf(buf, sizeof(buf), "%.*spalette/00", (int)prefixLen, tilesId);
        rowIndex = FindEntryIndex(buf);
        if (rowIndex == (size_t)-1 || rowIndex + 15u >= sEntryCount)
            return FALSE;
        for (j = 1u; j < 16u; j++)
        {
            snprintf(buf, sizeof(buf), "%.*spalette/%02u",
                     (int)prefixLen, tilesId, j);
            if (FindEntryIndex(buf) != rowIndex + (size_t)j)
                return FALSE;
        }
    }
    return TRUE;
}

/* Publish every migrated struct pointer and array byte from the retained
 * image. Runs both at init (fresh image) and at republish (idempotent,
 * allocation-free). */
static void PublishFromImage(void)
{
#define TILESET_STRUCT(symbol, tilesId, tilesSize, metId, metSize, attrId, attrSize, compressed, secondary) \
    do                                                                      \
    {                                                                       \
        const uint8_t *tiles = FindStream(tilesId);                         \
        const uint8_t *met = FindStream(metId);                             \
        const uint8_t *attrs = FindStream(attrId);                          \
        const uint8_t *pals = FindPaletteStream(tilesId);                   \
        if (tiles != NULL)                                                  \
            symbol.tiles = (const u32 *)tiles;                              \
        if (met != NULL)                                                    \
            symbol.metatiles = (const u16 *)met;                            \
        if (attrs != NULL)                                                  \
            symbol.metatileAttributes = (const u16 *)attrs;                 \
        if (pals != NULL)                                                   \
            symbol.palettes = (const u16 (*)[16])pals;                      \
    } while (0);
#define TILESET_PALETTE_ROW(symbol, row, id) \
    do                                                                      \
    {                                                                       \
        const uint8_t *stream = FindStream(id);                             \
        if (stream != NULL)                                                 \
            memcpy(&(symbol)[(row)], stream, 32);                           \
    } while (0);
#define TILESET_ANIM_FRAME(symbol, id, size) \
    do                                                                      \
    {                                                                       \
        const uint8_t *stream = FindStream(id);                             \
        if (stream != NULL)                                                 \
            memcpy(symbol, stream, (size));                                 \
    } while (0);
#define TILESET_FLOOR_LIGHT(s0, s1, s2, s3, i0, i1, i2, i3) \
    do                                                                      \
    {                                                                       \
        const uint8_t *a = FindStream(i0);                                  \
        const uint8_t *b = FindStream(i1);                                  \
        const uint8_t *c = FindStream(i2);                                  \
        const uint8_t *d = FindStream(i3);                                  \
        if (a != NULL) memcpy(s0, a, 32);                                   \
        if (b != NULL) memcpy(s1, b, 32);                                   \
        if (c != NULL) memcpy(s2, c, 32);                                   \
        if (d != NULL) memcpy(s3, d, 32);                                   \
    } while (0);
#include "emerald/resources/tileset_frames.generated.h"
#undef TILESET_STRUCT
#undef TILESET_PALETTE_ROW
#undef TILESET_ANIM_FRAME
#undef TILESET_FLOOR_LIGHT
#undef TILESET_FRAMES_GENERATED_H
}

enum EmeraldResourceCompatStatus
EmeraldTilesetCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    struct EmeraldResourceCompatSourceEntry *entries = NULL;
    struct Gen3ResourceView *views = NULL;
    uint8_t *encodedArena = NULL;
    struct EmeraldResourceCompatibilityImage *image = NULL;
    enum EmeraldResourceCompatStatus status = EMERALD_COMPAT_OK;
    size_t i;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;

    BuildEntries();
    if (sEntryCount != TILESET_ENTRY_COUNT)
    {
        if (diagnostics != NULL)
        {
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "build");
            diagnostics->expectedSize = TILESET_ENTRY_COUNT;
            diagnostics->actualSize = (u32)sEntryCount;
        }
        return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
    }
    if (!VerifyPaletteRegions())
    {
        if (diagnostics != NULL)
        {
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "build");
            snprintf(diagnostics->canonicalName,
                     sizeof(diagnostics->canonicalName), "palette-regions");
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

    /* Phase 2: source entries. The pack serves the DECODED bytes of each
     * compressed tile stream, so the seam re-encodes them into deterministic
     * literal-only GBA LZ77 streams (EMERALD_COMPAT_ENTRY_GBA_LZ): the
     * published .tiles consumers (DecompressAndCopyTileDataToVram)
     * LZ77-decompress exactly as they do on the GBA build. */
    {
        size_t total = 0u;
        size_t offset = 0u;
        for (i = 0u; i < sEntryCount; i++)
        {
            if (sEntries[i].encoding == EMERALD_COMPAT_ENTRY_GBA_LZ)
                total += Gen3LzLiteral_EncodedSize(sEntries[i].expectedSize);
        }
        if (total > 0u)
        {
            encodedArena = malloc(total);
            if (encodedArena == NULL)
            {
                status = EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
                goto done;
            }
        }
        for (i = 0u; i < sEntryCount; i++)
        {
            entries[i].canonicalName = sEntries[i].id;
            entries[i].type = sEntries[i].type;
            entries[i].schema = sEntries[i].schema;
            entries[i].expectedSize = sEntries[i].expectedSize;
            entries[i].encoding = sEntries[i].encoding;
            if (sEntries[i].encoding == EMERALD_COMPAT_ENTRY_GBA_LZ)
            {
                size_t outSize = 0u;
                enum Gen3LzResult lz = Gen3LzLiteral_Encode(
                    views[i].payload, (uint32_t)views[i].payloadSize,
                    encodedArena + offset, total - offset, &outSize);
                if (lz != GEN3_LZ_OK)
                {
                    status = EMERALD_COMPAT_ERR_ENCODE_FAILED;
                    goto done;
                }
                entries[i].payload = encodedArena + offset;
                entries[i].payloadSize = (uint32_t)outSize;
                offset += outSize;
            }
            else
            {
                entries[i].payload = views[i].payload;
                entries[i].payloadSize = (uint32_t)views[i].payloadSize;
            }
        }
    }

    /* Phase 3: transactional image build (RAW entries: the stream IS the
     * canonical payload bytes; GBA_LZ entries: the stream IS the re-encoded
     * GBA LZ77 stream, validated against expectedSize by the image). */
    status = EmeraldResourceCompatImage_CreateFamily(
        entries, sEntryCount, &image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
        goto done;

    /* Phase 4: publish from the validated image, then adopt it. The
     * publication writes only migrated struct payload pointers and palette/
     * anim/floor array bytes; every other field stays identical. */
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = image;
    PublishFromImage();
    ClearDiagnostics(diagnostics);
    status = EMERALD_COMPAT_OK;
done:
    free(entries);
    free(views);
    free(encodedArena);
    if (status != EMERALD_COMPAT_OK)
    {
        EmeraldResourceCompatImage_Destroy(image);
        /* Fail closed: nothing was mutated (publication happens only after
         * the image was built and adopted on the success path). */
    }
    return status;
}

enum EmeraldResourceCompatStatus
EmeraldTilesetCompat_Republish(
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

void EmeraldTilesetCompat_ClearMigratedEntries(void)
{
#define TILESET_STRUCT(symbol, tilesId, tilesSize, metId, metSize, attrId, attrSize, compressed, secondary) \
    do                                                                      \
    {                                                                       \
        symbol.tiles = NULL;                                                \
        symbol.palettes = NULL;                                             \
        symbol.metatiles = NULL;                                            \
        symbol.metatileAttributes = NULL;                                   \
    } while (0);
#define TILESET_PALETTE_ROW(symbol, row, id) \
    memset(&(symbol)[(row)], 0, 32);
#define TILESET_ANIM_FRAME(symbol, id, size) \
    memset(symbol, 0, (size));
#define TILESET_FLOOR_LIGHT(s0, s1, s2, s3, i0, i1, i2, i3) \
    do                                                                      \
    {                                                                       \
        memset(s0, 0, 32);                                                  \
        memset(s1, 0, 32);                                                  \
        memset(s2, 0, 32);                                                  \
        memset(s3, 0, 32);                                                  \
    } while (0);
#include "emerald/resources/tileset_frames.generated.h"
#undef TILESET_STRUCT
#undef TILESET_PALETTE_ROW
#undef TILESET_ANIM_FRAME
#undef TILESET_FLOOR_LIGHT
#undef TILESET_FRAMES_GENERATED_H
}

void EmeraldTilesetCompat_Shutdown(void)
{
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = NULL;
}

const struct EmeraldResourceCompatibilityImage *EmeraldTilesetCompat_GetImage(void)
{
    return sImage;
}

size_t EmeraldTilesetCompat_GetEntryCount(void)
{
    return sImage != NULL ? EmeraldResourceCompatImage_GetEntryCount(sImage)
                          : 0u;
}

uint32_t EmeraldTilesetCompat_GetEntrySchema(size_t i)
{
    if (i >= sEntryCount)
        return 0u;
    return sEntries[i].schema;
}

uint32_t EmeraldTilesetCompat_GetEntryRole(size_t i)
{
    if (i >= sEntryCount)
        return EMERALD_RESOURCE_ROLE_CANONICAL;
    return sEntries[i].encoding == EMERALD_COMPAT_ENTRY_GBA_LZ
        ? EMERALD_RESOURCE_ROLE_LEGACY_LZ
        : EMERALD_RESOURCE_ROLE_CANONICAL;
}

#endif /* PLATFORM_SDL2 && NATIVE_LINUX */

/*
 * gen_tileset_family — R11-C tileset graphics family generator.
 *
 * Consumes the machine-readable inventory emitted by
 * gen_tileset_inventory.py
 * (resources/extraction/emerald/bpee01/tileset/inventory.generated.toml)
 * and emits six deterministic, bytewise-sorted metadata files:
 *
 *   catalog.generated.toml         — full family resource catalog (1544).
 *   bindings.generated.toml        — semantic extraction bindings (1544).
 *   ownership.generated.toml       — native/GBA ownership per resource +
 *                                    GBA_PARITY documentation rows.
 *   tileset_consumers.generated.toml — struct -> resource consumer map.
 *   tileset_native.generated.h     — NULL-sentinel native slots: the 75
 *                                    struct Tileset objects (callbacks
 *                                    compiled), 1200 palette-row arrays,
 *                                    125 anim-frame leaves, 4 floor-light
 *                                    palettes (include-once toggle).
 *   tileset_frames.generated.h     — seam publication rows: struct pointers,
 *                                    palette rows, anim frames, floor light.
 *
 * Canonical-name rule (R11-L/R11-B): canonical = ASCII-lowercase of the
 * symbol suffix with underscores converted to hyphens:
 *   gTilesetTiles_Petalburg  -> "petalburg"
 *   gTilesetAnims_General_Flower_Frame1 -> "general-flower" (frame 1)
 * IDs:
 *   emerald:tileset/<canonical>/tiles             (type tile-graphics, s1)
 *   emerald:tileset/<canonical>/palette/<row 00-15> (type palette, s1)
 *   emerald:tileset/<canonical>/metatiles         (type tileset, s1)
 *   emerald:tileset/<canonical>/metatile-attributes (type tileset, s2)
 *   emerald:tileset-anim/<canonical>/frame/<n>    (type tile-graphics, s1)
 *   emerald:tileset-anim/battle-dome/floor-light-pal/<0-3> (type palette, s1)
 *
 * Encodings: compressed tile leaves are gba-lz77 streams (strict three-way
 * decode: LZ header declared size == 16384, strict decode OK, decoded ==
 * 16384); everything else is raw (artifact IS the canonical decoded bytes).
 * Anim frames with two artifacts (Sootopolis StormyWater) concatenate them
 * in order — the preproc emits every INCBIN argument, so the leaf symbol is
 * the concatenation (verified against the qualified reference ELF: symbol
 * size 0xC00 == kyogre 1536 + groudon 1536).
 *
 * Determinism: output order independent of inventory order; resources
 * bytewise by canonical id, structs/palette rows/frames/tables by symbol,
 * consumers by (struct, kind, resource). Running twice produces
 * byte-identical files.
 *
 * Fail-closed validation: duplicate canonical ids, invalid struct symbols,
 * artifact size/LZ violations, and consumer-reference mismatches are hard
 * failures.
 *
 * Usage:
 *   gen_tileset_family --inventory PATH --catalog-out PATH --bindings-out PATH
 *       --ownership-out PATH --consumers-out PATH
 *       --native-header-out PATH --frames-header-out PATH
 *   gen_tileset_family --inventory PATH --check <same outputs>
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/lz77.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/sha256.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"

#define MAX_RESOURCES 2048u
#define MAX_STRUCTS 128u
#define MAX_PALETTE_ARRAYS 96u
#define MAX_PALETTE_ROWS 1280u
#define MAX_FRAME_TABLES 64u
#define MAX_CONSUMERS 4096u
#define MAX_PARITY 32u

#define TILESET_TILE_DECODED_SIZE 16384u /* raw (uncompressed) tilesets only */
#define TILESET_TILE_DECODED_SIZE_MAX 65536u

enum ResourceKind
{
    KIND_TILE,
    KIND_PALETTE_ROW,
    KIND_METATILES,
    KIND_METATILE_ATTRIBUTES,
    KIND_ANIM_FRAME,
    KIND_FLOOR_LIGHT_PAL
};

struct FamilyResource
{
    enum ResourceKind kind;
    char symbol[96];    /* legacy leaf symbol */
    char canonical[96];
    char id[192];
    char artifact[256];
    char artifact2[256]; /* optional concatenation tail (StormyWater) */
    bool hasArtifact2;
    bool compressed;    /* gba-lz77 vs raw */
    uint32_t row;       /* palette row index, or floor-light index */
    uint64_t byteLength; /* encoded artifact size (compressed) / decoded size */
    uint64_t decodedLength;
    char encodedSha256[GEN3_RESOURCE_KEY_HEX_SIZE];
    char canonicalSha256[GEN3_RESOURCE_KEY_HEX_SIZE];
    char ownershipKey[GEN3_RESOURCE_KEY_HEX_SIZE];
};

struct StructRecord
{
    char symbol[96];
    bool isCompressed;
    bool isSecondary;
    char callback[96];
    char tiles[96];
    char palettes[96];
    char metatiles[96];
    char attributes[96];
};

struct FrameTableRecord
{
    char symbol[96];
    uint32_t frameCount;
};

struct FrameConsumer
{
    char table[96];
    char frame[96];
    char resourceId[192];
};

struct StructConsumer
{
    char structName[96];
    char kind[24];
    uint32_t row;
    char resourceId[192];
};

struct FloorLightConsumer
{
    char symbol[96];
    char resourceId[192];
};

struct ParityRecord
{
    char symbol[96];
    char kind[24];
    char artifact[256];
};

static void Fail(const char *format, ...)
{
    va_list args;
    fprintf(stderr, "FAIL: ");
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(1);
}

static bool GetStringRequired(const struct Gen3TomlMap *map, const char *key,
                              char *out, size_t outSize)
{
    const char *value;
    if (!Gen3Toml_GetString(map, key, &value) || value == NULL)
        return false;
    if (strlen(value) >= outSize)
        return false;
    snprintf(out, outSize, "%s", value);
    return true;
}

static bool GetIntegerRequired(const struct Gen3TomlMap *map, const char *key,
                               long long *out)
{
    return Gen3Toml_GetInteger(map, key, out);
}

static bool GetBoolRequired(const struct Gen3TomlMap *map, const char *key,
                            bool *out)
{
    return Gen3Toml_GetBool(map, key, out);
}

/* Lowercase suffix after the known prefix, '_' -> '-'. */
static void CanonicalizeWithPrefix(const char *symbol, const char *prefix,
                                   char *out, size_t outSize)
{
    size_t prefixLength = strlen(prefix);
    const char *suffix;
    size_t i;

    if (strncmp(symbol, prefix, prefixLength) != 0)
        Fail("%s does not start with %s", symbol, prefix);
    suffix = symbol + prefixLength;
    for (i = 0; suffix[i] != '\0' && i + 1 < outSize; i++)
        out[i] = suffix[i] == '_' ? '-' : (char)((unsigned char)suffix[i] | 0x20u);
    out[i] = '\0';
}

/* The inventory carries a `canonical` field for cross-checking the
 * recomputed canonical (the generator recomputes from the symbol so a stale
 * inventory cannot silently rename a resource). */
static void CheckInventoryCanonical(const struct Gen3TomlMap *item,
                                    const char *symbol, const char *computed)
{
    const char *expected;
    if (Gen3Toml_GetString(item, "canonical", &expected) && expected != NULL
     && strcmp(expected, computed) != 0)
        Fail("%s canonical mismatch: inventory says '%s', recomputed '%s'",
             symbol, expected, computed);
}

static void CanonicalAnimFrame(const char *symbol, char *out, size_t outSize)
{
    /* gTilesetAnims_<Anim>_Frame<n> -> lowercase <Anim>, '_' -> '-'. */
    const char *marker = strstr(symbol, "_Frame");
    size_t prefixLength;
    size_t i;

    if (marker == NULL)
        Fail("%s is not an anim frame leaf", symbol);
    prefixLength = strlen("gTilesetAnims_");
    for (i = 0; i < (size_t)(marker - (symbol + prefixLength)) && i + 1 < outSize; i++)
    {
        char c = symbol[prefixLength + i];
        out[i] = c == '_' ? '-' : (char)((unsigned char)c | 0x20u);
    }
    out[i] = '\0';
}

static int CompareResourceById(const void *left, const void *right)
{
    return strcmp(((const struct FamilyResource *)left)->id,
                  ((const struct FamilyResource *)right)->id);
}

static int CompareStructBySymbol(const void *left, const void *right)
{
    return strcmp(((const struct StructRecord *)left)->symbol,
                  ((const struct StructRecord *)right)->symbol);
}

static int CompareFrameTable(const void *left, const void *right)
{
    return strcmp(((const struct FrameTableRecord *)left)->symbol,
                  ((const struct FrameTableRecord *)right)->symbol);
}

static int CompareFrameConsumer(const void *left, const void *right)
{
    const struct FrameConsumer *l = left;
    const struct FrameConsumer *r = right;
    int cmp = strcmp(l->table, r->table);
    if (cmp != 0)
        return cmp;
    return strcmp(l->frame, r->frame);
}

static int CompareStructConsumer(const void *left, const void *right)
{
    const struct StructConsumer *l = left;
    const struct StructConsumer *r = right;
    int cmp = strcmp(l->structName, r->structName);
    if (cmp != 0)
        return cmp;
    cmp = strcmp(l->kind, r->kind);
    if (cmp != 0)
        return cmp;
    if (l->row < r->row)
        return -1;
    if (l->row > r->row)
        return 1;
    return strcmp(l->resourceId, r->resourceId);
}

static int CompareFloorLight(const void *left, const void *right)
{
    return strcmp(((const struct FloorLightConsumer *)left)->symbol,
                  ((const struct FloorLightConsumer *)right)->symbol);
}

static int CompareParity(const void *left, const void *right)
{
    return strcmp(((const struct ParityRecord *)left)->symbol,
                  ((const struct ParityRecord *)right)->symbol);
}

/* Load artifact(s) into `payload` (single file, or the concatenation when
 * artifact2 is set, mirroring the preproc's emit-every-argument semantics). */
static void LoadArtifact(const struct FamilyResource *r, char *errbuf,
                         size_t errbufSize, struct Gen3Buffer *payload)
{
    if (!Gen3Util_ReadFile(r->artifact, payload, errbuf, errbufSize))
        Fail("resource %s: cannot read artifact %s: %s", r->id, r->artifact,
             errbuf);
    if (r->hasArtifact2)
    {
        struct Gen3Buffer second;
        Gen3Buffer_Init(&second, 4096);
        if (!Gen3Util_ReadFile(r->artifact2, &second, errbuf, errbufSize))
            Fail("resource %s: cannot read artifact %s: %s", r->id, r->artifact2,
                 errbuf);
        if (!Gen3Buffer_Append(payload, second.data, second.length))
            Fail("resource %s: out of memory concatenating artifacts", r->id);
        Gen3Buffer_Destroy(&second);
    }
}

static void Sha256Hex(const void *data, size_t length, char *out)
{
    struct Gen3Sha256Context sha;
    uint8_t digest[32];
    Gen3Sha256_Init(&sha);
    Gen3Sha256_Update(&sha, (const uint8_t *)data, length);
    Gen3Sha256_Final(&sha, digest);
    Gen3Util_FormatHex(digest, sizeof(digest), out);
}

int main(int argc, char **argv)
{
    const char *inventoryPath = NULL;
    const char *catalogOut = NULL;
    const char *bindingsOut = NULL;
    const char *ownershipOut = NULL;
    const char *consumersOut = NULL;
    const char *nativeHeaderOut = NULL;
    const char *framesHeaderOut = NULL;
    bool check = false;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--inventory") == 0 && i + 1 < argc)
            inventoryPath = argv[++i];
        else if (strcmp(argv[i], "--catalog-out") == 0 && i + 1 < argc)
            catalogOut = argv[++i];
        else if (strcmp(argv[i], "--bindings-out") == 0 && i + 1 < argc)
            bindingsOut = argv[++i];
        else if (strcmp(argv[i], "--ownership-out") == 0 && i + 1 < argc)
            ownershipOut = argv[++i];
        else if (strcmp(argv[i], "--consumers-out") == 0 && i + 1 < argc)
            consumersOut = argv[++i];
        else if (strcmp(argv[i], "--native-header-out") == 0 && i + 1 < argc)
            nativeHeaderOut = argv[++i];
        else if (strcmp(argv[i], "--frames-header-out") == 0 && i + 1 < argc)
            framesHeaderOut = argv[++i];
        else if (strcmp(argv[i], "--check") == 0)
            check = true;
        else
        {
            fprintf(stderr, "usage: gen_tileset_family --inventory PATH "
                    "--catalog-out PATH --bindings-out PATH "
                    "--ownership-out PATH --consumers-out PATH "
                    "--native-header-out PATH --frames-header-out PATH "
                    "[--check]\n");
            return 2;
        }
    }
    if (inventoryPath == NULL || catalogOut == NULL || bindingsOut == NULL
     || ownershipOut == NULL || consumersOut == NULL || nativeHeaderOut == NULL
     || framesHeaderOut == NULL)
    {
        fprintf(stderr, "gen_tileset_family: missing required argument\n");
        return 2;
    }

    {
        char errbuf[256];
        struct Gen3Buffer text;
        struct Gen3TomlDocument doc;
        struct FamilyResource resources[MAX_RESOURCES];
        struct StructRecord structs[MAX_STRUCTS];
        struct FrameTableRecord frameTables[MAX_FRAME_TABLES];
        struct FrameConsumer frameConsumers[MAX_CONSUMERS];
        struct StructConsumer structConsumers[MAX_CONSUMERS];
        struct FloorLightConsumer floorLightConsumers[8];
        struct ParityRecord parity[MAX_PARITY];
        char paletteArrays[MAX_PALETTE_ARRAYS][96];
        size_t resourceCount = 0;
        size_t structCount = 0;
        size_t frameTableCount = 0;
        size_t frameConsumerCount = 0;
        size_t structConsumerCount = 0;
        size_t floorLightCount = 0;
        size_t parityCount = 0;
        size_t paletteArrayCount = 0;
        size_t kindCounts[6] = { 0, 0, 0, 0, 0, 0 };
        size_t n;

        Gen3Buffer_Init(&text, 8192);
        if (!Gen3Util_ReadFile(inventoryPath, &text, errbuf, sizeof(errbuf)))
            Fail("cannot read inventory %s: %s", inventoryPath, errbuf);
        if (!Gen3Toml_Parse(text.data, text.length, &doc, errbuf, sizeof(errbuf)))
            Fail("cannot parse inventory %s: %s", inventoryPath, errbuf);

        /* Structs. */
        n = Gen3Toml_GetArrayCount(&doc.root, "structs");
        if (n == 0 || n > MAX_STRUCTS)
            Fail("inventory struct count %zu out of range", n);
        for (i = 0; i < (int)n; i++)
        {
            const struct Gen3TomlMap *item =
                Gen3Toml_GetArrayItem(&doc.root, "structs", (size_t)i);
            struct StructRecord *s = &structs[structCount];
            bool isCompressed;
            bool isSecondary;

            memset(s, 0, sizeof(*s));
            if (!GetStringRequired(item, "symbol", s->symbol, sizeof(s->symbol))
             || !GetStringRequired(item, "tiles", s->tiles, sizeof(s->tiles))
             || !GetStringRequired(item, "palettes", s->palettes,
                                   sizeof(s->palettes))
             || !GetStringRequired(item, "metatiles", s->metatiles,
                                   sizeof(s->metatiles))
             || !GetStringRequired(item, "metatile_attributes", s->attributes,
                                   sizeof(s->attributes))
             || !GetStringRequired(item, "callback", s->callback,
                                   sizeof(s->callback))
             || !GetBoolRequired(item, "is_compressed", &isCompressed)
             || !GetBoolRequired(item, "is_secondary", &isSecondary))
                Fail("struct %zu missing required fields", i);
            s->isCompressed = isCompressed;
            s->isSecondary = isSecondary;
            structCount++;
        }

        /* Resources. */
        n = Gen3Toml_GetArrayCount(&doc.root, "resources");
        if (n == 0 || n > MAX_RESOURCES)
            Fail("inventory resource count %zu out of range", n);
        for (i = 0; i < (int)n; i++)
        {
            const struct Gen3TomlMap *item =
                Gen3Toml_GetArrayItem(&doc.root, "resources", (size_t)i);
            struct FamilyResource *r = &resources[resourceCount];
            char kind[32];
            bool parityFlag = false;

            memset(r, 0, sizeof(*r));
            if (!GetStringRequired(item, "symbol", r->symbol, sizeof(r->symbol))
             || !GetStringRequired(item, "kind", kind, sizeof(kind)))
                Fail("resource %zu missing symbol/kind", i);
            if (strcmp(kind, "tile") == 0)
            {
                r->kind = KIND_TILE;
                if (!GetStringRequired(item, "source_artifact", r->artifact,
                                       sizeof(r->artifact)))
                    Fail("tile %s missing source_artifact", r->symbol);
                r->compressed = strstr(r->artifact, ".lz") != NULL;
                if (!GetBoolRequired(item, "parity", &parityFlag))
                    parityFlag = false;
                if (parityFlag)
                {
                    if (parityCount >= MAX_PARITY)
                        Fail("too many parity tiles");
                    snprintf(parity[parityCount].symbol,
                             sizeof(parity[parityCount].symbol), "%s", r->symbol);
                    snprintf(parity[parityCount].kind,
                             sizeof(parity[parityCount].kind), "%s", "tile");
                    snprintf(parity[parityCount].artifact,
                             sizeof(parity[parityCount].artifact), "%s",
                             r->artifact);
                    parityCount++;
                    continue; /* GBA_PARITY: no canonical id, no resource */
                }
                CanonicalizeWithPrefix(r->symbol, "gTilesetTiles_",
                                       r->canonical, sizeof(r->canonical));
                CheckInventoryCanonical(item, r->symbol, r->canonical);
                {
                    char canonicalCopy[96];
                    snprintf(canonicalCopy, sizeof(canonicalCopy), "%s",
                             r->canonical);
                    snprintf(r->id, sizeof(r->id), "emerald:tileset/%s/tiles",
                             canonicalCopy);
                }
            }
            else if (strcmp(kind, "palette_array") == 0)
            {
                /* The 75 arrays are not resources themselves — their 1,200
                 * rows are ([[palette_rows]]). Record the array canonical
                 * for the exactly-16-rows cross-check below, then skip. */
                CanonicalizeWithPrefix(r->symbol, "gTilesetPalettes_",
                                       r->canonical, sizeof(r->canonical));
                CheckInventoryCanonical(item, r->symbol, r->canonical);
                if (paletteArrayCount >= MAX_PALETTE_ARRAYS)
                    Fail("too many palette arrays");
                snprintf(paletteArrays[paletteArrayCount],
                         sizeof(paletteArrays[0]), "%s", r->canonical);
                paletteArrayCount++;
                continue;
            }
            else if (strcmp(kind, "metatiles") == 0)
            {
                r->kind = KIND_METATILES;
                if (!GetStringRequired(item, "source_artifact", r->artifact,
                                       sizeof(r->artifact)))
                    Fail("metatiles %s missing source_artifact", r->symbol);
                CanonicalizeWithPrefix(r->symbol, "gMetatiles_", r->canonical,
                                       sizeof(r->canonical));
                CheckInventoryCanonical(item, r->symbol, r->canonical);
                {
                    char canonicalCopy[96];
                    snprintf(canonicalCopy, sizeof(canonicalCopy), "%s",
                             r->canonical);
                    snprintf(r->id, sizeof(r->id),
                             "emerald:tileset/%s/metatiles", canonicalCopy);
                }
            }
            else if (strcmp(kind, "metatile_attributes") == 0)
            {
                r->kind = KIND_METATILE_ATTRIBUTES;
                if (!GetStringRequired(item, "source_artifact", r->artifact,
                                       sizeof(r->artifact)))
                    Fail("attributes %s missing source_artifact", r->symbol);
                CanonicalizeWithPrefix(r->symbol, "gMetatileAttributes_",
                                       r->canonical, sizeof(r->canonical));
                CheckInventoryCanonical(item, r->symbol, r->canonical);
                {
                    char canonicalCopy[96];
                    snprintf(canonicalCopy, sizeof(canonicalCopy), "%s",
                             r->canonical);
                    snprintf(r->id, sizeof(r->id),
                             "emerald:tileset/%s/metatile-attributes",
                             canonicalCopy);
                }
            }
            else if (strcmp(kind, "anim_frame") == 0)
            {
                long long frame;
                const char *second;
                r->kind = KIND_ANIM_FRAME;
                if (!GetStringRequired(item, "source_artifact", r->artifact,
                                       sizeof(r->artifact))
                 || !GetIntegerRequired(item, "frame", &frame))
                    Fail("anim frame %s missing source_artifact/frame", r->symbol);
                if (frame < 0 || frame > 255)
                    Fail("anim frame %s has invalid frame %lld", r->symbol, frame);
                r->row = (uint32_t)frame;
                if (Gen3Toml_GetString(item, "source_artifact_2", &second)
                 && second != NULL)
                {
                    snprintf(r->artifact2, sizeof(r->artifact2), "%s", second);
                    r->hasArtifact2 = true;
                }
                CanonicalAnimFrame(r->symbol, r->canonical, sizeof(r->canonical));
                CheckInventoryCanonical(item, r->symbol, r->canonical);
                {
                    char canonicalCopy[96];
                    snprintf(canonicalCopy, sizeof(canonicalCopy), "%s",
                             r->canonical);
                    snprintf(r->id, sizeof(r->id),
                             "emerald:tileset-anim/%s/frame/%u", canonicalCopy,
                             r->row);
                }
            }
            else if (strcmp(kind, "floor_light_pal") == 0)
            {
                r->kind = KIND_FLOOR_LIGHT_PAL;
                if (!GetStringRequired(item, "source_artifact", r->artifact,
                                       sizeof(r->artifact)))
                    Fail("floor light %s missing source_artifact", r->symbol);
                r->row = (uint32_t)(r->symbol[strlen(r->symbol) - 1u] - '0');
                if (r->row > 3)
                    Fail("floor light %s has invalid index %u", r->symbol,
                         r->row);
                snprintf(r->id, sizeof(r->id),
                         "emerald:tileset-anim/battle-dome/floor-light-pal/%u",
                         r->row);
            }
            else
                Fail("resource %s has unknown kind '%s'", r->symbol, kind);
            kindCounts[r->kind]++;
            resourceCount++;
        }

        /* Palette rows: one resource per (array, row). */
        n = Gen3Toml_GetArrayCount(&doc.root, "palette_rows");
        if (n == 0 || n > MAX_PALETTE_ROWS)
            Fail("inventory palette_row count %zu out of range", n);
        for (i = 0; i < (int)n; i++)
        {
            const struct Gen3TomlMap *item =
                Gen3Toml_GetArrayItem(&doc.root, "palette_rows", (size_t)i);
            struct FamilyResource *r = &resources[resourceCount];
            char arraySymbol[96];
            long long row;
            char arrayCanonical[96];

            memset(r, 0, sizeof(*r));
            r->kind = KIND_PALETTE_ROW;
            if (!GetStringRequired(item, "array", arraySymbol,
                                   sizeof(arraySymbol))
             || !GetIntegerRequired(item, "row", &row)
             || !GetStringRequired(item, "source_artifact", r->artifact,
                                   sizeof(r->artifact)))
                Fail("palette_row %zu missing array/row/source_artifact", i);
            if (row < 0 || row > 15)
                Fail("palette row %s row %lld out of range", arraySymbol, row);
            CanonicalizeWithPrefix(arraySymbol, "gTilesetPalettes_",
                                   arrayCanonical, sizeof(arrayCanonical));
            snprintf(r->symbol, sizeof(r->symbol), "%s", arraySymbol);
            snprintf(r->canonical, sizeof(r->canonical), "%s", arrayCanonical);
            r->row = (uint32_t)row;
            snprintf(r->id, sizeof(r->id), "emerald:tileset/%s/palette/%02u",
                     arrayCanonical, (uint32_t)row);
            kindCounts[KIND_PALETTE_ROW]++;
            resourceCount++;
        }

        /* Cross-check: every palette array must have exactly 16 rows, and
         * every row must belong to a known array. */
        {
            size_t j;
            for (i = 0; i < (int)paletteArrayCount; i++)
            {
                size_t rowCount = 0;
                for (j = 0; j < resourceCount; j++)
                {
                    if (resources[j].kind == KIND_PALETTE_ROW
                     && strcmp(resources[j].canonical, paletteArrays[i]) == 0)
                        rowCount++;
                }
                if (rowCount != 16)
                    Fail("palette array %s has %zu rows (expected 16)",
                         paletteArrays[i], rowCount);
            }
            for (j = 0; j < resourceCount; j++)
            {
                size_t k;
                if (resources[j].kind != KIND_PALETTE_ROW)
                    continue;
                for (k = 0; k < paletteArrayCount; k++)
                {
                    if (strcmp(resources[j].canonical, paletteArrays[k]) == 0)
                        break;
                }
                if (k == paletteArrayCount)
                    Fail("palette row %s belongs to unknown array",
                         resources[j].id);
            }
        }

        /* Frame tables. */
        n = Gen3Toml_GetArrayCount(&doc.root, "frame_tables");
        if (n == 0 || n > MAX_FRAME_TABLES)
            Fail("inventory frame_table count %zu out of range", n);
        for (i = 0; i < (int)n; i++)
        {
            const struct Gen3TomlMap *item =
                Gen3Toml_GetArrayItem(&doc.root, "frame_tables", (size_t)i);
            long long count;
            if (!GetStringRequired(item, "symbol",
                                   frameTables[frameTableCount].symbol,
                                   sizeof(frameTables[0].symbol))
             || !GetIntegerRequired(item, "frame_count", &count))
                Fail("frame_table %zu missing symbol/frame_count", i);
            if (count <= 0)
                Fail("frame table %s has non-positive frame_count %lld",
                     frameTables[frameTableCount].symbol, count);
            frameTables[frameTableCount].frameCount = (uint32_t)count;
            frameTableCount++;
        }

        /* Frame consumers (table -> frame leaf -> resource). */
        n = Gen3Toml_GetArrayCount(&doc.root, "consumers");
        if (n == 0 || n > MAX_CONSUMERS)
            Fail("inventory consumer count %zu out of range", n);
        for (i = 0; i < (int)n; i++)
        {
            const struct Gen3TomlMap *item =
                Gen3Toml_GetArrayItem(&doc.root, "consumers", (size_t)i);
            struct FrameConsumer *c = &frameConsumers[frameConsumerCount];
            char frameSymbol[96];
            const struct FamilyResource *frameResource = NULL;
            size_t j;

            memset(c, 0, sizeof(*c));
            if (!GetStringRequired(item, "table", c->table, sizeof(c->table))
             || !GetStringRequired(item, "frame", frameSymbol,
                                   sizeof(frameSymbol)))
                Fail("consumer %zu missing table/frame", i);
            for (j = 0; j < resourceCount; j++)
            {
                if (resources[j].kind == KIND_ANIM_FRAME
                 && strcmp(resources[j].symbol, frameSymbol) == 0)
                {
                    frameResource = &resources[j];
                    break;
                }
            }
            if (frameResource == NULL)
                Fail("consumer %s references unknown anim frame %s", c->table,
                     frameSymbol);
            snprintf(c->frame, sizeof(c->frame), "%s", frameSymbol);
            snprintf(c->resourceId, sizeof(c->resourceId), "%s",
                     frameResource->id);
            frameConsumerCount++;
        }

        /* Parity frames. */
        n = Gen3Toml_GetArrayCount(&doc.root, "parity_frames");
        if (n > MAX_PARITY)
            Fail("inventory parity_frame count %zu out of range", n);
        for (i = 0; i < (int)n; i++)
        {
            const struct Gen3TomlMap *item =
                Gen3Toml_GetArrayItem(&doc.root, "parity_frames", (size_t)i);
            if (parityCount >= MAX_PARITY)
                Fail("too many parity frames");
            if (!GetStringRequired(item, "symbol", parity[parityCount].symbol,
                                   sizeof(parity[parityCount].symbol)))
                Fail("parity_frame %zu missing symbol", i);
            snprintf(parity[parityCount].kind, sizeof(parity[parityCount].kind),
                     "%s", "anim_frame");
            snprintf(parity[parityCount].artifact,
                     sizeof(parity[parityCount].artifact), "%s", "");
            parityCount++;
        }

        /* Struct -> resource consumers (validates every struct reference). */
        for (i = 0; i < (int)structCount; i++)
        {
            const struct StructRecord *s = &structs[i];
            const struct FamilyResource *tiles = NULL;
            const struct FamilyResource *metatiles = NULL;
            const struct FamilyResource *attributes = NULL;
            char tilesId[192];
            char metatilesId[192];
            char attributesId[192];
            char paletteCanonical[96];
            size_t j;

            /* Find the tiles resource by legacy symbol. */
            for (j = 0; j < resourceCount; j++)
            {
                if (resources[j].kind == KIND_TILE
                 && strcmp(resources[j].symbol, s->tiles) == 0)
                {
                    tiles = &resources[j];
                    break;
                }
            }
            if (tiles == NULL)
                Fail("struct %s references unknown tile leaf %s", s->symbol,
                     s->tiles);
            if (tiles->compressed != s->isCompressed)
                Fail("struct %s isCompressed mismatch with leaf %s", s->symbol,
                     s->tiles);
            for (j = 0; j < resourceCount; j++)
            {
                if (resources[j].kind == KIND_METATILES
                 && strcmp(resources[j].symbol, s->metatiles) == 0)
                {
                    metatiles = &resources[j];
                    break;
                }
            }
            if (metatiles == NULL)
                Fail("struct %s references unknown metatiles leaf %s", s->symbol,
                     s->metatiles);
            for (j = 0; j < resourceCount; j++)
            {
                if (resources[j].kind == KIND_METATILE_ATTRIBUTES
                 && strcmp(resources[j].symbol, s->attributes) == 0)
                {
                    attributes = &resources[j];
                    break;
                }
            }
            if (attributes == NULL)
                Fail("struct %s references unknown attributes leaf %s",
                     s->symbol, s->attributes);
            snprintf(tilesId, sizeof(tilesId), "%s", tiles->id);
            snprintf(metatilesId, sizeof(metatilesId), "%s", metatiles->id);
            snprintf(attributesId, sizeof(attributesId), "%s", attributes->id);
            CanonicalizeWithPrefix(s->palettes, "gTilesetPalettes_",
                                   paletteCanonical, sizeof(paletteCanonical));

            if (structConsumerCount + 19u > MAX_CONSUMERS)
                Fail("too many struct consumers");
            snprintf(structConsumers[structConsumerCount].structName,
                     sizeof(structConsumers[structConsumerCount].structName),
                     "%s", s->symbol);
            snprintf(structConsumers[structConsumerCount].kind,
                     sizeof(structConsumers[structConsumerCount].kind),
                     "%s", "tiles");
            snprintf(structConsumers[structConsumerCount].resourceId,
                     sizeof(structConsumers[structConsumerCount].resourceId),
                     "%s", tilesId);
            structConsumerCount++;
            for (j = 0; j < 16; j++)
            {
                snprintf(structConsumers[structConsumerCount].structName,
                         sizeof(structConsumers[structConsumerCount].structName),
                         "%s", s->symbol);
                snprintf(structConsumers[structConsumerCount].kind,
                         sizeof(structConsumers[structConsumerCount].kind),
                         "%s", "palette_row");
                structConsumers[structConsumerCount].row = (uint32_t)j;
                snprintf(structConsumers[structConsumerCount].resourceId,
                         sizeof(structConsumers[structConsumerCount].resourceId),
                         "emerald:tileset/%s/palette/%02u", paletteCanonical,
                         (uint32_t)j);
                structConsumerCount++;
            }
            snprintf(structConsumers[structConsumerCount].structName,
                     sizeof(structConsumers[structConsumerCount].structName),
                     "%s", s->symbol);
            snprintf(structConsumers[structConsumerCount].kind,
                     sizeof(structConsumers[structConsumerCount].kind),
                     "%s", "metatiles");
            snprintf(structConsumers[structConsumerCount].resourceId,
                     sizeof(structConsumers[structConsumerCount].resourceId),
                     "%s", metatilesId);
            structConsumerCount++;
            snprintf(structConsumers[structConsumerCount].structName,
                     sizeof(structConsumers[structConsumerCount].structName),
                     "%s", s->symbol);
            snprintf(structConsumers[structConsumerCount].kind,
                     sizeof(structConsumers[structConsumerCount].kind),
                     "%s", "metatile_attributes");
            snprintf(structConsumers[structConsumerCount].resourceId,
                     sizeof(structConsumers[structConsumerCount].resourceId),
                     "%s", attributesId);
            structConsumerCount++;
        }

        /* Floor-light consumers (the 4 pal symbols -> their resources). */
        for (i = 0; i < (int)resourceCount; i++)
        {
            if (resources[i].kind != KIND_FLOOR_LIGHT_PAL)
                continue;
            if (floorLightCount >= 8)
                Fail("too many floor-light consumers");
            snprintf(floorLightConsumers[floorLightCount].symbol,
                     sizeof(floorLightConsumers[floorLightCount].symbol), "%s",
                     resources[i].symbol);
            snprintf(floorLightConsumers[floorLightCount].resourceId,
                     sizeof(floorLightConsumers[floorLightCount].resourceId),
                     "%s", resources[i].id);
            floorLightCount++;
        }

        /* Artifact validation + digests. */
        for (i = 0; i < (int)resourceCount; i++)
        {
            struct FamilyResource *r = &resources[i];
            struct Gen3Buffer payload;
            struct Gen3Buffer decoded;
            Gen3ResourceKey key;

            Gen3Buffer_Init(&payload, 8192);
            LoadArtifact(r, errbuf, sizeof(errbuf), &payload);
            r->byteLength = payload.length;
            if (payload.length == 0)
                Fail("resource %s has an empty artifact", r->id);
            switch (r->kind)
            {
            case KIND_TILE:
                if (r->compressed)
                {
                    /* Compressed tilesets declare their own decoded size
                     * (BattleArena: 8704 = 544 tiles; most: 16384). The
                     * three-way validation is: header declared == strict
                     * decode output == per-record expected size. */
                    uint32_t declaredSize;
                    size_t decodedSize = 0;
                    enum Gen3Lz77Result lzResult;
                    if (payload.length < 4 || (uint8_t)payload.data[0] != 0x10)
                        Fail("tile %s is not a GBA LZ77 stream", r->id);
                    declaredSize = (uint32_t)(uint8_t)payload.data[1]
                                 | ((uint32_t)(uint8_t)payload.data[2] << 8)
                                 | ((uint32_t)(uint8_t)payload.data[3] << 16);
                    if (declaredSize == 0 || declaredSize > TILESET_TILE_DECODED_SIZE_MAX)
                        Fail("tile %s declares implausible decoded size %u",
                             r->id, declaredSize);
                    Gen3Buffer_Init(&decoded, declaredSize);
                    lzResult = Gen3Lz77_Decode(
                        (const uint8_t *)payload.data, payload.length,
                        (uint8_t *)decoded.data, declaredSize, &decodedSize);
                    if (lzResult != GEN3_LZ77_OK)
                        Fail("tile %s: LZ77 decode rejected (error %d)", r->id,
                             (int)lzResult);
                    if (decodedSize != declaredSize)
                        Fail("tile %s decoded %zu bytes (declared %u)", r->id,
                             decodedSize, declaredSize);
                    r->decodedLength = decodedSize;
                    Sha256Hex(payload.data, payload.length, r->encodedSha256);
                    Sha256Hex(decoded.data, decodedSize, r->canonicalSha256);
                    Gen3Buffer_Destroy(&decoded);
                }
                else
                {
                    /* Raw (uncompressed) tilesets carry their own size in
                     * the struct's u16 tilesSize field (SecretBase family:
                     * 2656 bytes; most others: 16384). The exact per-symbol
                     * size equivalence with the GBA build is enforced later
                     * by elf_manifest's byte-compare guardrails against the
                     * qualified ELF. */
                    if (payload.length < 32 || (payload.length & 31u) != 0
                     || payload.length > 65536u)
                        Fail("raw tile %s has implausible size %zu", r->id,
                             payload.length);
                    r->decodedLength = payload.length;
                    Sha256Hex(payload.data, payload.length, r->encodedSha256);
                    memcpy(r->canonicalSha256, r->encodedSha256,
                           GEN3_RESOURCE_KEY_HEX_SIZE);
                }
                break;
            case KIND_PALETTE_ROW:
            case KIND_FLOOR_LIGHT_PAL:
                if (payload.length != 32)
                    Fail("palette %s has %zu bytes (expected 32)", r->id,
                         payload.length);
                r->decodedLength = payload.length;
                Sha256Hex(payload.data, payload.length, r->encodedSha256);
                memcpy(r->canonicalSha256, r->encodedSha256,
                       GEN3_RESOURCE_KEY_HEX_SIZE);
                break;
            case KIND_METATILES:
            case KIND_METATILE_ATTRIBUTES:
                if ((payload.length & 1u) != 0)
                    Fail("metatile blob %s has odd size %zu", r->id,
                         payload.length);
                r->decodedLength = payload.length;
                Sha256Hex(payload.data, payload.length, r->encodedSha256);
                memcpy(r->canonicalSha256, r->encodedSha256,
                       GEN3_RESOURCE_KEY_HEX_SIZE);
                break;
            case KIND_ANIM_FRAME:
                if ((payload.length & 31u) != 0)
                    Fail("anim frame %s has non-tile-aligned size %zu", r->id,
                         payload.length);
                r->decodedLength = payload.length;
                Sha256Hex(payload.data, payload.length, r->encodedSha256);
                memcpy(r->canonicalSha256, r->encodedSha256,
                       GEN3_RESOURCE_KEY_HEX_SIZE);
                break;
            }
            Gen3ResourceId_DeriveKey(r->id, &key);
            Gen3ResourceId_FormatKeyHex(&key, r->ownershipKey);
            Gen3Buffer_Destroy(&payload);
        }

        /* Duplicate canonical ids. */
        for (i = 0; i < (int)resourceCount; i++)
        {
            size_t j;
            for (j = (size_t)i + 1; j < resourceCount; j++)
            {
                if (strcmp(resources[i].id, resources[j].id) == 0)
                    Fail("duplicate canonical id %s (%s and %s)",
                         resources[i].id, resources[i].symbol,
                         resources[j].symbol);
            }
        }

        /* Deterministic order. */
        qsort(resources, resourceCount, sizeof(resources[0]),
              CompareResourceById);
        qsort(structs, structCount, sizeof(structs[0]), CompareStructBySymbol);
        qsort(frameTables, frameTableCount, sizeof(frameTables[0]),
              CompareFrameTable);
        qsort(frameConsumers, frameConsumerCount, sizeof(frameConsumers[0]),
              CompareFrameConsumer);
        qsort(structConsumers, structConsumerCount, sizeof(structConsumers[0]),
              CompareStructConsumer);
        qsort(floorLightConsumers, floorLightCount,
              sizeof(floorLightConsumers[0]), CompareFloorLight);
        qsort(parity, parityCount, sizeof(parity[0]), CompareParity);

        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&text);

        /* Emit. */
        {
            struct Gen3Buffer catalog;
            struct Gen3Buffer bindings;
            struct Gen3Buffer ownership;
            struct Gen3Buffer consumerFile;
            struct Gen3Buffer nativeHeader;
            struct Gen3Buffer framesHeader;
            static const char header[] =
                "# Generated by tools/gen3_resources/tileset_family/gen_tileset_family.\n"
                "# Do not edit by hand; edit the tileset inventory and re-run the generator.\n\n";
            static const char seamHeader[] =
                "// Generated by tools/gen3_resources/tileset_family/gen_tileset_family.\n"
                "// Do not edit by hand; edit the tileset inventory and re-run the generator.\n"
                "// Native-only: the payload leaves are ROM_BASE; the compiled structs carry\n"
                "// NULL payload pointers and the compat seam publishes session streams.\n\n";

            Gen3Buffer_Init(&catalog, 65536);
            Gen3Buffer_Init(&bindings, 65536);
            Gen3Buffer_Init(&ownership, 65536);
            Gen3Buffer_Init(&consumerFile, 65536);
            Gen3Buffer_Init(&nativeHeader, 65536);
            Gen3Buffer_Init(&framesHeader, 65536);
            Gen3Buffer_AppendCStr(&catalog, header);
            Gen3Buffer_AppendCStr(&bindings, header);
            Gen3Buffer_AppendCStr(&ownership, header);
            Gen3Buffer_AppendCStr(&consumerFile, header);

            Gen3Buffer_AppendCStr(&catalog, "catalog_version = 1\n"
                                   "namespace = \"emerald\"\n"
                                   "resource_api = \"1.0.0\"\n"
                                   "game = \"emerald\"\n"
                                   "rom_profile = \"bpee01-rev0\"\n\n");
            for (i = 0; i < (int)resourceCount; i++)
            {
                const struct FamilyResource *r = &resources[i];
                const char *type;
                uint32_t schema;
                switch (r->kind)
                {
                case KIND_TILE:
                case KIND_ANIM_FRAME:
                    type = "tile-graphics";
                    schema = 1u;
                    break;
                case KIND_PALETTE_ROW:
                case KIND_FLOOR_LIGHT_PAL:
                    type = "palette";
                    schema = 1u;
                    break;
                case KIND_METATILES:
                    type = "tileset";
                    schema = 1u;
                    break;
                default:
                    type = "tileset";
                    schema = 2u;
                    break;
                }
                Gen3Buffer_AppendFormat(&catalog,
                    "[[resources]]\nid = \"%s\"\ntype = \"%s\"\nschema = %u\n"
                    "required_for_base = true\n",
                    r->id, type, schema);
            }

            Gen3Buffer_AppendCStr(&bindings, "bindings_version = 1\n"
                                  "game = \"emerald\"\n"
                                  "rom_profile = \"bpee01-rev0\"\n\n");
            for (i = 0; i < (int)resourceCount; i++)
            {
                const struct FamilyResource *r = &resources[i];
                const char *encoding = r->compressed ? "gba-lz77" : "raw";
                const char *representation;
                switch (r->kind)
                {
                case KIND_TILE:
                case KIND_ANIM_FRAME:
                    representation = "gba-4bpp-tiles";
                    break;
                case KIND_METATILES:
                    representation = "gba-metatile-defs";
                    break;
                case KIND_METATILE_ATTRIBUTES:
                    representation = "gba-metatile-attributes";
                    break;
                default:
                    representation = "gba-bgr555-palette";
                    break;
                }
                Gen3Buffer_AppendFormat(&bindings,
                    "[[bindings]]\nid = \"%s\"\nsymbol = \"%s\"\n"
                    "source_artifact = \"%s\"\nsource_encoding = \"%s\"\n"
                    "canonical_representation = \"%s\"\n"
                    "expected_decoded_size = %llu\n",
                    r->id, r->symbol, r->artifact, encoding, representation,
                    (unsigned long long)r->decodedLength);
                if (r->hasArtifact2)
                    Gen3Buffer_AppendFormat(&bindings,
                        "source_artifact_2 = \"%s\"\n", r->artifact2);
                if (r->kind == KIND_PALETTE_ROW)
                    Gen3Buffer_AppendFormat(&bindings,
                        "symbol_offset = %u\n", r->row * 32u);
            }

            Gen3Buffer_AppendCStr(&ownership, "ownership_version = 1\n"
                                  "game = \"emerald\"\n"
                                  "rom_profile = \"bpee01-rev0\"\n\n");
            for (i = 0; i < (int)resourceCount; i++)
            {
                const struct FamilyResource *r = &resources[i];
                const char *type;
                const char *encoding;
                switch (r->kind)
                {
                case KIND_TILE:
                    type = "tile-graphics";
                    break;
                case KIND_PALETTE_ROW:
                case KIND_FLOOR_LIGHT_PAL:
                    type = "palette";
                    break;
                case KIND_METATILES:
                    type = "tileset";
                    break;
                case KIND_METATILE_ATTRIBUTES:
                    type = "tileset";
                    break;
                default:
                    type = "tile-graphics";
                    break;
                }
                encoding = r->compressed ? "gba-lz77" : "raw";
                Gen3Buffer_AppendFormat(&ownership,
                    "[[resources]]\nid = \"%s\"\nkey = \"%s\"\n"
                    "legacy_symbol = \"%s\"\ntype = \"%s\"\n"
                    "source_artifact = \"%s\"\n",
                    r->id, r->ownershipKey, r->symbol, type, r->artifact);
                /* Multi-file concatenations (Sootopolis StormyWater frames
                 * INCBIN kyogre + groudon): the ownership record must carry
                 * the full artifact list the runner concatenates, mirroring
                 * the bindings emission above. */
                if (r->hasArtifact2)
                    Gen3Buffer_AppendFormat(&ownership,
                        "source_artifact_2 = \"%s\"\n", r->artifact2);
                Gen3Buffer_AppendFormat(&ownership,
                    "encoded_length = %llu\ndecoded_length = %llu\n"
                    "source_encoding = \"%s\"\n"
                    "source_encoded_sha256 = \"%s\"\n"
                    "canonical_decoded_sha256 = \"%s\"\n"
                    "ownership_state = \"ROM_BASE_ONLY\"\n\n"
                    "[resources.targets]\nnative = \"ROM_BASE_ONLY\"\n"
                    "gba = \"COMPILED\"\n",
                    (unsigned long long)r->byteLength,
                    (unsigned long long)r->decodedLength, encoding,
                    r->encodedSha256, r->canonicalSha256);
            }
            /* GBA_PARITY documentation rows (no canonical id, no resource). */
            for (i = 0; i < (int)parityCount; i++)
            {
                Gen3Buffer_AppendFormat(&ownership,
                    "\n[[gba_parity]]\nsymbol = \"%s\"\nkind = \"%s\"\n"
                    "source_artifact = \"%s\"\n",
                    parity[i].symbol, parity[i].kind, parity[i].artifact);
            }

            Gen3Buffer_AppendCStr(&consumerFile, "consumers_version = 1\n"
                                  "game = \"emerald\"\n"
                                  "rom_profile = \"bpee01-rev0\"\n\n");
            for (i = 0; i < (int)structCount; i++)
            {
                Gen3Buffer_AppendFormat(&consumerFile,
                    "[[structs]]\nsymbol = \"%s\"\nis_compressed = %s\n"
                    "is_secondary = %s\ncallback = \"%s\"\n",
                    structs[i].symbol,
                    structs[i].isCompressed ? "true" : "false",
                    structs[i].isSecondary ? "true" : "false",
                    structs[i].callback);
            }
            for (i = 0; i < (int)structConsumerCount; i++)
            {
                Gen3Buffer_AppendFormat(&consumerFile,
                    "[[struct_consumers]]\nstruct = \"%s\"\nkind = \"%s\"\n"
                    "resource = \"%s\"\n",
                    structConsumers[i].structName, structConsumers[i].kind,
                    structConsumers[i].resourceId);
            }
            for (i = 0; i < (int)frameTableCount; i++)
            {
                Gen3Buffer_AppendFormat(&consumerFile,
                    "[[frame_tables]]\nsymbol = \"%s\"\nframe_count = %u\n",
                    frameTables[i].symbol, frameTables[i].frameCount);
            }
            for (i = 0; i < (int)frameConsumerCount; i++)
            {
                Gen3Buffer_AppendFormat(&consumerFile,
                    "[[frame_consumers]]\ntable = \"%s\"\nframe = \"%s\"\n"
                    "resource = \"%s\"\n",
                    frameConsumers[i].table, frameConsumers[i].frame,
                    frameConsumers[i].resourceId);
            }
            for (i = 0; i < (int)floorLightCount; i++)
            {
                Gen3Buffer_AppendFormat(&consumerFile,
                    "[[floor_light_consumers]]\nsymbol = \"%s\"\n"
                    "resource = \"%s\"\n",
                    floorLightConsumers[i].symbol,
                    floorLightConsumers[i].resourceId);
            }

            /* Native header: include-once toggle; definitions in the seam TU,
             * extern declarations everywhere else. The whole file is guarded
             * so data headers and the seam can both include it. */
            Gen3Buffer_AppendCStr(&nativeHeader, seamHeader);
            Gen3Buffer_AppendCStr(&nativeHeader,
                "#ifndef TILESET_NATIVE_GENERATED_H\n"
                "#define TILESET_NATIVE_GENERATED_H\n\n"
                "/* Include-once toggle: define TILESET_NATIVE_DEFINE in exactly one TU\n"
                " * (the compat seam) to emit the definitions; every other includer gets\n"
                " * the declarations. The initializer is a variadic argument so the\n"
                " * commas inside the braced lists are not macro separators. */\n"
                "#if defined(TILESET_NATIVE_DEFINE)\n"
                "#define TILESET_NATIVE_STRUCT(decl, ...) decl = __VA_ARGS__\n"
                "#define TILESET_NATIVE_ARRAY(decl, ...) decl = __VA_ARGS__\n"
                "#else\n"
                "#define TILESET_NATIVE_STRUCT(decl, ...) extern decl\n"
                "#define TILESET_NATIVE_ARRAY(decl, ...) extern decl\n"
                "#endif\n\n");
            Gen3Buffer_AppendCStr(&nativeHeader,
                "/* The 75 struct Tileset objects: payload members NULL-sentineled,\n"
                " * .callback kept compiled. */\n");
            for (i = 0; i < (int)structCount; i++)
            {
                const struct StructRecord *s = &structs[i];
                Gen3Buffer_AppendFormat(&nativeHeader,
                    "TILESET_NATIVE_STRUCT(struct Tileset %s, {\n"
                    "    .isCompressed = %s,\n"
                    "    .isSecondary = %s,\n"
                    "    .tiles = NULL,\n"
                    "    .palettes = NULL,\n"
                    "    .metatiles = NULL,\n"
                    "    .metatileAttributes = NULL,\n"
                    "    .callback = %s\n"
                    "});\n",
                    s->symbol, s->isCompressed ? "TRUE" : "FALSE",
                    s->isSecondary ? "TRUE" : "FALSE", s->callback);
            }
            /* One native slot per distinct symbol (resources are sorted by
             * id, so same-symbol rows are contiguous: the 1,200 palette rows
             * are 75 arrays x 16 rows and must collapse to 75 declarations;
             * tiles/metatiles/attributes are already one resource each). */
            Gen3Buffer_AppendCStr(&nativeHeader, "\n"
                "/* Migrated tile leaves: session-published u32 slots. Compressed\n"
                " * tilesets serve a literal-only GBA LZ77 stream via .tiles\n"
                " * (4-byte header + 9 bytes per 8 decoded bytes), so their arrays\n"
                " * are sized by the ENCODED length; raw tiles hold the decoded\n"
                " * bytes verbatim. */\n");
            {
                const char *lastSymbol = "";
                for (i = 0; i < (int)resourceCount; i++)
                {
                    const struct FamilyResource *r = &resources[i];
                    uint64_t slotBytes;
                    if (r->kind != KIND_TILE
                     || strcmp(lastSymbol, r->symbol) == 0)
                        continue;
                    lastSymbol = r->symbol;
                    if (r->compressed)
                    {
                        /* 4 + ceil(decoded / 8) * 9, rounded up to u32s. */
                        uint64_t groups = (r->decodedLength + 7u) / 8u;
                        slotBytes = (4u + groups * 9u + 3u) / 4u * 4u;
                    }
                    else
                    {
                        slotBytes = (r->decodedLength + 3u) / 4u * 4u;
                    }
                    Gen3Buffer_AppendFormat(&nativeHeader,
                        "TILESET_NATIVE_ARRAY(u32 %s[%llu / 4], {0});\n",
                        r->symbol, (unsigned long long)slotBytes);
                }
            }
            Gen3Buffer_AppendCStr(&nativeHeader, "\n"
                "/* Migrated palette-row arrays: session-published u16 slots. */\n");
            {
                const char *lastSymbol = "";
                for (i = 0; i < (int)resourceCount; i++)
                {
                    const struct FamilyResource *r = &resources[i];
                    if (r->kind != KIND_PALETTE_ROW
                     || strcmp(lastSymbol, r->symbol) == 0)
                        continue;
                    lastSymbol = r->symbol;
                    Gen3Buffer_AppendFormat(&nativeHeader,
                        "TILESET_NATIVE_ARRAY(u16 %s[16][16], {0});\n", r->symbol);
                }
            }
            Gen3Buffer_AppendCStr(&nativeHeader, "\n"
                "/* Migrated metatile definitions: session-published u16 slots. */\n");
            {
                const char *lastSymbol = "";
                for (i = 0; i < (int)resourceCount; i++)
                {
                    const struct FamilyResource *r = &resources[i];
                    if (r->kind != KIND_METATILES
                     || strcmp(lastSymbol, r->symbol) == 0)
                        continue;
                    lastSymbol = r->symbol;
                    Gen3Buffer_AppendFormat(&nativeHeader,
                        "TILESET_NATIVE_ARRAY(u16 %s[%llu / 2], {0});\n",
                        r->symbol, (unsigned long long)r->decodedLength);
                }
            }
            Gen3Buffer_AppendCStr(&nativeHeader, "\n"
                "/* Migrated metatile attributes: session-published u16 slots. */\n");
            {
                const char *lastSymbol = "";
                for (i = 0; i < (int)resourceCount; i++)
                {
                    const struct FamilyResource *r = &resources[i];
                    if (r->kind != KIND_METATILE_ATTRIBUTES
                     || strcmp(lastSymbol, r->symbol) == 0)
                        continue;
                    lastSymbol = r->symbol;
                    Gen3Buffer_AppendFormat(&nativeHeader,
                        "TILESET_NATIVE_ARRAY(u16 %s[%llu / 2], {0});\n",
                        r->symbol, (unsigned long long)r->decodedLength);
                }
            }
            Gen3Buffer_AppendCStr(&nativeHeader, "\n"
                "/* Migrated anim-frame leaves: session-published u16 slots. */\n");
            for (i = 0; i < (int)resourceCount; i++)
            {
                const struct FamilyResource *r = &resources[i];
                if (r->kind != KIND_ANIM_FRAME)
                    continue;
                Gen3Buffer_AppendFormat(&nativeHeader,
                    "TILESET_NATIVE_ARRAY(u16 %s[%llu / 2], {0});\n",
                    r->symbol, (unsigned long long)r->decodedLength);
            }
            Gen3Buffer_AppendCStr(&nativeHeader, "\n"
                "/* Migrated floor-light palettes: session-published u16 slots. */\n");
            for (i = 0; i < (int)resourceCount; i++)
            {
                const struct FamilyResource *r = &resources[i];
                if (r->kind != KIND_FLOOR_LIGHT_PAL)
                    continue;
                Gen3Buffer_AppendFormat(&nativeHeader,
                    "TILESET_NATIVE_ARRAY(u16 %s[16], {0});\n", r->symbol);
            }
            Gen3Buffer_AppendCStr(&nativeHeader,
                "\n#endif /* TILESET_NATIVE_GENERATED_H */\n");

            /* Publication rows. */
            Gen3Buffer_AppendCStr(&framesHeader, seamHeader);
            Gen3Buffer_AppendCStr(&framesHeader,
                "#ifndef TILESET_FRAMES_GENERATED_H\n"
                "#define TILESET_FRAMES_GENERATED_H\n\n"
                "#define TILESET_STRUCT_ROWS_DEFINED\n\n");
            for (i = 0; i < (int)structCount; i++)
            {
                const struct StructRecord *s = &structs[i];
                const struct FamilyResource *tiles = NULL;
                const struct FamilyResource *metatiles = NULL;
                const struct FamilyResource *attributes = NULL;
                size_t j;
                for (j = 0; j < resourceCount; j++)
                {
                    if (resources[j].kind == KIND_TILE
                     && strcmp(resources[j].symbol, s->tiles) == 0)
                        tiles = &resources[j];
                    if (resources[j].kind == KIND_METATILES
                     && strcmp(resources[j].symbol, s->metatiles) == 0)
                        metatiles = &resources[j];
                    if (resources[j].kind == KIND_METATILE_ATTRIBUTES
                     && strcmp(resources[j].symbol, s->attributes) == 0)
                        attributes = &resources[j];
                }
                /* Rows carry no trailing ';' (the object-event precedent):
                 * the compat seam re-includes this header under different
                 * row macros for its static tables and publish passes. */
                Gen3Buffer_AppendFormat(&framesHeader,
                    "TILESET_STRUCT(%s, \"%s\", %llu, \"%s\", %llu, \"%s\", "
                    "%llu, %s, %s)\n",
                    s->symbol, tiles->id,
                    (unsigned long long)tiles->decodedLength, metatiles->id,
                    (unsigned long long)metatiles->decodedLength,
                    attributes->id,
                    (unsigned long long)attributes->decodedLength,
                    s->isCompressed ? "TRUE" : "FALSE",
                    s->isSecondary ? "TRUE" : "FALSE");
            }
            Gen3Buffer_AppendCStr(&framesHeader, "\n#define TILESET_PALETTE_ROW_ROWS_DEFINED\n\n");
            for (i = 0; i < (int)resourceCount; i++)
            {
                const struct FamilyResource *r = &resources[i];
                if (r->kind != KIND_PALETTE_ROW)
                    continue;
                Gen3Buffer_AppendFormat(&framesHeader,
                    "TILESET_PALETTE_ROW(%s, %u, \"%s\")\n",
                    r->symbol, r->row, r->id);
            }
            Gen3Buffer_AppendCStr(&framesHeader, "\n#define TILESET_ANIM_FRAME_ROWS_DEFINED\n\n");
            for (i = 0; i < (int)resourceCount; i++)
            {
                const struct FamilyResource *r = &resources[i];
                if (r->kind != KIND_ANIM_FRAME)
                    continue;
                Gen3Buffer_AppendFormat(&framesHeader,
                    "TILESET_ANIM_FRAME(%s, \"%s\", %llu)\n",
                    r->symbol, r->id, (unsigned long long)r->decodedLength);
            }
            Gen3Buffer_AppendCStr(&framesHeader, "\n#define TILESET_FLOOR_LIGHT_ROWS_DEFINED\n\n");
            Gen3Buffer_AppendFormat(&framesHeader,
                "TILESET_FLOOR_LIGHT(");
            for (i = 0; i < (int)floorLightCount; i++)
            {
                Gen3Buffer_AppendFormat(&framesHeader, "%s%s",
                    i > 0 ? ", " : "", floorLightConsumers[i].symbol);
            }
            for (i = 0; i < (int)floorLightCount; i++)
            {
                Gen3Buffer_AppendFormat(&framesHeader, ", \"%s\"",
                    floorLightConsumers[i].resourceId);
            }
            Gen3Buffer_AppendCStr(&framesHeader, ")\n");
            Gen3Buffer_AppendCStr(&framesHeader,
                "\n#endif /* TILESET_FRAMES_GENERATED_H */\n");

            if (check)
            {
                const char *extraPaths[2] = { nativeHeaderOut, framesHeaderOut };
                const struct Gen3Buffer *extraOutputs[2] = { &nativeHeader,
                                                             &framesHeader };
                const char *paths[4] = { catalogOut, bindingsOut,
                                         ownershipOut, consumersOut };
                const struct Gen3Buffer *outputs[4] = { &catalog, &bindings,
                                                        &ownership, &consumerFile };
                size_t k;
                for (k = 0; k < 2; k++)
                {
                    struct Gen3Buffer existing;
                    Gen3Buffer_Init(&existing, 32768);
                    if (!Gen3Util_ReadFile(extraPaths[k], &existing, errbuf,
                                           sizeof(errbuf))
                     || existing.length != extraOutputs[k]->length
                     || memcmp(existing.data, extraOutputs[k]->data,
                               extraOutputs[k]->length) != 0)
                        Fail("--check: %s is not up to date", extraPaths[k]);
                    Gen3Buffer_Destroy(&existing);
                }
                for (k = 0; k < 4; k++)
                {
                    struct Gen3Buffer existing;
                    Gen3Buffer_Init(&existing, 32768);
                    if (!Gen3Util_ReadFile(paths[k], &existing, errbuf,
                                           sizeof(errbuf))
                     || existing.length != outputs[k]->length
                     || memcmp(existing.data, outputs[k]->data,
                               outputs[k]->length) != 0)
                        Fail("--check: %s is not up to date", paths[k]);
                    Gen3Buffer_Destroy(&existing);
                }
                fprintf(stderr,
                        "check passed: %zu resources (%zu tiles, %zu palette "
                        "rows, %zu metatiles, %zu attributes, %zu anim frames, "
                        "%zu floor-light), %zu structs, %zu frame tables, "
                        "%zu consumers, %zu parity\n",
                        resourceCount, kindCounts[KIND_TILE],
                        kindCounts[KIND_PALETTE_ROW],
                        kindCounts[KIND_METATILES],
                        kindCounts[KIND_METATILE_ATTRIBUTES],
                        kindCounts[KIND_ANIM_FRAME],
                        kindCounts[KIND_FLOOR_LIGHT_PAL], structCount,
                        frameTableCount, frameConsumerCount, parityCount);
            }
            else
            {
                const char *extraPaths[2] = { nativeHeaderOut, framesHeaderOut };
                const struct Gen3Buffer *extraOutputs[2] = { &nativeHeader,
                                                             &framesHeader };
                const char *paths[4] = { catalogOut, bindingsOut,
                                         ownershipOut, consumersOut };
                const struct Gen3Buffer *outputs[4] = { &catalog, &bindings,
                                                        &ownership, &consumerFile };
                size_t k;
                for (k = 0; k < 2; k++)
                {
                    if (!Gen3Util_WriteFile(extraPaths[k], extraOutputs[k]->data,
                                            extraOutputs[k]->length, errbuf,
                                            sizeof(errbuf)))
                        Fail("cannot write %s: %s", extraPaths[k], errbuf);
                }
                for (k = 0; k < 4; k++)
                {
                    if (!Gen3Util_WriteFile(paths[k], outputs[k]->data,
                                            outputs[k]->length, errbuf,
                                            sizeof(errbuf)))
                        Fail("cannot write %s: %s", paths[k], errbuf);
                }
                fprintf(stderr,
                        "wrote %zu resources (%zu tiles, %zu palette rows, "
                        "%zu metatiles, %zu attributes, %zu anim frames, "
                        "%zu floor-light), %zu structs, %zu frame tables, "
                        "%zu consumers, %zu parity\n",
                        resourceCount, kindCounts[KIND_TILE],
                        kindCounts[KIND_PALETTE_ROW],
                        kindCounts[KIND_METATILES],
                        kindCounts[KIND_METATILE_ATTRIBUTES],
                        kindCounts[KIND_ANIM_FRAME],
                        kindCounts[KIND_FLOOR_LIGHT_PAL], structCount,
                        frameTableCount, frameConsumerCount, parityCount);
            }

            Gen3Buffer_Destroy(&catalog);
            Gen3Buffer_Destroy(&bindings);
            Gen3Buffer_Destroy(&ownership);
            Gen3Buffer_Destroy(&consumerFile);
            Gen3Buffer_Destroy(&nativeHeader);
            Gen3Buffer_Destroy(&framesHeader);
        }
    }
    return 0;
}

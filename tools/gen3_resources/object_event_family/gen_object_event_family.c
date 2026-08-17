/*
 * gen_object_event_family — R11 object-event graphics family generator.
 *
 * Consumes the machine-readable inventory emitted by
 * gen_object_event_inventory.py
 * (resources/extraction/emerald/bpee01/object_event/inventory.generated.toml)
 * and emits four deterministic, bytewise-sorted metadata files:
 *
 *   catalog.generated.toml              — full family resource catalog (288).
 *   bindings.generated.toml             — semantic extraction bindings (288).
 *   ownership.generated.toml            — native/GBA ownership per resource.
 *   object_event_consumers.generated.toml — per-frame consumer map: every
 *                                     SpriteFrameImage entry publishes
 *                                     frame.data = canonical sheet stream
 *                                     + width*height*frame*32 (whole-sheet
 *                                     frames use offset 0).
 *
 * Canonical-name rule (R11-L): canonical = ASCII-lowercase of the symbol
 * suffix with underscores converted to hyphens:
 *   gObjectEventPic_BrendanNormal  -> "brendan-normal"
 *   gObjectEventPal_Brendan        -> "brendan"
 * IDs:
 *   emerald:object-event/<canonical>/sheet    (type sprite-sheet, raw)
 *   emerald:object-event/<canonical>/palette  (type palette, raw)
 *
 * Sheets are RAW 4bpp payloads (source_encoding "raw"): the artifact IS the
 * canonical decoded bytes. Palettes are RAW 16-u16 BGR555 payloads, exactly
 * 32 bytes. Frame consumers derive their in-sheet offset purely from the
 * source macro geometry (width*height*frame*32) — never from compiled
 * payload addresses.
 *
 * Determinism: output order independent of inventory order; resources
 * bytewise by canonical id, frame arrays bytewise by symbol, consumers
 * bytewise by (array, index). Running twice produces byte-identical files.
 *
 * Fail-closed validation: every check below reports "FAIL: <message>" and
 * returns a nonzero exit code. Duplicate canonical ids, missing sheets,
 * out-of-bounds frames, and non-32-byte palettes are hard failures.
 *
 * Usage:
 *   gen_object_event_family --inventory PATH \
 *       --catalog-out PATH --bindings-out PATH \
 *       --ownership-out PATH --consumers-out PATH
 *   gen_object_event_family --inventory PATH --check \
 *       --catalog-out PATH --bindings-out PATH \
 *       --ownership-out PATH --consumers-out PATH
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_id.h"
#include "gen3/resources/sha256.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"

#define MAX_RESOURCES 512u
#define MAX_FRAME_ARRAYS 512u
#define MAX_CONSUMERS 2048u

struct FamilyResource
{
    char symbol[96];
    char canonical[96];
    char id[128];
    char artifact[256];
    bool isPalette;
    bool reflection;
    uint64_t byteLength; /* artifact size (canonical == encoded for raw) */
    char encodedSha256[GEN3_RESOURCE_KEY_HEX_SIZE];
    char ownershipKey[GEN3_RESOURCE_KEY_HEX_SIZE];
};

struct FrameConsumer
{
    char array[96];
    uint32_t index;
    char sheetId[128];
    uint32_t offset;    /* bytes into the canonical sheet */
    uint32_t frameSize; /* w*h*32 bytes copied by the consumer; 0 = whole sheet */
    bool wholeSheet;
};

struct FrameArrayRecord
{
    char symbol[96];
    uint32_t frameCount;
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

/* gObjectEventPic_X / gObjectEventPal_X -> lowercase suffix, _ -> -. */
static void Canonicalize(const char *symbol, char *out, size_t outSize)
{
    const char *underscore = strrchr(symbol, '_');
    const char *suffix = underscore != NULL ? underscore + 1 : symbol;
    size_t i;

    for (i = 0; suffix[i] != '\0' && i + 1 < outSize; i++)
        out[i] = suffix[i] == '_' ? '-' : (char)((unsigned char)suffix[i] | 0x20u);
    out[i] = '\0';
}

static int CompareFrameArray(const void *left, const void *right)
{
    return strcmp(((const struct FrameArrayRecord *)left)->symbol,
                  ((const struct FrameArrayRecord *)right)->symbol);
}

static int CompareResourceById(const void *left, const void *right)
{
    return strcmp(((const struct FamilyResource *)left)->id,
                  ((const struct FamilyResource *)right)->id);
}

static int CompareConsumer(const void *left, const void *right)
{
    const struct FrameConsumer *l = left;
    const struct FrameConsumer *r = right;
    int cmp = strcmp(l->array, r->array);
    if (cmp != 0)
        return cmp;
    if (l->index < r->index)
        return -1;
    if (l->index > r->index)
        return 1;
    return 0;
}

static const struct FamilyResource *FindSheetBySymbol(
    const struct FamilyResource *resources, size_t resourceCount,
    const char *symbol)
{
    size_t i;
    for (i = 0; i < resourceCount; i++)
    {
        if (!resources[i].isPalette && strcmp(resources[i].symbol, symbol) == 0)
            return &resources[i];
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *inventoryPath = NULL;
    const char *catalogOut = NULL;
    const char *bindingsOut = NULL;
    const char *ownershipOut = NULL;
    const char *consumersOut = NULL;
    const char *nativeArraysOut = NULL;
    const char *framesTableOut = NULL;
    const char *palettesTableOut = NULL;
    const char *sheetsTableOut = NULL;
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
        else if (strcmp(argv[i], "--native-arrays-out") == 0 && i + 1 < argc)
            nativeArraysOut = argv[++i];
        else if (strcmp(argv[i], "--frames-table-out") == 0 && i + 1 < argc)
            framesTableOut = argv[++i];
        else if (strcmp(argv[i], "--palettes-table-out") == 0 && i + 1 < argc)
            palettesTableOut = argv[++i];
        else if (strcmp(argv[i], "--sheets-table-out") == 0 && i + 1 < argc)
            sheetsTableOut = argv[++i];
        else if (strcmp(argv[i], "--check") == 0)
            check = true;
        else
        {
            fprintf(stderr, "usage: gen_object_event_family --inventory PATH "
                    "--catalog-out PATH --bindings-out PATH "
                    "--ownership-out PATH --consumers-out PATH [--check]\n");
            return 2;
        }
    }
    if (inventoryPath == NULL || catalogOut == NULL || bindingsOut == NULL
     || ownershipOut == NULL || consumersOut == NULL
     || nativeArraysOut == NULL || framesTableOut == NULL
     || palettesTableOut == NULL || sheetsTableOut == NULL)
    {
        fprintf(stderr, "gen_object_event_family: missing required argument\n");
        return 2;
    }

    {
        char errbuf[256];
        struct Gen3Buffer text;
        struct Gen3TomlDocument doc;
        struct FamilyResource resources[MAX_RESOURCES];
        struct FrameConsumer consumers[MAX_CONSUMERS];
        struct FrameArrayRecord frameArrays[MAX_FRAME_ARRAYS];
        size_t resourceCount = 0;
        size_t consumerCount = 0;
        size_t frameArrayCount = 0;
        size_t sheetCount = 0;
        size_t paletteCount = 0;
        size_t n;

        Gen3Buffer_Init(&text, 8192);
        if (!Gen3Util_ReadFile(inventoryPath, &text, errbuf, sizeof(errbuf)))
            Fail("cannot read inventory %s: %s", inventoryPath, errbuf);
        if (!Gen3Toml_Parse(text.data, text.length, &doc, errbuf, sizeof(errbuf)))
            Fail("cannot parse inventory %s: %s", inventoryPath, errbuf);

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

            memset(r, 0, sizeof(*r));
            if (!GetStringRequired(item, "symbol", r->symbol, sizeof(r->symbol))
             || !GetStringRequired(item, "kind", kind, sizeof(kind))
             || !GetStringRequired(item, "source_artifact", r->artifact,
                                   sizeof(r->artifact)))
                Fail("resource %zu missing symbol/kind/source_artifact", i);
            if (strcmp(kind, "sheet") == 0)
            {
                r->isPalette = false;
                sheetCount++;
            }
            else if (strcmp(kind, "palette") == 0)
            {
                r->isPalette = true;
                paletteCount++;
                if (!GetBoolRequired(item, "reflection", &r->reflection))
                    r->reflection = false;
            }
            else
                Fail("resource %s has unknown kind '%s'", r->symbol, kind);
            Canonicalize(r->symbol, r->canonical, sizeof(r->canonical));
            if (r->canonical[0] == '\0')
                Fail("resource %s has an empty canonical component", r->symbol);
            {
                char canonicalCopy[96];
                snprintf(canonicalCopy, sizeof(canonicalCopy), "%s", r->canonical);
                snprintf(r->id, sizeof(r->id), "emerald:object-event/%s/%s",
                         canonicalCopy, r->isPalette ? "palette" : "sheet");
            }

            /* Artifact: raw source == canonical decoded bytes. */
            {
                struct Gen3Buffer payload;
                struct Gen3Sha256Context sha;
                uint8_t digest[32];
                Gen3Buffer_Init(&payload, 4096);
                if (!Gen3Util_ReadFile(r->artifact, &payload, errbuf,
                                       sizeof(errbuf)))
                    Fail("resource %s: cannot read artifact %s: %s", r->symbol,
                         r->artifact, errbuf);
                r->byteLength = payload.length;
                if (r->isPalette && payload.length != 32)
                    Fail("palette %s has invalid size %zu (must be 32)",
                         r->symbol, payload.length);
                if (payload.length == 0)
                    Fail("resource %s has an empty artifact", r->symbol);
                Gen3Sha256_Init(&sha);
                Gen3Sha256_Update(&sha, payload.data, payload.length);
                Gen3Sha256_Final(&sha, digest);
                Gen3Util_FormatHex(digest, sizeof(digest),
                                   r->encodedSha256);
                Gen3Buffer_Destroy(&payload);
            }
            {
                Gen3ResourceKey key;
                Gen3ResourceId_DeriveKey(r->id, &key);
                Gen3ResourceId_FormatKeyHex(&key, r->ownershipKey);
            }
            resourceCount++;
        }

        /* Frame arrays. */
        n = Gen3Toml_GetArrayCount(&doc.root, "frame_arrays");
        if (n == 0 || n > MAX_FRAME_ARRAYS)
            Fail("inventory frame_array count %zu out of range", n);
        for (i = 0; i < (int)n; i++)
        {
            const struct Gen3TomlMap *item =
                Gen3Toml_GetArrayItem(&doc.root, "frame_arrays", (size_t)i);
            long long count;
            if (!GetStringRequired(item, "symbol", frameArrays[frameArrayCount].symbol,
                                   sizeof(frameArrays[0].symbol))
             || !GetIntegerRequired(item, "frame_count", &count))
                Fail("frame_array %zu missing symbol/frame_count", i);
            if (count <= 0)
                Fail("frame array %s has non-positive frame_count %lld",
                     frameArrays[frameArrayCount].symbol, count);
            frameArrays[frameArrayCount].frameCount = (uint32_t)count;
            frameArrayCount++;
        }

        /* Consumers + validation against artifact sizes. */
        n = Gen3Toml_GetArrayCount(&doc.root, "consumers");
        if (n == 0 || n > MAX_CONSUMERS)
            Fail("inventory consumer count %zu out of range", n);
        for (i = 0; i < (int)n; i++)
        {
            const struct Gen3TomlMap *item =
                Gen3Toml_GetArrayItem(&doc.root, "consumers", (size_t)i);
            struct FrameConsumer *c = &consumers[consumerCount];
            char sheetSymbol[96];
            long long width;
            long long height;
            long long frame;
            const struct FamilyResource *sheet;
            uint64_t offset;
            uint64_t frameBytes;

            memset(c, 0, sizeof(*c));
            if (!GetStringRequired(item, "array", c->array, sizeof(c->array))
             || !GetStringRequired(item, "sheet", sheetSymbol, sizeof(sheetSymbol)))
                Fail("consumer %zu missing array/sheet", i);
            {
                long long indexValue;
                if (!GetIntegerRequired(item, "index", &indexValue)
                 || indexValue < 0 || indexValue > 0xFFFFFFFFLL)
                    Fail("consumer %s missing/invalid index", c->array);
                c->index = (uint32_t)indexValue;
            }
            if (!GetBoolRequired(item, "whole_sheet", &c->wholeSheet))
                c->wholeSheet = false;
            if (!c->wholeSheet)
            {
                if (!GetIntegerRequired(item, "width", &width)
                 || !GetIntegerRequired(item, "height", &height)
                 || !GetIntegerRequired(item, "frame", &frame))
                    Fail("consumer %s[%u] missing width/height/frame", c->array,
                         c->index);
                if (width <= 0 || height <= 0 || frame < 0
                 || width > 1024 || height > 1024 || frame > 1024)
                    Fail("consumer %s[%u] has invalid frame geometry %lldx%lld#%lld",
                         c->array, c->index, width, height, frame);
                offset = (uint64_t)(width * height * frame) * 32u;
                frameBytes = (uint64_t)(width * height) * 32u;
            }
            else
            {
                offset = 0;
                frameBytes = 0; /* the whole sheet */
            }
            if (frameBytes > 0xFFFFu && !c->wholeSheet)
                Fail("consumer %s[%u] frame size %llu exceeds the u16 size field",
                     c->array, c->index, (unsigned long long)frameBytes);
            sheet = FindSheetBySymbol(resources, resourceCount, sheetSymbol);
            if (sheet == NULL)
                Fail("consumer %s[%u] references unknown sheet %s", c->array,
                     c->index, sheetSymbol);
            if (c->wholeSheet)
                frameBytes = sheet->byteLength;
            if (offset > sheet->byteLength
             || frameBytes > sheet->byteLength
             || offset > sheet->byteLength - frameBytes)
                Fail("consumer %s[%u] frame range [%llu, +%llu) exceeds sheet %s (%llu bytes)",
                     c->array, c->index, (unsigned long long)offset,
                     (unsigned long long)frameBytes, sheetSymbol,
                     (unsigned long long)sheet->byteLength);
            snprintf(c->sheetId, sizeof(c->sheetId), "%s", sheet->id);
            c->offset = (uint32_t)offset;
            c->frameSize = c->wholeSheet ? 0u : (uint32_t)frameBytes;
            consumerCount++;
        }
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&text);

        /* Duplicate canonical ids per kind. */
        for (i = 0; i < (int)resourceCount; i++)
        {
            size_t j;
            for (j = (size_t)i + 1; j < resourceCount; j++)
            {
                if (resources[i].isPalette != resources[j].isPalette)
                    continue;
                if (strcmp(resources[i].id, resources[j].id) == 0)
                    Fail("duplicate canonical id %s (%s and %s)", resources[i].id,
                         resources[i].symbol, resources[j].symbol);
            }
        }

        /* Deterministic order. */
        qsort(resources, resourceCount, sizeof(resources[0]), CompareResourceById);
        qsort(frameArrays, frameArrayCount, sizeof(frameArrays[0]), CompareFrameArray);
        qsort(consumers, consumerCount, sizeof(consumers[0]), CompareConsumer);

        /* Emit. */
        {
            struct Gen3Buffer catalog;
            struct Gen3Buffer bindings;
            struct Gen3Buffer ownership;
            struct Gen3Buffer consumerFile;
            static const char header[] =
                "# Generated by tools/gen3_resources/object_event_family/gen_object_event_family.\n"
                "# Do not edit by hand; edit the object-event inventory and re-run the generator.\n\n";

            Gen3Buffer_Init(&catalog, 16384);
            Gen3Buffer_Init(&bindings, 16384);
            Gen3Buffer_Init(&ownership, 16384);
            Gen3Buffer_Init(&consumerFile, 16384);
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
                Gen3Buffer_AppendFormat(&catalog,
                    "[[resources]]\nid = \"%s\"\ntype = \"%s\"\nschema = 1\n"
                    "required_for_base = true\n",
                    r->id, r->isPalette ? "palette" : "sprite-sheet");
            }

            Gen3Buffer_AppendCStr(&bindings, "bindings_version = 1\n"
                                  "game = \"emerald\"\n"
                                  "rom_profile = \"bpee01-rev0\"\n\n");
            for (i = 0; i < (int)resourceCount; i++)
            {
                const struct FamilyResource *r = &resources[i];
                Gen3Buffer_AppendFormat(&bindings,
                    "[[bindings]]\nid = \"%s\"\nsymbol = \"%s\"\n"
                    "source_artifact = \"%s\"\nsource_encoding = \"raw\"\n"
                    "canonical_representation = \"%s\"\n"
                    "expected_decoded_size = %llu\n",
                    r->id, r->symbol, r->artifact,
                    r->isPalette ? "gba-bgr555-palette" : "gba-4bpp-tiles",
                    (unsigned long long)r->byteLength);
            }

            Gen3Buffer_AppendCStr(&ownership, "ownership_version = 1\n"
                                  "game = \"emerald\"\n"
                                  "rom_profile = \"bpee01-rev0\"\n\n");
            for (i = 0; i < (int)resourceCount; i++)
            {
                const struct FamilyResource *r = &resources[i];
                Gen3Buffer_AppendFormat(&ownership,
                    "[[resources]]\nid = \"%s\"\nkey = \"%s\"\n"
                    "legacy_symbol = \"%s\"\ntype = \"%s\"\nschema = 1\n"
                    "source_artifact = \"%s\"\n"
                    "encoded_length = %llu\ndecoded_length = %llu\n"
                    "source_encoding = \"raw\"\n"
                    "source_encoded_sha256 = \"%s\"\n"
                    "canonical_decoded_sha256 = \"%s\"\n"
                    "ownership_state = \"ROM_BASE_ONLY\"\n"
                    "reflection = %s\n\n"
                    "[resources.targets]\nnative = \"ROM_BASE_ONLY\"\n"
                    "gba = \"COMPILED\"\n",
                    r->id, r->ownershipKey, r->symbol,
                    r->isPalette ? "palette" : "sprite-sheet",
                    r->artifact,
                    (unsigned long long)r->byteLength,
                    (unsigned long long)r->byteLength,
                    r->encodedSha256, r->encodedSha256,
                    r->reflection ? "true" : "false");
            }

            Gen3Buffer_AppendCStr(&consumerFile, "consumers_version = 1\n"
                                  "game = \"emerald\"\n"
                                  "rom_profile = \"bpee01-rev0\"\n\n");
            for (i = 0; i < (int)frameArrayCount; i++)
            {
                Gen3Buffer_AppendFormat(&consumerFile,
                    "[[frame_arrays]]\nsymbol = \"%s\"\nframe_count = %u\n",
                    frameArrays[i].symbol, frameArrays[i].frameCount);
            }
            for (i = 0; i < (int)consumerCount; i++)
            {
                const struct FrameConsumer *c = &consumers[i];
                Gen3Buffer_AppendFormat(&consumerFile,
                    "[[consumers]]\narray = \"%s\"\nindex = %u\n"
                    "sheet_id = \"%s\"\noffset = %u\nwhole_sheet = %s\n",
                    c->array, c->index, c->sheetId, c->offset,
                    c->wholeSheet ? "true" : "false");
            }

            /* Native-only generated outputs: the frame arrays with NULL
             * sentinels (geometry identical to the compiled GBA tables), the
             * seam publication table, and the palette publication table. */
            {
                struct Gen3Buffer nativeArrays;
                struct Gen3Buffer framesTable;
                struct Gen3Buffer palettesTable;
                struct Gen3Buffer sheetsTable;
                static const char seamHeader[] =
                    "// Generated by tools/gen3_resources/object_event_family/gen_object_event_family.\n"
                    "// Do not edit by hand; edit the object-event inventory and re-run the generator.\n"
                    "// Native-only: the payload leaves are ROM_BASE; the compiled arrays carry\n"
                    "// NULL sentinels and the compat seam publishes session streams.\n\n";
                Gen3Buffer_Init(&nativeArrays, 32768);
                Gen3Buffer_Init(&framesTable, 32768);
                Gen3Buffer_Init(&palettesTable, 8192);
                Gen3Buffer_Init(&sheetsTable, 16384);
                Gen3Buffer_AppendCStr(&nativeArrays, seamHeader);
                Gen3Buffer_AppendCStr(&nativeArrays,
                    "/* Include-once toggle: define OBJECT_EVENT_NATIVE_DEFINE in exactly one TU\n"
                    " * (the compat seam) to emit the definitions; every other includer gets\n"
                    " * the declarations. The initializer is a variadic argument so the\n"
                    " * commas inside the frame-array braced lists are not macro\n"
                    " * separators. */\n"
                    "#if defined(OBJECT_EVENT_NATIVE_DEFINE)\n"
                    "#define OE_NATIVE_ARRAY(decl, ...) decl = __VA_ARGS__\n"
                    "#else\n"
                    "#define OE_NATIVE_ARRAY(decl, ...) extern decl\n"
                    "#endif\n\n");
                Gen3Buffer_AppendCStr(&nativeArrays,
                    "/* Migrated payload leaves: session-published arrays. */\n");
                for (i = 0; i < (int)resourceCount; i++)
                {
                    const struct FamilyResource *r = &resources[i];
                    if (r->isPalette)
                        Gen3Buffer_AppendFormat(&nativeArrays,
                            "OE_NATIVE_ARRAY(u16 %s[16], {0});\n", r->symbol);
                    else
                        Gen3Buffer_AppendFormat(&nativeArrays,
                            "OE_NATIVE_ARRAY(u32 %s[%llu / 4], {0});\n",
                            r->symbol, (unsigned long long)r->byteLength);
                }
                Gen3Buffer_AppendCStr(&nativeArrays, "\n");
                Gen3Buffer_AppendCStr(&framesTable, seamHeader);
                Gen3Buffer_AppendCStr(&palettesTable, seamHeader);
                Gen3Buffer_AppendCStr(&sheetsTable, seamHeader);
                for (i = 0; i < (int)frameArrayCount; i++)
                {
                    size_t c;
                    size_t done = 0;
                    Gen3Buffer_AppendFormat(&nativeArrays,
                        "OE_NATIVE_ARRAY(struct SpriteFrameImage %s[],\n"
                        "{\n", frameArrays[i].symbol);
                    for (c = 0; c < consumerCount; c++)
                    {
                        const struct FrameConsumer *con = &consumers[c];
                        if (strcmp(con->array, frameArrays[i].symbol) != 0)
                            continue;
                        if (con->wholeSheet)
                            Gen3Buffer_AppendFormat(&nativeArrays,
                                "    { .data = NULL, .size = 0 },\n");
                        else
                            Gen3Buffer_AppendFormat(&nativeArrays,
                                "    { .data = NULL, .size = %u },\n",
                                con->frameSize);
                        done++;
                    }
                    if (done != frameArrays[i].frameCount)
                        Fail("frame array %s emission mismatch (%zu != %u)",
                             frameArrays[i].symbol, done, frameArrays[i].frameCount);
                    Gen3Buffer_AppendCStr(&nativeArrays, "});\n\n");
                }
                Gen3Buffer_AppendCStr(&framesTable, "#define OBJECT_EVENT_FRAME_ROWS_DEFINED\n\n");
                for (i = 0; i < (int)consumerCount; i++)
                {
                    const struct FrameConsumer *con = &consumers[i];
                    Gen3Buffer_AppendFormat(&framesTable,
                        "OBJECT_EVENT_FRAME(%s, %u, \"%s\", %u, %u, %s);\n",
                        con->array, con->index, con->sheetId, con->offset,
                        con->frameSize, con->wholeSheet ? "TRUE" : "FALSE");
                }
                Gen3Buffer_AppendCStr(&sheetsTable, "#define OBJECT_EVENT_SHEET_ROWS_DEFINED\n\n");
                for (i = 0; i < (int)resourceCount; i++)
                {
                    const struct FamilyResource *r = &resources[i];
                    if (r->isPalette)
                        continue;
                    Gen3Buffer_AppendFormat(&sheetsTable,
                        "OBJECT_EVENT_SHEET(%s, \"%s\", %llu)\n",
                        r->symbol, r->id, (unsigned long long)r->byteLength);
                }
                Gen3Buffer_AppendCStr(&palettesTable, "#define OBJECT_EVENT_PALETTE_ROWS_DEFINED\n\n");
                for (i = 0; i < (int)resourceCount; i++)
                {
                    const struct FamilyResource *r = &resources[i];
                    if (!r->isPalette)
                        continue;
                    Gen3Buffer_AppendFormat(&palettesTable,
                        "OBJECT_EVENT_PALETTE(%s, \"%s\", %s)\n",
                        r->symbol, r->id, r->reflection ? "TRUE" : "FALSE");
                }

                if (check)
                {
                    const char *extraPaths[4] = { nativeArraysOut, framesTableOut,
                                                  palettesTableOut, sheetsTableOut };
                    const struct Gen3Buffer *extraOutputs[4] = { &nativeArrays,
                                                                 &framesTable,
                                                                 &palettesTable,
                                                                 &sheetsTable };
                    size_t k;
                    for (k = 0; k < 4; k++)
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
                }
                else
                {
                    const char *extraPaths[4] = { nativeArraysOut, framesTableOut,
                                                  palettesTableOut, sheetsTableOut };
                    const struct Gen3Buffer *extraOutputs[4] = { &nativeArrays,
                                                                 &framesTable,
                                                                 &palettesTable,
                                                                 &sheetsTable };
                    size_t k;
                    for (k = 0; k < 4; k++)
                    {
                        if (!Gen3Util_WriteFile(extraPaths[k], extraOutputs[k]->data,
                                                extraOutputs[k]->length, errbuf,
                                                sizeof(errbuf)))
                            Fail("cannot write %s: %s", extraPaths[k], errbuf);
                    }
                }
                Gen3Buffer_Destroy(&nativeArrays);
                Gen3Buffer_Destroy(&framesTable);
                Gen3Buffer_Destroy(&palettesTable);
                Gen3Buffer_Destroy(&sheetsTable);
            }

            if (check)
            {
                const char *paths[4] = { catalogOut, bindingsOut,
                                         ownershipOut, consumersOut };
                const struct Gen3Buffer *outputs[4] = { &catalog, &bindings,
                                                        &ownership, &consumerFile };
                size_t k;
                for (k = 0; k < 4; k++)
                {
                    struct Gen3Buffer existing;
                    Gen3Buffer_Init(&existing, 16384);
                    if (!Gen3Util_ReadFile(paths[k], &existing, errbuf,
                                           sizeof(errbuf))
                     || existing.length != outputs[k]->length
                     || memcmp(existing.data, outputs[k]->data,
                               outputs[k]->length) != 0)
                    {
                        Fail("--check: %s is not up to date", paths[k]);
                    }
                    Gen3Buffer_Destroy(&existing);
                }
                fprintf(stderr,
                        "check passed: %zu resources (%zu sheets, %zu palettes), "
                        "%zu frame arrays, %zu consumers\n",
                        resourceCount, sheetCount, paletteCount,
                        frameArrayCount, consumerCount);
            }
            else
            {
                const char *paths[4] = { catalogOut, bindingsOut,
                                         ownershipOut, consumersOut };
                const struct Gen3Buffer *outputs[4] = { &catalog, &bindings,
                                                        &ownership, &consumerFile };
                size_t k;
                for (k = 0; k < 4; k++)
                {
                    if (!Gen3Util_WriteFile(paths[k], outputs[k]->data,
                                            outputs[k]->length, errbuf,
                                            sizeof(errbuf)))
                        Fail("cannot write %s: %s", paths[k], errbuf);
                }
                fprintf(stderr,
                        "wrote %zu resources (%zu sheets, %zu palettes), "
                        "%zu frame arrays, %zu consumers\n",
                        resourceCount, sheetCount, paletteCount,
                        frameArrayCount, consumerCount);
            }

            Gen3Buffer_Destroy(&catalog);
            Gen3Buffer_Destroy(&bindings);
            Gen3Buffer_Destroy(&ownership);
            Gen3Buffer_Destroy(&consumerFile);
        }
    }
    return 0;
}

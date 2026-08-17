/* gen-layout-family - R11-D map layout family generator.
 *
 * Consumes the layout inventory (resources/extraction/emerald/bpee01/layout/
 * inventory.generated.toml) plus data/layouts/layouts.json facts carried in
 * it, and emits:
 *
 *   catalog.generated.toml   emerald:layout/<canonical>/blockdata|border
 *                            (type tilemap, schema 1, required_for_base)
 *   bindings.generated.toml  canonical_representation gba-tilemap, raw
 *   ownership.generated.toml native ROM_BASE_ONLY / gba COMPILED
 *   layout_consumers.generated.toml  441 layouts + 882 consumers
 *   layout_native.generated.h   writable struct MapLayout records (seam TU
 *                            defines via LAYOUT_NATIVE_DEFINE; all other
 *                            includers extern)
 *   layout_frames.generated.h  LAYOUT_RECORD publication rows
 *
 * Deterministic: all rows bytewise sorted by canonical id / symbol.
 * Regeneration must be a no-op diff; --check enforces that.
 *
 * Build isolation (R7A §24 / R11-C precedent): only -std=c99 -Wall -Wextra
 * -Werror plus the two include roots for the shared Gen3 core. No Emerald
 * defines, no global.h/gba/SDL. The tool links the exact M0/M1 key
 * derivation so the ownership keys are recomputed with the same algorithm.
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

#define MAX_LAYOUTS 512u
#define MAX_RESOURCES 1024u
#define MAX_CONSUMERS 1024u

enum ResourceKind
{
    KIND_BLOCKDATA = 0,
    KIND_BORDER = 1
};

struct LayoutRecord
{
    char name[96];
    char id[64];
    uint32_t width;
    uint32_t height;
    char primaryTileset[96];
    char secondaryTileset[96];
};

struct FamilyResource
{
    enum ResourceKind kind;
    char symbol[128];
    char canonical[96];
    char id[192];
    char artifact[256];
    char encodedSha256[GEN3_RESOURCE_KEY_HEX_SIZE];
    char canonicalSha256[GEN3_RESOURCE_KEY_HEX_SIZE];
    char ownershipKey[GEN3_RESOURCE_KEY_HEX_SIZE];
    size_t byteLength;
    size_t decodedLength;
};

struct LayoutConsumer
{
    char layout[96];
    char kind[24];
    char resourceId[192];
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

/* R11-B canonical rule: lowercase, '_' -> '-'. The layout slug drops the
 * trailing "_Layout" (the inventory pre-computes it; cross-check below). */
static void CanonicalizeSlug(const char *layoutName, char *out, size_t outSize)
{
    size_t nameLength = strlen(layoutName);
    const char *suffix;
    size_t i;

    if (nameLength <= strlen("_Layout")
     || strcmp(layoutName + nameLength - strlen("_Layout"), "_Layout") != 0)
        Fail("%s does not end with _Layout", layoutName);
    suffix = layoutName;
    for (i = 0; suffix[i] != '\0' && i + 1 < outSize; i++)
    {
        char c = suffix[i];
        if (c == '_')
        {
            /* drop the trailing "_Layout" marker */
            if (strcmp(suffix + i, "_Layout") == 0)
                break;
            c = '-';
        }
        out[i] = (char)((unsigned char)c | 0x20u);
    }
    out[i] = '\0';
}

/* The inventory carries a `canonical` field for cross-checking the
 * recomputed slug (stale inventory cannot silently rename a resource). */
static void CheckInventoryCanonical(const struct Gen3TomlMap *item,
                                    const char *layoutName, const char *kind,
                                    const char *computed)
{
    const char *expected;
    if (Gen3Toml_GetString(item, "canonical", &expected) && expected != NULL
     && strcmp(expected, computed) != 0)
        Fail("%s %s canonical mismatch: inventory says '%s', recomputed '%s'",
             layoutName, kind, expected, computed);
}

static int CompareResourceById(const void *left, const void *right)
{
    return strcmp(((const struct FamilyResource *)left)->id,
                  ((const struct FamilyResource *)right)->id);
}

static int CompareLayoutBySymbol(const void *left, const void *right)
{
    return strcmp(((const struct LayoutRecord *)left)->name,
                  ((const struct LayoutRecord *)right)->name);
}

static int CompareConsumer(const void *left, const void *right)
{
    const struct LayoutConsumer *l = left;
    const struct LayoutConsumer *r = right;
    int cmp = strcmp(l->layout, r->layout);
    if (cmp != 0)
        return cmp;
    cmp = strcmp(l->kind, r->kind);
    if (cmp != 0)
        return cmp;
    return strcmp(l->resourceId, r->resourceId);
}

static void LoadArtifact(const char *id, const char *artifact, char *errbuf,
                         size_t errbufSize, struct Gen3Buffer *payload)
{
    if (!Gen3Util_ReadFile(artifact, payload, errbuf, errbufSize))
        Fail("resource %s: cannot read artifact %s: %s", id, artifact, errbuf);
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
            fprintf(stderr, "usage: gen_layout_family --inventory PATH "
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
        fprintf(stderr, "gen_layout_family: missing required argument\n");
        return 2;
    }

    {
        char errbuf[256];
        struct Gen3Buffer text;
        struct Gen3TomlDocument doc;
        struct LayoutRecord layouts[MAX_LAYOUTS];
        struct FamilyResource resources[MAX_RESOURCES];
        struct LayoutConsumer consumers[MAX_CONSUMERS];
        size_t layoutCount = 0;
        size_t resourceCount = 0;
        size_t consumerCount = 0;
        size_t kindCounts[2] = { 0, 0 };
        size_t n;

        Gen3Buffer_Init(&text, 8192);
        if (!Gen3Util_ReadFile(inventoryPath, &text, errbuf, sizeof(errbuf)))
            Fail("cannot read inventory %s: %s", inventoryPath, errbuf);
        if (!Gen3Toml_Parse(text.data, text.length, &doc, errbuf, sizeof(errbuf)))
            Fail("cannot parse inventory %s: %s", inventoryPath, errbuf);

        /* Layouts. */
        n = Gen3Toml_GetArrayCount(&doc.root, "layouts");
        if (n == 0 || n > MAX_LAYOUTS)
            Fail("inventory layout count %zu out of range", n);
        for (i = 0; i < (int)n; i++)
        {
            const struct Gen3TomlMap *item =
                Gen3Toml_GetArrayItem(&doc.root, "layouts", (size_t)i);
            struct LayoutRecord *l = &layouts[layoutCount];
            long long width;
            long long height;

            memset(l, 0, sizeof(*l));
            if (!GetStringRequired(item, "name", l->name, sizeof(l->name))
             || !GetStringRequired(item, "id", l->id, sizeof(l->id))
             || !GetIntegerRequired(item, "width", &width)
             || !GetIntegerRequired(item, "height", &height)
             || !GetStringRequired(item, "primary_tileset", l->primaryTileset,
                                   sizeof(l->primaryTileset))
             || !GetStringRequired(item, "secondary_tileset", l->secondaryTileset,
                                   sizeof(l->secondaryTileset)))
                Fail("layout %zu missing required fields", i);
            if (width <= 0 || width > 140 || height <= 0 || height > 140)
                Fail("layout %s has out-of-range dimensions %lldx%lld", l->name,
                     width, height);
            l->width = (uint32_t)width;
            l->height = (uint32_t)height;
            layoutCount++;
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
            char layoutName[96];
            char kind[24];
            char slug[96];
            size_t j;
            const struct LayoutRecord *layout = NULL;

            memset(r, 0, sizeof(*r));
            if (!GetStringRequired(item, "layout", layoutName,
                                   sizeof(layoutName))
             || !GetStringRequired(item, "kind", kind, sizeof(kind))
             || !GetStringRequired(item, "source_artifact", r->artifact,
                                   sizeof(r->artifact)))
                Fail("resource %zu missing layout/kind/source_artifact", i);
            for (j = 0; j < layoutCount; j++)
            {
                if (strcmp(layouts[j].name, layoutName) == 0)
                {
                    layout = &layouts[j];
                    break;
                }
            }
            if (layout == NULL)
                Fail("resource %zu references unknown layout %s", i, layoutName);
            CanonicalizeSlug(layoutName, slug, sizeof(slug));
            CheckInventoryCanonical(item, layoutName, kind, slug);
            snprintf(r->symbol, sizeof(r->symbol), "%s%s", layoutName,
                     strcmp(kind, "blockdata") == 0 ? "_Blockdata" : "_Border");
            snprintf(r->canonical, sizeof(r->canonical), "%s", slug);
            if (strcmp(kind, "blockdata") == 0)
            {
                r->kind = KIND_BLOCKDATA;
                snprintf(r->id, sizeof(r->id), "emerald:layout/%s/blockdata",
                         slug);
            }
            else if (strcmp(kind, "border") == 0)
            {
                r->kind = KIND_BORDER;
                snprintf(r->id, sizeof(r->id), "emerald:layout/%s/border", slug);
            }
            else
                Fail("resource %s has unknown kind '%s'", r->symbol, kind);
            kindCounts[r->kind]++;
            resourceCount++;
        }

        /* Exactly two leaves per layout (blockdata + border). */
        for (i = 0; i < (int)layoutCount; i++)
        {
            size_t blockdata = 0;
            size_t border = 0;
            size_t j;
            for (j = 0; j < resourceCount; j++)
            {
                if (resources[j].kind == KIND_BLOCKDATA)
                    blockdata++;
                else if (resources[j].kind == KIND_BORDER)
                    border++;
            }
            if (blockdata != layoutCount || border != layoutCount)
                Fail("layout leaf counts mismatch: %zu blockdata, %zu border "
                     "for %zu layouts", blockdata, border, layoutCount);
        }

        /* Artifact validation + digests. */
        for (i = 0; i < (int)resourceCount; i++)
        {
            struct FamilyResource *r = &resources[i];
            struct Gen3Buffer payload;
            Gen3ResourceKey key;

            Gen3Buffer_Init(&payload, 8192);
            LoadArtifact(r->id, r->artifact, errbuf, sizeof(errbuf), &payload);
            r->byteLength = payload.length;
            if (payload.length == 0)
                Fail("resource %s has an empty artifact", r->id);
            switch (r->kind)
            {
            case KIND_BLOCKDATA:
                if ((payload.length & 1u) != 0)
                    Fail("blockdata %s has odd size %zu", r->id,
                         payload.length);
                break;
            case KIND_BORDER:
                if (payload.length != 8)
                    Fail("border %s has %zu bytes (expected 8)", r->id,
                         payload.length);
                break;
            }
            r->decodedLength = payload.length;
            Sha256Hex(payload.data, payload.length, r->encodedSha256);
            memcpy(r->canonicalSha256, r->encodedSha256,
                   GEN3_RESOURCE_KEY_HEX_SIZE);
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

        /* Consumers: two per layout. A leaf symbol is
         * `<layoutName>_Blockdata` / `<layoutName>_Border`, so a prefix
         * match on the layout name selects exactly the two leaves. */
        for (i = 0; i < (int)layoutCount; i++)
        {
            size_t j;
            for (j = 0; j < resourceCount; j++)
            {
                if (strncmp(resources[j].symbol, layouts[i].name,
                            strlen(layouts[i].name)) != 0)
                    continue;
                {
                    struct LayoutConsumer *c;
                    if (consumerCount >= MAX_CONSUMERS)
                        Fail("too many layout consumers");
                    c = &consumers[consumerCount];
                    snprintf(c->layout, sizeof(c->layout), "%s",
                             layouts[i].name);
                    snprintf(c->kind, sizeof(c->kind), "%s",
                             resources[j].kind == KIND_BLOCKDATA
                                 ? "blockdata" : "border");
                    snprintf(c->resourceId, sizeof(c->resourceId), "%s",
                             resources[j].id);
                    consumerCount++;
                }
            }
        }
        if (consumerCount != layoutCount * 2u)
            Fail("consumer count %zu != 2 x layout count %zu", consumerCount,
                 layoutCount);

        /* Deterministic order. */
        qsort(resources, resourceCount, sizeof(resources[0]),
              CompareResourceById);
        qsort(layouts, layoutCount, sizeof(layouts[0]),
              CompareLayoutBySymbol);
        qsort(consumers, consumerCount, sizeof(consumers[0]), CompareConsumer);

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
                "# Generated by tools/gen3_resources/layout_family/gen_layout_family.\n"
                "# Do not edit by hand; edit the layout inventory and re-run the generator.\n\n";
            static const char seamHeader[] =
                "// Generated by tools/gen3_resources/layout_family/gen_layout_family.\n"
                "// Do not edit by hand; edit the layout inventory and re-run the generator.\n"
                "// Native-only: the blockdata/border leaves are ROM_BASE; the compiled\n"
                "// records carry NULL map/border pointers and the compat seam publishes\n"
                "// session streams.\n\n";

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
                Gen3Buffer_AppendFormat(&catalog,
                    "[[resources]]\nid = \"%s\"\ntype = \"tilemap\"\n"
                    "schema = 1\nrequired_for_base = true\n",
                    r->id);
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
                    "canonical_representation = \"gba-tilemap\"\n"
                    "expected_decoded_size = %llu\n",
                    r->id, r->symbol, r->artifact,
                    (unsigned long long)r->decodedLength);
            }

            Gen3Buffer_AppendCStr(&ownership, "ownership_version = 1\n"
                                  "game = \"emerald\"\n"
                                  "rom_profile = \"bpee01-rev0\"\n\n");
            for (i = 0; i < (int)resourceCount; i++)
            {
                const struct FamilyResource *r = &resources[i];
                Gen3Buffer_AppendFormat(&ownership,
                    "[[resources]]\nid = \"%s\"\nkey = \"%s\"\n"
                    "legacy_symbol = \"%s\"\ntype = \"tilemap\"\n"
                    "source_artifact = \"%s\"\n"
                    "encoded_length = %llu\ndecoded_length = %llu\n"
                    "source_encoding = \"raw\"\n"
                    "source_encoded_sha256 = \"%s\"\n"
                    "canonical_decoded_sha256 = \"%s\"\n"
                    "ownership_state = \"ROM_BASE_ONLY\"\n\n"
                    "[resources.targets]\nnative = \"ROM_BASE_ONLY\"\n"
                    "gba = \"COMPILED\"\n",
                    r->id, r->ownershipKey, r->symbol, r->artifact,
                    (unsigned long long)r->byteLength,
                    (unsigned long long)r->decodedLength,
                    r->encodedSha256, r->canonicalSha256);
            }

            Gen3Buffer_AppendCStr(&consumerFile, "consumers_version = 1\n"
                                  "game = \"emerald\"\n"
                                  "rom_profile = \"bpee01-rev0\"\n\n");
            for (i = 0; i < (int)layoutCount; i++)
            {
                Gen3Buffer_AppendFormat(&consumerFile,
                    "[[layouts]]\nname = \"%s\"\nid = \"%s\"\n"
                    "width = %u\nheight = %u\n"
                    "primary_tileset = \"%s\"\nsecondary_tileset = \"%s\"\n",
                    layouts[i].name, layouts[i].id, layouts[i].width,
                    layouts[i].height, layouts[i].primaryTileset,
                    layouts[i].secondaryTileset);
            }
            for (i = 0; i < (int)consumerCount; i++)
            {
                Gen3Buffer_AppendFormat(&consumerFile,
                    "[[layout_consumers]]\nlayout = \"%s\"\nkind = \"%s\"\n"
                    "resource = \"%s\"\n",
                    consumers[i].layout, consumers[i].kind,
                    consumers[i].resourceId);
            }

            /* Native header: include-once toggle; definitions in the seam TU,
             * extern declarations everywhere else. The whole file is guarded
             * so data headers and the seam can both include it. The tileset
             * symbols referenced in the initializers are declared by
             * tileset_native.generated.h (extern mode) in the same TU. */
            Gen3Buffer_AppendCStr(&nativeHeader, seamHeader);
            Gen3Buffer_AppendCStr(&nativeHeader,
                "#ifndef LAYOUT_NATIVE_GENERATED_H\n"
                "#define LAYOUT_NATIVE_GENERATED_H\n\n"
                "/* Include-once toggle: define LAYOUT_NATIVE_DEFINE in exactly one TU\n"
                " * (the compat seam) to emit the definitions; every other includer gets\n"
                " * the declarations. The initializer is a variadic argument so the\n"
                " * commas inside the braced lists are not macro separators. */\n"
                "#if defined(LAYOUT_NATIVE_DEFINE)\n"
                "#define LAYOUT_NATIVE_STRUCT(decl, ...) decl = __VA_ARGS__\n"
                "#else\n"
                "#define LAYOUT_NATIVE_STRUCT(decl, ...) extern decl\n"
                "#endif\n\n"
                "/* The 441 struct MapLayout objects: .border/.map NULL-sentineled,\n"
                " * dimensions and tileset references kept compiled. */\n");
            for (i = 0; i < (int)layoutCount; i++)
            {
                /* layouts.json carries the no-tileset case as the string
                 * "0" (UnusedOutdoorArea_Layout's secondary tileset); the
                 * GBA-side mapjson emission writes a bare 0 for it, so the
                 * native record must initialize the pointer to NULL. */
                char primaryRef[128];
                char secondaryRef[128];
                snprintf(primaryRef, sizeof(primaryRef),
                         strcmp(layouts[i].primaryTileset, "0") == 0
                             ? "NULL" : "&%s", layouts[i].primaryTileset);
                snprintf(secondaryRef, sizeof(secondaryRef),
                         strcmp(layouts[i].secondaryTileset, "0") == 0
                             ? "NULL" : "&%s", layouts[i].secondaryTileset);
                Gen3Buffer_AppendFormat(&nativeHeader,
                    "LAYOUT_NATIVE_STRUCT(struct MapLayout %s, {\n"
                    "    .width = %u,\n"
                    "    .height = %u,\n"
                    "    .border = NULL,\n"
                    "    .map = NULL,\n"
                    "    .primaryTileset = %s,\n"
                    "    .secondaryTileset = %s\n"
                    "});\n",
                    layouts[i].name, layouts[i].width, layouts[i].height,
                    primaryRef, secondaryRef);
            }
            Gen3Buffer_AppendCStr(&nativeHeader,
                "\n#endif /* LAYOUT_NATIVE_GENERATED_H */\n");

            /* Publication rows. */
            Gen3Buffer_AppendCStr(&framesHeader, seamHeader);
            Gen3Buffer_AppendCStr(&framesHeader,
                "#ifndef LAYOUT_FRAMES_GENERATED_H\n"
                "#define LAYOUT_FRAMES_GENERATED_H\n\n"
                "#define LAYOUT_RECORD_ROWS_DEFINED\n\n"
                "/* Rows carry no trailing ';' (the object-event precedent):\n"
                " * the compat seam re-includes this header under different\n"
                " * row macros for its static tables and publish passes. */\n");
            for (i = 0; i < (int)layoutCount; i++)
            {
                const struct FamilyResource *blockdata = NULL;
                const struct FamilyResource *border = NULL;
                size_t j;
                for (j = 0; j < resourceCount; j++)
                {
                    if (strncmp(resources[j].symbol, layouts[i].name,
                                strlen(layouts[i].name)) != 0)
                        continue;
                    if (resources[j].kind == KIND_BLOCKDATA)
                        blockdata = &resources[j];
                    else if (resources[j].kind == KIND_BORDER)
                        border = &resources[j];
                }
                if (blockdata == NULL || border == NULL)
                    Fail("layout %s missing blockdata/border resource",
                         layouts[i].name);
                Gen3Buffer_AppendFormat(&framesHeader,
                    "LAYOUT_RECORD(%s, \"%s\", %llu, \"%s\", %llu)\n",
                    layouts[i].name, blockdata->id,
                    (unsigned long long)blockdata->decodedLength, border->id,
                    (unsigned long long)border->decodedLength);
            }
            Gen3Buffer_AppendCStr(&framesHeader,
                "\n#endif /* LAYOUT_FRAMES_GENERATED_H */\n");

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
                        "check passed: %zu resources (%zu blockdata, %zu "
                        "border), %zu layouts, %zu consumers\n",
                        resourceCount, kindCounts[KIND_BLOCKDATA],
                        kindCounts[KIND_BORDER], layoutCount, consumerCount);
                return 0;
            }

            if (!Gen3Util_WriteFile(catalogOut, catalog.data, catalog.length,
                                    errbuf, sizeof(errbuf))
             || !Gen3Util_WriteFile(bindingsOut, bindings.data,
                                    bindings.length, errbuf, sizeof(errbuf))
             || !Gen3Util_WriteFile(ownershipOut, ownership.data,
                                    ownership.length, errbuf, sizeof(errbuf))
             || !Gen3Util_WriteFile(consumersOut, consumerFile.data,
                                    consumerFile.length, errbuf, sizeof(errbuf))
             || !Gen3Util_WriteFile(nativeHeaderOut, nativeHeader.data,
                                    nativeHeader.length, errbuf, sizeof(errbuf))
             || !Gen3Util_WriteFile(framesHeaderOut, framesHeader.data,
                                    framesHeader.length, errbuf, sizeof(errbuf)))
                Fail("cannot write generated output: %s", errbuf);

            Gen3Buffer_Destroy(&catalog);
            Gen3Buffer_Destroy(&bindings);
            Gen3Buffer_Destroy(&ownership);
            Gen3Buffer_Destroy(&consumerFile);
            Gen3Buffer_Destroy(&nativeHeader);
            Gen3Buffer_Destroy(&framesHeader);
        }

        fprintf(stderr,
                "wrote %zu resources (%zu blockdata, %zu border), %zu "
                "layouts, %zu consumers\n",
                resourceCount, kindCounts[KIND_BLOCKDATA],
                kindCounts[KIND_BORDER], layoutCount, consumerCount);
    }
    return 0;
}

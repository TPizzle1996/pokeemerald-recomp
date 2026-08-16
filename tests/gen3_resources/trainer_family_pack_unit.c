/* Stage R7A/R8 §22: the R2 pack format represents the COMPLETE trainer
 * family (front + back). This test builds a deterministic v1 .rpack over all
 * 196 catalogued resources (93 front sheets + 93 front palettes + 8 back
 * sheets + 2 back palettes) directly from the committed descriptor-derived
 * files:
 *
 *   resources/catalogs/emerald/catalog.toml
 *   resources/extraction/emerald/bpee01/bindings.generated.toml
 *   resources/extraction/emerald/bpee01/back_bindings.generated.toml
 *
 * Every binding is loaded; front artifacts are strictly LZ77-decoded while
 * the R8 raw back sheets are their own canonical payload; the M0/M1 key is
 * derived from the canonical id, and the pack is assembled via
 * Gen3ResourcePackBuild + Gen3ResourcePackWriter_Write. The test asserts:
 *
 *   * all 196 entries are accepted (entry count == 196),
 *   * the pack bytes are deterministic (two independent builds byte-identical),
 *   * entries are sorted bytewise by canonical name,
 *   * every decoded size is a multiple of 2048 (sheet) / 32 (palette),
 *   * every stored key/payload digest matches the independently computed value,
 *   * the parsed pack's provider-content digest matches across builds.
 *
 * The pack is NOT hooked into gameplay (R7A §22); it only proves the format
 * carries the whole family. Platform-neutral: no Emerald, GBA, SDL, ROM_BASE,
 * or frontend symbols. Run from the repo root.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/lz77.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_pack_writer.h"
#include "gen3/resources/sha256.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"

#define CATALOG_PATH   "resources/catalogs/emerald/catalog.toml"
#define BINDINGS_PATH  "resources/extraction/emerald/bpee01/bindings.generated.toml"
#define BACK_BINDINGS_PATH "resources/extraction/emerald/bpee01/back_bindings.generated.toml"
#define MAX_BINDINGS   256u

#define SHEET_SIZE  2048u
#define PALETTE_SIZE 32u

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        gChecks++;                                                           \
        if (!(cond))                                                         \
        {                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            gFailures++;                                                     \
        }                                                                    \
    } while (0)

struct Binding
{
    char id[GEN3_RESOURCE_NAME_MAX + 1u];
    char symbol[192];
    char artifact[512];
    char encoding[32];
    char representation[64];
    long long expectedSize;
};

/* Loads one TOML bindings document, appending its [[bindings]] blocks to
 * `out` starting at `*outCount`. Reads the front family then the back family
 * so the union covers the whole catalog. */
static bool LoadBindingsFile(const char *path, struct Binding *out,
                             size_t *outCount)
{
    struct Gen3Buffer text;
    struct Gen3TomlDocument doc;
    char errbuf[512];
    size_t count;
    size_t i;

    /* Gen3Util_ReadFile owns buffer init; a caller-side Gen3Buffer_Init would
     * leak its allocation. Pass the struct uninitialized. */
    if (!Gen3Util_ReadFile(path, &text, errbuf, sizeof(errbuf)))
    {
        printf("FAIL: cannot read %s: %s\n", path, errbuf);
        return false;
    }
    if (!Gen3Toml_Parse(text.data, text.length, &doc, errbuf, sizeof(errbuf)))
    {
        printf("FAIL: parse %s: %s\n", path, errbuf);
        Gen3Buffer_Destroy(&text);
        return false;
    }
    count = Gen3Toml_GetArrayCount(&doc.root, "bindings");
    if (count == 0u || *outCount + count > MAX_BINDINGS)
    {
        printf("FAIL: bindings count %zu out of range\n", count);
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&text);
        return false;
    }
    for (i = 0u; i < count; i++)
    {
        const struct Gen3TomlMap *item = Gen3Toml_GetArrayItem(&doc.root, "bindings", i);
        struct Binding *b = &out[*outCount + i];
        const char *s;
        long long n;
        memset(b, 0, sizeof(*b));
        if (!Gen3Toml_GetString(item, "id", &s)
         || strlen(s) >= sizeof(b->id)
         || !Gen3Toml_GetString(item, "symbol", &s)
         || strlen(s) >= sizeof(b->symbol)
         || !Gen3Toml_GetString(item, "source_artifact", &s)
         || strlen(s) >= sizeof(b->artifact)
         || !Gen3Toml_GetString(item, "source_encoding", &s)
         || strlen(s) >= sizeof(b->encoding)
         || !Gen3Toml_GetString(item, "canonical_representation", &s)
         || strlen(s) >= sizeof(b->representation)
         || !Gen3Toml_GetInteger(item, "expected_decoded_size", &n))
        {
            printf("FAIL: binding %zu missing a required field\n", i);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&text);
            return false;
        }
        Gen3Toml_GetString(item, "id", &s);
        strcpy(b->id, s);
        Gen3Toml_GetString(item, "symbol", &s);
        strcpy(b->symbol, s);
        Gen3Toml_GetString(item, "source_artifact", &s);
        strcpy(b->artifact, s);
        Gen3Toml_GetString(item, "source_encoding", &s);
        strcpy(b->encoding, s);
        Gen3Toml_GetString(item, "canonical_representation", &s);
        strcpy(b->representation, s);
        b->expectedSize = n;
        if (strcmp(b->encoding, "gba-lz77") != 0
         && strcmp(b->encoding, "raw") != 0)
        {
            printf("FAIL: binding %zu has unsupported encoding '%s'\n", i, b->encoding);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&text);
            return false;
        }
    }
    *outCount += count;
    Gen3Toml_Destroy(&doc);
    Gen3Buffer_Destroy(&text);
    return true;
}

static bool LoadBindings(struct Binding *out, size_t *outCount)
{
    *outCount = 0u;
    if (!LoadBindingsFile(BINDINGS_PATH, out, outCount))
        return false;
    return LoadBindingsFile(BACK_BINDINGS_PATH, out, outCount);
}

static int CompareBindingsById(const struct Binding *left, const struct Binding *right)
{
    return strcmp(left->id, right->id);
}

/* Catalog lookup: type for an id. Returns "tile-graphics" / "palette", or
 * NULL when the id is absent (an orphaned binding must fail). */
static const char *CatalogType(const struct Gen3TomlDocument *catalog,
                               const char *id)
{
    size_t count = Gen3Toml_GetArrayCount(&catalog->root, "resources");
    size_t i;
    for (i = 0u; i < count; i++)
    {
        const struct Gen3TomlMap *item = Gen3Toml_GetArrayItem(&catalog->root, "resources", i);
        const char *rid = NULL;
        const char *rtype = NULL;
        Gen3Toml_GetString(item, "id", &rid);
        if (rid != NULL && strcmp(rid, id) == 0)
        {
            Gen3Toml_GetString(item, "type", &rtype);
            return rtype;
        }
    }
    return NULL;
}

static void Sha256OfBytes(const uint8_t *data, size_t size, uint8_t out[32])
{
    struct Gen3Sha256Context ctx;
    Gen3Sha256_Init(&ctx);
    Gen3Sha256_Update(&ctx, data, size);
    Gen3Sha256_Final(&ctx, out);
}

static void Sha256OfFile(const char *path, uint8_t out[32], char *errbuf, size_t errbufSize)
{
    struct Gen3Buffer file; /* ReadFile owns init (see LoadBindings note) */
    if (!Gen3Util_ReadFile(path, &file, errbuf, errbufSize))
    {
        memset(out, 0, 32u);
        Gen3Buffer_Destroy(&file);
        return;
    }
    Sha256OfBytes((const uint8_t *)file.data, file.length, out);
    Gen3Buffer_Destroy(&file);
}

/* One loaded resource, ready to hand to the pack writer. */
struct LoadedResource
{
    struct Binding binding;
    struct Gen3Buffer encoded;   /* the .lz artifact */
    struct Gen3Buffer decoded;   /* strict LZ77 output */
    uint8_t key[GEN3_RESOURCE_KEY_SIZE];
    uint8_t encodedSha[32];
    uint8_t decodedSha[32];
    enum Gen3ResourceType type;
};

static int BuildPackBytes(const struct LoadedResource *resources, size_t count,
                          const uint8_t catalogSha[32],
                          const uint8_t manifestSha[32],
                          uint8_t **outBytes, size_t *outSize)
{
    struct Gen3ResourcePackProfileInput profile;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePackBuild *build;
    uint8_t romSha256[32];
    uint8_t romSha1[20];
    size_t i;
    int result;

    /* Synthetic ROM provenance (R7A/R8 §22: not hooked into gameplay). The
     * digest is a fixed label hash so the pack is deterministic; the real
     * fixture ROM hashes would serve the same purpose. */
    Sha256OfBytes((const uint8_t *)"R8 synthetic trainer family fixture",
                  sizeof("R8 synthetic trainer family fixture") - 1u, romSha256);
    memcpy(romSha1, romSha256, sizeof(romSha1));

    memset(&profile, 0, sizeof(profile));
    profile.basePackVersion = 1u;
    profile.catalogVersion = 1u;
    profile.extractionManifestVersion = 1u;
    profile.canonicalRepresentationVersion = 1u;
    profile.sourceRomSize = 16777216u;
    profile.sourceRomSha1 = romSha1;
    profile.sourceRomSha256 = romSha256;
    memcpy(profile.gameCode, "BPEE", 4);
    memcpy(profile.makerCode, "01", 2);
    profile.softwareRevision = 0;
    memset(profile.gameId, 0, sizeof(profile.gameId));
    memcpy(profile.gameId, "emerald", 7);
    profile.catalogSha256 = catalogSha;
    profile.extractionManifestSha256 = manifestSha;

    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
        return -1;
    if (Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) != GEN3_PACK_OK)
    {
        Gen3ResourcePackBuild_Destroy(build);
        Gen3ResourcePackDiagnostics_Destroy(&diag);
        return -2;
    }
    for (i = 0u; i < count; i++)
    {
        const struct LoadedResource *r = &resources[i];
        struct Gen3ResourcePackEntryInput entry;
        memset(&entry, 0, sizeof(entry));
        entry.canonicalName = r->binding.id;
        entry.key = (const Gen3ResourceKey *)r->key;
        entry.type = r->type;
        entry.schema = 1u;
        entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
        entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
        entry.sourceEncoding = strcmp(r->binding.encoding, "raw") == 0
            ? GEN3_PACK_SOURCE_ENCODING_RAW : GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
        entry.canonicalPayload = (const uint8_t *)r->decoded.data;
        entry.canonicalPayloadSize = r->decoded.length;
        entry.canonicalPayloadSha256 = r->decodedSha;
        entry.sourceRomOffset = 0u; /* R7B derives real offsets; provenance only */
        entry.sourceEncodedSize = r->encoded.length;
        entry.sourceEncodedSha256 = r->encodedSha;
        if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
        {
            printf("FAIL: AddEntry %s\n", r->binding.id);
            Gen3ResourcePackBuild_Destroy(build);
            Gen3ResourcePackDiagnostics_Destroy(&diag);
            return -3;
        }
    }
    memset(&bytes, 0, sizeof(bytes));
    result = Gen3ResourcePackWriter_Write(build, &bytes, &diag);
    if (result == GEN3_PACK_OK)
    {
        *outBytes = bytes.data;
        *outSize = bytes.size;
    }
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    return result == GEN3_PACK_OK ? 0 : -4;
}

int main(void)
{
    struct Gen3TomlDocument catalog;
    struct Gen3Buffer catalogText;
    struct Binding bindings[MAX_BINDINGS];
    struct LoadedResource *resources = NULL;
    size_t bindingCount = 0;
    size_t i;
    char errbuf[512];
    uint8_t catalogSha[32];
    uint8_t manifestSha[32];
    uint8_t *bytesA = NULL;
    uint8_t *bytesB = NULL;
    size_t sizeA = 0;
    size_t sizeB = 0;
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList diag;

    printf("== R7A/R8 §22: full-family pack (196 resources) ==\n");

    if (!LoadBindings(bindings, &bindingCount))
        return 1;
    if (bindingCount != 196u)
    {
        printf("FAIL: expected 196 bindings, found %zu\n", bindingCount);
        return 1;
    }
    gChecks++;
    printf("  PASS %zu bindings loaded (186 front + 10 back)\n", bindingCount);

    /* Catalog. ReadFile owns init (see LoadBindings note). */
    if (!Gen3Util_ReadFile(CATALOG_PATH, &catalogText, errbuf, sizeof(errbuf))
     || !Gen3Toml_Parse(catalogText.data, catalogText.length, &catalog, errbuf, sizeof(errbuf)))
    {
        printf("FAIL: cannot load catalog: %s\n", errbuf);
        return 1;
    }
    if (Gen3Toml_GetArrayCount(&catalog.root, "resources") != 196u)
    {
        printf("FAIL: catalog does not have 196 resources\n");
        return 1;
    }

    Sha256OfFile(CATALOG_PATH, catalogSha, errbuf, sizeof(errbuf));
    Sha256OfFile(BINDINGS_PATH, manifestSha, errbuf, sizeof(errbuf));

    resources = (struct LoadedResource *)calloc(bindingCount, sizeof(resources[0]));
    if (resources == NULL)
    {
        printf("FAIL: out of memory\n");
        return 1;
    }

    /* Load + strictly decode every artifact (raw sheets: encoded == decoded). */
    for (i = 0u; i < bindingCount; i++)
    {
        struct LoadedResource *r = &resources[i];
        size_t decodedSize = 0u;
        size_t want;
        const char *type;
        enum Gen3Lz77Result lz;

        r->binding = bindings[i];
        /* ReadFile owns `encoded` init; `decoded` is (re)initialized below to
         * the exact expected size before the strict decode. */
        if (!Gen3Util_ReadFile(r->binding.artifact, &r->encoded, errbuf, sizeof(errbuf)))
        {
            printf("FAIL: cannot read %s: %s\n", r->binding.artifact, errbuf);
            return 1;
        }
        want = (size_t)r->binding.expectedSize;
        Gen3Buffer_Destroy(&r->decoded);
        Gen3Buffer_Init(&r->decoded, want);
        if (strcmp(r->binding.encoding, "raw") == 0)
        {
            /* R8 raw sheets: the artifact IS the canonical decoded payload
             * (no LZ77 wrapper); the pack entry uses the RAW source encoding
             * and the bytes are identical either way. */
            if (r->encoded.length != want)
            {
                printf("FAIL: raw artifact size %zu != expected %zu for %s\n",
                       r->encoded.length, want, r->binding.id);
                return 1;
            }
            memcpy(r->decoded.data, r->encoded.data, want);
            decodedSize = want;
        }
        else
        {
            lz = Gen3Lz77_Decode((const uint8_t *)r->encoded.data, r->encoded.length,
                                 (uint8_t *)r->decoded.data, want, &decodedSize);
            if (lz != GEN3_LZ77_OK || decodedSize != want)
            {
                printf("FAIL: strict decode %s (lz=%d, %zu != %zu)\n",
                       r->binding.artifact, (int)lz, decodedSize, want);
                return 1;
            }
        }
        r->decoded.length = decodedSize;
        if ((want % SHEET_SIZE != 0u || want == 0u) && want != PALETTE_SIZE)
        {
            printf("FAIL: unexpected decoded size %zu for %s\n", want, r->binding.id);
            return 1;
        }
        Sha256OfBytes((const uint8_t *)r->encoded.data, r->encoded.length, r->encodedSha);
        Sha256OfBytes((const uint8_t *)r->decoded.data, r->decoded.length, r->decodedSha);
        Gen3ResourceId_DeriveKey(r->binding.id, (Gen3ResourceKey *)r->key);
        type = CatalogType(&catalog, r->binding.id);
        if (type == NULL)
        {
            printf("FAIL: binding %s has no catalog entry\n", r->binding.id);
            return 1;
        }
        if (strcmp(type, "tile-graphics") == 0)
            r->type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
        else if (strcmp(type, "palette") == 0)
            r->type = GEN3_RESOURCE_TYPE_PALETTE;
        else
        {
            printf("FAIL: unknown catalog type '%s' for %s\n", type, r->binding.id);
            return 1;
        }
    }
    gChecks++;
    printf("  PASS all %zu artifacts strictly decoded (2048-multiple/32) + keys derived\n",
           bindingCount);

    /* The pack stores entries bytewise-sorted by canonical id; the union is
     * front-run-then-back-run, so sort resources to pair with entries. */
    qsort(resources, bindingCount, sizeof(resources[0]),
          (int (*)(const void *, const void *))CompareBindingsById);

    /* Two independent builds must be byte-identical. */
    if (BuildPackBytes(resources, bindingCount, catalogSha, manifestSha,
                       &bytesA, &sizeA) != 0
     || BuildPackBytes(resources, bindingCount, catalogSha, manifestSha,
                       &bytesB, &sizeB) != 0)
    {
        printf("FAIL: pack build failed\n");
        return 1;
    }
    gChecks++;
    if (sizeA != sizeB || memcmp(bytesA, bytesB, sizeA) != 0)
    {
        printf("FAIL: pack not deterministic (%zu vs %zu)\n", sizeA, sizeB);
        return 1;
    }
    printf("  PASS pack bytes deterministic across two builds (%zu bytes)\n", sizeA);

    /* Parse and verify the family pack. */
    Gen3ResourcePackDiagnostics_Init(&diag);
    if (Gen3ResourcePack_Parse(bytesA, sizeA, &pack, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: parse full-family pack\n");
        return 1;
    }
    gChecks++;
    if (Gen3ResourcePack_GetEntryCount(pack) != 196u)
    {
        printf("FAIL: parsed pack entry count != 196\n");
        return 1;
    }
    printf("  PASS parsed pack has 196 entries\n");

    {
        const char *prev = NULL;
        for (i = 0u; i < bindingCount; i++)
        {
            const struct Gen3ResourcePackEntry *entry =
                Gen3ResourcePack_GetEntry(pack, i);
            const struct LoadedResource *r = &resources[i];
            uint8_t wantSha[32];

            if (entry == NULL)
            {
                printf("FAIL: missing entry %zu\n", i);
                return 1;
            }
            if (prev != NULL && strcmp(prev, entry->canonicalName) >= 0)
            {
                printf("FAIL: entries not sorted at %zu\n", i);
                return 1;
            }
            prev = entry->canonicalName;
            if (memcmp(entry->key.bytes, r->key, GEN3_RESOURCE_KEY_SIZE) != 0)
            {
                printf("FAIL: key mismatch for %s\n", r->binding.id);
                return 1;
            }
            if (entry->payloadSize != r->decoded.length
             || memcmp(entry->payload, r->decoded.data, r->decoded.length) != 0)
            {
                printf("FAIL: payload mismatch for %s\n", r->binding.id);
                return 1;
            }
            Sha256OfBytes(entry->payload, entry->payloadSize, wantSha);
            if (memcmp(entry->payloadSha256, wantSha, 32u) != 0)
            {
                printf("FAIL: stored payload digest mismatch for %s\n", r->binding.id);
                return 1;
            }
            if (entry->type != r->type)
            {
                printf("FAIL: type mismatch for %s\n", r->binding.id);
                return 1;
            }
            if ((entry->flags & GEN3_PACK_FLAG_REQUIRED_FOR_BASE) == 0u)
            {
                printf("FAIL: required_for_base not set for %s\n", r->binding.id);
                return 1;
            }
        }
    }
    gChecks++;
    printf("  PASS entries sorted bytewise; keys/payloads/types/flags verified\n");

    {
        uint8_t providerA[GEN3_PACK_SHA256_SIZE];
        uint8_t providerB[GEN3_PACK_SHA256_SIZE];
        struct Gen3ResourcePack *packB = NULL;
        if (Gen3ResourcePack_Parse(bytesB, sizeB, &packB, &diag) != GEN3_PACK_OK
         || !Gen3ResourcePack_GetProviderDigest(pack, providerA)
         || !Gen3ResourcePack_GetProviderDigest(packB, providerB)
         || memcmp(providerA, providerB, sizeof(providerA)) != 0)
        {
            printf("FAIL: provider-content digest unstable\n");
            return 1;
        }
        Gen3ResourcePack_Destroy(packB);
    }
    gChecks++;
    printf("  PASS provider-content digest stable across builds\n");

    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    for (i = 0u; i < bindingCount; i++)
    {
        Gen3Buffer_Destroy(&resources[i].encoded);
        Gen3Buffer_Destroy(&resources[i].decoded);
    }
    free(resources);
    Gen3Toml_Destroy(&catalog);
    Gen3Buffer_Destroy(&catalogText);
    free(bytesA);
    free(bytesB);

    printf("== %d checks, %d failures ==\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

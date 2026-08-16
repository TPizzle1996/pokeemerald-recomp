/* Stage R4: shared pack->provider adapter tests.
 *
 * Covers the platform-neutral Gen3ResourcePackProvider_CreateFromPack adapter:
 * the two-resource happy path (through the normal M0/M1 resolver), payload
 * ownership across pack destruction, the fail-closed catalog gate (key / type /
 * schema / unknown-resource mismatches), empty-pack refusal, malformed
 * requiredForProvider entries, missing requiredForBase resources, invalid
 * arguments, and the "unvalidated pack is impossible" guarantee (raw bytes only
 * reach a provider after the R2 strict reader accepts them).
 *
 * Everything here is platform-neutral. The resource names use the reserved
 * `gen3` namespace and there is no Emerald / BPEE01 / Brendan / FireRed
 * constant in this file or in the code under test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resource_internal.h"
#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_content_digest.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_pack_provider.h"
#include "gen3/resources/resource_pack_writer.h"
#include "gen3/resources/resource_provider.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/sha256.h"

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(label, cond)                                                   \
    do                                                                       \
    {                                                                        \
        gChecks++;                                                           \
        if (!(cond))                                                         \
        {                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (label));         \
            gFailures++;                                                     \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Fixture: a deterministic two-resource pack + catalog                 */
/* ------------------------------------------------------------------ */

#define TILESET_NAME "gen3:sample/bg/tileset"
#define PALETTE_NAME "gen3:sample/bg/palette"

#define TILESET_SIZE 2048u /* tile-graphics: multiple of 32 */
#define PALETTE_SIZE 32u   /* palette: even, <= 512 */

#define PROVIDER_ID   "sample.rom-base.test"
#define PROVIDER_VER  "v1"
#define PROVIDER_PREC 300u

static const char kRomSha1Hex[] =
    "092ee293c6e4381074682375249561bbc41bec44";
static const char kRomSha256Hex[] =
    "e4c8f3693d840fb233dfa31c1cd2b4d21aa2dde67359e4aad4b064a4b1b33bca";
static const char kCatalogShaHex[] =
    "472f8fd1c9c824d8eb30a0a553a483699be52256a94a7ed16b92a2699db80bb9";
static const char kManifestShaHex[] =
    "fbdaba5ceed53da4fb0cbf375b2178250252acc4fe38f4ea876de5ffbf0bc475";

static unsigned char HexVal(char c)
{
    if (c >= '0' && c <= '9')
        return (unsigned char)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (unsigned char)(c - 'a' + 10u);
    if (c >= 'A' && c <= 'F')
        return (unsigned char)(c - 'A' + 10u);
    return 0u;
}

static void FromHex(unsigned char *out, const char *hex)
{
    size_t i;
    for (i = 0; hex[i] != '\0'; i += 2u)
        out[i / 2u] = (unsigned char)((HexVal(hex[i]) << 4) | HexVal(hex[i + 1u]));
}

static void Sha256BytesOf(const unsigned char *data, size_t size,
                          unsigned char digest[32])
{
    struct Gen3Sha256Context context;
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, data, size);
    Gen3Sha256_Final(&context, digest);
}

static bool AllZero8(const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i] != 0u)
            return false;
    return true;
}

/* Storage for the profile's borrowed hash pointers. The caller owns these
 * buffers; SetProfile copies from them synchronously, before they go out of
 * scope at the end of the calling function. */
struct FixtureHashes
{
    unsigned char romSha1[GEN3_PACK_SHA1_SIZE];
    unsigned char romSha256[GEN3_PACK_SHA256_SIZE];
    unsigned char catalog[GEN3_PACK_SHA256_SIZE];
    unsigned char manifest[GEN3_PACK_SHA256_SIZE];
    char gameId[GEN3_PACK_GAME_ID_SIZE];
};

static void FillHashes(struct FixtureHashes *hashes, const char *gameId)
{
    FromHex(hashes->romSha1, kRomSha1Hex);
    FromHex(hashes->romSha256, kRomSha256Hex);
    FromHex(hashes->catalog, kCatalogShaHex);
    FromHex(hashes->manifest, kManifestShaHex);
    memset(hashes->gameId, 0, sizeof(hashes->gameId));
    memcpy(hashes->gameId, gameId, strlen(gameId) + 1u);
}

static void FillProfile(struct Gen3ResourcePackProfileInput *profile,
                        const struct FixtureHashes *hashes)
{
    memset(profile, 0, sizeof(*profile));
    profile->basePackVersion = 1u;
    profile->catalogVersion = 1u;
    profile->extractionManifestVersion = 1u;
    profile->canonicalRepresentationVersion = 1u;
    profile->sourceRomSize = 16u * 1024u * 1024u;
    profile->sourceRomSha1 = hashes->romSha1;
    profile->sourceRomSha256 = hashes->romSha256;
    memcpy(profile->gameCode, "TEST", 4u);
    memcpy(profile->makerCode, "01", 2u);
    profile->softwareRevision = 0u;
    memcpy(profile->gameId, hashes->gameId, GEN3_PACK_GAME_ID_SIZE);
    profile->catalogSha256 = hashes->catalog;
    profile->extractionManifestSha256 = hashes->manifest;
}

/* Deterministic payload generator so byte comparisons are meaningful. */
static void FillPayload(unsigned char *payload, size_t size, unsigned seed)
{
    size_t i;
    for (i = 0; i < size; i++)
        payload[i] = (unsigned char)((i * seed + (i >> 3u)) & 0xFFu);
}

/* Build a two-entry pack. `tilesetSize` may be set to 33 to produce a malformed
 * tile-graphics payload (the writer does not validate payload shape). */
static void BuildSyntheticPack(const char *tilesetName, const char *paletteName,
                               size_t tilesetSize, size_t paletteSize,
                               struct Gen3ResourcePackBytes *out)
{
    unsigned char *tileset = malloc(tilesetSize != 0 ? tilesetSize : 1u);
    unsigned char *palette = malloc(paletteSize != 0 ? paletteSize : 1u);
    unsigned char tilesetSha[32];
    unsigned char paletteSha[32];
    Gen3ResourceKey tilesetKey;
    Gen3ResourceKey paletteKey;
    struct Gen3ResourcePackProfileInput profile;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackBuild *build;
    struct FixtureHashes hashes;

    FillPayload(tileset, tilesetSize, 7u);
    FillPayload(palette, paletteSize, 3u);
    Sha256BytesOf(tileset, tilesetSize, tilesetSha);
    Sha256BytesOf(palette, paletteSize, paletteSha);
    Gen3ResourceId_DeriveKey(tilesetName, &tilesetKey);
    Gen3ResourceId_DeriveKey(paletteName, &paletteKey);

    Gen3ResourcePackDiagnostics_Init(&diag);
    FillHashes(&hashes, "sample");
    FillProfile(&profile, &hashes);

    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
    {
        printf("FAIL: BuildSyntheticPack: OOM\n");
        exit(1);
    }
    if (Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildSyntheticPack: SetProfile\n");
        exit(1);
    }

    memset(&entry, 0, sizeof(entry));
    entry.canonicalName = paletteName;
    entry.key = &paletteKey;
    entry.type = GEN3_RESOURCE_TYPE_PALETTE;
    entry.schema = 1u;
    entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
    entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
    entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
    entry.canonicalPayload = palette;
    entry.canonicalPayloadSize = paletteSize;
    entry.canonicalPayloadSha256 = paletteSha;
    entry.sourceRomOffset = 0x00310000u; /* within the 16 MiB source ROM */
    entry.sourceEncodedSize = paletteSize;
    entry.sourceEncodedSha256 = paletteSha;
    if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildSyntheticPack: AddEntry palette\n");
        exit(1);
    }

    memset(&entry, 0, sizeof(entry));
    entry.canonicalName = tilesetName;
    entry.key = &tilesetKey;
    entry.type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    entry.schema = 1u;
    entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
    entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
    entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
    entry.canonicalPayload = tileset;
    entry.canonicalPayloadSize = tilesetSize;
    entry.canonicalPayloadSha256 = tilesetSha;
    entry.sourceRomOffset = 0x00300000u; /* within the 16 MiB source ROM */
    entry.sourceEncodedSize = tilesetSize;
    entry.sourceEncodedSha256 = tilesetSha;
    if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildSyntheticPack: AddEntry tileset\n");
        exit(1);
    }

    memset(out, 0, sizeof(*out));
    if (Gen3ResourcePackWriter_Write(build, out, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildSyntheticPack: Write\n");
        exit(1);
    }

    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    free(tileset);
    free(palette);
}

static void PutLE32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value);
    p[1] = (unsigned char)(value >> 8u);
    p[2] = (unsigned char)(value >> 16u);
    p[3] = (unsigned char)(value >> 24u);
}

static void PutLE64(unsigned char *p, uint64_t value)
{
    size_t i;
    for (i = 0; i < 8u; i++)
        p[i] = (unsigned char)(value >> (8u * i));
}

/* Build a valid v1 zero-entry pack by hand.
 *
 * The writer intentionally refuses to emit an empty pack (a base pack with
 * nothing in it is meaningless), but the v1 reader accepts one: with
 * entry_count = 0 the TOC/names/payload sections are empty, the file is exactly
 * the 448-byte header, and every section hash is SHA-256 of the empty input.
 * This replicates WriteHeader's layout for the empty case so the adapter's
 * NO_ENTRIES guard can be exercised through a real parsed pack. */
static void BuildEmptyPack(struct Gen3ResourcePackBytes *out)
{
    struct Gen3ResourcePackLogicalInput logicalInput;
    struct Gen3Sha256Context context;
    struct FixtureHashes hashes;
    unsigned char emptySha[32];
    unsigned char logicalSha[32];
    unsigned char headerSha[32];
    unsigned char *header;

    Gen3Sha256_Init(&context);
    Gen3Sha256_Final(&context, emptySha); /* SHA-256 of empty input */

    FillHashes(&hashes, "sample");
    header = calloc(1, GEN3_PACK_HEADER_SIZE);

    memcpy(header + GEN3_PACK_OFF_MAGIC, GEN3_PACK_MAGIC_BYTES, 8u);
    PutLE32(header + GEN3_PACK_OFF_HEADER_SIZE, GEN3_PACK_HEADER_SIZE);
    PutLE32(header + GEN3_PACK_OFF_FORMAT_VERSION, 1u);
    PutLE32(header + GEN3_PACK_OFF_ENDIAN_TAG, GEN3_PACK_ENDIAN_TAG);
    PutLE32(header + GEN3_PACK_OFF_FLAGS, 0u);
    PutLE32(header + GEN3_PACK_OFF_BASE_PACK_VERSION, 1u);
    PutLE32(header + GEN3_PACK_OFF_ENTRY_COUNT, 0u);
    PutLE32(header + GEN3_PACK_OFF_ENTRY_SIZE, GEN3_PACK_ENTRY_SIZE);
    PutLE32(header + GEN3_PACK_OFF_API_MAJOR, 1u);
    PutLE32(header + GEN3_PACK_OFF_API_MINOR, 0u);
    PutLE32(header + GEN3_PACK_OFF_API_PATCH, 0u);
    PutLE32(header + GEN3_PACK_OFF_CATALOG_VERSION, 1u);
    PutLE32(header + GEN3_PACK_OFF_EXTRACTION_MANIFEST_VER, 1u);
    PutLE32(header + GEN3_PACK_OFF_CANONICAL_REP_VERSION, 1u);
    PutLE32(header + GEN3_PACK_OFF_RESERVED0, 0u);
    PutLE64(header + GEN3_PACK_OFF_FILE_SIZE, GEN3_PACK_HEADER_SIZE);
    PutLE64(header + GEN3_PACK_OFF_TOC_OFFSET, GEN3_PACK_HEADER_SIZE);
    PutLE64(header + GEN3_PACK_OFF_TOC_SIZE, 0u);
    PutLE64(header + GEN3_PACK_OFF_NAMES_OFFSET, GEN3_PACK_HEADER_SIZE);
    PutLE64(header + GEN3_PACK_OFF_NAMES_SIZE, 0u);
    PutLE64(header + GEN3_PACK_OFF_PAYLOAD_OFFSET, GEN3_PACK_HEADER_SIZE);
    PutLE64(header + GEN3_PACK_OFF_PAYLOAD_SIZE, 0u);
    PutLE64(header + GEN3_PACK_OFF_ROM_SIZE, 16u * 1024u * 1024u);
    memcpy(header + GEN3_PACK_OFF_ROM_SHA1, hashes.romSha1, GEN3_PACK_SHA1_SIZE);
    memcpy(header + GEN3_PACK_OFF_ROM_SHA256, hashes.romSha256, GEN3_PACK_SHA256_SIZE);
    memcpy(header + GEN3_PACK_OFF_GAME_CODE, "TEST", 4u);
    memcpy(header + GEN3_PACK_OFF_MAKER_CODE, "01", 2u);
    header[GEN3_PACK_OFF_SOFTWARE_REVISION] = 0u;
    header[GEN3_PACK_OFF_RESERVED1] = 0u;
    memcpy(header + GEN3_PACK_OFF_GAME_ID, hashes.gameId, GEN3_PACK_GAME_ID_SIZE);
    memcpy(header + GEN3_PACK_OFF_CATALOG_SHA256, hashes.catalog, GEN3_PACK_SHA256_SIZE);
    memcpy(header + GEN3_PACK_OFF_MANIFEST_SHA256, hashes.manifest, GEN3_PACK_SHA256_SIZE);
    memcpy(header + GEN3_PACK_OFF_TOC_SHA256, emptySha, GEN3_PACK_SHA256_SIZE);
    memcpy(header + GEN3_PACK_OFF_NAMES_SHA256, emptySha, GEN3_PACK_SHA256_SIZE);
    memcpy(header + GEN3_PACK_OFF_PAYLOAD_SHA256, emptySha, GEN3_PACK_SHA256_SIZE);

    memset(&logicalInput, 0, sizeof(logicalInput));
    logicalInput.formatVersion = 1u;
    logicalInput.apiMajor = 1u;
    logicalInput.apiMinor = 0u;
    logicalInput.apiPatch = 0u;
    logicalInput.basePackVersion = 1u;
    logicalInput.catalogVersion = 1u;
    logicalInput.extractionManifestVersion = 1u;
    logicalInput.canonicalRepresentationVersion = 1u;
    logicalInput.sourceRomSize = 16u * 1024u * 1024u;
    logicalInput.sourceRomSha1 = hashes.romSha1;
    logicalInput.sourceRomSha256 = hashes.romSha256;
    logicalInput.gameCode = header + GEN3_PACK_OFF_GAME_CODE;
    logicalInput.makerCode = header + GEN3_PACK_OFF_MAKER_CODE;
    logicalInput.softwareRevision = 0u;
    logicalInput.reserved = 0u;
    logicalInput.catalogSha256 = hashes.catalog;
    logicalInput.extractionManifestSha256 = hashes.manifest;
    logicalInput.tocSha256 = emptySha;
    logicalInput.namesSha256 = emptySha;
    logicalInput.payloadSha256 = emptySha;
    if (!Gen3ResourceContentDigest_LogicalPack(&logicalInput, logicalSha))
    {
        printf("FAIL: BuildEmptyPack: logical digest\n");
        exit(1);
    }
    memcpy(header + GEN3_PACK_OFF_LOGICAL_SHA256, logicalSha, GEN3_PACK_SHA256_SIZE);

    /* Header hash: over the whole header with its own hash field still zero. */
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, header, GEN3_PACK_HEADER_SIZE);
    Gen3Sha256_Final(&context, headerSha);
    memcpy(header + GEN3_PACK_OFF_HEADER_SHA256, headerSha, GEN3_PACK_SHA256_SIZE);

    memset(out, 0, sizeof(*out));
    out->data = header;
    out->size = GEN3_PACK_HEADER_SIZE;
}

/* Build a finalized catalog for the two resources. */
static void BuildGoodCatalog(struct Gen3ResourceCatalog **outCatalog)
{
    struct Gen3ResourceCatalog *catalog = Gen3ResourceCatalog_Create();
    if (catalog == NULL)
    {
        printf("FAIL: BuildGoodCatalog: OOM\n");
        exit(1);
    }
    Gen3ResourceCatalog_Add(catalog, PALETTE_NAME, GEN3_RESOURCE_TYPE_PALETTE,
                            1u, true, NULL);
    Gen3ResourceCatalog_Add(catalog, TILESET_NAME, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                            1u, true, NULL);
    Gen3ResourceCatalog_Finalize(catalog, NULL);
    *outCatalog = catalog;
}

/* A catalog whose derived key for every name is mutated (bit-flipped) so the
 * pack's correct keys never match. */
static void MutatedKeyDeriver(const char *canonicalName, Gen3ResourceKey *outKey,
                              void *context)
{
    Gen3ResourceKey derived;
    (void)context;
    Gen3ResourceId_DeriveKey(canonicalName, &derived);
    derived.bytes[0] ^= 0x80u;
    *outKey = derived;
}

static void BuildMutatedKeyCatalog(struct Gen3ResourceCatalog **outCatalog)
{
    struct Gen3ResourceCatalog *catalog = Gen3ResourceCatalog_Create();
    Gen3ResourceCatalog_SetKeyDeriverForTest(catalog, MutatedKeyDeriver, NULL);
    Gen3ResourceCatalog_Add(catalog, PALETTE_NAME, GEN3_RESOURCE_TYPE_PALETTE,
                            1u, true, NULL);
    Gen3ResourceCatalog_Add(catalog, TILESET_NAME, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                            1u, true, NULL);
    Gen3ResourceCatalog_Finalize(catalog, NULL);
    *outCatalog = catalog;
}

static void BuildCatalogWithType(struct Gen3ResourceCatalog **outCatalog)
{
    /* Tileset declared as PALETTE: type mismatch. */
    struct Gen3ResourceCatalog *catalog = Gen3ResourceCatalog_Create();
    Gen3ResourceCatalog_Add(catalog, PALETTE_NAME, GEN3_RESOURCE_TYPE_PALETTE,
                            1u, true, NULL);
    Gen3ResourceCatalog_Add(catalog, TILESET_NAME, GEN3_RESOURCE_TYPE_PALETTE,
                            1u, true, NULL);
    Gen3ResourceCatalog_Finalize(catalog, NULL);
    *outCatalog = catalog;
}

static void BuildCatalogWithSchema(struct Gen3ResourceCatalog **outCatalog)
{
    /* Tileset declared schema 2: schema mismatch. */
    struct Gen3ResourceCatalog *catalog = Gen3ResourceCatalog_Create();
    Gen3ResourceCatalog_Add(catalog, PALETTE_NAME, GEN3_RESOURCE_TYPE_PALETTE,
                            1u, true, NULL);
    Gen3ResourceCatalog_Add(catalog, TILESET_NAME, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                            2u, true, NULL);
    Gen3ResourceCatalog_Finalize(catalog, NULL);
    *outCatalog = catalog;
}

static void BuildCatalogMissingTileset(struct Gen3ResourceCatalog **outCatalog)
{
    /* Pack has two entries, catalog knows only the palette. */
    struct Gen3ResourceCatalog *catalog = Gen3ResourceCatalog_Create();
    Gen3ResourceCatalog_Add(catalog, PALETTE_NAME, GEN3_RESOURCE_TYPE_PALETTE,
                            1u, true, NULL);
    Gen3ResourceCatalog_Finalize(catalog, NULL);
    *outCatalog = catalog;
}

static void BuildCatalogWithExtraBase(struct Gen3ResourceCatalog **outCatalog)
{
    /* Catalog requires an extra base resource the pack does not provide. */
    struct Gen3ResourceCatalog *catalog = Gen3ResourceCatalog_Create();
    Gen3ResourceCatalog_Add(catalog, PALETTE_NAME, GEN3_RESOURCE_TYPE_PALETTE,
                            1u, true, NULL);
    Gen3ResourceCatalog_Add(catalog, TILESET_NAME, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                            1u, true, NULL);
    Gen3ResourceCatalog_Add(catalog, "gen3:sample/bg/missing",
                            GEN3_RESOURCE_TYPE_BITMAP, 1u, true, NULL);
    Gen3ResourceCatalog_Finalize(catalog, NULL);
    *outCatalog = catalog;
}

static void FillProviderMetadata(struct Gen3ResourceProviderMetadata *metadata)
{
    memset(metadata, 0, sizeof(*metadata));
    metadata->id = PROVIDER_ID;
    metadata->version = PROVIDER_VER;
    metadata->kind = GEN3_PROVIDER_ROM_BASE;
    metadata->precedence = PROVIDER_PREC;
}

static struct Gen3ResourcePack *ParseBytes(const struct Gen3ResourcePackBytes *bytes)
{
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePack *pack = NULL;
    enum Gen3ResourcePackError error;
    Gen3ResourcePackDiagnostics_Init(&diag);
    error = Gen3ResourcePack_Parse(bytes->data, bytes->size, &pack, &diag);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    CHECK("valid synthetic pack parses", error == GEN3_PACK_OK && pack != NULL);
    return pack;
}

/* ------------------------------------------------------------------ */
/* 1. Happy path: provider built, metadata exposed, resolver resolves   */
/* ------------------------------------------------------------------ */

static void TestHappyPath(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider = NULL;
    struct Gen3ResourcePackProviderInfo info;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceView view;
    struct Gen3ResourcePackProfile profile;
    Gen3ResourceKey expectedKey;
    Gen3ResourceHandle handle;
    unsigned char *expectedTileset;
    unsigned char *expectedPalette;
    unsigned char tilesetSha[32];
    unsigned char paletteSha[32];
    enum Gen3ResourcePackProviderError error;

    expectedTileset = malloc(TILESET_SIZE);
    expectedPalette = malloc(PALETTE_SIZE);
    FillPayload(expectedTileset, TILESET_SIZE, 7u);
    FillPayload(expectedPalette, PALETTE_SIZE, 3u);
    Sha256BytesOf(expectedTileset, TILESET_SIZE, tilesetSha);
    Sha256BytesOf(expectedPalette, PALETTE_SIZE, paletteSha);

    BuildSyntheticPack(TILESET_NAME, PALETTE_NAME, TILESET_SIZE, PALETTE_SIZE, &bytes);
    pack = ParseBytes(&bytes);
    BuildGoodCatalog(&catalog);
    FillProviderMetadata(&metadata);
    Gen3ResourceDiagnostics_Init(&diag);

    error = Gen3ResourcePackProvider_CreateFromPack(pack, catalog, &metadata,
        &provider, &info, &diag);
    CHECK("happy path builds provider", error == GEN3_PACK_PROVIDER_OK);
    CHECK("provider non-NULL on success", provider != NULL);

    if (provider != NULL)
    {
        struct Gen3ResourceProviderMetadata got;
        CHECK("GetMetadata succeeds",
              Gen3ResourceProvider_GetMetadata(provider, &got));
        CHECK("metadata id preserved", strcmp(got.id, PROVIDER_ID) == 0);
        CHECK("metadata version preserved", strcmp(got.version, PROVIDER_VER) == 0);
        CHECK("metadata kind is ROM_BASE", got.kind == GEN3_PROVIDER_ROM_BASE);
        CHECK("metadata precedence preserved", got.precedence == PROVIDER_PREC);
    }

    CHECK("info basePackVersion", info.basePackVersion == 1u);
    CHECK("info catalogVersion", info.catalogVersion == 1u);
    CHECK("info extractionManifestVersion", info.extractionManifestVersion == 1u);
    CHECK("info canonicalRepresentationVersion",
          info.canonicalRepresentationVersion == 1u);
    CHECK("info gameId", strcmp(info.gameId, "sample") == 0);
    CHECK("info entryCount", info.entryCount == 2u);
    CHECK("info has provider digest", info.hasProviderContentDigest);
    CHECK("info has logical digest", info.hasLogicalContentDigest);
    CHECK("provider digest non-zero", !AllZero8(info.providerContentDigest, 32));
    CHECK("logical digest non-zero", !AllZero8(info.logicalContentDigest, 32));

    /* The provider's payloads must be byte-identical to the source material,
     * and the R2 profile must agree on the pack versions. */
    CHECK("pack profile readable",
          Gen3ResourcePack_GetProfile(pack, &profile));
    CHECK("profile base version matches info",
          profile.basePackVersion == info.basePackVersion);

    /* Resolve both resources through the normal resolver. */
    candidate = Gen3ResourceCandidate_Create(catalog, &diag);
    CHECK("candidate created", candidate != NULL);
    CHECK("provider added to candidate",
          Gen3ResourceCandidate_AddProvider(candidate, provider, &diag));
    CHECK("candidate builds snapshot",
          Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));
    CHECK("snapshot non-NULL", snapshot != NULL);

    if (snapshot != NULL)
    {
        Gen3ResourceId_DeriveKey(TILESET_NAME, &expectedKey);
        CHECK("tileset handle found",
              Gen3ResourceSnapshot_FindHandle(snapshot, TILESET_NAME, &handle)
                  == GEN3_RESOURCE_OK);
        CHECK("tileset resolves",
              Gen3ResourceSnapshot_Resolve(snapshot, handle,
                  GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u, &view) == GEN3_RESOURCE_OK);
        CHECK("view canonicalName",
              strcmp(view.canonicalName, TILESET_NAME) == 0);
        CHECK("view key matches derived",
              memcmp(view.key.bytes, expectedKey.bytes, GEN3_RESOURCE_KEY_SIZE) == 0);
        CHECK("view type", view.type == GEN3_RESOURCE_TYPE_TILE_GRAPHICS);
        CHECK("view schema", view.schema == 1u);
        CHECK("view payloadSize", view.payloadSize == TILESET_SIZE);
        CHECK("view payload bytes",
              memcmp(view.payload, expectedTileset, TILESET_SIZE) == 0);
        CHECK("view winner id", strcmp(view.winningProviderId, PROVIDER_ID) == 0);
        CHECK("view winner version", strcmp(view.winningProviderVersion, PROVIDER_VER) == 0);
        CHECK("view winner precedence", view.winningProviderPrecedence == PROVIDER_PREC);

        Gen3ResourceId_DeriveKey(PALETTE_NAME, &expectedKey);
        CHECK("palette handle found",
              Gen3ResourceSnapshot_FindHandle(snapshot, PALETTE_NAME, &handle)
                  == GEN3_RESOURCE_OK);
        CHECK("palette resolves",
              Gen3ResourceSnapshot_Resolve(snapshot, handle,
                  GEN3_RESOURCE_TYPE_PALETTE, 1u, &view) == GEN3_RESOURCE_OK);
        CHECK("palette view type", view.type == GEN3_RESOURCE_TYPE_PALETTE);
        CHECK("palette view payloadSize", view.payloadSize == PALETTE_SIZE);
        CHECK("palette view payload bytes",
              memcmp(view.payload, expectedPalette, PALETTE_SIZE) == 0);
        CHECK("palette winner id", strcmp(view.winningProviderId, PROVIDER_ID) == 0);

        Gen3ResourceSnapshot_Destroy(snapshot);
    }

    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceProvider_Destroy(provider);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourceDiagnostics_Destroy(&diag);
    free(expectedTileset);
    free(expectedPalette);
}

/* ------------------------------------------------------------------ */
/* 2. Payload ownership: provider survives pack destruction            */
/* ------------------------------------------------------------------ */

static void TestProviderOwnsPayloads(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider = NULL;
    struct Gen3ResourcePackProviderInfo info;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceView view;
    Gen3ResourceHandle handle;
    unsigned char *expectedPalette;
    enum Gen3ResourcePackProviderError error;

    expectedPalette = malloc(PALETTE_SIZE);
    FillPayload(expectedPalette, PALETTE_SIZE, 3u);

    BuildSyntheticPack(TILESET_NAME, PALETTE_NAME, TILESET_SIZE, PALETTE_SIZE, &bytes);
    pack = ParseBytes(&bytes);
    BuildGoodCatalog(&catalog);
    FillProviderMetadata(&metadata);
    Gen3ResourceDiagnostics_Init(&diag);

    error = Gen3ResourcePackProvider_CreateFromPack(pack, catalog, &metadata,
        &provider, &info, &diag);
    CHECK("adapter builds provider", error == GEN3_PACK_PROVIDER_OK);

    /* Destroy the source pack; the provider already owns immutable copies. */
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);

    /* Build the candidate + snapshot AFTER the pack is gone. */
    candidate = Gen3ResourceCandidate_Create(catalog, &diag);
    CHECK("candidate created", candidate != NULL);
    CHECK("provider added", Gen3ResourceCandidate_AddProvider(candidate, provider, &diag));
    CHECK("candidate builds after pack destroyed",
          Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));

    /* The snapshot also clones, so destroying the provider + candidate now must
     * leave the snapshot payloads valid too. */
    Gen3ResourceProvider_Destroy(provider);
    Gen3ResourceCandidate_Destroy(candidate);

    if (snapshot != NULL)
    {
        CHECK("palette resolves after all sources destroyed",
              Gen3ResourceSnapshot_FindHandle(snapshot, PALETTE_NAME, &handle)
                  == GEN3_RESOURCE_OK);
        CHECK("palette view after destruction",
              Gen3ResourceSnapshot_Resolve(snapshot, handle,
                  GEN3_RESOURCE_TYPE_PALETTE, 1u, &view) == GEN3_RESOURCE_OK);
        CHECK("palette payload intact",
              memcmp(view.payload, expectedPalette, PALETTE_SIZE) == 0);
        Gen3ResourceSnapshot_Destroy(snapshot);
    }

    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourceDiagnostics_Destroy(&diag);
    free(expectedPalette);
}

/* ------------------------------------------------------------------ */
/* 3. Empty pack refusal                                               */
/* ------------------------------------------------------------------ */

static void TestEmptyPackRejected(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider = (struct Gen3ResourceProvider *)0x1;
    struct Gen3ResourcePackProviderInfo info;
    struct Gen3ResourceDiagnosticList diag;
    enum Gen3ResourcePackProviderError error;

    BuildEmptyPack(&bytes);
    pack = ParseBytes(&bytes);
    BuildGoodCatalog(&catalog);
    FillProviderMetadata(&metadata);
    Gen3ResourceDiagnostics_Init(&diag);

    error = Gen3ResourcePackProvider_CreateFromPack(pack, catalog, &metadata,
        &provider, &info, &diag);
    CHECK("empty pack refused", error == GEN3_PACK_PROVIDER_ERR_NO_ENTRIES);
    CHECK("outProvider reset to NULL on failure", provider == NULL);

    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourceDiagnostics_Destroy(&diag);
}

/* ------------------------------------------------------------------ */
/* 4. Fail-closed catalog gate                                         */
/* ------------------------------------------------------------------ */

static void ExpectCatalogMismatch(const char *label,
                                  struct Gen3ResourceCatalog *catalog,
                                  enum Gen3ResourceReason expectedReason,
                                  const char *expectedResource)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider = NULL;
    struct Gen3ResourcePackProviderInfo info;
    struct Gen3ResourceDiagnosticList diag;
    enum Gen3ResourcePackProviderError error;

    BuildSyntheticPack(TILESET_NAME, PALETTE_NAME, TILESET_SIZE, PALETTE_SIZE, &bytes);
    pack = ParseBytes(&bytes);
    FillProviderMetadata(&metadata);
    Gen3ResourceDiagnostics_Init(&diag);

    error = Gen3ResourcePackProvider_CreateFromPack(pack, catalog, &metadata,
        &provider, &info, &diag);
    CHECK(label, error == GEN3_PACK_PROVIDER_ERR_CATALOG_MISMATCH);
    CHECK("no provider on mismatch", provider == NULL);
    CHECK("structured diagnostic emitted",
          diag.count == 1u
              && diag.items[0].severity == GEN3_DIAGNOSTIC_ERROR);
    if (diag.count == 1u)
    {
        CHECK("diagnostic reason", diag.items[0].reason == expectedReason);
        CHECK("diagnostic resource", strcmp(diag.items[0].resourceName, expectedResource) == 0);
        CHECK("diagnostic provider id", strcmp(diag.items[0].providerId, PROVIDER_ID) == 0);
        CHECK("diagnostic entry required",
              diag.items[0].providerEntryRequired);
    }

    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourceDiagnostics_Destroy(&diag);
}

static void TestCatalogGate(void)
{
    struct Gen3ResourceCatalog *catalog;

    /* Pack keys are correct, but the catalog derives mutated keys. The first
     * entry ValidateCatalog inspects is the palette (sorted name order); the
     * palette key mismatches, so the palette is the reported resource. */
    BuildMutatedKeyCatalog(&catalog);
    ExpectCatalogMismatch("key mismatch",
                          catalog, GEN3_RESOURCE_REASON_RESOURCE_KEY_COLLISION,
                          PALETTE_NAME);

    BuildCatalogWithType(&catalog);
    ExpectCatalogMismatch("type mismatch",
                          catalog, GEN3_RESOURCE_REASON_TYPE_MISMATCH,
                          TILESET_NAME);

    BuildCatalogWithSchema(&catalog);
    ExpectCatalogMismatch("schema mismatch",
                          catalog, GEN3_RESOURCE_REASON_SCHEMA_MISMATCH,
                          TILESET_NAME);

    BuildCatalogMissingTileset(&catalog);
    ExpectCatalogMismatch("unknown resource",
                          catalog, GEN3_RESOURCE_REASON_UNKNOWN_RESOURCE,
                          TILESET_NAME);
}

/* ------------------------------------------------------------------ */
/* 5. Invalid arguments                                                */
/* ------------------------------------------------------------------ */

static void TestInvalidArguments(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider = NULL;
    struct Gen3ResourcePackProviderInfo info;
    struct Gen3ResourceDiagnosticList diag;
    enum Gen3ResourcePackProviderError error;

    BuildSyntheticPack(TILESET_NAME, PALETTE_NAME, TILESET_SIZE, PALETTE_SIZE, &bytes);
    pack = ParseBytes(&bytes);
    BuildGoodCatalog(&catalog);
    FillProviderMetadata(&metadata);
    Gen3ResourceDiagnostics_Init(&diag);

    error = Gen3ResourcePackProvider_CreateFromPack(NULL, catalog, &metadata,
        &provider, &info, &diag);
    CHECK("NULL pack rejected", error == GEN3_PACK_PROVIDER_ERR_INVALID_ARGUMENT);

    error = Gen3ResourcePackProvider_CreateFromPack(pack, NULL, &metadata,
        &provider, &info, &diag);
    CHECK("NULL catalog rejected", error == GEN3_PACK_PROVIDER_ERR_INVALID_ARGUMENT);

    error = Gen3ResourcePackProvider_CreateFromPack(pack, catalog, NULL,
        &provider, &info, &diag);
    CHECK("NULL metadata rejected", error == GEN3_PACK_PROVIDER_ERR_INVALID_ARGUMENT);

    error = Gen3ResourcePackProvider_CreateFromPack(pack, catalog, &metadata,
        NULL, &info, &diag);
    CHECK("NULL outProvider rejected", error == GEN3_PACK_PROVIDER_ERR_INVALID_ARGUMENT);

    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourceDiagnostics_Destroy(&diag);
}

/* ------------------------------------------------------------------ */
/* 6. Unvalidated pack is impossible                                   */
/* ------------------------------------------------------------------ */

static void TestUnvalidatedPackImpossible(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePack *pack = NULL;
    unsigned char *mutated;
    uint64_t payloadOffset;
    enum Gen3ResourcePackError error;

    /* Garbage bytes must never yield a pack. */
    static const unsigned char garbage[64] = { 0xDE, 0xAD, 0xBE, 0xEF };
    Gen3ResourcePackDiagnostics_Init(&diag);
    error = Gen3ResourcePack_Parse(garbage, sizeof(garbage), &pack, &diag);
    CHECK("garbage rejected by the R2 reader", error != GEN3_PACK_OK);
    CHECK("no pack from garbage", pack == NULL);
    Gen3ResourcePackDiagnostics_Destroy(&diag);

    /* A valid pack with one payload byte flipped must also be rejected. */
    BuildSyntheticPack(TILESET_NAME, PALETTE_NAME, TILESET_SIZE, PALETTE_SIZE, &bytes);
    mutated = malloc(bytes.size);
    memcpy(mutated, bytes.data, bytes.size);
    /* Independent LE read of the header's payload offset (v1 layout). */
    payloadOffset = (uint64_t)mutated[104]
                  | ((uint64_t)mutated[105] << 8u)
                  | ((uint64_t)mutated[106] << 16u)
                  | ((uint64_t)mutated[107] << 24u)
                  | ((uint64_t)mutated[108] << 32u)
                  | ((uint64_t)mutated[109] << 40u)
                  | ((uint64_t)mutated[110] << 48u)
                  | ((uint64_t)mutated[111] << 56u);
    mutated[payloadOffset] ^= 0x01u;
    Gen3ResourcePackDiagnostics_Init(&diag);
    error = Gen3ResourcePack_Parse(mutated, bytes.size, &pack, &diag);
    CHECK("payload-corrupted pack rejected",
          error == GEN3_PACK_ERR_PAYLOAD_HASH_MISMATCH);
    CHECK("no pack from corrupt image", pack == NULL);
    Gen3ResourcePackDiagnostics_Destroy(&diag);

    Gen3ResourcePackBytes_Destroy(&bytes);
    free(mutated);
}

/* ------------------------------------------------------------------ */
/* 7. Malformed requiredForProvider entry fails the candidate          */
/* ------------------------------------------------------------------ */

static void TestMalformedRequiredProviderEntry(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider = NULL;
    struct Gen3ResourcePackProviderInfo info;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    enum Gen3ResourcePackProviderError error;

    /* tile-graphics with a non-multiple-of-32 payload: the writer accepts it
     * (it does not validate shape), the R2 reader accepts it (hashes pass), and
     * ValidateCatalog accepts it (name/key/type/schema all match). Only the M0/M1
     * candidate build detects the malformed payload - and because the entry was
     * added requiredForProvider, it must fail the candidate. */
    BuildSyntheticPack(TILESET_NAME, PALETTE_NAME, 33u, PALETTE_SIZE, &bytes);
    pack = ParseBytes(&bytes);
    BuildGoodCatalog(&catalog);
    FillProviderMetadata(&metadata);
    Gen3ResourceDiagnostics_Init(&diag);

    error = Gen3ResourcePackProvider_CreateFromPack(pack, catalog, &metadata,
        &provider, &info, &diag);
    CHECK("malformed-but-consistent entry builds a provider",
          error == GEN3_PACK_PROVIDER_OK);

    candidate = Gen3ResourceCandidate_Create(catalog, &diag);
    CHECK("candidate created", candidate != NULL);
    CHECK("provider added", Gen3ResourceCandidate_AddProvider(candidate, provider, &diag));
    CHECK("candidate REJECTS malformed required entry",
          !Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));
    CHECK("no snapshot from invalid candidate", snapshot == NULL);

    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceProvider_Destroy(provider);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourceDiagnostics_Destroy(&diag);
}

/* ------------------------------------------------------------------ */
/* 8. Missing requiredForBase resource fails the candidate             */
/* ------------------------------------------------------------------ */

static void TestMissingRequiredBase(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider = NULL;
    struct Gen3ResourcePackProviderInfo info;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    enum Gen3ResourcePackProviderError error;

    BuildSyntheticPack(TILESET_NAME, PALETTE_NAME, TILESET_SIZE, PALETTE_SIZE, &bytes);
    pack = ParseBytes(&bytes);
    BuildCatalogWithExtraBase(&catalog);
    FillProviderMetadata(&metadata);
    Gen3ResourceDiagnostics_Init(&diag);

    /* The adapter only checks pack entries against the catalog, so a catalog
     * resource the pack does not carry is not the adapter's concern. */
    error = Gen3ResourcePackProvider_CreateFromPack(pack, catalog, &metadata,
        &provider, &info, &diag);
    CHECK("provider builds when a catalog base is absent from the pack",
          error == GEN3_PACK_PROVIDER_OK);

    /* The candidate build enforces requiredForBase: the missing resource has no
     * valid base implementation, so publication fails closed. */
    candidate = Gen3ResourceCandidate_Create(catalog, &diag);
    CHECK("candidate created", candidate != NULL);
    CHECK("provider added", Gen3ResourceCandidate_AddProvider(candidate, provider, &diag));
    CHECK("candidate REJECTS missing required base",
          !Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));
    CHECK("no snapshot from missing-base candidate", snapshot == NULL);

    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceProvider_Destroy(provider);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourceDiagnostics_Destroy(&diag);
}

/* ------------------------------------------------------------------ */
/* 9. Invalid catalog inputs can never reach a provider                */
/* ------------------------------------------------------------------ */

static void TestCatalogInputGuards(void)
{
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceDiagnosticList diag;

    Gen3ResourceDiagnostics_Init(&diag);

    /* Duplicate resource name: the second add must be refused before the
     * catalog can ever be finalized, so a duplicate-resource catalog cannot
     * silently produce a working provider. */
    catalog = Gen3ResourceCatalog_Create();
    CHECK("first catalog add ok",
          Gen3ResourceCatalog_Add(catalog, TILESET_NAME,
              GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u, true, &diag));
    CHECK("duplicate catalog name refused",
          !Gen3ResourceCatalog_Add(catalog, TILESET_NAME,
              GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u, true, &diag));
    CHECK("duplicate-name diagnostic",
          diag.count == 1u
              && diag.items[0].reason == GEN3_RESOURCE_REASON_DUPLICATE_CATALOG_NAME);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourceDiagnostics_Destroy(&diag);
    Gen3ResourceDiagnostics_Init(&diag);

    /* Unsupported type: an out-of-range/invalid type code is refused at the
     * catalog boundary, so no provider input can ever carry it. */
    catalog = Gen3ResourceCatalog_Create();
    CHECK("invalid type refused",
          !Gen3ResourceCatalog_Add(catalog, TILESET_NAME,
              GEN3_RESOURCE_TYPE_INVALID, 1u, true, &diag));
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourceDiagnostics_Destroy(&diag);
}

int main(void)
{
    TestHappyPath();
    TestProviderOwnsPayloads();
    TestEmptyPackRejected();
    TestCatalogGate();
    TestInvalidArguments();
    TestUnvalidatedPackImpossible();
    TestMalformedRequiredProviderEntry();
    TestMissingRequiredBase();
    TestCatalogInputGuards();

    if (gFailures != 0)
    {
        printf("resource_pack_provider_test FAILED: %d/%d checks\n",
               gFailures, gChecks);
        return 1;
    }
    printf("resource pack provider test passed (%d checks)\n", gChecks);
    return 0;
}

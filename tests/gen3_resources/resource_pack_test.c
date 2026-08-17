/* R2: Gen3 resource-pack codec tests.
 *
 * Covers the deterministic writer, the strict reader/validator, the logical and
 * provider-content digests, the corruption matrix (A-AF), catalog validation,
 * and the thin file helper. Everything is platform-neutral: no Emerald, GBA,
 * SDL, ROM_BASE, or frontend symbols.
 *
 * The fixed digest vectors (EXPECTED_*) were produced by the INDEPENDENT
 * reference at tests/gen3_resources/resource_pack_vectors.py, so the C codec is
 * verified against the documented v1 format rather than against itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_content_digest.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_pack_writer.h"

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

/* ------------------------------------------------------------------ */
/* Hex helpers                                                          */
/* ------------------------------------------------------------------ */

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

/* Independent little-endian decoders. The test verifies the on-disk layout
 * against the documented GEN3_PACK_OFF_* offsets with its own decoding rather
 * than reaching into the codec's internal byte helpers. */
static uint32_t ReadLE32(const unsigned char *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8u)
         | ((uint32_t)p[2] << 16u)
         | ((uint32_t)p[3] << 24u);
}

static uint64_t ReadLE64(const unsigned char *p)
{
    uint64_t value = 0;
    size_t i;
    for (i = 0; i < 8u; i++)
        value |= (uint64_t)p[i] << (8u * i);
    return value;
}

static bool AllZero(const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i] != 0u)
            return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* Fixed fixture inputs (identical to the Python reference)             */
/* ------------------------------------------------------------------ */

#define FIXTURE_SHEET_NAME   "emerald:trainer/brendan/battle/front/sheet"
#define FIXTURE_PALETTE_NAME "emerald:trainer/brendan/battle/front/normal-palette"

static const char kSheetShaHex[] =
    "7980642dbfcc34a81e8dc2ab3344fe5782df444df310f07ca4cc1904fc773fed";
static const char kPaletteShaHex[] =
    "427c4cdfcacf83c8292714515c85ffe3afdab23c883431f7f60fea0738992189";
static const char kCatalogShaHex[] =
    "472f8fd1c9c824d8eb30a0a553a483699be52256a94a7ed16b92a2699db80bb9";
static const char kManifestShaHex[] =
    "fbdaba5ceed53da4fb0cbf375b2178250252acc4fe38f4ea876de5ffbf0bc475";
static const char kRomSha1Hex[] =
    "092ee293c6e4381074682375249561bbc41bec44";
static const char kRomSha256Hex[] =
    "e4c8f3693d840fb233dfa31c1cd2b4d21aa2dde67359e4aad4b064a4b1b33bca";
static const char kSheetSourceShaHex[] =
    "c5fca4e037ab22c2c24bce1f0cf0c2b35679c3a1cf64b6e64dedbd26b3cebfde";
static const char kPaletteSourceShaHex[] =
    "7cb7654625eabaa37dbcc96f116ca3ac64b6a4424ec6f8bf1117e9ddcaabd18f";
static const char kSheetKeyHex[] =
    "95821423a175034d3b872a3d3587200ceecc20ec81e258a83ee0bc477849b211";
static const char kPaletteKeyHex[] =
    "6d7aec37ceb0faad6153f5c11380143c4fd6c033aa69599996e9ad0add670d56";

static const char kTocShaHex[] =
    "2f83ec65832a6d513a19a047bebae87aa8e059685ec07ab052d7e0e02c5ed086";
static const char kNamesShaHex[] =
    "4366e64bc3ef607c1e9503814f6fc4282596af244b52b16bf36c7a811c5a887e";
static const char kPayloadShaHex[] =
    "7f637bc1732fbb8201afc8de4a49436bc205f0b93706bda28cae360e2b27b0c1";
static const char kLogicalShaHex[] =
    "514e911d474d28e2466892e4e360f5799b78c8034b22339b333dc5003746a99c";
static const char kProviderShaHex[] =
    "70250913fc8635221bf63bad3710f41e6e3e9b92447188a383ccc685521f1722";
static const char kHeaderShaHex[] =
    "3bfcb9db8525c51ae88ae842ae32207e58977afaa66cf16a1268d4082652bcd3";

static void MakeFixturePayloads(unsigned char **outSheet, unsigned char **outPalette)
{
    size_t i;
    unsigned char *sheet = malloc(2048u);
    unsigned char *palette = malloc(32u);
    for (i = 0; i < 2048u; i++)
        sheet[i] = (unsigned char)((i * 7u + (i >> 3u)) & 0xFFu);
    for (i = 0; i < 32u; i++)
        palette[i] = (unsigned char)((i * 3u + 1u) & 0xFFu);
    *outSheet = sheet;
    *outPalette = palette;
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

static void FillHashes(struct FixtureHashes *hashes)
{
    FromHex(hashes->romSha1, kRomSha1Hex);
    FromHex(hashes->romSha256, kRomSha256Hex);
    FromHex(hashes->catalog, kCatalogShaHex);
    FromHex(hashes->manifest, kManifestShaHex);
    memset(hashes->gameId, 0, sizeof(hashes->gameId));
    memcpy(hashes->gameId, "emerald", 7u);
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
    memcpy(profile->gameCode, "BPEE", 4u);
    memcpy(profile->makerCode, "01", 2u);
    profile->softwareRevision = 0u;
    memcpy(profile->gameId, hashes->gameId, GEN3_PACK_GAME_ID_SIZE);
    profile->catalogSha256 = hashes->catalog;
    profile->extractionManifestSha256 = hashes->manifest;
}

/* Builds the fixed two-resource pack and returns a heap copy of its bytes. */
static void BuildFixtureBytes(unsigned char **outBytes, size_t *outSize)
{
    unsigned char *sheet;
    unsigned char *palette;
    unsigned char sheetSha[GEN3_PACK_SHA256_SIZE];
    unsigned char paletteSha[GEN3_PACK_SHA256_SIZE];
    unsigned char sheetSrc[GEN3_PACK_SHA256_SIZE];
    unsigned char paletteSrc[GEN3_PACK_SHA256_SIZE];
    unsigned char sheetKey[GEN3_RESOURCE_KEY_SIZE];
    unsigned char paletteKey[GEN3_RESOURCE_KEY_SIZE];
    Gen3ResourceKey sheetKeyStruct;
    Gen3ResourceKey paletteKeyStruct;
    struct FixtureHashes hashes;
    struct Gen3ResourcePackProfileInput profile;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackBuild *build;
    unsigned char *result;

    MakeFixturePayloads(&sheet, &palette);
    FromHex(sheetSha, kSheetShaHex);
    FromHex(paletteSha, kPaletteShaHex);
    FromHex(sheetSrc, kSheetSourceShaHex);
    FromHex(paletteSrc, kPaletteSourceShaHex);
    FromHex(sheetKey, kSheetKeyHex);
    FromHex(paletteKey, kPaletteKeyHex);
    memcpy(&sheetKeyStruct.bytes, sheetKey, GEN3_RESOURCE_KEY_SIZE);
    memcpy(&paletteKeyStruct.bytes, paletteKey, GEN3_RESOURCE_KEY_SIZE);

    Gen3ResourcePackDiagnostics_Init(&diag);
    FillHashes(&hashes);
    FillProfile(&profile, &hashes);

    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
    {
        printf("FAIL: BuildFixtureBytes: out of memory\n");
        exit(1);
    }
    if (Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildFixtureBytes: SetProfile\n");
        exit(1);
    }

    memset(&entry, 0, sizeof(entry));
    entry.canonicalName = FIXTURE_PALETTE_NAME;
    entry.key = &paletteKeyStruct;
    entry.type = GEN3_RESOURCE_TYPE_PALETTE;
    entry.schema = 1u;
    entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
    entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
    entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
    entry.canonicalPayload = palette;
    entry.canonicalPayloadSize = 32u;
    entry.canonicalPayloadSha256 = paletteSha;
    entry.sourceRomOffset = 0x310000u;
    entry.sourceEncodedSize = 40u;
    entry.sourceEncodedSha256 = paletteSrc;
    if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildFixtureBytes: AddEntry palette\n");
        exit(1);
    }

    entry.canonicalName = FIXTURE_SHEET_NAME;
    entry.key = &sheetKeyStruct;
    entry.type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    entry.canonicalPayload = sheet;
    entry.canonicalPayloadSize = 2048u;
    entry.canonicalPayloadSha256 = sheetSha;
    entry.sourceRomOffset = 0x300000u;
    entry.sourceEncodedSize = 804u;
    entry.sourceEncodedSha256 = sheetSrc;
    if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildFixtureBytes: AddEntry sheet\n");
        exit(1);
    }

    memset(&bytes, 0, sizeof(bytes));
    if (Gen3ResourcePackWriter_Write(build, &bytes, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildFixtureBytes: Write\n");
        exit(1);
    }

    result = malloc(bytes.size);
    if (result == NULL)
    {
        printf("FAIL: BuildFixtureBytes: malloc\n");
        exit(1);
    }
    memcpy(result, bytes.data, bytes.size);
    *outBytes = result;
    *outSize = bytes.size;

    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    free(sheet);
    free(palette);
}

/* Parse a byte image and assert the exact expected result. */
static void ExpectParse(const unsigned char *bytes, size_t size,
                        enum Gen3ResourcePackError expected,
                        const char *label)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList diag;
    enum Gen3ResourcePackError error;

    Gen3ResourcePackDiagnostics_Init(&diag);
    error = Gen3ResourcePack_Parse(bytes, size, &pack, &diag);
    gChecks++;
    if (error != expected)
    {
        printf("FAIL [%s]: expected %d (%s), got %d (%s)\n", label,
               (int)expected, Gen3ResourcePackError_Describe(expected),
               (int)error, Gen3ResourcePackError_Describe(error));
        gFailures++;
    }
    if (error != GEN3_PACK_OK)
    {
        gChecks++;
        if (!(diag.count >= 1u && diag.items[0].error == error))
        {
            printf("FAIL [%s]: diagnostics did not report the error\n", label);
            gFailures++;
        }
    }
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
}

/* Patch length bytes at offset within a copy. */
static void Patch(unsigned char *bytes, size_t size, size_t offset,
                  const unsigned char *value, size_t length)
{
    if (offset + length > size)
    {
        printf("FAIL: Patch out of range\n");
        exit(1);
    }
    memcpy(bytes + offset, value, length);
}

static void PatchU32(unsigned char *bytes, size_t size, size_t offset, uint32_t value)
{
    unsigned char buf[4];
    buf[0] = (unsigned char)(value & 0xFFu);
    buf[1] = (unsigned char)((value >> 8u) & 0xFFu);
    buf[2] = (unsigned char)((value >> 16u) & 0xFFu);
    buf[3] = (unsigned char)((value >> 24u) & 0xFFu);
    Patch(bytes, size, offset, buf, 4u);
}

static void PatchU64(unsigned char *bytes, size_t size, size_t offset, uint64_t value)
{
    unsigned char buf[8];
    size_t i;
    for (i = 0; i < 8u; i++)
        buf[i] = (unsigned char)((value >> (8u * i)) & 0xFFu);
    Patch(bytes, size, offset, buf, 8u);
}

static void PatchZero(unsigned char *bytes, size_t size, size_t offset, size_t length)
{
    static const unsigned char zeros[64] = {0};
    size_t done;
    for (done = 0; done < length;)
    {
        size_t chunk = length - done < sizeof(zeros) ? length - done : sizeof(zeros);
        Patch(bytes, size, offset + done, zeros, chunk);
        done += chunk;
    }
}

/* ------------------------------------------------------------------ */
/* Test 1: writer output matches the independent fixed vectors          */
/* ------------------------------------------------------------------ */

static void TestFixedVectors(void)
{
    unsigned char *bytes;
    size_t size;
    static const struct
    {
        size_t offset;
        const char *hex;
    } fields[] =
    {
        { GEN3_PACK_OFF_TOC_SHA256, kTocShaHex },
        { GEN3_PACK_OFF_NAMES_SHA256, kNamesShaHex },
        { GEN3_PACK_OFF_PAYLOAD_SHA256, kPayloadShaHex },
        { GEN3_PACK_OFF_LOGICAL_SHA256, kLogicalShaHex },
        { GEN3_PACK_OFF_HEADER_SHA256, kHeaderShaHex },
    };
    size_t i;
    unsigned char expected[GEN3_PACK_SHA256_SIZE];

    BuildFixtureBytes(&bytes, &size);
    CHECK(size == 2944u);
    CHECK(ReadLE64(bytes + GEN3_PACK_OFF_FILE_SIZE) == (uint64_t)size);
    CHECK(ReadLE64(bytes + GEN3_PACK_OFF_TOC_OFFSET) == 448u);
    CHECK(ReadLE64(bytes + GEN3_PACK_OFF_NAMES_OFFSET) == 768u);
    CHECK(ReadLE64(bytes + GEN3_PACK_OFF_NAMES_SIZE) == 93u);
    CHECK(ReadLE64(bytes + GEN3_PACK_OFF_PAYLOAD_OFFSET) == 864u);
    CHECK(ReadLE64(bytes + GEN3_PACK_OFF_PAYLOAD_SIZE) == 2080u);
    CHECK(ReadLE32(bytes + GEN3_PACK_OFF_FORMAT_VERSION) == 1u);
    CHECK(ReadLE32(bytes + GEN3_PACK_OFF_HEADER_SIZE) == GEN3_PACK_HEADER_SIZE);
    CHECK(ReadLE32(bytes + GEN3_PACK_OFF_ENTRY_SIZE) == GEN3_PACK_ENTRY_SIZE);
    CHECK(ReadLE32(bytes + GEN3_PACK_OFF_ENTRY_COUNT) == 2u);

    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
    {
        FromHex(expected, fields[i].hex);
        gChecks++;
        if (memcmp(bytes + fields[i].offset, expected, GEN3_PACK_SHA256_SIZE) != 0)
        {
            printf("FAIL: fixed vector mismatch at offset %zu\n", fields[i].offset);
            gFailures++;
        }
    }

    /* Reserved bytes are all zero. */
    CHECK(ReadLE32(bytes + GEN3_PACK_OFF_RESERVED0) == 0u);
    CHECK(bytes[GEN3_PACK_OFF_RESERVED1] == 0u);
    CHECK(AllZero(bytes + GEN3_PACK_OFF_RESERVED2, GEN3_PACK_RESERVED2_SIZE));
    CHECK(AllZero(bytes + 448u + 2u * GEN3_PACK_ENTRY_SIZE + 93u,
                  864u - (448u + 2u * GEN3_PACK_ENTRY_SIZE + 93u)));

    free(bytes);
}

/* ------------------------------------------------------------------ */
/* Test 2: round-trip read API                                         */
/* ------------------------------------------------------------------ */

static void TestRoundTrip(void)
{
    unsigned char *bytes;
    size_t size;
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList diag;
    const struct Gen3ResourcePackEntry *entry;
    struct Gen3ResourcePackProfile profile;
    unsigned char digest[GEN3_PACK_SHA256_SIZE];
    unsigned char expected[GEN3_PACK_SHA256_SIZE];
    Gen3ResourceKey sheetKey;
    unsigned char sheetKeyBytes[GEN3_RESOURCE_KEY_SIZE];

    BuildFixtureBytes(&bytes, &size);
    Gen3ResourcePackDiagnostics_Init(&diag);
    CHECK(Gen3ResourcePack_Parse(bytes, size, &pack, &diag) == GEN3_PACK_OK);
    CHECK(pack != NULL);

    CHECK(Gen3ResourcePack_GetEntryCount(pack) == 2u);
    entry = Gen3ResourcePack_GetEntry(pack, 0u);
    CHECK(entry != NULL);
    CHECK(strcmp(entry->canonicalName, FIXTURE_PALETTE_NAME) == 0);
    CHECK(entry->type == GEN3_RESOURCE_TYPE_PALETTE);
    CHECK(entry->schema == 1u);
    CHECK(entry->payloadSize == 32u);
    CHECK(entry->sourceRomOffset == 0x310000u);
    CHECK(entry->sourceEncodedSize == 40u);
    CHECK(entry->representation == GEN3_PACK_REPRESENTATION_DECODED);
    CHECK(entry->sourceEncoding == GEN3_PACK_SOURCE_ENCODING_GBA_LZ77);
    CHECK((entry->flags & GEN3_PACK_FLAG_REQUIRED_FOR_BASE) != 0u);
    CHECK(memcmp(entry->payload, bytes + 864u, 32u) == 0);

    entry = Gen3ResourcePack_GetEntry(pack, 1u);
    CHECK(entry != NULL);
    CHECK(strcmp(entry->canonicalName, FIXTURE_SHEET_NAME) == 0);
    CHECK(entry->type == GEN3_RESOURCE_TYPE_TILE_GRAPHICS);
    CHECK(entry->payloadSize == 2048u);
    CHECK(entry->sourceRomOffset == 0x300000u);
    CHECK(entry->sourceEncodedSize == 804u);

    CHECK(Gen3ResourcePack_GetEntry(pack, 2u) == NULL);

    entry = Gen3ResourcePack_FindByCanonicalName(pack, FIXTURE_SHEET_NAME);
    CHECK(entry != NULL && entry->payloadSize == 2048u);
    entry = Gen3ResourcePack_FindByCanonicalName(pack, "mod:does/not/exist");
    CHECK(entry == NULL);

    FromHex(sheetKeyBytes, kSheetKeyHex);
    memcpy(&sheetKey.bytes, sheetKeyBytes, GEN3_RESOURCE_KEY_SIZE);
    entry = Gen3ResourcePack_FindByKey(pack, &sheetKey);
    CHECK(entry != NULL && strcmp(entry->canonicalName, FIXTURE_SHEET_NAME) == 0);

    CHECK(Gen3ResourcePack_GetProfile(pack, &profile));
    CHECK(profile.basePackVersion == 1u);
    CHECK(profile.sourceRomSize == 16u * 1024u * 1024u);
    CHECK(memcmp(profile.gameCode, "BPEE", 4u) == 0);
    CHECK(memcmp(profile.gameId, "emerald", 7u) == 0);
    CHECK(profile.gameId[7] == 0u);

    CHECK(Gen3ResourcePack_GetLogicalDigest(pack, digest));
    FromHex(expected, kLogicalShaHex);
    CHECK(memcmp(digest, expected, GEN3_PACK_SHA256_SIZE) == 0);

    CHECK(Gen3ResourcePack_GetProviderDigest(pack, digest));
    FromHex(expected, kProviderShaHex);
    CHECK(memcmp(digest, expected, GEN3_PACK_SHA256_SIZE) == 0);

    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    free(bytes);
}

/* ------------------------------------------------------------------ */
/* Test 3: insertion-order independence + digest input-order independence */
/* ------------------------------------------------------------------ */

static void TestInsertionOrderIndependence(void)
{
    unsigned char *first;
    size_t firstSize;
    unsigned char *second;
    size_t secondSize;
    unsigned char sheet[2048];
    unsigned char palette[32];
    unsigned char sheetSha[GEN3_PACK_SHA256_SIZE];
    unsigned char paletteSha[GEN3_PACK_SHA256_SIZE];
    unsigned char sheetSrc[GEN3_PACK_SHA256_SIZE];
    unsigned char paletteSrc[GEN3_PACK_SHA256_SIZE];
    unsigned char sheetKey[GEN3_RESOURCE_KEY_SIZE];
    unsigned char paletteKey[GEN3_RESOURCE_KEY_SIZE];
    Gen3ResourceKey sheetKeyStruct;
    Gen3ResourceKey paletteKeyStruct;
    struct FixtureHashes hashes;
    struct Gen3ResourcePackProfileInput profile;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackBuild *build;
    unsigned char expected[GEN3_PACK_SHA256_SIZE];
    size_t i;

    /* Reference: writer A inserts palette then sheet (already covered by
     * BuildFixtureBytes). Writer B inserts sheet then palette. */
    BuildFixtureBytes(&first, &firstSize);
    for (i = 0; i < 2048u; i++)
        sheet[i] = (unsigned char)((i * 7u + (i >> 3u)) & 0xFFu);
    for (i = 0; i < 32u; i++)
        palette[i] = (unsigned char)((i * 3u + 1u) & 0xFFu);
    FromHex(sheetSha, kSheetShaHex);
    FromHex(paletteSha, kPaletteShaHex);
    FromHex(sheetSrc, kSheetSourceShaHex);
    FromHex(paletteSrc, kPaletteSourceShaHex);
    FromHex(sheetKey, kSheetKeyHex);
    FromHex(paletteKey, kPaletteKeyHex);
    memcpy(&sheetKeyStruct.bytes, sheetKey, GEN3_RESOURCE_KEY_SIZE);
    memcpy(&paletteKeyStruct.bytes, paletteKey, GEN3_RESOURCE_KEY_SIZE);

    Gen3ResourcePackDiagnostics_Init(&diag);
    FillHashes(&hashes);
    FillProfile(&profile, &hashes);
    build = Gen3ResourcePackBuild_Create();
    CHECK(build != NULL);
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);

    memset(&entry, 0, sizeof(entry));
    entry.canonicalName = FIXTURE_SHEET_NAME;
    entry.key = &sheetKeyStruct;
    entry.type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    entry.schema = 1u;
    entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
    entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
    entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
    entry.canonicalPayload = sheet;
    entry.canonicalPayloadSize = sizeof(sheet);
    entry.canonicalPayloadSha256 = sheetSha;
    entry.sourceRomOffset = 0x300000u;
    entry.sourceEncodedSize = 804u;
    entry.sourceEncodedSha256 = sheetSrc;
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_OK);

    entry.canonicalName = FIXTURE_PALETTE_NAME;
    entry.key = &paletteKeyStruct;
    entry.type = GEN3_RESOURCE_TYPE_PALETTE;
    entry.canonicalPayload = palette;
    entry.canonicalPayloadSize = sizeof(palette);
    entry.canonicalPayloadSha256 = paletteSha;
    entry.sourceRomOffset = 0x310000u;
    entry.sourceEncodedSize = 40u;
    entry.sourceEncodedSha256 = paletteSrc;
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_OK);

    memset(&bytes, 0, sizeof(bytes));
    CHECK(Gen3ResourcePackWriter_Write(build, &bytes, &diag) == GEN3_PACK_OK);
    second = malloc(bytes.size);
    CHECK(second != NULL);
    secondSize = bytes.size;
    memcpy(second, bytes.data, bytes.size);

    CHECK(firstSize == secondSize);
    CHECK(memcmp(first, second, firstSize) == 0);

    /* Provider-content digest: input order must not matter. */
    {
        struct Gen3ResourceContentRecord records[2];
        unsigned char a[GEN3_PACK_SHA256_SIZE];
        unsigned char b[GEN3_PACK_SHA256_SIZE];
        records[0].key = paletteKeyStruct;
        records[0].type = GEN3_RESOURCE_TYPE_PALETTE;
        records[0].schema = 1u;
        records[0].payloadSize = 32u;
        records[0].payloadSha256 = paletteSha;
        records[1].key = sheetKeyStruct;
        records[1].type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
        records[1].schema = 1u;
        records[1].payloadSize = 2048u;
        records[1].payloadSha256 = sheetSha;
        CHECK(Gen3ResourceContentDigest_Provider(records, 2u, a));
        {
            struct Gen3ResourceContentRecord reversed[2];
            reversed[0] = records[1];
            reversed[1] = records[0];
            CHECK(Gen3ResourceContentDigest_Provider(reversed, 2u, b));
            CHECK(memcmp(a, b, GEN3_PACK_SHA256_SIZE) == 0);
        }
        FromHex(expected, kProviderShaHex);
        CHECK(memcmp(a, expected, GEN3_PACK_SHA256_SIZE) == 0);
    }

    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    free(first);
    free(second);
}

/* ------------------------------------------------------------------ */
/* Test 4: corruption matrix A-AF                                       */
/* ------------------------------------------------------------------ */

static void TestCorruptionMatrix(void)
{
    unsigned char *base;
    size_t size;
    unsigned char *copy;

    BuildFixtureBytes(&base, &size);

    /* A: bad magic. */
    copy = malloc(size);
    memcpy(copy, base, size);
    copy[0] = 'X';
    ExpectParse(copy, size, GEN3_PACK_ERR_BAD_MAGIC, "A bad magic");
    free(copy);

    /* B: wrong header size. */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchU32(copy, size, GEN3_PACK_OFF_HEADER_SIZE, GEN3_PACK_HEADER_SIZE + 1u);
    ExpectParse(copy, size, GEN3_PACK_ERR_BAD_HEADER_SIZE, "B bad header size");
    free(copy);

    /* C: unsupported version. */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchU32(copy, size, GEN3_PACK_OFF_FORMAT_VERSION, 2u);
    ExpectParse(copy, size, GEN3_PACK_ERR_UNSUPPORTED_VERSION, "C version");
    free(copy);

    /* D: bad endian tag. */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchU32(copy, size, GEN3_PACK_OFF_ENDIAN_TAG, 0u);
    ExpectParse(copy, size, GEN3_PACK_ERR_BAD_ENDIAN_TAG, "D endian tag");
    free(copy);

    /* E: nonzero reserved. */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchU32(copy, size, GEN3_PACK_OFF_RESERVED0, 1u);
    ExpectParse(copy, size, GEN3_PACK_ERR_NONZERO_RESERVED, "E reserved");
    free(copy);

    /* F: truncated header. */
    copy = malloc(size);
    memcpy(copy, base, size);
    ExpectParse(copy, GEN3_PACK_HEADER_SIZE - 1u, GEN3_PACK_ERR_TRUNCATED_HEADER, "F truncated header");
    free(copy);

    /* G: truncated TOC (toc offset pushes the section past the file). */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchU64(copy, size, GEN3_PACK_OFF_TOC_OFFSET, (uint64_t)size - 100u);
    ExpectParse(copy, size, GEN3_PACK_ERR_TRUNCATED_TOC, "G truncated TOC");
    free(copy);

    /* H: TOC size/count mismatch (entry size field). */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchU32(copy, size, GEN3_PACK_OFF_ENTRY_SIZE, GEN3_PACK_ENTRY_SIZE + 1u);
    ExpectParse(copy, size, GEN3_PACK_ERR_TOC_SIZE_MISMATCH, "H TOC size mismatch");
    free(copy);

    /* I: section overlap (names at the TOC start). */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchU64(copy, size, GEN3_PACK_OFF_NAMES_OFFSET, GEN3_PACK_HEADER_SIZE);
    ExpectParse(copy, size, GEN3_PACK_ERR_SECTION_OVERLAP, "I overlap");
    free(copy);

    /* J: section outside file (payload size huge). */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchU64(copy, size, GEN3_PACK_OFF_PAYLOAD_SIZE, (uint64_t)size * 2u);
    ExpectParse(copy, size, GEN3_PACK_ERR_SECTION_OUT_OF_FILE, "J out of file");
    free(copy);

    /* K: arithmetic overflow (names size max). */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchU64(copy, size, GEN3_PACK_OFF_NAMES_SIZE, 0xFFFFFFFFFFFFFFFFull);
    ExpectParse(copy, size, GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, "K overflow");
    free(copy);

    /* L: bad name offset (beyond names section). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        unsigned char buf[4];
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU32(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_NAME_OFFSET, 1000u);
        (void)buf;
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_BAD_NAME_OFFSET, "L name offset");
    free(copy);

    /* M: invalid canonical name (uppercase letter in a segment). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t names = (size_t)ReadLE64(base + GEN3_PACK_OFF_NAMES_OFFSET);
        copy[names + 51u] = 'S'; /* "sheet" -> "Sheet" */
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_INVALID_CANONICAL_NAME, "M invalid name");
    free(copy);

    /* N: key/name mismatch (flip a stored key byte). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        copy[entry1 + GEN3_PACK_ENTRY_OFF_KEY] ^= 0x01u;
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_KEY_MISMATCH, "N key mismatch");
    free(copy);

    /* O: duplicate canonical name (entry 1 names entry 0's bytes). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU32(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_NAME_OFFSET, 0u);
        PatchU32(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_NAME_LENGTH, 51u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_DUPLICATE_NAME, "O duplicate name");
    free(copy);

    /* P: duplicate key with different name (entry 1 copies entry 0's key). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry0 = GEN3_PACK_HEADER_SIZE;
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        memcpy(copy + entry1 + GEN3_PACK_ENTRY_OFF_KEY,
               copy + entry0 + GEN3_PACK_ENTRY_OFF_KEY, GEN3_RESOURCE_KEY_SIZE);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_DUPLICATE_KEY, "P duplicate key");
    free(copy);

    /* Q: TOC not sorted (entry 0 renamed to sort after entry 1, with a valid key). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t names = (size_t)ReadLE64(base + GEN3_PACK_OFF_NAMES_OFFSET);
        size_t entry0 = GEN3_PACK_HEADER_SIZE;
        Gen3ResourceKey derived;
        /* Byte 37 is the 'n' in "normal-palette" (first divergence vs "sheet").
         * 'z' keeps the segment valid but sorts the name after "sheet". */
        copy[names + 37u] = 'z';
        Gen3ResourceId_DeriveKey("emerald:trainer/brendan/battle/front/zormal-palette",
                                 &derived);
        memcpy(copy + entry0 + GEN3_PACK_ENTRY_OFF_KEY, derived.bytes,
               GEN3_RESOURCE_KEY_SIZE);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_UNSORTED_TOC, "Q unsorted");
    free(copy);

    /* R: invalid type code. */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU32(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_TYPE_CODE, 99u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_INVALID_TYPE_CODE, "R type code");
    free(copy);

    /* S: invalid schema (zero). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU32(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_SCHEMA, 0u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_INVALID_SCHEMA, "S schema");
    free(copy);

    /* S2: invalid representation code. */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU32(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_REPRESENTATION, 99u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_INVALID_REPRESENTATION_CODE, "S2 representation");
    free(copy);

    /* S3: invalid source-encoding code. */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU32(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_SOURCE_ENCODING, 99u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_INVALID_ENCODING_CODE, "S3 encoding");
    free(copy);

    /* T: payload offset before the payload section. */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU64(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_PAYLOAD_OFFSET, 100u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_PAYLOAD_OFFSET_BEFORE_SECTION, "T offset before");
    free(copy);

    /* U: payload range overflow (size within per-resource cap but past section end). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU64(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_PAYLOAD_SIZE, 0xFFFFF0u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_PAYLOAD_RANGE_OVERFLOW, "U range overflow");
    free(copy);

    /* V: misaligned payload offset. */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU64(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_PAYLOAD_OFFSET, 900u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_PAYLOAD_MISALIGNED, "V misaligned");
    free(copy);

    /* W: canonical payload hash mismatch. */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchZero(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_PAYLOAD_SHA256,
                  GEN3_PACK_SHA256_SIZE);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_PAYLOAD_HASH_MISMATCH, "W payload hash");
    free(copy);

    /* X: TOC hash mismatch. */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchZero(copy, size, GEN3_PACK_OFF_TOC_SHA256, GEN3_PACK_SHA256_SIZE);
    ExpectParse(copy, size, GEN3_PACK_ERR_TOC_HASH_MISMATCH, "X toc hash");
    free(copy);

    /* Y: names hash mismatch. */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchZero(copy, size, GEN3_PACK_OFF_NAMES_SHA256, GEN3_PACK_SHA256_SIZE);
    ExpectParse(copy, size, GEN3_PACK_ERR_NAMES_HASH_MISMATCH, "Y names hash");
    free(copy);

    /* Z: payload-section hash mismatch. */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchZero(copy, size, GEN3_PACK_OFF_PAYLOAD_SHA256, GEN3_PACK_SHA256_SIZE);
    ExpectParse(copy, size, GEN3_PACK_ERR_PAYLOAD_SECTION_HASH_MISMATCH, "Z payload section hash");
    free(copy);

    /* AA: logical pack digest mismatch. */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchZero(copy, size, GEN3_PACK_OFF_LOGICAL_SHA256, GEN3_PACK_SHA256_SIZE);
    ExpectParse(copy, size, GEN3_PACK_ERR_LOGICAL_DIGEST_MISMATCH, "AA logical");
    free(copy);

    /* AB: header hash mismatch. */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchZero(copy, size, GEN3_PACK_OFF_HEADER_SHA256, GEN3_PACK_SHA256_SIZE);
    ExpectParse(copy, size, GEN3_PACK_ERR_HEADER_HASH_MISMATCH, "AB header hash");
    free(copy);

    /* AC: declared file size mismatch. */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchU64(copy, size, GEN3_PACK_OFF_FILE_SIZE, (uint64_t)size + 1u);
    ExpectParse(copy, size, GEN3_PACK_ERR_SIZE_MISMATCH, "AC file size");
    free(copy);

    /* AC2: trailing garbage. */
    copy = malloc(size + 1u);
    memcpy(copy, base, size);
    copy[size] = 0x42u;
    ExpectParse(copy, size + 1u, GEN3_PACK_ERR_SIZE_MISMATCH, "AC trailing garbage");
    free(copy);

    /* AD: bad game ID (nonzero byte after the NUL). */
    copy = malloc(size);
    memcpy(copy, base, size);
    copy[GEN3_PACK_OFF_GAME_ID + 8u] = 1u;
    ExpectParse(copy, size, GEN3_PACK_ERR_BAD_GAME_ID, "AD game id");
    free(copy);

    /* AE: unknown nonzero header flags. */
    copy = malloc(size);
    memcpy(copy, base, size);
    PatchU32(copy, size, GEN3_PACK_OFF_FLAGS, 0x2u);
    ExpectParse(copy, size, GEN3_PACK_ERR_BAD_FLAGS, "AE flags");
    free(copy);

    /* AE2: unknown nonzero entry flags. */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU32(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_FLAGS, 0x2u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_BAD_FLAGS, "AE2 entry flags");
    free(copy);

    /* AF: malformed name length (empty). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU32(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_NAME_LENGTH, 0u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_INVALID_CANONICAL_NAME, "AF empty name length");
    free(copy);

    /* AF2: malformed name length (too long). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU32(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_NAME_LENGTH, 256u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_NAME_TOO_LONG, "AF2 name too long");
    free(copy);

    /* Additional: payload too large (per-entry cap). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU64(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_PAYLOAD_SIZE,
                 (uint64_t)GEN3_PACK_MAX_PAYLOAD_SIZE + 1u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_PAYLOAD_TOO_LARGE, "payload too large");
    free(copy);

    /* Additional: bad payload layout (offset equals a prior aligned slot). */
    copy = malloc(size);
    memcpy(copy, base, size);
    {
        size_t entry1 = GEN3_PACK_HEADER_SIZE + GEN3_PACK_ENTRY_SIZE;
        PatchU64(copy, size, entry1 + GEN3_PACK_ENTRY_OFF_PAYLOAD_OFFSET, 864u);
    }
    ExpectParse(copy, size, GEN3_PACK_ERR_BAD_PAYLOAD_LAYOUT, "bad payload layout");
    free(copy);

    free(base);
}

/* ------------------------------------------------------------------ */
/* Test 5: catalog validation                                          */
/* ------------------------------------------------------------------ */

static void TestCatalogValidation(void)
{
    unsigned char *bytes;
    size_t size;
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourceDiagnosticList coreDiag;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCatalog *wrongCatalog;

    BuildFixtureBytes(&bytes, &size);
    Gen3ResourcePackDiagnostics_Init(&diag);
    Gen3ResourceDiagnostics_Init(&coreDiag);
    CHECK(Gen3ResourcePack_Parse(bytes, size, &pack, &diag) == GEN3_PACK_OK);

    /* Matching catalog: OK. */
    catalog = Gen3ResourceCatalog_Create();
    CHECK(catalog != NULL);
    CHECK(Gen3ResourceCatalog_Add(catalog, FIXTURE_PALETTE_NAME,
                                  GEN3_RESOURCE_TYPE_PALETTE, 1u, true, &coreDiag));
    CHECK(Gen3ResourceCatalog_Add(catalog, FIXTURE_SHEET_NAME,
                                  GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u, true, &coreDiag));
    CHECK(Gen3ResourceCatalog_Finalize(catalog, &coreDiag));
    CHECK(Gen3ResourcePack_ValidateCatalog(pack, catalog, &diag) == GEN3_PACK_OK);

    /* No catalog: CATALOG_NOT_SUPPLIED. */
    CHECK(Gen3ResourcePack_ValidateCatalog(pack, NULL, &diag) == GEN3_PACK_ERR_CATALOG_NOT_SUPPLIED);

    /* Unknown resource: catalog missing the sheet. */
    wrongCatalog = Gen3ResourceCatalog_Create();
    CHECK(wrongCatalog != NULL);
    CHECK(Gen3ResourceCatalog_Add(wrongCatalog, FIXTURE_PALETTE_NAME,
                                  GEN3_RESOURCE_TYPE_PALETTE, 1u, true, &coreDiag));
    CHECK(Gen3ResourceCatalog_Finalize(wrongCatalog, &coreDiag));
    CHECK(Gen3ResourcePack_ValidateCatalog(pack, wrongCatalog, &diag) == GEN3_PACK_ERR_UNKNOWN_RESOURCE);
    Gen3ResourceCatalog_Destroy(wrongCatalog);

    /* Type mismatch: sheet declared as BINARY. */
    wrongCatalog = Gen3ResourceCatalog_Create();
    CHECK(wrongCatalog != NULL);
    CHECK(Gen3ResourceCatalog_Add(wrongCatalog, FIXTURE_PALETTE_NAME,
                                  GEN3_RESOURCE_TYPE_PALETTE, 1u, true, &coreDiag));
    CHECK(Gen3ResourceCatalog_Add(wrongCatalog, FIXTURE_SHEET_NAME,
                                  GEN3_RESOURCE_TYPE_BINARY, 1u, true, &coreDiag));
    CHECK(Gen3ResourceCatalog_Finalize(wrongCatalog, &coreDiag));
    CHECK(Gen3ResourcePack_ValidateCatalog(pack, wrongCatalog, &diag) == GEN3_PACK_ERR_TYPE_MISMATCH);
    Gen3ResourceCatalog_Destroy(wrongCatalog);

    /* Schema mismatch: sheet schema 2. */
    wrongCatalog = Gen3ResourceCatalog_Create();
    CHECK(wrongCatalog != NULL);
    CHECK(Gen3ResourceCatalog_Add(wrongCatalog, FIXTURE_PALETTE_NAME,
                                  GEN3_RESOURCE_TYPE_PALETTE, 1u, true, &coreDiag));
    CHECK(Gen3ResourceCatalog_Add(wrongCatalog, FIXTURE_SHEET_NAME,
                                  GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 2u, true, &coreDiag));
    CHECK(Gen3ResourceCatalog_Finalize(wrongCatalog, &coreDiag));
    CHECK(Gen3ResourcePack_ValidateCatalog(pack, wrongCatalog, &diag) == GEN3_PACK_ERR_SCHEMA_MISMATCH);
    Gen3ResourceCatalog_Destroy(wrongCatalog);

    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    Gen3ResourceDiagnostics_Destroy(&coreDiag);
    free(bytes);
}

/* ------------------------------------------------------------------ */
/* Test 6: writer validation paths                                     */
/* ------------------------------------------------------------------ */

static void TestWriterValidation(void)
{
    struct Gen3ResourcePackProfileInput profile;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackBuild *build;
    struct Gen3ResourcePackBytes bytes;
    unsigned char sheet[2048];
    unsigned char sheetSha[GEN3_PACK_SHA256_SIZE];
    unsigned char paletteSha[GEN3_PACK_SHA256_SIZE];
    unsigned char sheetSrc[GEN3_PACK_SHA256_SIZE];
    unsigned char paletteSrc[GEN3_PACK_SHA256_SIZE];
    unsigned char sheetKey[GEN3_RESOURCE_KEY_SIZE];
    unsigned char paletteKey[GEN3_RESOURCE_KEY_SIZE];
    struct FixtureHashes hashes;
    Gen3ResourceKey sheetKeyStruct;
    Gen3ResourceKey paletteKeyStruct;
    size_t i;

    for (i = 0; i < 2048u; i++)
        sheet[i] = (unsigned char)((i * 7u + (i >> 3u)) & 0xFFu);
    FromHex(sheetSha, kSheetShaHex);
    FromHex(paletteSha, kPaletteShaHex);
    FromHex(sheetSrc, kSheetSourceShaHex);
    FromHex(paletteSrc, kPaletteSourceShaHex);
    FromHex(sheetKey, kSheetKeyHex);
    FromHex(paletteKey, kPaletteKeyHex);
    memcpy(&sheetKeyStruct.bytes, sheetKey, GEN3_RESOURCE_KEY_SIZE);
    memcpy(&paletteKeyStruct.bytes, paletteKey, GEN3_RESOURCE_KEY_SIZE);
    Gen3ResourcePackDiagnostics_Init(&diag);

    FillHashes(&hashes);
    FillProfile(&profile, &hashes);

    /* Write with no profile -> BAD_METADATA. */
    build = Gen3ResourcePackBuild_Create();
    CHECK(build != NULL);
    memset(&bytes, 0, sizeof(bytes));
    CHECK(Gen3ResourcePackWriter_Write(build, &bytes, &diag) == GEN3_PACK_ERR_BAD_METADATA);
    Gen3ResourcePackBuild_Destroy(build);

    /* Profile validation: bad game id. */
    {
        struct Gen3ResourcePackProfileInput bad = profile;
        bad.gameId[0] = '\0';
        build = Gen3ResourcePackBuild_Create();
        CHECK(Gen3ResourcePackBuild_SetProfile(build, &bad, &diag) == GEN3_PACK_ERR_BAD_GAME_ID);
        Gen3ResourcePackBuild_Destroy(build);
    }
    /* Profile validation: zero ROM SHA-256. */
    {
        struct Gen3ResourcePackProfileInput bad = profile;
        unsigned char zero[GEN3_PACK_SHA256_SIZE] = {0};
        bad.sourceRomSha256 = zero;
        build = Gen3ResourcePackBuild_Create();
        CHECK(Gen3ResourcePackBuild_SetProfile(build, &bad, &diag) == GEN3_PACK_ERR_BAD_METADATA);
        Gen3ResourcePackBuild_Destroy(build);
    }

    /* Entry validation: bad key. */
    build = Gen3ResourcePackBuild_Create();
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    memset(&entry, 0, sizeof(entry));
    entry.canonicalName = FIXTURE_SHEET_NAME;
    entry.key = &paletteKeyStruct; /* wrong key */
    entry.type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    entry.schema = 1u;
    entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
    entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
    entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
    entry.canonicalPayload = sheet;
    entry.canonicalPayloadSize = sizeof(sheet);
    entry.canonicalPayloadSha256 = sheetSha;
    entry.sourceRomOffset = 0x300000u;
    entry.sourceEncodedSize = 804u;
    entry.sourceEncodedSha256 = sheetSrc;
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_ERR_KEY_MISMATCH);
    Gen3ResourcePackBuild_Destroy(build);

    /* Entry validation: payload hash mismatch. */
    build = Gen3ResourcePackBuild_Create();
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    entry.key = &sheetKeyStruct;
    entry.canonicalPayloadSha256 = paletteSha; /* wrong hash */
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_ERR_PAYLOAD_HASH_MISMATCH);
    Gen3ResourcePackBuild_Destroy(build);

    /* Entry validation: invalid type code. */
    build = Gen3ResourcePackBuild_Create();
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    entry.canonicalPayloadSha256 = sheetSha;
    entry.type = GEN3_RESOURCE_TYPE_INVALID;
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_ERR_INVALID_TYPE_CODE);
    Gen3ResourcePackBuild_Destroy(build);

    /* Entry validation: invalid schema. */
    build = Gen3ResourcePackBuild_Create();
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    entry.type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    entry.schema = 0u;
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_ERR_INVALID_SCHEMA);
    Gen3ResourcePackBuild_Destroy(build);

    /* Entry validation: empty payload. */
    build = Gen3ResourcePackBuild_Create();
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    entry.schema = 1u;
    entry.canonicalPayloadSize = 0u;
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_ERR_BAD_METADATA);
    Gen3ResourcePackBuild_Destroy(build);

    /* Entry validation: unknown representation code. */
    build = Gen3ResourcePackBuild_Create();
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    entry.canonicalPayloadSize = sizeof(sheet);
    entry.representation = (enum Gen3ResourcePackRepresentationCode)99;
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_ERR_INVALID_REPRESENTATION_CODE);
    Gen3ResourcePackBuild_Destroy(build);

    /* Entry validation: unknown encoding code. */
    build = Gen3ResourcePackBuild_Create();
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
    entry.sourceEncoding = (enum Gen3ResourcePackSourceEncodingCode)99;
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_ERR_INVALID_ENCODING_CODE);
    Gen3ResourcePackBuild_Destroy(build);

    /* Duplicate name. */
    build = Gen3ResourcePackBuild_Create();
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_OK);
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_ERR_DUPLICATE_NAME);
    Gen3ResourcePackBuild_Destroy(build);

    /* Provenance: source range exceeds source ROM (deferred to Write). */
    build = Gen3ResourcePackBuild_Create();
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    entry.sourceRomOffset = 16u * 1024u * 1024u; /* past 16 MiB ROM */
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_OK);
    memset(&bytes, 0, sizeof(bytes));
    CHECK(Gen3ResourcePackWriter_Write(build, &bytes, &diag) == GEN3_PACK_ERR_SOURCE_RANGE_OVERFLOW);
    Gen3ResourcePackBuild_Destroy(build);

    /* Caps: too many entries. */
    build = Gen3ResourcePackBuild_Create();
    Gen3ResourcePackBuild_SetLimits(build, 1u, 0u, 0u, 0u);
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    entry.sourceRomOffset = 0x300000u;
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_OK);
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_ERR_TOO_MANY_ENTRIES);
    Gen3ResourcePackBuild_Destroy(build);

    /* Caps: payload too large. */
    build = Gen3ResourcePackBuild_Create();
    Gen3ResourcePackBuild_SetLimits(build, 0u, 16u, 0u, 0u);
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_ERR_PAYLOAD_TOO_LARGE);
    Gen3ResourcePackBuild_Destroy(build);

    /* Caps: total payload too large. */
    build = Gen3ResourcePackBuild_Create();
    Gen3ResourcePackBuild_SetLimits(build, 0u, 0u, 10u, 0u);
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_ERR_TOTAL_PAYLOAD_TOO_LARGE);
    Gen3ResourcePackBuild_Destroy(build);

    /* Caps: pack too large (tiny max). */
    build = Gen3ResourcePackBuild_Create();
    Gen3ResourcePackBuild_SetLimits(build, 0u, 0u, 0u, 100u);
    CHECK(Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);
    CHECK(Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_OK);
    memset(&bytes, 0, sizeof(bytes));
    CHECK(Gen3ResourcePackWriter_Write(build, &bytes, &diag) == GEN3_PACK_ERR_PACK_TOO_LARGE);
    Gen3ResourcePackBuild_Destroy(build);

    /* NULL handling. */
    CHECK(Gen3ResourcePack_Parse(NULL, 0u, NULL, &diag) == GEN3_PACK_ERR_INVALID_ARGUMENT);
    CHECK(Gen3ResourcePack_OpenFile(NULL, NULL, &diag) == GEN3_PACK_ERR_INVALID_ARGUMENT);
    CHECK(Gen3ResourcePack_GetEntryCount(NULL) == 0u);
    CHECK(Gen3ResourcePack_GetEntry(NULL, 0u) == NULL);
    CHECK(Gen3ResourcePack_FindByCanonicalName(NULL, FIXTURE_SHEET_NAME) == NULL);
    CHECK(Gen3ResourcePack_GetProfile(NULL, NULL) == false);

    Gen3ResourcePackDiagnostics_Destroy(&diag);
}

/* ------------------------------------------------------------------ */
/* Test 7: file helper (thin stdio path)                               */
/* ------------------------------------------------------------------ */

static void TestFileHelper(void)
{
    unsigned char *bytes;
    size_t size;
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList diag;
    FILE *file;

    BuildFixtureBytes(&bytes, &size);
    Gen3ResourcePackDiagnostics_Init(&diag);

    file = fopen("gen3_pack_openfile_test.tmp", "wb");
    CHECK(file != NULL);
    if (file != NULL)
    {
        CHECK(fwrite(bytes, 1u, size, file) == size);
        fclose(file);
    }
    CHECK(Gen3ResourcePack_OpenFile("gen3_pack_openfile_test.tmp", &pack, &diag) == GEN3_PACK_OK);
    CHECK(pack != NULL);
    CHECK(Gen3ResourcePack_GetEntryCount(pack) == 2u);
    Gen3ResourcePack_Destroy(pack);
    pack = NULL;

    CHECK(Gen3ResourcePack_OpenFile("gen3_pack_does_not_exist.tmp", &pack, &diag) == GEN3_PACK_ERR_FILE_IO);

    remove("gen3_pack_openfile_test.tmp");
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    free(bytes);
}

/* ------------------------------------------------------------------ */
/* R12-A: instrument-bank on-disk type code (append-only v1 integer)   */
/* ------------------------------------------------------------------ */

static void TestR12AudioTypeCodes(void)
{
    enum Gen3ResourceType t;

    /* The new code is the append-only integer 15; every pre-existing code is
     * untouched (packs encode these integers, so nothing may renumber). */
    CHECK(GEN3_PACK_TYPE_INSTRUMENT_BANK == 15);
    CHECK(GEN3_PACK_TYPE_BINARY == 14);
    CHECK(GEN3_PACK_TYPE_CRY == 13);
    CHECK(GEN3_PACK_TYPE_AUDIO_SAMPLE == 10);
    CHECK(GEN3_PACK_TYPE_MUSIC_SEQUENCE == 11);
    CHECK(GEN3_PACK_TYPE_SOUND_EFFECT == 12);

    /* Instrument-bank specifically, both directions. */
    CHECK(Gen3ResourcePack_TypeToCode(GEN3_RESOURCE_TYPE_INSTRUMENT_BANK)
          == GEN3_PACK_TYPE_INSTRUMENT_BANK);
    CHECK(Gen3ResourcePack_TypeFromCode(GEN3_PACK_TYPE_INSTRUMENT_BANK)
          == GEN3_RESOURCE_TYPE_INSTRUMENT_BANK);

    /* Full round-trip over every valid type: code -> type -> code. */
    for (t = (enum Gen3ResourceType)1; t < GEN3_RESOURCE_TYPE_COUNT; t++)
    {
        enum Gen3ResourcePackTypeCode code = Gen3ResourcePack_TypeToCode(t);
        CHECK((int)code >= 1);
        CHECK(Gen3ResourcePack_TypeFromCode(code) == t);
    }

    /* Out of range maps to INVALID in both directions; COUNT is neither a
     * storable type nor a storable on-disk code. */
    CHECK(Gen3ResourcePack_TypeToCode(GEN3_RESOURCE_TYPE_COUNT) == GEN3_PACK_TYPE_INVALID);
    CHECK(Gen3ResourcePack_TypeToCode(GEN3_RESOURCE_TYPE_INVALID) == GEN3_PACK_TYPE_INVALID);
    CHECK(Gen3ResourcePack_TypeToCode((enum Gen3ResourceType)9999) == GEN3_PACK_TYPE_INVALID);
    CHECK(Gen3ResourcePack_TypeFromCode(GEN3_PACK_TYPE_COUNT) == GEN3_RESOURCE_TYPE_INVALID);
    CHECK(Gen3ResourcePack_TypeFromCode(GEN3_PACK_TYPE_INVALID) == GEN3_RESOURCE_TYPE_INVALID);
    CHECK(Gen3ResourcePack_TypeFromCode((enum Gen3ResourcePackTypeCode)9999) == GEN3_RESOURCE_TYPE_INVALID);
}

/* ------------------------------------------------------------------ */
/* Test 8: error description table                                     */
/* ------------------------------------------------------------------ */

static void TestDescribe(void)
{
    CHECK(Gen3ResourcePackError_Describe(GEN3_PACK_OK) != NULL);
    CHECK(Gen3ResourcePackError_Describe(GEN3_PACK_ERR_BAD_MAGIC) != NULL);
    CHECK(Gen3ResourcePackError_Describe(GEN3_PACK_ERR_SCHEMA_MISMATCH) != NULL);
    CHECK(Gen3ResourcePackError_Describe(GEN3_PACK_ERR_FILE_IO) != NULL);
    CHECK(Gen3ResourcePackError_Describe((enum Gen3ResourcePackError)9999) != NULL);
}

int main(void)
{
    TestFixedVectors();
    TestRoundTrip();
    TestInsertionOrderIndependence();
    TestCorruptionMatrix();
    TestCatalogValidation();
    TestWriterValidation();
    TestFileHelper();
    TestR12AudioTypeCodes();
    TestDescribe();

    printf("== %d checks, %d failures ==\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

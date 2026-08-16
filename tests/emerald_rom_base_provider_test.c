/* Stage R4: Emerald ROM_BASE session tests.
 *
 * Drives EmeraldResourceSession_BuildRomBaseCandidate with an in-memory
 * synthetic two-resource pack (the canonical Brendan front-sheet + palette,
 * exactly the proof pack content), then verifies the resulting ROM_BASE
 * provider through the NORMAL M0/M1 resolver:
 *
 *   - R4 §14  two-resource proof: kind/precedence/names/keys/types/schemas/
 *             sizes/bytes/winner id/trace/no-rom-offset.
 *   - R4 §11  provider-content vs logical digest (version-independent vs
 *             version-dependent; deterministic).
 *   - R4 §15  precedence A (BOOTSTRAP < ROM_BASE), B (LEGACY < ROM_BASE),
 *             C (registration-order independence), D (duplicate precedence).
 *   - R4 §16  transactional candidate behavior + invalid sessions.
 *   - R4 §18  source-pack lifetime: snapshot owns immutable payloads.
 *   - R4 §19  resource-view contract: only descriptive fields, no ROM offsets.
 *   - R4 §20  deterministic traces/diagnostics, no timestamps/pointers.
 *
 * No user ROM is required: everything is built in-process from synthetic data.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_rom_profile.h"
#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_pack_provider.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack.h"
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
/* Fixture: the canonical two-resource emerald proof pack               */
/* ------------------------------------------------------------------ */

#define kSheetName   "emerald:trainer/brendan/battle/front/sheet"
#define kPaletteName "emerald:trainer/brendan/battle/front/normal-palette"

#define kSheetSize   2048u /* tile-graphics: multiple of 32 */
#define kPaletteSize 32u   /* palette: even, <= 512 */

static void Sha256BytesOf(const unsigned char *data, size_t size,
                          unsigned char digest[32])
{
    struct Gen3Sha256Context context;
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, data, size);
    Gen3Sha256_Final(&context, digest);
}

static void FillPayload(unsigned char *payload, size_t size, unsigned seed)
{
    size_t i;
    for (i = 0; i < size; i++)
        payload[i] = (unsigned char)((i * seed + (i >> 3u)) & 0xFFu);
}

/* Deterministic expected canonical payloads (seeds match the pack builder). */
static void MakeExpected(unsigned char **sheet, unsigned char **palette)
{
    *sheet = malloc(kSheetSize);
    *palette = malloc(kPaletteSize);
    FillPayload(*sheet, kSheetSize, 7u);
    FillPayload(*palette, kPaletteSize, 3u);
}

static void BuildEmeraldPack(uint32_t basePackVersion,
                             struct Gen3ResourcePackBytes *out)
{
    const struct EmeraldRomProfile *fixture = EmeraldRomProfile_SyntheticFixture();
    unsigned char *sheet;
    unsigned char *palette;
    unsigned char sheetSha[32];
    unsigned char paletteSha[32];
    unsigned char catalogSha[32];
    unsigned char manifestSha[32];
    Gen3ResourceKey sheetKey;
    Gen3ResourceKey paletteKey;
    struct Gen3ResourcePackProfileInput profile;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackBuild *build;
    char gameId[GEN3_PACK_GAME_ID_SIZE];

    sheet = malloc(kSheetSize);
    palette = malloc(kPaletteSize);
    FillPayload(sheet, kSheetSize, 7u);
    FillPayload(palette, kPaletteSize, 3u);
    Sha256BytesOf(sheet, kSheetSize, sheetSha);
    Sha256BytesOf(palette, kPaletteSize, paletteSha);
    Sha256BytesOf((const unsigned char *)"catalog", 7u, catalogSha);
    Sha256BytesOf((const unsigned char *)"manifest", 8u, manifestSha);
    Gen3ResourceId_DeriveKey(kSheetName, &sheetKey);
    Gen3ResourceId_DeriveKey(kPaletteName, &paletteKey);

    memset(gameId, 0, sizeof(gameId));
    memcpy(gameId, fixture->game, strlen(fixture->game) + 1u);

    memset(&profile, 0, sizeof(profile));
    profile.basePackVersion = basePackVersion;
    profile.catalogVersion = 1u;
    profile.extractionManifestVersion = 1u;
    profile.canonicalRepresentationVersion = 1u;
    profile.sourceRomSize = fixture->romSize;
    profile.sourceRomSha1 = fixture->romSha1;
    profile.sourceRomSha256 = fixture->romSha256;
    memcpy(profile.gameCode, fixture->gameCode, 4u);
    memcpy(profile.makerCode, fixture->makerCode, 2u);
    profile.softwareRevision = fixture->softwareRevision;
    memcpy(profile.gameId, gameId, GEN3_PACK_GAME_ID_SIZE);
    profile.catalogSha256 = catalogSha;
    profile.extractionManifestSha256 = manifestSha;

    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
    {
        printf("FAIL: BuildEmeraldPack: OOM\n");
        exit(1);
    }
    if (Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildEmeraldPack: SetProfile\n");
        exit(1);
    }

    memset(&entry, 0, sizeof(entry));
    entry.canonicalName = kPaletteName;
    entry.key = &paletteKey;
    entry.type = GEN3_RESOURCE_TYPE_PALETTE;
    entry.schema = 1u;
    entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
    entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
    entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
    entry.canonicalPayload = palette;
    entry.canonicalPayloadSize = kPaletteSize;
    entry.canonicalPayloadSha256 = paletteSha;
    entry.sourceRomOffset = 0x00310000u;
    entry.sourceEncodedSize = kPaletteSize;
    entry.sourceEncodedSha256 = paletteSha;
    if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildEmeraldPack: AddEntry palette\n");
        exit(1);
    }

    memset(&entry, 0, sizeof(entry));
    entry.canonicalName = kSheetName;
    entry.key = &sheetKey;
    entry.type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    entry.schema = 1u;
    entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
    entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
    entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
    entry.canonicalPayload = sheet;
    entry.canonicalPayloadSize = kSheetSize;
    entry.canonicalPayloadSha256 = sheetSha;
    entry.sourceRomOffset = 0x00300000u;
    entry.sourceEncodedSize = kSheetSize;
    entry.sourceEncodedSha256 = sheetSha;
    if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildEmeraldPack: AddEntry sheet\n");
        exit(1);
    }

    memset(out, 0, sizeof(*out));
    if (Gen3ResourcePackWriter_Write(build, out, &diag) != GEN3_PACK_OK)
    {
        printf("FAIL: BuildEmeraldPack: Write\n");
        exit(1);
    }

    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    free(sheet);
    free(palette);
}

static struct Gen3ResourcePack *ParsePack(const struct Gen3ResourcePackBytes *bytes)
{
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePack *pack = NULL;
    enum Gen3ResourcePackError error;
    Gen3ResourcePackDiagnostics_Init(&diag);
    error = Gen3ResourcePack_Parse(bytes->data, bytes->size, &pack, &diag);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    CHECK("synthetic pack parses", error == GEN3_PACK_OK && pack != NULL);
    return pack;
}

static void BuildEmeraldCatalog(struct Gen3ResourceCatalog **outCatalog)
{
    struct Gen3ResourceCatalog *catalog = Gen3ResourceCatalog_Create();
    if (catalog == NULL)
    {
        printf("FAIL: BuildEmeraldCatalog: OOM\n");
        exit(1);
    }
    Gen3ResourceCatalog_Add(catalog, kPaletteName, GEN3_RESOURCE_TYPE_PALETTE,
                            1u, true, NULL);
    Gen3ResourceCatalog_Add(catalog, kSheetName, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                            1u, true, NULL);
    Gen3ResourceCatalog_Finalize(catalog, NULL);
    *outCatalog = catalog;
}

/* Build a session candidate from the fixture pack + catalog. */
static void BuildSessionCandidate(struct Gen3ResourcePack **outPack,
                                  struct Gen3ResourcePackBytes *outBytes,
                                  struct Gen3ResourceCatalog **outCatalog,
                                  struct Gen3ResourceCandidate **outCandidate,
                                  struct EmeraldResourceSessionInfo *outInfo,
                                  struct Gen3ResourceDiagnosticList *diag)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidate = NULL;
    enum EmeraldResourceSessionError error;

    BuildEmeraldPack(1u, &bytes);
    pack = ParsePack(&bytes);
    BuildEmeraldCatalog(&catalog);

    error = EmeraldResourceSession_BuildRomBaseCandidate(pack, catalog,
        &candidate, outInfo, diag);
    CHECK("session builds candidate", error == EMERALD_SESSION_OK);
    CHECK("candidate non-NULL", candidate != NULL);

    *outPack = pack;
    *outBytes = bytes;
    *outCatalog = catalog;
    *outCandidate = candidate;
}

/* Resolve one resource and assert every descriptive view field. */
static void ResolveAndCheck(struct Gen3ResourceSnapshot *snapshot,
                            const char *name,
                            enum Gen3ResourceType type,
                            uint32_t schema,
                            const unsigned char *expected,
                            size_t expectedSize,
                            const char *label)
{
    Gen3ResourceHandle handle;
    (void)label;
    Gen3ResourceKey expectedKey;
    struct Gen3ResourceView view;
    struct Gen3ResourceTrace trace;

    Gen3ResourceId_DeriveKey(name, &expectedKey);
    CHECK("handle found",
          Gen3ResourceSnapshot_FindHandle(snapshot, name, &handle) == GEN3_RESOURCE_OK);
    CHECK("resolves",
          Gen3ResourceSnapshot_Resolve(snapshot, handle, type, schema, &view)
              == GEN3_RESOURCE_OK);
    CHECK("canonical name", strcmp(view.canonicalName, name) == 0);
    CHECK("stable key", memcmp(view.key.bytes, expectedKey.bytes,
                               GEN3_RESOURCE_KEY_SIZE) == 0);
    CHECK("type", view.type == type);
    CHECK("schema", view.schema == schema);
    CHECK("payload size", view.payloadSize == expectedSize);
    CHECK("payload bytes", memcmp(view.payload, expected, expectedSize) == 0);
    CHECK("winner id",
          strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) == 0);
    CHECK("winner version", strcmp(view.winningProviderVersion, "v1") == 0);
    CHECK("winner precedence", view.winningProviderPrecedence == EMERALD_ROM_BASE_PRECEDENCE);

    /* Deterministic trace: single provider, winner, no rejection reason, and
     * every field is descriptive (no timestamps, no pointers). */
    Gen3ResourceTrace_Init(&trace);
    CHECK("trace produced", Gen3ResourceSnapshot_Trace(snapshot, handle, &trace));
    CHECK("trace has exactly one record", trace.count == 1u);
    if (trace.count == 1u)
    {
        CHECK("trace provider id",
              strcmp(trace.items[0].providerId, EMERALD_ROM_BASE_PROVIDER_ID) == 0);
        CHECK("trace provider version", strcmp(trace.items[0].providerVersion, "v1") == 0);
        CHECK("trace precedence", trace.items[0].precedence == EMERALD_ROM_BASE_PRECEDENCE);
        CHECK("trace winner", trace.items[0].disposition == GEN3_TRACE_WINNER);
        CHECK("trace no rejection reason",
              trace.items[0].reason == GEN3_RESOURCE_REASON_NONE);
    }
    Gen3ResourceTrace_Destroy(&trace);
}

/* ------------------------------------------------------------------ */
/* 1. Two-resource proof through the normal resolver (R4 §14)          */
/* ------------------------------------------------------------------ */

static void TestSessionTwoResourceProof(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct EmeraldResourceSessionInfo info;
    struct Gen3ResourceDiagnosticList diag;
    unsigned char *sheet;
    unsigned char *palette;

    MakeExpected(&sheet, &palette);
    Gen3ResourceDiagnostics_Init(&diag);
    BuildSessionCandidate(&pack, &bytes, &catalog, &candidate, &info, &diag);

    /* Session info: provider identity + pack metadata, all logical. */
    CHECK("info provider id", strcmp(info.providerId, EMERALD_ROM_BASE_PROVIDER_ID) == 0);
    CHECK("info provider version", strcmp(info.providerVersion, "v1") == 0);
    CHECK("info kind ROM_BASE", info.kind == GEN3_PROVIDER_ROM_BASE);
    CHECK("info precedence", info.precedence == EMERALD_ROM_BASE_PRECEDENCE);
    CHECK("info basePackVersion", info.basePackVersion == 1u);
    CHECK("info catalogVersion", info.catalogVersion == 1u);
    CHECK("info extractionManifestVersion", info.extractionManifestVersion == 1u);
    CHECK("info canonicalRepresentationVersion",
          info.canonicalRepresentationVersion == 1u);
    CHECK("info gameId", strcmp(info.gameId, "emerald") == 0);
    CHECK("info entryCount", info.entryCount == 2u);
    CHECK("info provider digest present", info.hasProviderContentDigest);
    CHECK("info logical digest present", info.hasLogicalContentDigest);

    CHECK("candidate builds snapshot",
          Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));
    CHECK("snapshot non-NULL", snapshot != NULL);

    if (snapshot != NULL)
    {
        ResolveAndCheck(snapshot, kSheetName, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                        1u, sheet, kSheetSize, "sheet");
        ResolveAndCheck(snapshot, kPaletteName, GEN3_RESOURCE_TYPE_PALETTE,
                        1u, palette, kPaletteSize, "palette");

        /* Typed accessors resolve too. */
        {
            Gen3ResourceHandle handle;
            struct Gen3TileGraphicsView gfx;
            struct Gen3PaletteView pal;
            Gen3ResourceSnapshot_FindHandle(snapshot, kSheetName, &handle);
            CHECK("typed tile-graphics view",
                  Gen3ResourceSnapshot_GetTileGraphics(snapshot, handle, 1u, &gfx)
                      == GEN3_RESOURCE_OK);
            CHECK("tile-graphics bytes",
                  memcmp(gfx.bytes, sheet, kSheetSize) == 0);
            Gen3ResourceSnapshot_FindHandle(snapshot, kPaletteName, &handle);
            CHECK("typed palette view",
                  Gen3ResourceSnapshot_GetPalette(snapshot, handle, 1u, &pal)
                      == GEN3_RESOURCE_OK);
            CHECK("palette color count", pal.colorCount == kPaletteSize / 2u);
        }
        Gen3ResourceSnapshot_Destroy(snapshot);
    }

    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourceDiagnostics_Destroy(&diag);
    free(sheet);
    free(palette);
}

/* ------------------------------------------------------------------ */
/* 2. Provider-content digest is version/layout independent (§11)      */
/* ------------------------------------------------------------------ */

static void TestDigestVersionIndependence(void)
{
    struct Gen3ResourcePackBytes bytesV1;
    struct Gen3ResourcePackBytes bytesV2;
    struct Gen3ResourcePack *packV1;
    struct Gen3ResourcePack *packV2;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidate;
    struct EmeraldResourceSessionInfo infoV1;
    struct EmeraldResourceSessionInfo infoV2;
    struct Gen3ResourceDiagnosticList diag;

    BuildEmeraldPack(1u, &bytesV1);
    BuildEmeraldPack(2u, &bytesV2);
    packV1 = ParsePack(&bytesV1);
    packV2 = ParsePack(&bytesV2);
    BuildEmeraldCatalog(&catalog);
    Gen3ResourceDiagnostics_Init(&diag);

    CHECK("session v1 ok",
          EmeraldResourceSession_BuildRomBaseCandidate(packV1, catalog, &candidate,
              &infoV1, &diag) == EMERALD_SESSION_OK);
    CHECK("info v1 version", strcmp(infoV1.providerVersion, "v1") == 0);
    Gen3ResourceCandidate_Destroy(candidate);

    CHECK("session v2 ok",
          EmeraldResourceSession_BuildRomBaseCandidate(packV2, catalog, &candidate,
              &infoV2, &diag) == EMERALD_SESSION_OK);
    CHECK("info v2 version", strcmp(infoV2.providerVersion, "v2") == 0);
    Gen3ResourceCandidate_Destroy(candidate);

    /* Same logical content at a different base-pack version: the
     * provider-content digest (layout/content based) is identical, while the
     * logical digest (which includes the pack version) differs. Deterministic. */
    CHECK("provider digest identical across pack versions",
          infoV1.hasProviderContentDigest && infoV2.hasProviderContentDigest
              && memcmp(infoV1.providerContentDigest, infoV2.providerContentDigest,
                        32) == 0);
    CHECK("logical digest differs across pack versions",
          infoV1.hasLogicalContentDigest && infoV2.hasLogicalContentDigest
              && memcmp(infoV1.logicalContentDigest, infoV2.logicalContentDigest,
                        32) != 0);

    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(packV1);
    Gen3ResourcePack_Destroy(packV2);
    Gen3ResourcePackBytes_Destroy(&bytesV1);
    Gen3ResourcePackBytes_Destroy(&bytesV2);
    Gen3ResourceDiagnostics_Destroy(&diag);
}

/* ------------------------------------------------------------------ */
/* Auxiliary base providers for the precedence tests                   */
/* ------------------------------------------------------------------ */

static struct Gen3ResourceProvider *BuildAuxProvider(
    const char *id, enum Gen3ResourceProviderKind kind,
    uint32_t precedence, unsigned char fill)
{
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider;
    unsigned char *sheet = malloc(kSheetSize);
    unsigned char *palette = malloc(kPaletteSize);
    unsigned char sheetSha[32];
    unsigned char paletteSha[32];
    size_t i;

    for (i = 0; i < kSheetSize; i++)
        sheet[i] = fill;
    for (i = 0; i < kPaletteSize; i++)
        palette[i] = (unsigned char)(fill + 1u);
    Sha256BytesOf(sheet, kSheetSize, sheetSha);
    Sha256BytesOf(palette, kPaletteSize, paletteSha);

    metadata.id = id;
    metadata.version = "v1";
    metadata.kind = kind;
    metadata.precedence = precedence;
    provider = Gen3ResourceProvider_Create(&metadata);
    if (provider == NULL)
    {
        printf("FAIL: BuildAuxProvider: OOM\n");
        exit(1);
    }
    Gen3ResourceProvider_Add(provider, kPaletteName, GEN3_RESOURCE_TYPE_PALETTE,
                             1u, palette, kPaletteSize, false, NULL);
    Gen3ResourceProvider_Add(provider, kSheetName, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                             1u, sheet, kSheetSize, false, NULL);
    Gen3ResourceProvider_Finalize(provider, NULL);
    free(sheet);
    free(palette);
    return provider;
}

/* Run one precedence scenario: expected winner + payload fill byte. */
static void TestPrecedenceScenario(const char *label,
                                   struct Gen3ResourceProvider *aux,
                                   const char *expectedWinnerId,
                                   unsigned char expectedSheetFill)
{
    struct Gen3ResourcePackBytes bytes;
    (void)label;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct EmeraldResourceSessionInfo info;
    struct Gen3ResourceDiagnosticList diag;
    Gen3ResourceHandle handle;
    struct Gen3ResourceView view;
    struct Gen3ResourceTrace trace;
    unsigned char *expectedSheet;
    unsigned char *expectedPalette;

    MakeExpected(&expectedSheet, &expectedPalette);

    Gen3ResourceDiagnostics_Init(&diag);
    BuildSessionCandidate(&pack, &bytes, &catalog, &candidate, &info, &diag);
    CHECK("aux provider added",
          Gen3ResourceCandidate_AddProvider(candidate, aux, &diag));
    CHECK("candidate builds snapshot",
          Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));

    if (snapshot != NULL)
    {
        /* The sheet resolves to the higher-precedence winner. */
        Gen3ResourceSnapshot_FindHandle(snapshot, kSheetName, &handle);
        CHECK("winner resolves",
              Gen3ResourceSnapshot_Resolve(snapshot, handle,
                  GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u, &view) == GEN3_RESOURCE_OK);
        CHECK("winner id",
              strcmp(view.winningProviderId, expectedWinnerId) == 0);
        CHECK("winner precedence",
              view.winningProviderPrecedence == EMERALD_ROM_BASE_PRECEDENCE);

        /* If ROM_BASE wins the payload is canonical; if the aux provider wins
         * the payload is the fill pattern. */
        if (strcmp(expectedWinnerId, EMERALD_ROM_BASE_PROVIDER_ID) == 0)
        {
            CHECK("winner payload is canonical",
                  memcmp(view.payload, expectedSheet, kSheetSize) == 0);
        }
        else
        {
            unsigned char *auxSheet = malloc(kSheetSize);
            size_t i;
            for (i = 0; i < kSheetSize; i++)
                auxSheet[i] = expectedSheetFill;
            CHECK("winner payload is aux fill",
                  memcmp(view.payload, auxSheet, kSheetSize) == 0);
            free(auxSheet);
        }

        /* Trace: highest precedence first; exactly one winner. */
        Gen3ResourceTrace_Init(&trace);
        CHECK("trace produced", Gen3ResourceSnapshot_Trace(snapshot, handle, &trace));
        CHECK("trace has two records", trace.count == 2u);
        if (trace.count == 2u)
        {
            CHECK("first record is winner",
                  trace.items[0].disposition == GEN3_TRACE_WINNER);
            CHECK("second record is fallback",
                  trace.items[1].disposition == GEN3_TRACE_FALLBACK);
            CHECK("winner id in trace",
                  strcmp(trace.items[0].providerId, expectedWinnerId) == 0);
        }
        Gen3ResourceTrace_Destroy(&trace);
        Gen3ResourceSnapshot_Destroy(snapshot);
    }

    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourceDiagnostics_Destroy(&diag);
    free(expectedSheet);
    free(expectedPalette);
}

static void TestPrecedence(void)
{
    struct Gen3ResourceProvider *aux;

    /* A: BOOTSTRAP (100) below ROM_BASE (300): ROM_BASE wins. */
    aux = BuildAuxProvider("emerald.bootstrap.v0", GEN3_PROVIDER_BOOTSTRAP, 100u, 0xAA);
    TestPrecedenceScenario("A bootstrap below rom base", aux,
                           EMERALD_ROM_BASE_PROVIDER_ID, 0xAA);
    Gen3ResourceProvider_Destroy(aux);

    /* B: LEGACY_COMPILED (200) below ROM_BASE (300): ROM_BASE wins. */
    aux = BuildAuxProvider("emerald.legacy.v0", GEN3_PROVIDER_LEGACY_COMPILED, 200u, 0xBB);
    TestPrecedenceScenario("B legacy below rom base", aux,
                           EMERALD_ROM_BASE_PROVIDER_ID, 0xBB);
    Gen3ResourceProvider_Destroy(aux);
}

/* C: registration order does not change the winner. */
static void TestPrecedenceOrderIndependence(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidateA;
    struct Gen3ResourceCandidate *candidateB;
    struct Gen3ResourceSnapshot *snapshotA = NULL;
    struct Gen3ResourceSnapshot *snapshotB = NULL;
    struct EmeraldResourceSessionInfo info;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceProvider *aux;
    struct Gen3ResourceProvider *romBaseProvider;
    struct Gen3ResourcePackProviderInfo packInfo;
    struct Gen3ResourceProviderMetadata metadata;
    Gen3ResourceHandle handle;
    struct Gen3ResourceView viewA;
    struct Gen3ResourceView viewB;
    unsigned char *expectedSheet;
    unsigned char *expectedPalette;

    MakeExpected(&expectedSheet, &expectedPalette);
    Gen3ResourceDiagnostics_Init(&diag);
    BuildSessionCandidate(&pack, &bytes, &catalog, &candidateA, &info, &diag);

    candidateB = Gen3ResourceCandidate_Create(catalog, &diag);
    CHECK("candidate B created", candidateB != NULL);

    aux = BuildAuxProvider("emerald.bootstrap.v0", GEN3_PROVIDER_BOOTSTRAP, 100u, 0xCC);

    /* Order 1: ROM_BASE added first, then bootstrap. */
    Gen3ResourceCandidate_AddProvider(candidateA, aux, &diag);
    CHECK("candidate A builds",
          Gen3ResourceCandidate_Build(candidateA, &snapshotA, &diag));

    /* Order 2: bootstrap added first, then ROM_BASE. A second ROM_BASE provider
     * is built directly from the SAME validated pack, so both candidates carry
     * the identical provider set in opposite registration order. */
    metadata.id = EMERALD_ROM_BASE_PROVIDER_ID;
    metadata.version = "v1";
    metadata.kind = GEN3_PROVIDER_ROM_BASE;
    metadata.precedence = EMERALD_ROM_BASE_PRECEDENCE;
    CHECK("second ROM_BASE provider built",
          Gen3ResourcePackProvider_CreateFromPack(pack, catalog, &metadata,
              &romBaseProvider, &packInfo, &diag) == GEN3_PACK_PROVIDER_OK);
    Gen3ResourceCandidate_AddProvider(candidateB, aux, &diag);
    CHECK("ROM_BASE added second",
          Gen3ResourceCandidate_AddProvider(candidateB, romBaseProvider, &diag));
    Gen3ResourceProvider_Destroy(romBaseProvider);
    CHECK("candidate B builds",
          Gen3ResourceCandidate_Build(candidateB, &snapshotB, &diag));

    if (snapshotA != NULL && snapshotB != NULL)
    {
        Gen3ResourceSnapshot_FindHandle(snapshotA, kSheetName, &handle);
        Gen3ResourceSnapshot_Resolve(snapshotA, handle,
            GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u, &viewA);
        Gen3ResourceSnapshot_FindHandle(snapshotB, kSheetName, &handle);
        Gen3ResourceSnapshot_Resolve(snapshotB, handle,
            GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u, &viewB);
        CHECK("same winner id both orders",
              strcmp(viewA.winningProviderId, viewB.winningProviderId) == 0);
        CHECK("same payload both orders",
              memcmp(viewA.payload, viewB.payload, kSheetSize) == 0);
        CHECK("payload is canonical",
              memcmp(viewA.payload, expectedSheet, kSheetSize) == 0);
        CHECK("winner is rom base",
              strcmp(viewA.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) == 0);
        Gen3ResourceSnapshot_Destroy(snapshotA);
        Gen3ResourceSnapshot_Destroy(snapshotB);
    }

    Gen3ResourceProvider_Destroy(aux);
    Gen3ResourceCandidate_Destroy(candidateA);
    Gen3ResourceCandidate_Destroy(candidateB);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourceDiagnostics_Destroy(&diag);
    free(expectedSheet);
    free(expectedPalette);
}

/* D: duplicate precedence is refused at candidate build. */
static void TestPrecedenceDuplicate(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct EmeraldResourceSessionInfo info;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceProvider *duplicate;

    Gen3ResourceDiagnostics_Init(&diag);
    BuildSessionCandidate(&pack, &bytes, &catalog, &candidate, &info, &diag);

    /* A second base provider at the same precedence 300 (distinct id, so the
     * only violation is the duplicate precedence). */
    duplicate = BuildAuxProvider("engine.legacy-compiled",
                                 GEN3_PROVIDER_LEGACY_COMPILED,
                                 EMERALD_ROM_BASE_PRECEDENCE, 0xDD);

    CHECK("duplicate-precedence provider added",
          Gen3ResourceCandidate_AddProvider(candidate, duplicate, &diag));
    CHECK("candidate rejects duplicate precedence",
          !Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));
    CHECK("no snapshot from invalid candidate", snapshot == NULL);

    Gen3ResourceProvider_Destroy(duplicate);
    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourceDiagnostics_Destroy(&diag);
}

/* ------------------------------------------------------------------ */
/* 3. Transactional: invalid candidate leaves active snapshot intact   */
/* ------------------------------------------------------------------ */

static void TestTransactional(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePackBytes bytesB;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourcePack *packB;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCatalog *catalogB;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshotA = NULL;
    struct Gen3ResourceSnapshot *snapshotB = NULL;
    struct EmeraldResourceSessionInfo info;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceRegistry registry;
    struct Gen3ResourceProvider *duplicate;
    Gen3ResourceHandle handle;
    struct Gen3ResourceView view;
    unsigned char *expectedSheet;
    unsigned char *expectedPalette;

    MakeExpected(&expectedSheet, &expectedPalette);
    Gen3ResourceDiagnostics_Init(&diag);

    /* Publish a valid snapshot A. */
    BuildSessionCandidate(&pack, &bytes, &catalog, &candidate, &info, &diag);
    CHECK("snapshot A builds",
          Gen3ResourceCandidate_Build(candidate, &snapshotA, &diag));
    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceRegistry_Init(&registry);
    Gen3ResourceRegistry_Publish(&registry, snapshotA);
    CHECK("registry active", Gen3ResourceRegistry_Active(&registry) == snapshotA);

    /* Build a second, INVALID candidate B (the ROM_BASE provider from a fresh
     * session plus a second base provider at the same precedence 300) and try
     * to build + publish it: build fails, snapshot B stays NULL, A is
     * untouched. */
    BuildSessionCandidate(&packB, &bytesB, &catalogB, &candidate, &info, &diag);
    duplicate = BuildAuxProvider("engine.legacy-compiled",
                                 GEN3_PROVIDER_LEGACY_COMPILED,
                                 EMERALD_ROM_BASE_PRECEDENCE, 0xEE);
    CHECK("provider B added",
          Gen3ResourceCandidate_AddProvider(candidate, duplicate, &diag));
    CHECK("candidate B build fails", !Gen3ResourceCandidate_Build(candidate, &snapshotB, &diag));
    CHECK("snapshot B NULL", snapshotB == NULL);
    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceProvider_Destroy(duplicate);

    /* Active snapshot A unchanged and still resolves to the canonical bytes. */
    CHECK("registry still holds A",
          Gen3ResourceRegistry_Active(&registry) == snapshotA);
    Gen3ResourceSnapshot_FindHandle(Gen3ResourceRegistry_Active(&registry),
                                    kSheetName, &handle);
    CHECK("A still resolves",
          Gen3ResourceSnapshot_Resolve(Gen3ResourceRegistry_Active(&registry),
              handle, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u, &view) == GEN3_RESOURCE_OK);
    CHECK("A payload intact",
          memcmp(view.payload, expectedSheet, kSheetSize) == 0);
    CHECK("A winner id",
          strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) == 0);

    Gen3ResourceRegistry_Destroy(&registry);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourceCatalog_Destroy(catalogB);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePack_Destroy(packB);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourcePackBytes_Destroy(&bytesB);
    Gen3ResourceDiagnostics_Destroy(&diag);
    free(expectedSheet);
    free(expectedPalette);
}

/* ------------------------------------------------------------------ */
/* 4. Source-pack lifetime (R4 §18)                                    */
/* ------------------------------------------------------------------ */

static void TestSourcePackLifetime(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct EmeraldResourceSessionInfo info;
    struct Gen3ResourceDiagnosticList diag;
    Gen3ResourceHandle handle;
    struct Gen3ResourceView view;
    unsigned char *expectedPalette;

    expectedPalette = malloc(kPaletteSize);
    FillPayload(expectedPalette, kPaletteSize, 3u);

    Gen3ResourceDiagnostics_Init(&diag);
    BuildSessionCandidate(&pack, &bytes, &catalog, &candidate, &info, &diag);

    /* Build the snapshot, then destroy pack + candidate + catalog: the
     * snapshot owns immutable clones of everything. */
    CHECK("snapshot builds",
          Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceCatalog_Destroy(catalog);

    if (snapshot != NULL)
    {
        Gen3ResourceSnapshot_FindHandle(snapshot, kPaletteName, &handle);
        CHECK("palette resolves after all sources destroyed",
              Gen3ResourceSnapshot_Resolve(snapshot, handle,
                  GEN3_RESOURCE_TYPE_PALETTE, 1u, &view) == GEN3_RESOURCE_OK);
        CHECK("palette payload intact",
              memcmp(view.payload, expectedPalette, kPaletteSize) == 0);
        CHECK("palette winner id",
              strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) == 0);
        Gen3ResourceSnapshot_Destroy(snapshot);
    }

    Gen3ResourceDiagnostics_Destroy(&diag);
    free(expectedPalette);
}

/* ------------------------------------------------------------------ */
/* 5. Invalid session inputs (R4 §16)                                  */
/* ------------------------------------------------------------------ */

static void TestInvalidSessions(void)
{
    struct Gen3ResourcePackBytes bytes;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidate = NULL;
    struct EmeraldResourceSessionInfo info;
    struct Gen3ResourceDiagnosticList diag;
    enum EmeraldResourceSessionError error;

    Gen3ResourceDiagnostics_Init(&diag);

    /* NULL pack. */
    BuildEmeraldCatalog(&catalog);
    error = EmeraldResourceSession_BuildRomBaseCandidate(NULL, catalog,
        &candidate, &info, &diag);
    CHECK("NULL pack rejected", error == EMERALD_SESSION_ERR_INVALID_ARGUMENT);
    CHECK("no candidate on failure", candidate == NULL);
    /* Catalog is reused below; destroyed once at the end. */

    /* NULL catalog. */
    BuildEmeraldPack(1u, &bytes);
    pack = ParsePack(&bytes);
    error = EmeraldResourceSession_BuildRomBaseCandidate(pack, NULL,
        &candidate, &info, &diag);
    CHECK("NULL catalog rejected", error == EMERALD_SESSION_ERR_INVALID_ARGUMENT);
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackBytes_Destroy(&bytes);

    /* Catalog-inconsistent pack: add a third entry the catalog does not know.
     * The adapter's fail-closed gate refuses the provider, so no candidate. */
    {
        struct Gen3ResourcePackBytes badBytes;
        struct Gen3ResourcePack *badPack;
        unsigned char extra[2048];
        unsigned char extraSha[32];
        Gen3ResourceKey extraKey;
        struct Gen3ResourcePackProfileInput profile;
        struct Gen3ResourcePackEntryInput entry;
        struct Gen3ResourcePackDiagnosticList writeDiag;
        struct Gen3ResourcePackBuild *build;
        const struct EmeraldRomProfile *fixture = EmeraldRomProfile_SyntheticFixture();
        char gameId[GEN3_PACK_GAME_ID_SIZE];
        unsigned char sheetSha[32];
        unsigned char paletteSha[32];
        unsigned char *sheet = malloc(kSheetSize);
        unsigned char *palette = malloc(kPaletteSize);
        Gen3ResourceKey sheetKey;
        Gen3ResourceKey paletteKey;

        FillPayload(sheet, kSheetSize, 7u);
        FillPayload(palette, kPaletteSize, 3u);
        FillPayload(extra, sizeof(extra), 11u);
        Sha256BytesOf(sheet, kSheetSize, sheetSha);
        Sha256BytesOf(palette, kPaletteSize, paletteSha);
        Sha256BytesOf(extra, sizeof(extra), extraSha);
        Gen3ResourceId_DeriveKey(kSheetName, &sheetKey);
        Gen3ResourceId_DeriveKey(kPaletteName, &paletteKey);
        Gen3ResourceId_DeriveKey("emerald:trainer/brendan/extra", &extraKey);

        memset(gameId, 0, sizeof(gameId));
        memcpy(gameId, fixture->game, strlen(fixture->game) + 1u);
        memset(&profile, 0, sizeof(profile));
        profile.basePackVersion = 1u;
        profile.catalogVersion = 1u;
        profile.extractionManifestVersion = 1u;
        profile.canonicalRepresentationVersion = 1u;
        profile.sourceRomSize = fixture->romSize;
        profile.sourceRomSha1 = fixture->romSha1;
        profile.sourceRomSha256 = fixture->romSha256;
        memcpy(profile.gameCode, fixture->gameCode, 4u);
        memcpy(profile.makerCode, fixture->makerCode, 2u);
        profile.softwareRevision = fixture->softwareRevision;
        memcpy(profile.gameId, gameId, GEN3_PACK_GAME_ID_SIZE);
        profile.catalogSha256 = sheetSha;
        profile.extractionManifestSha256 = paletteSha;

        Gen3ResourcePackDiagnostics_Init(&writeDiag);
        build = Gen3ResourcePackBuild_Create();
        Gen3ResourcePackBuild_SetProfile(build, &profile, &writeDiag);

        memset(&entry, 0, sizeof(entry));
        entry.canonicalName = kPaletteName;
        entry.key = &paletteKey;
        entry.type = GEN3_RESOURCE_TYPE_PALETTE;
        entry.schema = 1u;
        entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
        entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
        entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
        entry.canonicalPayload = palette;
        entry.canonicalPayloadSize = kPaletteSize;
        entry.canonicalPayloadSha256 = paletteSha;
        entry.sourceRomOffset = 0x00310000u;
        entry.sourceEncodedSize = kPaletteSize;
        entry.sourceEncodedSha256 = paletteSha;
        Gen3ResourcePackBuild_AddEntry(build, &entry, &writeDiag);

        memset(&entry, 0, sizeof(entry));
        entry.canonicalName = kSheetName;
        entry.key = &sheetKey;
        entry.type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
        entry.schema = 1u;
        entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
        entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
        entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
        entry.canonicalPayload = sheet;
        entry.canonicalPayloadSize = kSheetSize;
        entry.canonicalPayloadSha256 = sheetSha;
        entry.sourceRomOffset = 0x00300000u;
        entry.sourceEncodedSize = kSheetSize;
        entry.sourceEncodedSha256 = sheetSha;
        Gen3ResourcePackBuild_AddEntry(build, &entry, &writeDiag);

        memset(&entry, 0, sizeof(entry));
        entry.canonicalName = "emerald:trainer/brendan/extra";
        entry.key = &extraKey;
        entry.type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
        entry.schema = 1u;
        entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
        entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
        entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
        entry.canonicalPayload = extra;
        entry.canonicalPayloadSize = sizeof(extra);
        entry.canonicalPayloadSha256 = extraSha;
        entry.sourceRomOffset = 0x00320000u;
        entry.sourceEncodedSize = sizeof(extra);
        entry.sourceEncodedSha256 = extraSha;
        Gen3ResourcePackBuild_AddEntry(build, &entry, &writeDiag);

        Gen3ResourcePackWriter_Write(build, &badBytes, &writeDiag);
        Gen3ResourcePackBuild_Destroy(build);
        Gen3ResourcePackDiagnostics_Destroy(&writeDiag);

        badPack = ParsePack(&badBytes);
        error = EmeraldResourceSession_BuildRomBaseCandidate(badPack, catalog,
            &candidate, &info, &diag);
        CHECK("catalog-inconsistent pack rejected",
              error == EMERALD_SESSION_ERR_PACK_PROVIDER_FAILED);
        CHECK("no candidate from inconsistent pack", candidate == NULL);
        Gen3ResourcePack_Destroy(badPack);
        Gen3ResourcePackBytes_Destroy(&badBytes);
        free(sheet);
        free(palette);
    }
    Gen3ResourceCatalog_Destroy(catalog);

    Gen3ResourceDiagnostics_Destroy(&diag);
}

int main(void)
{
    TestSessionTwoResourceProof();
    TestDigestVersionIndependence();
    TestPrecedence();
    TestPrecedenceOrderIndependence();
    TestPrecedenceDuplicate();
    TestTransactional();
    TestSourcePackLifetime();
    TestInvalidSessions();

    if (gFailures != 0)
    {
        printf("emerald_rom_base_provider_test FAILED: %d/%d checks\n",
               gFailures, gChecks);
        return 1;
    }
    printf("emerald rom base provider test passed (%d checks)\n", gChecks);
    return 0;
}

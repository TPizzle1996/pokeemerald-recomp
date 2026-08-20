/* R9 §8d: runtime loader registration test (emerald_runtime_loader.c).
 *
 * Drives EmeraldResourceCompat_RegisterRuntimeSnapshot - the R6 loader the
 * native startup path uses - end-to-end:
 *
 *   1. a trainer-ONLY pack (the 196-entry front+back family, built here with
 *      the pack writer from the same committed fixtures the R7B/R8 harness
 *      pins) must be REFUSED: R9 §8 makes registration publish through the
 *      strict init, and with the compiled Pokémon leaf payloads gone from the
 *      native link (R9 §7) a pack that cannot serve the Pokémon battle family
 *      is a refused session. The loader rolls everything back - no trainer or
 *      Pokémon slot may hold a published pointer afterwards - and returns
 *      EMERALD_COMPAT_ERR_RESOLVE_FAILED with the first missing Pokémon
 *      resource in its diagnostics;
 *   2. a NULL/empty/absent pack path is fail-closed UNAVAILABLE;
 *   3. the REAL production pack registers OK, the full trainer + Pokémon
 *      battle families are published into the live tables, and registration
 *      is idempotent thereafter.
 *
 * The loader source is compiled into this TU directly (native-target
 * guarded); the harness TU (renamed main) supplies the real tables, the
 * fixture loaders and the CHECK machinery, exactly as the production proof
 * reuses it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main emerald_runtime_loader_test_harness_main
#include "emerald_trainer_native_compat_test.c"
#undef main

#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack_writer.h"
#include "gen3/resources/sha256.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_rom_profile.h"
#include "emerald/resources/emerald_gameplay_compat.h"
#include "emerald/resources/emerald_trainer_compat.h"
#include "emerald/resources/trainer_native.generated.h"
#include "emerald/resources/text_skeleton_arrays.generated.h"
#include "wild_encounter.h"   /* R13-E2: real struct WildPokemon{Info,Header} +
                                 the NATIVE_LINUX gWildMonHeaders extern */
#include "emerald/resources/emerald_encounter_compat.h"
#include "item_use.h"   /* R13-D2: native ItemUse*_... symbols for pointer checks */
#include "emerald/resources/frontier_data_native.h"  /* R13-E3a-2 HOST_DATA fill
                                                       targets (pike/pyramid/brain/
                                                       factory/palace/arena) */
#include "apprentice.h"        /* R13-E3a-2 gApprentices fill target */
#include "constants/moves.h"   /* R13-E3a-2 factory move assertions */
#include "constants/items.h"   /* R13-E3a-2 pyramid pickup assertions */
#include "emerald/resources/emerald_pokedex_compat.h" /* R13-E3b Pokédex seam */
#include "emerald/resources/pokedex_data_native.h"    /* R13-E3b HOST_DATA targets */
#include "emerald/resources/emerald_map_compat.h"     /* R13-F map seam */
#include "emerald/resources/map_data_native.h"        /* R13-F gMapHeaders HOST_DATA */
#include "../src/emerald/resources/emerald_runtime_loader.c"

/* ------------------------------------------------------------------ */
/* Trainer-only pack construction (writer API, R2)                    */
/* ------------------------------------------------------------------ */

static void DigestSha256(const u8 *data, size_t size, u8 digest[32])
{
    struct Gen3Sha256Context ctx;
    Gen3Sha256_Init(&ctx);
    Gen3Sha256_Update(&ctx, data, size);
    Gen3Sha256_Final(&ctx, digest);
}

/* Build the 196-entry trainer pack (186 front + 10 back) from the harness
 * fixtures: canonical ids mirror the family descriptors exactly as the seam
 * builds them, payloads are the committed-artifact decodes (front) and raw
 * sheets (back), digests are computed, provenance fields are non-zero
 * placeholders (the loader never inspects ROM provenance). */
static bool BuildTrainerOnlyPack(const char *path)
{
    struct Gen3ResourcePackBuild *build = NULL;
    struct Gen3ResourcePackBytes bytes = { NULL, 0 };
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackProfileInput profile;
    struct Gen3ResourcePackEntryInput entry;
    Gen3ResourceKey derived;
    uint8_t sha[32];
    uint8_t provenanceSha[32];
    char id[128];
    FILE *f = NULL;
    size_t i;
    enum Gen3ResourcePackError packError;
    bool ok = false;

    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
        goto done;

    /* Deterministic synthetic identity: any valid profile serves the loader
     * (it derives the provider version from basePackVersion and never checks
     * ROM digests); the synthetic fixture digests are already in the link
     * (emerald_rom_profile.c). */
    memset(&profile, 0, sizeof(profile));
    profile.basePackVersion = 1u;
    profile.catalogVersion = 1u;
    profile.extractionManifestVersion = 1u;
    profile.canonicalRepresentationVersion = 1u;
    profile.sourceRomSize = EMERALD_ROM_SIZE;
    profile.sourceRomSha1 = kEmeraldProfileSyntheticSha1;
    profile.sourceRomSha256 = kEmeraldProfileSyntheticSha256;
    memcpy(profile.gameCode, EMERALD_PROFILE_GAME_CODE, 4u);
    memcpy(profile.makerCode, EMERALD_PROFILE_MAKER_CODE, 2u);
    profile.softwareRevision = EMERALD_PROFILE_SOFTWARE_REV;
    snprintf(profile.gameId, sizeof(profile.gameId), "%s", EMERALD_PROFILE_GAME);
    profile.catalogSha256 = kEmeraldProfileSyntheticSha256;
    profile.extractionManifestSha256 = kEmeraldProfileSyntheticSha256;
    packError = Gen3ResourcePackBuild_SetProfile(build, &profile, &diag);
    if (packError != GEN3_PACK_OK)
        goto done;

    /* Non-zero provenance digest + size (writer requires them; they are
     * never cross-checked against the payload). */
    memset(provenanceSha, 0x5A, sizeof(provenanceSha));

    memset(&entry, 0, sizeof(entry));
    entry.schema = 1u;
    entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
    entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
    entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
    entry.sourceEncodedSha256 = provenanceSha;

    /* Front family: 93 sheets (TILE_GRAPHICS) + 93 palettes (PALETTE). */
    for (i = 0; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        const struct FamilyTrainerFixture *fix = &sFixtures[i];

        BuildFamilyId(id, sizeof(id), fix->canonical, false);
        Gen3ResourceId_DeriveKey(id, &derived);
        DigestSha256(fix->sheet, EMERALD_TRAINER_SHEET_SIZE, sha);
        entry.canonicalName = id;
        entry.key = &derived;
        entry.type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
        entry.canonicalPayload = fix->sheet;
        entry.canonicalPayloadSize = EMERALD_TRAINER_SHEET_SIZE;
        entry.canonicalPayloadSha256 = sha;
        entry.sourceEncodedSize = EMERALD_TRAINER_SHEET_SIZE;
        packError = Gen3ResourcePackBuild_AddEntry(build, &entry, &diag);
        if (packError != GEN3_PACK_OK)
            goto done;

        BuildFamilyId(id, sizeof(id), fix->canonical, true);
        Gen3ResourceId_DeriveKey(id, &derived);
        DigestSha256(fix->palette, EMERALD_TRAINER_PALETTE_SIZE, sha);
        entry.canonicalName = id;
        entry.key = &derived;
        entry.type = GEN3_RESOURCE_TYPE_PALETTE;
        entry.canonicalPayload = fix->palette;
        entry.canonicalPayloadSize = EMERALD_TRAINER_PALETTE_SIZE;
        entry.canonicalPayloadSha256 = sha;
        entry.sourceEncodedSize = EMERALD_TRAINER_PALETTE_SIZE;
        packError = Gen3ResourcePackBuild_AddEntry(build, &entry, &diag);
        if (packError != GEN3_PACK_OK)
            goto done;
    }

    /* Back family: 8 RAW sheets + the 2 Red/Leaf palettes (only back
     * fixtures with a palette). */
    for (i = 0; i < EMERALD_TRAINER_BACK_SHEET_COUNT; i++)
    {
        const struct FamilyBackFixture *fix = &sBackFixtures[i];

        BuildBackFamilyId(id, sizeof(id), fix->canonical, false);
        Gen3ResourceId_DeriveKey(id, &derived);
        DigestSha256(fix->sheet, fix->sheetSize, sha);
        entry.canonicalName = id;
        entry.key = &derived;
        entry.type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
        entry.representation = GEN3_PACK_REPRESENTATION_RAW;
        entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_RAW;
        entry.canonicalPayload = fix->sheet;
        entry.canonicalPayloadSize = fix->sheetSize;
        entry.canonicalPayloadSha256 = sha;
        entry.sourceEncodedSize = fix->sheetSize;
        packError = Gen3ResourcePackBuild_AddEntry(build, &entry, &diag);
        if (packError != GEN3_PACK_OK)
            goto done;
        entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
        entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;

        if (fix->palette != NULL)
        {
            BuildBackFamilyId(id, sizeof(id), fix->canonical, true);
            Gen3ResourceId_DeriveKey(id, &derived);
            DigestSha256(fix->palette, EMERALD_TRAINER_PALETTE_SIZE, sha);
            entry.canonicalName = id;
            entry.key = &derived;
            entry.type = GEN3_RESOURCE_TYPE_PALETTE;
            entry.canonicalPayload = fix->palette;
            entry.canonicalPayloadSize = EMERALD_TRAINER_PALETTE_SIZE;
            entry.canonicalPayloadSha256 = sha;
            entry.sourceEncodedSize = EMERALD_TRAINER_PALETTE_SIZE;
            packError = Gen3ResourcePackBuild_AddEntry(build, &entry, &diag);
            if (packError != GEN3_PACK_OK)
                goto done;
        }
    }

    packError = Gen3ResourcePackWriter_Write(build, &bytes, &diag);
    if (packError != GEN3_PACK_OK)
        goto done;
    f = fopen(path, "wb");
    if (f == NULL)
        goto done;
    if (fwrite(bytes.data, 1, bytes.size, f) != bytes.size)
        goto done;
    fclose(f);
    f = NULL;
    ok = true;

done:
    if (f != NULL)
        fclose(f);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* Fail-closed path handling: nothing registers, nothing publishes. */
static void TestLoaderUnavailable(void)
{
    CHECK("NULL path is UNAVAILABLE",
          EmeraldResourceCompat_RegisterRuntimeSnapshot(NULL)
              == EMERALD_COMPAT_ERR_UNAVAILABLE);
    CHECK("empty path is UNAVAILABLE",
          EmeraldResourceCompat_RegisterRuntimeSnapshot("")
              == EMERALD_COMPAT_ERR_UNAVAILABLE);
    CHECK("absent path is UNAVAILABLE",
          EmeraldResourceCompat_RegisterRuntimeSnapshot(
              "tests/definitely-not-a-real-pack.rpack")
              == EMERALD_COMPAT_ERR_UNAVAILABLE);
}

/* A pack that cannot serve the Pokémon battle family is a refused session:
 * strict init fails, the loader rolls back every published pointer (trainer
 * family included) and the tables end at their NULL sentinels. */
static void TestLoaderRefusesTrainerOnlyPack(const char *tempDir)
{
    enum EmeraldResourceCompatStatus status;
    size_t i;
    char path[512];

    snprintf(path, sizeof(path), "%s/trainer_only.rpack", tempDir);
    CHECK("trainer-only pack builds", BuildTrainerOnlyPack(path));

    /* Clean slate (nothing registered in this process yet). */
    EmeraldResourceCompat_ClearMigratedEntries();
    EmeraldResourceCompat_ClearSnapshot();

    status = EmeraldResourceCompat_RegisterRuntimeSnapshot(path);
    CHECK("trainer-only pack refused (RESOLVE_FAILED)",
          status == EMERALD_COMPAT_ERR_RESOLVE_FAILED);

    /* Rolled back: every migrated trainer slot back at its sentinel. */
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
        CHECK("refused: front sheet sentinel",
              gTrainerFrontPicTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicPaletteTable); i++)
        CHECK("refused: front palette sentinel",
              gTrainerFrontPicPaletteTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicPaletteTable); i++)
        CHECK("refused: back palette sentinel",
              gTrainerBackPicPaletteTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
        CHECK("refused: back sheet sentinel",
              gTrainerBackPicTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        size_t f;
        for (f = 0; f < sBackFixtures[i].frames; f++)
            CHECK("refused: back frame sentinel",
                  kBackFrameArrays[i][f].data == NULL);
    }

    /* Rolled back: every Pokémon battle slot at its sentinel. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("refused: mon front sentinel", gMonFrontPicTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("refused: mon back sentinel", gMonBackPicTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("refused: mon palette sentinel", gMonPaletteTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("refused: mon shiny sentinel", gMonShinyPaletteTable[i].data == NULL);

    /* Not registered: a second attempt re-opens and refuses identically. */
    status = EmeraldResourceCompat_RegisterRuntimeSnapshot(path);
    CHECK("refusal is repeatable", status == EMERALD_COMPAT_ERR_RESOLVE_FAILED);
}

/* The REAL production pack registers OK: the full trainer + Pokémon battle
 * families publish, and registration is at-most-once thereafter. */
static void TestLoaderProductionPack(const char *packPath)
{
    enum EmeraldResourceCompatStatus status;

    status = EmeraldResourceCompat_RegisterRuntimeSnapshot(packPath);
    CHECK("production pack registers", status == EMERALD_COMPAT_OK);
    if (status != EMERALD_COMPAT_OK)
        return;

    /* Trainer family published into the live tables. */
    CHECK("production: front sheet 0 published",
          gTrainerFrontPicTable[0].data != NULL);
    CHECK("production: front palette 0 published",
          gTrainerFrontPicPaletteTable[0].data != NULL);
    CHECK("production: back sheet 0 published",
          gTrainerBackPicTable[0].data != NULL);

    /* Pokémon battle family published. */
    CHECK("production: mon front 0 published",
          gMonFrontPicTable[0].data != NULL);
    CHECK("production: mon back 0 published",
          gMonBackPicTable[0].data != NULL);
    CHECK("production: mon palette 0 published",
          gMonPaletteTable[0].data != NULL);
    CHECK("production: mon shiny 0 published",
          gMonShinyPaletteTable[0].data != NULL);

    /* Idempotent: a second registration is a no-op success. */
    status = EmeraldResourceCompat_RegisterRuntimeSnapshot(packPath);
    CHECK("registration idempotent", status == EMERALD_COMPAT_OK);
}

/* R13-F focused publication proof.  A successful top-level registration is
 * necessary but not sufficient for this harness: pin the complete map count,
 * populated header metadata, and representative schema-43/44 arena identities
 * in the same range index State-v5 consumes.  This offline harness's host shim
 * deliberately supplies a zero gMapLayouts routing table; the real maps.o
 * layout edges are covered by the native-world harnesses. */
static void TestMapPublication(void)
{
    const struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    bool foundEvents = false;
    bool foundConnections = false;
    bool foundHeaderMetadata = false;
    size_t i;

    CHECK("R13-F map resource count",
          EmeraldMapCompat_GetPublishedCount()
              == EMERALD_MAP_HEADER_COUNT + EMERALD_MAP_LAYOUT_COUNT
               + EMERALD_MAP_EVENT_COUNT + EMERALD_MAP_CONNECTION_COUNT);
    CHECK("R13-F event arena published",
          EmeraldMapCompat_GetEventArenaBytes() != 0u);
    CHECK("R13-F connection arena published",
          EmeraldMapCompat_GetConnArenaBytes() != 0u);
    CHECK("R13-F range index available", index != NULL);

    for (i = 0u; i < EMERALD_MAP_HEADER_COUNT; i++)
    {
        struct EmeraldResourceRangeHit hit;

        if (gMapHeaders[i].mapLayoutId != 0u
         || gMapHeaders[i].music != 0u
         || gMapHeaders[i].mapScripts != NULL)
            foundHeaderMetadata = true;
        if (!foundEvents && gMapHeaders[i].events != NULL && index != NULL
         && EmeraldResourceRangeIndex_Lookup(
                index, (uintptr_t)gMapHeaders[i].events, &hit))
        {
            foundEvents = hit.type == GEN3_RESOURCE_TYPE_STRUCTURED_DATA
                       && hit.schema == EMERALD_MAP_SCHEMA_EVENTS
                       && hit.role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT;
        }
        if (!foundConnections && gMapHeaders[i].connections != NULL
         && index != NULL
         && EmeraldResourceRangeIndex_Lookup(
                index, (uintptr_t)gMapHeaders[i].connections, &hit))
        {
            foundConnections = hit.type == GEN3_RESOURCE_TYPE_STRUCTURED_DATA
                            && hit.schema == EMERALD_MAP_SCHEMA_CONNECTIONS
                            && hit.role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT;
        }
    }
    CHECK("R13-F header metadata populated", foundHeaderMetadata);
    CHECK("R13-F schema-43 event range", foundEvents);
    CHECK("R13-F schema-44 connection range", foundConnections);
}

/* ------------------------------------------------------------------ */
/* R13-C: text family focused tests                                   */
/* ------------------------------------------------------------------ */

/* The production pack's gText_123Dot record (#112): "1.\xff2.\xff3.\xff"
 * as a single 9-byte resource, three 3-byte strings tiled by kind-0
 * skeleton fills at byte offsets 0/3/6. */
static const uint8_t k123DotBytes[9] = {
    0xA2, 0xAD, 0xFF, 0xA3, 0xAD, 0xFF, 0xA4, 0xAD, 0xFF
};
#define k123DotId "emerald:text/system/gtext-123dot"

/* Writer-rebuilt pack variants: the full production catalog with exactly
 * one text record damaged (payload zeroed -> escape grammar) or dropped
 * (-> inventory mismatch). Every other entry is copied through verbatim
 * with its own digests, so the variant passes every seam but the text
 * one - the refusal is provably the text record's. */
static bool BuildTextVariantPack(const char *srcPath, const char *dstPath,
                                 bool zero123Dot, bool drop123Dot)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourcePackProfile profile;
    struct Gen3ResourcePackBuild *build = NULL;
    struct Gen3ResourcePackProfileInput pin;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackBytes bytes = { NULL, 0 };
    struct Gen3ResourcePackDiagnosticList diag;
    uint8_t zero[9] = { 0 };
    uint8_t zeroSha[32];
    uint8_t provenanceSha[32];
    size_t count;
    size_t i;
    FILE *f = NULL;
    bool ok = false;

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(srcPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
        goto done;
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);

    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
        goto done;

    /* The production pack's own profile: the provider identity and
     * version derive from it exactly as the production session's, so the
     * variant behaves identically in every seam but the one record. */
    if (!Gen3ResourcePack_GetProfile(pack, &profile))
        goto done;
    memset(&pin, 0, sizeof(pin));
    pin.basePackVersion = profile.basePackVersion;
    pin.catalogVersion = profile.catalogVersion;
    pin.extractionManifestVersion = profile.extractionManifestVersion;
    pin.canonicalRepresentationVersion = profile.canonicalRepresentationVersion;
    pin.sourceRomSize = profile.sourceRomSize;
    /* ProfileInput takes the SHA digests by pointer (the profile holds
     * them inline); the digests outlive SetProfile because `profile`
     * lives in this frame and SetProfile copies them. */
    pin.sourceRomSha1 = profile.sourceRomSha1;
    pin.sourceRomSha256 = profile.sourceRomSha256;
    memcpy(pin.gameCode, profile.gameCode, 4u);
    memcpy(pin.makerCode, profile.makerCode, 2u);
    pin.softwareRevision = profile.softwareRevision;
    memcpy(pin.gameId, profile.gameId, GEN3_PACK_GAME_ID_SIZE);
    pin.catalogSha256 = profile.catalogSha256;
    pin.extractionManifestSha256 = profile.extractionManifestSha256;
    if (Gen3ResourcePackBuild_SetProfile(build, &pin, &diag) != GEN3_PACK_OK)
        goto done;

    memset(provenanceSha, 0x5A, sizeof(provenanceSha));
    DigestSha256(zero, sizeof(zero), zeroSha);

    count = Gen3ResourcePack_GetEntryCount(pack);
    for (i = 0; i < count; i++)
    {
        const struct Gen3ResourcePackEntry *e =
            Gen3ResourcePack_GetEntry(pack, i);
        bool is123Dot = strcmp(e->canonicalName, k123DotId) == 0;

        if (is123Dot && drop123Dot)
            continue;
        memset(&entry, 0, sizeof(entry));
        entry.schema = e->schema;
        entry.flags = e->flags;
        entry.representation = e->representation;
        entry.sourceEncoding = e->sourceEncoding;
        entry.canonicalName = e->canonicalName;
        entry.key = &e->key;
        entry.type = e->type;
        entry.canonicalPayload =
            is123Dot ? zero : e->payload;
        entry.canonicalPayloadSize = e->payloadSize;
        entry.canonicalPayloadSha256 =
            is123Dot ? zeroSha : e->payloadSha256;
        entry.sourceRomOffset = e->sourceRomOffset;
        entry.sourceEncodedSize = e->sourceEncodedSize;
        entry.sourceEncodedSha256 = provenanceSha;
        if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
            goto done;
    }

    if (Gen3ResourcePackWriter_Write(build, &bytes, &diag) != GEN3_PACK_OK)
        goto done;
    f = fopen(dstPath, "wb");
    if (f == NULL)
        goto done;
    if (fwrite(bytes.data, 1, bytes.size, f) != bytes.size)
        goto done;
    fclose(f);
    f = NULL;
    ok = true;

done:
    if (f != NULL)
        fclose(f);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    if (pack != NULL)
        Gen3ResourcePack_Destroy(pack);
    return ok;
}

/* The text seam against a freshly built session (the same pack ->
 * catalog -> candidate -> snapshot chain the loader runs, driven
 * directly so the loader's at-most-once registration is not consumed by
 * the refusal probes). */
static enum EmeraldTextCompatStatus RunTextSeamDirect(
    const char *packPath, struct EmeraldTextCompatDiagnostics *diag)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourceCatalog *catalog = NULL;
    struct Gen3ResourceCandidate *candidate = NULL;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList diagnostics;
    struct EmeraldResourceSessionInfo info;
    enum EmeraldResourceSessionError sessionError;
    enum EmeraldTextCompatStatus status = EMERALD_TEXT_ERR_UNAVAILABLE;

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
    {
        Gen3ResourcePackDiagnostics_Destroy(&packDiag);
        return status;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    if (!BuildCatalogFromPack(pack, &catalog) || catalog == NULL)
        goto done;
    Gen3ResourceDiagnostics_Init(&diagnostics);
    sessionError = EmeraldResourceSession_BuildRomBaseCandidate(
        pack, catalog, &candidate, &info, &diagnostics);
    if (sessionError == EMERALD_SESSION_OK && candidate != NULL
     && Gen3ResourceCandidate_Build(candidate, &snapshot, &diagnostics)
     && snapshot != NULL)
        status = EmeraldTextCompat_TryInitialize(snapshot, pack, diag);
    Gen3ResourceDiagnostics_Destroy(&diagnostics);
done:
    if (snapshot != NULL)
        Gen3ResourceSnapshot_Destroy(snapshot);
    if (candidate != NULL)
        Gen3ResourceCandidate_Destroy(candidate);
    if (catalog != NULL)
        Gen3ResourceCatalog_Destroy(catalog);
    if (pack != NULL)
        Gen3ResourcePack_Destroy(pack);
    return status;
}

/* A production pack whose text family is malformed (record payload
 * zeroed) or incomplete (record dropped) is a REFUSED session: the
 * loader publishes the earlier seams, then rolls EVERYTHING back - no
 * trainer/Pokémon/text pointer survives and the snapshot is not
 * registered, so a repeat attempt refuses identically. */
static void TestLoaderRefusesTextVariant(const char *tempDir,
                                         const char *prodPack,
                                         bool zero123Dot, bool drop123Dot,
                                         const char *label)
{
    enum EmeraldResourceCompatStatus status;
    char path[512];

    snprintf(path, sizeof(path), "%s/%s.rpack", tempDir, label);
    CHECK("text variant pack builds",
          BuildTextVariantPack(prodPack, path, zero123Dot, drop123Dot));

    /* Clean slate (nothing registered in this process yet). */
    EmeraldResourceCompat_ClearMigratedEntries();
    EmeraldResourceCompat_ClearSnapshot();

    status = EmeraldResourceCompat_RegisterRuntimeSnapshot(path);
    CHECK("text variant refused (PUBLISH_FAILED)",
          status == EMERALD_COMPAT_ERR_PUBLISH_FAILED);

    /* Rolled back: no session, no text arena, no fills, no trainer or
     * Pokémon pointers. */
    CHECK("refused: session not registered",
          !EmeraldResourceCompat_IsSessionRegistered());
    CHECK("refused: text unpublished",
          EmeraldTextCompat_GetPublishedCount() == 0u);
    CHECK("refused: #112 fills NULL",
          gText_123Dot[0] == NULL && gText_123Dot[1] == NULL
              && gText_123Dot[2] == NULL);
    CHECK("refused: trainer front sheet sentinel",
          gTrainerFrontPicTable[0].data == NULL);
    CHECK("refused: mon front sentinel",
          gMonFrontPicTable[0].data == NULL);

    /* Not registered: a second attempt re-opens and refuses identically. */
    status = EmeraldResourceCompat_RegisterRuntimeSnapshot(path);
    CHECK("text refusal is repeatable",
          status == EMERALD_COMPAT_ERR_PUBLISH_FAILED);
}

/* The production registration published the text family: sixteen arenas
 * with the pinned composition, every pack text entry byte-identical in
 * its arena (C labels by resource id, bundle labels by blob slice), and
 * the #112 nine-byte record resolving whole. */
static void TestTextPublication(const char *packPath)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct { const char *id; const uint8_t *payload; size_t size; }
        blobs[TEXT_BUNDLE_COUNT];
    const uint8_t *bytes;
    size_t size;
    size_t packCount;
    size_t i;
    size_t total = 0u;
    size_t textEntries = 0u;
    size_t labelRecords = 0u;
    size_t bundleRecords = 0u;
    size_t blobsCount = 0u;

    CHECK("16 family arenas",
          EmeraldTextCompat_GetArenaCount() == TEXT_ARENA_COUNT);
    CHECK("published label count",
          EmeraldTextCompat_GetPublishedCount() == EMERALD_TEXT_LABEL_COUNT);

    for (i = 0u; i < TEXT_ARENA_COUNT; i++)
    {
        const uint8_t *base;
        size_t sz;
        CHECK("arena published",
              EmeraldTextCompat_GetArenaByKey(kTextArenaSummaries[i].key,
                                              &base, &sz));
        if (base != NULL)
            total += sz;
    }
    CHECK("arena byte total == 903157", total == EMERALD_TEXT_TOTAL_BYTES);

    /* #112: the single 9-byte record resolves whole. */
    CHECK("123Dot record resolves",
          EmeraldTextCompat_GetResourceBytes(k123DotId, &bytes, &size));
    CHECK("123Dot record size", size == 9u);
    CHECK("123Dot record bytes",
          size == 9u && memcmp(bytes, k123DotBytes, 9u) == 0);

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
    {
        CHECK("production pack opens for verification", false);
        Gen3ResourcePackDiagnostics_Destroy(&packDiag);
        return;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);

    packCount = Gen3ResourcePack_GetEntryCount(pack);
    printf("production pack entry count: %zu\n", packCount);
    CHECK("production pack entry count pinned at 18521+59 (R13-E3a-2)",
          packCount == 20501u);

    /* Every text entry: C-side labels resolve by resource id; bundle ids
     * are captured for the blob-slice pass below. */
    for (i = 0u; i < packCount; i++)
    {
        const struct Gen3ResourcePackEntry *e =
            Gen3ResourcePack_GetEntry(pack, i);
        const char *dash;
        bool isBundle = false;
        size_t lo = 0u;
        size_t hi = TEXT_BUNDLE_COUNT;
        size_t mid;

        if (e->type != GEN3_RESOURCE_TYPE_TEXT || e->schema != 1u)
            continue;
        textEntries++;
        /* Bundle id? (kTextBundleIds is sorted.) */
        while (lo < hi)
        {
            mid = lo + (hi - lo) / 2u;
            int cmp = strcmp(e->canonicalName, kTextBundleIds[mid]);
            if (cmp == 0)
            {
                isBundle = true;
                break;
            }
            if (cmp < 0)
                hi = mid;
            else
                lo = mid + 1u;
        }
        if (isBundle)
        {
            bundleRecords++;
            CHECK("bundle entry has a payload", e->payload != NULL);
            if (blobsCount < TEXT_BUNDLE_COUNT)
            {
                blobs[blobsCount].id = e->canonicalName;
                blobs[blobsCount].payload = e->payload;
                blobs[blobsCount].size = e->payloadSize;
                blobsCount++;
            }
            continue;
        }
        labelRecords++;
        CHECK("C label resolves in arena",
              EmeraldTextCompat_GetResourceBytes(e->canonicalName,
                                                 &bytes, &size));
        CHECK("C label size matches", size == e->payloadSize);
        CHECK("C label arena bytes == pack bytes",
              bytes != NULL && size == e->payloadSize
                  && memcmp(bytes, e->payload, size) == 0);
        dash = strrchr(e->canonicalName, '/');
        CHECK("C label id ends with a dash-form name",
              dash != NULL && dash[1] != '\0');
    }
    printf("text entries: %zu (%zu C labels + %zu bundles)\n",
           textEntries, labelRecords, bundleRecords);
    CHECK("text composition pinned (4824 + 363)",
          labelRecords == TEXT_NATIVE_LABEL_COUNT
              && bundleRecords == TEXT_NATIVE_BUNDLE_COUNT
              && textEntries == TEXT_NATIVE_RESOURCE_COUNT);

    /* Every bundle-local label's arena slice == its blob slice. */
    for (i = 0u; i < TEXT_BUNDLE_ENTRY_COUNT; i++)
    {
        const struct TextBundleEntry *be = &kTextBundleIndex[i];
        const uint8_t *blob = NULL;
        size_t blobSize = 0u;
        size_t k;

        for (k = 0u; k < blobsCount; k++)
        {
            if (strcmp(blobs[k].id, kTextBundleIds[be->bundleIndex]) == 0)
            {
                blob = blobs[k].payload;
                blobSize = blobs[k].size;
                break;
            }
        }
        CHECK("bundle blob captured",
              blob != NULL && be->blobOffset + be->size <= blobSize);
        CHECK("bundle label resolves",
              EmeraldTextCompat_GetLabelBytes(be->name, &bytes, &size));
        CHECK("bundle label size matches", size == be->size);
        CHECK("bundle label slice == blob",
              bytes != NULL && blob != NULL && size == be->size
                  && memcmp(bytes, blob + be->blobOffset, size) == 0);
    }

    Gen3ResourcePack_Destroy(pack);
}

/* #112: the kind-0 byte-offset fills publish gText_123Dot's three slices
 * at its label's payload start + 0/3/6 - the fill row is a byte offset
 * INTO the label, never an arena-absolute offset (the label sorts at a
 * deterministic non-zero position inside the system-shared arena). */
static void TestTextSkeletonFills(void)
{
    const uint8_t *labelBytes;
    size_t labelSize;

    CHECK("#112 label published",
          EmeraldTextCompat_GetResourceBytes(k123DotId, &labelBytes,
                                             &labelSize));
    if (labelBytes == NULL)
        return;
    CHECK("#112 label is the 9-byte record", labelSize == 9u);
    CHECK("#112 fill[0] at label payload start", gText_123Dot[0] == labelBytes);
    CHECK("#112 fill[1] at label payload + 3", gText_123Dot[1] == labelBytes + 3);
    CHECK("#112 fill[2] at label payload + 6", gText_123Dot[2] == labelBytes + 6);
    CHECK("#112 fills are arena pointers",
          EmeraldTextCompat_ContainsPointer((uintptr_t)gText_123Dot[0])
              && EmeraldTextCompat_ContainsPointer((uintptr_t)gText_123Dot[1])
              && EmeraldTextCompat_ContainsPointer((uintptr_t)gText_123Dot[2]));
    CHECK("#112 row[0] bytes",
          gText_123Dot[0] != NULL
              && memcmp(gText_123Dot[0], k123DotBytes + 0, 3) == 0);
    CHECK("#112 row[1] bytes",
          gText_123Dot[1] != NULL
              && memcmp(gText_123Dot[1], k123DotBytes + 3, 3) == 0);
    CHECK("#112 row[2] bytes",
          gText_123Dot[2] != NULL
              && memcmp(gText_123Dot[2], k123DotBytes + 6, 3) == 0);
}

/* Every one of the 3,118 generated slot bindings points at the start of
 * its own label inside a family arena (the label's deterministic sorted
 * record start, not arena offset 0 - only one record per arena can sit
 * at the arena's first byte). */
static void TestTextSlotPointers(void)
{
    size_t i;

    for (i = 0u; i < TEXT_SLOT_BINDING_COUNT; i++)
    {
        const struct TextSlotBinding *b = &kTextSlotBindings[i];
        const uint8_t *ptr = *b->slot;
        const char *dash;
        const char *name;
        size_t arenaIndex;
        size_t offset;
        size_t start;
        size_t size;

        CHECK("slot published", ptr != NULL);
        if (ptr == NULL)
            continue;
        CHECK("slot is an arena pointer",
              EmeraldTextCompat_ContainsPointer((uintptr_t)ptr));
        if (!EmeraldTextCompat_GetArenaForPointer((uintptr_t)ptr,
                                                  &arenaIndex, &offset))
            continue;
        if (!EmeraldTextCompat_GetLabelAtOffset((uint32_t)arenaIndex, offset,
                                                &name, &start, &size))
            continue;
        dash = strrchr(b->name, '/');
        CHECK("slot points at its label's start",
              dash != NULL && offset == start && strcmp(name, dash + 1) == 0);
    }
}

/* State-v5 currentChar routing: a mid-string pointer (gText_123Dot[1],
 * 3 bytes into its label) hits the arena range, round-trips by key +
 * offset, and resolves to the enclosing label with the in-label offset
 * recoverable as hit-offset - label-start. */
static void TestTextCurrentCharRouting(void)
{
    struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    struct EmeraldResourceRangeHit hit;
    Gen3ResourceKey arenaKey;
    uintptr_t address;
    uintptr_t resolved;
    size_t arenaIndex = TEXT_ARENA_COUNT;
    size_t offset = 0u;
    size_t start;
    size_t size;
    const char *name;
    size_t a;
    bool arenaFound = false;

    CHECK("range index live", index != NULL);
    if (index == NULL)
        return;
    address = (uintptr_t)gText_123Dot[1];
    CHECK("mid-string pointer published", address != 0u);
    if (address == 0u)
        return;
    CHECK("mid-string pointer is a registered range",
          EmeraldResourceRangeIndex_Lookup(index, address, &hit));

    /* The enclosing arena identity: system-shared is family arena 6. */
    for (a = 0u; a < TEXT_ARENA_COUNT; a++)
    {
        if (strcmp(kTextArenaSummaries[a].key, "system-shared") == 0)
        {
            arenaIndex = a;
            arenaFound = true;
            break;
        }
    }
    CHECK("system-shared arena found", arenaFound);
    Gen3ResourceId_DeriveKey("emerald:text/arena/system-shared", &arenaKey);
    CHECK("hit key is the arena identity",
          Gen3ResourceId_KeyEqual(&hit.key, &arenaKey));
    CHECK("hit type text", hit.type == GEN3_RESOURCE_TYPE_TEXT);
    CHECK("hit schema 1", hit.schema == 1u);
    CHECK("hit role compat-object",
          hit.role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT);

    /* Load-direction round trip: key + offset reconstruct the pointer. */
    CHECK("ResolveByKey round-trips the mid-string pointer",
          EmeraldResourceRangeIndex_ResolveByKey(index, &hit.key,
                                                 hit.type, hit.schema,
                                                 hit.role, hit.rangeOffset,
                                                 &resolved));
    CHECK("resolved pointer == the live slot", resolved == address);

    /* The walker's label-span validation at the same offset: the label
     * sorts at a deterministic non-zero record start inside the
     * system-shared arena, so the mid-string offsets are label-relative
     * (hit/offset = label start + 3), and the hit and the walker must
     * agree on the same zone-relative offset. */
    CHECK("GetArenaForPointer locates the arena",
          EmeraldTextCompat_GetArenaForPointer(address, &arenaIndex,
                                               &offset));
    CHECK("GetLabelAtOffset identifies the label",
          EmeraldTextCompat_GetLabelAtOffset((uint32_t)arenaIndex, offset,
                                             &name, &start, &size));
    CHECK("label is gtext-123dot",
          name != NULL && strcmp(name, "gtext-123dot") == 0);
    CHECK("label span is the whole record", size == 9u);
    CHECK("walker offset is label start + 3", offset == start + 3u);
    CHECK("hit offset is label start + 3", hit.rangeOffset == start + 3u);
}

/* A failed direct republish (malformed/missing variant) must not disturb
 * the loader's live session; the fail-closed clear then NULLs every
 * applied pointer before the arena frees. */
static void TestTextTransactionalRefusal(const char *tempDir,
                                         const char *prodPack)
{
    struct EmeraldTextCompatDiagnostics diag;
    const uint8_t *liveBytes;
    size_t liveSize;
    enum EmeraldTextCompatStatus status;
    char path[512];

    CHECK("live session registered",
          EmeraldResourceCompat_IsSessionRegistered());
    CHECK("live arena published",
          EmeraldTextCompat_GetPublishedCount() == EMERALD_TEXT_LABEL_COUNT);
    CHECK("live #112 slice published", gText_123Dot[1] != NULL);
    CHECK("live record captured",
          EmeraldTextCompat_GetResourceBytes(k123DotId, &liveBytes,
                                             &liveSize));

    /* Malformed: the record's payload fails the escape grammar. */
    snprintf(path, sizeof(path), "%s/text_malformed.rpack", tempDir);
    CHECK("malformed variant builds",
          BuildTextVariantPack(prodPack, path, true, false));
    memset(&diag, 0, sizeof(diag));
    status = RunTextSeamDirect(path, &diag);
    CHECK("malformed text refused (ESCAPE_GRAMMAR)",
          status == EMERALD_TEXT_ERR_ESCAPE_GRAMMAR);
    CHECK("malformed names the record",
          strcmp(diag.canonicalName, k123DotId) == 0);

    /* The live session survives the failed republish byte-for-byte. */
    CHECK("live session survives", EmeraldResourceCompat_IsSessionRegistered());
    CHECK("live arena survives",
          EmeraldTextCompat_GetPublishedCount() == EMERALD_TEXT_LABEL_COUNT);
    CHECK("live #112 slice survives", gText_123Dot[1] != NULL);

    /* Missing: the record is dropped -> inventory mismatch. */
    snprintf(path, sizeof(path), "%s/text_missing.rpack", tempDir);
    CHECK("missing variant builds",
          BuildTextVariantPack(prodPack, path, false, true));
    memset(&diag, 0, sizeof(diag));
    status = RunTextSeamDirect(path, &diag);
    CHECK("missing text refused (TABLE_MISMATCH)",
          status == EMERALD_TEXT_ERR_TABLE_MISMATCH);
    CHECK("missing names the record",
          strcmp(diag.canonicalName, k123DotId) == 0);
    CHECK("live session survives", EmeraldResourceCompat_IsSessionRegistered());
    CHECK("live arena survives",
          EmeraldTextCompat_GetPublishedCount() == EMERALD_TEXT_LABEL_COUNT);
    CHECK("live #112 slice survives", gText_123Dot[1] != NULL);

    /* Fail-closed clear: every applied fill NULLs before the arena frees. */
    EmeraldTextCompat_ClearMigratedEntries();
    CHECK("cleared: no published arena",
          EmeraldTextCompat_GetPublishedCount() == 0u);
    CHECK("cleared: #112 fills NULL",
          gText_123Dot[0] == NULL && gText_123Dot[1] == NULL
              && gText_123Dot[2] == NULL);
    CHECK("cleared: no arena pointers",
          !EmeraldTextCompat_ContainsPointer((uintptr_t)liveBytes));
}

/* ------------------------------------------------------------------ */
/* R13-D1: gameplay-data publication                                  */
/* ------------------------------------------------------------------ */

/* The D1 fill arrays are defined by gameplay_data_native.c (linked into
 * this harness). The harness TU includes the engine pokemon.h / data.h, so
 * the real fill-target structs/types are visible; elements are
 * index-addressed by species id (SPECIES_BULBASAUR = 1) / move id
 * (MOVE_POUND = 1). */
extern u16 gEggMoves[];                 /* defined as u16[1139] */
extern u16 *gLevelUpLearnsets[];        /* defined as u16*[412] */
extern u16 gFontNormalJapaneseGlyphs[]; /* defined as u16[8192] */
extern u16 gFontNormalLatinGlyphs[];    /* defined as u16[16384] */
/* R13-D2: the gameplay seam's HOST_DATA item fill target (struct Item from
 * item.h, already in scope via emerald_gameplay_compat.h). */
extern struct Item gItems[ITEMS_COUNT];

static void TestGameplayPublication(const char *packPath)
{
    size_t rangeCount;
    unsigned int i;
    int nonzero = 0;

    (void)packPath;
    (void)gSpeciesNames; (void)gMoveNames;

    CHECK("R13-D1 published",
          EmeraldGameplayCompat_GetPublishedCount()
              == GAMEPLAY_NATIVE_RESOURCE_COUNT);

    /* Species base stats: bulbasaur baseHP 45. */
    CHECK("R13-D1 bulbasaur baseHP 45",
          gSpeciesInfo[SPECIES_BULBASAUR].baseHP == 45u);
    /* Species name "BULBASAUR" first charmap byte 0xBC. */
    CHECK("R13-D1 bulbasaur name 0xBC",
          gSpeciesNames[SPECIES_BULBASAUR][0] == 0xBC);
    /* Move pound: type is TYPE_NORMAL. */
    CHECK("R13-D1 pound type normal",
          gBattleMoves[MOVE_POUND].type == 0u);
    /* Move name "POUND" first charmap byte 0xCA. */
    CHECK("R13-D1 pound name 0xCA",
          gMoveNames[MOVE_POUND][0] == 0xCA);
    /* Egg-moves stream header: gEggMoves[0] == 20000 + SPECIES_BULBASAUR. */
    CHECK("R13-D1 egg stream header",
          gEggMoves[0u] == (u16)(20000u + SPECIES_BULBASAUR));
    /* Level-up learnset pointers are published and the "none" row shares
     * bulbasaur's leaf (vanilla sharing). */
    CHECK("R13-D1 bulbasaur learnset published",
          gLevelUpLearnsets[SPECIES_BULBASAUR] != NULL);
    CHECK("R13-D1 none==bulbasaur learnset share",
          gLevelUpLearnsets[SPECIES_NONE] == gLevelUpLearnsets[SPECIES_BULBASAUR]);

    /* A Japanese + a Latin font glyph array were filled (not all-zero). */
    nonzero = 0;
    for (i = 0u; i < 8192u; i++)
        if (gFontNormalJapaneseGlyphs[i] != 0u) { nonzero = 1; break; }
    CHECK("R13-D1 normal-japanese font filled", nonzero != 0);
    nonzero = 0;
    for (i = 0u; i < 16384u; i++)
        if (gFontNormalLatinGlyphs[i] != 0u) { nonzero = 1; break; }
    CHECK("R13-D1 normal-latin font filled", nonzero != 0);

    /* Arena ranges registered: the levelup arena + egg array + fonts. */
    {
        const struct EmeraldResourceRangeIndex *index =
            EmeraldResourceCompat_GetRangeIndex();
        CHECK("R13-D1 range index present", index != NULL);
        if (index != NULL)
        {
            rangeCount = index->rangeCount;
            printf("D1 range index count: %zu\n", rangeCount);
            CHECK("R13-D1 gameplay ranges registered",
                  rangeCount >= (size_t)(EMERALD_GAMEPLAY_FONT_COUNT + 2u));
        }
    }

    /* --- R13-D2: gItems publication + item-description cutover --- */
    /* A medicine row: Potion is priced 300 and is a party-menu medicine both
     * on the field (ItemUseOutOfBattle_Medicine) and in battle. */
    CHECK("R13-D2 potion price 300",
          gItems[ITEM_POTION].price == 300u);
    CHECK("R13-D2 potion field medicine",
          gItems[ITEM_POTION].fieldUseFunc == ItemUseOutOfBattle_Medicine);
    CHECK("R13-D2 potion battle medicine",
          gItems[ITEM_POTION].battleUseFunc == ItemUseInBattle_Medicine);
    /* A Pokeball row resolves its battle callback to a native function. */
    CHECK("R13-D2 master ball battle callback",
          gItems[ITEM_MASTER_BALL].battleUseFunc == ItemUseInBattle_PokeBall);
    /* The 6 fork overrides are applied (type PARTY_MENU + EvolutionStone),
     * each on the guarded 0x04+CannotUse vanilla baseline. */
    CHECK("R13-D2 kings-rock override type",
          gItems[ITEM_KINGS_ROCK].type == ITEM_USE_PARTY_MENU);
    CHECK("R13-D2 kings-rock override func",
          gItems[ITEM_KINGS_ROCK].fieldUseFunc == ItemUseOutOfBattle_EvolutionStone);
    /* A non-battle held item has no battle-use function (NULL). */
    CHECK("R13-D2 kings-rock no battle func",
          gItems[ITEM_KINGS_ROCK].battleUseFunc == NULL);
    /* The item name is copied from the packed row (14-byte charmap "POTION"
     * row: P==0xCA in the condensed charmap). */
    CHECK("R13-D2 potion name first byte",
          gItems[ITEM_POTION].name[0] == 0xCAu);
    /* Descriptions re-point into the R13-C item text arena (ROM_BASE_ONLY). */
    CHECK("R13-D2 potion desc in text arena",
          gItems[ITEM_POTION].description != NULL
          && EmeraldTextCompat_ContainsPointer(
                 (uintptr_t)gItems[ITEM_POTION].description));
    {
        const uint8_t *potionBytes;
        size_t potionSize;
        CHECK("R13-D2 potion desc == spotiondesc arena bytes",
              EmeraldTextCompat_GetResourceBytes(
                  "emerald:text/item/spotiondesc", &potionBytes, &potionSize)
              && gItems[ITEM_POTION].description == potionBytes);
    }
    /* A dummy row shares sDummyDesc. Index 0 = ITEM_NONE is a dummy. */
    {
        const uint8_t *dummyBytes;
        size_t dummySize;
        CHECK("R13-D2 dummy desc bound",
              EmeraldTextCompat_GetResourceBytes(
                  "emerald:text/item/sdummydesc", &dummyBytes, &dummySize)
              && gItems[0].description == dummyBytes);
    }
}

/* ------------------------------------------------------------------ */
/* R13-E1: trainer-data (gTrainers / party leaves / class names)       */
/* ------------------------------------------------------------------ */

/* Drive EmeraldTrainerCompat directly against a session built from the given
 * pack (the same pack->catalog->candidate->snapshot chain the loader runs,
 * so the loader's at-most-once registration is not consumed by the refusal
 * probes). */
static enum EmeraldTrainerCompatStatus RunTrainerSeamDirect(
    const char *packPath, struct EmeraldTrainerCompatDiagnostics *diag)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourceCatalog *catalog = NULL;
    struct Gen3ResourceCandidate *candidate = NULL;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList diagnostics;
    struct EmeraldResourceSessionInfo info;
    enum EmeraldResourceSessionError sessionError;
    enum EmeraldTrainerCompatStatus status = EMERALD_TRAINER_ERR_UNAVAILABLE;

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
    {
        Gen3ResourcePackDiagnostics_Destroy(&packDiag);
        return status;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    if (!BuildCatalogFromPack(pack, &catalog) || catalog == NULL)
        goto done;
    Gen3ResourceDiagnostics_Init(&diagnostics);
    sessionError = EmeraldResourceSession_BuildRomBaseCandidate(
        pack, catalog, &candidate, &info, &diagnostics);
    if (sessionError == EMERALD_SESSION_OK && candidate != NULL
     && Gen3ResourceCandidate_Build(candidate, &snapshot, &diagnostics)
     && snapshot != NULL)
        status = EmeraldTrainerCompat_TryInitialize(snapshot, pack, diag);
    Gen3ResourceDiagnostics_Destroy(&diagnostics);
done:
    if (snapshot != NULL)
        Gen3ResourceSnapshot_Destroy(snapshot);
    if (candidate != NULL)
        Gen3ResourceCandidate_Destroy(candidate);
    if (catalog != NULL)
        Gen3ResourceCatalog_Destroy(catalog);
    if (pack != NULL)
        Gen3ResourcePack_Destroy(pack);
    return status;
}

enum { TRAINER_VAR_DROP_PARTY = 0, TRAINER_VAR_BAD_VARIANT = 1,
       TRAINER_VAR_BAD_PARTY_SIZE = 2 };

/* A production-pack variant with EXACTLY ONE trainer record damaged/dropped
 * so the refusal is provably the trainer seam's:
 *   DROP_PARTY       - the trainer-1 party leaf is dropped (unresolvable);
 *   BAD_VARIANT      - trainer-1's metadata partyFlags byte is forced to
 *                      custom moveset (variant/partyFlags mismatch);
 *   BAD_PARTY_SIZE   - trainer-1's metadata partySize byte is forced to 2
 *                      (bad party size).
 * Every other entry is copied through verbatim with its own digests. */
static bool BuildTrainerVariantPack(const char *srcPath, const char *dstPath,
                                    int mode)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourcePackProfile profile;
    struct Gen3ResourcePackBuild *build = NULL;
    struct Gen3ResourcePackProfileInput pin;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackBytes bytes = { NULL, 0 };
    struct Gen3ResourcePackDiagnosticList diag;
    uint8_t rowbuf[EMERALD_TRAINER_ROW_WIRE];
    uint8_t rowSha[32];
    uint8_t provenanceSha[32];
    const char *metaKey = kGameplayTrainerKeys[1];
    const char *partyKey = kGameplayTrainerPartyKeys[1];
    size_t count;
    size_t i;
    FILE *f = NULL;
    bool ok = false;

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(srcPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
        goto done;
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);

    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
        goto done;

    if (!Gen3ResourcePack_GetProfile(pack, &profile))
        goto done;
    memset(&pin, 0, sizeof(pin));
    pin.basePackVersion = profile.basePackVersion;
    pin.catalogVersion = profile.catalogVersion;
    pin.extractionManifestVersion = profile.extractionManifestVersion;
    pin.canonicalRepresentationVersion = profile.canonicalRepresentationVersion;
    pin.sourceRomSize = profile.sourceRomSize;
    pin.sourceRomSha1 = profile.sourceRomSha1;
    pin.sourceRomSha256 = profile.sourceRomSha256;
    memcpy(pin.gameCode, profile.gameCode, 4u);
    memcpy(pin.makerCode, profile.makerCode, 2u);
    pin.softwareRevision = profile.softwareRevision;
    memcpy(pin.gameId, profile.gameId, GEN3_PACK_GAME_ID_SIZE);
    pin.catalogSha256 = profile.catalogSha256;
    pin.extractionManifestSha256 = profile.extractionManifestSha256;
    if (Gen3ResourcePackBuild_SetProfile(build, &pin, &diag) != GEN3_PACK_OK)
        goto done;

    memset(provenanceSha, 0x5A, sizeof(provenanceSha));

    count = Gen3ResourcePack_GetEntryCount(pack);
    for (i = 0; i < count; i++)
    {
        const struct Gen3ResourcePackEntry *e =
            Gen3ResourcePack_GetEntry(pack, i);
        bool isMeta = strcmp(e->canonicalName, metaKey) == 0;
        bool isParty = strcmp(e->canonicalName, partyKey) == 0;
        const uint8_t *payload = e->payload;
        const uint8_t *payloadSha = e->payloadSha256;

        if (isParty && mode == TRAINER_VAR_DROP_PARTY)
            continue; /* drop the trainer-1 party leaf */
        if (isMeta && (mode == TRAINER_VAR_BAD_VARIANT
                    || mode == TRAINER_VAR_BAD_PARTY_SIZE))
        {
            memcpy(rowbuf, e->payload, EMERALD_TRAINER_ROW_WIRE);
            if (mode == TRAINER_VAR_BAD_VARIANT)
                rowbuf[0] = 0x01u;             /* custom moveset bit */
            else
                rowbuf[0x20] = 2u;             /* partySize -> 2 */
            DigestSha256(rowbuf, EMERALD_TRAINER_ROW_WIRE, rowSha);
            payload = rowbuf;
            payloadSha = rowSha;
        }
        memset(&entry, 0, sizeof(entry));
        entry.schema = e->schema;
        entry.flags = e->flags;
        entry.representation = e->representation;
        entry.sourceEncoding = e->sourceEncoding;
        entry.canonicalName = e->canonicalName;
        entry.key = &e->key;
        entry.type = e->type;
        entry.canonicalPayload = payload;
        entry.canonicalPayloadSize = e->payloadSize;
        entry.canonicalPayloadSha256 = payloadSha;
        entry.sourceRomOffset = e->sourceRomOffset;
        entry.sourceEncodedSize = e->sourceEncodedSize;
        entry.sourceEncodedSha256 = provenanceSha;
        if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
            goto done;
    }

    if (Gen3ResourcePackWriter_Write(build, &bytes, &diag) != GEN3_PACK_OK)
        goto done;
    f = fopen(dstPath, "wb");
    if (f == NULL)
        goto done;
    if (fwrite(bytes.data, 1, bytes.size, f) != bytes.size)
        goto done;
    fclose(f);
    f = NULL;
    ok = true;

done:
    if (f != NULL)
        fclose(f);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    if (pack != NULL)
        Gen3ResourcePack_Destroy(pack);
    return ok;
}

/* The production registration published the trainer families: 855 metadata
 * rows (48-byte native, party pointer rebuilt to the seam's packed party
 * arena), 854 party leaves, 66 class-name rows. Verify a handful of
 * representative trainers exactly, the arena/range residency, and that the
 * generated index maps agree with the pack. */
static void TestTrainerPublication(const char *packPath)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    const struct EmeraldResourceRangeIndex *index;
    static const uint8_t kHikerRow[13] = {
        0xC2, 0xC3, 0xC5, 0xBF, 0xCC, 0xFF, 0, 0, 0, 0, 0, 0, 0 };
    size_t expectedArena = 0u;
    size_t arenaBytes;
    size_t i;

    CHECK("R13-E1 published count 1775",
          EmeraldTrainerCompat_GetPublishedCount()
              == GAMEPLAY_NATIVE_TRAINER_COUNT
                 + GAMEPLAY_NATIVE_TRAINER_PARTY_COUNT
                 + GAMEPLAY_NATIVE_CLASS_COUNT);
    arenaBytes = EmeraldTrainerCompat_GetPartyArenaBytes();
    CHECK("R13-E1 party arena built", arenaBytes > 0u);

    /* Arena size == the generated native-stride sum (6/14/8/16 per mon). */
    {
        static const size_t kNativeStride[4] = { 6u, 14u, 8u, 16u };
        for (i = 0u; i < GAMEPLAY_NATIVE_TRAINER_COUNT; i++)
        {
            uint8_t v;
            if (kGameplayTrainerPartyKeys[i][0] == '\0')
                continue;
            v = kGameplayTrainerPartyMeta[i].variant;
            expectedArena += (size_t)kGameplayTrainerPartyMeta[i].partySize
                           * kNativeStride[v & 3u];
        }
    }
    CHECK("R13-E1 arena bytes == generated sum", arenaBytes == expectedArena);

    /* TRAINER_NONE (id 0): NULL party / partySize 0, empty party key. */
    CHECK("R13-E1 none partySize 0", gTrainers[0].partySize == 0u);
    CHECK("R13-E1 none NULL party",
          gTrainers[0].party.NoItemDefaultMoves == NULL);
    CHECK("R13-E1 none party key empty", kGameplayTrainerPartyKeys[0][0] == '\0');

    /* SAWYER (id 1): no item, default moves, one Geodude. */
    CHECK("R13-E1 sawyer flags", gTrainers[1].partyFlags == 0u);
    CHECK("R13-E1 sawyer class hiker",
          gTrainers[1].trainerClass == TRAINER_CLASS_HIKER);
    CHECK("R13-E1 sawyer not double", gTrainers[1].doubleBattle == FALSE);
    CHECK("R13-E1 sawyer aiFlags", gTrainers[1].aiFlags == 0x7u);
    CHECK("R13-E1 sawyer items zero",
          gTrainers[1].items[0] == 0u && gTrainers[1].items[3] == 0u);
    CHECK("R13-E1 sawyer partySize 1", gTrainers[1].partySize == 1u);
    CHECK("R13-E1 sawyer mon species", gTrainers[1].party.NoItemDefaultMoves[0].species == 74u);
    CHECK("R13-E1 sawyer mon lvl 21", gTrainers[1].party.NoItemDefaultMoves[0].lvl == 21u);
    CHECK("R13-E1 sawyer mon iv 0", gTrainers[1].party.NoItemDefaultMoves[0].iv == 0u);

    /* Party pointer is arena-resident (registered COMPAT_OBJECT range). */
    index = EmeraldResourceCompat_GetRangeIndex();
    CHECK("R13-E1 range index present", index != NULL);
    if (index != NULL)
    {
        struct EmeraldResourceRangeHit hit;
        if (EmeraldResourceRangeIndex_Lookup(
                index, (uintptr_t)gTrainers[1].party.NoItemDefaultMoves,
                &hit))
            CHECK("R13-E1 party pointer in COMPAT_OBJECT arena",
                  hit.role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT
                  && hit.schema == EMERALD_TRAINER_SCHEMA_PARTY);
        else
            CHECK("R13-E1 party pointer resolvable in arena", false);
    }

    /* A 6-mon trainer (id 9): species/levels exact. */
    CHECK("R13-E1 6mon partySize", gTrainers[9].partySize == 6u);
    CHECK("R13-E1 6mon m0", gTrainers[9].party.NoItemDefaultMoves[0].species == 315u);
    CHECK("R13-E1 6mon m0 lvl", gTrainers[9].party.NoItemDefaultMoves[0].lvl == 26u);
    CHECK("R13-E1 6mon m5", gTrainers[9].party.NoItemDefaultMoves[5].species == 304u);

    /* A double-battle trainer (id 51): doubleBattle TRUE, 2 mons. */
    CHECK("R13-E1 dbl battle flag", gTrainers[51].doubleBattle == TRUE);
    CHECK("R13-E1 dbl partySize 2", gTrainers[51].partySize == 2u);
    CHECK("R13-E1 dbl m0", gTrainers[51].party.NoItemDefaultMoves[0].species == 81u
          && gTrainers[51].party.NoItemDefaultMoves[0].iv == 50u
          && gTrainers[51].party.NoItemDefaultMoves[0].lvl == 17u);
    CHECK("R13-E1 dbl m1", gTrainers[51].party.NoItemDefaultMoves[1].species == 370u);

    /* A custom-moves trainer (id 38): variant 1, moves exact. */
    CHECK("R13-E1 custom flags", (gTrainers[38].partyFlags & 3u) == 1u);
    CHECK("R13-E1 custom partySize", gTrainers[38].partySize == 2u);
    CHECK("R13-E1 custom m0 moves",
          gTrainers[38].party.NoItemCustomMoves[0].species == 357u
          && gTrainers[38].party.NoItemCustomMoves[0].lvl == 43u
          && gTrainers[38].party.NoItemCustomMoves[0].moves[0] == 94u);
    CHECK("R13-E1 custom m1 moves",
          gTrainers[38].party.NoItemCustomMoves[1].species == 319u
          && gTrainers[38].party.NoItemCustomMoves[1].moves[0] == 285u
          && gTrainers[38].party.NoItemCustomMoves[1].moves[1] == 89u);

    /* An item+custom-moves trainer (id 71): variant 3, held item + moves. */
    CHECK("R13-E1 itemcustom flags", (gTrainers[71].partyFlags & 3u) == 3u);
    CHECK("R13-E1 itemcustom partySize", gTrainers[71].partySize == 1u);
    CHECK("R13-E1 itemcustom items", gTrainers[71].items[0] == 21u);
    CHECK("R13-E1 itemcustom mon",
          gTrainers[71].party.ItemCustomMoves[0].species == 305u
          && gTrainers[71].party.ItemCustomMoves[0].lvl == 26u
          && gTrainers[71].party.ItemCustomMoves[0].iv == 255u
          && gTrainers[71].party.ItemCustomMoves[0].heldItem == 0u
          && gTrainers[71].party.ItemCustomMoves[0].moves[0] == 98u
          && gTrainers[71].party.ItemCustomMoves[0].moves[1] == 97u
          && gTrainers[71].party.ItemCustomMoves[0].moves[2] == 17u);

    /* Class-name rows: HIKER (index 2) published verbatim. */
    CHECK("R13-E1 hiker class row",
          memcmp(gTrainerClassNames[TRAINER_CLASS_HIKER], kHikerRow, 13) == 0);
    CHECK("R13-E1 hiker class node",
          kGameplayTrainerClassKeys[TRAINER_CLASS_HIKER][0] != '\0' &&
          gTrainerClassNames[TRAINER_CLASS_COLLECTOR][0] == 0xBDu);

    /* Generated maps agree with the pack: every key resolves to an entry
     * with the right type/schema/size. */
    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &packDiag) == GEN3_PACK_OK
     && pack != NULL)
    {
        static const size_t kProbe[3] = { 1u, 9u, 71u };
        size_t k;
        for (k = 0u; k < 3u; k++)
        {
            const struct Gen3ResourcePackEntry *me =
                Gen3ResourcePack_FindByCanonicalName(
                    pack, kGameplayTrainerKeys[kProbe[k]]);
            const struct Gen3ResourcePackEntry *pe =
                Gen3ResourcePack_FindByCanonicalName(
                    pack, kGameplayTrainerPartyKeys[kProbe[k]]);
            CHECK("R13-E1 meta key present in pack",
                  me != NULL && me->type == GEN3_RESOURCE_TYPE_STRUCTURED_DATA
                  && me->schema == EMERALD_TRAINER_SCHEMA_METADATA
                  && me->payloadSize == EMERALD_TRAINER_ROW_WIRE);
            CHECK("R13-E1 party key present in pack",
                  pe != NULL && pe->schema == EMERALD_TRAINER_SCHEMA_PARTY
                  && pe->payloadSize == (size_t)kGameplayTrainerPartyMeta[kProbe[k]].partySize * (kProbe[k]==71?16u:8u));
        }
        Gen3ResourcePack_Destroy(pack);
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
}

/* The trainer seam REFUSES a pack whose trainer records are malformed:
 * a missing party leaf, a partyFlags that disagrees with the generated
 * variant, or a mis-sized party. Nothing is published (fail-closed); the
 * already-live production session's published gTrainers are left untouched
 * (captured before the probes, compared after). */
static void TestTrainerRefusals(const char *tempDir, const char *prodPack)
{
    static const char *const labels[3] = {
        "trainer_missing_party", "trainer_bad_variant", "trainer_bad_party_size" };
    const struct TrainerMonNoItemDefaultMoves *liveParty =
        gTrainers[1].party.NoItemDefaultMoves;
    uint8_t livePartySize = gTrainers[1].partySize;
    size_t liveArena = EmeraldTrainerCompat_GetPartyArenaBytes();
    int mode;
    for (mode = 0; mode < 3; mode++)
    {
        struct EmeraldTrainerCompatDiagnostics diag;
        enum EmeraldTrainerCompatStatus status;
        char path[512];

        snprintf(path, sizeof(path), "%s/%s.rpack", tempDir, labels[mode]);
        CHECK("R13-E1 variant pack builds",
              BuildTrainerVariantPack(prodPack, path, mode));
        status = RunTrainerSeamDirect(path, &diag);
        CHECK("R13-E1 trainer variant refused", status != EMERALD_TRAINER_OK);
    }
    /* The refused direct runs did not overwrite the live published tables. */
    CHECK("R13-E1 refusal leaves live party untouched",
          gTrainers[1].partySize == livePartySize
          && gTrainers[1].party.NoItemDefaultMoves == liveParty
          && EmeraldTrainerCompat_GetPartyArenaBytes() == liveArena);
}

/* ------------------------------------------------------------------ */
/* R13-E2: wild-encounter (headers + map-based info + slot tables)     */
/* ------------------------------------------------------------------ */

static uint16_t EncLe16(const uint8_t *d)
{
    return (uint16_t)(d[0] | ((uint16_t)d[1] << 8));
}
static uint32_t ReadLe32Native(const uint8_t *d)
{
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8)
        | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}
static void WriteLe32Native(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)(v & 0xFF); d[1] = (uint8_t)((v >> 8) & 0xFF);
    d[2] = (uint8_t)((v >> 16) & 0xFF); d[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int FindHeaderIdx(u8 mapGroup, u8 mapNum)
{
    size_t i;
    for (i = 0u; i < EMERALD_ENCOUNTER_HEADER_COUNT; i++)
        if (gWildMonHeaders[i].mapGroup == mapGroup
         && gWildMonHeaders[i].mapNum == mapNum)
            return (int)i;
    return -1;
}

/* Binary search over the (sorted) generated info records by canonical key. */
static size_t FindEncInfoIndex(const char *key)
{
    size_t lo = 0u;
    size_t hi = EMERALD_ENCOUNTER_INFO_COUNT;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2u;
        int cmp = strcmp(key, kEncounterInfos[mid].name);
        if (cmp == 0)
            return mid;
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1u;
    }
    return (size_t)-1;
}

/* Drive EmeraldEncounterCompat directly against a session built from the
 * given pack (the same pack->catalog->candidate->snapshot chain the loader
 * runs, independent of the loader's at-most-once registration). */
static enum EmeraldEncounterCompatStatus RunEncounterSeamDirect(
    const char *packPath, struct EmeraldEncounterCompatDiagnostics *diag)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourceCatalog *catalog = NULL;
    struct Gen3ResourceCandidate *candidate = NULL;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList diagnostics;
    struct EmeraldResourceSessionInfo info;
    enum EmeraldResourceSessionError sessionError;
    enum EmeraldEncounterCompatStatus status = EMERALD_ENCOUNTER_ERR_UNAVAILABLE;

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
    {
        Gen3ResourcePackDiagnostics_Destroy(&packDiag);
        return status;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    if (!BuildCatalogFromPack(pack, &catalog) || catalog == NULL)
        goto done;
    Gen3ResourceDiagnostics_Init(&diagnostics);
    sessionError = EmeraldResourceSession_BuildRomBaseCandidate(
        pack, catalog, &candidate, &info, &diagnostics);
    if (sessionError == EMERALD_SESSION_OK && candidate != NULL
     && Gen3ResourceCandidate_Build(candidate, &snapshot, &diagnostics)
     && snapshot != NULL)
        status = EmeraldEncounterCompat_TryInitialize(snapshot, pack, diag);
    Gen3ResourceDiagnostics_Destroy(&diagnostics);
done:
    if (snapshot != NULL)
        Gen3ResourceSnapshot_Destroy(snapshot);
    if (candidate != NULL)
        Gen3ResourceCandidate_Destroy(candidate);
    if (catalog != NULL)
        Gen3ResourceCatalog_Destroy(catalog);
    if (pack != NULL)
        Gen3ResourcePack_Destroy(pack);
    return status;
}

enum {
    ENC_VAR_BAD_HEADERS_SIZE = 0,
    ENC_VAR_BAD_SENTINEL,
    ENC_VAR_HEADER_INFO_PTR,
    ENC_VAR_INFO_SLOT_PTR,
    ENC_VAR_MISSING_SLOT,
    ENC_VAR_ALTERING_CAVE,
};

/* A production-pack variant with EXACTLY ONE encounter record damaged/dropped
 * so the refusal is provably the encounter seam's:
 *   BAD_HEADERS_SIZE  - the headers resource payload is re-sized (2500 -> 2480);
 *   BAD_SENTINEL      - the sentinel header's mapGroup is flipped off 0xFF;
 *   HEADER_INFO_PTR   - Route 101's land info pointer is re-pointed at Route
 *                       102's land info (header->info edge breaks);
 *   INFO_SLOT_PTR     - Route 101's land slot resource ROM offset moved by +4
 *                       (info->slot edge breaks);
 *   MISSING_SLOT      - the Route 101 land slot resource is dropped;
 *   ALTERING_CAVE     - Altering Cave row 114's mapGroup/mapNum changed.
 * Every other entry is copied through verbatim with its own digests. */
static bool BuildEncounterVariantPack(const char *srcPath, const char *dstPath,
                                      int mode)
{
    static const char *kHeadersId = "emerald:data/encounter/headers";
    static const char *kRoute101Land = "emerald:data/encounter/route101/land";
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourcePackProfile profile;
    struct Gen3ResourcePackBuild *build = NULL;
    struct Gen3ResourcePackProfileInput pin;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackBytes bytes = { NULL, 0 };
    struct Gen3ResourcePackDiagnosticList diag;
    uint8_t headerBuf[EMERALD_ENCOUNTER_HEADERS_BYTES];
    uint8_t bufSha[32];
    uint8_t provenanceSha[32];
    size_t count;
    size_t i;
    FILE *f = NULL;
    bool ok = false;

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(srcPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
        goto done;
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);

    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
        goto done;

    if (!Gen3ResourcePack_GetProfile(pack, &profile))
        goto done;
    memset(&pin, 0, sizeof(pin));
    pin.basePackVersion = profile.basePackVersion;
    pin.catalogVersion = profile.catalogVersion;
    pin.extractionManifestVersion = profile.extractionManifestVersion;
    pin.canonicalRepresentationVersion = profile.canonicalRepresentationVersion;
    pin.sourceRomSize = profile.sourceRomSize;
    pin.sourceRomSha1 = profile.sourceRomSha1;
    pin.sourceRomSha256 = profile.sourceRomSha256;
    memcpy(pin.gameCode, profile.gameCode, 4u);
    memcpy(pin.makerCode, profile.makerCode, 2u);
    pin.softwareRevision = profile.softwareRevision;
    memcpy(pin.gameId, profile.gameId, GEN3_PACK_GAME_ID_SIZE);
    pin.catalogSha256 = profile.catalogSha256;
    pin.extractionManifestSha256 = profile.extractionManifestSha256;
    if (Gen3ResourcePackBuild_SetProfile(build, &pin, &diag) != GEN3_PACK_OK)
        goto done;

    memset(provenanceSha, 0x5A, sizeof(provenanceSha));

    count = Gen3ResourcePack_GetEntryCount(pack);
    for (i = 0u; i < count; i++)
    {
        const struct Gen3ResourcePackEntry *e =
            Gen3ResourcePack_GetEntry(pack, i);
        bool isHeaders = strcmp(e->canonicalName, kHeadersId) == 0;
        bool isRoute101Land = strcmp(e->canonicalName, kRoute101Land) == 0;
        const uint8_t *payload = e->payload;
        const uint8_t *payloadSha = e->payloadSha256;
        size_t payloadSize = e->payloadSize;
        uint64_t romOffset = e->sourceRomOffset;

        if (isHeaders && e->payloadSize == EMERALD_ENCOUNTER_HEADERS_BYTES)
        {
            /* Route 102's land info GBA pointer (header row 1 @ +4). */
            uint32_t route102LandPtr;
            memcpy(headerBuf, e->payload, EMERALD_ENCOUNTER_HEADERS_BYTES);
            switch (mode)
            {
            case ENC_VAR_BAD_SENTINEL:
                headerBuf[EMERALD_ENCOUNTER_SENTINEL_ROW
                          * EMERALD_ENCOUNTER_HEADER_WIRE + 0u] = 0x01u;
                break;
            case ENC_VAR_HEADER_INFO_PTR:
                route102LandPtr = (uint32_t)ReadLe32Native(
                    e->payload + 1u * EMERALD_ENCOUNTER_HEADER_WIRE + 4u);
                WriteLe32Native(headerBuf + 4u, route102LandPtr);
                break;
            case ENC_VAR_ALTERING_CAVE:
                headerBuf[EMERALD_ENCOUNTER_ALTERING_ROW0
                          * EMERALD_ENCOUNTER_HEADER_WIRE + 0u] = 0x00u;
                headerBuf[EMERALD_ENCOUNTER_ALTERING_ROW0
                          * EMERALD_ENCOUNTER_HEADER_WIRE + 1u] = 0x10u;
                break;
            default:
                break;
            }
            if (mode == ENC_VAR_BAD_HEADERS_SIZE)
            {
                payloadSize = EMERALD_ENCOUNTER_HEADERS_BYTES - 20u;
                DigestSha256(headerBuf, payloadSize, bufSha);
                payloadSha = bufSha;
            }
            else
            {
                DigestSha256(headerBuf, EMERALD_ENCOUNTER_HEADERS_BYTES, bufSha);
                payloadSha = bufSha;
            }
            payload = headerBuf;
        }
        if (isRoute101Land && mode == ENC_VAR_MISSING_SLOT)
            continue; /* drop the Route 101 land slot resource */
        if (isRoute101Land && mode == ENC_VAR_INFO_SLOT_PTR)
            romOffset = e->sourceRomOffset + 4u;

        memset(&entry, 0, sizeof(entry));
        entry.schema = e->schema;
        entry.flags = e->flags;
        entry.representation = e->representation;
        entry.sourceEncoding = e->sourceEncoding;
        entry.canonicalName = e->canonicalName;
        entry.key = &e->key;
        entry.type = e->type;
        entry.canonicalPayload = payload;
        entry.canonicalPayloadSize = payloadSize;
        entry.canonicalPayloadSha256 = payloadSha;
        entry.sourceRomOffset = romOffset;
        entry.sourceEncodedSize = e->sourceEncodedSize;
        entry.sourceEncodedSha256 = provenanceSha;
        if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
            goto done;
    }

    if (Gen3ResourcePackWriter_Write(build, &bytes, &diag) != GEN3_PACK_OK)
        goto done;
    f = fopen(dstPath, "wb");
    if (f == NULL)
        goto done;
    if (fwrite(bytes.data, 1, bytes.size, f) != bytes.size)
        goto done;
    fclose(f);
    f = NULL;
    ok = true;

done:
    if (f != NULL)
        fclose(f);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    if (pack != NULL)
        Gen3ResourcePack_Destroy(pack);
    return ok;
}

/* The production registration published the encounter families: 210 resources
 * (1 headers block + 209 slot tables), the headers array + 209 native info
 * objects + a byte-exact slot arena, with the pointer graph rebuilt. */
static void TestEncounterPublication(const char *packPath)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    const struct EmeraldResourceRangeIndex *index;
    uint8_t *arena = (uint8_t *)gWildEncounterInfos[0].wildPokemon;
    size_t offset = 0u;
    size_t slotCount = 0u;
    size_t i;
    int route101;

    CHECK("R13-E2 published count 210",
          EmeraldEncounterCompat_GetPublishedCount()
              == EMERALD_ENCOUNTER_INFO_COUNT + 1u);
    CHECK("R13-E2 slot arena 7900 B",
          EmeraldEncounterCompat_GetSlotArenaBytes() == 7900u);

    /* Sentinel terminates the consumer's linear scan. */
    CHECK("R13-E2 sentinel mapGroup/mapNum",
          gWildMonHeaders[EMERALD_ENCOUNTER_SENTINEL_ROW].mapGroup == 0xFFu
          && gWildMonHeaders[EMERALD_ENCOUNTER_SENTINEL_ROW].mapNum == 0xFFu);
    CHECK("R13-E2 sentinel info NULL",
          gWildMonHeaders[EMERALD_ENCOUNTER_SENTINEL_ROW].landMonsInfo == NULL
          && gWildMonHeaders[EMERALD_ENCOUNTER_SENTINEL_ROW].waterMonsInfo == NULL
          && gWildMonHeaders[EMERALD_ENCOUNTER_SENTINEL_ROW].rockSmashMonsInfo == NULL
          && gWildMonHeaders[EMERALD_ENCOUNTER_SENTINEL_ROW].fishingMonsInfo == NULL);

    /* The slot arena is byte-exact: every published info's slot pointer is
     * arena-resident at the generated offset, matching the pack payload. */
    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
    {
        CHECK("R13-E2 pack opens for verification", false);
        Gen3ResourcePackDiagnostics_Destroy(&packDiag);
        return;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);

    for (i = 0u; i < EMERALD_ENCOUNTER_INFO_COUNT; i++)
    {
        const struct Gen3ResourcePackEntry *e =
            Gen3ResourcePack_FindByCanonicalName(pack, kEncounterInfos[i].name);
        size_t bytes = (size_t)kEncounterInfos[i].slotRows * 4u;
        if (e != NULL && e->schema == EMERALD_ENCOUNTER_SCHEMA_SLOT)
            slotCount++;
        CHECK("R13-E2 slot entry present",
              e != NULL && e->type == GEN3_RESOURCE_TYPE_STRUCTURED_DATA
              && e->schema == EMERALD_ENCOUNTER_SCHEMA_SLOT
              && e->payloadSize == bytes);
        if (e == NULL)
            continue;
        CHECK("R13-E2 info pointer arena-resident",
              (uint8_t *)gWildEncounterInfos[i].wildPokemon == arena + offset);
        CHECK("R13-E2 arena slice == pack payload",
              memcmp(arena + offset, e->payload, bytes) == 0);
        CHECK("R13-E2 info rate == generated",
              gWildEncounterInfos[i].encounterRate
                  == kEncounterInfos[i].rate);
        offset += bytes;
    }
    CHECK("R13-E2 all 209 slot entries enumerated", slotCount == 209u);

    /* Sample per-type maps (through the published graph). Route 101 is the
     * first header (map 0,16) and has a land family only (a null-family
     * example for water/rock/fishing). */
    route101 = FindHeaderIdx(0u, 16u);
    CHECK("R13-E2 route101 header found", route101 == 0);
    if (route101 >= 0)
    {
        const struct Gen3ResourcePackEntry *land =
            Gen3ResourcePack_FindByCanonicalName(pack,
                kEncounterHeaderKeys[route101][EMERALD_ENCOUNTER_FIELD_LAND]);
        const struct WildPokemonInfo *li =
            gWildMonHeaders[route101].landMonsInfo;
        CHECK("R13-E2 route101 land published", li != NULL);
        if (li != NULL && land != NULL)
        {
            CHECK("R13-E2 route101 land rate 20", li->encounterRate == 20u);
            CHECK("R13-E2 route101 land slot0 exact",
                  li->wildPokemon[0].minLevel == land->payload[0]
                  && li->wildPokemon[0].maxLevel == land->payload[1]
                  && li->wildPokemon[0].species
                      == EncLe16(land->payload + 2u));
        }
        CHECK("R13-E2 route101 null water/rock/fishing",
              gWildMonHeaders[route101].waterMonsInfo == NULL
              && gWildMonHeaders[route101].rockSmashMonsInfo == NULL
              && gWildMonHeaders[route101].fishingMonsInfo == NULL);
    }
    /* Route 102 has land + water + fishing (sample each type). */
    {
        int r102 = FindHeaderIdx(0u, 17u);
        const struct WildPokemonInfo *li, *wi, *fi;
        CHECK("R13-E2 route102 header found", r102 == 1);
        if (r102 < 0)
        {
            Gen3ResourcePack_Destroy(pack);
            return;
        }
        li = gWildMonHeaders[r102].landMonsInfo;
        wi = gWildMonHeaders[r102].waterMonsInfo;
        fi = gWildMonHeaders[r102].fishingMonsInfo;
        CHECK("R13-E2 route102 land/water/fishing",
              li != NULL && wi != NULL && fi != NULL);
        if (li != NULL && wi != NULL && fi != NULL)
        {
            CHECK("R13-E2 route102 land rate", li->encounterRate > 0u);
            CHECK("R13-E2 route102 water rate", wi->encounterRate == 4u);
            CHECK("R13-E2 route102 fishing rate", fi->encounterRate == 30u);
            CHECK("R13-E2 route102 water slot0 minLevel",
                  wi->wildPokemon[0].minLevel <= wi->wildPokemon[0].maxLevel);
        }
    }
    /* A rock-smash family: Route 114 (map 0,29) has land/water/rock/fishing. */
    {
        int r114 = FindHeaderIdx(0u, 29u);
        const struct WildPokemonInfo *ri;
        CHECK("R13-E2 route114 header found", r114 >= 0);
        if (r114 >= 0)
        {
            ri = gWildMonHeaders[r114].rockSmashMonsInfo;
            CHECK("R13-E2 route114 rock-smash published", ri != NULL);
            if (ri != NULL)
            {
                CHECK("R13-E2 route114 rock-smash rate", ri->encounterRate > 0u);
                CHECK("R13-E2 route114 rock-smash slot0 valid",
                      ri->wildPokemon[0].minLevel <= ri->wildPokemon[0].maxLevel
                      && ri->wildPokemon[0].species != 0u);
            }
        }
    }

    /* Altering Cave: exactly 9 headers at map (24,106), in header order, each
     * carrying its altering-cave-1..9 land family at the matching key. */
    {
        int altering[9];
        int found = 0;
        size_t k;
        for (i = 0u; i < EMERALD_ENCOUNTER_HEADER_COUNT; i++)
        {
            if (gWildMonHeaders[i].mapGroup
                    == EMERALD_ENCOUNTER_ALTERING_MAP_GROUP
             && gWildMonHeaders[i].mapNum
                    == EMERALD_ENCOUNTER_ALTERING_MAP_NUM
             && found < 9)
                altering[found++] = (int)i;
        }
        CHECK("R13-E2 9 altering-cave headers", found == 9);
        for (k = 0u; k < 9u && k < (size_t)found; k++)
        {
            char expect[64];
            int idx = altering[k];
            size_t infoi;
            CHECK("R13-E2 altering cave at 114..122",
                  idx == (int)(EMERALD_ENCOUNTER_ALTERING_ROW0 + k));
            snprintf(expect, sizeof(expect),
                     "emerald:data/encounter/altering-cave-%zu/land", k + 1u);
            CHECK("R13-E2 altering land key matches",
                  strcmp(expect, kEncounterHeaderKeys[idx]
                      [EMERALD_ENCOUNTER_FIELD_LAND]) == 0);
            infoi = FindEncInfoIndex(expect);
            CHECK("R13-E2 altering-cave info found", infoi != (size_t)-1);
            if (infoi != (size_t)-1)
                CHECK("R13-E2 altering-cave order exact",
                      gWildMonHeaders[idx].landMonsInfo
                          == &gWildEncounterInfos[infoi]);
        }
    }

    /* The slot arena is a registered COMPAT_OBJECT range. */
    index = EmeraldResourceCompat_GetRangeIndex();
    CHECK("R13-E2 range index present", index != NULL);
    if (index != NULL)
    {
        struct EmeraldResourceRangeHit hit;
        if (EmeraldResourceRangeIndex_Lookup(index, (uintptr_t)arena, &hit))
            CHECK("R13-E2 slot arena COMPAT_OBJECT range",
                  hit.role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT
                  && hit.schema == EMERALD_ENCOUNTER_SCHEMA_SLOT);
        else
            CHECK("R13-E2 slot arena resolvable", false);
    }

    Gen3ResourcePack_Destroy(pack);
}

/* The encounter seam REFUSES a pack whose encounter records are malformed
 * (wrong headers size, bad sentinel, broken header->info / info->slot edges,
 * a missing slot, or an Altering Cave misorder). Nothing is published. */
static void TestEncounterRefusals(const char *tempDir, const char *prodPack)
{
    static const char *const labels[6] = {
        "enc_bad_headers_size", "enc_bad_sentinel", "enc_header_info_ptr",
        "enc_info_slot_ptr", "enc_missing_slot", "enc_altering_cave" };
    const struct WildPokemonInfo *liveLand = gWildMonHeaders[0].landMonsInfo;
    size_t liveArena = EmeraldEncounterCompat_GetSlotArenaBytes();
    int mode;
    for (mode = 0; mode < 6; mode++)
    {
        struct EmeraldEncounterCompatDiagnostics diag;
        enum EmeraldEncounterCompatStatus status;
        char path[512];

        snprintf(path, sizeof(path), "%s/%s.rpack", tempDir, labels[mode]);
        CHECK("R13-E2 variant pack builds",
              BuildEncounterVariantPack(prodPack, path, mode));
        status = RunEncounterSeamDirect(path, &diag);
        CHECK("R13-E2 encounter variant refused", status != EMERALD_ENCOUNTER_OK);
    }
    /* The refused direct runs did not overwrite the live published tables. */
    CHECK("R13-E2 refusal leaves live tables untouched",
          gWildMonHeaders[0].landMonsInfo == liveLand
          && EmeraldEncounterCompat_GetSlotArenaBytes() == liveArena);
}

/* ---- R13-E3a-1: Frontier + Battle Tent publication seam tests. ---- */

/* R13-E3a: the engine EWRAM facility aliases gFacilityTrainers /
 * gFacilityTrainerMons (defined in src/battle_tower.c:44-45, re-pointed at
 * SetFacilityPtrsGetLevel/SetTentPtrsGetLevel). src/battle_tower.c is not
 * linked into this test binary, so the aliases are stubbed here (matching the
 * engine declaration in include/battle_tower.h) so the relocation test can
 * drive/observe the alias target. */
const struct BattleFrontierTrainer *gFacilityTrainers;
const struct FacilityMon *gFacilityTrainerMons;

/* Drive EmeraldFrontierCompat directly against a session built from the given
 * pack (the same chain the loader runs, independent of its at-most-once
 * registration). */
static enum EmeraldFrontierCompatStatus RunFrontierSeamDirect(
    const char *packPath, struct EmeraldFrontierCompatDiagnostics *diag)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourceCatalog *catalog = NULL;
    struct Gen3ResourceCandidate *candidate = NULL;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList diagnostics;
    struct EmeraldResourceSessionInfo info;
    enum EmeraldResourceSessionError sessionError;
    enum EmeraldFrontierCompatStatus status = EMERALD_FRONTIER_ERR_UNAVAILABLE;

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
    {
        Gen3ResourcePackDiagnostics_Destroy(&packDiag);
        return status;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    if (!BuildCatalogFromPack(pack, &catalog) || catalog == NULL)
        goto done;
    Gen3ResourceDiagnostics_Init(&diagnostics);
    sessionError = EmeraldResourceSession_BuildRomBaseCandidate(
        pack, catalog, &candidate, &info, &diagnostics);
    if (sessionError == EMERALD_SESSION_OK && candidate != NULL
     && Gen3ResourceCandidate_Build(candidate, &snapshot, &diagnostics)
     && snapshot != NULL)
        status = EmeraldFrontierCompat_TryInitialize(snapshot, pack, diag);
    Gen3ResourceDiagnostics_Destroy(&diagnostics);
done:
    if (snapshot != NULL)
        Gen3ResourceSnapshot_Destroy(snapshot);
    if (candidate != NULL)
        Gen3ResourceCandidate_Destroy(candidate);
    if (catalog != NULL)
        Gen3ResourceCatalog_Destroy(catalog);
    if (pack != NULL)
        Gen3ResourcePack_Destroy(pack);
    return status;
}

/* Is `p` inside the CURRENTLY registered mon-set COMPAT_OBJECT arena? */
static bool MonSetResident(const u16 *p)
{
    const struct EmeraldResourceRangeIndex *index;
    struct EmeraldResourceRangeHit hit;
    if (p == NULL)
        return false;
    index = EmeraldResourceCompat_GetRangeIndex();
    if (index == NULL
     || !EmeraldResourceRangeIndex_Lookup(index, (uintptr_t)p, &hit))
        return false;
    return hit.role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT
        && hit.schema == EMERALD_FRONTIER_SCHEMA_MON_SET;
}

enum {
    FRONTIER_VAR_BAD_TRAINER_SIZE = 0,
    FRONTIER_VAR_BAD_MONSET_PTR,   /* row monSet GBA ptr re-pointed (link breaks) */
    FRONTIER_VAR_UNTERMINATED,     /* a mon-set leaf's terminator removed */
    FRONTIER_VAR_MON_INDEX,        /* a mon-set leaf index forced >= pool size */
    FRONTIER_VAR_MISSING_POOL,     /* the shared mons pool resource dropped */
    FRONTIER_VAR_MON_TRANSFORM,    /* pool payload resized (16-row -> wrong len) */
    FRONTIER_VAR_MISSING_HELD_BANNED, /* drop held-items + banned-species */
};

/* A production-pack variant with EXACTLY ONE frontier record damaged/dropped
 * so the refusal is provably the frontier seam's. Every other entry is copied
 * through verbatim with its own digests. */
static bool BuildFrontierVariantPack(const char *srcPath, const char *dstPath,
                                     int mode)
{
    static const char *kTrainer0 = "emerald:data/frontier/trainer/0";
    static const char *kMonSet1 = "emerald:data/frontier/trainer/1/mons";
    static const char *kHeld = "emerald:data/frontier/held-items";
    static const char *kBanned = "emerald:data/frontier/banned-species";
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourcePackProfile profile;
    struct Gen3ResourcePackBuild *build = NULL;
    struct Gen3ResourcePackProfileInput pin;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackBytes bytes = { NULL, 0 };
    struct Gen3ResourcePackDiagnosticList diag;
    uint8_t bufSha[32];
    uint8_t provenanceSha[32];
    uint8_t trainer0Buf[EMERALD_FRONTIER_TRAINER_WIRE];
    uint8_t leafBuf[128];
    size_t count;
    size_t i;
    FILE *f = NULL;
    bool ok = false;

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(srcPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
        goto done;
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);

    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
        goto done;
    if (!Gen3ResourcePack_GetProfile(pack, &profile))
        goto done;
    memset(&pin, 0, sizeof(pin));
    pin.basePackVersion = profile.basePackVersion;
    pin.catalogVersion = profile.catalogVersion;
    pin.extractionManifestVersion = profile.extractionManifestVersion;
    pin.canonicalRepresentationVersion = profile.canonicalRepresentationVersion;
    pin.sourceRomSize = profile.sourceRomSize;
    pin.sourceRomSha1 = profile.sourceRomSha1;
    pin.sourceRomSha256 = profile.sourceRomSha256;
    memcpy(pin.gameCode, profile.gameCode, 4u);
    memcpy(pin.makerCode, profile.makerCode, 2u);
    pin.softwareRevision = profile.softwareRevision;
    memcpy(pin.gameId, profile.gameId, GEN3_PACK_GAME_ID_SIZE);
    pin.catalogSha256 = profile.catalogSha256;
    pin.extractionManifestSha256 = profile.extractionManifestSha256;
    if (Gen3ResourcePackBuild_SetProfile(build, &pin, &diag) != GEN3_PACK_OK)
        goto done;

    memset(provenanceSha, 0x5A, sizeof(provenanceSha));
    count = Gen3ResourcePack_GetEntryCount(pack);

    for (i = 0u; i < count; i++)
    {
        const struct Gen3ResourcePackEntry *e = Gen3ResourcePack_GetEntry(pack, i);
        const uint8_t *payload = e->payload;
        const uint8_t *payloadSha = e->payloadSha256;
        size_t payloadSize = e->payloadSize;
        uint64_t romOffset = e->sourceRomOffset;

        if (mode == FRONTIER_VAR_MISSING_POOL
         && strcmp(e->canonicalName, kFrontierMonsKey) == 0)
            continue;
        if ((mode == FRONTIER_VAR_MISSING_HELD_BANNED)
         && (strcmp(e->canonicalName, kHeld) == 0
             || strcmp(e->canonicalName, kBanned) == 0))
            continue;

        if (mode == FRONTIER_VAR_BAD_TRAINER_SIZE
         && strcmp(e->canonicalName, kTrainer0) == 0)
        {
            payloadSize = EMERALD_FRONTIER_TRAINER_WIRE - 4u;
            DigestSha256(e->payload, payloadSize, bufSha);
            payloadSha = bufSha;
        }
        /* Re-point trainer 0's monSet GBA ptr at +4 into its own leaf: the
         * row->leaf edge (== leaf ROM address) breaks. */
        if (mode == FRONTIER_VAR_BAD_MONSET_PTR
         && strcmp(e->canonicalName, kTrainer0) == 0)
        {
            memcpy(trainer0Buf, e->payload, EMERALD_FRONTIER_TRAINER_WIRE);
            WriteLe32Native(trainer0Buf + 48u,
                            (uint32_t)ReadLe32Native(e->payload + 48u) + 4u);
            payload = trainer0Buf;
            DigestSha256(payload, EMERALD_FRONTIER_TRAINER_WIRE, bufSha);
            payloadSha = bufSha;
        }
        if (mode == FRONTIER_VAR_UNTERMINATED
         && strcmp(e->canonicalName, kMonSet1) == 0)
        {
            memcpy(leafBuf, e->payload, e->payloadSize <= sizeof(leafBuf)
                                         ? e->payloadSize : sizeof(leafBuf));
            /* destroy the trailing 0xFFFF terminator (stream "@4" the last
             * two payload bytes to a non-FFFF), making it unterminated. */
            leafBuf[e->payloadSize - 2u] = 0x01u;
            leafBuf[e->payloadSize - 1u] = 0x02u;
            payload = leafBuf;
            DigestSha256(payload, e->payloadSize, bufSha);
            payloadSha = bufSha;
        }
        if (mode == FRONTIER_VAR_MON_INDEX
         && strcmp(e->canonicalName, kMonSet1) == 0)
        {
            memcpy(leafBuf, e->payload, e->payloadSize <= sizeof(leafBuf)
                                         ? e->payloadSize : sizeof(leafBuf));
            /* Force the FIRST mon index to 900 (>= 882). */
            leafBuf[0] = 0x84u;
            leafBuf[1] = 0x03u;
            payload = leafBuf;
            DigestSha256(payload, e->payloadSize, bufSha);
            payloadSha = bufSha;
        }
        if (mode == FRONTIER_VAR_MON_TRANSFORM
         && strcmp(e->canonicalName, kFrontierMonsKey) == 0)
        {
            /* Resize the shared pool to a non-16-multiple (transform input
             * mismatch): 16*882 -> 16*881+2. */
            payloadSize = (size_t)EMERALD_FRONTIER_MONS_COUNT
                        * EMERALD_FRONTIER_MON_WIRE - 14u;
            DigestSha256(e->payload, payloadSize, bufSha);
            payloadSha = bufSha;
        }

        memset(&entry, 0, sizeof(entry));
        entry.schema = e->schema;
        entry.flags = e->flags;
        entry.representation = e->representation;
        entry.sourceEncoding = e->sourceEncoding;
        entry.canonicalName = e->canonicalName;
        entry.key = &e->key;
        entry.type = e->type;
        entry.canonicalPayload = payload;
        entry.canonicalPayloadSize = payloadSize;
        entry.canonicalPayloadSha256 = payloadSha;
        entry.sourceRomOffset = romOffset;
        entry.sourceEncodedSize = e->sourceEncodedSize;
        entry.sourceEncodedSha256 = provenanceSha;
        if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
            goto done;
    }

    if (Gen3ResourcePackWriter_Write(build, &bytes, &diag) != GEN3_PACK_OK)
        goto done;
    f = fopen(dstPath, "wb");
    if (f == NULL)
        goto done;
    if (fwrite(bytes.data, 1, bytes.size, f) != bytes.size)
        goto done;
    fclose(f);
    f = NULL;
    ok = true;

done:
    if (f != NULL)
        fclose(f);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    if (pack != NULL)
        Gen3ResourcePack_Destroy(pack);
    return ok;
}

/* The production registration published the frontier + tent families into the
 * live fill targets: 786 resources, the shared 882-mons pool transformed to
 * 14-byte native rows, held-items/banned leaves copied, and every published
 * trainer row's monSet in the packed mon-set arena, with the row->leaf GBA
 * edges intact in the pack. */
static void TestFrontierTrainerGraph(const char *packPath)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    const struct EmeraldResourceRangeIndex *index;
    struct EmeraldResourceRangeHit hit;
    const struct Gen3ResourcePackEntry *poolEntry;
    const struct Gen3ResourcePackEntry *heldEntry;
    const struct Gen3ResourcePackEntry *bannedEntry;
    size_t i;
    u16 allHeldOk = 1u;

    /* E3a-1 (786) + E3a-2 facility AUX + wild handoff (57). */
    CHECK("R13-E3a published count 786+57",
          EmeraldFrontierCompat_GetPublishedCount() == 843u);
    CHECK("R13-E3a mon-set arena published",
          EmeraldFrontierCompat_GetMonSetArenaBytes() > 0u);

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
    {
        CHECK("R13-E3a pack opens for verification", false);
        Gen3ResourcePackDiagnostics_Destroy(&packDiag);
        return;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);

    index = EmeraldResourceCompat_GetRangeIndex();
    CHECK("R13-E3a range index present", index != NULL);

    poolEntry = Gen3ResourcePack_FindByCanonicalName(pack, kFrontierMonsKey);
    heldEntry = Gen3ResourcePack_FindByCanonicalName(pack, kFrontierHeldItemsKey);
    bannedEntry = Gen3ResourcePack_FindByCanonicalName(pack,
                                                       kFrontierBannedSpeciesKey);
    CHECK("R13-E3a pool/held/banned present",
          poolEntry != NULL && heldEntry != NULL && bannedEntry != NULL);

    /* Facility aliases: the engine re-points these at SetFacilityPtrsGetLevel;
     * proving they alias the published fill targets is the State-v5 target. */
    {
        const struct BattleFrontierTrainer *facTrainers = gBattleFrontierTrainers;
        const struct FacilityMon *facMons = gBattleFrontierMons;
        CHECK("R13-E3a facility trainer alias -> published table",
              facTrainers == gBattleFrontierTrainers);
        CHECK("R13-E3a facility mons alias -> published pool",
              facMons == gBattleFrontierMons);
    }

    /* Every frontier trainer row matches its pack metadata; monSet arena-
     * resident; the mon-set leaf bytes are byte-exact vs the pack. */
    for (i = 0u; i < EMERALD_FRONTIER_TRAINER_COUNT; i++)
    {
        const struct Gen3ResourcePackEntry *te =
            Gen3ResourcePack_FindByCanonicalName(pack, kFrontierTrainerKeys[i]);
        const struct Gen3ResourcePackEntry *me =
            Gen3ResourcePack_FindByCanonicalName(pack, kFrontierMonSetKeys[i]);
        const struct BattleFrontierTrainer *t = &gBattleFrontierTrainers[i];
        if (te == NULL || me == NULL)
        {
            CHECK("R13-E3a trainer/monset entry present", false);
            continue;
        }
        CHECK("R13-E3a trainer metadata wire match",
              t->facilityClass == te->payload[0]
              && memcmp(t->trainerName, te->payload + 4u, 8u) == 0);
        CHECK("R13-E3a trainer speech wire match",
              memcmp(t->speechBefore, te->payload + 12u, 12u) == 0
              && memcmp(t->speechWin, te->payload + 24u, 12u) == 0
              && memcmp(t->speechLose, te->payload + 36u, 12u) == 0);
        CHECK("R13-E3a monSet arena-resident", MonSetResident(t->monSet));
        CHECK("R13-E3a mon-set leaf bytes exact",
              t->monSet != NULL
              && memcmp(t->monSet, me->payload, me->payloadSize) == 0);
    }

    /* Shared 882-mons pool: every row is a byte-exact 16->14 transform. */
    CHECK("R13-E3a pool sized 882x16",
          poolEntry->payloadSize
              == (size_t)EMERALD_FRONTIER_MONS_COUNT * EMERALD_FRONTIER_MON_WIRE);
    for (i = 0u; i < 32u; i++)
    {
        const uint8_t *w = poolEntry->payload + i * EMERALD_FRONTIER_MON_WIRE;
        const struct FacilityMon *m = &gBattleFrontierMons[i];
        bool movesOk = (EncLe16(w + 2u) == m->moves[0]
                     && EncLe16(w + 4u) == m->moves[1]
                     && EncLe16(w + 6u) == m->moves[2]
                     && EncLe16(w + 8u) == m->moves[3]);
        CHECK("R13-E3a mons row species", EncLe16(w + 0u) == m->species);
        CHECK("R13-E3a mons row moves", movesOk);
        CHECK("R13-E3a mons row scalar tail",
              w[10u] == m->itemTableId && w[11u] == m->evSpread
              && w[12u] == m->nature);
    }
    /* A row whose itemTableId indexes into the published held-items table. */
    {
        const struct FacilityMon *m = &gBattleFrontierMons[0];
        if (m->itemTableId < EMERALD_FRONTIER_HELDITEMS_COUNT
         && heldEntry != NULL)
            CHECK("R13-E3a held-item resolution",
                  gBattleFrontierHeldItems[m->itemTableId]
                      == EncLe16(heldEntry->payload + 2u * m->itemTableId));
    }
    /* Held items + banned species leaf copies. */
    for (i = 0u; i < (size_t)EMERALD_FRONTIER_HELDITEMS_COUNT && heldEntry; i++)
        allHeldOk &= (gBattleFrontierHeldItems[i]
                      == EncLe16(heldEntry->payload + 2u * i));
    CHECK("R13-E3a held items leaf exact", allHeldOk == 1u);
    CHECK("R13-E3a banned leaf exact",
          bannedEntry != NULL
          && memcmp(gFrontierBannedSpecies, bannedEntry->payload,
                    (size_t)EMERALD_FRONTIER_BANNED_COUNT * 2u) == 0);

    /* Tent families: three trainer tables + three mons pools, monSet leaves
     * index into each tent's own pool. */
    for (i = 0u; i < 3u; i++)
    {
        const struct Gen3ResourcePackEntry *pool =
            Gen3ResourcePack_FindByCanonicalName(pack, kFrontierTentMonsKeys[i]);
        const struct BattleFrontierTrainer *tt;
        const struct FacilityMon *poolPtr;
        size_t j;
        switch (i)
        {
        case 0: tt = gSlateportBattleTentTrainers;
                poolPtr = gSlateportBattleTentMons; break;
        case 1: tt = gVerdanturfBattleTentTrainers;
                poolPtr = gVerdanturfBattleTentMons; break;
        default: tt = gFallarborBattleTentTrainers;
                 poolPtr = gFallarborBattleTentMons; break;
        }
        CHECK("R13-E3a tent pool present", pool != NULL);
        for (j = 0u; j < EMERALD_FRONTIER_TENT_TRAINER_COUNT; j++)
        {
            const struct Gen3ResourcePackEntry *te = Gen3ResourcePack_FindByCanonicalName(
                pack, kFrontierTentTrainerKeys[i][j]);
            const struct Gen3ResourcePackEntry *me = Gen3ResourcePack_FindByCanonicalName(
                pack, kFrontierTentMonSetKeys[i][j]);
            const struct BattleFrontierTrainer *r = &tt[j];
            if (te == NULL || me == NULL)
                continue;
            CHECK("R13-E3a tent trainer wire", r->facilityClass == te->payload[0]);
            CHECK("R13-E3a tent monSet arena-resident", MonSetResident(r->monSet));
            CHECK("R13-E3a tent mon-set leaf exact",
                  r->monSet != NULL
                  && memcmp(r->monSet, me->payload, me->payloadSize) == 0);
            /* every index in the tent leaf is within the tent's own pool. */
            {
                uint16_t poolSize = (i == 0u) ? EMERALD_FRONTIER_TENT_SLATEPORT_MONS
                                   : EMERALD_FRONTIER_TENT_VERDANTURF_MONS;
                size_t k;
                int oob = 0;
                for (k = 0u; k < me->payloadSize / 2u - 1u; k++)
                    oob |= (EncLe16(me->payload + 2u * k) >= poolSize);
                CHECK("R13-E3a tent indices in-pool", oob == 0);
            }
        }
        CHECK("R13-E3a tent mons transformed",
              poolPtr[0].species
                  == EncLe16(pool->payload + 0u));
    }

    /* The mon-set arena is a registered COMPAT_OBJECT range (schema 22). */
    if (index != NULL
     && EmeraldResourceRangeIndex_Lookup(index,
            (uintptr_t)gBattleFrontierTrainers[0].monSet, &hit))
        CHECK("R13-E3a mon-set arena COMPAT_OBJECT range",
              hit.role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT
              && hit.schema == EMERALD_FRONTIER_SCHEMA_MON_SET);
    else
        CHECK("R13-E3a mon-set arena resolvable", false);

    Gen3ResourcePack_Destroy(pack);
}

/* State-v5 fresh-process relocation: establish facility-style pointers into
 * the published tables, capture the mon-set arena, tear the session down
 * (all monSet NULL, the arena range unregistered, the arena freed), then
 * republish from a fresh snapshot and prove every monSet pointer resolves
 * into the CURRENT registered mon-set arena range (no dangling creator
 * pointer survives) while the facility aliases still point at the published
 * (non-zero) fill targets. The loader seam publishes the HOST_DATA train/mon
 * tables itself, so gFacilityTrainers/gFacilityTrainerMons (engine EWRAM
 * aliases re-pointed at SetFacilityPtrsGetLevel) resolve into these tables
 * by construction; the mon-set arena is the only malloc'd span the State-v5
 * walker must re-derive, so the relocation proof centres on it. */
static void TestFrontierRelocation(const char *packPath)
{
    struct EmeraldResourceRangeIndex *index;
    struct EmeraldFrontierCompatDiagnostics diag;
    enum EmeraldFrontierCompatStatus status;
    const u16 *republishedMonSet;
    struct EmeraldResourceRangeHit hit;
    uintptr_t oldArenaBase = 0u;

    /* Establish the facility aliases (SetFacilityPtrsGetLevel-equivalent). */
    gFacilityTrainers = gBattleFrontierTrainers;
    gFacilityTrainerMons = gBattleFrontierMons;

    index = EmeraldResourceCompat_GetRangeIndex();
    /* Capture the production mon-set arena base (the creator's span). */
    if (index != NULL
     && EmeraldResourceRangeIndex_Lookup(index,
            (uintptr_t)gBattleFrontierTrainers[0].monSet, &hit))
        oldArenaBase = (uintptr_t)gBattleFrontierTrainers[0].monSet
                     - hit.rangeOffset;

    /* Tear the session down: every monSet NULL, the arena span unregistered. */
    EmeraldFrontierCompat_ClearMigratedEntries();
    CHECK("R13-E3a teardown NULLs monSet (no creator pointer survives)",
          gBattleFrontierTrainers[0].monSet == NULL
          && gSlateportBattleTentTrainers[0].monSet == NULL);
    CHECK("R13-E3a teardown releases arena",
          EmeraldFrontierCompat_GetMonSetArenaBytes() == 0u);
    if (index != NULL && oldArenaBase != 0u)
        CHECK("R13-E3a old arena span unregistered (no creator pointer survives)",
              !EmeraldResourceRangeIndex_Lookup(index, oldArenaBase, &hit));

    /* Republish from a fresh snapshot: a brand-new mon-set arena. */
    status = RunFrontierSeamDirect(packPath, &diag);
    CHECK("R13-E3a republish OK", status == EMERALD_FRONTIER_OK);

    republishedMonSet = gBattleFrontierTrainers[0].monSet;
    CHECK("R13-E3a republished monSet non-NULL", republishedMonSet != NULL);
    /* The monSet is re-derived into the CURRENT registered arena range, not a
     * dangling creator address. */
    CHECK("R13-E3a republished monSet in current arena",
          MonSetResident(republishedMonSet));
    index = EmeraldResourceCompat_GetRangeIndex();
    if (index != NULL
     && EmeraldResourceRangeIndex_Lookup(index, (uintptr_t)republishedMonSet,
                                         &hit))
        CHECK("R13-E3a republished monSet COMPAT_OBJECT schema-22",
              hit.role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT
              && hit.schema == EMERALD_FRONTIER_SCHEMA_MON_SET);

    /* The facility aliases still resolve into the published (non-zero) tables
     * after the republish, and reads through them are valid. */
    CHECK("R13-E3a facility aliases re-resolve to published tables",
          gFacilityTrainers == gBattleFrontierTrainers
          && gFacilityTrainerMons == gBattleFrontierMons
          && gFacilityTrainers[0].monSet != NULL
          && MonSetResident(gFacilityTrainers[0].monSet)
          && gFacilityTrainerMons[0].species != 0u);
}

/* The frontier seam REFUSES a pack whose frontier records are malformed (a
 * resized trainer row, a broken row->monSet GBA edge, an unterminated mon-set,
 * an out-of-range mon index, a missing shared mons pool, a transform-length
 * mismatch, or missing held/banned). Nothing is published. */
static void TestFrontierRefusals(const char *tempDir, const char *prodPack)
{
    static const char *const labels[7] = {
        "fr_bad_trainer_size", "fr_bad_monset_ptr", "fr_unterminated",
        "fr_mon_index", "fr_missing_pool", "fr_mon_transform",
        "fr_missing_held_banned" };
    const u16 *liveMonSet = gBattleFrontierTrainers[0].monSet;
    size_t liveArena = EmeraldFrontierCompat_GetMonSetArenaBytes();
    int mode;
    for (mode = 0; mode < 7; mode++)
    {
        struct EmeraldFrontierCompatDiagnostics diag;
        enum EmeraldFrontierCompatStatus status;
        char path[512];

        snprintf(path, sizeof(path), "%s/%s.rpack", tempDir, labels[mode]);
        CHECK("R13-E3a variant pack builds",
              BuildFrontierVariantPack(prodPack, path, mode));
        status = RunFrontierSeamDirect(path, &diag);
        CHECK("R13-E3a frontier variant refused", status != EMERALD_FRONTIER_OK);
    }
    /* The refused direct runs did not overwrite the live published tables. */
    CHECK("R13-E3a refusal leaves live tables untouched",
          gBattleFrontierTrainers[0].monSet == liveMonSet
          && EmeraldFrontierCompat_GetMonSetArenaBytes() == liveArena);
}

/* ---- R13-E3a-2: facility AUX + pike/pyramid wild-handoff tests. ---- */

/* R13-E3a-2 facility AUX fill targets against a freshly driven production
 * session. RunFrontierSeamDirect publishes into the SAME HOST_DATA fill
 * targets the loader path uses, so the assertions check the publication
 * seam output that E3a-2 carries. Run while the production frontier state is
 * live (before any teardown) so each table reflects the pack exactly. */
static void TestFrontierAux(const char *packPath)
{
    struct EmeraldFrontierCompatDiagnostics diag;
    enum EmeraldFrontierCompatStatus status;

    status = RunFrontierSeamDirect(packPath, &diag);
    CHECK("R13-E3a2 aux publication OK", status == EMERALD_FRONTIER_OK);
    if (status != EMERALD_FRONTIER_OK)
        return;

    /* E3a-1 (786) + E3a-2 (57) fill targets. */
    CHECK("R13-E3a2 published count 843",
          EmeraldFrontierCompat_GetPublishedCount() == 843u);

    /* Factory move lists: first strategy is swords-dance led, last ends with
     * the 0xFFFF (MOVE_NONE) terminator. */
    CHECK("R13-E3a2 factory lead move",
          gBattleFactoryMovesTotalPreparation[0] == MOVE_SWORDS_DANCE);
    CHECK("R13-E3a2 factory list terminator",
          gBattleFactoryMovesTotalPreparation[27] == MOVE_NONE);

    /* Pike NPC transform: row0 is OBJ_EVENT_GFX_POKEFAN_F (0x12) with the
     * packed 6-byte speech prefix; row2's third speech id lands in the row. */
    CHECK("R13-E3a2 pike npc row0 gfx", gBattlePikeNPC[0].graphicsId == 0x12u);
    CHECK("R13-E3a2 pike npc row0 speech1",
          gBattlePikeNPC[0].speechId1 == 3u);
    CHECK("R13-E3a2 pike npc row0 speech2",
          gBattlePikeNPC[0].speechId2 == 5u);
    CHECK("R13-E3a2 pike npc row0 speech3",
          gBattlePikeNPC[0].speechId3 == 6u);

    /* Pyramid floor templates: row0 pins the busy floor of the Magic Room /
       Battle Pyramid (7 items, 3 trainers, run multiplier 128, layout top). */
    CHECK("R13-E3a2 floor row0 numItems", gBattlePyramidFloorTemplates[0].numItems == 7u);
    CHECK("R13-E3a2 floor row0 numTrainers",
          gBattlePyramidFloorTemplates[0].numTrainers == 3u);
    CHECK("R13-E3a2 floor row0 runMultiplier",
          gBattlePyramidFloorTemplates[0].runMultiplier == 128u);
    CHECK("R13-E3a2 floor row0 layoutOffsets",
          gBattlePyramidFloorTemplates[0].layoutOffsets[0] == 0u
          && gBattlePyramidFloorTemplates[0].layoutOffsets[1] == 0u
          && gBattlePyramidFloorTemplates[0].layoutOffsets[2] == 1u);

    /* Pyramid pickup items: lvl50 row0 and lvlopen row0 share one DEDUPED
     * payload, both leading with a Hyper Potion. */
    CHECK("R13-E3a2 pickup [0][0]", gBattlePyramidPickupItems[0][0] == ITEM_HYPER_POTION);
    CHECK("R13-E3a2 pickup [1][0] deduped", gBattlePyramidPickupItems[1][0] == ITEM_HYPER_POTION);

    /* Apprentice transform: row0 ot id/facility class survive the 88->86 cut. */
    CHECK("R13-E3a2 apprentice row0 otId", gApprentices[0].otId == 48585u);
    CHECK("R13-E3a2 apprentice row0 class",
          gApprentices[0].facilityClass == FACILITY_CLASS_BUG_CATCHER);

    /* Brain: ids + mons published non-empty. */
    CHECK("R13-E3a2 brain ids published", gFrontierBrainTrainerIds[0] != 0u);
    CHECK("R13-E3a2 brain mons published",
          gFrontierBrainsMons[0][0][0].species != 0u);

    /* Pike wild-encounter handoff: headers 0-3 real (rate 10), row4 sentinel. */
    CHECK("R13-E3a2 pike header0 info", gBattlePikeWildMonHeaders[0].landMonsInfo != NULL);
    if (gBattlePikeWildMonHeaders[0].landMonsInfo != NULL)
    {
        CHECK("R13-E3a2 pike header0 rate",
              gBattlePikeWildMonHeaders[0].landMonsInfo->encounterRate == 10u);
        CHECK("R13-E3a2 pike header0 slot0 species",
              gBattlePikeWildMonHeaders[0].landMonsInfo->wildPokemon[0].species != 0u);
    }
    CHECK("R13-E3a2 pike sentinel row4", gBattlePikeWildMonHeaders[4].landMonsInfo == NULL);

    /* Pyramid wild-encounter handoff: rows0-6 real (row6 is the rate-8 set),
       row7 sentinel. */
    CHECK("R13-E3a2 pyramid header6 info",
          gBattlePyramidWildMonHeaders[6].landMonsInfo != NULL);
    if (gBattlePyramidWildMonHeaders[6].landMonsInfo != NULL)
    {
        CHECK("R13-E3a2 pyramid header6 rate",
              gBattlePyramidWildMonHeaders[6].landMonsInfo->encounterRate == 8u);
        CHECK("R13-E3a2 pyramid header6 slot",
              gBattlePyramidWildMonHeaders[6].landMonsInfo->wildPokemon != NULL);
    }
    CHECK("R13-E3a2 pyramid sentinel row7",
          gBattlePyramidWildMonHeaders[7].landMonsInfo == NULL);
}

enum {
    FRONTIER_AUX_BAD_FACTORY_SIZE = 0,  /* factory total-preparation resized */
    FRONTIER_AUX_BAD_PIKE_NPC_SIZE,     /* pike NPC 8B-row wire resized */
    FRONTIER_AUX_BAD_APPRENTICE_SCHEMA, /* apprentice row0 schema != 35 */
    FRONTIER_AUX_MISSING_WILD_SLOT,     /* drop the pike-1 slot resource */
    FRONTIER_AUX_BAD_WILD_INFO_PTR,     /* break pike header row0 info ptr */
    FRONTIER_AUX_BAD_PIKE_WILDMON_SIZE, /* pike lvl50/1 wildmon table resized */
    FRONTIER_AUX_BAD_PYRAMID_FLOOR_SIZE,/* pyramid floor templates resized */
    FRONTIER_AUX_VARIANT_COUNT
};

/* A production-pack variant with EXACTLY ONE R13-E3a-2 resource damaged or
 * dropped so the refusal is provably the facility-AUX seam's. Every other
 * entry (E3a-1 frontier/tent + all other families) is copied through verbatim
 * with its own digests, so the pack passes every earlier seam.  */
static bool BuildFrontierAuxVariantPack(const char *srcPath, const char *dstPath,
                                        int mode)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourcePackProfile profile;
    struct Gen3ResourcePackBuild *build = NULL;
    struct Gen3ResourcePackProfileInput pin;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackBytes bytes = { NULL, 0 };
    struct Gen3ResourcePackDiagnosticList diag;
    uint8_t bufSha[32];
    uint8_t provenanceSha[32];
    uint8_t hdrBuf[256];       /* pike/pyramid wild-header block scratch */
    size_t count;
    size_t i;
    FILE *f = NULL;
    bool ok = false;

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(srcPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
        goto done;
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);

    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
        goto done;
    if (!Gen3ResourcePack_GetProfile(pack, &profile))
        goto done;
    memset(&pin, 0, sizeof(pin));
    pin.basePackVersion = profile.basePackVersion;
    pin.catalogVersion = profile.catalogVersion;
    pin.extractionManifestVersion = profile.extractionManifestVersion;
    pin.canonicalRepresentationVersion = profile.canonicalRepresentationVersion;
    pin.sourceRomSize = profile.sourceRomSize;
    pin.sourceRomSha1 = profile.sourceRomSha1;
    pin.sourceRomSha256 = profile.sourceRomSha256;
    memcpy(pin.gameCode, profile.gameCode, 4u);
    memcpy(pin.makerCode, profile.makerCode, 2u);
    pin.softwareRevision = profile.softwareRevision;
    memcpy(pin.gameId, profile.gameId, GEN3_PACK_GAME_ID_SIZE);
    pin.catalogSha256 = profile.catalogSha256;
    pin.extractionManifestSha256 = profile.extractionManifestSha256;
    if (Gen3ResourcePackBuild_SetProfile(build, &pin, &diag) != GEN3_PACK_OK)
        goto done;

    memset(provenanceSha, 0x5A, sizeof(provenanceSha));
    count = Gen3ResourcePack_GetEntryCount(pack);

    for (i = 0u; i < count; i++)
    {
        const struct Gen3ResourcePackEntry *e = Gen3ResourcePack_GetEntry(pack, i);
        const uint8_t *payload = e->payload;
        const uint8_t *payloadSha = e->payloadSha256;
        size_t payloadSize = e->payloadSize;
        uint32_t schema = e->schema;
        uint64_t romOffset = e->sourceRomOffset;

        /* Drop the pike-1 wild slot resource (E3a-2 schema 37). */
        if (mode == FRONTIER_AUX_MISSING_WILD_SLOT
         && strcmp(e->canonicalName, kFrontierAuxPikeWildInfos[0].key) == 0)
            continue;

        /* Factory total-preparation move list: shrink below the expected
         * 56-byte leaf (28 u16). */
        if (mode == FRONTIER_AUX_BAD_FACTORY_SIZE
         && strcmp(e->canonicalName, kFrontierAuxFactoryMoves[0]) == 0)
        {
            payloadSize = 40u;
            DigestSha256(e->payload, payloadSize, bufSha);
            payloadSha = bufSha;
        }
        /* Pike NPC: shrink the 25x8 wire block below the expected 200 bytes. */
        if (mode == FRONTIER_AUX_BAD_PIKE_NPC_SIZE
         && strcmp(e->canonicalName, kFrontierAuxPikeNpcKey) == 0)
        {
            payloadSize =
                (size_t)EMERALD_FRONTIER_AUX_PIKE_NPC_SLOTS
                    * EMERALD_FRONTIER_AUX_PIKE_NPC_WIRE - 4u;
            DigestSha256(e->payload, payloadSize, bufSha);
            payloadSha = bufSha;
        }
        /* Apprentice row 0: retag the entry with a non-35 schema so the
         * (key, schema 35) resolution fails before any transform. */
        if (mode == FRONTIER_AUX_BAD_APPRENTICE_SCHEMA
         && strcmp(e->canonicalName, kFrontierAuxApprenticeKeys[0]) == 0)
            schema = 99u;
        /* Pike wild headers: re-point row0's land info GBA pointer (+4) so the
         * header->info edge breaks. */
        if (mode == FRONTIER_AUX_BAD_WILD_INFO_PTR
         && strcmp(e->canonicalName, kFrontierAuxPikeWildHeadersKey) == 0
         && e->payloadSize <= sizeof(hdrBuf))
        {
            memcpy(hdrBuf, e->payload, e->payloadSize);
            hdrBuf[4] = (uint8_t)(hdrBuf[4] + 4u);        /* +4 into the ptr */
            payload = hdrBuf;
            DigestSha256(payload, e->payloadSize, bufSha);
            payloadSha = bufSha;
        }
        /* Pike lvl50/1 wild-mon table: shrink below the expected 36 bytes. */
        if (mode == FRONTIER_AUX_BAD_PIKE_WILDMON_SIZE
         && strcmp(e->canonicalName, kFrontierAuxPikeWildMons[0]) == 0)
        {
            payloadSize = 32u;
            DigestSha256(e->payload, payloadSize, bufSha);
            payloadSha = bufSha;
        }
        /* Pyramid floor templates: shrink the 16x16 wire below 256 bytes. */
        if (mode == FRONTIER_AUX_BAD_PYRAMID_FLOOR_SIZE
         && strcmp(e->canonicalName, kFrontierAuxPyramidFloor[0]) == 0)
        {
            payloadSize =
                (size_t)EMERALD_FRONTIER_AUX_PYRAMID_FLOOR_SLOTS
                    * EMERALD_FRONTIER_AUX_PYRAMID_FLOOR_WIRE - 4u;
            DigestSha256(e->payload, payloadSize, bufSha);
            payloadSha = bufSha;
        }

        memset(&entry, 0, sizeof(entry));
        entry.schema = schema;
        entry.flags = e->flags;
        entry.representation = e->representation;
        entry.sourceEncoding = e->sourceEncoding;
        entry.canonicalName = e->canonicalName;
        entry.key = &e->key;
        entry.type = e->type;
        entry.canonicalPayload = payload;
        entry.canonicalPayloadSize = payloadSize;
        entry.canonicalPayloadSha256 = payloadSha;
        entry.sourceRomOffset = romOffset;
        entry.sourceEncodedSize = e->sourceEncodedSize;
        entry.sourceEncodedSha256 = provenanceSha;
        if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
            goto done;
    }

    if (Gen3ResourcePackWriter_Write(build, &bytes, &diag) != GEN3_PACK_OK)
        goto done;
    f = fopen(dstPath, "wb");
    if (f == NULL)
        goto done;
    if (fwrite(bytes.data, 1, bytes.size, f) != bytes.size)
        goto done;
    fclose(f);
    f = NULL;
    ok = true;

done:
    if (f != NULL)
        fclose(f);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    if (pack != NULL)
        Gen3ResourcePack_Destroy(pack);
    return ok;
}

/* The E3a-2 seam REFUSES a pack with any facility-AUX record malformed (a
 * resized factory/pike-npc/pike-wildmon/pyramid-floor leaf, a retagged
 * apprentice schema, a dropped wild slot, or a broken header->info edge).
 * Nothing is re-published: the live published tables (set by the last
 * successful run) are untouched. NOTE: the EMERALD_FRONTIER_ERR_WILD_RATE
 * branch is driven purely by the generated metadata constants (all pike sets
 * are 10, pyramid sets 4/8), so it is not reachable from a pack mutation in
 * this harness - documented in the test's commit notes. */
static void TestFrontierAuxRefusals(const char *tempDir, const char *prodPack)
{
    static const char *const labels[FRONTIER_AUX_VARIANT_COUNT] = {
        "fa_bad_factory_size", "fa_bad_pike_npc_size", "fa_bad_apprentice_schema",
        "fa_missing_wild_slot", "fa_bad_wild_info_ptr", "fa_bad_pike_wildmon_size",
        "fa_bad_pyramid_floor_size" };
    static const enum EmeraldFrontierCompatStatus expects[FRONTIER_AUX_VARIANT_COUNT] = {
        EMERALD_FRONTIER_ERR_FACTORY_MOVES,
        EMERALD_FRONTIER_ERR_PIKE_NPC,
        EMERALD_FRONTIER_ERR_APPRENTICE,
        EMERALD_FRONTIER_ERR_WILD_SLOT,
        EMERALD_FRONTIER_ERR_WILD_INFO,
        EMERALD_FRONTIER_ERR_PIKE_WILDMON,
        EMERALD_FRONTIER_ERR_PYRAMID_FLOOR,
    };
    int mode;

    for (mode = 0; mode < FRONTIER_AUX_VARIANT_COUNT; mode++)
    {
        struct EmeraldFrontierCompatDiagnostics diag;
        enum EmeraldFrontierCompatStatus status;
        size_t liveCount;
        u16 livePikeGfx, liveApprenticeOt, liveBrainId, livePickup00;
        const struct WildPokemonInfo *livePikeInfo;
        char path[512];

        /* Clear any prior session's frontier arenas/ranges and re-deploy the
         * PRODUCTION pack so each refusal starts from a freshly-published,
         * non-zero baseline AND the process-global range index holds only a
         * bounded set of frontier ranges (7 stacked failed E3a-2 sessions
         * would each leak an E3a-1 mon-set range and eventually collide). */
        EmeraldFrontierCompat_ClearMigratedEntries();
        status = RunFrontierSeamDirect(prodPack, &diag);
        CHECK("R13-E3a2 refusal baseline deploy", status == EMERALD_FRONTIER_OK);

        liveCount = EmeraldFrontierCompat_GetPublishedCount();
        livePikeGfx = gBattlePikeNPC[0].graphicsId;
        liveApprenticeOt = gApprentices[0].otId;
        liveBrainId = gFrontierBrainTrainerIds[0];
        livePickup00 = gBattlePyramidPickupItems[0][0];
        livePikeInfo = gBattlePikeWildMonHeaders[0].landMonsInfo;

        snprintf(path, sizeof(path), "%s/%s.rpack", tempDir, labels[mode]);
        CHECK("R13-E3a2 variant pack builds",
              BuildFrontierAuxVariantPack(prodPack, path, mode));
        status = RunFrontierSeamDirect(path, &diag);
        CHECK("R13-E3a2 aux variant refused", status != EMERALD_FRONTIER_OK);
        CHECK("R13-E3a2 aux variant exact code", status == expects[mode]);

        /* The failed republish did not overwrite the deployed E3a-2 tables:
         * every E3a-2 target still carries the baseline deploy's content. */
        CHECK("R13-E3a2 refusal leaves deployed tables untouched",
              EmeraldFrontierCompat_GetPublishedCount() == liveCount
              && gBattlePikeNPC[0].graphicsId == livePikeGfx
              && gApprentices[0].otId == liveApprenticeOt
              && gFrontierBrainTrainerIds[0] == liveBrainId
              && gBattlePyramidPickupItems[0][0] == livePickup00
              && gBattlePikeWildMonHeaders[0].landMonsInfo == livePikeInfo);
    }
}

int main(int argc, char **argv)
{
    const char *tempDir;
    const char *prodPack;

    if (argc < 3)
    {
        fprintf(stderr,
                "usage: %s <temp-dir> <production-pack.rpack>\n", argv[0]);
        return 2;
    }
    tempDir = argv[1];
    prodPack = argv[2];

    CHECK("family fixtures load", LoadFamilyFixtures());
    CHECK("back fixtures load", LoadBackFamilyFixtures());
    if (gFailures != 0)
        return 1;

    TestLoaderUnavailable();
    TestLoaderRefusesTrainerOnlyPack(tempDir);
    TestLoaderRefusesTextVariant(tempDir, prodPack, true, false,
                                 "text_malformed");
    TestLoaderRefusesTextVariant(tempDir, prodPack, false, true,
                                 "text_missing");
    TestLoaderProductionPack(prodPack);
    TestMapPublication();
    TestTextPublication(prodPack);
    TestTextSkeletonFills();
    TestTextSlotPointers();
    TestTextCurrentCharRouting();
    /* R13-D1/D2 gameplay publication. Runs while the production text arena
     * is still live (BEFORE TestTextTransactionalRefusal tears it down), so
     * the R13-D2 gItems description pointers can be checked against the
     * R13-C item-arena bytes. */
    TestGameplayPublication(prodPack);
    /* R13-E1 trainer-data publication. Runs while the production session is
     * still live (BEFORE TestTextTransactionalRefusal tears it down), so the
     * published gTrainers / gTrainerClassNames + party arena can be checked
     * against the pack. */
    TestTrainerPublication(prodPack);
    /* R13-E2 wild-encounter publication. Runs while the production session is
     * still live (BEFORE TestTextTransactionalRefusal tears it down), so the
     * published gWildMonHeaders / gWildEncounterInfos + slot arena can be
     * checked against the pack. */
    TestEncounterPublication(prodPack);
    /* R13-E3a-1 frontier/tent publication. Runs while the production session
     * is still live (BEFORE TestTextTransactionalRefusal tears it down), so
     * the published gBattleFrontierTrainers / gBattleFrontierMons / held /
     * banned / tent tables + the mon-set arena can be checked against the
     * pack. The relocation test then tears the frontier session down and
     * republishes from a fresh snapshot (the State-v5 fresh-process
     * relocation proof for gFacilityTrainers/gFacilityTrainerMons). */
    TestFrontierTrainerGraph(prodPack);
    TestFrontierRelocation(prodPack);
    /* R13-E3a-2 facility-AUX + pike/pyramid wild-handoff publication. Runs
     * while the frontier session republished by TestFrontierRelocation is
     * live, so the whole 843-fill-target frontier publication can be checked. */
    TestFrontierAux(prodPack);
    TestTextTransactionalRefusal(tempDir, prodPack);
    /* R13-E1 failure-matrix: trainer seams against freshly built variant
     * sessions (independent of the (now rolled-back) production session). */
    TestTrainerRefusals(tempDir, prodPack);
    /* R13-E2 failure-matrix: encounter seams against freshly built variant
     * sessions (after the production session is rolled back). */
    TestEncounterRefusals(tempDir, prodPack);
    /* R13-E3a-1 failure-matrix: frontier seams against freshly built variant
     * sessions (after the production session is rolled back). */
    TestFrontierRefusals(tempDir, prodPack);
    /* R13-E3a-2 failure-matrix: facility-AUX seams against freshly built
     * variant sessions (after the E3a-1 refusals). Verifies a malformed
     * facility-AUX resource refuses without clobbering the live tables. */
    TestFrontierAuxRefusals(tempDir, prodPack);

    FreeFamilyFixtures();
    FreeBackFamilyFixtures();
    if (gFailures != 0)
    {
        printf("emerald runtime loader test FAILED: %d/%d checks\n",
               gFailures, gChecks);
        return 1;
    }
    printf("emerald runtime loader test passed (%d checks)\n", gChecks);
    return 0;
}

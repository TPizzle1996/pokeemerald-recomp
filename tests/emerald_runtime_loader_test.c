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
#include "emerald/resources/text_skeleton_arrays.generated.h"
#include "item_use.h"   /* R13-D2: native ItemUse*_... symbols for pointer checks */
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
    CHECK("production pack entry count pinned at 15750 (R13-D2)", packCount == 15750u);

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
    TestTextPublication(prodPack);
    TestTextSkeletonFills();
    TestTextSlotPointers();
    TestTextCurrentCharRouting();
    /* R13-D1/D2 gameplay publication. Runs while the production text arena
     * is still live (BEFORE TestTextTransactionalRefusal tears it down), so
     * the R13-D2 gItems description pointers can be checked against the
     * R13-C item-arena bytes. */
    TestGameplayPublication(prodPack);
    TestTextTransactionalRefusal(tempDir, prodPack);

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

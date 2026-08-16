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
#include "emerald/resources/emerald_rom_profile.h"
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
    TestLoaderProductionPack(prodPack);

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

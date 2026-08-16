/* Stage 7: production compatibility / linker-load proof (family-wide, R7B/R8).
 *
 * Reuses the ENTIRE Stage R7B/R8 harness (tests/emerald_trainer_native_compat_test.c)
 * - the REAL Emerald decompressor (src/platform/bios.c LZ77UnCompWram), the REAL
 * native trainer front/back tables (front_pic_tables.h + back_pic_tables.h,
 * native non-const branches), the REAL load-path functions (src/decompress.c
 * DecompressPicFromTable_2 / LoadCompressedSpritePalette), the family fixture
 * loaders (descriptor-driven, committed .lz assets decoded by the real
 * decompressor, plus the R8 back-family RAW sheets), the table-state capture +
 * "only the migrated slots changed" assertions, and the R7B/R8 compat seam -
 * but drives the seam with the REAL PRODUCTION ROM_BASE snapshot built from the
 * REAL installed production pack, for the FULL trainer family: 93 front sheets
 * + 93 front palettes + 6 shared back-pic palette consumers (R7B), and the 8
 * back sheets + 2 back-only Red/Leaf palettes (R8):
 *
 *   retail ROM -> production-qualified manifest -> emerald-bpee01-v1.rpack
 *     -> EmeraldResourceSession_BuildRomBaseCandidate (provider id
 *        "emerald.rom-base.bpee01", precedence 300, 1804 entries;
 *        186 trainer-front + 10 trainer-back (R8) + 1608 Pokémon battle (R9))
 *     -> Gen3ResourceCandidate_Build (1804-resource ROM_BASE snapshot;
 *        front+back family contract checks below)
 *     -> EmeraldResourceCompat_InitializeFromSnapshot
 *     -> EmeraldResourceCompatibilityImage (literal-only LZ77 streams)
 *     -> live native trainer tables
 *     -> existing Emerald decompression/load consumers
 *
 * This closes the full production chain in one process: retail bytes -> pack ->
 * snapshot -> compatibility image -> the exact same trainer tables and loaders
 * the running game uses.
 *
 * The family descriptor (resources/extraction/emerald/bpee01/
 * trainer_front_family.toml) drives BOTH sides, so the production chain is
 * verified against the same canonical source artifacts the R7B unit regression
 * pins:
 *   - the 186 canonical payloads are derived from its slug/palette_dir fields
 *     (graphics/trainers/front_pics/<slug>.4bpp.lz + graphics/trainers/
 *     <palette_dir>/<slug>.gbapal.lz, decoded by the REAL decompressor);
 *   - every one of the 186 contracts must resolve through the production
 *     snapshot with a stable key, type/schema, ROM_BASE winner and payload
 *     byte-identical to that canonical payload;
 *   - the seam's resource-id -> table-slot mapping is cross-checked via the
 *     descriptor's index/back_palette_index fields: every published stream
 *     must decode to the canonical payload of the trainer the descriptor
 *     places at that slot.
 *
 * "Byte-for-byte vs canonical" is proven two independent ways:
 *   1. the ROM_BASE snapshot's payloads are byte-identical to the canonical
 *      decoded artifacts, for all 186 resources;
 *   2. the published compatibility streams, decoded by the REAL decompressor
 *      and the REAL DecompressPicFromTable_2 / LoadCompressedSpritePalette
 *      load path, equal those same canonical bytes at the descriptor's table
 *      index.
 *
 * Usage:
 *   emerald_trainer_native_compat_production \
 *     <pack.rpack> --catalog <catalog.toml>... --descriptor <family-descriptor.toml>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Rename the included R7B harness's main() so it is linkable here but never
 * called; every harness helper (CHECK, LoadFamilyFixturesFrom, FixtureForIndex,
 * BuildFamilyId, Capture*Table, AssertOnlyMigratedChanged, IsMigratedBackPaletteSlot,
 * the load-path functions, the stubs) lands in this translation unit and is
 * reused as-is. The harness's own six tests do NOT run here; the production
 * chain gets its own family-wide sequence below. */
#define main emerald_compat_production_r6_main
#include "emerald_trainer_native_compat_test.c"
#undef main

/* Resource-pack / catalog / session / id headers the R7B harness does not
 * include (or includes only transitively). */
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"
#include "emerald/resources/emerald_resource_session.h"

static enum Gen3ResourceType CatalogTypeForName(const char *name)
{
    if (name == NULL)
        return GEN3_RESOURCE_TYPE_INVALID;
    if (strcmp(name, "bitmap") == 0)
        return GEN3_RESOURCE_TYPE_BITMAP;
    if (strcmp(name, "tile-graphics") == 0)
        return GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    if (strcmp(name, "palette") == 0)
        return GEN3_RESOURCE_TYPE_PALETTE;
    if (strcmp(name, "sprite-sheet") == 0)
        return GEN3_RESOURCE_TYPE_SPRITE_SHEET;
    return GEN3_RESOURCE_TYPE_BINARY;
}

/* Append every resource of one catalog TOML file to `catalog` (R9 Stage 4:
 * the real pack contract spans several family catalogs). Duplicate ids are
 * rejected by Gen3ResourceCatalog_Add, matching the importer's fail-closed
 * merge. */
static bool AddCatalogFile(struct Gen3ResourceCatalog *catalog, const char *path)
{
    struct Gen3Buffer buf;
    struct Gen3TomlDocument doc;
    struct Gen3ResourceDiagnosticList diag;
    char errbuf[512];
    size_t i;
    bool ok = false;

    if (!Gen3Util_ReadFile(path, &buf, errbuf, sizeof(errbuf)))
    {
        fprintf(stderr, "catalog read: %s\n", errbuf);
        return false;
    }
    if (!Gen3Toml_Parse(buf.data, buf.length, &doc, errbuf, sizeof(errbuf)))
    {
        fprintf(stderr, "catalog parse: %s\n", errbuf);
        Gen3Buffer_Destroy(&buf);
        return false;
    }
    Gen3ResourceDiagnostics_Init(&diag);
    for (i = 0; i < Gen3Toml_GetArrayCount(&doc.root, "resources"); i++)
    {
        const struct Gen3TomlMap *item =
            Gen3Toml_GetArrayItem(&doc.root, "resources", i);
        const char *id, *type;
        long long schema;
        bool required = false;
        if (!Gen3Toml_GetString(item, "id", &id)
         || !Gen3Toml_GetString(item, "type", &type)
         || !Gen3Toml_GetInteger(item, "schema", &schema))
        {
            fprintf(stderr, "catalog record %zu missing id/type/schema\n", i);
            goto done;
        }
        Gen3Toml_GetBool(item, "required_for_base", &required);
        if (!Gen3ResourceCatalog_Add(catalog, id, CatalogTypeForName(type),
                                     (uint32_t)schema, required, &diag))
        {
            fprintf(stderr, "catalog add failed for %s\n", id);
            goto done;
        }
    }
    ok = true;
done:
    Gen3ResourceDiagnostics_Destroy(&diag);
    Gen3Toml_Destroy(&doc);
    Gen3Buffer_Destroy(&buf);
    return ok;
}

/* Resolve ONE family contract through the production snapshot and verify it
 * byte-for-byte against the canonical fixture (itself the committed-.lz decode
 * the R7B unit regression pins). Runs its own CHECKs. */
static void CheckFamilyContract(struct Gen3ResourceSnapshot *snapshot,
                                const char *name,
                                enum Gen3ResourceType type,
                                const u8 *canonical, u32 canonicalSize,
                                const char *what)
{
    struct Gen3ResourceView view;
    Gen3ResourceHandle handle;
    Gen3ResourceKey derivedKey;
    char label[160];

    CHECK("find handle",
          Gen3ResourceSnapshot_FindHandle(snapshot, name, &handle) == GEN3_RESOURCE_OK);
    snprintf(label, sizeof(label), "%s resolves", what);
    CHECK(label, Gen3ResourceSnapshot_Resolve(snapshot, handle, type, 1u, &view)
                     == GEN3_RESOURCE_OK);
    Gen3ResourceId_DeriveKey(name, &derivedKey);
    snprintf(label, sizeof(label), "%s key is the derived stable key", what);
    CHECK(label, memcmp(view.key.bytes, derivedKey.bytes, GEN3_RESOURCE_KEY_SIZE) == 0);
    snprintf(label, sizeof(label), "%s type/schema", what);
    CHECK(label, view.type == type && view.schema == 1u);
    snprintf(label, sizeof(label), "%s winner is ROM_BASE", what);
    CHECK(label, view.winningProviderId != NULL
                     && strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) == 0
                     && view.winningProviderPrecedence == EMERALD_ROM_BASE_PRECEDENCE);
    snprintf(label, sizeof(label), "%s payload == canonical (%zu bytes)", what,
             (size_t)canonicalSize);
    CHECK(label, view.payloadSize == canonicalSize
                     && memcmp(view.payload, canonical, canonicalSize) == 0);
}

int main(int argc, char **argv)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourceCatalog *catalog = NULL;
    struct Gen3ResourceCandidate *candidate = NULL;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList gdiag;
    struct EmeraldResourceSessionInfo info;
    struct EmeraldResourceCompatDiagnostics cdiag;
    enum EmeraldResourceSessionError sessionError;
    enum EmeraldResourceCompatStatus status;
    struct CompressedSpriteSheet beforeSheet[ARRAY_COUNT(gTrainerFrontPicTable)];
    struct CompressedSpritePalette beforePal[ARRAY_COUNT(gTrainerFrontPicPaletteTable)];
    struct CompressedSpritePalette beforeBackPal[ARRAY_COUNT(gTrainerBackPicPaletteTable)];
    struct CompressedSpriteSheet beforeBackSheet[ARRAY_COUNT(gTrainerBackPicTable)];
    struct BackFrameCapture beforeBackFrames;
    u8 decodedSheet[EMERALD_TRAINER_SHEET_SIZE];
    u8 decodedPalette[EMERALD_TRAINER_PALETTE_SIZE];
    u8 decodedBackSheet[10240]; /* largest back sheet: red/leaf, 5 frames */
    char id[128];
    char label[160];
    size_t i;
    const char *descriptor = NULL;

    {
        const char *catalogPaths[16];
        size_t catalogCount = 0u;
        int arg;
        for (arg = 2; arg < argc; arg++)
        {
            if (strcmp(argv[arg], "--catalog") == 0 && arg + 1 < argc
             && catalogCount < sizeof(catalogPaths) / sizeof(catalogPaths[0]))
            {
                catalogPaths[catalogCount++] = argv[++arg];
            }
            else if (strcmp(argv[arg], "--descriptor") == 0 && arg + 1 < argc)
            {
                descriptor = argv[++arg];
            }
            else
            {
                fprintf(stderr, "usage: %s <pack.rpack> --catalog <catalog.toml>... "
                                "--descriptor <family-descriptor.toml>\n", argv[0]);
                return 2;
            }
        }
        if (catalogCount == 0u || descriptor == NULL)
        {
            fprintf(stderr, "usage: %s <pack.rpack> --catalog <catalog.toml>... "
                            "--descriptor <family-descriptor.toml>\n", argv[0]);
            return 2;
        }
        catalog = Gen3ResourceCatalog_Create();
        if (catalog == NULL)
        {
            fprintf(stderr, "out of memory creating catalog\n");
            return 1;
        }
        for (arg = 0; (size_t)arg < catalogCount; arg++)
        {
            if (!AddCatalogFile(catalog, catalogPaths[arg]))
            {
                Gen3ResourceCatalog_Destroy(catalog);
                return 1;
            }
        }
        Gen3ResourceDiagnostics_Init(&gdiag);
        if (!Gen3ResourceCatalog_Finalize(catalog, &gdiag))
        {
            fprintf(stderr, "catalog finalize failed\n");
            Gen3ResourceDiagnostics_Destroy(&gdiag);
            Gen3ResourceCatalog_Destroy(catalog);
            return 1;
        }
        Gen3ResourceDiagnostics_Destroy(&gdiag);
    }

    printf("== Stage 7: production compatibility / linker-load proof "
           "(family-wide) ==\n");

    /* 0. Canonical payloads: the descriptor-driven fixture load. Each fixture
     * is the REAL committed .lz artifact (graphics/trainers/front_pics/<slug>
     * .4bpp.lz + graphics/trainers/<palette_dir>/<slug>.gbapal.lz) decoded by
     * the REAL decompressor - exactly the bytes the R7B unit regression pins,
     * so the production chain demonstrably serves the same canonical content. */
    CHECK("family fixtures load from production descriptor",
          LoadFamilyFixturesFrom(descriptor));
    CHECK("back fixtures load (committed R8 descriptor)",
          LoadBackFamilyFixtures());
    if (gFailures != 0)
        return 1;

    /* 1. Open the REAL installed production pack. */
    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(argv[1], &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
    {
        printf("FAIL: cannot open pack '%s'\n", argv[1]);
        Gen3ResourcePackDiagnostics_Destroy(&packDiag);
        Gen3ResourceCatalog_Destroy(catalog);
        return 1;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    printf("opened production pack : %zu entries\n", Gen3ResourcePack_GetEntryCount(pack));
    CHECK("production pack has 1804 entries (186 trainer front + 10 trainer "
          "back + 1608 Pokémon battle)",
          Gen3ResourcePack_GetEntryCount(pack) == 1804u);
    printf("loaded catalog          : %zu resources\n", Gen3ResourceCatalog_Count(catalog));
    CHECK("catalog has 1804 resources (186 trainer front + 10 trainer back + "
          "1608 Pokémon battle)",
          Gen3ResourceCatalog_Count(catalog) == 1804u);

    /* 3. Build the production ROM_BASE candidate + snapshot from the real pack. */
    Gen3ResourceDiagnostics_Init(&gdiag);
    memset(&info, 0, sizeof(info));
    sessionError = EmeraldResourceSession_BuildRomBaseCandidate(
        pack, catalog, &candidate, &info, &gdiag);
    CHECK("BuildRomBaseCandidate ok",
          sessionError == EMERALD_SESSION_OK && candidate != NULL);
    if (sessionError != EMERALD_SESSION_OK || candidate == NULL)
    {
        printf("FAIL: BuildRomBaseCandidate (%d)\n", (int)sessionError);
        Gen3ResourceDiagnostics_Destroy(&gdiag);
        Gen3ResourceCatalog_Destroy(catalog);
        Gen3ResourcePack_Destroy(pack);
        return 1;
    }
    printf("ROM_BASE provider       : %s %s kind=%d precedence=%u entryCount=%zu\n",
           info.providerId, info.providerVersion, (int)info.kind,
           info.precedence, info.entryCount);
    CHECK("provider id is emerald.rom-base.bpee01",
          strcmp(info.providerId, EMERALD_ROM_BASE_PROVIDER_ID) == 0);
    CHECK("provider precedence 300", info.precedence == EMERALD_ROM_BASE_PRECEDENCE);
    CHECK("provider version v1", strcmp(info.providerVersion, "v1") == 0);
    CHECK("provider entryCount 1804 (186 front + 10 back + 1608 Pokémon battle)",
          info.entryCount == 1804u);

    CHECK("snapshot builds",
          Gen3ResourceCandidate_Build(candidate, &snapshot, &gdiag) && snapshot != NULL);
    Gen3ResourceDiagnostics_Destroy(&gdiag);
    if (snapshot == NULL)
    {
        printf("FAIL: candidate build\n");
        Gen3ResourceCandidate_Destroy(candidate);
        Gen3ResourceCatalog_Destroy(catalog);
        Gen3ResourcePack_Destroy(pack);
        return 1;
    }

    /* 4. Production snapshot content == canonical artifacts for ALL 186
     * contracts (independent of the intermediate pack entry checks). */
    for (i = 0; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        const struct FamilyTrainerFixture *fix = FixtureForIndex((int)i);
        BuildFamilyId(id, sizeof(id), fix->canonical, false);
        snprintf(label, sizeof(label), "sheet %zu (%s)", i, fix->canonical);
        CheckFamilyContract(snapshot, id, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                            fix->sheet, EMERALD_TRAINER_SHEET_SIZE, label);
        BuildFamilyId(id, sizeof(id), fix->canonical, true);
        snprintf(label, sizeof(label), "palette %zu (%s)", i, fix->canonical);
        CheckFamilyContract(snapshot, id, GEN3_RESOURCE_TYPE_PALETTE,
                            fix->palette, EMERALD_TRAINER_PALETTE_SIZE, label);
    }

    /* 5. Pristine pre-publish baseline: fresh process, no prior publish, so
     * every migrated slot starts at the NULL sentinel (no legacy leaf symbols
     * linked on native, R7B §4/§14-A; R8 removes the Red/Leaf compiled pointers
     * too - all 8 back-palette slots, all 8 back SHEET table slots and all 34
     * back SpriteFrameImage slots start NULL). */
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
        CHECK("front sheet slot starts NULL (no legacy dep)",
              gTrainerFrontPicTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicPaletteTable); i++)
        CHECK("front palette slot starts NULL (no legacy dep)",
              gTrainerFrontPicPaletteTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicPaletteTable); i++)
        CHECK("back-pal slot starts NULL (no legacy dep)",
              gTrainerBackPicPaletteTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
        CHECK("back sheet slot starts NULL (no legacy dep)",
              gTrainerBackPicTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        size_t f;
        for (f = 0; f < sBackFixtures[i].frames; f++)
            CHECK("back frame slot starts NULL (no legacy dep)",
                  kBackFrameArrays[i][f].data == NULL);
    }
    CaptureSheetTable(beforeSheet);
    CapturePaletteTable(beforePal);
    CaptureBackPaletteTable(beforeBackPal);
    CaptureBackSheetTable(beforeBackSheet);
    CaptureBackFrames(&beforeBackFrames);

    /* 6. Publish through the R7B compatibility seam with the production
     * snapshot. */
    status = EmeraldResourceCompat_InitializeFromSnapshot(snapshot, &cdiag);
    CHECK("initialize ok", status == EMERALD_COMPAT_OK);
    CHECK("success diagnostics empty", cdiag.stage[0] == '\0');

    /* 7. Only the migrated data slots changed (236: 186 front + 6 shared
     * back-pal + 2 Red/Leaf back-pal + 8 back sheets + 34 back frames); every
     * other entry of the five live tables is byte-identical - size/tag
     * included (R7B §4/§7, R8). */
    AssertOnlyMigratedChanged(beforeSheet, beforePal, beforeBackPal,
                              beforeBackSheet, &beforeBackFrames);

    /* 8. Shared back-pic palette consumers: the six gTrainerBackPicPaletteTable
     * slots publish the SAME normal-palette stream as their owner trainer's
     * front palette (battle_controller_player.c indexes it by player gender),
     * and each decodes to the owner's canonical palette. */
    for (i = 0; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        const struct FamilyTrainerFixture *fix = FixtureForIndex((int)i);
        if (fix->backPaletteIndex < 0)
            continue;
        snprintf(label, sizeof(label), "shared back %d reuses %s's front stream",
                 fix->backPaletteIndex, fix->canonical);
        CHECK(label, gTrainerBackPicPaletteTable[fix->backPaletteIndex].data
                         == gTrainerFrontPicPaletteTable[i].data);
        LZ77UnCompWram((const u32 *)(const void *)
                           gTrainerBackPicPaletteTable[fix->backPaletteIndex].data,
                       decodedPalette);
        snprintf(label, sizeof(label), "shared back %d decodes to owner palette",
                 fix->backPaletteIndex);
        CHECK(label, memcmp(decodedPalette, fix->palette,
                            EMERALD_TRAINER_PALETTE_SIZE) == 0);
    }

    /* 9. Real decompressor on the published compat streams == canonical, for
     * all 93 sheets + 93 palettes, at the descriptor's table index. */
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
    {
        const struct FamilyTrainerFixture *fix = FixtureForIndex((int)i);
        LZ77UnCompWram((const u32 *)(const void *)gTrainerFrontPicTable[i].data,
                       decodedSheet);
        snprintf(label, sizeof(label), "compat sheet %zu decodes to canonical", i);
        CHECK(label, memcmp(decodedSheet, fix->sheet,
                            EMERALD_TRAINER_SHEET_SIZE) == 0);
        LZ77UnCompWram((const u32 *)(const void *)gTrainerFrontPicPaletteTable[i].data,
                       decodedPalette);
        snprintf(label, sizeof(label), "compat palette %zu decodes to canonical", i);
        CHECK(label, memcmp(decodedPalette, fix->palette,
                            EMERALD_TRAINER_PALETTE_SIZE) == 0);
    }

    /* 9b. R8: the back family through the PRODUCTION chain. The 10 back
     * contracts (8 raw sheets + 2 Red/Leaf palettes) must resolve through the
     * production snapshot byte-identical to the committed artifacts, and the
     * published live tables must expose them exactly as the GBA runtime
     * consumes them: RAW sheet streams verbatim (the sprite pipeline copies
     * images[frame].data into OBJ VRAM; DecompressTrainerBackPic LZ77-decodes
     * these same bytes), the 34 frame slots pointing into the sheet stream at
     * per-frame offsets, and the Red/Leaf palette slots decoding to canonical. */
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        const struct FamilyBackFixture *fix = &sBackFixtures[i];
        BuildBackFamilyId(id, sizeof(id), fix->canonical, false);
        snprintf(label, sizeof(label), "back sheet %zu (%s) contract",
                 i, fix->canonical);
        CheckFamilyContract(snapshot, id, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                            fix->sheet, fix->sheetSize, label);
        if (fix->palette != NULL)
        {
            BuildBackFamilyId(id, sizeof(id), fix->canonical, true);
            snprintf(label, sizeof(label), "back palette %zu (%s) contract",
                     i, fix->canonical);
            CheckFamilyContract(snapshot, id, GEN3_RESOURCE_TYPE_PALETTE,
                                fix->palette, EMERALD_TRAINER_PALETTE_SIZE, label);
        }
    }
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicPaletteTable); i++)
        CHECK("every back-pal slot migrated (R8)", IsMigratedBackPaletteSlot(i));
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        const struct FamilyBackFixture *fix = &sBackFixtures[i];
        size_t f;
        snprintf(label, sizeof(label), "back sheet %zu stream == raw canonical",
                 i);
        CHECK(label, memcmp(gTrainerBackPicTable[i].data, fix->sheet,
                            fix->sheetSize) == 0);
        snprintf(label, sizeof(label), "back sheet %zu size metadata", i);
        CHECK(label, gTrainerBackPicTable[i].size == fix->sheetSize);
        snprintf(label, sizeof(label), "back sheet %zu tag metadata", i);
        CHECK(label, gTrainerBackPicTable[i].tag == i);
        for (f = 0; f < fix->frames; f++)
        {
            snprintf(label, sizeof(label), "back frame %zu/%u in stream (%s)",
                     f, fix->frames, fix->canonical);
            CHECK(label,
                  (const u8 *)(const void *)kBackFrameArrays[i][f].data
                      == (const u8 *)(const void *)gTrainerBackPicTable[i].data
                             + TRAINER_PIC_SIZE * f);
            snprintf(label, sizeof(label), "back frame %zu/%u decodes raw (%s)",
                     f, fix->frames, fix->canonical);
            CHECK(label,
                  memcmp(kBackFrameArrays[i][f].data,
                         fix->sheet + TRAINER_PIC_SIZE * f, TRAINER_PIC_SIZE) == 0);
            CHECK("back frame size metadata",
                  kBackFrameArrays[i][f].size == TRAINER_PIC_SIZE);
        }
        if (fix->palette != NULL)
        {
            LoadCompressedSpritePalette(&gTrainerBackPicPaletteTable[fix->index]);
            snprintf(label, sizeof(label), "load path back palette == canonical (%s)",
                     fix->canonical);
            CHECK(label, memcmp(sLastLoadedPalette, fix->palette,
                                EMERALD_TRAINER_PALETTE_SIZE) == 0);
            CHECK("back palette tag metadata",
                  gTrainerBackPicPaletteTable[fix->index].tag == fix->index);
        }
    }

    /* 10. The actual native load path the game uses, for the full family. */
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
    {
        const struct FamilyTrainerFixture *fix = FixtureForIndex((int)i);
        DecompressPicFromTable_2(&gTrainerFrontPicTable[i], decodedSheet, SPECIES_NONE);
        snprintf(label, sizeof(label), "load path sheet %zu == canonical", i);
        CHECK(label, memcmp(decodedSheet, fix->sheet,
                            EMERALD_TRAINER_SHEET_SIZE) == 0);
        LoadCompressedSpritePalette(&gTrainerFrontPicPaletteTable[i]);
        snprintf(label, sizeof(label), "load path palette %zu == canonical", i);
        CHECK(label, memcmp(sLastLoadedPalette, fix->palette,
                            EMERALD_TRAINER_PALETTE_SIZE) == 0);
    }

    /* 11. Cross-check: the canonical fixtures are the committed-.lz decodes the
     * R7B unit regression pins (and the R8 back RAW sheets are the committed
     * .4bpp files verbatim), so steps 4/9/9b already prove the production chain
     * serves exactly those bytes (pack payload == legacy decoded). */

    /* Report. */
    if (gFailures != 0)
    {
        printf("PRODUCTION COMPAT/LINKER-LOAD PROOF: FAIL (%d/%d checks)\n",
               gFailures, gChecks);
        Gen3ResourceSnapshot_Destroy(snapshot);
        Gen3ResourceCandidate_Destroy(candidate);
        Gen3ResourceCatalog_Destroy(catalog);
        Gen3ResourcePack_Destroy(pack);
        FreeFamilyFixtures();
        return 1;
    }
    printf("PRODUCTION COMPAT/LINKER-LOAD PROOF: PASS (%d checks)\n", gChecks);
    printf("  production pack  : %s (%zu entries)\n", argv[1],
           Gen3ResourcePack_GetEntryCount(pack));
    printf("  provider         : %s %s kind=%d precedence=%u\n",
           info.providerId, info.providerVersion, (int)info.kind, info.precedence);
    printf("  family           : 186 front + 44 back published slots (236 total), "
           "all byte-identical to canonical\n");

    Gen3ResourceSnapshot_Destroy(snapshot);
    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    FreeFamilyFixtures();
    return 0;
}

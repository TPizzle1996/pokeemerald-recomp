/* Stage R5/R6/R7B/R8: native trainer-table publication + real load-path tests.
 *
 * Links the REAL Emerald decompressor (src/platform/bios.c LZ77UnCompWram), the
 * REAL native trainer front/back tables (front_pic_tables.h + back_pic_tables.h,
 * native non-const branches), and the REAL load-path functions (src/decompress.c
 * DecompressPicFromTable_2 / LoadCompressedSpritePalette), then drives the
 * compatibility seam end-to-end for the FULL trainer family: the R7B
 * trainer-front family (93 sheets + 93 palettes + 6 shared back-pic palette
 * consumers, 192 published slots) and the R8 trainer-BACK family (the 8 raw
 * back sheets in the sheet table, the 34 back SpriteFrameImage slots - the
 * live pixel surface - and the 2 Red/Leaf back-only palettes, 44 more
 * published slots, 236 total):
 *
 *   ROM_BASE snapshot -> EmeraldResourceCompat_InitializeFromSnapshot
 *       -> EmeraldResourceCompatibilityImage (literal-only LZ77 streams)
 *       -> live native trainer tables
 *       -> existing Emerald decompression/load consumers
 *
 * The family fixtures are driven by the R7A family descriptor
 * (resources/extraction/emerald/bpee01/trainer_front_family.toml): for each of
 * the 93 [[trainers]] entries the test loads the REAL committed source artifact
 * (graphics/trainers/front_pics/<slug>.4bpp.lz + graphics/trainers/
 * <palette_dir>/<slug>.gbapal.lz), decodes it with the real decompressor, and
 * uses the decoded bytes as the canonical payload for that table index. The
 * published tables must then decode back to exactly those bytes at exactly the
 * descriptor's index - so the seam's internal resource-id -> table-slot
 * mapping (kTrainerFrontSlots) cannot drift from the descriptor without this
 * test failing. No user ROM is needed (R5 §9/§10/§25).
 *
 * The migrated leaf symbols (gTrainerFrontPic_* / gTrainerPalette_* /
 * gTrainerBackPic_* / gTrainerBackPicPalette_*) are NOT compiled into this TU
 * (the native table branches expand to NULL sentinels; the INCBIN overrides
 * below are inert stubs): the test proves the native runtime path has zero
 * dependency on them (§14/§16).
 *
 * Coverage:
 *   - §7/§16 only the 236 migrated data slots change (front sheets, front
 *     palettes, all 8 back-palette slots, the 8 back SHEET table slots and the
 *     34 back SpriteFrameImage slots); every non-migrated field (size/tag/
 *     indices) is byte-identical before and after.
 *   - §14/A pre-publication state: all migrated slots start at the NULL
 *     sentinel; no consumer can read them until publication.
 *   - §3/§9 real-decompressor parity for all 186 LZ resources: decompress
 *     (published stream) == canonical fixture, at the descriptor's index; the
 *     8 back sheets are RAW entries, so the published stream IS the canonical
 *     bytes, and the 34 frame slots point into it at per-frame offsets.
 *   - §25 real native load path for the full family.
 *   - §11 resolve through the normal snapshot; winner id verified in diag.
 *   - §13/§24 transactional + fail-closed: wrong/missing resources (front AND
 *     back), wrong winners and mismatched images leave the live tables
 *     untouched.
 *   - §14 runtime hook (SetSnapshot/TryInitialize) is idempotent and gated
 *     on a registered snapshot.
 *   - §2/§3 R7B republish after state load: corrupt all 236 migrated slots
 *     with synthetic stale words, EmeraldResourceCompat_Republish repairs
 *     them to the current-session pointers, is idempotent, and never touches
 *     non-migrated entries.
 *   - §2/§18 republish unavailable before init: after Shutdown drops the
 *     session image, Republish fails closed (EMERALD_COMPAT_ERR_UNAVAILABLE)
 *     and ClearMigratedEntries returns exactly the migrated slots to the NULL
 *     sentinel.
 *
 * Compiled with the REAL native flags (-DPORTABLE -DNONMATCHING -DUBFIX
 * -DMODERN=1 -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1); the only stubs are
 * the sprite-registry loaders decompress.c calls (LoadSpriteSheet,
 * LoadSpritePalette) plus its support externs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/platform/bios.c" /* real LZ77UnCompWram + hardware globals */

#include "data.h"
#include "graphics.h"
#include "sprite.h"
#include "constants/trainers.h"
#include "constants/species.h"

#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_provider.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_trainer_native_compat.h"
#include "emerald/resources/pokemon_battle_slots.generated.h"

/* config.h (pulled in by global.h via bios.c) defines NDEBUG; re-enable assert
 * support so the checks below are actually enforced. */
#undef NDEBUG
#include <assert.h>

/* R8 §5: the front/back payload symbols (gTrainerFrontPic_*, gTrainerPalette_*,
 * gTrainerBackPic_*, gTrainerBackPicPalette_*) are GBA-only - the shared
 * data/graphics/trainers.h is gone (emptied by R7B, deleted by R8) and the
 * payloads compile from the GBA-only TUs trainers_front_payload.c /
 * trainers_back_payload.c, excluded from the native build. The native table
 * branches never reference them (NULL sentinels), so this TU has no payload
 * symbols at all - exactly as in the real native build; only the extern
 * declarations from graphics.h are visible. */
#include "../src/data/trainer_graphics/front_pic_tables.h"

/* R8: the back-pic SpriteFrameImage tables (normally defined in data.c) are the
 * LIVE pixel surface the compat seam publishes into - define them here in the
 * same native shape as data.c (NULL data sentinels, TRAINER_PIC_SIZE per frame)
 * so back_pic_tables.h's ARRAY_COUNT(frameArray) and the seam's 34-slot
 * publication see the real arrays. The back fixture loader cross-checks every
 * array's length against the descriptor `frames` field, so this cannot drift
 * from data.c. */
#define TRAINER_BACK_FRAME(trainerPic, frameNum) {NULL, TRAINER_PIC_SIZE}
struct SpriteFrameImage gTrainerBackPicTable_Brendan[] =
{
    TRAINER_BACK_FRAME(Brendan, 0), TRAINER_BACK_FRAME(Brendan, 1),
    TRAINER_BACK_FRAME(Brendan, 2), TRAINER_BACK_FRAME(Brendan, 3),
};
struct SpriteFrameImage gTrainerBackPicTable_May[] =
{
    TRAINER_BACK_FRAME(May, 0), TRAINER_BACK_FRAME(May, 1),
    TRAINER_BACK_FRAME(May, 2), TRAINER_BACK_FRAME(May, 3),
};
struct SpriteFrameImage gTrainerBackPicTable_Red[] =
{
    TRAINER_BACK_FRAME(Red, 0), TRAINER_BACK_FRAME(Red, 1),
    TRAINER_BACK_FRAME(Red, 2), TRAINER_BACK_FRAME(Red, 3),
    TRAINER_BACK_FRAME(Red, 4),
};
struct SpriteFrameImage gTrainerBackPicTable_Leaf[] =
{
    TRAINER_BACK_FRAME(Leaf, 0), TRAINER_BACK_FRAME(Leaf, 1),
    TRAINER_BACK_FRAME(Leaf, 2), TRAINER_BACK_FRAME(Leaf, 3),
    TRAINER_BACK_FRAME(Leaf, 4),
};
struct SpriteFrameImage gTrainerBackPicTable_RubySapphireBrendan[] =
{
    TRAINER_BACK_FRAME(RubySapphireBrendan, 0), TRAINER_BACK_FRAME(RubySapphireBrendan, 1),
    TRAINER_BACK_FRAME(RubySapphireBrendan, 2), TRAINER_BACK_FRAME(RubySapphireBrendan, 3),
};
struct SpriteFrameImage gTrainerBackPicTable_RubySapphireMay[] =
{
    TRAINER_BACK_FRAME(RubySapphireMay, 0), TRAINER_BACK_FRAME(RubySapphireMay, 1),
    TRAINER_BACK_FRAME(RubySapphireMay, 2), TRAINER_BACK_FRAME(RubySapphireMay, 3),
};
struct SpriteFrameImage gTrainerBackPicTable_Wally[] =
{
    TRAINER_BACK_FRAME(Wally, 0), TRAINER_BACK_FRAME(Wally, 1),
    TRAINER_BACK_FRAME(Wally, 2), TRAINER_BACK_FRAME(Wally, 3),
};
struct SpriteFrameImage gTrainerBackPicTable_Steven[] =
{
    TRAINER_BACK_FRAME(Steven, 0), TRAINER_BACK_FRAME(Steven, 1),
    TRAINER_BACK_FRAME(Steven, 2), TRAINER_BACK_FRAME(Steven, 3),
};

/* Back-descriptor row -> compiled frame array (row i covers table index i; the
 * back fixture loader enforces the row order). Non-const elements: the compat
 * seam and the republish test write the published pointers into the slots. */
static struct SpriteFrameImage *const kBackFrameArrays[EMERALD_TRAINER_BACK_SHEET_COUNT] =
{
    gTrainerBackPicTable_Brendan,
    gTrainerBackPicTable_May,
    gTrainerBackPicTable_Red,
    gTrainerBackPicTable_Leaf,
    gTrainerBackPicTable_RubySapphireBrendan,
    gTrainerBackPicTable_RubySapphireMay,
    gTrainerBackPicTable_Wally,
    gTrainerBackPicTable_Steven,
};

/* Real-array lengths for the loader's drift check against the descriptor
 * `frames` field (ARRAY_COUNT needs the array object, not a pointer to it). */
static const size_t kBackFrameCounts[EMERALD_TRAINER_BACK_SHEET_COUNT] =
{
    ARRAY_COUNT(gTrainerBackPicTable_Brendan),
    ARRAY_COUNT(gTrainerBackPicTable_May),
    ARRAY_COUNT(gTrainerBackPicTable_Red),
    ARRAY_COUNT(gTrainerBackPicTable_Leaf),
    ARRAY_COUNT(gTrainerBackPicTable_RubySapphireBrendan),
    ARRAY_COUNT(gTrainerBackPicTable_RubySapphireMay),
    ARRAY_COUNT(gTrainerBackPicTable_Wally),
    ARRAY_COUNT(gTrainerBackPicTable_Steven),
};

#include "../src/data/trainer_graphics/back_pic_tables.h"
#include "../src/decompress.c"

/* ------------------------------------------------------------------ */
/* Stubs for decompress.c support externs (the assertions only exercise */
/* LZ77UnCompWram; DrawSpindaSpots is a no-op for SPECIES_NONE, and the */
/* sprite-registry loaders capture instead of registering).             */
/* ------------------------------------------------------------------ */

static u8 sLastLoadedPalette[EMERALD_TRAINER_PALETTE_SIZE];

/* Sized generously (SPECIES_CELEBI = 251) so the decompress.c special-poke
 * loops never index past the stub. R9 §5: on native all four Pokémon battle
 * tables are mutable - the compat seam publishes the ROM_BASE session streams
 * into their .data slots (the production proof stamps + verifies them); the
 * const/non-const split mirrors data.h. The real table headers are re-checked
 * against the generated slot map by the Stage 8 full-family test. */
#if defined(NATIVE_LINUX)
struct CompressedSpriteSheet gMonFrontPicTable[512] = { 0 };
struct CompressedSpriteSheet gMonBackPicTable[512] = { 0 };
struct CompressedSpritePalette gMonPaletteTable[512] = { 0 };
struct CompressedSpritePalette gMonShinyPaletteTable[512] = { 0 };
#else
const struct CompressedSpriteSheet gMonFrontPicTable[512] = { 0 };
const struct CompressedSpriteSheet gMonBackPicTable[512] = { 0 };
const struct CompressedSpritePalette gMonPaletteTable[512] = { 0 };
const struct CompressedSpritePalette gMonShinyPaletteTable[512] = { 0 };
#endif

void *AllocZeroed(u32 size)
{
    void *p = malloc(size);
    if (p != NULL)
        memset(p, 0, size);
    return p;
}

void Free(void *pointer)
{
    free(pointer);
}

void DrawSpindaSpots(u16 species, u32 personality, u8 *dest, bool8 isFrontPic)
{
    (void)species;
    (void)personality;
    (void)dest;
    (void)isFrontPic;
}

u16 LoadSpriteSheet(const struct SpriteSheet *sheet)
{
    (void)sheet;
    return 0;
}

u8 LoadSpritePalette(const struct SpritePalette *palette)
{
    if (palette != NULL && palette->data != NULL)
        memcpy(sLastLoadedPalette, palette->data, EMERALD_TRAINER_PALETTE_SIZE);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

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
/* Family fixtures: driven by the R7A family descriptor               */
/* ------------------------------------------------------------------ */

#define FAMILY_DESCRIPTOR_PATH \
    "resources/extraction/emerald/bpee01/trainer_front_family.toml"

struct FamilyTrainerFixture
{
    char canonical[64];
    char slug[64];
    char paletteDir[16];     /* "front_pics" or "palettes" */
    int index;               /* table index (descriptor explicit index) */
    int backPaletteIndex;    /* gTrainerBackPicPaletteTable slot or -1 */
    u32 sizeMult;            /* table .size multiplier */
    u8 *sheet;               /* decoded canonical sheet (2048) */
    u8 *palette;             /* decoded canonical palette (32) */
};

static struct FamilyTrainerFixture sFixtures[EMERALD_TRAINER_FRONT_COUNT];
static size_t sFixtureCount = 0;

static int ReadFileBytes(const char *path, u8 **out, u32 *outSize)
{
    FILE *f = fopen(path, "rb");
    long sz;
    if (f == NULL)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return 0;
    }
    *out = (u8 *)malloc((size_t)sz);
    if (*out == NULL || fread(*out, 1, (size_t)sz, f) != (size_t)sz)
    {
        free(*out);
        *out = NULL;
        fclose(f);
        return 0;
    }
    fclose(f);
    *outSize = (u32)sz;
    return 1;
}

/* Build the family resource id for a trainer canonical component, mirroring
 * the descriptor's id templates (the same strings the seam's mapping builds). */
static void BuildFamilyId(char *out, size_t outSize,
                          const char *canonical, bool isPalette)
{
    if (isPalette)
        snprintf(out, outSize, "emerald:trainer/%s/battle/front/normal-palette",
                 canonical);
    else
        snprintf(out, outSize, "emerald:trainer/%s/battle/front/sheet",
                 canonical);
}

/* ------------------------------------------------------------------ */
/* Back-family fixtures: driven by the R8 back-family descriptor      */
/* ------------------------------------------------------------------ */

/* The front-fixture loaders (defined after the front loader section) - the
 * back loader reads the front back_palette_index map for the slot-coverage
 * check. */
static int LoadFamilyFixtures(void);
static const struct FamilyTrainerFixture *FixtureForIndex(int index);

#define BACK_FAMILY_DESCRIPTOR_PATH \
    "resources/extraction/emerald/bpee01/trainer_back_family.toml"

struct FamilyBackFixture
{
    char canonical[64];
    char slug[64];           /* artifact basename (brendan_rs/may_rs differ) */
    int index;               /* TRAINER_BACK_PIC_* value (0..7) */
    u32 sheetSize;           /* 8192 or 10240 (frames * TRAINER_PIC_SIZE) */
    u32 frames;              /* frame-array length (4/5) */
    u8 *sheet;               /* RAW canonical sheet bytes (verbatim payload) */
    u8 *palette;             /* decoded canonical palette (32) or NULL */
};

static struct FamilyBackFixture sBackFixtures[EMERALD_TRAINER_BACK_SHEET_COUNT];
static size_t sBackFixtureCount = 0;

/* Build the back-family resource id, mirroring the back descriptor's id
 * templates (the same strings the seam's mapping builds). */
static void BuildBackFamilyId(char *out, size_t outSize,
                              const char *canonical, bool isPalette)
{
    if (isPalette)
        snprintf(out, outSize, "emerald:trainer/%s/battle/back/palette",
                 canonical);
    else
        snprintf(out, outSize, "emerald:trainer/%s/battle/back/sheet",
                 canonical);
}

/* Load the REAL committed back-family artifacts for every trainer in the back
 * descriptor, and cross-check the descriptor itself: exactly 8 entries,
 * explicit indexes 0..7 each exactly once in row order, sheet_size ==
 * frames*2048, frames == the compiled frame-array length, raw sheet file size
 * == sheet_size (the raw bytes ARE the canonical payload), and has_palette
 * exactly on the Red/Leaf rows. The final union check proves every one of the
 * 8 gTrainerBackPicPaletteTable slots is migrated by exactly one source (6
 * front normal-palette aliases + 2 back-only palettes) - the R8 "all eight
 * slots ROM_BASE-only" contract cannot drift. */
static int LoadBackFamilyFixtures(void)
{
    struct Gen3Buffer buf;
    struct Gen3TomlDocument doc;
    char errbuf[512];
    size_t i;
    int seen[EMERALD_TRAINER_BACK_SHEET_COUNT] = { 0 };
    int palSlotCoverage[ARRAY_COUNT(gTrainerBackPicPaletteTable)] = { 0 };

    if (sBackFixtureCount == EMERALD_TRAINER_BACK_SHEET_COUNT)
        return 1; /* already loaded */
    if (!LoadFamilyFixtures())
        return 0; /* the union check reads the front back_palette_index map */
    if (!Gen3Util_ReadFile(BACK_FAMILY_DESCRIPTOR_PATH, &buf, errbuf, sizeof(errbuf)))
    {
        printf("FAIL: back descriptor read: %s\n", errbuf);
        return 0;
    }
    if (!Gen3Toml_Parse(buf.data, buf.length, &doc, errbuf, sizeof(errbuf)))
    {
        printf("FAIL: back descriptor parse: %s\n", errbuf);
        Gen3Buffer_Destroy(&buf);
        return 0;
    }
    if (Gen3Toml_GetArrayCount(&doc.root, "trainers") != EMERALD_TRAINER_BACK_SHEET_COUNT)
    {
        printf("FAIL: back descriptor trainer count != %u\n",
               (unsigned)EMERALD_TRAINER_BACK_SHEET_COUNT);
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&buf);
        return 0;
    }
    for (i = 0; i < EMERALD_TRAINER_BACK_SHEET_COUNT; i++)
    {
        const struct Gen3TomlMap *item = Gen3Toml_GetArrayItem(&doc.root, "trainers", i);
        struct FamilyBackFixture *fix = &sBackFixtures[i];
        const char *canonical = NULL, *slug = NULL;
        long long index = -1, sheetSize = 0, frames = 0;
        bool hasPalette = false;
        char sheetPath[512], palettePath[512];
        u8 *sheetRaw = NULL, *paletteLz = NULL;
        u32 sheetRawSize = 0, paletteLzSize = 0;

        if (!Gen3Toml_GetString(item, "canonical", &canonical)
         || !Gen3Toml_GetString(item, "slug", &slug)
         || !Gen3Toml_GetInteger(item, "index", &index)
         || !Gen3Toml_GetInteger(item, "sheet_size", &sheetSize)
         || !Gen3Toml_GetInteger(item, "frames", &frames))
        {
            printf("FAIL: back descriptor trainer %zu missing fields\n", i);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        Gen3Toml_GetBool(item, "has_palette", &hasPalette);
        if (index < 0 || index >= EMERALD_TRAINER_BACK_SHEET_COUNT)
        {
            printf("FAIL: back descriptor trainer %zu index %lld out of range\n",
                   i, index);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        if (seen[index] != 0)
        {
            printf("FAIL: back descriptor duplicate index %lld\n", index);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        if ((int)i != (int)index)
        {
            /* Fixture row i == table index i: the rows must be in table-index
             * order for the kBackFrameArrays cross-check to be meaningful. */
            printf("FAIL: back descriptor row %zu index %lld out of row order\n",
                   i, index);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        seen[index] = 1;
        if (sheetSize < 1 || sheetSize % EMERALD_TRAINER_SHEET_SIZE != 0)
        {
            printf("FAIL: back trainer %zu sheet_size %lld not a frame multiple\n",
                   i, sheetSize);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        if (frames < 1 || frames > 5)
        {
            printf("FAIL: back trainer %zu frames %lld out of range\n", i, frames);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        if (sheetSize != (long long)frames * EMERALD_TRAINER_SHEET_SIZE)
        {
            printf("FAIL: back trainer %zu sheet_size != frames*2048\n", i);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        if (kBackFrameCounts[i] != (size_t)frames)
        {
            printf("FAIL: back trainer %zu frames %lld != compiled array length %zu\n",
                   i, frames, kBackFrameCounts[i]);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        if ((int)i == 2 || (int)i == 3)
        {
            /* Red/Leaf carry the 2 back-only palettes; nobody else may. */
            if (!hasPalette)
            {
                printf("FAIL: back trainer %zu (red/leaf) missing has_palette\n", i);
                Gen3Toml_Destroy(&doc);
                Gen3Buffer_Destroy(&buf);
                return 0;
            }
        }
        else if (hasPalette)
        {
            printf("FAIL: back trainer %zu unexpected has_palette\n", i);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }

        memset(fix, 0, sizeof(*fix));
        snprintf(fix->canonical, sizeof(fix->canonical), "%s", canonical);
        snprintf(fix->slug, sizeof(fix->slug), "%s", slug);
        fix->index = (int)index;
        fix->sheetSize = (u32)sheetSize;
        fix->frames = (u32)frames;

        snprintf(sheetPath, sizeof(sheetPath), "graphics/trainers/back_pics/%s.4bpp",
                 slug);
        if (!ReadFileBytes(sheetPath, &sheetRaw, &sheetRawSize))
        {
            printf("FAIL: cannot read raw back sheet for %s (%s)\n",
                   canonical, sheetPath);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        if (sheetRawSize != fix->sheetSize)
        {
            printf("FAIL: back sheet %s size %u != descriptor %u\n",
                   canonical, sheetRawSize, fix->sheetSize);
            free(sheetRaw);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        fix->sheet = sheetRaw; /* verbatim: the RAW payload IS the stream */
        if (hasPalette)
        {
            fix->palette = (u8 *)malloc(EMERALD_TRAINER_PALETTE_SIZE);
            if (fix->palette == NULL)
            {
                printf("FAIL: back palette alloc for %s\n", canonical);
                Gen3Toml_Destroy(&doc);
                Gen3Buffer_Destroy(&buf);
                return 0;
            }
            snprintf(palettePath, sizeof(palettePath),
                     "graphics/trainers/back_pics/%s.gbapal.lz", slug);
            if (!ReadFileBytes(palettePath, &paletteLz, &paletteLzSize))
            {
                printf("FAIL: cannot read back palette for %s (%s)\n",
                       canonical, palettePath);
                Gen3Toml_Destroy(&doc);
                Gen3Buffer_Destroy(&buf);
                return 0;
            }
            LZ77UnCompWram((const u32 *)(const void *)paletteLz, fix->palette);
            free(paletteLz);
        }
    }
    Gen3Toml_Destroy(&doc);
    Gen3Buffer_Destroy(&buf);

    /* The index set must be exactly 0..7. */
    for (i = 0; i < EMERALD_TRAINER_BACK_SHEET_COUNT; i++)
    {
        if (seen[i] != 1)
        {
            printf("FAIL: back descriptor missing index %zu\n", i);
            return 0;
        }
    }

    /* Every gTrainerBackPicPaletteTable slot is migrated by exactly one source:
     * the 6 front normal-palette aliases (R7B) + the 2 back-only palettes (R8). */
    for (i = 0; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        int slot = FixtureForIndex((int)i)->backPaletteIndex;
        if (slot >= 0 && slot < (int)ARRAY_COUNT(gTrainerBackPicPaletteTable))
            palSlotCoverage[slot]++;
    }
    sBackFixtureCount = EMERALD_TRAINER_BACK_SHEET_COUNT;
    for (i = 0; i < sBackFixtureCount; i++)
    {
        if (sBackFixtures[i].palette != NULL)
            palSlotCoverage[sBackFixtures[i].index]++;
    }
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicPaletteTable); i++)
    {
        if (palSlotCoverage[i] != 1)
        {
            printf("FAIL: back-palette slot %zu covered %d times (must be 1)\n",
                   i, palSlotCoverage[i]);
            return 0;
        }
    }
    return 1;
}

static void FreeBackFamilyFixtures(void)
{
    size_t i;
    for (i = 0; i < sBackFixtureCount; i++)
    {
        free(sBackFixtures[i].sheet);
        free(sBackFixtures[i].palette);
    }
    sBackFixtureCount = 0;
}

/* Load + decode the REAL committed .lz artifacts for every trainer in the
 * descriptor, and cross-check the descriptor itself: exactly 93 entries,
 * explicit indexes 0..92 each exactly once in row order, decoded payload
 * sizes 2048/32. The decoded bytes are the canonical payloads the ROM_BASE
 * provider serves. */
static int LoadFamilyFixturesFrom(const char *descriptorPath)
{
    struct Gen3Buffer buf;
    struct Gen3TomlDocument doc;
    char errbuf[512];
    size_t i;
    int seen[EMERALD_TRAINER_FRONT_COUNT] = { 0 };

    if (sFixtureCount == EMERALD_TRAINER_FRONT_COUNT)
        return 1; /* already loaded */
    if (!Gen3Util_ReadFile(descriptorPath, &buf, errbuf, sizeof(errbuf)))
    {
        printf("FAIL: family descriptor read: %s\n", errbuf);
        return 0;
    }
    if (!Gen3Toml_Parse(buf.data, buf.length, &doc, errbuf, sizeof(errbuf)))
    {
        printf("FAIL: family descriptor parse: %s\n", errbuf);
        Gen3Buffer_Destroy(&buf);
        return 0;
    }
    if (Gen3Toml_GetArrayCount(&doc.root, "trainers") != EMERALD_TRAINER_FRONT_COUNT)
    {
        printf("FAIL: descriptor trainer count != %u\n",
               (unsigned)EMERALD_TRAINER_FRONT_COUNT);
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&buf);
        return 0;
    }
    for (i = 0; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        const struct Gen3TomlMap *item = Gen3Toml_GetArrayItem(&doc.root, "trainers", i);
        struct FamilyTrainerFixture *fix = &sFixtures[i];
        const char *canonical = NULL, *slug = NULL, *paletteDir = "front_pics";
        long long index = -1, backIndex = -1, sizeMult = 1;
        char sheetPath[512], palettePath[512];
        u8 *sheetLz = NULL, *paletteLz = NULL;
        u32 sheetLzSize = 0, paletteLzSize = 0;

        if (!Gen3Toml_GetString(item, "canonical", &canonical)
         || !Gen3Toml_GetString(item, "slug", &slug)
         || !Gen3Toml_GetInteger(item, "index", &index))
        {
            printf("FAIL: descriptor trainer %zu missing canonical/slug/index\n", i);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        Gen3Toml_GetString(item, "palette_dir", &paletteDir);
        Gen3Toml_GetInteger(item, "back_palette_index", &backIndex);
        Gen3Toml_GetInteger(item, "size_mult", &sizeMult);
        if (index < 0 || index >= EMERALD_TRAINER_FRONT_COUNT)
        {
            printf("FAIL: descriptor trainer %zu index %lld out of range\n",
                   i, index);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        if (seen[index] != 0)
        {
            printf("FAIL: descriptor duplicate index %lld\n", index);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        if ((int)i != (int)index)
        {
            /* FixtureForIndex(i) == row i, so the descriptor rows must be in
             * table-index order for the drift check to be meaningful. */
            printf("FAIL: descriptor row %zu index %lld out of row order\n", i, index);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        seen[index] = 1;
        if (backIndex < 0)
            backIndex = -1;
        if (sizeMult < 1)
            sizeMult = 1;

        memset(fix, 0, sizeof(*fix));
        snprintf(fix->canonical, sizeof(fix->canonical), "%s", canonical);
        snprintf(fix->slug, sizeof(fix->slug), "%s", slug);
        snprintf(fix->paletteDir, sizeof(fix->paletteDir), "%s", paletteDir);
        fix->index = (int)index;
        fix->backPaletteIndex = (int)backIndex;
        fix->sizeMult = (u32)sizeMult;

        snprintf(sheetPath, sizeof(sheetPath), "graphics/trainers/front_pics/%s.4bpp.lz",
                 slug);
        snprintf(palettePath, sizeof(palettePath), "graphics/trainers/%s/%s.gbapal.lz",
                 paletteDir, slug);
        if (!ReadFileBytes(sheetPath, &sheetLz, &sheetLzSize)
         || !ReadFileBytes(palettePath, &paletteLz, &paletteLzSize))
        {
            printf("FAIL: cannot read artifacts for %s (%s / %s)\n",
                   canonical, sheetPath, palettePath);
            free(sheetLz);
            free(paletteLz);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        fix->sheet = (u8 *)malloc(EMERALD_TRAINER_SHEET_SIZE);
        fix->palette = (u8 *)malloc(EMERALD_TRAINER_PALETTE_SIZE);
        if (fix->sheet == NULL || fix->palette == NULL)
        {
            printf("FAIL: fixture alloc for %s\n", canonical);
            free(sheetLz);
            free(paletteLz);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&buf);
            return 0;
        }
        LZ77UnCompWram((const u32 *)(const void *)sheetLz, fix->sheet);
        LZ77UnCompWram((const u32 *)(const void *)paletteLz, fix->palette);
        free(sheetLz);
        free(paletteLz);
    }
    Gen3Toml_Destroy(&doc);
    Gen3Buffer_Destroy(&buf);

    /* The index set must be exactly 0..92. */
    for (i = 0; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        if (seen[i] != 1)
        {
            printf("FAIL: descriptor missing index %zu\n", i);
            return 0;
        }
    }
    sFixtureCount = EMERALD_TRAINER_FRONT_COUNT;
    return 1;
}

/* Unit-test entry point: load from the committed family descriptor path
 * (production passes its own path). Idempotent. */
static int LoadFamilyFixtures(void)
{
    return LoadFamilyFixturesFrom(FAMILY_DESCRIPTOR_PATH);
}

/* The descriptor rows are in table-index order; the fixture at position i
 * covers table index i. */
static const struct FamilyTrainerFixture *FixtureForIndex(int index)
{
    assert(index >= 0 && index < EMERALD_TRAINER_FRONT_COUNT);
    return &sFixtures[index];
}

static void FreeFamilyFixtures(void)
{
    size_t i;
    for (i = 0; i < sFixtureCount; i++)
    {
        free(sFixtures[i].sheet);
        free(sFixtures[i].palette);
    }
    sFixtureCount = 0;
}

/* ------------------------------------------------------------------ */
/* Snapshot construction through the NORMAL catalog + candidate path   */
/* ------------------------------------------------------------------ */

struct FamilySnapshotOpts
{
    const char *providerId;         /* default EMERALD_ROM_BASE_PROVIDER_ID */
    enum Gen3ResourceProviderKind kind;
    uint32_t precedence;
    int omitPaletteIndex;           /* -1: all palettes included */
    int omitBackSheetIndex;         /* -1: all 8 back sheets included */
    int omitBackPaletteIndex;       /* -1: all back-only palettes included */
    const u8 *wrongSizePalette;     /* palette bytes replacing the fixture */
    int wrongSizePaletteIndex;      /* palette replaced at this index */
    u32 wrongSizePaletteSize;
};

static void BuildFamilySnapshotOpts(struct FamilySnapshotOpts *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->providerId = EMERALD_ROM_BASE_PROVIDER_ID;
    opts->kind = GEN3_PROVIDER_ROM_BASE;
    opts->precedence = EMERALD_ROM_BASE_PRECEDENCE;
    opts->omitPaletteIndex = -1;
    opts->omitBackSheetIndex = -1;
    opts->omitBackPaletteIndex = -1;
    opts->wrongSizePaletteIndex = -1;
}

/* Build a snapshot through the NORMAL catalog + candidate + build path with
 * all 196 family contracts (186 front + 10 back) or the failure variants
 * selected by `opts`. */
static struct Gen3ResourceSnapshot *BuildFamilySnapshot(
    const struct FamilySnapshotOpts *opts)
{
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourceProvider *provider;
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList diag;
    int i;

    Gen3ResourceDiagnostics_Init(&diag);
    metadata.id = opts->providerId;
    metadata.version = "v1";
    metadata.kind = opts->kind;
    metadata.precedence = opts->precedence;
    provider = Gen3ResourceProvider_Create(&metadata);
    assert(provider != NULL);

    catalog = Gen3ResourceCatalog_Create();
    assert(catalog != NULL);

    for (i = 0; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        const struct FamilyTrainerFixture *fix = FixtureForIndex(i);
        char sheetId[96], paletteId[96];
        bool includePalette = (opts->omitPaletteIndex != i);

        BuildFamilyId(sheetId, sizeof(sheetId), fix->canonical, false);
        BuildFamilyId(paletteId, sizeof(paletteId), fix->canonical, true);

        Gen3ResourceProvider_Add(provider, sheetId, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                 1u, fix->sheet, EMERALD_TRAINER_SHEET_SIZE, false, NULL);
        Gen3ResourceCatalog_Add(catalog, sheetId, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                1u, true, NULL);
        if (includePalette)
        {
            const u8 *palette = fix->palette;
            u32 paletteSize = EMERALD_TRAINER_PALETTE_SIZE;
            if (opts->wrongSizePaletteIndex == i && opts->wrongSizePalette != NULL)
            {
                palette = opts->wrongSizePalette;
                paletteSize = opts->wrongSizePaletteSize;
            }
            Gen3ResourceProvider_Add(provider, paletteId, GEN3_RESOURCE_TYPE_PALETTE,
                                     1u, palette, paletteSize, false, NULL);
            Gen3ResourceCatalog_Add(catalog, paletteId, GEN3_RESOURCE_TYPE_PALETTE,
                                    1u, true, NULL);
        }
    }
    /* R8 back family: 8 raw sheets (payload verbatim) + 2 back-only palettes
     * (red/leaf, decoded 32-byte payloads; the seam LZ-encodes them). The 6
     * alias back-palette slots add no entries - they reuse the front palette
     * resources above. */
    for (i = 0; i < (int)EMERALD_TRAINER_BACK_SHEET_COUNT; i++)
    {
        const struct FamilyBackFixture *fix = &sBackFixtures[i];
        char sheetId[96], paletteId[96];
        bool includeSheet = (opts->omitBackSheetIndex != fix->index);

        BuildBackFamilyId(sheetId, sizeof(sheetId), fix->canonical, false);
        if (includeSheet)
        {
            Gen3ResourceProvider_Add(provider, sheetId, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                     1u, fix->sheet, fix->sheetSize, false, NULL);
            Gen3ResourceCatalog_Add(catalog, sheetId, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                    1u, true, NULL);
        }
        if (fix->palette != NULL && opts->omitBackPaletteIndex != fix->index)
        {
            BuildBackFamilyId(paletteId, sizeof(paletteId), fix->canonical, true);
            Gen3ResourceProvider_Add(provider, paletteId, GEN3_RESOURCE_TYPE_PALETTE,
                                     1u, fix->palette, EMERALD_TRAINER_PALETTE_SIZE,
                                     false, NULL);
            Gen3ResourceCatalog_Add(catalog, paletteId, GEN3_RESOURCE_TYPE_PALETTE,
                                    1u, true, NULL);
        }
    }
    Gen3ResourceProvider_Finalize(provider, NULL);
    Gen3ResourceCatalog_Finalize(catalog, NULL);

    candidate = Gen3ResourceCandidate_Create(catalog, &diag);
    assert(candidate != NULL);
    Gen3ResourceCandidate_AddProvider(candidate, provider, &diag);
    if (!Gen3ResourceCandidate_Build(candidate, &snapshot, &diag))
        snapshot = NULL;
    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourceProvider_Destroy(provider);
    Gen3ResourceDiagnostics_Destroy(&diag);
    return snapshot;
}

static struct Gen3ResourceSnapshot *BuildFullFamilySnapshot(void)
{
    struct FamilySnapshotOpts opts;
    BuildFamilySnapshotOpts(&opts);
    return BuildFamilySnapshot(&opts);
}

static void DestroySnapshot(struct Gen3ResourceSnapshot *snapshot)
{
    Gen3ResourceSnapshot_Destroy(snapshot);
}

/* ------------------------------------------------------------------ */
/* Table state capture / comparison                                    */
/* ------------------------------------------------------------------ */

static void CaptureSheetTable(struct CompressedSpriteSheet *out)
{
    memcpy(out, gTrainerFrontPicTable, sizeof(gTrainerFrontPicTable));
}

static void CapturePaletteTable(struct CompressedSpritePalette *out)
{
    memcpy(out, gTrainerFrontPicPaletteTable, sizeof(gTrainerFrontPicPaletteTable));
}

static void CaptureBackPaletteTable(struct CompressedSpritePalette *out)
{
    memcpy(out, gTrainerBackPicPaletteTable, sizeof(gTrainerBackPicPaletteTable));
}

static void CaptureBackSheetTable(struct CompressedSpriteSheet *out)
{
    memcpy(out, gTrainerBackPicTable, sizeof(gTrainerBackPicTable));
}

/* The 34 back SpriteFrameImage slots (5 = max frame count, enforced by the
 * back fixture loader). */
struct BackFrameCapture
{
    struct SpriteFrameImage frames[EMERALD_TRAINER_BACK_SHEET_COUNT][5];
};

static void CaptureBackFrames(struct BackFrameCapture *out)
{
    size_t i;
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        size_t f;
        for (f = 0; f < sBackFixtures[i].frames; f++)
            out->frames[i][f] = kBackFrameArrays[i][f];
    }
}

static int BackFramesMatch(const struct BackFrameCapture *before)
{
    size_t i;
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        size_t f;
        for (f = 0; f < sBackFixtures[i].frames; f++)
        {
            if (kBackFrameArrays[i][f].data != before->frames[i][f].data
             || kBackFrameArrays[i][f].size != before->frames[i][f].size)
                return 0;
        }
    }
    return 1;
}

/* Byte-compare the five live tables against the captured before-state. */
static int TablesMatch(const struct CompressedSpriteSheet *beforeSheet,
                       const struct CompressedSpritePalette *beforePal,
                       const struct CompressedSpritePalette *beforeBackPal,
                       const struct CompressedSpriteSheet *beforeBackSheet,
                       const struct BackFrameCapture *beforeBackFrames)
{
    return memcmp(gTrainerFrontPicTable, beforeSheet, sizeof(gTrainerFrontPicTable)) == 0
        && memcmp(gTrainerFrontPicPaletteTable, beforePal,
                  sizeof(gTrainerFrontPicPaletteTable)) == 0
        && memcmp(gTrainerBackPicPaletteTable, beforeBackPal,
                  sizeof(gTrainerBackPicPaletteTable)) == 0
        && memcmp(gTrainerBackPicTable, beforeBackSheet, sizeof(gTrainerBackPicTable)) == 0
        && BackFramesMatch(beforeBackFrames);
}

/* R8: every one of the 8 back-pic palette slots is migrated - the 6 shared
 * slots by the front normal-palette aliases (descriptor back_palette_index),
 * the 2 Red/Leaf slots by the back-only palettes (back descriptor
 * has_palette). The loader's union check proves this classification covers
 * each slot exactly once, so this function is the live expression of "all
 * eight ROM_BASE-only", not a hardcoded list. */
static int IsMigratedBackPaletteSlot(size_t slot)
{
    size_t i;
    if (slot >= ARRAY_COUNT(gTrainerBackPicPaletteTable))
        return 0;
    for (i = 0; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        if (FixtureForIndex((int)i)->backPaletteIndex == (int)slot)
            return 1;
    }
    for (i = 0; i < sBackFixtureCount; i++)
    {
        if (sBackFixtures[i].palette != NULL && sBackFixtures[i].index == (int)slot)
            return 1;
    }
    return 0;
}

/* Assert the five tables are unchanged except the migrated data slots - the
 * ONLY permitted mutation (R7B §4/§7, R8): every front sheet [i].data, every
 * front palette [i].data, the 8 back-pic palette slots' .data, the 8 back
 * SHEET table slots' .data, and the 34 back SpriteFrameImage slots' .data.
 * size/tag of the migrated entries must also match. */
static void AssertOnlyMigratedChanged(
    const struct CompressedSpriteSheet *beforeSheet,
    const struct CompressedSpritePalette *beforePal,
    const struct CompressedSpritePalette *beforeBackPal,
    const struct CompressedSpriteSheet *beforeBackSheet,
    const struct BackFrameCapture *beforeBackFrames)
{
    size_t i;
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
    {
        CHECK("sheet data changed", gTrainerFrontPicTable[i].data != beforeSheet[i].data);
        CHECK("sheet size unchanged", gTrainerFrontPicTable[i].size == beforeSheet[i].size);
        CHECK("sheet tag unchanged", gTrainerFrontPicTable[i].tag == beforeSheet[i].tag);
    }
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicPaletteTable); i++)
    {
        CHECK("pal data changed", gTrainerFrontPicPaletteTable[i].data != beforePal[i].data);
        CHECK("pal tag unchanged", gTrainerFrontPicPaletteTable[i].tag == beforePal[i].tag);
    }
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicPaletteTable); i++)
    {
        CHECK("back-pal data changed",
              gTrainerBackPicPaletteTable[i].data != beforeBackPal[i].data);
        CHECK("back-pal tag unchanged",
              gTrainerBackPicPaletteTable[i].tag == beforeBackPal[i].tag);
    }
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        const struct FamilyBackFixture *fix = &sBackFixtures[i];
        size_t f;
        CHECK("back sheet data changed",
              gTrainerBackPicTable[i].data != beforeBackSheet[i].data);
        CHECK("back sheet size unchanged",
              gTrainerBackPicTable[i].size == beforeBackSheet[i].size);
        CHECK("back sheet tag unchanged",
              gTrainerBackPicTable[i].tag == beforeBackSheet[i].tag);
        for (f = 0; f < fix->frames; f++)
        {
            CHECK("back frame data changed",
                  kBackFrameArrays[i][f].data != beforeBackFrames->frames[i][f].data);
            CHECK("back frame size unchanged",
                  kBackFrameArrays[i][f].size == beforeBackFrames->frames[i][f].size);
        }
    }
}

/* Assert only the migrated data slots hold a specific pointer; every other
 * entry of the five tables is unchanged (used for the republish repair). */
static void AssertOnlyMigratedPointsTo(
    const struct CompressedSpriteSheet *beforeSheet,
    const struct CompressedSpritePalette *beforePal,
    const struct CompressedSpritePalette *beforeBackPal,
    const struct CompressedSpriteSheet *beforeBackSheet,
    const struct BackFrameCapture *beforeBackFrames)
{
    size_t i;
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
        CHECK("sheet points to repaired stream",
              gTrainerFrontPicTable[i].data == beforeSheet[i].data);
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicPaletteTable); i++)
        CHECK("pal points to repaired stream",
              gTrainerFrontPicPaletteTable[i].data == beforePal[i].data);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicPaletteTable); i++)
        CHECK("back-pal points to repaired stream",
              gTrainerBackPicPaletteTable[i].data == beforeBackPal[i].data);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        const struct FamilyBackFixture *fix = &sBackFixtures[i];
        size_t f;
        CHECK("back sheet points to repaired stream",
              gTrainerBackPicTable[i].data == beforeBackSheet[i].data);
        for (f = 0; f < fix->frames; f++)
            CHECK("back frame points to repaired stream",
                  kBackFrameArrays[i][f].data == beforeBackFrames->frames[i][f].data);
    }
}

/* ------------------------------------------------------------------ */
/* 1. Save-state audit (R5 §17, family-wide)                           */
/* ------------------------------------------------------------------ */

static void TestSaveStateAudit(void)
{
    struct Gen3ResourceSnapshot *snapshot;
    u8 decodedSheet[EMERALD_TRAINER_SHEET_SIZE];
    u8 decodedPalette[EMERALD_TRAINER_PALETTE_SIZE];
    size_t i;

    /* Fixtures MUST be loaded before the baseline loop: the slot
     * classification (IsMigratedBackPaletteSlot) reads the descriptors'
     * back_palette_index / has_palette mappings, and a zeroed fixture table
     * would mislabel slots. Loading is idempotent and touches nothing in the
     * tables. */
    if (!LoadFamilyFixtures() || !LoadBackFamilyFixtures())
        return;

    /* Pre-publish baseline (runs before any publish): on native every migrated
     * slot starts at the NULL sentinel - the legacy leaf symbols are not
     * linked and no consumer may read them until the compat seam publishes
     * (§14/A). This TU does not even reference gTrainerFrontPic_* /
     * gTrainerPalette_* / gTrainerBackPic_* / gTrainerBackPicPalette_* (the
     * native table branches expand to NULL), which is itself the compile-time
     * proof that the native runtime path has zero legacy dependency. */
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

    snapshot = BuildFullFamilySnapshot();
    CHECK("save-state audit snapshot builds", snapshot != NULL);
    if (snapshot == NULL)
        return;
    /* R9 §8: this audit snapshot is trainer-only, so the strict init would
     * hard-fail - the additive opt-in keeps the offline degraded contract. */
    CHECK("save-state audit init ok",
          EmeraldResourceCompat_InitializeFromSnapshotAllowPokemonDegradation(snapshot, NULL) == EMERALD_COMPAT_OK);

    /* Post-publish: all migrated slots hold process-local streams in the
     * session image (build-constant INCBIN addresses can no longer occur). */
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
        CHECK("sheet pointer published", gTrainerFrontPicTable[i].data != NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicPaletteTable); i++)
        CHECK("palette pointer published", gTrainerFrontPicPaletteTable[i].data != NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicPaletteTable); i++)
        CHECK("back-pal pointer published", gTrainerBackPicPaletteTable[i].data != NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        const struct FamilyBackFixture *fix = &sBackFixtures[i];
        size_t f;
        CHECK("back sheet pointer published", gTrainerBackPicTable[i].data != NULL);
        /* The 34 frame slots point INTO the sheet stream at per-frame offsets:
         * the sprite pipeline's read path (images[frame].data -> CpuCopy16). */
        for (f = 0; f < fix->frames; f++)
        {
            CHECK("back frame pointer published", kBackFrameArrays[i][f].data != NULL);
            CHECK("back frame == sheet stream + frame offset",
                  (const u8 *)(const void *)kBackFrameArrays[i][f].data
                      == (const u8 *)(const void *)gTrainerBackPicTable[i].data
                             + TRAINER_PIC_SIZE * f);
        }
    }
    /* R9 §5/§7 additive degradation: this snapshot carries the trainer family
     * only, so the Pokémon battle family is NOT published - every Pokémon
     * slot stays at the NULL sentinel (the compiled payloads are gone from
     * the native link since R9 §7) while the trainer contract above stands. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("mon front slot untouched (additive degradation)",
              gMonFrontPicTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("mon back slot untouched (additive degradation)",
              gMonBackPicTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("mon palette slot untouched (additive degradation)",
              gMonPaletteTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("mon shiny slot untouched (additive degradation)",
              gMonShinyPaletteTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
        CHECK("sheet stream != palette stream",
              gTrainerFrontPicTable[i].data != gTrainerFrontPicPaletteTable[i].data);
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
    {
        const struct FamilyTrainerFixture *fix = FixtureForIndex((int)i);
        if (fix->backPaletteIndex >= 0)
            CHECK("back palette reuses front palette stream",
                  gTrainerBackPicPaletteTable[fix->backPaletteIndex].data
                      == gTrainerFrontPicPaletteTable[i].data);
    }
    /* The R8 back-only palettes must NOT alias anything: red/leaf are distinct
     * image entries with distinct streams. */
    CHECK("red/leaf back palettes distinct",
          gTrainerBackPicPaletteTable[TRAINER_BACK_PIC_RED].data
              != gTrainerBackPicPaletteTable[TRAINER_BACK_PIC_LEAF].data);

    /* The streams live in the session image, not the transient snapshot: even
     * after the source snapshot is destroyed the published streams survive and
     * decode to the canonical payloads (§5). Back sheets are RAW entries, so
     * the stream must equal the fixture bytes verbatim. */
    DestroySnapshot(snapshot);
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
    {
        const struct FamilyTrainerFixture *fix = FixtureForIndex((int)i);
        LZ77UnCompWram((const u32 *)(const void *)gTrainerFrontPicTable[i].data,
                       decodedSheet);
        CHECK("sheet stream survives snapshot destruction",
              memcmp(decodedSheet, fix->sheet, EMERALD_TRAINER_SHEET_SIZE) == 0);
    }
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicPaletteTable); i++)
    {
        const struct FamilyTrainerFixture *fix = NULL;
        int owner;
        for (owner = 0; owner < EMERALD_TRAINER_FRONT_COUNT; owner++)
        {
            if (FixtureForIndex(owner)->backPaletteIndex == (int)i)
            {
                fix = FixtureForIndex(owner);
                break;
            }
        }
        if (fix == NULL)
            continue; /* Red/Leaf back-only slots are covered below */
        LZ77UnCompWram((const u32 *)(const void *)gTrainerBackPicPaletteTable[i].data,
                       decodedPalette);
        CHECK("shared back-pal stream survives snapshot destruction",
              memcmp(decodedPalette, fix->palette, EMERALD_TRAINER_PALETTE_SIZE) == 0);
    }
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        const struct FamilyBackFixture *fix = &sBackFixtures[i];
        size_t f;
        CHECK("back sheet stream survives snapshot destruction (raw)",
              memcmp(gTrainerBackPicTable[i].data, fix->sheet, fix->sheetSize) == 0);
        for (f = 0; f < fix->frames; f++)
        {
            CHECK("back frame survives snapshot destruction (raw)",
                  memcmp(kBackFrameArrays[i][f].data,
                         fix->sheet + TRAINER_PIC_SIZE * f, TRAINER_PIC_SIZE) == 0);
        }
        if (fix->palette != NULL)
        {
            LZ77UnCompWram((const u32 *)(const void *)
                               gTrainerBackPicPaletteTable[fix->index].data,
                           decodedPalette);
            CHECK("red/leaf back-pal stream survives snapshot destruction",
                  memcmp(decodedPalette, fix->palette,
                         EMERALD_TRAINER_PALETTE_SIZE) == 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 2. Family mapping + parity through the real load path (R7B §6)      */
/* ------------------------------------------------------------------ */

static void TestFamilyMappingAndParity(void)
{
    struct Gen3ResourceSnapshot *snapshot;
    struct EmeraldResourceCompatDiagnostics diag;
    struct CompressedSpriteSheet beforeSheet[ARRAY_COUNT(gTrainerFrontPicTable)];
    struct CompressedSpritePalette beforePal[ARRAY_COUNT(gTrainerFrontPicPaletteTable)];
    struct CompressedSpritePalette beforeBackPal[ARRAY_COUNT(gTrainerBackPicPaletteTable)];
    struct CompressedSpriteSheet beforeBackSheet[ARRAY_COUNT(gTrainerBackPicTable)];
    struct BackFrameCapture beforeBackFrames;
    u8 decodedSheet[EMERALD_TRAINER_SHEET_SIZE];
    u8 decodedPalette[EMERALD_TRAINER_PALETTE_SIZE];
    enum EmeraldResourceCompatStatus status;
    size_t i;

    CHECK("family fixtures load", LoadFamilyFixtures());
    if (gFailures != 0)
        return;
    CHECK("back fixtures load", LoadBackFamilyFixtures());
    if (gFailures != 0)
        return;
    snapshot = BuildFullFamilySnapshot();
    CHECK("family snapshot builds", snapshot != NULL);
    if (snapshot == NULL)
        return;

    /* R7B §11: verify type/schema/size/winner through the NORMAL snapshot for
     * the first trainer (the seam enforces the same checks internally for all
     * 186, fail closed, exercised by the fail-closed tests). */
    {
        Gen3ResourceHandle handle;
        struct Gen3ResourceView view;
        const struct FamilyTrainerFixture *fix = FixtureForIndex(0);
        char sheetId[96];
        BuildFamilyId(sheetId, sizeof(sheetId), fix->canonical, false);
        CHECK("find sheet handle",
              Gen3ResourceSnapshot_FindHandle(snapshot, sheetId, &handle)
                  == GEN3_RESOURCE_OK);
        CHECK("sheet resolves",
              Gen3ResourceSnapshot_Resolve(snapshot, handle,
                  GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1u, &view) == GEN3_RESOURCE_OK);
        if (view.type == GEN3_RESOURCE_TYPE_TILE_GRAPHICS)
        {
            CHECK("sheet winner id is rom base",
                  view.winningProviderId != NULL
                      && strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) == 0);
            CHECK("sheet winner precedence",
                  view.winningProviderPrecedence == EMERALD_ROM_BASE_PRECEDENCE);
            CHECK("sheet canonical size",
                  view.payloadSize == EMERALD_TRAINER_SHEET_SIZE);
        }
    }

    CaptureSheetTable(beforeSheet);
    CapturePaletteTable(beforePal);
    CaptureBackPaletteTable(beforeBackPal);
    CaptureBackSheetTable(beforeBackSheet);
    CaptureBackFrames(&beforeBackFrames);

    /* R9 §8: this fixture snapshot is trainer-only, so the strict init would
     * hard-fail - the additive opt-in keeps the offline degraded contract
     * pinned here (trainer published, Pokémon at sentinels, diagnostics
     * cleared). The strict game path is pinned by TestPokemonStrictHardFail. */
    status = EmeraldResourceCompat_InitializeFromSnapshotAllowPokemonDegradation(snapshot, &diag);
    CHECK("initialize ok", status == EMERALD_COMPAT_OK);

    /* On SUCCESS the compat diagnostics are cleared. */
    CHECK("success leaves diagnostics empty", diag.stage[0] == '\0');

    AssertOnlyMigratedChanged(beforeSheet, beforePal, beforeBackPal,
                              beforeBackSheet, &beforeBackFrames);

    /* R8 §4: every back-pic palette slot is migrated (6 front-palette aliases
     * + 2 back-only palettes - the loader's coverage check pins the source
     * counts; this pins the classification the seam acts on). */
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicPaletteTable); i++)
        CHECK("every back-pal slot migrated (R8)", IsMigratedBackPaletteSlot(i));

    /* R7B §6: every published stream decodes to the canonical artifact of the
     * trainer the descriptor places at that table index - the seam's internal
     * mapping cannot drift from the descriptor. All 93 sheets + 93 palettes. */
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
    {
        const struct FamilyTrainerFixture *fix = FixtureForIndex((int)i);
        char label[96];

        LZ77UnCompWram((const u32 *)(const void *)gTrainerFrontPicTable[i].data,
                       decodedSheet);
        snprintf(label, sizeof(label), "sheet %d decodes to canonical (%s)",
                 (int)i, fix->canonical);
        CHECK(label, memcmp(decodedSheet, fix->sheet,
                            EMERALD_TRAINER_SHEET_SIZE) == 0);
        CHECK("sheet size metadata",
              gTrainerFrontPicTable[i].size == TRAINER_PIC_SIZE * fix->sizeMult);
        CHECK("sheet tag metadata", gTrainerFrontPicTable[i].tag == i);

        LZ77UnCompWram((const u32 *)(const void *)gTrainerFrontPicPaletteTable[i].data,
                       decodedPalette);
        snprintf(label, sizeof(label), "palette %d decodes to canonical (%s)",
                 (int)i, fix->canonical);
        CHECK(label, memcmp(decodedPalette, fix->palette,
                            EMERALD_TRAINER_PALETTE_SIZE) == 0);
        CHECK("palette tag metadata", gTrainerFrontPicPaletteTable[i].tag == i);
    }

    /* R7B §6: shared back-pic palette consumers point to the same stream as
     * the front palette of their owner trainer, and it decodes to the owner's
     * canonical palette. */
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
    {
        const struct FamilyTrainerFixture *fix = FixtureForIndex((int)i);
        char label[96];
        if (fix->backPaletteIndex < 0)
            continue;
        CHECK("shared back stream == front palette stream",
              gTrainerBackPicPaletteTable[fix->backPaletteIndex].data
                  == gTrainerFrontPicPaletteTable[i].data);
        LZ77UnCompWram((const u32 *)(const void *)
                           gTrainerBackPicPaletteTable[fix->backPaletteIndex].data,
                       decodedPalette);
        snprintf(label, sizeof(label), "shared back %d decodes to owner palette",
                 fix->backPaletteIndex);
        CHECK(label, memcmp(decodedPalette, fix->palette,
                            EMERALD_TRAINER_PALETTE_SIZE) == 0);
    }

    /* R8: the back family - 8 raw sheets + 2 back-only palettes. The published
     * sheet-table streams ARE the raw canonical bytes (RAW entries, no LZ
     * layer), the 34 frame slots point into the same stream at per-frame
     * offsets (the sprite pipeline's read path), and the red/leaf palette
     * slots decode to their canonical palettes. */
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        const struct FamilyBackFixture *fix = &sBackFixtures[i];
        char label[96];
        size_t f;

        snprintf(label, sizeof(label), "back sheet stream == raw canonical (%s)",
                 fix->canonical);
        CHECK(label, memcmp(gTrainerBackPicTable[i].data, fix->sheet,
                            fix->sheetSize) == 0);
        CHECK("back sheet size metadata",
              gTrainerBackPicTable[i].size == fix->sheetSize);
        CHECK("back sheet tag metadata", gTrainerBackPicTable[i].tag == i);
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

    /* §25 actual native load path for the full family: the real game loaders
     * must produce the canonical bytes from every published stream. */
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
    {
        const struct FamilyTrainerFixture *fix = FixtureForIndex((int)i);
        DecompressPicFromTable_2(&gTrainerFrontPicTable[i], decodedSheet, SPECIES_NONE);
        CHECK("load path sheet == canonical",
              memcmp(decodedSheet, fix->sheet, EMERALD_TRAINER_SHEET_SIZE) == 0);
        LoadCompressedSpritePalette(&gTrainerFrontPicPaletteTable[i]);
        CHECK("load path palette == canonical",
              memcmp(sLastLoadedPalette, fix->palette,
                     EMERALD_TRAINER_PALETTE_SIZE) == 0);
    }

    DestroySnapshot(snapshot);
}

/* ------------------------------------------------------------------ */
/* 3. Runtime hook (§14): SetSnapshot/TryInitialize idempotent, gated  */
/* ------------------------------------------------------------------ */

static void TestRuntimeHook(void)
{
    struct Gen3ResourceSnapshot *snapshot;
    struct CompressedSpriteSheet beforeSheet[ARRAY_COUNT(gTrainerFrontPicTable)];
    struct CompressedSpritePalette beforePal[ARRAY_COUNT(gTrainerFrontPicPaletteTable)];
    struct CompressedSpritePalette beforeBackPal[ARRAY_COUNT(gTrainerBackPicPaletteTable)];
    struct CompressedSpriteSheet beforeBackSheet[ARRAY_COUNT(gTrainerBackPicTable)];
    struct BackFrameCapture beforeBackFrames;

    if (!LoadFamilyFixtures() || !LoadBackFamilyFixtures())
        return;
    snapshot = BuildFullFamilySnapshot();
    CHECK("hook snapshot builds", snapshot != NULL);
    if (snapshot == NULL)
        return;

    /* No snapshot registered: TryInitialize is a no-op. */
    CaptureSheetTable(beforeSheet);
    CapturePaletteTable(beforePal);
    CaptureBackPaletteTable(beforeBackPal);
    CaptureBackSheetTable(beforeBackSheet);
    CaptureBackFrames(&beforeBackFrames);
    EmeraldResourceCompat_TryInitialize();
    CHECK("hook no-op without snapshot",
          TablesMatch(beforeSheet, beforePal, beforeBackPal,
                      beforeBackSheet, &beforeBackFrames));

    /* Registered: TryInitialize publishes exactly once. */
    EmeraldResourceCompat_SetSnapshot(snapshot);
    EmeraldResourceCompat_TryInitialize();
    CHECK("hook published sheet", gTrainerFrontPicTable[0].data != beforeSheet[0].data);
    CHECK("hook published back sheet", gTrainerBackPicTable[0].data != beforeBackSheet[0].data);
    CHECK("hook published back frame",
          kBackFrameArrays[0][0].data != beforeBackFrames.frames[0][0].data);
    CHECK("hook published back palette",
          gTrainerBackPicPaletteTable[FixtureForIndex(0)->backPaletteIndex >= 0
                                          ? FixtureForIndex(0)->backPaletteIndex
                                          : 0].data != beforeBackPal[0].data);
    CaptureSheetTable(beforeSheet);
    CapturePaletteTable(beforePal);
    CaptureBackPaletteTable(beforeBackPal);
    CaptureBackSheetTable(beforeBackSheet);
    CaptureBackFrames(&beforeBackFrames);

    /* Idempotent: a second TryInitialize must not republish. */
    EmeraldResourceCompat_TryInitialize();
    CHECK("hook idempotent",
          TablesMatch(beforeSheet, beforePal, beforeBackPal,
                      beforeBackSheet, &beforeBackFrames));

    /* Clearing the snapshot gates it again. */
    EmeraldResourceCompat_ClearSnapshot();
    EmeraldResourceCompat_TryInitialize();
    CHECK("hook cleared no-op",
          TablesMatch(beforeSheet, beforePal, beforeBackPal,
                      beforeBackSheet, &beforeBackFrames));

    DestroySnapshot(snapshot);
}

/* ------------------------------------------------------------------ */
/* 4. Fail-closed + transactional (§11/§12/§13/§24, family-wide)       */
/* ------------------------------------------------------------------ */

static void TestFailClosedTransactional(void)
{
    struct Gen3ResourceSnapshot *snapshot;
    struct EmeraldResourceCompatDiagnostics cdiag;
    struct CompressedSpriteSheet beforeSheet[ARRAY_COUNT(gTrainerFrontPicTable)];
    struct CompressedSpritePalette beforePal[ARRAY_COUNT(gTrainerFrontPicPaletteTable)];
    struct CompressedSpritePalette beforeBackPal[ARRAY_COUNT(gTrainerBackPicPaletteTable)];
    struct CompressedSpriteSheet beforeBackSheet[ARRAY_COUNT(gTrainerBackPicTable)];
    struct BackFrameCapture beforeBackFrames;
    struct FamilySnapshotOpts opts;
    /* A palette payload the RESOLVER accepts (even, <= 512) but the compat seam
     * rejects: the palette canonical size is exactly 32. A 31-byte palette
     * would be refused at snapshot build time by the resolver itself, which
     * would test the resolver instead of the compat seam. */
    u8 wrongSizePalette[64];
    const struct FamilyTrainerFixture *omitted;
    char omittedPaletteId[96];
    enum EmeraldResourceCompatStatus status;

    if (!LoadFamilyFixtures() || !LoadBackFamilyFixtures())
        return;

    /* Prior valid state: the tables currently hold the streams published by the
     * earlier tests. Every failure below must leave that state byte-identical. */
    CaptureSheetTable(beforeSheet);
    CapturePaletteTable(beforePal);
    CaptureBackPaletteTable(beforeBackPal);
    CaptureBackSheetTable(beforeBackSheet);
    CaptureBackFrames(&beforeBackFrames);

    /* 4a. Missing resource: the snapshot BUILDS but one trainer's palette
     * contract is not provided -> resolve returns NOT_FOUND for that trainer ->
     * compat fails closed with that trainer's palette id in the diagnostics. */
    omitted = FixtureForIndex(17);
    BuildFamilyId(omittedPaletteId, sizeof(omittedPaletteId), omitted->canonical, true);
    BuildFamilySnapshotOpts(&opts);
    opts.omitPaletteIndex = 17;
    snapshot = BuildFamilySnapshot(&opts);
    CHECK("omitted-palette snapshot builds", snapshot != NULL);
    if (snapshot == NULL)
        return;
    status = EmeraldResourceCompat_InitializeFromSnapshot(snapshot, &cdiag);
    CHECK("missing palette fails", status == EMERALD_COMPAT_ERR_RESOLVE_FAILED);
    CHECK("diag missing palette name", strcmp(cdiag.canonicalName, omittedPaletteId) == 0);
    CHECK("tables untouched after missing palette",
          TablesMatch(beforeSheet, beforePal, beforeBackPal,
                      beforeBackSheet, &beforeBackFrames));
    DestroySnapshot(snapshot);

    /* 4b. Wrong size: trainer 33's palette canonical is 64 bytes, not 32. */
    memset(wrongSizePalette, 0x5A, sizeof(wrongSizePalette));
    BuildFamilySnapshotOpts(&opts);
    opts.wrongSizePalette = wrongSizePalette;
    opts.wrongSizePaletteIndex = 33;
    opts.wrongSizePaletteSize = sizeof(wrongSizePalette);
    snapshot = BuildFamilySnapshot(&opts);
    CHECK("wrong-size snapshot builds", snapshot != NULL);
    if (snapshot == NULL)
        return;
    status = EmeraldResourceCompat_InitializeFromSnapshot(snapshot, &cdiag);
    CHECK("wrong palette size fails", status == EMERALD_COMPAT_ERR_RESOLVE_FAILED);
    {
        const struct FamilyTrainerFixture *fix = FixtureForIndex(33);
        char id[96];
        BuildFamilyId(id, sizeof(id), fix->canonical, true);
        CHECK("diag palette mismatch name", strcmp(cdiag.canonicalName, id) == 0);
    }
    CHECK("diag expected palette size", cdiag.expectedSize == EMERALD_TRAINER_PALETTE_SIZE);
    CHECK("diag actual palette size", cdiag.actualSize == 64u);
    CHECK("tables untouched after wrong size",
          TablesMatch(beforeSheet, beforePal, beforeBackPal,
                      beforeBackSheet, &beforeBackFrames));
    DestroySnapshot(snapshot);

    /* 4c. Wrong winner: a higher-precedence legacy provider shadows ROM_BASE
     * for the whole family, so the resolved winner id is not ROM_BASE. */
    BuildFamilySnapshotOpts(&opts);
    opts.providerId = "legacy.compiled";
    opts.kind = GEN3_PROVIDER_LEGACY_COMPILED;
    opts.precedence = EMERALD_ROM_BASE_PRECEDENCE + 1u;
    snapshot = BuildFamilySnapshot(&opts);
    CHECK("wrong-winner snapshot builds", snapshot != NULL);
    if (snapshot == NULL)
        return;
    status = EmeraldResourceCompat_InitializeFromSnapshot(snapshot, &cdiag);
    CHECK("non-rom-base winner fails", status == EMERALD_COMPAT_ERR_RESOLVE_FAILED);
    CHECK("diag wrong winner",
          strcmp(cdiag.winningProviderId, "legacy.compiled") == 0);
    CHECK("tables untouched after wrong winner",
          TablesMatch(beforeSheet, beforePal, beforeBackPal,
                      beforeBackSheet, &beforeBackFrames));
    DestroySnapshot(snapshot);

    /* 4d. Direct publish with a NULL image: refused, tables untouched. */
    status = EmeraldResourceCompat_PublishTrainerTables(NULL, &cdiag);
    CHECK("publish null image fails", status == EMERALD_COMPAT_ERR_INVALID_ARGUMENT);
    CHECK("tables untouched after null publish",
          TablesMatch(beforeSheet, beforePal, beforeBackPal,
                      beforeBackSheet, &beforeBackFrames));

    /* 4e. Mismatched image: build a VALID image whose entry names do not match
     * the seam's family mapping (slot 0 named for trainer 1). The seam must
     * refuse to publish it. */
    {
        struct EmeraldResourceCompatSourceEntry entries[2];
        struct EmeraldResourceCompatibilityImage *image = NULL;
        const struct FamilyTrainerFixture *fix0 = FixtureForIndex(0);
        const struct FamilyTrainerFixture *fix1 = FixtureForIndex(1);
        char wrongSheetId[96];
        char rightPaletteId[96];

        /* Zeroed: the R8 per-entry fields (expectedSize, encoding) must take
         * their LZ/derived defaults rather than stack garbage. */
        memset(entries, 0, sizeof(entries));
        BuildFamilyId(wrongSheetId, sizeof(wrongSheetId), fix1->canonical, false);
        BuildFamilyId(rightPaletteId, sizeof(rightPaletteId), fix0->canonical, true);
        entries[0].canonicalName = wrongSheetId; /* trainer 1's sheet at slot 0 */
        entries[0].type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
        entries[0].schema = 1u;
        entries[0].payload = fix1->sheet;
        entries[0].payloadSize = EMERALD_TRAINER_SHEET_SIZE;
        entries[1].canonicalName = rightPaletteId;
        entries[1].type = GEN3_RESOURCE_TYPE_PALETTE;
        entries[1].schema = 1u;
        entries[1].payload = fix0->palette;
        entries[1].payloadSize = EMERALD_TRAINER_PALETTE_SIZE;
        status = EmeraldResourceCompatImage_CreateFamily(entries, 2u, &image, NULL);
        CHECK("mismatched image builds", status == EMERALD_COMPAT_OK && image != NULL);
        if (image != NULL)
        {
            status = EmeraldResourceCompat_PublishTrainerTables(image, &cdiag);
            CHECK("mismatched image publish refused",
                  status == EMERALD_COMPAT_ERR_PUBLISH_FAILED);
            CHECK("tables untouched after mismatched publish",
                  TablesMatch(beforeSheet, beforePal, beforeBackPal,
                              beforeBackSheet, &beforeBackFrames));
            EmeraldResourceCompatImage_Destroy(image);
        }
    }

    /* 4f. R8: missing BACK resource: red's back sheet contract is not provided
     * -> resolve fails with the back sheet id in the diagnostics, tables
     * untouched - the back family is part of the same transaction. */
    BuildFamilySnapshotOpts(&opts);
    opts.omitBackSheetIndex = TRAINER_BACK_PIC_RED;
    snapshot = BuildFamilySnapshot(&opts);
    CHECK("omitted-back-sheet snapshot builds", snapshot != NULL);
    if (snapshot == NULL)
        return;
    status = EmeraldResourceCompat_InitializeFromSnapshot(snapshot, &cdiag);
    CHECK("missing back sheet fails", status == EMERALD_COMPAT_ERR_RESOLVE_FAILED);
    {
        const struct FamilyBackFixture *fix = &sBackFixtures[TRAINER_BACK_PIC_RED];
        char id[96];
        BuildBackFamilyId(id, sizeof(id), fix->canonical, false);
        CHECK("diag missing back sheet name", strcmp(cdiag.canonicalName, id) == 0);
    }
    CHECK("tables untouched after missing back sheet",
          TablesMatch(beforeSheet, beforePal, beforeBackPal,
                      beforeBackSheet, &beforeBackFrames));
    DestroySnapshot(snapshot);

    /* 4f2. R8: missing BACK-ONLY palette: red's back palette contract is not
     * provided -> resolve fails with the back palette id in the diagnostics,
     * tables untouched (the red/leaf palette slots are part of the same
     * transaction as the sheets and the front family). */
    BuildFamilySnapshotOpts(&opts);
    opts.omitBackPaletteIndex = TRAINER_BACK_PIC_RED;
    snapshot = BuildFamilySnapshot(&opts);
    CHECK("omitted-back-palette snapshot builds", snapshot != NULL);
    if (snapshot == NULL)
        return;
    status = EmeraldResourceCompat_InitializeFromSnapshot(snapshot, &cdiag);
    CHECK("missing back palette fails", status == EMERALD_COMPAT_ERR_RESOLVE_FAILED);
    {
        const struct FamilyBackFixture *fix = &sBackFixtures[TRAINER_BACK_PIC_RED];
        char id[96];
        BuildBackFamilyId(id, sizeof(id), fix->canonical, true);
        CHECK("diag missing back palette name", strcmp(cdiag.canonicalName, id) == 0);
    }
    CHECK("tables untouched after missing back palette",
          TablesMatch(beforeSheet, beforePal, beforeBackPal,
                      beforeBackSheet, &beforeBackFrames));
    DestroySnapshot(snapshot);
}

/* ------------------------------------------------------------------ */
/* 4b. R7B republish after state load (§2/§3/§15/§18, family-wide)     */
/*                                                                     */
/* A native save-state GAME_DATA slice restores the three trainer       */
/* tables verbatim, which may hold stale or foreign process-local       */
/* compat pointers (R5 §17). The runtime re-publishes the current-      */
/* session image into the migrated slots right after a load; this test  */
/* simulates the corrupt state and proves the repair is exact,          */
/* idempotent, and isolated to the migrated slots.                      */
/* ------------------------------------------------------------------ */

static void TestRepublishStalePointer(void)
{
    struct CompressedSpriteSheet beforeSheet[ARRAY_COUNT(gTrainerFrontPicTable)];
    struct CompressedSpritePalette beforePal[ARRAY_COUNT(gTrainerFrontPicPaletteTable)];
    struct CompressedSpritePalette beforeBackPal[ARRAY_COUNT(gTrainerBackPicPaletteTable)];
    struct CompressedSpriteSheet beforeBackSheet[ARRAY_COUNT(gTrainerBackPicTable)];
    struct BackFrameCapture beforeBackFrames;
    struct EmeraldResourceCompatDiagnostics diag;
    size_t i;
    enum EmeraldResourceCompatStatus status;

    if (!LoadFamilyFixtures() || !LoadBackFamilyFixtures())
        return;

    /* Requires a valid session image (published by the earlier tests). */
    CHECK("valid session published before republish test",
          gTrainerFrontPicTable[0].data != NULL
              && gTrainerFrontPicPaletteTable[0].data != NULL
              && gTrainerBackPicTable[0].data != NULL
              && gTrainerBackPicPaletteTable[FixtureForIndex(0)->backPaletteIndex >= 0
                                              ? FixtureForIndex(0)->backPaletteIndex
                                              : 0].data != NULL);

    CaptureSheetTable(beforeSheet);
    CapturePaletteTable(beforePal);
    CaptureBackPaletteTable(beforeBackPal);
    CaptureBackSheetTable(beforeBackSheet);
    CaptureBackFrames(&beforeBackFrames);

    /* Simulate a state load that restored stale/foreign process-local words
     * into EVERY migrated slot - front sheets, front palettes, all 8 back
     * palettes, the 8 back SHEET table slots and all 34 back frame slots. */
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
        gTrainerFrontPicTable[i].data = (const u32 *)(uintptr_t)(0xDEAD0000u + i);
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicPaletteTable); i++)
        gTrainerFrontPicPaletteTable[i].data =
            (const u32 *)(uintptr_t)(0xDEAD1000u + i);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicPaletteTable); i++)
        gTrainerBackPicPaletteTable[i].data =
            (const u32 *)(uintptr_t)(0xDEAD2000u + i);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
        gTrainerBackPicTable[i].data = (const u32 *)(uintptr_t)(0xDEAD3000u + i);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        size_t f;
        for (f = 0; f < sBackFixtures[i].frames; f++)
            kBackFrameArrays[i][f].data =
                (const void *)(uintptr_t)(0xDEAD4000u + i * 5u + f);
    }

    status = EmeraldResourceCompat_Republish(&diag);
    CHECK("republish repairs stale pointers", status == EMERALD_COMPAT_OK);

    /* Repaired to the current-session pointers; only the migrated slots are
     * ever touched across the whole republish (R7B §4/§7, R8). */
    AssertOnlyMigratedPointsTo(beforeSheet, beforePal, beforeBackPal,
                               beforeBackSheet, &beforeBackFrames);
    CHECK("republish success leaves diagnostics empty", diag.stage[0] == '\0');

    /* Idempotent: a second republish keeps the same pointers and the tables
     * byte-identical (nothing to change). */
    status = EmeraldResourceCompat_Republish(&diag);
    CHECK("republish idempotent ok", status == EMERALD_COMPAT_OK);
    CHECK("republish idempotent tables byte-identical",
          TablesMatch(beforeSheet, beforePal, beforeBackPal,
                      beforeBackSheet, &beforeBackFrames));
}

/* ------------------------------------------------------------------ */
/* 4c. R9 §8: strict init hard-fails on an unserved Pokémon family      */
/*                                                                     */
/* With the compiled leaf payloads gone from the native link (R9 §7),  */
/* a snapshot that cannot publish the Pokémon battle family is an init */
/* error on the game path: NULL Pokémon slots cannot render battles.   */
/* The strict entry point returns the Pokémon failure with the first  */
/* failing resource in the diagnostics; the trainer family's own       */
/* publication stands (per-family transactionality); the additive      */
/* opt-in above remains the offline/harness contract.                  */
/* ------------------------------------------------------------------ */

static void TestPokemonStrictHardFail(void)
{
    struct Gen3ResourceSnapshot *snapshot;
    struct EmeraldResourceCompatDiagnostics diag;
    size_t i;
    enum EmeraldResourceCompatStatus status;

    if (!LoadFamilyFixtures() || !LoadBackFamilyFixtures())
        return;
    snapshot = BuildFullFamilySnapshot();
    CHECK("strict-fail snapshot builds", snapshot != NULL);
    if (snapshot == NULL)
        return;

    /* Clean slate: tables at sentinels, no registered snapshot. */
    EmeraldResourceCompat_ClearMigratedEntries();
    EmeraldResourceCompat_ClearSnapshot();

    /* Strict init with a trainer-only snapshot: the trainer phase succeeds
     * and publishes, the Pokémon phase fails closed with the first missing
     * resource (slot map resource 0 = abra back sheet) in the diagnostics. */
    status = EmeraldResourceCompat_InitializeFromSnapshot(snapshot, &diag);
    CHECK("strict init hard-fails on missing Pokémon family",
          status == EMERALD_COMPAT_ERR_RESOLVE_FAILED);
    CHECK("strict diag names first Pokémon resource",
          strcmp(diag.canonicalName, "emerald:pokemon/abra/battle/back/sheet") == 0);
    CHECK("strict diag stage is resolve", strcmp(diag.stage, "resolve") == 0);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("strict fail leaves front sentinel",
              gMonFrontPicTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("strict fail leaves back sentinel",
              gMonBackPicTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("strict fail leaves palette sentinel",
              gMonPaletteTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("strict fail leaves shiny sentinel",
              gMonShinyPaletteTable[i].data == NULL);
    /* The trainer family's own publication stands (per-family transaction). */
    CHECK("strict fail keeps trainer published",
          gTrainerFrontPicTable[0].data != NULL
              && gTrainerFrontPicPaletteTable[0].data != NULL
              && gTrainerBackPicTable[0].data != NULL);

    /* Repeatable: a second strict init with the same snapshot fails the same
     * way, tables unchanged (fail-closed, no drift). */
    status = EmeraldResourceCompat_InitializeFromSnapshot(snapshot, &diag);
    CHECK("strict init repeatable failure", status == EMERALD_COMPAT_ERR_RESOLVE_FAILED);
    CHECK("strict diag repeatable name",
          strcmp(diag.canonicalName, "emerald:pokemon/abra/battle/back/sheet") == 0);
    CHECK("strict fail repeatable keeps trainer published",
          gTrainerFrontPicTable[0].data != NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("strict fail repeatable keeps front sentinel",
              gMonFrontPicTable[i].data == NULL);

    DestroySnapshot(snapshot);
}

/* ------------------------------------------------------------------ */
/* 5. Session lifetime + shutdown                                      */
/* ------------------------------------------------------------------ */

static void TestShutdown(void)
{
    struct EmeraldResourceCompatDiagnostics diag;
    size_t i;
    /* Shutdown releases the session image and gates re-initialization. The
     * tables keep the last published pointers (documented R5 behavior: only
     * call Shutdown when no consumer can read the tables). */
    if (!LoadFamilyFixtures() || !LoadBackFamilyFixtures())
        return;
    EmeraldResourceCompat_ClearSnapshot();
    EmeraldResourceCompat_Shutdown();
    EmeraldResourceCompat_TryInitialize(); /* no snapshot -> no-op, no crash */

    /* Republish is unavailable once the session image is gone - fail closed
     * with structured diagnostics, no hidden compiled copy substituted (§8). */
    CHECK("republish unavailable after shutdown",
          EmeraldResourceCompat_Republish(&diag) == EMERALD_COMPAT_ERR_UNAVAILABLE);
    CHECK("unavailable stage is republish", strcmp(diag.stage, "republish") == 0);

    /* Fail-closed clear: exactly the migrated slots return to the NULL
     * sentinel so no consumer can dereference a stale pointer (§14/§18) - the
     * whole family: front sheets, front palettes, all 8 back palettes, the 8
     * back SHEET table slots and all 34 back frame slots. R9 §10: the
     * Pokémon battle tables clear the same way (slot-map driven; this TU's
     * degraded session never published them, so they were already NULL - the
     * pin is that the clear covers every migrated battle slot). */
    EmeraldResourceCompat_ClearMigratedEntries();
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicTable); i++)
        CHECK("clear sets front sheet NULL", gTrainerFrontPicTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerFrontPicPaletteTable); i++)
        CHECK("clear sets front palette NULL", gTrainerFrontPicPaletteTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicPaletteTable); i++)
        CHECK("clear sets back-pal NULL", gTrainerBackPicPaletteTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
        CHECK("clear sets back sheet NULL", gTrainerBackPicTable[i].data == NULL);
    for (i = 0; i < ARRAY_COUNT(gTrainerBackPicTable); i++)
    {
        size_t f;
        for (f = 0; f < sBackFixtures[i].frames; f++)
            CHECK("clear sets back frame NULL", kBackFrameArrays[i][f].data == NULL);
    }
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("clear sets mon front NULL", gMonFrontPicTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("clear sets mon back NULL", gMonBackPicTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("clear sets mon palette NULL", gMonPaletteTable[i].data == NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("clear sets mon shiny NULL", gMonShinyPaletteTable[i].data == NULL);
}

int main(void)
{
    /* Save-state audit first: it asserts the pristine pre-publish sentinel
     * baseline, which requires no prior publish to have run. */
    TestSaveStateAudit();
    TestFamilyMappingAndParity();
    TestRuntimeHook();
    TestFailClosedTransactional();
    TestRepublishStalePointer();
    TestPokemonStrictHardFail();
    TestShutdown();

    FreeFamilyFixtures();
    FreeBackFamilyFixtures();
    if (gFailures != 0)
    {
        printf("emerald_trainer_native_compat_test FAILED: %d/%d checks\n",
               gFailures, gChecks);
        return 1;
    }
    printf("emerald trainer native compat test passed (%d checks)\n", gChecks);
    return 0;
}

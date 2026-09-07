/* R9 §8f: REAL native table headers vs the generated Pokémon battle slot map.
 *
 * Compiles the REAL src/data.c native branch - the actual
 * data/pokemon_graphics/ table headers (front_pic_table.h, back_pic_table.h,
 * palette_table.h, shiny_palette_table.h, still_front_pic_table.h) - together
 * with the real compatibility seams (emerald_resource_compat.c,
 * emerald_trainer_native_compat.c, emerald_pokemon_native_compat.c), the R4
 * session/pack chain, the R6 runtime loader and the real decompressor
 * (src/platform/bios.c), then verifies the tables against the generated slot
 * map (pokemon_battle_slots.generated.h):
 *
 *   PRE-INIT (compiled state):
 *    - ARRAY_COUNT of every real battle table == POKEMON_BATTLE_SLOTS_PER_TABLE
 *      (440): the real headers cannot add or drop a row without failing;
 *    - every row 0..439 of the four battle tables carries tag == row index
 *      (designated-placement agreement: each row sits at its species id);
 *    - the four battle tables start at the NULL sentinel (the still-front
 *      table and the icon/footprint tables still hold their compiled leaves
 *      during the R15 pending phase - the seam overwrites them at publish);
 *    - the generated slot map itself: R15 removed the last external marker
 *      (back[412] and still[412] both publish the still-front EGG payload).
 *
 *   POST-INIT (RegisterRuntimeSnapshot with the REAL production pack, the
 *   strict publish-at-registration path):
 *    - every slot of all SEVEN tables is published (data != NULL) with tag
 *      and size retained (only .data changes); back[412] and still[412]
 *      publish the SAME still-front EGG stream;
 *    - the trainer family publishes into the REAL data.c trainer tables too.
 *
 *   POST-INIT LIFECYCLE (R9 §10, the state-load path native_state.c drives):
 *    - stale every slot of all seven tables with foreign words, then
 *      EmeraldResourceCompat_Republish re-derives every migrated slot to the
 *      SAME session-image pointer (allocation-free, idempotent);
 *    - EmeraldResourceCompat_ClearMigratedEntries NULLs exactly the migrated
 *      slots (size/tag preserved) and the final Republish restores every
 *      slot to its session pointer.
 *
 * The TU cannot coexist with the harness TU (both define the real tables), so
 * it carries its own CHECK machinery and runner. INCBIN_* expands to {0}
 * stubs: the real native build resolves them via tools/preproc into literal
 * payload bytes, which a standalone gcc TU cannot do. The only payload
 * symbols the real tables reference on native are still-front leaves
 * (still_front_pic_table.h uses the compiled SPECIES_SPRITE macro on every
 * target); pokemon.h + the CircledQuestionMark stub (normally in graphics.c)
 * supply them. No battle-family leaf is referenced: the four battle tables
 * expand to NULL sentinels through the SPECIES_BATTLE_* macros (data.c).
 * §8f pins pointer identity and slot agreement - payload bytes are pinned by
 * the §8e production proof.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Real decompressor + hardware globals, exactly as the harness TU compiles
 * them - keeps this TU's environment identical to the real native target. */
#include "../src/platform/bios.c"

/* INCBIN: the real native build resolves INCBIN_* in tools/preproc into
 * literal payload bytes; a standalone gcc TU cannot. {0} stubs - see the
 * header comment. */
#define INCBIN(...) {0}
#define INCBIN_U8  INCBIN
#define INCBIN_U16 INCBIN
#define INCBIN_U32 INCBIN
#define INCBIN_S8  INCBIN
#define INCBIN_S16 INCBIN
#define INCBIN_S32 INCBIN

/* _()/__(): the charmap macros tools/preproc normally resolves into plain
 * (encoded) string literals before gcc sees them; identity is the faithful
 * emulation for a standalone TU (used in every data/text/ header
 * initializer). */
#define _(x)  x
#define __(x) x

/* gMonStillFrontPic_CircledQuestionMark lives in src/graphics.c (row 8); the
 * still-front table's NONE row references it, so provide the same stub. */
const u32 gMonStillFrontPic_CircledQuestionMark[] = {0};

/* The REAL native tables: still-front payload stubs + data.c's native branch
 * (front/back/palette/shiny tables through the SPECIES_BATTLE_* sentinel
 * macros, trainer tables, back frame arrays, still-front table). */
const u8 gMonIcon_Egg[] = {0};
const u8 gMonIcon_QuestionMark[] = {0};
/* R15: the question-mark footprint lives in src/graphics.c (not pokemon.h). */
const u8 gMonFootprint_QuestionMark[] = {0};
#include "../src/data/graphics/pokemon.h"
#include "../src/pokemon_icon.c"
#include "../src/data.c"
/* R15 Phase 2: footprint table + shared icon palettes (graphics.c-owned). */
#include "../src/data/pokemon_graphics/footprint_table.h"
const u16 gMonIconPalettes[4][16] = {0};

#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_provider.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_trainer_native_compat.h"
#include "emerald/resources/pokemon_battle_slots.generated.h"
#include "wild_encounter.h"
#include "../src/emerald/resources/emerald_runtime_loader.c"

/* ------------------------------------------------------------------ */
/* Test harness (own copy - cannot share the harness TU)               */
/* ------------------------------------------------------------------ */

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(label, cond)                                                   \
    do                                                                       \
    {                                                                        \
        gChecks++;                                                           \
        if (!(cond))                                                         \
        {                                                                    \
            fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__);        \
            gFailures++;                                                     \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Pre-init: the compiled tables against the generated slot map        */
/* ------------------------------------------------------------------ */

static void TestCompiledTableState(void)
{
    size_t i;
    size_t externalCount = 0;
    size_t externalIndex = 0;

    /* The real headers must span exactly the slot map's 440 rows. */
    CHECK("front table rows == slots per table",
          ARRAY_COUNT(gMonFrontPicTable) == POKEMON_BATTLE_SLOTS_PER_TABLE);
    CHECK("back table rows == slots per table",
          ARRAY_COUNT(gMonBackPicTable) == POKEMON_BATTLE_SLOTS_PER_TABLE);
    CHECK("palette table rows == slots per table",
          ARRAY_COUNT(gMonPaletteTable) == POKEMON_BATTLE_SLOTS_PER_TABLE);
    CHECK("shiny table rows == slots per table",
          ARRAY_COUNT(gMonShinyPaletteTable) == POKEMON_BATTLE_SLOTS_PER_TABLE);
    CHECK("still-front table rows == slots per table",
          ARRAY_COUNT(gMonStillFrontPicTable) == POKEMON_BATTLE_SLOTS_PER_TABLE);

    /* Designated-placement agreement: every row of every battle table sits at
     * its species id - the table headers place row i at index i, so the slot
     * map's species-ordered layout cannot drift from the headers. */
    /* The shiny table's tag carries the species id PLUS SPECIES_SHINY_TAG
     * (SPECIES_BATTLE_SHINY_PAL in data.c) - the shiny/normal tag split. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        CHECK("front tag == row index", gMonFrontPicTable[i].tag == i);
        CHECK("back tag == row index", gMonBackPicTable[i].tag == i);
        CHECK("palette tag == row index", gMonPaletteTable[i].tag == i);
        CHECK("shiny tag == row index + SHINY_TAG",
              gMonShinyPaletteTable[i].tag == i + SPECIES_SHINY_TAG);
    }

    /* R9 §7: every battle slot starts at the NULL sentinel... */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        CHECK("front sentinel pre-init", gMonFrontPicTable[i].data == NULL);
        CHECK("palette sentinel pre-init", gMonPaletteTable[i].data == NULL);
        CHECK("shiny sentinel pre-init", gMonShinyPaletteTable[i].data == NULL);
    }
    /* R9 §6: the one external still-EGG slot stays compiled on every
     * target - back[EGG] and still[EGG] keep the compiled
     * gMonStillFrontPic_Egg pointer pre-init; every other row starts at
     * the NULL sentinel. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        if (i == SPECIES_EGG)
        {
            CHECK("back-EGG compiled pre-init",
                  gMonBackPicTable[i].data ==
                      (const u32 *)(const void *)gMonStillFrontPic_Egg);
        }
        else
        {
            CHECK("back sentinel pre-init", gMonBackPicTable[i].data == NULL);
        }
        CHECK("back tag == row index", gMonBackPicTable[i].tag == i);
    }

    /* R15 Phase 2: the still-front table is migrated too - NULL sentinel
     * pre-init on native (SPECIES_STILL_SPRITE) except the external EGG
     * row (SPECIES_SPRITE keeps the compiled pointer). */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        if (i == SPECIES_EGG)
        {
            CHECK("still-EGG compiled pre-init",
                  gMonStillFrontPicTable[i].data ==
                      (const u32 *)(const void *)gMonStillFrontPic_Egg);
        }
        else
        {
            CHECK("still-front sentinel pre-init",
                  gMonStillFrontPicTable[i].data == NULL);
        }
        CHECK("still-front size == MON_PIC_SIZE",
              gMonStillFrontPicTable[i].size == MON_PIC_SIZE);
        CHECK("still-front tag == row index",
              gMonStillFrontPicTable[i].tag == i);
    }

    /* The generated slot map: exactly two external markers (back[EGG] and
     * still[EGG] - the R9 §6 stays-compiled slot). */
    for (i = 0; i < POKEMON_BATTLE_SLOT_COUNT; i++)
    {
        if (kPokemonBattleCompatSlots[i] == POKEMON_BATTLE_EXTERNAL_SLOT)
        {
            externalCount++;
            externalIndex = i;
        }
    }
    CHECK("slot map has exactly two external markers", externalCount == 2);
    (void)externalIndex;
}

/* ------------------------------------------------------------------ */
/* Post-init: the real production pack publishes, external untouched   */
/* ------------------------------------------------------------------ */

static void TestProductionPublication(const char *packPath)
{
    enum EmeraldResourceCompatStatus status;
    size_t i;

    status = EmeraldResourceCompat_RegisterRuntimeSnapshot(packPath);
    CHECK("real tables: production pack registers", status == EMERALD_COMPAT_OK);
    if (status != EMERALD_COMPAT_OK)
        return;

    /* Every non-external battle slot published; tags and sizes retained. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        CHECK("front published", gMonFrontPicTable[i].data != NULL);
        CHECK("front tag retained", gMonFrontPicTable[i].tag == i);
        CHECK("front size retained", gMonFrontPicTable[i].size == MON_PIC_SIZE);
        CHECK("palette published", gMonPaletteTable[i].data != NULL);
        CHECK("palette tag retained", gMonPaletteTable[i].tag == i);
        CHECK("shiny published", gMonShinyPaletteTable[i].data != NULL);
        CHECK("shiny tag retained",
              gMonShinyPaletteTable[i].tag == i + SPECIES_SHINY_TAG);
        CHECK("back published", i == SPECIES_EGG
              || gMonBackPicTable[i].data != NULL);
        CHECK("back tag retained", gMonBackPicTable[i].tag == i);
        CHECK("back size retained",
              gMonBackPicTable[i].size == MON_PIC_SIZE);
    }

    /* R9 §6: the external back-EGG row keeps the compiled pointer after
     * publication (the compat skips POKEMON_BATTLE_EXTERNAL_SLOT rows). */
    CHECK("back-EGG stays compiled after publication",
          gMonBackPicTable[SPECIES_EGG].data ==
              (const u32 *)(const void *)gMonStillFrontPic_Egg);
    CHECK("back-EGG size retained",
          gMonBackPicTable[SPECIES_EGG].size == MON_PIC_SIZE);
    CHECK("back-EGG tag retained",
          gMonBackPicTable[SPECIES_EGG].tag == SPECIES_EGG);

    /* R15: the still-front table is published like the other sheets. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        CHECK("still-front published", gMonStillFrontPicTable[i].data != NULL);
        CHECK("still-front tag retained",
              gMonStillFrontPicTable[i].tag == i);
        CHECK("still-front size retained",
              gMonStillFrontPicTable[i].size == MON_PIC_SIZE);
    }
    CHECK("still-front EGG == back-EGG compiled pointer",
          gMonStillFrontPicTable[SPECIES_EGG].data ==
              gMonBackPicTable[SPECIES_EGG].data);

    /* R15: the icon and footprint tables publish raw pack streams. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("icon published", gMonIconTable[i] != NULL);
    for (i = 0; i < POKEMON_BATTLE_SLOT_COUNT - 6u * POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("footprint published", gMonFootprintTable[i] != NULL);

    /* The trainer family publishes into the REAL data.c tables. */
    CHECK("real trainer front published",
          gTrainerFrontPicTable[0].data != NULL);
    CHECK("real trainer back published",
          gTrainerBackPicTable[0].data != NULL);
    CHECK("real trainer back frame published",
          gTrainerBackPicTable_Brendan[0].data != NULL);

    /* Idempotent registration. */
    status = EmeraldResourceCompat_RegisterRuntimeSnapshot(packPath);
    CHECK("real tables: registration idempotent", status == EMERALD_COMPAT_OK);
}

/* ------------------------------------------------------------------ */
/* Post-init lifecycle: the state-load path over the REAL tables       */
/* ------------------------------------------------------------------ */

/* R9 §10: a save-state load restores process-local words into the native
 * tables (plain .data, outside the serialized slices), so the runtime calls
 * EmeraldResourceCompat_Republish after every load to re-derive the
 * current-session compatibility image (native_state.c). This test drives
 * that exact sequence at the API level over the REAL tables and the REAL
 * production pack session:
 *
 *   - stale EVERY battle slot (all four tables, all 440 rows) with synthetic
 *     DEAD words, plus a planted stale word in the external back-EGG row;
 *   - Republish re-derives every migrated slot to the SAME session-image
 *     pointer captured before the staling (allocation-free, idempotent);
 *     the external back-EGG row is never written by the seam - the planted
 *     stale word survives, which pins that the seam cannot corrupt it either;
 *     the compiled still-front table is byte-identical throughout;
 *   - ClearMigratedEntries (the fail-closed path) NULLs exactly the migrated
 *     slots, preserving size/tag, leaving the external row and still-front
 *     table untouched;
 *   - a final Republish restores every slot to its session-image pointer.
 */
static void TestSaveStateLifecycle(const char *packPath)
{
    struct EmeraldResourceCompatDiagnostics diag;
    const u32 *published[5][POKEMON_BATTLE_SLOTS_PER_TABLE];
    const u8 *publishedIcon[POKEMON_BATTLE_SLOTS_PER_TABLE];
    const u8 *publishedFootprint[POKEMON_BATTLE_SLOT_COUNT
                                 - 6u * POKEMON_BATTLE_SLOTS_PER_TABLE];
    size_t i;
    enum EmeraldResourceCompatStatus status;

    (void)packPath;

    /* Capture the published session-image pointers (kind-major: front, back,
     * palette, shiny, still-front) plus the raw icon/footprint rows. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        published[0][i] = gMonFrontPicTable[i].data;
        published[1][i] = gMonBackPicTable[i].data;
        published[2][i] = gMonPaletteTable[i].data;
        published[3][i] = gMonShinyPaletteTable[i].data;
        published[4][i] = gMonStillFrontPicTable[i].data;
        publishedIcon[i] = gMonIconTable[i];
    }
    for (i = 0; i < POKEMON_BATTLE_SLOT_COUNT - 6u * POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        publishedFootprint[i] = gMonFootprintTable[i];
    CHECK("lifecycle: session image published",
          published[0][0] != NULL && published[1][0] != NULL
              && published[2][0] != NULL && published[3][0] != NULL
              && published[4][0] != NULL && publishedIcon[0] != NULL
              && publishedFootprint[0] != NULL);
    CHECK("lifecycle: back-EGG and still-EGG share the compiled leaf",
          published[1][SPECIES_EGG] == published[4][SPECIES_EGG]);

    /* Simulate the state-load restore: foreign words in EVERY migrated slot
     * plus a planted stale word in the two external EGG rows (the seam is
     * never allowed to write those, R9 §6). */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        gMonFrontPicTable[i].data = (const u32 *)(uintptr_t)(0xDEAD0000u + i);
        gMonBackPicTable[i].data = (const u32 *)(uintptr_t)(0xDEAD1000u + i);
        gMonPaletteTable[i].data = (const u32 *)(uintptr_t)(0xDEAD2000u + i);
        gMonShinyPaletteTable[i].data = (const u32 *)(uintptr_t)(0xDEAD3000u + i);
        gMonStillFrontPicTable[i].data = (const u32 *)(uintptr_t)(0xDEAD4000u + i);
        gMonIconTable[i] = (const u8 *)(uintptr_t)(0xDEAD5000u + i);
    }
    for (i = 0; i < POKEMON_BATTLE_SLOT_COUNT - 6u * POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        gMonFootprintTable[i] = (const u8 *)(uintptr_t)(0xDEAD6000u + i);
    gMonBackPicTable[SPECIES_EGG].data = (const u32 *)(uintptr_t)0xDEADBEEF;
    gMonStillFrontPicTable[SPECIES_EGG].data = (const u32 *)(uintptr_t)0xDEADBEEF;

    /* The runtime's post-load republish (native_state.c). */
    status = EmeraldResourceCompat_Republish(&diag);
    CHECK("lifecycle: republish repairs stale pointers", status == EMERALD_COMPAT_OK);
    CHECK("lifecycle: republish diagnostics empty", diag.stage[0] == '\0');

    /* Every migrated slot re-derived to its session-image pointer. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        CHECK("lifecycle: front re-derived to session pointer",
              gMonFrontPicTable[i].data == published[0][i]);
        if (i == SPECIES_EGG)
        {
            /* external slot: the planted word survives (the seam cannot
             * write it either - R9 §6 corruption pin). */
            CHECK("lifecycle: back-EGG planted word survives",
                  gMonBackPicTable[i].data == (const u32 *)(uintptr_t)0xDEADBEEF);
        }
        else
        {
            CHECK("lifecycle: back re-derived to session pointer",
                  gMonBackPicTable[i].data == published[1][i]);
        }
        CHECK("lifecycle: palette re-derived to session pointer",
              gMonPaletteTable[i].data == published[2][i]);
        CHECK("lifecycle: shiny re-derived to session pointer",
              gMonShinyPaletteTable[i].data == published[3][i]);
        if (i == SPECIES_EGG)
        {
            CHECK("lifecycle: still-EGG planted word survives",
                  gMonStillFrontPicTable[i].data == (const u32 *)(uintptr_t)0xDEADBEEF);
        }
        else
        {
            CHECK("lifecycle: still-front re-derived to session pointer",
                  gMonStillFrontPicTable[i].data == published[4][i]);
        }
        CHECK("lifecycle: icon re-derived to session pointer",
              gMonIconTable[i] == publishedIcon[i]);
    }
    for (i = 0; i < POKEMON_BATTLE_SLOT_COUNT - 6u * POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("lifecycle: footprint re-derived to session pointer",
              gMonFootprintTable[i] == publishedFootprint[i]);
    CHECK("lifecycle: trainer tables republished too",
          gTrainerFrontPicTable[0].data != NULL
              && gTrainerBackPicTable[0].data != NULL
              && gTrainerBackPicTable_Brendan[0].data != NULL);

    /* Idempotent: a second republish keeps the same pointers. */
    status = EmeraldResourceCompat_Republish(&diag);
    CHECK("lifecycle: republish idempotent ok", status == EMERALD_COMPAT_OK);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        CHECK("lifecycle: front idempotent pointer",
              gMonFrontPicTable[i].data == published[0][i]);
        if (i != SPECIES_EGG)
        {
            CHECK("lifecycle: back idempotent pointer",
                  gMonBackPicTable[i].data == published[1][i]);
            CHECK("lifecycle: still-front idempotent pointer",
                  gMonStillFrontPicTable[i].data == published[4][i]);
        }
        CHECK("lifecycle: palette idempotent pointer",
              gMonPaletteTable[i].data == published[2][i]);
        CHECK("lifecycle: shiny idempotent pointer",
              gMonShinyPaletteTable[i].data == published[3][i]);
        CHECK("lifecycle: icon idempotent pointer",
              gMonIconTable[i] == publishedIcon[i]);
    }
    for (i = 0; i < POKEMON_BATTLE_SLOT_COUNT - 6u * POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("lifecycle: footprint idempotent pointer",
              gMonFootprintTable[i] == publishedFootprint[i]);
    CHECK("lifecycle: back-EGG planted word still survives",
          gMonBackPicTable[SPECIES_EGG].data == (const u32 *)(uintptr_t)0xDEADBEEF);
    CHECK("lifecycle: still-EGG planted word still survives",
          gMonStillFrontPicTable[SPECIES_EGG].data == (const u32 *)(uintptr_t)0xDEADBEEF);

    /* Fail-closed clear: exactly the migrated slots (all seven tables)
     * return to NULL; size/tag survive (only .data is the migrated word);
     * the trainer family NULLs alongside. */
    EmeraldResourceCompat_ClearMigratedEntries();
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        CHECK("lifecycle: clear front NULL", gMonFrontPicTable[i].data == NULL);
        if (i == SPECIES_EGG)
        {
            /* external slot: the clear never touches it (planted word). */
            CHECK("lifecycle: clear back-EGG untouched",
                  gMonBackPicTable[i].data == (const u32 *)(uintptr_t)0xDEADBEEF);
            CHECK("lifecycle: clear still-EGG untouched",
                  gMonStillFrontPicTable[i].data == (const u32 *)(uintptr_t)0xDEADBEEF);
        }
        else
        {
            CHECK("lifecycle: clear back NULL", gMonBackPicTable[i].data == NULL);
            CHECK("lifecycle: clear still-front NULL",
                  gMonStillFrontPicTable[i].data == NULL);
        }
        CHECK("lifecycle: clear palette NULL", gMonPaletteTable[i].data == NULL);
        CHECK("lifecycle: clear shiny NULL", gMonShinyPaletteTable[i].data == NULL);
        CHECK("lifecycle: clear icon NULL", gMonIconTable[i] == NULL);
    }
    for (i = 0; i < POKEMON_BATTLE_SLOT_COUNT - 6u * POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("lifecycle: clear footprint NULL", gMonFootprintTable[i] == NULL);
    CHECK("lifecycle: clear preserves size/tag",
          gMonFrontPicTable[0].size == MON_PIC_SIZE
              && gMonFrontPicTable[0].tag == 0
              && gMonShinyPaletteTable[0].tag == SPECIES_SHINY_TAG
              && gMonStillFrontPicTable[0].size == MON_PIC_SIZE
              && gMonStillFrontPicTable[0].tag == 0);
    CHECK("lifecycle: clear leaves trainer slots NULL",
          gTrainerFrontPicTable[0].data == NULL
              && gTrainerBackPicTable[0].data == NULL
              && gTrainerBackPicTable_Brendan[0].data == NULL);

    /* The final republish restores every slot to its session pointer. */
    status = EmeraldResourceCompat_Republish(&diag);
    CHECK("lifecycle: republish after clear ok", status == EMERALD_COMPAT_OK);
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        CHECK("lifecycle: front restored to session pointer",
              gMonFrontPicTable[i].data == published[0][i]);
        if (i != SPECIES_EGG)
        {
            CHECK("lifecycle: back restored to session pointer",
                  gMonBackPicTable[i].data == published[1][i]);
            CHECK("lifecycle: still-front restored to session pointer",
                  gMonStillFrontPicTable[i].data == published[4][i]);
        }
        CHECK("lifecycle: palette restored to session pointer",
              gMonPaletteTable[i].data == published[2][i]);
        CHECK("lifecycle: shiny restored to session pointer",
              gMonShinyPaletteTable[i].data == published[3][i]);
        CHECK("lifecycle: icon restored to session pointer",
              gMonIconTable[i] == publishedIcon[i]);
    }
    for (i = 0; i < POKEMON_BATTLE_SLOT_COUNT - 6u * POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
        CHECK("lifecycle: footprint restored to session pointer",
              gMonFootprintTable[i] == publishedFootprint[i]);
    CHECK("lifecycle: back-EGG planted word survives the final republish",
          gMonBackPicTable[SPECIES_EGG].data == (const u32 *)(uintptr_t)0xDEADBEEF);
    CHECK("lifecycle: still-EGG planted word survives the final republish",
          gMonStillFrontPicTable[SPECIES_EGG].data == (const u32 *)(uintptr_t)0xDEADBEEF);
}

int main(int argc, char **argv)
{
    const char *packPath;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <production-pack.rpack>\n", argv[0]);
        return 2;
    }
    packPath = argv[1];

    TestCompiledTableState();
    TestProductionPublication(packPath);
    TestSaveStateLifecycle(packPath);

    if (gFailures != 0)
    {
        printf("emerald real tables test FAILED: %d/%d checks\n",
               gFailures, gChecks);
        return 1;
    }
    printf("emerald real tables test passed (%d checks)\n", gChecks);
    return 0;
}

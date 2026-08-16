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
 *    - every row 0..439 of all four battle tables carries tag == row index
 *      (designated-placement agreement: each row sits at its species id);
 *    - every battle slot starts at the NULL sentinel EXCEPT the one permanent
 *      external slot: gMonBackPicTable[412] (the back-EGG row, R9 §6/§7) whose
 *      data IS the compiled gMonStillFrontPic_Egg leaf - the identity pin that
 *      makes the slot map's POKEMON_BATTLE_EXTERNAL_SLOT real;
 *    - the still-front table (compiled on every target, not migrated) has all
 *      440 rows live, its EGG row aliasing the same gMonStillFrontPic_Egg;
 *    - the generated slot map itself: exactly ONE external marker, at slot
 *      index POKEMON_BATTLE_SLOTS_PER_TABLE + 412 (852 == back[412]).
 *
 *   POST-INIT (RegisterRuntimeSnapshot with the REAL production pack, the
 *   strict publish-at-registration path):
 *    - every non-external battle slot 0..439 is published (data != NULL) with
 *      tag and size retained (only .data changes);
 *    - the external back-EGG row is UNTOUCHED: same data pointer
 *      (&gMonStillFrontPic_Egg), size MON_PIC_SIZE, tag 412;
 *    - the compiled still-front table is never touched; the trainer family
 *      publishes into the REAL data.c trainer tables as well.
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
#include "../src/data/graphics/pokemon.h"
#include "../src/data.c"

#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_provider.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_trainer_native_compat.h"
#include "emerald/resources/pokemon_battle_slots.generated.h"
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
    /* ...except the ONE permanent external slot: back[412] (back-EGG row) is
     * the compiled leaf, never touched by the seam. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        if (i == SPECIES_EGG)
        {
            CHECK("external back-EGG data == compiled egg leaf",
                  gMonBackPicTable[i].data ==
                      (const u32 *)gMonStillFrontPic_Egg);
            CHECK("external back-EGG size == MON_PIC_SIZE",
                  gMonBackPicTable[i].size == MON_PIC_SIZE);
            CHECK("external back-EGG tag == SPECIES_EGG",
                  gMonBackPicTable[i].tag == SPECIES_EGG);
        }
        else
        {
            CHECK("back sentinel pre-init", gMonBackPicTable[i].data == NULL);
        }
    }

    /* The compiled still-front table (not migrated) is fully live, and its
     * EGG row aliases the same compiled leaf as the back-EGG external row. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        CHECK("still-front live pre-init",
              gMonStillFrontPicTable[i].data != NULL);
        CHECK("still-front tag == row index",
              gMonStillFrontPicTable[i].tag == i);
    }
    CHECK("still-front EGG aliases compiled leaf",
          gMonStillFrontPicTable[SPECIES_EGG].data ==
              (const u32 *)gMonStillFrontPic_Egg);

    /* The generated slot map itself: exactly ONE external marker, and it is
     * back[412] (slot index 852 = 440 + 412 in the kind-major layout). */
    for (i = 0; i < POKEMON_BATTLE_SLOT_COUNT; i++)
    {
        if (kPokemonBattleCompatSlots[i] == POKEMON_BATTLE_EXTERNAL_SLOT)
        {
            externalCount++;
            externalIndex = i;
        }
    }
    CHECK("slot map has exactly one external marker", externalCount == 1);
    CHECK("external marker is back[412]",
          externalIndex ==
              POKEMON_BATTLE_SLOTS_PER_TABLE + SPECIES_EGG);
    CHECK("external marker == SPECIES_EGG row of back table",
          kPokemonBattleCompatSlots[POKEMON_BATTLE_SLOTS_PER_TABLE +
                                    SPECIES_EGG] == POKEMON_BATTLE_EXTERNAL_SLOT);
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
        if (i != SPECIES_EGG)
        {
            CHECK("back published", gMonBackPicTable[i].data != NULL);
            CHECK("back tag retained", gMonBackPicTable[i].tag == i);
            CHECK("back size retained",
                  gMonBackPicTable[i].size == MON_PIC_SIZE);
        }
    }

    /* The external back-EGG row is UNTOUCHED by the seam: same compiled
     * pointer, same size, same tag. */
    CHECK("external back-EGG untouched after init",
          gMonBackPicTable[SPECIES_EGG].data ==
              (const u32 *)gMonStillFrontPic_Egg);
    CHECK("external back-EGG size untouched",
          gMonBackPicTable[SPECIES_EGG].size == MON_PIC_SIZE);
    CHECK("external back-EGG tag untouched",
          gMonBackPicTable[SPECIES_EGG].tag == SPECIES_EGG);

    /* The compiled still-front table is never touched. */
    for (i = 0; i < POKEMON_BATTLE_SLOTS_PER_TABLE; i++)
    {
        CHECK("still-front untouched", gMonStillFrontPicTable[i].data != NULL);
    }
    CHECK("still-front EGG still aliases compiled leaf",
          gMonStillFrontPicTable[SPECIES_EGG].data ==
              (const u32 *)gMonStillFrontPic_Egg);

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

    if (gFailures != 0)
    {
        printf("emerald real tables test FAILED: %d/%d checks\n",
               gFailures, gChecks);
        return 1;
    }
    printf("emerald real tables test passed (%d checks)\n", gChecks);
    return 0;
}

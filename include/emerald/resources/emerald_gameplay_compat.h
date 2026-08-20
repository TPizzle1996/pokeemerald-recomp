#ifndef EMERALD_RESOURCES_EMERALD_GAMEPLAY_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_GAMEPLAY_COMPAT_H

/* R13-D1: Emerald gameplay-data (species/moves/shared tables/fonts)
 * publication seam.
 *
 * The gameplay families are the D1 structured-data rows + fonts. Their
 * native fill targets are the HOST_DATA arrays declared by
 * gameplay_data_native.h (and the ten u16 font glyph arrays already
 * HOST_DATA in src/fonts.c). This seam validates the session's pack
 * against the generated inventory (kGameplayNativeResources: name/size/
 * schema set equality) plus the family count pins, resolves every
 * resource through the NORMAL M0/M1 snapshot (type STRUCTURED_DATA/FONT,
 * winner must be the ROM_BASE provider), verifies byte equality with the
 * pack and slice-disjointness, then publishes all the arrays atomically.
 *
 * REFUSE CONTRACT: the D1 families have NO compiled fallback once the
 * C-side guards land (the const definitions are NATIVE_LINUX-guarded in
 * src/data/pokemon/{tmhm_learnsets,tutor_learnsets,egg_moves,
 * level_up_learnsets,level_up_learnset_pointers}.h and
 * src/data/contest_moves.h), so a session whose gameplay data cannot be
 * published is a REFUSED session - the loader rolls the whole
 * registration back (including every earlier seam). The seam itself is
 * transactional: phase 1 validates everything before any allocation,
 * phase 2 builds the levelup leaf arena + precomputes every fill, phase 3
 * publishes atomically (infallible stores). On any phase-1/2 failure
 * nothing is written and the diagnostics name the first failing entry.
 *
 * Platform-neutral in the sense that no ENGINE GLOBALS beyond the
 * fill-target header/struct headers are touched; it drives the gen3
 * session core like the other compat seams.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/resource_types.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/gameplay_native.generated.h"
#include "emerald/resources/gameplay_callbacks.generated.h"
#include "emerald/resources/gameplay_item_callbacks_native.h"
#include "emerald/resources/gameplay_data_native.h"
#include "emerald/resources/emerald_text_compat.h"

/* Gameplay+trainer inventory pins. D1: 3,300 structured + 10 fonts; D2 adds
 * the gItems family (schema 11, 377 x 44 B); R13-E1 adds the three trainer
 * families (schema 16 metadata 855, 17 party 854, 18 class-name 66).
 * Combined inventory = 5,462 resources, which must agree with the generated
 * inventory at compile time. EmeraldGameplayCompat publishes the schema 1..15
 * structured + font families in-place; the schema 16..18 trainer families are
 * owned by EmeraldTrainerCompat (also validated here for presence/ownership
 * but filled by that seam). */
#if GAMEPLAY_NATIVE_RESOURCE_COUNT != 5462u
#error "R13-E gameplay+trainer resource count disagrees with the generated inventory"
#endif

#define EMERALD_GAMEPLAY_SPECIES_COUNT 412u
#define EMERALD_GAMEPLAY_LEVELUP_COUNT 411u
#define EMERALD_GAMEPLAY_TMHM_COUNT    412u
#define EMERALD_GAMEPLAY_TUTOR_COUNT   412u
#define EMERALD_GAMEPLAY_EGG_COUNT     165u
#define EMERALD_GAMEPLAY_MOVE_COUNT    355u
#define EMERALD_GAMEPLAY_GROWTH_COUNT  8u
#define EMERALD_GAMEPLAY_FONT_COUNT    10u
#define EMERALD_GAMEPLAY_ITEM_COUNT    377u

/* The six trade-evolution held items for which the recomp's compiled gItems
 * deliberately diverges from vanilla (the fork override, NOT normalized
 * back): vanilla rows are ITEM_USE_BAG_MENU(0x04) + CannotUse; the recomp
 * uses ITEM_USE_PARTY_MENU(0x01) + ItemUseOutOfBattle_EvolutionStone so the
 * held items are directly usable to trigger evolution. The publication seam
 * applies exactly these overrides, each guarded (assert the vanilla row is
 * the 0x04+CannotUse baseline before overriding; else REFUSE). Values are
 * the gItems array indices == item ids (constants/items.h). */
#define EMERALD_GAMEPLAY_OVERRIDE_ITEM_COUNT 6u
static const uint16_t kGameplayItemUseOverrides[EMERALD_GAMEPLAY_OVERRIDE_ITEM_COUNT] =
{
    187u, /* ITEM_KINGS_ROCK */
    192u, /* ITEM_DEEP_SEA_TOOTH */
    193u, /* ITEM_DEEP_SEA_SCALE */
    199u, /* ITEM_METAL_COAT */
    201u, /* ITEM_DRAGON_SCALE */
    218u, /* ITEM_UP_GRADE */
};

enum EmeraldGameplayCompatStatus
{
    EMERALD_GAMEPLAY_OK = 0,
    EMERALD_GAMEPLAY_ERR_INVALID_ARGUMENT,   /* NULL snapshot/pack pointer */
    EMERALD_GAMEPLAY_ERR_OUT_OF_MEMORY,
    EMERALD_GAMEPLAY_ERR_RESOLVE_FAILED,     /* M0/M1 snapshot did not resolve */
    EMERALD_GAMEPLAY_ERR_UNEXPECTED_OWNERSHIP, /* winner is not ROM_BASE */
    EMERALD_GAMEPLAY_ERR_PAYLOAD_SIZE_MISMATCH, /* pack/view size or bytes differ */
    EMERALD_GAMEPLAY_ERR_UNEXPECTED_COUNT,   /* family composition != pins */
    EMERALD_GAMEPLAY_ERR_TABLE_MISMATCH,     /* pack disagrees with the inventory */
    EMERALD_GAMEPLAY_ERR_OVERLAPPING_SLICE,  /* two claimed ROM slices overlap */
    EMERALD_GAMEPLAY_ERR_UNEXPECTED_TYPE,    /* entry type != expected family type */
    EMERALD_GAMEPLAY_ERR_UNEXPECTED_SCHEMA,  /* entry schema != expected family schema */
    EMERALD_GAMEPLAY_ERR_RANGE_REGISTRATION, /* arena spans could not register */
    EMERALD_GAMEPLAY_ERR_ITEM_DESCRIPTION,   /* item row's description GBA addr did not bind to an R13-C item text label */
    EMERALD_GAMEPLAY_ERR_ITEM_CALLBACK,      /* item row's field/battle callback addr not in the 27-value census */
    EMERALD_GAMEPLAY_ERR_ITEM_OVERRIDE,      /* a fork-override guard failed (row not the 0x04+CannotUse baseline) */
    EMERALD_GAMEPLAY_ERR_UNAVAILABLE,        /* no published state to republish */
};

/* Structured diagnostics (same shape as the other compat seams). On
 * failure the FIRST failing entry is named. */
struct EmeraldGameplayCompatDiagnostics
{
    char canonicalName[96];
    char stage[24];                /* "build" / "publish" */
    char expectedType[24];
    char actualType[24];
    uint32_t expectedSchema;
    uint32_t actualSchema;
    uint32_t expectedSize;
    uint32_t actualSize;
    char winningProviderId[64];
    char winningProviderVersion[32];
    uint32_t winningProviderPrecedence;
};

/* Resolve every D1 resource of the session through the NORMAL M0/M1
 * snapshot (type STRUCTURED_DATA/FONT, family schema, winner must be the
 * ROM_BASE provider), cross-check the resolver view against the pack
 * entry (size and byte equality), validate the composition against the
 * generated inventory (name/size/schema set equality) and the family
 * count pins, prove the claimed ROM slices pairwise disjoint, validate
 * the fixed-width name rows, then publish the native fill targets
 * (species/move names+tables, exp curves, TM/HM, tutor, egg stream,
 * contest data/tables, TMHM, contest, fonts) and rebuild the level-up
 * learnset pointer table over the freshly built leaf arena. Transactional:
 * every validation runs and every fill is precomputed before any
 * publication; the phase-3 stores are infallible. On any failure the
 * arrays are left untouched and the diagnostics name the first failing
 * resource. Idempotent for a new session: an already-published arena is
 * replaced atomically only after the new session validates. `pack` must
 * be the pack the session was built from. */
enum EmeraldGameplayCompatStatus
EmeraldGameplayCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldGameplayCompatDiagnostics *diagnostics);

/* Fail-closed clear: NULL every level-up learnset pointer (so no host
 * pointer dangles into a freed arena), release the leaf arena, and
 * unregister the seam's arena ranges. Used when a session is rolled back
 * or shut down (idempotent). The fixed HOST_DATA arrays are not pointers
 * and carry no dangling risk; they stay whatever the last success
 * published. */
void EmeraldGameplayCompat_ClearMigratedEntries(void);
void EmeraldGameplayCompat_Shutdown(void);

/* Query helpers (tests + the R13-D ranges walker). PublishedCount is the
 * number of D1 resources published (0 until OK). */
size_t EmeraldGameplayCompat_GetPublishedCount(void);
const char *EmeraldGameplayCompatStatus_Describe(
    enum EmeraldGameplayCompatStatus status);

#endif /* EMERALD_RESOURCES_EMERALD_GAMEPLAY_COMPAT_H */
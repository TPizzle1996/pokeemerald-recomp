/* R13-D1: native gameplay-data fill-target definitions.
 *
 * The single source of the runtime gameplay-data arrays on the native
 * build. Every array is a HOST_DATA fill target: the publication seam
 * (emerald_gameplay_compat.c) fills them from the production pack at
 * boot (Refuse-class), and engine consumers read them through the extern
 * declarations in include/pokemon.h / include/data.h / the guarded data
 * headers / gameplay_data_native.h. Native-only (see Makefile:230 - the
 * emerald resources source files are compiled only by Makefile_pc).
 *
 * gEggMoves is SIZED [1139] so ARRAY_COUNT(gEggMoves) works for
 * src/daycare.c. gLevelUpLearnsets is NULL-initialized and repointed at
 * the seam's levelup leaf arena on publication.
 */

#include "emerald/resources/gameplay_data_native.h"

HOST_DATA struct SpeciesInfo gSpeciesInfo[NUM_SPECIES];
HOST_DATA struct BattleMove gBattleMoves[MOVES_COUNT];
HOST_DATA u32 gExperienceTables[8][MAX_LEVEL + 1];
HOST_DATA u8 gSpeciesNames[NUM_SPECIES][POKEMON_NAME_LENGTH + 1];
HOST_DATA u8 gMoveNames[MOVES_COUNT][MOVE_NAME_LENGTH + 1];
HOST_DATA GameplayTMHMLearnsetData gTMHMLearnsets[NUM_SPECIES];
HOST_DATA u32 sTutorLearnsets[NUM_SPECIES];
HOST_DATA u16 gTutorMoves[TUTOR_MOVE_COUNT];
HOST_DATA u16 gEggMoves[GAMEPLAY_GEGG_MOVES_U16];
HOST_DATA struct ContestMove gContestMoves[MOVES_COUNT];
HOST_DATA struct ContestEffect gContestEffects[48];
HOST_DATA u8 gComboStarterLookupTable[63];
HOST_DATA u16 *gLevelUpLearnsets[NUM_SPECIES];

/* R13-D2: the item fill target. Zero-initialized; the publication seam
 * (emerald_gameplay_compat.c) writes all 377 rows at boot (REFUSE-class:
 * the compiled gItems in src/data/items.h is NATIVE_LINUX-guarded out, so
 * there is no fallback). This is the single native gItems definition. */
HOST_DATA struct Item gItems[ITEMS_COUNT];

#if NUM_SPECIES != 412
#error "R13-D1 species count disagrees with the gameplay-D1 pins"
#endif
#if MOVES_COUNT != 355
#error "R13-D1 move count disagrees with the gameplay-D1 pins"
#endif
#if ITEMS_COUNT != 377
#error "R13-D2 item count disagrees with the gameplay-D2 pins"
#endif
_Static_assert(sizeof(struct SpeciesInfo) == 26, "GameplaySpeciesInfoSize");
_Static_assert(sizeof(struct BattleMove) == 9, "GameplayBattleMoveSize");
_Static_assert(sizeof(struct ContestMove) == 7, "GameplayContestMoveSize");
_Static_assert(sizeof(struct ContestEffect) == 3, "GameplayContestEffectSize");
_Static_assert(sizeof(GameplayTMHMLearnsetData) == 8, "GameplayTMHMSize");
_Static_assert(sizeof(struct Item) == 72, "GameplayItemSize");
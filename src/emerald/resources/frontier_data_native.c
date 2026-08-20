/* R13-E3a-1: native Battle Frontier + Battle Tent fill-target definitions.
 *
 * The single source of the runtime Frontier trainer/mon tables on the native
 * build. The arrays are HOST_DATA fill targets: the publication seam
 * (emerald_frontier_compat.c) fills them from the production pack at boot
 * (REFUSE-class - the compiled const definitions in the battle_frontier data
 * headers, the gBattleFrontierHeldItems block in src/battle_tower.c and
 * gFrontierBannedSpecies in src/frontier_util.c are NATIVE_LINUX-guarded out
 * of the link, so there is no fallback), and engine consumers read them
 * through the extern declarations in include/battle_tower.h (non-const under
 * NATIVE_LINUX). The mon-set leaves (the u16 index streams each trainer's
 * monSet points at) are NOT defined here: the seam rebuilds every trainer
 * row's monSet pointer into its own packed mon-set arena (frontier_data_
 * native.h comment), exactly like the trainer-party arena.
 * Native-only; compiled by Makefile_pc.
 */

#include "emerald/resources/frontier_data_native.h"
#include "emerald/resources/frontier_aux_native.generated.h"
#include "wild_encounter.h"        /* struct WildPokemonHeader for the pike/pyramid wild handoff */
#include "apprentice.h"            /* struct ApprenticeTrainer for gApprentices */

HOST_DATA struct BattleFrontierTrainer gBattleFrontierTrainers[FRONTIER_TRAINERS_COUNT];
HOST_DATA struct FacilityMon gBattleFrontierMons[NUM_FRONTIER_MONS];
HOST_DATA u16 gBattleFrontierHeldItems[EMERALD_FRONTIER_HELDITEMS_COUNT];
HOST_DATA u16 gFrontierBannedSpecies[EMERALD_FRONTIER_BANNED_COUNT];
HOST_DATA struct BattleFrontierTrainer gSlateportBattleTentTrainers[NUM_BATTLE_TENT_TRAINERS];
HOST_DATA struct FacilityMon gSlateportBattleTentMons[NUM_SLATEPORT_TENT_MONS];
HOST_DATA struct BattleFrontierTrainer gVerdanturfBattleTentTrainers[NUM_BATTLE_TENT_TRAINERS];
HOST_DATA struct FacilityMon gVerdanturfBattleTentMons[NUM_VERDANTURF_TENT_MONS];
HOST_DATA struct BattleFrontierTrainer gFallarborBattleTentTrainers[NUM_BATTLE_TENT_TRAINERS];
HOST_DATA struct FacilityMon gFallarborBattleTentMons[NUM_FALLARBOR_TENT_MONS];

_Static_assert(sizeof(struct BattleFrontierTrainer) == 56u,
               "BattleFrontierTrainerWireToNativeSize");
_Static_assert(sizeof(struct FacilityMon) == 14u, "FacilityMonWireToNativeSize");
_Static_assert(sizeof(struct BattleFrontierTrainer) == EMERALD_FRONTIER_TRAINER_NATIVE,
               "FrontierTrainerNativeSize");
_Static_assert(sizeof(struct FacilityMon) == EMERALD_FRONTIER_MON_NATIVE,
               "FrontierMonNativeSize");

/* ---- R13-E3a-2: facility AUX fill targets. The seam fills these at boot. */
HOST_DATA u16 gBattleFactoryMovesTotalPreparation[28];
HOST_DATA u16 gBattleFactoryMovesImpossibleToPredict[15];
HOST_DATA u16 gBattleFactoryMovesWeakeningTheFoe[20];
HOST_DATA u16 gBattleFactoryMovesHighRiskHighReturn[27];
HOST_DATA u16 gBattleFactoryMovesEndurance[28];
HOST_DATA u16 gBattleFactoryMovesSlowAndSteady[33];
HOST_DATA u16 gBattleFactoryMovesDependsOnTheBattlesFlow[6];
HOST_DATA u16 gBattlePalaceEarlyPrizes[6];
HOST_DATA u16 gBattlePalaceLatePrizes[9];
HOST_DATA u16 gBattleArenaShortStreakPrizeItems[6];
HOST_DATA u16 gBattleArenaLongStreakPrizeItems[9];
HOST_DATA struct BattlePikeNPC gBattlePikeNPC[EMERALD_FRONTIER_AUX_PIKE_NPC_SLOTS];
HOST_DATA u16 gBattlePikeSpeeches[42][6];
HOST_DATA u8 gBattlePikeRoomTypeHints[9];
HOST_DATA u8 gBattlePikeHeals[6][3];
HOST_DATA struct BattlePikeWildMon gBattlePikeLvl50Mons1[3];
HOST_DATA struct BattlePikeWildMon gBattlePikeLvl50Mons2[3];
HOST_DATA struct BattlePikeWildMon gBattlePikeLvl50Mons3[3];
HOST_DATA struct BattlePikeWildMon gBattlePikeLvl50Mons4[3];
HOST_DATA struct BattlePikeWildMon gBattlePikeLvlOpenMons1[3];
HOST_DATA struct BattlePikeWildMon gBattlePikeLvlOpenMons2[3];
HOST_DATA struct BattlePikeWildMon gBattlePikeLvlOpenMons3[3];
HOST_DATA struct BattlePikeWildMon gBattlePikeLvlOpenMons4[3];
HOST_DATA struct BattlePyramidFloorTemplate
    gBattlePyramidFloorTemplates[EMERALD_FRONTIER_AUX_PYRAMID_FLOOR_SLOTS];
HOST_DATA u8 gBattlePyramidFloorTemplateOptions[34][2];
HOST_DATA u16 gBattlePyramidPickupItems[20][10];
HOST_DATA u8 gBattlePyramidPickupItemSlots[63][2];
HOST_DATA u16 gFrontierBrainTrainerIds[7];
HOST_DATA struct BattleFrontierBrainMon gFrontierBrainsMons[7][2][3];
HOST_DATA u8 gFrontierBrainStreakAppearances[7][4];

/* Apprentice: the 16-byte global the engine reads via include/apprentice.h
 * (made non-const extern under NATIVE_LINUX). */
HOST_DATA struct ApprenticeTrainer gApprentices[EMERALD_FRONTIER_AUX_APPRENTICE_ROWS];

/* Pike/Pyramid wild-encounter handoff: the header blocks. The info structs
 * (and their 12-row slot tables) live in the seam's packed wild arena; the
 * seam re-points each header's landMonsInfo into that arena. */
HOST_DATA struct WildPokemonHeader gBattlePikeWildMonHeaders[EMERALD_FRONTIER_AUX_PIKE_WILD_SETS + 1u];
HOST_DATA struct WildPokemonHeader gBattlePyramidWildMonHeaders[EMERALD_FRONTIER_AUX_PYRAMID_WILD_SETS + 1u];

_Static_assert(sizeof(struct BattlePikeNPC) == EMERALD_FRONTIER_AUX_PIKE_NPC_NATIVE,
               "PikeNPCWireToNativeSize");
_Static_assert(sizeof(struct BattlePikeWildMon) == 12u, "PikeWildMonWireToNativeSize");
_Static_assert(sizeof(struct BattlePyramidFloorTemplate)
               == EMERALD_FRONTIER_AUX_PYRAMID_FLOOR_NATIVE,
               "PyramidFloorWireToNativeSize");
_Static_assert(sizeof(struct BattleFrontierBrainMon) == EMERALD_FRONTIER_AUX_BRAIN_MONS_WIRE,
               "FrontierBrainMonWireToNativeSize");
_Static_assert(sizeof(struct ApprenticeTrainer) == EMERALD_FRONTIER_AUX_APPRENTICE_NATIVE,
               "ApprenticeWireToNativeSize");
_Static_assert(sizeof(struct WildPokemonHeader) == 40u, "FacilityWildHeaderNativeSize");
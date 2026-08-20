#ifndef EMERALD_RESOURCES_FRONTIER_DATA_NATIVE_H
#define EMERALD_RESOURCES_FRONTIER_DATA_NATIVE_H

/* R13-E3a-1: native Battle Frontier + Battle Tent fill-target declarations.
 *
 * Declares the HOST_DATA arrays the publication seam (emerald_frontier_compat.c)
 * fills from the production pack: `struct BattleFrontierTrainer
 * gBattleFrontierTrainers[300]` (56-byte native rows, monSet rebuilt into the
 * seam's packed mon-set arena), `struct FacilityMon gBattleFrontierMons[882]`
 * (14-byte native rows), the held-items/banned-species u16 leaf arrays, and the
 * three tent trainer tables + three tent mons pools. The mon-set leaves
 * themselves live in a single packed arena owned by the seam (not a fixed
 * array here), exactly like the trainer-party arena. The DEFINITIONS live in
 * src/emerald/resources/frontier_data_native.c (native-only); this header is
 * included by BOTH the definitions TU and the seam TU.
 *
 * The structs here are layout-identical to the engine's definitions in
 * include/battle_tower.h, so the seam writes the exact layout engine consumers
 * read through include/battle_tower.h, without pulling global.h into these
 * global.h-free TUs. sizeof: struct BattleFrontierTrainer 56, struct
 * FacilityMon 14 (checked by _Static_assert in the definitions TU).
 *
 * The native declarations of the arrays in include/battle_tower.h are
 * non-const under NATIVE_LINUX; GBA keeps the const definitions verbatim in
 * src/data/battle_frontier (the trainer/mons/tent headers), src/battle_tower.c
 * and src/frontier_util.c.
 */

#include "gba/types.h"
#include "gba/defines.h"
#include "constants/global.h"     /* PLAYER_NAME_LENGTH, EASY_CHAT_BATTLE_WORDS_COUNT, MAX_MON_MOVES */
#include "constants/battle_frontier_trainers.h"  /* FRONTIER_TRAINERS_COUNT */
#include "constants/battle_frontier_mons.h"      /* NUM_FRONTIER_MONS */
#include "constants/battle_tent_trainers.h"      /* NUM_BATTLE_TENT_TRAINERS */
#include "constants/battle_tent_mons.h"          /* tent mons counts */
#include "emerald/resources/frontier_native.generated.h"  /* stride/count macros */
#include "emerald/resources/frontier_aux_native.generated.h"  /* E3a-2 stride/count macros */

/* Layout-identical copies of include/battle_tower.h's structs, so the seam and
 * the definitions TU never pull global.h. A TU that already includes
 * battle_tower.h (GUARD_BATTLE_TOWER_H set) uses the engine's real structs. The
 * layouts are identical (checked by _Static_assert in the definitions TU). */
#ifndef GUARD_BATTLE_TOWER_H

struct BattleFrontierTrainer
{
    u8 facilityClass;
    u8 filler1[3];
    u8 trainerName[PLAYER_NAME_LENGTH + 1];
    u16 speechBefore[EASY_CHAT_BATTLE_WORDS_COUNT];
    u16 speechWin[EASY_CHAT_BATTLE_WORDS_COUNT];
    u16 speechLose[EASY_CHAT_BATTLE_WORDS_COUNT];
    const u16 *monSet;
};

struct FacilityMon
{
    u16 species;
    u16 moves[MAX_MON_MOVES];
    u8 itemTableId;
    u8 evSpread;
    u8 nature;
};


#endif /* GUARD_BATTLE_TOWER_H */

/* R13-E3a-2 layout-identical copies of the Battle Frontier facility structs
 * (Battle Pike / Pyramid / brain), so the seam and the definitions TU can fill
 * them without pulling global.h. Field NAMES match the engine (battle_pike.c /
 * battle_pyramid.c / frontier_util.c) so a TU that `#define`s its local static
 * table name to the native fill target compiles unchanged. Sizes are the
 * PACKED native sizes (GBA-wire rows drop trailing pad); checked by
 * _Static_assert in the definitions TU. */
struct BattlePikeNPC
{
    u16 graphicsId;
    u8 speechId1;
    u8 speechId2;
    u8 speechId3;
};

struct BattlePikeWildMon
{
    u16 species;
    u8 levelDelta;
    u16 moves[MAX_MON_MOVES];
};

struct BattlePyramidFloorTemplate
{
    u8 numItems;
    u8 numTrainers;
    u8 itemPositions;
    u8 trainerPositions;
    u8 runMultiplier;
    u8 layoutOffsets[8];   /* NUM_LAYOUT_OFFSETS */
};

struct BattleFrontierBrainMon
{
    u16 species;
    u16 heldItem;
    u8 fixedIV;
    u8 nature;
    u8 evs[6];             /* NUM_STATS */
    u16 moves[MAX_MON_MOVES];
};



/* The native fill targets. The E3a-1 arrays (trainers/mons/held/banned/tents)
 * are already declared by include/battle_tower.h + include/frontier_util.h
 * (non-const under NATIVE_LINUX), so they are only redeclared here for TUs
 * that do NOT pull battle_tower.h (the definitions TU + the seam). The
 * R13-E3a-2 facility AUX arrays are unique to this header and are always
 * declared. */
#ifndef GUARD_BATTLE_TOWER_H
extern struct BattleFrontierTrainer gBattleFrontierTrainers[EMERALD_FRONTIER_TRAINER_COUNT];
extern struct FacilityMon gBattleFrontierMons[EMERALD_FRONTIER_MONS_COUNT];
extern u16 gBattleFrontierHeldItems[EMERALD_FRONTIER_HELDITEMS_COUNT];
extern u16 gFrontierBannedSpecies[EMERALD_FRONTIER_BANNED_COUNT];
extern struct BattleFrontierTrainer
    gSlateportBattleTentTrainers[EMERALD_FRONTIER_TENT_TRAINER_COUNT];
extern struct FacilityMon gSlateportBattleTentMons[EMERALD_FRONTIER_TENT_SLATEPORT_MONS];
extern struct BattleFrontierTrainer
    gVerdanturfBattleTentTrainers[EMERALD_FRONTIER_TENT_TRAINER_COUNT];
extern struct FacilityMon gVerdanturfBattleTentMons[EMERALD_FRONTIER_TENT_VERDANTURF_MONS];
extern struct BattleFrontierTrainer
    gFallarborBattleTentTrainers[EMERALD_FRONTIER_TENT_TRAINER_COUNT];
extern struct FacilityMon gFallarborBattleTentMons[EMERALD_FRONTIER_TENT_FALLARBOR_MONS];
#endif /* GUARD_BATTLE_TOWER_H */

/* ---- R13-E3a-2 facility AUX fill targets (native HOST_DATA). ----
 * The seam (emerald_frontier_compat.c) fills each from the pack. Engine TUs
 * `#define` their local static table name to these under NATIVE_LINUX
 * (e.g. `#define sNPCTable gBattlePikeNPC` in src/battle_pike.c), so the
 * engine's field access compiles against a layout-identical struct.
 * gBattlePyramidPickupItems is the DEDUPED shared payload bound to BOTH
 * sPickupItemsLvl50 and sPickupItemsLvlOpen (byte-identical twins). */
extern u16 gBattleFactoryMovesTotalPreparation[28];
extern u16 gBattleFactoryMovesImpossibleToPredict[15];
extern u16 gBattleFactoryMovesWeakeningTheFoe[20];
extern u16 gBattleFactoryMovesHighRiskHighReturn[27];
extern u16 gBattleFactoryMovesEndurance[28];
extern u16 gBattleFactoryMovesSlowAndSteady[33];
extern u16 gBattleFactoryMovesDependsOnTheBattlesFlow[6];
extern u16 gBattlePalaceEarlyPrizes[6];
extern u16 gBattlePalaceLatePrizes[9];
extern u16 gBattleArenaShortStreakPrizeItems[6];
extern u16 gBattleArenaLongStreakPrizeItems[9];
extern struct BattlePikeNPC gBattlePikeNPC[EMERALD_FRONTIER_AUX_PIKE_NPC_SLOTS];
extern u16 gBattlePikeSpeeches[42][6];
extern u8 gBattlePikeRoomTypeHints[9];
extern u8 gBattlePikeHeals[6][3];
extern struct BattlePikeWildMon gBattlePikeLvl50Mons1[3];
extern struct BattlePikeWildMon gBattlePikeLvl50Mons2[3];
extern struct BattlePikeWildMon gBattlePikeLvl50Mons3[3];
extern struct BattlePikeWildMon gBattlePikeLvl50Mons4[3];
extern struct BattlePikeWildMon gBattlePikeLvlOpenMons1[3];
extern struct BattlePikeWildMon gBattlePikeLvlOpenMons2[3];
extern struct BattlePikeWildMon gBattlePikeLvlOpenMons3[3];
extern struct BattlePikeWildMon gBattlePikeLvlOpenMons4[3];
extern struct BattlePyramidFloorTemplate
    gBattlePyramidFloorTemplates[EMERALD_FRONTIER_AUX_PYRAMID_FLOOR_SLOTS];
extern u8 gBattlePyramidFloorTemplateOptions[34][2];
extern u16 gBattlePyramidPickupItems[20][10];
extern u8 gBattlePyramidPickupItemSlots[63][2];
extern u16 gFrontierBrainTrainerIds[7];
extern struct BattleFrontierBrainMon
    gFrontierBrainsMons[7][2][3];
extern u8 gFrontierBrainStreakAppearances[7][4];

#endif /* EMERALD_RESOURCES_FRONTIER_DATA_NATIVE_H */
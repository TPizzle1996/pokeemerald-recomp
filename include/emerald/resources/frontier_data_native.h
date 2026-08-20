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

/* Layout-identical copies of include/battle_tower.h's structs, so the seam and
 * the definitions TU never pull global.h. A TU that already includes
 * battle_tower.h (BATTLE_TOWER_H set) uses the engine's real structs. The
 * layouts are identical (checked by _Static_assert in the definitions TU). */
#ifndef BATTLE_TOWER_H

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

#endif /* BATTLE_TOWER_H */

/* The native fill targets. Declared OUTSIDE the BATTLE_TOWER_H guard so a TU
 * that includes battle_tower.h (engine consumers) can still bind these; a TU
 * that includes this header without battle_tower.h gets the copies above.
 * battle_tower.h already declares these non-const under NATIVE_LINUX; the
 * redeclaration here is identical and legal, and gVerdanturf/gFallarbor tent
 * arrays are unique to this header. */
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

#endif /* EMERALD_RESOURCES_FRONTIER_DATA_NATIVE_H */
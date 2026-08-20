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
/* R13-E1: native trainer-data fill-target definitions.
 *
 * The single source of the runtime trainer tables on the native build.
 * Both arrays are HOST_DATA fill targets: the publication seam
 * (emerald_trainer_compat.c) fills them from the production pack at boot
 * (REFUSE-class - the compiled const definitions in src/data/trainers.h and
 * src/data/text/trainer_class_names.h are NATIVE_LINUX-guarded out of the
 * link, so there is no fallback), and engine consumers read them through
 * the extern declarations in include/data.h. The party leaves themselves
 * are NOT defined here: the seam rebuilds each trainer's party pointer
 * into its own packed party arena (trainer_data_native.h comment).
 * Native-only; compiled by Makefile_pc.
 */

#include "emerald/resources/trainer_data_native.h"

HOST_DATA struct Trainer gTrainers[EMERALD_TRAINER_COUNT];
HOST_DATA u8 gTrainerClassNames[EMERALD_TRAINER_CLASS_COUNT][13];

_Static_assert(sizeof(struct TrainerMonNoItemDefaultMoves) == 6u,
               "TrainerMonNoItemDefaultMovesSize");
_Static_assert(sizeof(struct TrainerMonNoItemCustomMoves) == 14u,
               "TrainerMonNoItemCustomMovesSize");
_Static_assert(sizeof(struct TrainerMonItemDefaultMoves) == 8u,
               "TrainerMonItemDefaultMovesSize");
_Static_assert(sizeof(struct TrainerMonItemCustomMoves) == 16u,
               "TrainerMonItemCustomMovesSize");
_Static_assert(sizeof(struct Trainer) == 48u, "TrainerWireToNativeSize");
_Static_assert(sizeof(gTrainerClassNames) == 66u * 13u,
               "TrainerClassNamesSize");
#ifndef EMERALD_RESOURCES_TRAINER_DATA_NATIVE_H
#define EMERALD_RESOURCES_TRAINER_DATA_NATIVE_H

/* R13-E1: native trainer-data fill targets.
 *
 * Declares the HOST_DATA arrays the publication seam (emerald_trainer_compat.c)
 * fills from the production pack: `struct Trainer gTrainers[855]` (48-byte
 * native rows, party pointer rebuilt to the seam's party arena) and
 * `u8 gTrainerClassNames[66][13]`. The DEFINITIONS live in
 * src/emerald/resources/trainer_data_native.c (native-only); this header is
 * included by BOTH the definitions TU and the seam TU.
 *
 * The structs here are layout-identical to the engine's definitions in
 * include/data.h (struct Trainer / union TrainerMonPtr / the four
 * TrainerMon variants), so the seam writes the exact layout engine consumers
 * read through include/data.h, without pulling global.h into these
 * global.h-free TUs. sizeof: TrainerMon no-item/default 6, no-item/custom 14,
 * item/default 8, item/custom 16; union/struct Trainer 48 (checked by
 * _Static_assert in the definitions TU).
 *
 * The native declaration of gTrainers/gTrainerClassNames in include/data.h is
 * non-const under NATIVE_LINUX (extern struct Trainer gTrainers[] / extern u8
 * gTrainerClassNames[][13]); GBA keeps the const definition verbatim in
 * src/data/trainers.h / src/data/text/trainer_class_names.h.
 */

#include "gba/types.h"
#include "gba/defines.h"
#include "constants/global.h"  /* MAX_MON_MOVES, TRAINER_NAME_LENGTH */

#ifndef MAX_TRAINER_ITEMS
#define MAX_TRAINER_ITEMS 4u  /* include/data.h (data.h-free copy) */
#endif

#define EMERALD_TRAINER_COUNT      855u  /* GAMEPLAY_NATIVE_TRAINER_COUNT */
#define EMERALD_TRAINER_CLASS_COUNT 66u  /* GAMEPLAY_NATIVE_CLASS_COUNT  */

/* A TU that already includes include/data.h (GUARD_DATA_H) provides the real
 * structs + the `extern struct Trainer gTrainers[]` / `extern u8
 * gTrainerClassNames[][13]` declarations; skip the layout-identical copies so
 * there is never a redefinition. The definitions TU + the seam TU never pull
 * data.h, so they get the copies here. The layouts are identical (checked by
 * _Static_assert in the definitions TU), so the symbol bytes match. */
#ifndef GUARD_DATA_H

/* Layout-identical to the four include/data.h TrainerMon structs. */
#define TRAINER_MON_MAX_MOVES (4u)  /* MAX_MON_MOVES */

struct TrainerMonNoItemDefaultMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
};

struct TrainerMonNoItemCustomMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
    u16 moves[TRAINER_MON_MAX_MOVES];
};

struct TrainerMonItemDefaultMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
    u16 heldItem;
};

struct TrainerMonItemCustomMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
    u16 heldItem;
    u16 moves[TRAINER_MON_MAX_MOVES];
};

union TrainerMonPtr
{
    const struct TrainerMonNoItemDefaultMoves *NoItemDefaultMoves;
    const struct TrainerMonNoItemCustomMoves *NoItemCustomMoves;
    const struct TrainerMonItemDefaultMoves *ItemDefaultMoves;
    const struct TrainerMonItemCustomMoves *ItemCustomMoves;
};

/* Layout-identical to the include/data.h struct Trainer (48 B native). */
struct Trainer
{
    u8 partyFlags;
    u8 trainerClass;
    u8 encounterMusic_gender; /* last bit is gender */
    u8 trainerPic;
    u8 trainerName[TRAINER_NAME_LENGTH + 1];
    u16 items[MAX_TRAINER_ITEMS];
    bool8 doubleBattle;
    u32 aiFlags;
    u8 partySize;
    union TrainerMonPtr party;
};

/* The native fill targets (see the header comment). */
extern struct Trainer gTrainers[EMERALD_TRAINER_COUNT];
extern u8 gTrainerClassNames[EMERALD_TRAINER_CLASS_COUNT][13];

#endif /* GUARD_DATA_H */

#endif /* EMERALD_RESOURCES_TRAINER_DATA_NATIVE_H */
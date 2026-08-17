/* R11-E/F: Harness B native-table stubs for the compat seams.
 *
 * The trainer/Pokémon compat TUs (emerald_trainer_native_compat.c,
 * emerald_pokemon_native_compat.c) publish the production snapshot's
 * streams into the REAL native tables; the production harness gets those
 * tables from emerald_trainer_native_compat_test.c, which also pulls the
 * entire R7B fixture machinery (descriptor loaders, decompress.c, bios.c,
 * its own CHECK harness). Harness B needs only the table SYMBOLS:
 *
 *   - front_pic_tables.h / back_pic_tables.h emit the REAL native table
 *     definitions (their native branches carry NULL data slots + size/tag
 *     metadata, no legacy leaf graphic refs - R7B §4/§14-A, R8);
 *   - the four Pokémon battle tables mirror the compat test's definitions
 *     (native mutable, data.h's const/non-const split, R9 §5).
 *
 * The seams publish real pack payloads into these slots; Harness B never
 * inspects the published contents (the R7B/R8/R9 production proofs already
 * pin them byte-for-byte).
 */

#include "global.h"
#include "graphics.h"
#include "sprite.h"
#include "data.h"
#include "constants/trainers.h"

#include "../src/data/trainer_graphics/front_pic_tables.h"

/* back_pic_tables.h's native branch keeps ARRAY_COUNT(frameArray) in the sheet
 * table entries, so the eight SpriteFrameImage arrays must exist as real
 * symbols. Mirror the production fixture (emerald_trainer_native_compat_test.c,
 * lines 106-155): NULL data sentinels, TRAINER_PIC_SIZE per frame -- the compat
 * seam publishes the session streams into these slots; Harness B never reads
 * them. */
#define TRAINER_BACK_FRAME(trainerPic, frameNum) {NULL, TRAINER_PIC_SIZE}
struct SpriteFrameImage gTrainerBackPicTable_Brendan[] =
{
    TRAINER_BACK_FRAME(Brendan, 0), TRAINER_BACK_FRAME(Brendan, 1),
    TRAINER_BACK_FRAME(Brendan, 2), TRAINER_BACK_FRAME(Brendan, 3),
};
struct SpriteFrameImage gTrainerBackPicTable_May[] =
{
    TRAINER_BACK_FRAME(May, 0), TRAINER_BACK_FRAME(May, 1),
    TRAINER_BACK_FRAME(May, 2), TRAINER_BACK_FRAME(May, 3),
};
struct SpriteFrameImage gTrainerBackPicTable_Red[] =
{
    TRAINER_BACK_FRAME(Red, 0), TRAINER_BACK_FRAME(Red, 1),
    TRAINER_BACK_FRAME(Red, 2), TRAINER_BACK_FRAME(Red, 3),
    TRAINER_BACK_FRAME(Red, 4),
};
struct SpriteFrameImage gTrainerBackPicTable_Leaf[] =
{
    TRAINER_BACK_FRAME(Leaf, 0), TRAINER_BACK_FRAME(Leaf, 1),
    TRAINER_BACK_FRAME(Leaf, 2), TRAINER_BACK_FRAME(Leaf, 3),
    TRAINER_BACK_FRAME(Leaf, 4),
};
struct SpriteFrameImage gTrainerBackPicTable_RubySapphireBrendan[] =
{
    TRAINER_BACK_FRAME(RubySapphireBrendan, 0), TRAINER_BACK_FRAME(RubySapphireBrendan, 1),
    TRAINER_BACK_FRAME(RubySapphireBrendan, 2), TRAINER_BACK_FRAME(RubySapphireBrendan, 3),
};
struct SpriteFrameImage gTrainerBackPicTable_RubySapphireMay[] =
{
    TRAINER_BACK_FRAME(RubySapphireMay, 0), TRAINER_BACK_FRAME(RubySapphireMay, 1),
    TRAINER_BACK_FRAME(RubySapphireMay, 2), TRAINER_BACK_FRAME(RubySapphireMay, 3),
};
struct SpriteFrameImage gTrainerBackPicTable_Wally[] =
{
    TRAINER_BACK_FRAME(Wally, 0), TRAINER_BACK_FRAME(Wally, 1),
    TRAINER_BACK_FRAME(Wally, 2), TRAINER_BACK_FRAME(Wally, 3),
};
struct SpriteFrameImage gTrainerBackPicTable_Steven[] =
{
    TRAINER_BACK_FRAME(Steven, 0), TRAINER_BACK_FRAME(Steven, 1),
    TRAINER_BACK_FRAME(Steven, 2), TRAINER_BACK_FRAME(Steven, 3),
};

#include "../src/data/trainer_graphics/back_pic_tables.h"

#if defined(NATIVE_LINUX)
/* R9 §5: on native all four Pokémon battle tables are mutable - the compat
 * seam publishes the ROM_BASE session streams into their .data slots. Sized
 * generously (SPECIES_CELEBI = 251) so the decompress.c special-poke loops
 * never index past the stub; the slot layout is pinned by the generated
 * pokemon_battle_slots map. */
struct CompressedSpriteSheet gMonFrontPicTable[512] = { 0 };
struct CompressedSpriteSheet gMonBackPicTable[512] = { 0 };
struct CompressedSpritePalette gMonPaletteTable[512] = { 0 };
struct CompressedSpritePalette gMonShinyPaletteTable[512] = { 0 };
#endif

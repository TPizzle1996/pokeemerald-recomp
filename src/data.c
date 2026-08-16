#include "global.h"
#include "malloc.h"
#include "battle.h"
#include "data.h"
#include "graphics.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/trainers.h"
#include "constants/battle_ai.h"

const u16 gMinigameDigits_Pal[] = INCBIN_U16("graphics/link/minigame_digits.gbapal");
const u32 gMinigameDigits_Gfx[] = INCBIN_U32("graphics/link/minigame_digits.4bpp.lz");
static const u32 sMinigameDigitsThin_Gfx[] = INCBIN_U32("graphics/link/minigame_digits2.4bpp.lz"); // Unused

#define BATTLER_OFFSET(i) (gHeap + 0x8000 + MON_PIC_SIZE * (i))

const struct SpriteFrameImage gBattlerPicTable_PlayerLeft[] =
{
    {BATTLER_OFFSET(0), MON_PIC_SIZE},
    {BATTLER_OFFSET(1), MON_PIC_SIZE},
    {BATTLER_OFFSET(2), MON_PIC_SIZE},
    {BATTLER_OFFSET(3), MON_PIC_SIZE},
};

const struct SpriteFrameImage gBattlerPicTable_OpponentLeft[] =
{
    {BATTLER_OFFSET(4), MON_PIC_SIZE},
    {BATTLER_OFFSET(5), MON_PIC_SIZE},
    {BATTLER_OFFSET(6), MON_PIC_SIZE},
    {BATTLER_OFFSET(7), MON_PIC_SIZE},
};

const struct SpriteFrameImage gBattlerPicTable_PlayerRight[] =
{
    {BATTLER_OFFSET(8),  MON_PIC_SIZE},
    {BATTLER_OFFSET(9),  MON_PIC_SIZE},
    {BATTLER_OFFSET(10), MON_PIC_SIZE},
    {BATTLER_OFFSET(11), MON_PIC_SIZE},
};

const struct SpriteFrameImage gBattlerPicTable_OpponentRight[] =
{
    {BATTLER_OFFSET(12), MON_PIC_SIZE},
    {BATTLER_OFFSET(13), MON_PIC_SIZE},
    {BATTLER_OFFSET(14), MON_PIC_SIZE},
    {BATTLER_OFFSET(15), MON_PIC_SIZE},
};

/* R8: on native the frame tables are the published pixel surface: NULL
 * sentinels (payloads are ROM_BASE-only), repointed by the compat seam to the
 * session raw sheet + per-frame offsets. The GBA build keeps the compiled
 * payload pointers and constness unchanged. */
#if defined(NATIVE_LINUX)
#define TRAINER_BACK_FRAME(trainerPic, frameNum) {NULL, TRAINER_PIC_SIZE}
struct SpriteFrameImage gTrainerBackPicTable_Brendan[] =
#else
#define TRAINER_BACK_FRAME(trainerPic, frameNum) {gTrainerBackPic_##trainerPic + TRAINER_PIC_SIZE * frameNum, TRAINER_PIC_SIZE}
const struct SpriteFrameImage gTrainerBackPicTable_Brendan[] =
#endif
{
    TRAINER_BACK_FRAME(Brendan, 0),
    TRAINER_BACK_FRAME(Brendan, 1),
    TRAINER_BACK_FRAME(Brendan, 2),
    TRAINER_BACK_FRAME(Brendan, 3),
};

#if defined(NATIVE_LINUX)
struct SpriteFrameImage gTrainerBackPicTable_May[] =
#else
const struct SpriteFrameImage gTrainerBackPicTable_May[] =
#endif
{
    TRAINER_BACK_FRAME(May, 0),
    TRAINER_BACK_FRAME(May, 1),
    TRAINER_BACK_FRAME(May, 2),
    TRAINER_BACK_FRAME(May, 3),
};

#if defined(NATIVE_LINUX)
struct SpriteFrameImage gTrainerBackPicTable_Red[] =
#else
const struct SpriteFrameImage gTrainerBackPicTable_Red[] =
#endif
{
    TRAINER_BACK_FRAME(Red, 0),
    TRAINER_BACK_FRAME(Red, 1),
    TRAINER_BACK_FRAME(Red, 2),
    TRAINER_BACK_FRAME(Red, 3),
    TRAINER_BACK_FRAME(Red, 4),
};

#if defined(NATIVE_LINUX)
struct SpriteFrameImage gTrainerBackPicTable_Leaf[] =
#else
const struct SpriteFrameImage gTrainerBackPicTable_Leaf[] =
#endif
{
    TRAINER_BACK_FRAME(Leaf, 0),
    TRAINER_BACK_FRAME(Leaf, 1),
    TRAINER_BACK_FRAME(Leaf, 2),
    TRAINER_BACK_FRAME(Leaf, 3),
    TRAINER_BACK_FRAME(Leaf, 4),
};

#if defined(NATIVE_LINUX)
struct SpriteFrameImage gTrainerBackPicTable_RubySapphireBrendan[] =
#else
const struct SpriteFrameImage gTrainerBackPicTable_RubySapphireBrendan[] =
#endif
{
    TRAINER_BACK_FRAME(RubySapphireBrendan, 0),
    TRAINER_BACK_FRAME(RubySapphireBrendan, 1),
    TRAINER_BACK_FRAME(RubySapphireBrendan, 2),
    TRAINER_BACK_FRAME(RubySapphireBrendan, 3),
};

#if defined(NATIVE_LINUX)
struct SpriteFrameImage gTrainerBackPicTable_RubySapphireMay[] =
#else
const struct SpriteFrameImage gTrainerBackPicTable_RubySapphireMay[] =
#endif
{
    TRAINER_BACK_FRAME(RubySapphireMay, 0),
    TRAINER_BACK_FRAME(RubySapphireMay, 1),
    TRAINER_BACK_FRAME(RubySapphireMay, 2),
    TRAINER_BACK_FRAME(RubySapphireMay, 3),
};

#if defined(NATIVE_LINUX)
struct SpriteFrameImage gTrainerBackPicTable_Wally[] =
#else
const struct SpriteFrameImage gTrainerBackPicTable_Wally[] =
#endif
{
    TRAINER_BACK_FRAME(Wally, 0),
    TRAINER_BACK_FRAME(Wally, 1),
    TRAINER_BACK_FRAME(Wally, 2),
    TRAINER_BACK_FRAME(Wally, 3),
};

#if defined(NATIVE_LINUX)
struct SpriteFrameImage gTrainerBackPicTable_Steven[] =
#else
const struct SpriteFrameImage gTrainerBackPicTable_Steven[] =
#endif
{
    TRAINER_BACK_FRAME(Steven, 0),
    TRAINER_BACK_FRAME(Steven, 1),
    TRAINER_BACK_FRAME(Steven, 2),
    TRAINER_BACK_FRAME(Steven, 3),
};

static const union AnimCmd sAnim_GeneralFrame0[] =
{
    ANIMCMD_FRAME(0, 0),
    ANIMCMD_END,
};

static const union AnimCmd sAnim_GeneralFrame3[] =
{
    ANIMCMD_FRAME(3, 0),
    ANIMCMD_END,
};

// Many of these affine anims seem to go unused, and
// instead SetSpriteRotScale is used to manipulate
// the battler sprites directly (for instance, in AnimTask_SwitchOutShrinkMon).
// Those with explicit indexes are referenced elsewhere.

static const union AffineAnimCmd sAffineAnim_Battler_Normal[] =
{
    AFFINEANIMCMD_FRAME(0x100, 0x100, 0, 0),
    AFFINEANIMCMD_END,
};

static const union AffineAnimCmd sAffineAnim_Battler_Flipped[] =
{
    AFFINEANIMCMD_FRAME(-0x100, 0x100, 0, 0),
    AFFINEANIMCMD_END,
};

static const union AffineAnimCmd sAffineAnim_Battler_Emerge[] =
{
    AFFINEANIMCMD_FRAME(0x28, 0x28, 0, 0),
    AFFINEANIMCMD_FRAME(0x12, 0x12, 0, 12),
    AFFINEANIMCMD_END,
};

static const union AffineAnimCmd sAffineAnim_Battler_Return[] =
{
    AFFINEANIMCMD_FRAME( -0x2,  -0x2, 0, 18),
    AFFINEANIMCMD_FRAME(-0x10, -0x10, 0, 15),
    AFFINEANIMCMD_END,
};

static const union AffineAnimCmd sAffineAnim_Battler_HorizontalSquishLoop[] =
{
    AFFINEANIMCMD_FRAME(0xA0, 0x100, 0, 0),
    AFFINEANIMCMD_FRAME( 0x4,   0x0, 0, 8),
    AFFINEANIMCMD_FRAME(-0x4,   0x0, 0, 8),
    AFFINEANIMCMD_JUMP(1),
};

static const union AffineAnimCmd sAffineAnim_Battler_Grow[] =
{
    AFFINEANIMCMD_FRAME(0x2, 0x2, 0, 20),
    AFFINEANIMCMD_END,
};

static const union AffineAnimCmd sAffineAnim_Battler_Shrink[] =
{
    AFFINEANIMCMD_FRAME(-0x2, -0x2, 0, 20),
    AFFINEANIMCMD_END,
};

static const union AffineAnimCmd sAffineAnim_Battler_BigToSmall[] =
{
    AFFINEANIMCMD_FRAME(0x100, 0x100, 0, 0),
    AFFINEANIMCMD_FRAME(-0x10, -0x10, 0, 9),
    AFFINEANIMCMD_END,
};

static const union AffineAnimCmd sAffineAnim_Battler_GrowLarge[] =
{
    AFFINEANIMCMD_FRAME(0x4, 0x4, 0, 63),
    AFFINEANIMCMD_END,
};

static const union AffineAnimCmd sAffineAnim_Battler_TipRight[] =
{
    AFFINEANIMCMD_FRAME(0x0, 0x0, -3, 5),
    AFFINEANIMCMD_FRAME(0x0, 0x0,  3, 5),
    AFFINEANIMCMD_END,
};

const union AffineAnimCmd *const gAffineAnims_BattleSpritePlayerSide[] =
{
    [BATTLER_AFFINE_NORMAL] = sAffineAnim_Battler_Normal,
    [BATTLER_AFFINE_EMERGE] = sAffineAnim_Battler_Emerge,
    [BATTLER_AFFINE_RETURN] = sAffineAnim_Battler_Return,
    sAffineAnim_Battler_HorizontalSquishLoop,
    sAffineAnim_Battler_Grow,
    sAffineAnim_Battler_Shrink,
    sAffineAnim_Battler_GrowLarge,
    sAffineAnim_Battler_TipRight,
    sAffineAnim_Battler_BigToSmall,
};

static const union AffineAnimCmd sAffineAnim_Battler_SpinShrink[] =
{
    AFFINEANIMCMD_FRAME(-0x4, -0x4, 4, 63),
    AFFINEANIMCMD_END,
};

static const union AffineAnimCmd sAffineAnim_Battler_TipLeft[] =
{
    AFFINEANIMCMD_FRAME(0x0, 0x0,  3, 5),
    AFFINEANIMCMD_FRAME(0x0, 0x0, -3, 5),
    AFFINEANIMCMD_END,
};

static const union AffineAnimCmd sAffineAnim_Battler_RotateUpAndBack[] =
{
    AFFINEANIMCMD_FRAME(0x0, 0x0, -5, 20),
    AFFINEANIMCMD_FRAME(0x0, 0x0,  0, 20),
    AFFINEANIMCMD_FRAME(0x0, 0x0,  5, 20),
    AFFINEANIMCMD_END,
};

static const union AffineAnimCmd sAffineAnim_Battler_Spin[] =
{
    AFFINEANIMCMD_FRAME(0x0, 0x0, 9, 110),
    AFFINEANIMCMD_END,
};

const union AffineAnimCmd *const gAffineAnims_BattleSpriteOpponentSide[] =
{
    [BATTLER_AFFINE_NORMAL] = sAffineAnim_Battler_Normal,
    [BATTLER_AFFINE_EMERGE] = sAffineAnim_Battler_Emerge,
    [BATTLER_AFFINE_RETURN] = sAffineAnim_Battler_Return,
    sAffineAnim_Battler_HorizontalSquishLoop,
    sAffineAnim_Battler_Grow,
    sAffineAnim_Battler_Shrink,
    sAffineAnim_Battler_SpinShrink,
    sAffineAnim_Battler_TipLeft,
    sAffineAnim_Battler_RotateUpAndBack,
    sAffineAnim_Battler_BigToSmall,
    sAffineAnim_Battler_Spin,
};

const union AffineAnimCmd *const gAffineAnims_BattleSpriteContest[] =
{
    [BATTLER_AFFINE_NORMAL] = sAffineAnim_Battler_Flipped,
    [BATTLER_AFFINE_EMERGE] = sAffineAnim_Battler_Emerge,
    [BATTLER_AFFINE_RETURN] = sAffineAnim_Battler_Return,
    sAffineAnim_Battler_HorizontalSquishLoop,
    sAffineAnim_Battler_Grow,
    sAffineAnim_Battler_Shrink,
    sAffineAnim_Battler_SpinShrink,
    sAffineAnim_Battler_TipLeft,
    sAffineAnim_Battler_RotateUpAndBack,
    sAffineAnim_Battler_BigToSmall,
    sAffineAnim_Battler_Spin,
};


static const union AnimCmd sAnim_MonPic_0[] =
{
    ANIMCMD_FRAME(0, 0),
    ANIMCMD_END,
};

static const union AnimCmd sAnim_MonPic_1[] =
{
    ANIMCMD_FRAME(1, 0),
    ANIMCMD_END,
};

static const union AnimCmd sAnim_MonPic_2[] =
{
    ANIMCMD_FRAME(2, 0),
    ANIMCMD_END,
};

static const union AnimCmd sAnim_MonPic_3[] =
{
    ANIMCMD_FRAME(3, 0),
    ANIMCMD_END,
};

const union AnimCmd *const gAnims_MonPic[MAX_MON_PIC_FRAMES] =
{
    sAnim_MonPic_0,
    sAnim_MonPic_1,
    sAnim_MonPic_2,
    sAnim_MonPic_3,
};

#define SPECIES_SPRITE(species, sprite) [SPECIES_##species] = {sprite, MON_PIC_SIZE, SPECIES_##species}
#define SPECIES_PAL(species, pal) [SPECIES_##species] = {pal, SPECIES_##species}
#define SPECIES_SHINY_PAL(species, pal) [SPECIES_##species] = {pal, SPECIES_##species + SPECIES_SHINY_TAG}

/* R9 §7: the four Pokémon battle tables are ROM_BASE-migrated on native - the
 * compiled leaf payloads are gone from the native link (GBA-only TUs
 * anim_mon_front_pics.c + pokemon_battle_payload.c), so their rows expand to
 * NULL sentinels and the compat seam publishes the session pointers at init.
 * The `sprite` argument is intentionally unused on native; sizes and tags are
 * retained exactly. GBA/Windows keep the compile-time assets. The still-front
 * table and the external back-EGG row use the ORIGINAL macros above and stay
 * compiled on every target. */
#if defined(NATIVE_LINUX)
#define SPECIES_BATTLE_SPRITE(species, sprite) [SPECIES_##species] = {NULL, MON_PIC_SIZE, SPECIES_##species}
#define SPECIES_BATTLE_PAL(species, pal) [SPECIES_##species] = {NULL, SPECIES_##species}
#define SPECIES_BATTLE_SHINY_PAL(species, pal) [SPECIES_##species] = {NULL, SPECIES_##species + SPECIES_SHINY_TAG}
#else
#define SPECIES_BATTLE_SPRITE(species, sprite) [SPECIES_##species] = {sprite, MON_PIC_SIZE, SPECIES_##species}
#define SPECIES_BATTLE_PAL(species, pal) [SPECIES_##species] = {pal, SPECIES_##species}
#define SPECIES_BATTLE_SHINY_PAL(species, pal) [SPECIES_##species] = {pal, SPECIES_##species + SPECIES_SHINY_TAG}
#endif

#include "data/pokemon_graphics/unused_anims.h"
#include "data/pokemon_graphics/front_pic_coordinates.h"
#include "data/pokemon_graphics/still_front_pic_table.h"
#include "data/pokemon_graphics/back_pic_coordinates.h"

#include "data/pokemon_graphics/back_pic_table.h"
#include "data/pokemon_graphics/palette_table.h"
#include "data/pokemon_graphics/shiny_palette_table.h"

#include "data/trainer_graphics/front_pic_anims.h"
#include "data/trainer_graphics/front_pic_tables.h"
#include "data/trainer_graphics/back_pic_anims.h"
#include "data/trainer_graphics/back_pic_tables.h"

#include "data/pokemon_graphics/enemy_mon_elevation.h"
#include "data/pokemon_graphics/front_pic_anims.h"
#include "data/pokemon_graphics/front_pic_table.h"
#include "data/pokemon_graphics/unknown_table.h"

#include "data/trainer_parties.h"
#include "data/text/trainer_class_names.h"
#include "data/trainers.h"
#include "data/text/species_names.h"
#include "data/text/move_names.h"

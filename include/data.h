#ifndef GUARD_DATA_H
#define GUARD_DATA_H

#include "constants/moves.h"

#define SPECIES_SHINY_TAG 500

#define MAX_TRAINER_ITEMS 4

#define TRAINER_PIC_WIDTH 64
#define TRAINER_PIC_HEIGHT 64
#define TRAINER_PIC_SIZE (TRAINER_PIC_WIDTH * TRAINER_PIC_HEIGHT / 2)

// Red and Leaf's back pics have 5 frames, but this is presumably irrelevant in the places this is used.
#define MAX_TRAINER_PIC_FRAMES 4

enum {
    BATTLER_AFFINE_NORMAL,
    BATTLER_AFFINE_EMERGE,
    BATTLER_AFFINE_RETURN,
};

struct MonCoords
{
    // This would use a bitfield, but some function
    // uses it as a u8 and casting won't match.
    u8 size; // u8 width:4, height:4;
    u8 y_offset;
};

#define MON_COORDS_SIZE(width, height) (DIV_ROUND_UP(width, 8) << 4 | DIV_ROUND_UP(height, 8))
#define GET_MON_COORDS_WIDTH(size) ((size >> 4) * 8)
#define GET_MON_COORDS_HEIGHT(size) ((size & 0xF) * 8)

struct TrainerMonNoItemDefaultMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
};

struct TrainerMonItemDefaultMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
    u16 heldItem;
};

struct TrainerMonNoItemCustomMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
    u16 moves[MAX_MON_MOVES];
};

struct TrainerMonItemCustomMoves
{
    u16 iv;
    u8 lvl;
    u16 species;
    u16 heldItem;
    u16 moves[MAX_MON_MOVES];
};

#define NO_ITEM_DEFAULT_MOVES(party) { .NoItemDefaultMoves = party }, .partySize = ARRAY_COUNT(party), .partyFlags = 0
#define NO_ITEM_CUSTOM_MOVES(party) { .NoItemCustomMoves = party }, .partySize = ARRAY_COUNT(party), .partyFlags = F_TRAINER_PARTY_CUSTOM_MOVESET
#define ITEM_DEFAULT_MOVES(party) { .ItemDefaultMoves = party }, .partySize = ARRAY_COUNT(party), .partyFlags = F_TRAINER_PARTY_HELD_ITEM
#define ITEM_CUSTOM_MOVES(party) { .ItemCustomMoves = party }, .partySize = ARRAY_COUNT(party), .partyFlags = F_TRAINER_PARTY_CUSTOM_MOVESET | F_TRAINER_PARTY_HELD_ITEM

union TrainerMonPtr
{
    const struct TrainerMonNoItemDefaultMoves *NoItemDefaultMoves;
    const struct TrainerMonNoItemCustomMoves *NoItemCustomMoves;
    const struct TrainerMonItemDefaultMoves *ItemDefaultMoves;
    const struct TrainerMonItemCustomMoves *ItemCustomMoves;
};

struct Trainer
{
    /*0x00*/ u8 partyFlags;
    /*0x01*/ u8 trainerClass;
    /*0x02*/ u8 encounterMusic_gender; // last bit is gender
    /*0x03*/ u8 trainerPic;
    /*0x04*/ u8 trainerName[TRAINER_NAME_LENGTH + 1];
    /*0x10*/ u16 items[MAX_TRAINER_ITEMS];
    /*0x18*/ bool8 doubleBattle;
    /*0x1C*/ u32 aiFlags;
    /*0x20*/ u8 partySize;
    /*0x24*/ union TrainerMonPtr party;
};

#define TRAINER_ENCOUNTER_MUSIC(trainer) ((gTrainers[trainer].encounterMusic_gender & 0x7F))

extern const u16 gMinigameDigits_Pal[];
extern const u32 gMinigameDigits_Gfx[];

extern const struct SpriteFrameImage gBattlerPicTable_PlayerLeft[];
extern const struct SpriteFrameImage gBattlerPicTable_OpponentLeft[];
extern const struct SpriteFrameImage gBattlerPicTable_PlayerRight[];
extern const struct SpriteFrameImage gBattlerPicTable_OpponentRight[];
#if defined(NATIVE_LINUX)
/* R8: native back-pic frame tables are non-const so the compat seam can
 * publish the ROM_BASE raw sheet streams into the 34 SpriteFrameImage slots
 * (the live pixel surface: sTrainerBackSpriteTemplates -> CreateSprite ->
 * RequestSpriteFrameImageCopy reads images[frame].data every frame). */
extern struct SpriteFrameImage gTrainerBackPicTable_Brendan[];
extern struct SpriteFrameImage gTrainerBackPicTable_May[];
extern struct SpriteFrameImage gTrainerBackPicTable_Red[];
extern struct SpriteFrameImage gTrainerBackPicTable_Leaf[];
extern struct SpriteFrameImage gTrainerBackPicTable_RubySapphireBrendan[];
extern struct SpriteFrameImage gTrainerBackPicTable_RubySapphireMay[];
extern struct SpriteFrameImage gTrainerBackPicTable_Wally[];
extern struct SpriteFrameImage gTrainerBackPicTable_Steven[];
#else
extern const struct SpriteFrameImage gTrainerBackPicTable_Brendan[];
extern const struct SpriteFrameImage gTrainerBackPicTable_May[];
extern const struct SpriteFrameImage gTrainerBackPicTable_Red[];
extern const struct SpriteFrameImage gTrainerBackPicTable_Leaf[];
extern const struct SpriteFrameImage gTrainerBackPicTable_RubySapphireBrendan[];
extern const struct SpriteFrameImage gTrainerBackPicTable_RubySapphireMay[];
extern const struct SpriteFrameImage gTrainerBackPicTable_Wally[];
extern const struct SpriteFrameImage gTrainerBackPicTable_Steven[];
#endif

extern const union AffineAnimCmd *const gAffineAnims_BattleSpritePlayerSide[];
extern const union AffineAnimCmd *const gAffineAnims_BattleSpriteOpponentSide[];
extern const union AffineAnimCmd *const gAffineAnims_BattleSpriteContest[];

extern const union AnimCmd *const gAnims_MonPic[];
extern const struct MonCoords gMonFrontPicCoords[];
extern const struct CompressedSpriteSheet gMonStillFrontPicTable[];
extern const struct MonCoords gMonBackPicCoords[];
extern const struct CompressedSpriteSheet gMonBackPicTable[];
extern const struct CompressedSpritePalette gMonPaletteTable[];
extern const struct CompressedSpritePalette gMonShinyPaletteTable[];
extern const union AnimCmd *const *const gTrainerFrontAnimsPtrTable[];
extern const struct MonCoords gTrainerFrontPicCoords[];
#if defined(NATIVE_LINUX)
extern struct CompressedSpriteSheet gTrainerFrontPicTable[];
extern struct CompressedSpritePalette gTrainerFrontPicPaletteTable[];
#else
extern const struct CompressedSpriteSheet gTrainerFrontPicTable[];
extern const struct CompressedSpritePalette gTrainerFrontPicPaletteTable[];
#endif
extern const union AnimCmd *const *const gTrainerBackAnimsPtrTable[];
extern const struct MonCoords gTrainerBackPicCoords[];
/* The sheet table's decompression OUTPUT is functionally unused (raw sheets,
 * output overwritten later), but DecompressTrainerBackPic dereferences its
 * data pointers every battle, so R8 makes it ROM_BASE-backed on native like
 * the rest of the back family. */
#if defined(NATIVE_LINUX)
extern struct CompressedSpriteSheet gTrainerBackPicTable[];
#else
extern const struct CompressedSpriteSheet gTrainerBackPicTable[];
#endif
#if defined(NATIVE_LINUX)
/* R6: native back-pic palette table is non-const so the compat seam can publish
 * the ROM_BASE normal-palette image into the BRENDAN slot after state load. */
extern struct CompressedSpritePalette gTrainerBackPicPaletteTable[];
#else
extern const struct CompressedSpritePalette gTrainerBackPicPaletteTable[];
#endif

extern const u8 gEnemyMonElevation[NUM_SPECIES];

extern const union AnimCmd *const *const gMonFrontAnimsPtrTable[];
extern const struct CompressedSpriteSheet gMonFrontPicTable[];

extern const struct Trainer gTrainers[];
extern const u8 gTrainerClassNames[][13];
#ifdef DESKTOP_EXTERNAL_GAME_CONTENT
extern u8 gSpeciesNames[][POKEMON_NAME_LENGTH + 1];
extern u8 gMoveNames[MOVES_COUNT][MOVE_NAME_LENGTH + 1];
#else
extern const u8 gSpeciesNames[][POKEMON_NAME_LENGTH + 1];
extern const u8 gMoveNames[MOVES_COUNT][MOVE_NAME_LENGTH + 1];
#endif

#endif // GUARD_DATA_H

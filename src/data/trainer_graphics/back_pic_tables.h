const struct MonCoords gTrainerBackPicCoords[] =
{
    [TRAINER_BACK_PIC_BRENDAN] = {.size = 8, .y_offset = 4},
    [TRAINER_BACK_PIC_MAY] = {.size = 8, .y_offset = 4},
    [TRAINER_BACK_PIC_RED] = {.size = 8, .y_offset = 5},
    [TRAINER_BACK_PIC_LEAF] = {.size = 8, .y_offset = 5},
    [TRAINER_BACK_PIC_RUBY_SAPPHIRE_BRENDAN] = {.size = 8, .y_offset = 4},
    [TRAINER_BACK_PIC_RUBY_SAPPHIRE_MAY] = {.size = 8, .y_offset = 4},
    [TRAINER_BACK_PIC_WALLY] = {.size = 8, .y_offset = 4},
    [TRAINER_BACK_PIC_STEVEN] = {.size = 8, .y_offset = 4},
};

// This table's output is functionally unused: none of these pics are
// compressed (they are raw 4bpp sheets), so the LZ77 decode in
// DecompressTrainerBackPic reads the table's data pointer every battle but
// the garbage it produces gets overwritten later anyway. The READ is live
// though, so on native the table is ROM_BASE-backed (R8): NULL sentinels,
// published by the compat seam; the casts are so they'll play nice with the
// strict struct definition.
#if defined(NATIVE_LINUX)
#define TRAINER_BACK_SHEET(trainerPic, sprite, frameArray) [TRAINER_BACK_PIC_##trainerPic] = {NULL, TRAINER_PIC_SIZE * ARRAY_COUNT(frameArray), TRAINER_BACK_PIC_##trainerPic}
struct CompressedSpriteSheet gTrainerBackPicTable[] =
#else
#define TRAINER_BACK_SHEET(trainerPic, sprite, frameArray) [TRAINER_BACK_PIC_##trainerPic] = {(const u32 *)sprite, TRAINER_PIC_SIZE * ARRAY_COUNT(frameArray), TRAINER_BACK_PIC_##trainerPic}
const struct CompressedSpriteSheet gTrainerBackPicTable[] =
#endif
{
    TRAINER_BACK_SHEET(BRENDAN, gTrainerBackPic_Brendan, gTrainerBackPicTable_Brendan),
    TRAINER_BACK_SHEET(MAY, gTrainerBackPic_May, gTrainerBackPicTable_May),
    TRAINER_BACK_SHEET(RED, gTrainerBackPic_Red, gTrainerBackPicTable_Red),
    TRAINER_BACK_SHEET(LEAF, gTrainerBackPic_Leaf, gTrainerBackPicTable_Leaf),
    TRAINER_BACK_SHEET(RUBY_SAPPHIRE_BRENDAN, gTrainerBackPic_RubySapphireBrendan, gTrainerBackPicTable_RubySapphireBrendan),
    TRAINER_BACK_SHEET(RUBY_SAPPHIRE_MAY, gTrainerBackPic_RubySapphireMay, gTrainerBackPicTable_RubySapphireMay),
    TRAINER_BACK_SHEET(WALLY, gTrainerBackPic_Wally, gTrainerBackPicTable_Wally),
    TRAINER_BACK_SHEET(STEVEN, gTrainerBackPic_Steven, gTrainerBackPicTable_Steven),
};

#define TRAINER_BACK_PAL(trainerPic, pal) [TRAINER_BACK_PIC_##trainerPic] = {pal, TRAINER_BACK_PIC_##trainerPic}

#if defined(NATIVE_LINUX)
/* R7B §4 / R8 §4: every back-pic palette slot is ROM_BASE-only on native:
 * the six shared slots consume the FRONT normal-palette resources (R7B), the
 * Red/Leaf slots consume the BACK-only palette resources (R8). All eight are
 * NULL sentinels, published by the compat seam. The `pal` argument is
 * intentionally unused here. */
#define TRAINER_BACK_PAL_SHARED(trainerPic, pal) [TRAINER_BACK_PIC_##trainerPic] = {NULL, TRAINER_BACK_PIC_##trainerPic}
#define TRAINER_BACK_PAL_BACK_ONLY(trainerPic, pal) [TRAINER_BACK_PIC_##trainerPic] = {NULL, TRAINER_BACK_PIC_##trainerPic}
#else
#define TRAINER_BACK_PAL_SHARED(trainerPic, pal) [TRAINER_BACK_PIC_##trainerPic] = {pal, TRAINER_BACK_PIC_##trainerPic}
#define TRAINER_BACK_PAL_BACK_ONLY(trainerPic, pal) [TRAINER_BACK_PIC_##trainerPic] = {pal, TRAINER_BACK_PIC_##trainerPic}
#endif

/* The GBA build keeps the table const (compile-time assets); the native build
 * makes it non-const so the compat seam can publish the session streams into
 * all eight slots (live consumer: DecompressTrainerBackPic reads
 * gTrainerBackPicPaletteTable[backPicId].data via LoadCompressedPalette for
 * every trainer back pic, in battle_controller_player.c by gender plus wally/
 * steven/link/recorded battlers by pic id). The back-pic SHEET table above is
 * published too (R8): its data pointer is dereferenced by the same live
 * DecompressTrainerBackPic. */
#if defined(NATIVE_LINUX)
struct CompressedSpritePalette gTrainerBackPicPaletteTable[] =
#else
const struct CompressedSpritePalette gTrainerBackPicPaletteTable[] =
#endif
{
    TRAINER_BACK_PAL_SHARED(BRENDAN, gTrainerPalette_Brendan),
    TRAINER_BACK_PAL_SHARED(MAY, gTrainerPalette_May),
    TRAINER_BACK_PAL_BACK_ONLY(RED, gTrainerBackPicPalette_Red),
    TRAINER_BACK_PAL_BACK_ONLY(LEAF, gTrainerBackPicPalette_Leaf),
    TRAINER_BACK_PAL_SHARED(RUBY_SAPPHIRE_BRENDAN, gTrainerPalette_RubySapphireBrendan),
    TRAINER_BACK_PAL_SHARED(RUBY_SAPPHIRE_MAY, gTrainerPalette_RubySapphireMay),
    TRAINER_BACK_PAL_SHARED(WALLY, gTrainerPalette_Wally),
    TRAINER_BACK_PAL_SHARED(STEVEN, gTrainerPalette_Steven),
};

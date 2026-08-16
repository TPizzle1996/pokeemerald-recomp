/* GBA-only TU: every trainer-back compiled payload (R8 §5).
 *
 * The native target (Makefile_pc, NATIVE_LINUX=1) excludes this TU from its
 * C_SRCS so that no trainer-back leaf symbol or embedded byte enters the
 * native object/link graph - the whole back family is ROM_BASE-only on native
 * (8 sheets + 2 Red/Leaf back palettes, published by the compatibility seam);
 * the GBA Makefile auto-collects it via its C_SRCS wildcard, keeping the GBA
 * build's compiled payloads and table pointers exactly as before (R7A/R8
 * ownership: GBA COMPILED).
 *
 * This file is the mechanical extraction of the back-payload lines that used
 * to live in data/graphics/trainers.h, in table-index order (BRENDAN .. STEVEN,
 * then the Red/Leaf back-only palettes), TRAINER_BACK_PIC_* order. Do not
 * hand-edit the payload lines: they mirror the back-family descriptor's
 * source artifacts.
 */

#include "global.h"

const u8 gTrainerBackPic_Brendan[] = INCBIN_U8("graphics/trainers/back_pics/brendan.4bpp");
const u8 gTrainerBackPic_May[] = INCBIN_U8("graphics/trainers/back_pics/may.4bpp");
const u8 gTrainerBackPic_Red[] = INCBIN_U8("graphics/trainers/back_pics/red.4bpp");
const u8 gTrainerBackPic_Leaf[] = INCBIN_U8("graphics/trainers/back_pics/leaf.4bpp");
const u8 gTrainerBackPic_RubySapphireBrendan[] = INCBIN_U8("graphics/trainers/back_pics/brendan_rs.4bpp");
const u8 gTrainerBackPic_RubySapphireMay[] = INCBIN_U8("graphics/trainers/back_pics/may_rs.4bpp");
const u8 gTrainerBackPic_Wally[] = INCBIN_U8("graphics/trainers/back_pics/wally.4bpp");
const u8 gTrainerBackPic_Steven[] = INCBIN_U8("graphics/trainers/back_pics/steven.4bpp");
const u32 gTrainerBackPicPalette_Red[] = INCBIN_U32("graphics/trainers/back_pics/red.gbapal.lz");
const u32 gTrainerBackPicPalette_Leaf[] = INCBIN_U32("graphics/trainers/back_pics/leaf.gbapal.lz");

/*
 * Stage 3B real-oracle harness: native OBJ sampler vs the ACTUAL gba_easy_draw.c
 * DrawFrame OBJ oracle.
 *
 * The module unit test (native_obj_renderer_unit.c) validates the sampler
 * against an independent re-implementation of the GBA OBJ spec. A hand-port can
 * drift, so this harness compiles the production gba_easy_draw.c (its own TU,
 * exactly like the real build) and drives its OBJ oracle seam
 * (gParityOBJLayers): the snapshot under test is PUBLISHED into the live
 * OAM / OBJ VRAM (VRAM_+0x10000) / OBJ palette (PLTT+0x200) / DISPCNT / BLDCNT
 * registers the oracle reads, DrawFrame renders the frame and captures the
 * oracle's four per-OBJ-priority spriteLayers, and every pixel of every
 * priority layer must match the sampler's output byte-for-byte.
 *
 * Only SUPPORTED primitives are compared (the capability boundary is covered in
 * the unit test): normal non-affine 4bpp 1D-mapped OBJ with flips, every shape/
 * size, multi-tile addressing, clipping/wrap, exact OAM order, multiple
 * priorities, and both blend baselines (off, and the persistent 0x1E40 field
 * state which the sampler proves is oracle-neutral).
 *
 * gba_easy_draw.c is compiled separately with -DRENDERER_EASY_DRAW and linked
 * here; see tests/native_overworld_renderer_test.sh.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "native_obj_renderer_shared.h"

/* Same typedef as main.h; declared locally so this TU need not pull in all of
 * main.h. gba_easy_draw.c's `extern void (*const gIntrTable[])` binds to the
 * symbol at link time. Never invoked (DISPSTAT intr bits stay zero). */
typedef void (*IntrFunc)(void);

/* ---- Real oracle symbols (defined in the separate gba_easy_draw.c TU) ---- */
extern uint16_t *gParityOBJLayers;
extern uint16_t *gParityOBJPreBlendLayers;
extern uint8_t gParityOBJOracleProduced;
extern void DrawFrame(uint16_t *pixels);

/* ---- Stubs of the hardware globals gba_easy_draw.c reads ---- */
unsigned char REG_BASE[0x400] __attribute__((aligned(4)));
unsigned char PLTT[PLTT_SIZE] __attribute__((aligned(4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));
unsigned char OAM[OAM_SIZE] __attribute__((aligned(4)));

/* See native_overworld_real_oracle_unit.c: main.h declares `extern IntrFunc
 * gIntrTable[]` (non-const); gba_easy_draw.c sees a const view. Define the
 * symbol here for the link; never invoked (DISPSTAT intr bits stay zero). */
IntrFunc gIntrTable[8] = {0};

void RunDMAs(u32 type)
{
    (void)type;
}

/* ---- capture + comparison ---- */

static u16 gOracleLayers[NATIVE_OBJ_PRIORITY_LAYERS * DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u16 gOraclePreBlendLayers[NATIVE_OBJ_PRIORITY_LAYERS * DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u16 gGbaImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];

static void PublishSnapshotToLiveState(void)
{
    memset(VRAM_, 0, VRAM_SIZE);
    memset(PLTT, 0, PLTT_SIZE);
    memset(OAM, 0, OAM_SIZE);
    memcpy(VRAM_ + 0x10000, sSnap.objVram, OBJ_VRAM0_SIZE);
    memcpy(PLTT + 0x200, sSnap.objPalette, OBJ_PALETTE_ENTRY_COUNT * sizeof(u16));
    memcpy(OAM, sSnap.finalOam, OAM_ENTRY_COUNT * sizeof(struct OamData));

    REG_DISPCNT = sSnap.dispCnt;
    REG_BLDCNT = sSnap.bldCnt;
    REG_BLDALPHA = 0;
    REG_BLDY = 0;
    REG_MOSAIC = 0;
    REG_WIN0H = 0;
    REG_WIN0V = 0;
    REG_WIN1H = 0;
    REG_WIN1V = 0;
    REG_WININ = 0;
    REG_WINOUT = 0;
    REG_DISPSTAT = 0;
    gParityOBJOracleProduced = 0;
}

static void CompareLayersFrom(const char *name, const u16 *oracleBuf)
{
    u32 mismatches = 0;
    u32 firstPr = 0;
    u32 firstIdx = 0;
    u16 firstMod = 0;
    u16 firstOra = 0;

    for (u32 pr = 0; pr < NATIVE_OBJ_PRIORITY_LAYERS; pr++)
    {
        for (u32 i = 0; i < (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT); i++)
        {
            u16 a = sOut.layers[pr].color[i];
            u16 b = oracleBuf[pr * (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT) + i];

            if (a != b)
            {
                if (mismatches == 0)
                {
                    firstPr = pr;
                    firstIdx = i;
                    firstMod = a;
                    firstOra = b;
                }
                mismatches++;
            }
        }
    }

    if (mismatches != 0)
    {
        fprintf(stderr,
                "OBJ oracle MISMATCH %s prio=%u at (%u,%u) module=0x%04X oracle=0x%04X\n",
                name, firstPr, firstIdx % DISPLAY_WIDTH, firstIdx / DISPLAY_WIDTH,
                firstMod, firstOra);
        fflush(stderr);
    }
    printf("%-34s mismatches=%u (0 expected)\n", name, mismatches);
    assert(mismatches == 0);
}

static void CompareOracleScenario(const char *name)
{
    assert(NativeObjRender_Rasterize(&sSnap, &sOut));
    assert(sOut.produced == TRUE);
    PublishSnapshotToLiveState();
    gParityOBJLayers = gOracleLayers;
    DrawFrame(gGbaImage);
    gParityOBJLayers = NULL;
    assert(gParityOBJOracleProduced == 1);
    CompareLayersFrom(name, gOracleLayers);
}

/*
 * Stage 3E pre-blend variant: with a real blend config (BLDCNT blend effect +
 * TGT2_BD, BLDALPHA 13/7) the oracle's POST-blend spriteLayers differ from its
 * RAW sampled colors at every semi pixel. gParityOBJPreBlendLayers captures the
 * raw colors, so comparing the native OBJ layers against THAT buffer proves the
 * sampler's semi texel/geometry/priority sampling is byte-identical to the
 * oracle -- independent of the blend the Stage 3E compositor applies later.
 */
static void CompareOracleScenarioPreBlend(const char *name)
{
    assert(NativeObjRender_Rasterize(&sSnap, &sOut));
    assert(sOut.produced == TRUE);
    PublishSnapshotToLiveState();
    REG_BLDALPHA = BLDALPHA_BLEND(13, 7);
    gParityOBJLayers = gOracleLayers;
    gParityOBJPreBlendLayers = gOraclePreBlendLayers;
    DrawFrame(gGbaImage);
    gParityOBJLayers = NULL;
    gParityOBJPreBlendLayers = NULL;
    assert(gParityOBJOracleProduced == 1);
    CompareLayersFrom(name, gOraclePreBlendLayers);
}

/*
 * Prove the pre-blend seam is genuinely raw: at a pixel where a semi sprite
 * draws, the oracle's post-blend layer color must differ from the pre-blend
 * raw color (so the comparison against gOraclePreBlendLayers is meaningful).
 */
static void AssertSeamDistinct(const char *name)
{
    u32 pr, i;

    for (pr = 0; pr < NATIVE_OBJ_PRIORITY_LAYERS; pr++)
    {
        for (i = 0; i < (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT); i++)
        {
            u16 pre = gOraclePreBlendLayers[pr * (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT) + i];
            u16 post = gOracleLayers[pr * (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT) + i];

            if (pre != 0 && post != 0 && pre != post)
            {
                printf("%-34s seam distinct at prio=%u (%u,%u) pre=0x%04X post=0x%04X\n",
                       name, pr, i % DISPLAY_WIDTH, i / DISPLAY_WIDTH, pre & 0x7FFF,
                       post & 0x7FFF);
                return;
            }
        }
    }
    fprintf(stderr, "FAIL: %s pre-blend seam not distinct (semi blend had no effect)\n",
            name);
    assert(0);
}

/* ---- scenarios ---- */

/* Stage 3D: affine matrix is read from the presented OAM layout, so each
 * scenario writes the matrix params via SetAffineMatrix into sSnap.finalOam --
 * PublishSnapshotToLiveState copies the whole array to live OAM and the oracle
 * reads OAM[matrixNum*4+k].affineParam, identical to the native sampler. */

static void ScenarioAffineIdentity(void)
{
    SnapReset();
    SetOam(60, 20, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 3, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine identity 8x8");
}

static void ScenarioAffineTransforms(void)
{
    /* pa=-0x100: horizontal mirror. */
    SnapReset();
    SetOam(60, 20, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 3, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, -0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine H-mirror");

    /* 2x scale both axes: texture zoomed into the box center. */
    SnapReset();
    SetOam(61, 60, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(61, 4);
    SetAffineMatrix(4, 0x200, 0, 0, 0x200);
    AddCommand(61, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine 2x scale");

    /* V-scale 2x only. */
    SnapReset();
    SetOam(62, 100, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 5, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(62, 5);
    SetAffineMatrix(5, 0x100, 0, 0, 0x200);
    AddCommand(62, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine V-scale 2x");

    /* Shrink 0x80: whole texture stretched across the box. */
    SnapReset();
    SetOam(63, 140, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 6, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(63, 6);
    SetAffineMatrix(6, 0x80, 0, 0, 0x80);
    AddCommand(63, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine shrink 0.5x");

    /* 90-degree rotation. */
    SnapReset();
    SetOam(64, 20, 60, ST_OAM_SQUARE, ST_OAM_SIZE_0, 7, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(64, 7);
    SetAffineMatrix(7, 0, -0x100, 0x100, 0);
    AddCommand(64, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine 90-degree rotation");

    /* 1.5x rounding (0x180): fractional 8.8 sampling. */
    SnapReset();
    SetOam(65, 60, 60, ST_OAM_SQUARE, ST_OAM_SIZE_1, 8, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(65, 8);
    SetAffineMatrix(8, 0x180, 0, 0, 0x180);
    AddCommand(65, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine 1.5x rounding");
}

static void ScenarioAffineShapeAndClip(void)
{
    /* Non-square wide 32x16 identity. */
    SnapReset();
    SetOam(60, 20, 60, ST_OAM_H_RECTANGLE, ST_OAM_SIZE_1, 9, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine wide 32x16");

    /* Partially offscreen (x=-6 canonicalizes to -6). */
    SnapReset();
    SetOam(61, -6, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 10, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(61, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(61, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine partial offscreen L");

    /* Partially offscreen top (y=-4). */
    SnapReset();
    SetOam(62, 60, -4, ST_OAM_SQUARE, ST_OAM_SIZE_0, 11, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(62, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(62, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine partial offscreen T");

    /* Double-size 16x16 identity: texture near center of the doubled box. */
    SnapReset();
    SetOam(63, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 12, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_DOUBLE, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(63, 9);
    SetAffineMatrix(9, 0x100, 0, 0, 0x100);
    AddCommand(63, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine double-size 16x16");

    /* Double-size with a rotation: doubled box + transformed sampling. */
    SnapReset();
    SetOam(64, 120, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 13, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_DOUBLE, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(64, 10);
    SetAffineMatrix(10, 0, -0x100, 0x100, 0);
    AddCommand(64, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine double-size rotate");

    /* Transparent texel: zero tile-14 texel (0,0); double-size identity samples
     * it at the box's upper-left-corner texel mapping. */
    SnapReset();
    SetOam(65, 40, 80, ST_OAM_SQUARE, ST_OAM_SIZE_0, 14, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_DOUBLE, 0, ST_OAM_4BPP, false, false);
    ZeroObjPixel(14, 0, 0);
    SetOamMatrixNum(65, 9);
    SetAffineMatrix(9, 0x100, 0, 0, 0x100);
    AddCommand(65, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine double-size transparent texel");
}

static void ScenarioAffineMultiMatrix(void)
{
    /* Flip bits ignored: matrixNum=3|8 pins matrix 3 with the HFLIP bit set;
     * matrix 3 is identity, so the sprite draws unmirrored (affine ignores the
     * flip bits, which are matrix-index bits for affine). */
    SnapReset();
    SetOam(60, 20, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 15, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3 | 8);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenario("oracle: affine flip bits ignored");

    /* Matrix reuse: two sprites share matrix 3 (identity). */
    SnapReset();
    SetOam(60, 20, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 16, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOam(61, 60, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 17, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetOamMatrixNum(61, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(61, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    CompareOracleScenario("oracle: affine matrix reuse");

    /* Different matrix indices in one frame: 60 mirror (matrix 4), 61 identity
     * (matrix 3), 62 double-size (matrix 9). */
    SnapReset();
    SetOam(60, 20, 60, ST_OAM_SQUARE, ST_OAM_SIZE_0, 18, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOam(61, 80, 60, ST_OAM_SQUARE, ST_OAM_SIZE_0, 19, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOam(62, 140, 60, ST_OAM_SQUARE, ST_OAM_SIZE_0, 20, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_DOUBLE, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 4);
    SetOamMatrixNum(61, 3);
    SetOamMatrixNum(62, 9);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    SetAffineMatrix(4, -0x100, 0, 0, 0x100);
    SetAffineMatrix(9, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(61, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    AddCommand(62, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    CompareOracleScenario("oracle: affine mirror+identity+double");

    /* Affine overlapping a non-affine OBJ at equal priority: the lower OAM index
     * wins the shared region. */
    SnapReset();
    SetOam(40, 50, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 21, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(60, 54, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 22, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(40, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    CompareOracleScenario("oracle: affine + non-affine overlap");
}

/* ---- scenarios ---- */

static void ScenarioBasic(void)
{
    SnapReset();
    SetOam(5, 10, 20, ST_OAM_SQUARE, ST_OAM_SIZE_0, 3, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    CompareOracleScenario("oracle: basic 8x8");
}

static void ScenarioShapeSizeMatrix(void)
{
    for (u8 shape = 0; shape <= 2; shape++)
    {
        for (u8 size = 0; size <= 3; size++)
        {
            char name[48];

            SnapReset();
            SetOam(4, 40, 40, shape, size, (u16)(10 + size + shape * 4), shape + 1,
                   0, ST_OAM_OBJ_NORMAL, ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP,
                   false, false);
            AddCommand(4, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
            snprintf(name, sizeof(name), "oracle: shape %u size %u", shape, size);
            CompareOracleScenario(name);
        }
    }
}

static void ScenarioFlips(void)
{
    SnapReset();
    SetOam(0, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_1, 10, 3, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, true, false);
    SetOam(1, 40, 10, ST_OAM_H_RECTANGLE, ST_OAM_SIZE_0, 20, 4, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, true);
    SetOam(2, 70, 10, ST_OAM_SQUARE, ST_OAM_SIZE_2, 30, 5, 2, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, true, true);
    SetOam(3, 120, 10, ST_OAM_V_RECTANGLE, ST_OAM_SIZE_2, 40, 6, 3, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    AddCommand(2, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    AddCommand(3, NATIVE_SPRITE_SOURCE_GSPRITE, 3, 0);
    CompareOracleScenario("oracle: flips H/V/H+V/non-square");
}

static void ScenarioClipping(void)
{
    SnapReset();
    SetOam(0, -1, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(1, -7, 20, ST_OAM_SQUARE, ST_OAM_SIZE_0, 1, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(2, 200, 30, ST_OAM_SQUARE, ST_OAM_SIZE_3, 4, 0, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(3, -32, 40, ST_OAM_SQUARE, ST_OAM_SIZE_3, 5, 0, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(4, 60, -1, ST_OAM_H_RECTANGLE, ST_OAM_SIZE_0, 6, 0, 2, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    AddCommand(2, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    AddCommand(3, NATIVE_SPRITE_SOURCE_GSPRITE, 3, 0);
    AddCommand(4, NATIVE_SPRITE_SOURCE_GSPRITE, 4, 0);
    CompareOracleScenario("oracle: clipping + signed wrap");
}

static void ScenarioOrdering(void)
{
    SnapReset();
    ZeroObjPixel(102, 3, 5);
    SetOam(30, 50, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 100, 0, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(20, 50, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 101, 0, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(10, 50, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 102, 0, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(30, NATIVE_SPRITE_SOURCE_GSPRITE, 3, 0);
    AddCommand(20, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    AddCommand(10, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    CompareOracleScenario("oracle: 3-way overlap + transparent");
}

static void ScenarioMultiPriority(void)
{
    SnapReset();
    for (u8 pr = 0; pr < 4; pr++)
    {
        SetOam((u8)(pr * 3 + 2), 60, 60, ST_OAM_SQUARE, ST_OAM_SIZE_0, (u16)(100 + pr),
               (u8)(pr + 1), pr, ST_OAM_OBJ_NORMAL, ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP,
               false, false);
        AddCommand((u8)(pr * 3 + 2), NATIVE_SPRITE_SOURCE_GSPRITE, (u8)pr, 0);
    }
    CompareOracleScenario("oracle: 4 priorities");
}

static void ScenarioBlendNeutral(void)
{
    SnapReset();
    SetOam(0, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    sSnap.bldCnt = 0;
    CompareOracleScenario("oracle: blend off");
    assert(!(sOut.capabilityFlags & NATIVE_OBJ_CAP_BLEND_CONFIG));

    /* Persistent field blend 0x1E40 (alpha, TGT1 empty): oracle-neutral. */
    sSnap.bldCnt = 0x1E40;
    CompareOracleScenario("oracle: field blend 0x1E40");
    assert(!(sOut.capabilityFlags & NATIVE_OBJ_CAP_BLEND_CONFIG));
}

/*
 * Stage 3E semi-transparent sampling, proven against the oracle's RAW pre-blend
 * OBJ layers (not the post-blend spriteLayers) -- the byte-identical comparison
 * isolates geometry/ownership/texel/priority from the blend the Stage 3E
 * compositor applies. Every scenario uses the persistent field blend 0x1E40.
 */
static void ScenarioSemiSampling(void)
{
    /* Semi 8x8 and normal 8x8 overlap at equal priority, semi is lower OAM:
     * the semi wins the overlap and its raw texel color is preserved (the
     * sampler layer never blends; only layer->semi[] carries the mode). */
    SnapReset();
    sSnap.bldCnt = 0x1E40;
    SetOam(0, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 20, 1, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(1, 44, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 30, 2, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    CompareOracleScenarioPreBlend("oracle: semi under normal same prio");
    assert(!(sOut.capabilityFlags & NATIVE_OBJ_CAP_BLEND_CONFIG));

    /* Normal is lower OAM: the normal wins the overlap; the semi draws only
     * where the normal does not cover it. */
    SnapReset();
    sSnap.bldCnt = 0x1E40;
    SetOam(0, 44, 60, ST_OAM_SQUARE, ST_OAM_SIZE_0, 20, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(1, 40, 60, ST_OAM_SQUARE, ST_OAM_SIZE_0, 30, 2, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    CompareOracleScenarioPreBlend("oracle: normal under semi same prio");

    /* Semi 16x16 with a transparent texel over a normal sprite at a LOWER
     * priority: the transparent texel leaves the lower layer visible. */
    SnapReset();
    sSnap.bldCnt = 0x1E40;
    ZeroObjPixel(50, 3, 3);
    SetOam(0, 40, 80, ST_OAM_SQUARE, ST_OAM_SIZE_1, 50, 3, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(1, 43, 83, ST_OAM_SQUARE, ST_OAM_SIZE_0, 40, 4, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    CompareOracleScenarioPreBlend("oracle: semi transparent texel");

    /* Semi flips H / V / H+V and one semi per priority, plus a non-square. */
    SnapReset();
    sSnap.bldCnt = 0x1E40;
    SetOam(0, 10, 100, ST_OAM_SQUARE, ST_OAM_SIZE_1, 60, 5, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, true, false);
    SetOam(1, 60, 100, ST_OAM_SQUARE, ST_OAM_SIZE_1, 61, 6, 1, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, true);
    SetOam(2, 110, 100, ST_OAM_SQUARE, ST_OAM_SIZE_1, 62, 7, 2, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, true, true);
    SetOam(3, 160, 100, ST_OAM_H_RECTANGLE, ST_OAM_SIZE_0, 63, 8, 3, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    AddCommand(2, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    AddCommand(3, NATIVE_SPRITE_SOURCE_GSPRITE, 3, 0);
    CompareOracleScenarioPreBlend("oracle: semi flips + 4 priorities");
}

static void ScenarioSemiAffine(void)
{
    /* Affine semi, identity matrix: raw pre-blend colors across the box. */
    SnapReset();
    sSnap.bldCnt = 0x1E40;
    SetOam(60, 20, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 70, 9, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenarioPreBlend("oracle: affine semi identity");

    /* Affine semi double-size + 90-degree rotation: transformed sampling,
     * still pre-blend in the oracle seam. */
    SnapReset();
    sSnap.bldCnt = 0x1E40;
    SetOam(61, 100, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 71, 10, 1, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_DOUBLE, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(61, 9);
    SetAffineMatrix(9, 0, -0x100, 0x100, 0);
    AddCommand(61, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenarioPreBlend("oracle: affine semi double rotate");

    /* Affine semi 2x scale. */
    SnapReset();
    sSnap.bldCnt = 0x1E40;
    SetOam(62, 60, 80, ST_OAM_SQUARE, ST_OAM_SIZE_0, 72, 11, 2, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(62, 3);
    SetAffineMatrix(3, 0x200, 0, 0, 0x200);
    AddCommand(62, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    CompareOracleScenarioPreBlend("oracle: affine semi 2x scale");
}

static void ScenarioSemiBlendSeamDistinct(void)
{
    /* Real blend config: BLDCNT blend effect + TGT2_BD (0x2040), BLDALPHA 13/7
     * set by CompareOracleScenarioPreBlend. The oracle blends every semi pixel
     * against the backdrop (0 in this harness) -> the post-blend spriteLayers
     * differ from the pre-blend layers at every semi pixel, proving the seam
     * captured the RAW color. The sampler must reproduce those raw colors and
     * must NOT set the normal-OBJ blend fallback (TGT1_OBJ is clear). */
    SnapReset();
    SetOam(0, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 80, 1, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(1, 44, 44, ST_OAM_SQUARE, ST_OAM_SIZE_0, 81, 2, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    sSnap.bldCnt = BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BD;
    CompareOracleScenarioPreBlend("oracle: semi blend seam distinct");
    assert(!(sOut.capabilityFlags & NATIVE_OBJ_CAP_BLEND_CONFIG));
    AssertSeamDistinct("oracle: semi blend seam distinct");
}

static void ScenarioStress(void)
{
    SnapReset();
    for (int i = 0; i < 40; i++)
    {
        u8 idx = (u8)(i + 1);
        u8 shape = (u8)(i % 3);
        u8 size = (u8)(i % 4);
        int x = 8 + (i * 37) % 200;
        int y = 8 + (i * 53) % 120;
        bool flipX = (i % 4) > 1;
        bool flipY = (i % 5) > 2;

        SetOam(idx, x, y, shape, size, (u16)(400 + i), (u8)(i % 16),
               (u8)(i % 4), ST_OAM_OBJ_NORMAL, ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP,
               flipX, flipY);
        AddCommand(idx, (i % 3 == 0) ? NATIVE_SPRITE_SOURCE_RAW_OAM
                                     : NATIVE_SPRITE_SOURCE_GSPRITE,
                   (u8)i, 0);
    }
    CompareOracleScenario("oracle: 40-sprite stress");
}

int main(void)
{
    ScenarioBasic();
    ScenarioShapeSizeMatrix();
    ScenarioFlips();
    ScenarioClipping();
    ScenarioOrdering();
    ScenarioMultiPriority();
    ScenarioBlendNeutral();
    ScenarioSemiSampling();
    ScenarioSemiAffine();
    ScenarioSemiBlendSeamDistinct();
    ScenarioAffineIdentity();
    ScenarioAffineTransforms();
    ScenarioAffineShapeAndClip();
    ScenarioAffineMultiMatrix();
    ScenarioStress();
    fprintf(stderr, "OBJ real-oracle harness passed\n");
    return 0;
}

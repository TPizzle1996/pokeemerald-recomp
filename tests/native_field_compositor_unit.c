/*
 * Stage 3C compositor unit tests: the pure BG+OBJ merge, the composite blend
 * predicate, and the on-demand composite trace -- all independent of the BG
 * renderer and the real GBA oracle.
 *
 * The merge is tested with hand-built BG frames + BG-winner metadata + OBJ
 * layers (via the Stage 3B shared model where a real rasterize is needed for
 * the trace), so each final-pixel rule from the header is asserted directly:
 *
 *   - BG only / OBJ over backdrop / OBJ transparent over BG
 *   - OBJ wins iff OBJ priority <= BG priority (backdrop = priority infinity)
 *   - equal-priority OBJ-vs-BG tie -> OBJ wins
 *   - reordered BG priorities drive the winner exactly as captured
 *   - foremost OBJ layer wins; transparent OBJ reveals the BG below
 *
 * The blend predicate asserts the neutral states the field actually uses
 * (EFFECT_NONE, TGT1 empty, alpha EVA16/EVB0, alpha with no TGT2) return FALSE
 * and every config that can recolor/brighten the final pixel returns TRUE.
 *
 * Only native_field_compositor.c + native_obj_renderer.c (via the Stage 3B
 * shared model) are linked; -Wl,--gc-sections drops the never-called
 * DrawCompositeFrame wrapper and its BG-renderer dependencies, so no game-global
 * stubs are needed here. The full path (renderer + wrapper + gates) is covered
 * by native_field_compositor_oracle_unit.c.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "native_obj_renderer_shared.h"
#include "../src/platform/native_field_compositor.c"

static u16 sBgFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u8 sBgWinner[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u16 sBgLayers[4 * DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u16 sOutFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u8 sOutWinner[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u8 sBgPriority[4];
static struct NativeCompositeBlendState sBlend;

static void ResetBlend(void)
{
    /* Field-style neutral state (the persistent 0x1E40): alpha mode, no TGT1,
     * TGT2 = BG1|BG2|BG3|OBJ, BG1/2/3 enabled, EVA/EVB 13/7. Pure-merge tests
     * use normal OBJ so the semi blend never fires; semi tests override this. */
    memset(&sBlend, 0, sizeof(sBlend));
    sBlend.bldCnt = BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2
                  | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_OBJ;
    sBlend.bldAlpha = (7 << 8) | 13;
    sBlend.dispCnt = (0x0E << 8); // BG1/BG2/BG3 enabled
    sBlend.backdropColor = 0x0000;
}

static void ResetBufs(u8 p0, u8 p1, u8 p2, u8 p3)
{
    memset(sBgFrame, 0, sizeof(sBgFrame));
    memset(sBgWinner, 0, sizeof(sBgWinner));
    memset(sBgLayers, 0, sizeof(sBgLayers));
    memset(sOutFrame, 0, sizeof(sOutFrame));
    memset(sOutWinner, 0, sizeof(sOutWinner));
    sBgPriority[0] = p0;
    sBgPriority[1] = p1;
    sBgPriority[2] = p2;
    sBgPriority[3] = p3;
    ResetBlend();
}

static void SetBg(u16 x, u16 y, u16 color, u8 winner)
{
    u32 i = (u32)y * DISPLAY_WIDTH + x;

    sBgFrame[i] = color;
    sBgWinner[i] = winner;
}

static void SetObj(struct NativeObjRenderOutput *out, u8 pr, u16 x, u16 y,
                   u16 color, u16 own)
{
    u32 i = (u32)y * DISPLAY_WIDTH + x;

    out->layers[pr].color[i] = color | 0x8000;
    out->layers[pr].ownership[i] = own;
}

static void AssertPixel(u16 x, u16 y, u16 expColor, u8 expWinner)
{
    u32 i = (u32)y * DISPLAY_WIDTH + x;

    if (sOutFrame[i] != expColor || sOutWinner[i] != expWinner)
    {
        fprintf(stderr,
                "compositor pixel (%u,%u): color=0x%04X exp=0x%04X winner=%u exp=%u\n",
                x, y, sOutFrame[i], expColor, sOutWinner[i], expWinner);
        assert(sOutFrame[i] == expColor);
        assert(sOutWinner[i] == expWinner);
    }
}

/* ---- merge tests ---- */

static void TestBgOnly(void)
{
    static struct NativeObjRenderOutput obj;

    ResetBufs(0, 1, 2, 3);
    memset(&obj, 0, sizeof(obj));
    obj.produced = TRUE;
    SetBg(10, 10, 0x0202 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    assert(NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &obj, &sBlend, sOutFrame, sOutWinner));
    AssertPixel(10, 10, 0x0202 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    AssertPixel(11, 10, 0x0000, NATIVE_COMPOSITE_WIN_BACKDROP);
    printf("%-34s ok\n", "merge: BG only");
}

static void TestObjOverBackdrop(void)
{
    static struct NativeObjRenderOutput obj;

    ResetBufs(0, 1, 2, 3);
    memset(&obj, 0, sizeof(obj));
    obj.produced = TRUE;
    /* BG transparent at (5,5); OBJ priority 3 (weakest) still beats backdrop. */
    SetObj(&obj, 3, 5, 5, 0x0100 + 16 * 3 + 7, OwnOf(10, 3, false));
    assert(NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &obj, &sBlend, sOutFrame, sOutWinner));
    AssertPixel(5, 5, (0x0100 + 16 * 3 + 7) | 0x8000, NATIVE_COMPOSITE_WIN_OBJ3);
    printf("%-34s ok\n", "merge: OBJ over backdrop");
}

static void TestObjTransparentOverBg(void)
{
    static struct NativeObjRenderOutput obj;

    ResetBufs(0, 1, 2, 3);
    memset(&obj, 0, sizeof(obj));
    obj.produced = TRUE;
    /* BG1 opaque prio 1 at (8,8); OBJ prio 0 only claims (9,8) -- (8,8) has no
     * OBJ pixel, so the BG must show through there. */
    SetBg(8, 8, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    SetBg(9, 8, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    SetObj(&obj, 0, 9, 8, 0x0400, OwnOf(0, 0, false));
    assert(NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &obj, &sBlend, sOutFrame, sOutWinner));
    AssertPixel(8, 8, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    AssertPixel(9, 8, 0x0400 | 0x8000, NATIVE_COMPOSITE_WIN_OBJ0);
    printf("%-34s ok\n", "merge: OBJ transparent reveals BG");
}

static void TestPriorityTies(void)
{
    static struct NativeObjRenderOutput obj;

    ResetBufs(0, 1, 2, 3);
    memset(&obj, 0, sizeof(obj));
    obj.produced = TRUE;
    /* BG1 prio 1, OBJ prio 1: equal -> OBJ wins. */
    SetBg(2, 2, 0x0505 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    SetObj(&obj, 1, 2, 2, 0x0606, OwnOf(4, 1, false));
    /* BG1 prio 1, OBJ prio 2: BG wins. */
    SetBg(4, 4, 0x0505 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    SetObj(&obj, 2, 4, 4, 0x0707, OwnOf(5, 2, false));
    /* BG2 prio 2, OBJ prio 2: equal -> OBJ wins. */
    SetBg(6, 6, 0x0808 | 0x8000, NATIVE_COMPOSITE_WIN_BG2);
    SetObj(&obj, 2, 6, 6, 0x0909, OwnOf(6, 2, false));
    assert(NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &obj, &sBlend, sOutFrame, sOutWinner));
    AssertPixel(2, 2, 0x0606 | 0x8000, NATIVE_COMPOSITE_WIN_OBJ1);
    AssertPixel(4, 4, 0x0505 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    AssertPixel(6, 6, 0x0909 | 0x8000, NATIVE_COMPOSITE_WIN_OBJ2);
    printf("%-34s ok\n", "merge: OBJ/BG priority ties");
}

static void TestReorderedBgPriorities(void)
{
    static struct NativeObjRenderOutput obj;

    ResetBufs(0, 1, 2, 3);
    memset(&obj, 0, sizeof(obj));
    obj.produced = TRUE;
    /* Reordered capture: BG1 (bgnum 1) prio 3, BG3 (bgnum 3) prio 1. A prio-2
     * OBJ beats BG1 (prio 3) but loses to BG3 (prio 1). */
    sBgPriority[1] = 3;
    sBgPriority[3] = 1;
    SetBg(2, 2, 0x0A0A | 0x8000, NATIVE_COMPOSITE_WIN_BG1); // BG1 prio 3
    SetBg(4, 4, 0x0B0B | 0x8000, NATIVE_COMPOSITE_WIN_BG3); // BG3 prio 1
    SetObj(&obj, 2, 2, 2, 0x0C0C, OwnOf(1, 2, false));
    SetObj(&obj, 2, 4, 4, 0x0D0D, OwnOf(1, 2, false));
    assert(NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &obj, &sBlend, sOutFrame, sOutWinner));
    /* OBJ prio 2 <= BG1 prio 3 -> OBJ. */
    AssertPixel(2, 2, 0x0C0C | 0x8000, NATIVE_COMPOSITE_WIN_OBJ2);
    /* OBJ prio 2 > BG3 prio 1 -> BG3. */
    AssertPixel(4, 4, 0x0B0B | 0x8000, NATIVE_COMPOSITE_WIN_BG3);
    printf("%-34s ok\n", "merge: reordered BG priorities");
}

static void TestForemostObjLayerWins(void)
{
    static struct NativeObjRenderOutput obj;

    ResetBufs(0, 1, 2, 3);
    memset(&obj, 0, sizeof(obj));
    obj.produced = TRUE;
    /* Two OBJ priorities claim the same pixel: the foremost (prio 0) wins. */
    SetObj(&obj, 0, 3, 3, 0x0E0E, OwnOf(2, 0, false));
    SetObj(&obj, 2, 3, 3, 0x0F0F, OwnOf(3, 2, false));
    assert(NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &obj, &sBlend, sOutFrame, sOutWinner));
    AssertPixel(3, 3, 0x0E0E | 0x8000, NATIVE_COMPOSITE_WIN_OBJ0);
    printf("%-34s ok\n", "merge: foremost OBJ layer wins");
}

static void TestNullInput(void)
{
    static struct NativeObjRenderOutput obj;

    ResetBufs(0, 1, 2, 3);
    memset(&obj, 0, sizeof(obj));
    assert(!NativeFieldCompositor_Composite(NULL, sBgWinner, sBgPriority, sBgLayers, &obj, &sBlend, sOutFrame, sOutWinner));
    assert(!NativeFieldCompositor_Composite(sBgFrame, NULL, sBgPriority, sBgLayers, &obj, &sBlend, sOutFrame, sOutWinner));
    assert(!NativeFieldCompositor_Composite(sBgFrame, sBgWinner, NULL, sBgLayers, &obj, &sBlend, sOutFrame, sOutWinner));
    assert(!NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, NULL, &sBlend, sOutFrame, sOutWinner));
    assert(!NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &obj, &sBlend, NULL, sOutWinner));
    printf("%-34s ok\n", "merge: NULL input rejected");
}

/* ---- blend predicate ---- */

static void TestBlendPredicate(void)
{
    assert(!NativeField_BlendAffectsComposite(0, 0, 0));            // EFFECT_NONE
    assert(!NativeField_BlendAffectsComposite(0x1E40, 0, 0x1010));  // field neutral
    assert(!NativeField_BlendAffectsComposite(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG0,
                                              0, 0x1010));          // alpha EVA16/EVB0
    assert(!NativeField_BlendAffectsComposite(BLDCNT_EFFECT_BLEND | BLDCNT_TGT1_BG0,
                                              0, 0x0800));          // alpha TGT2 empty
    assert(!NativeField_BlendAffectsComposite(BLDCNT_EFFECT_BLEND | BLDCNT_TGT1_BD
                                              | BLDCNT_TGT2_BG0, 0, 0x0800)); // TGT1 backdrop only
    assert(!NativeField_BlendAffectsComposite(BLDCNT_EFFECT_LIGHTEN | BLDCNT_TGT1_BG0,
                                              0, 0));               // brightness BLDY==0
    assert(NativeField_BlendAffectsComposite(BLDCNT_EFFECT_BLEND | BLDCNT_TGT1_BG0
                                             | BLDCNT_TGT2_BG1, 0, 0x0800));
    assert(NativeField_BlendAffectsComposite(BLDCNT_EFFECT_BLEND | BLDCNT_TGT1_OBJ
                                             | BLDCNT_TGT2_BD, 0, 0x0800));
    assert(NativeField_BlendAffectsComposite(BLDCNT_EFFECT_LIGHTEN | BLDCNT_TGT1_BG0,
                                              8, 0));
    assert(NativeField_BlendAffectsComposite(BLDCNT_EFFECT_LIGHTEN | BLDCNT_TGT1_BD,
                                              8, 0));
    printf("%-34s ok\n", "blend: neutral vs effectful");
}

/* Stage 3D: affine OBJ produced by the real sampler composites correctly --
 * final color AND winner. Reuses the shared-model rasterize (RasterizeOk fills
 * sOut from sSnap) then merges over a hand-built BG frame. */
static void TestAffineComposite(void)
{
    u16 expColor;
    u8 expPixel;
    int i;

    /* Affine 8x8 identity at (40,40) prio 0 over BG1 prio 1: OBJ wins. The box
     * is [40,49) x [40,48) -- screen (48,40) is the inclusive-right column that
     * culls itself for the identity matrix, so it stays BG. */
    SnapReset();
    SetOam(60, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    ResetBufs(0, 1, 2, 3);
    for (i = 40; i < 56; i++)
        SetBg((u16)i, 40, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    SetBg(42, 42, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    assert(NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &sOut, &sBlend, sOutFrame, sOutWinner));
    assert(SampleExpected(&sSnap.finalOam[60], &sSnap, 42, 42, &expColor, &expPixel));
    AssertPixel(42, 42, expColor | 0x8000, NATIVE_COMPOSITE_WIN_OBJ0);
    AssertPixel(47, 40, ExpectedColor(4, ExpectedPixelIndex(4, 7, 0)) | 0x8000,
                NATIVE_COMPOSITE_WIN_OBJ0);
    AssertPixel(48, 40, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1); /* culled column */
    AssertPixel(50, 40, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1); /* outside box */

    /* Affine behind BG: prio 2 vs BG1 prio 1 -> BG wins. */
    SnapReset();
    SetOam(60, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 2, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &sOut, &sBlend, sOutFrame, sOutWinner));
    AssertPixel(42, 42, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);

    /* Affine transparent texel over BG: zero tile-4 texel (2,2); screen (42,42)
     * samples it -> BG1 shows. */
    SnapReset();
    ZeroObjPixel(4, 2, 2);
    SetOam(60, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &sOut, &sBlend, sOutFrame, sOutWinner));
    AssertPixel(42, 42, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);

    /* Affine tie with BG (BG1 prio 0): OBJ wins equal-priority ties. */
    SnapReset();
    SetOam(60, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    ResetBufs(0, 0, 2, 3);
    SetBg(42, 42, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    assert(NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &sOut, &sBlend, sOutFrame, sOutWinner));
    assert(SampleExpected(&sSnap.finalOam[60], &sSnap, 42, 42, &expColor, &expPixel));
    AssertPixel(42, 42, expColor | 0x8000, NATIVE_COMPOSITE_WIN_OBJ0);

    /* Affine double-size over BG: 16x16 identity at (40,40), center (56,56).
     * The texture covers the box center (50,50); the doubled-box corner (42,42)
     * samples tex -6 -> transparent, so BG1 shows. */
    SnapReset();
    SetOam(61, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 5, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_DOUBLE, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(61, 9);
    SetAffineMatrix(9, 0x100, 0, 0, 0x100);
    AddCommand(61, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    ResetBufs(0, 0, 2, 3);
    SetBg(42, 42, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    SetBg(50, 50, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);
    assert(NativeFieldCompositor_Composite(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &sOut, &sBlend, sOutFrame, sOutWinner));
    assert(SampleExpected(&sSnap.finalOam[61], &sSnap, 50, 50, &expColor, &expPixel));
    AssertPixel(50, 50, expColor | 0x8000, NATIVE_COMPOSITE_WIN_OBJ0);
    AssertPixel(42, 42, 0x0303 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);

    printf("%-34s ok\n", "merge: affine over/behind/transparent/tie/double");
}

/* ---- trace ---- */

static void TestTrace(void)
{
    struct NativeFieldCompositeTrace t;
    u16 expColor;
    u8 expPixel;

    SnapReset();
    SetOam(7, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 20, 3, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(7, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    RasterizeOk();

    ResetBufs(0, 1, 2, 3);
    /* Backdrop at the OBJ rect; a BG1 pixel elsewhere. */
    SetBg(10, 10, 0x1111 | 0x8000, NATIVE_COMPOSITE_WIN_BG1);

    /* OBJ pixel: full provenance + final winner OBJ1. */
    assert(SampleExpected(&sSnap.finalOam[7], &sSnap, 10, 10, &expColor, &expPixel));
    assert(NativeFieldCompositor_Trace(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &sSnap, &sOut, &sBlend, 10, 10, &t));
    assert(t.objPresent == TRUE);
    assert(t.objPriority == 1);
    assert(t.objColor == expColor);
    assert(t.objTrace.present == TRUE);
    assert(t.objTrace.oamIndex == 7);
    assert(t.objTrace.priority == 1);
    assert(t.objTrace.pixelIndex == expPixel);
    assert(t.finalWinner == NATIVE_COMPOSITE_WIN_OBJ1);
    assert(t.finalColor == (expColor | 0x8000));

    /* Non-OBJ pixel: BG side wins. */
    assert(NativeFieldCompositor_Trace(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &sSnap, &sOut, &sBlend, 50, 50, &t));
    assert(t.objPresent == FALSE);
    assert(t.bgWinner == NATIVE_COMPOSITE_WIN_BACKDROP);
    assert(t.bgPriority == 0xFF);
    assert(t.finalWinner == NATIVE_COMPOSITE_WIN_BACKDROP);

    /* Out-of-range + NULL rejected. */
    assert(!NativeFieldCompositor_Trace(sBgFrame, sBgWinner, sBgPriority, sBgLayers, &sSnap, &sOut, &sBlend, DISPLAY_WIDTH, 0, &t));
    assert(!NativeFieldCompositor_Trace(NULL, sBgWinner, sBgPriority, sBgLayers, &sSnap, &sOut, &sBlend, 10, 10, &t));
    printf("%-34s ok\n", "trace: provenance + winner");
}

int main(void)
{
    TestBgOnly();
    TestObjOverBackdrop();
    TestObjTransparentOverBg();
    TestPriorityTies();
    TestReorderedBgPriorities();
    TestForemostObjLayerWins();
    TestNullInput();
    TestBlendPredicate();
    TestAffineComposite();
    TestTrace();
    fprintf(stderr, "field compositor unit passed\n");
    return 0;
}

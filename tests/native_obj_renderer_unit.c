/*
 * Stage 3B OBJ sampler unit tests (self-contained; no game launch, no fixtures).
 *
 * Exercises the full required matrix against the independent model in
 * native_obj_renderer_shared.h:
 *   - basic 8x8 sprite, exact rect + ownership + on-demand trace
 *   - every legal non-affine shape/size combination
 *   - multi-tile row-stride addressing (16x16 quadrant tiles)
 *   - H / V / H+V / non-square flips
 *   - position / clipping / signed-wrap (x=-1,-7,-8, partial edges, large clip)
 *   - exact OAM order (3-way overlap; transparent exposing the next sprite)
 *   - subsprite multi-command + raw-OAM provenance
 *   - capability rejection per reason (8bpp, OBJ-window, mosaic, prohibited
 *     shape; silent skip for non-affine+double; offscreen affine reserved
 *     entries never flagged). Stage 3E: semi-transparent OBJ is SAMPLED (not
 *     rejected) with identical geometry and its mode preserved in layer->semi[]
 *     for the compositor to blend.
 *   - 2D OBJ mapping -> produced=FALSE; NULL input rejected
 *   - persistent field blend (0x1E40) neutral; genuine OBJ blend flagged
 *
 * Compile with the Makefile_pc test flags (-DPORTABLE -DNONMATCHING -DUBFIX
 * -DMODERN=1 -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1).
 */
#include "native_obj_renderer_shared.h"

/* ---- helpers local to this TU ---- */

static bool HasReject(const struct NativeObjRenderOutput *out, u8 oamIndex, u8 reason)
{
    u16 i;
    for (i = 0; i < out->commandRejectedCount; i++)
        if (out->rejects[i].oamIndex == oamIndex && out->rejects[i].reason == reason)
            return true;
    return false;
}

/* ---- 1. basic 8x8 sprite ---- */

static void TestBasicSprite(void)
{
    struct NativeObjSampleTrace tr;

    SnapReset();
    SetOam(5, 10, 20, ST_OAM_SQUARE, ST_OAM_SIZE_0, 3, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    RasterizeOk();

    assert(sOut.gspriteDrawnCount == 1);
    assert(sOut.rawDrawnCount == 0);
    assert(sOut.commandRejectedCount == 0);
    assert(sOut.capabilityFlags == NATIVE_OBJ_CAP_NONE);
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[5], 5, 0, false);

    /* Trace a claimed pixel. */
    assert(NativeObjRender_Trace(&sSnap, &sOut, 0, 12, 22, &tr));
    assert(tr.present == TRUE);
    assert(tr.oamIndex == 5);
    assert(tr.commandId == 0);
    assert(tr.sourceKind == NATIVE_SPRITE_SOURCE_GSPRITE);
    assert(tr.spriteId == 2);
    assert(tr.partIndex == 0);
    assert(tr.paletteNum == 1);
    assert(tr.tileNum == 3);
    assert(tr.shape == ST_OAM_SQUARE);
    assert(tr.size == ST_OAM_SIZE_0);
    assert(tr.flipX == FALSE);
    assert(tr.flipY == FALSE);
    assert(tr.priority == 0);
    {
        u16 color;
        u8 pixel;
        assert(SampleExpected(&sSnap.finalOam[5], &sSnap, 12, 22, &color, &pixel));
        assert(tr.color == (color & 0x7FFF));
        assert(tr.pixelIndex == pixel);
    }

    /* Trace an unclaimed pixel. */
    tr.present = TRUE;
    assert(!NativeObjRender_Trace(&sSnap, &sOut, 0, 0, 0, &tr));
    assert(tr.present == FALSE);

    printf("3B: basic 8x8 sprite + trace ... ok\n");
}

/* ---- 2. every legal shape/size combination ---- */

static void TestShapeSizeMatrix(void)
{
    const u8 dims[3][4][2] =
    {
        /* square: size0..3 */
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        /* wide */
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        /* tall */
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    };
    int idx = 0;

    for (u8 shape = 0; shape <= 2; shape++)
    {
        for (u8 size = 0; size <= 3; size++)
        {
            char name[80];
            u8 oamIndex = (u8)(10 + idx);
            int w = dims[shape][size][0];
            int h = dims[shape][size][1];

            SnapReset();
            SetOam(oamIndex, 40, 40, shape, size, (u16)(idx + 50), (u8)(idx & 15),
                   1, ST_OAM_OBJ_NORMAL, ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP,
                   false, false);
            AddCommand(oamIndex, NATIVE_SPRITE_SOURCE_GSPRITE, (u8)idx, 0);
            RasterizeOk();
            assert(sOut.gspriteDrawnCount == 1);
            AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[oamIndex], oamIndex, 1, false);

            snprintf(name, sizeof(name), "3B: shape %u size %u (%dx%d)", shape, size, w, h);
            printf("%-40s ... ok\n", name);
            idx++;
        }
    }
}

/* ---- 3. multi-tile row-stride addressing (16x16 quadrant tiles) ---- */

static void TestMultiTileAddressing(void)
{
    SnapReset();
    /* 16x16 square, tileNum 100. rowStride = 16/8 = 2. Quadrant tiles:
     * (tx<8, ty<8)->100, (tx>=8, ty<8)->101, (tx<8, ty>=8)->102, both->103. */
    SetOam(3, 50, 60, ST_OAM_SQUARE, ST_OAM_SIZE_1, 100, 2, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(3, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();

    /* One sample pixel per quadrant. Sprite top-left (50,60), center (58,68):
     *   top row sy=63 -> texY=3; bottom row sy=70 -> texY=10 (tileY=2).
     *   left col sx=52 -> texX=2; right col sx=60 -> texX=10 (blockX=1). */
    AssertLayerPresent(&sOut, 0, 52, 63,
                       ExpectedColor(2, ExpectedPixelIndex(100, 2, 3)),
                       OwnOf(3, 0, false));
    AssertLayerPresent(&sOut, 0, 60, 63,
                       ExpectedColor(2, ExpectedPixelIndex(101, 2, 3)),
                       OwnOf(3, 0, false));
    AssertLayerPresent(&sOut, 0, 52, 70,
                       ExpectedColor(2, ExpectedPixelIndex(102, 2, 2)),
                       OwnOf(3, 0, false));
    AssertLayerPresent(&sOut, 0, 60, 70,
                       ExpectedColor(2, ExpectedPixelIndex(103, 2, 2)),
                       OwnOf(3, 0, false));
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[3], 3, 0, false);

    printf("3B: 16x16 row-stride tile addressing ... ok\n");
}

/* ---- 4. flips (H, V, H+V, non-square) ---- */

static void TestFlips(void)
{
    /* Flip H: screen x maps to the mirrored texel; at the sprite center column
     * the colors of symmetric pixels must be swapped vs the unflipped case. */
    SnapReset();
    SetOam(7, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 10, 3, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, true, false); /* 16x16 H-flip */
    AddCommand(7, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    RasterizeOk();
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[7], 7, 0, false);

    /* Mirror proof (half-open rect: H-flip mirror of x is 2*centerX - x - 1):
     * pixel (47,44) of the H-flip sprite == (48,44) of the unflipped sprite. */
    {
        u16 flipped, unflipped;

        flipped = sOut.layers[0].color[44 * DISPLAY_WIDTH + 47];
        SnapReset();
        SetOam(7, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 10, 3, 0, ST_OAM_OBJ_NORMAL,
               ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
        AddCommand(7, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
        RasterizeOk();
        unflipped = sOut.layers[0].color[44 * DISPLAY_WIDTH + 48];
        assert(flipped != 0);
        assert(flipped == unflipped);
    }

    /* V-flip on a non-square (16x8 wide) sprite; assert the full rect via the
     * independent model, plus an explicit vertical mirror spot check. */
    SnapReset();
    SetOam(8, 100, 20, ST_OAM_H_RECTANGLE, ST_OAM_SIZE_0, 20, 4, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, true); /* 16x8 V-flip */
    AddCommand(8, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    RasterizeOk();
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[8], 8, 1, false);

    {
        u16 vflipped, unflippedV;

        vflipped = sOut.layers[1].color[22 * DISPLAY_WIDTH + 105];
        SnapReset();
        SetOam(8, 100, 20, ST_OAM_H_RECTANGLE, ST_OAM_SIZE_0, 20, 4, 1, ST_OAM_OBJ_NORMAL,
               ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
        AddCommand(8, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
        RasterizeOk();
        unflippedV = sOut.layers[1].color[25 * DISPLAY_WIDTH + 105];
        assert(vflipped != 0);
        assert(vflipped == unflippedV);
    }

    /* H+V combined on a 32x32 square. */
    SnapReset();
    SetOam(9, 60, 50, ST_OAM_SQUARE, ST_OAM_SIZE_2, 30, 5, 2, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, true, true);
    AddCommand(9, NATIVE_SPRITE_SOURCE_GSPRITE, 3, 0);
    RasterizeOk();
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[9], 9, 2, false);

    printf("3B: flips H/V/H+V/non-square ... ok\n");
}

/* ---- 5. position / clipping / signed wrap ---- */

static void TestPositionClipping(void)
{
    int sx;

    /* x=-1 (packed 511 -> canonical -1): 8x8 spans -1..6, 7 px visible. */
    SnapReset();
    SetOam(0, -1, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    for (sx = 0; sx <= 6; sx++)
        AssertLayerPresent(&sOut, 0, (u8)sx, 30, ExpectedColor(0, ExpectedPixelIndex(0, sx + 1, 0)), OwnOf(0, 0, false));
    AssertLayerAbsent(&sOut, 0, 7, 30);

    /* x=-7 (packed 505 -> canonical -7): spans -7..0, 1 px visible. */
    SnapReset();
    SetOam(0, -7, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 1, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertLayerPresent(&sOut, 0, 0, 30, ExpectedColor(0, ExpectedPixelIndex(1, 7, 0)), OwnOf(0, 0, false));
    AssertLayerAbsent(&sOut, 0, 1, 30);

    /* x=-8 (packed 504 -> canonical -8): spans -8..-1, fully offscreen. */
    SnapReset();
    SetOam(0, -8, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 2, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.gspriteDrawnCount == 0); /* not presented */
    for (sx = 0; sx < 8; sx++)
        AssertLayerAbsent(&sOut, 0, (u8)sx, 30);

    /* y=-1 (packed 255 -> canonical -1): 16x8 wide spans y -1..6, 7 rows. */
    SnapReset();
    SetOam(1, 30, -1, ST_OAM_H_RECTANGLE, ST_OAM_SIZE_0, 3, 0, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[1], 1, 1, false);

    /* Partial right edge: 64-wide square at x=200 spans 200..263, 40 px. */
    SnapReset();
    SetOam(2, 200, 10, ST_OAM_SQUARE, ST_OAM_SIZE_3, 4, 0, 2, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(2, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[2], 2, 2, false);
    assert(sOut.gspriteDrawnCount == 1);

    /* Partial bottom edge: 64-tall square at y=120 spans 120..183, 40 rows. */
    SnapReset();
    SetOam(2, 10, 120, ST_OAM_SQUARE, ST_OAM_SIZE_3, 4, 0, 2, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(2, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[2], 2, 2, false);

    /* Large clip: 64x64 at x=-32,y=-32 (packed 480, 224) -> 32x32 visible. */
    SnapReset();
    SetOam(3, -32, -32, ST_OAM_SQUARE, ST_OAM_SIZE_3, 5, 0, 3, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(3, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[3], 3, 3, false);
    AssertLayerAbsent(&sOut, 3, 32, 32);

    printf("3B: position / clipping / signed wrap ... ok\n");
}

/* ---- 6. OAM order: 3-way overlap + transparent exposes next ---- */

static void TestOrderingOverlap(void)
{
    int x, y;

    SnapReset();
    /* Three 8x8 sprites, same position, same priority 1, different tiles. */
    SetOam(30, 50, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 100, 0, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(20, 50, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 101, 0, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(10, 50, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 102, 0, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(30, NATIVE_SPRITE_SOURCE_GSPRITE, 3, 0);
    AddCommand(20, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    AddCommand(10, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    RasterizeOk();

    /* Lowest OAM index wins every pixel at equal priority. */
    for (y = 50; y < 58; y++)
        for (x = 50; x < 58; x++)
            AssertLayerPresent(&sOut, 1, (u8)x, (u8)y,
                               ExpectedColor(0, ExpectedPixelIndex(102, x - 50, y - 50)),
                               OwnOf(10, 1, false));
    assert(sOut.gspriteDrawnCount == 3);

    /* Transparent exposes next: make tile 102 transparent at texel (3,5). The
     * winner there must be OAM 20 (tile 101), everywhere else OAM 10. */
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
    RasterizeOk();

    AssertLayerPresent(&sOut, 1, 53, 55,
                       ExpectedColor(0, ExpectedPixelIndex(101, 3, 5)),
                       OwnOf(20, 1, false));
    AssertLayerPresent(&sOut, 1, 50, 50,
                       ExpectedColor(0, ExpectedPixelIndex(102, 0, 0)),
                       OwnOf(10, 1, false));

    printf("3B: OAM order (3-way overlap + transparent) ... ok\n");
}

/* ---- 7. subsprite multi-command + raw-OAM provenance ---- */

static void TestSubspriteMultiCommand(void)
{
    struct NativeObjSampleTrace tr;

    SnapReset();
    /* A 32x16 subsprite as two 16x16 parts: part0 at x=30 (OAM 4), part1 at
     * x=46 (OAM 5). Each is its own command; provenance must be preserved. */
    SetOam(4, 30, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 200, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(5, 46, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 201, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(4, NATIVE_SPRITE_SOURCE_GSPRITE, 7, 0);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 7, 1);
    RasterizeOk();
    assert(sOut.gspriteDrawnCount == 2);

    /* The two parts abut exactly (part0 spans x 30..45, part1 x 46..61), so
     * assert the whole 32x16 union rect with the per-part owner/color. */
    for (int y = 40; y < 56; y++)
    {
        for (int x = 30; x < 62; x++)
        {
            u8 owner = (x < 46) ? 4 : 5;
            u16 color;
            u8 pixel;
            assert(SampleExpected(&sSnap.finalOam[owner], &sSnap, x, y, &color, &pixel));
            AssertLayerPresent(&sOut, 0, (u8)x, (u8)y, color, OwnOf(owner, 0, false));
        }
    }
    AssertLayerAbsent(&sOut, 0, 29, 44);
    AssertLayerAbsent(&sOut, 0, 62, 44);
    AssertLayerAbsent(&sOut, 0, 40, 39);
    AssertLayerAbsent(&sOut, 0, 40, 56);

    assert(NativeObjRender_Trace(&sSnap, &sOut, 0, 60, 44, &tr));
    assert(tr.oamIndex == 5);
    assert(tr.commandId == 1);
    assert(tr.spriteId == 7);
    assert(tr.partIndex == 1);

    /* Raw-OAM provenance: a visible final OAM entry with no GSPRITE command is
     * classified RAW_OAM, drawn as a valid 240x160 primitive with the RAW bit. */
    SnapReset();
    SetOam(12, 90, 90, ST_OAM_SQUARE, ST_OAM_SIZE_1, 210, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(12, NATIVE_SPRITE_SOURCE_RAW_OAM, NATIVE_OBJ_NO_ID, 0);
    RasterizeOk();
    assert(sOut.rawDrawnCount == 1);
    assert(sOut.gspriteDrawnCount == 0);
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[12], 12, 0, true);
    assert(NativeObjRender_Trace(&sSnap, &sOut, 0, 95, 95, &tr));
    assert(tr.sourceKind == NATIVE_SPRITE_SOURCE_RAW_OAM);
    assert(tr.spriteId == NATIVE_OBJ_NO_ID);

    printf("3B: subsprite multi-command + raw-OAM provenance ... ok\n");
}

/* ---- 8. capability rejection per reason ---- */

static void TestCapabilityRejection(void)
{
    /* Affine is now SUPPORTED (Stage 3D): a plain affine 8x8 draws, no flag, no
     * reject. (SetOamMatrixNum pins matrixNum 1; SetAffineMatrix loads the
     * identity 8.8 matrix into the presented OAM layout.) */
    SnapReset();
    SetOam(0, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(0, 1);
    SetAffineMatrix(1, 0x100, 0, 0, 0x100);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.capabilityFlags == NATIVE_OBJ_CAP_NONE);
    assert(sOut.commandRejectedCount == 0);
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[0], 0, 0, false);

    /* Affine double-size: also supported, no flag, no reject. The doubled box
     * is only partially covered by the texture, so use the bidirectional
     * validator. */
    SnapReset();
    SetOam(1, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_DOUBLE, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(1, 2);
    SetAffineMatrix(2, 0x100, 0, 0, 0x100);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.capabilityFlags == NATIVE_OBJ_CAP_NONE);
    assert(sOut.commandRejectedCount == 0);
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[1], 1, 0, false);

    /* 8bpp. */
    SnapReset();
    SetOam(2, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_8BPP, false, false);
    AddCommand(2, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.capabilityFlags & NATIVE_OBJ_CAP_8BPP);
    assert(HasReject(&sOut, 2, NATIVE_OBJ_REJECT_8BPP));
    AssertLayerAbsent(&sOut, 0, 12, 12);

    /* Semi-transparent (Stage 3E): now SAMPLED like any normal OBJ -- identical
     * geometry/texel/priority semantics, capabilityFlags stay NONE (no reject),
     * and the mode is preserved per-pixel in layer->semi[] for the Stage 3E
     * compositor to blend (the sampler itself never blends). */
    SnapReset();
    SetOam(3, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(3, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.capabilityFlags == NATIVE_OBJ_CAP_NONE);
    assert(sOut.commandRejectedCount == 0);
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[3], 3, 0, false);
    for (int sy = 10; sy < 18; sy++)
        for (int sx = 10; sx < 18; sx++)
            assert(sOut.layers[0].semi[sy * DISPLAY_WIDTH + sx] == 1);
    assert(sOut.layers[0].semi[5 * DISPLAY_WIDTH + 5] == 0); /* off-sprite */

    /* OBJ-window. */
    SnapReset();
    SetOam(4, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_WINDOW,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(4, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.capabilityFlags & NATIVE_OBJ_CAP_OBJ_WINDOW);
    assert(HasReject(&sOut, 4, NATIVE_OBJ_REJECT_OBJ_WINDOW));
    AssertLayerAbsent(&sOut, 0, 12, 12);

    /* Mosaic. */
    SnapReset();
    SetOam(5, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 1, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.capabilityFlags & NATIVE_OBJ_CAP_MOSAIC);
    assert(HasReject(&sOut, 5, NATIVE_OBJ_REJECT_MOSAIC));
    AssertLayerAbsent(&sOut, 0, 12, 12);

    /* Prohibited shape 3. */
    SnapReset();
    SetOam(6, 10, 10, 3, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(6, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.capabilityFlags & NATIVE_OBJ_CAP_PROHIBITED_SHAPE);
    assert(HasReject(&sOut, 6, NATIVE_OBJ_REJECT_PROHIBITED_SHAPE));
    AssertLayerAbsent(&sOut, 0, 12, 12);

    /* Non-affine + double: oracle-disabled, silent skip (no reject, no flag). */
    SnapReset();
    SetOam(7, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           2 /* affine off, double set */, 0, ST_OAM_4BPP, false, false);
    AddCommand(7, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.capabilityFlags == NATIVE_OBJ_CAP_NONE);
    assert(sOut.commandRejectedCount == 0);
    AssertLayerAbsent(&sOut, 0, 12, 12);

    /* Offscreen reserved affine matrix: never capability-rejected. */
    SnapReset();
    SetOam(8, 300, 20, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    AddCommand(8, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.capabilityFlags == NATIVE_OBJ_CAP_NONE);
    assert(sOut.commandRejectedCount == 0);

    printf("3B: capability rejection per reason ... ok\n");
}

/* ---- 9. Stage 3D affine drawing (normal + double-size) ---- */

static void TestAffineDrawing(void)
{
    /* Identity 8x8: draws exactly like non-affine (AssertFullSprite walks the
     * center-origin box). Matrix loaded via the presented OAM layout. */
    SnapReset();
    SetOam(16, 20, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(16, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(16, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.capabilityFlags == NATIVE_OBJ_CAP_NONE);
    assert(sOut.commandRejectedCount == 0);
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[16], 16, 0, false);

    /* H-mirror via pa=-0x100: same box, mirrored texels. Independent-model walk
     * plus hand-computed pixels: center=(24,34); screen (21,34) samples tex
     * (7,4) (mirror) vs (1,4) for identity. */
    SnapReset();
    SetOam(16, 20, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(16, 4);
    SetAffineMatrix(4, -0x100, 0, 0, 0x100);
    AddCommand(16, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[16], 16, 0, false);
    AssertLayerPresent(&sOut, 0, 21, 34, ExpectedColor(0, ExpectedPixelIndex(0, 7, 4)),
                       OwnOf(16, 0, false));
    AssertLayerAbsent(&sOut, 0, 20, 34); /* tex 8 -> culled by the mirror */

    /* V-scale 2x (pd=0x200): box pixels that sample tex>=8 or <0 are blank. */
    SnapReset();
    SetOam(17, 60, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 1, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(17, 5);
    SetAffineMatrix(5, 0x100, 0, 0, 0x200);
    AddCommand(17, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[17], 17, 0, false);

    /* 2x both axes: texture zoomed into the box center. */
    SnapReset();
    SetOam(18, 100, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 2, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(18, 6);
    SetAffineMatrix(6, 0x200, 0, 0, 0x200);
    AddCommand(18, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[18], 18, 0, false);

    /* Shrink 0x80: whole texture stretched across the box. */
    SnapReset();
    SetOam(19, 140, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 3, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(19, 7);
    SetAffineMatrix(7, 0x80, 0, 0, 0x80);
    AddCommand(19, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[19], 19, 0, false);

    /* 90-degree rotation: pa=0 pb=-0x100 pc=0x100 pd=0. Center=(24,34);
     * screen (21,31) has local(-3,-3) -> tex(7,1). */
    SnapReset();
    SetOam(20, 20, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(20, 8);
    SetAffineMatrix(8, 0, -0x100, 0x100, 0);
    AddCommand(20, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[20], 20, 0, false);
    AssertLayerPresent(&sOut, 0, 21, 31, ExpectedColor(0, ExpectedPixelIndex(4, 7, 1)),
                       OwnOf(20, 0, false));

    /* Non-square wide 32x16, identity: full box coverage. */
    SnapReset();
    SetOam(21, 20, 60, ST_OAM_H_RECTANGLE, ST_OAM_SIZE_1, 5, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(21, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(21, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[21], 21, 0, false);

    /* Partially offscreen (left edge x=-6 canonicalizes to -6). */
    SnapReset();
    SetOam(22, -6, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 6, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(22, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(22, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[22], 22, 0, false);

    /* Affine double-size identity: box [x,x+2w)x[y,y+2h), texture only near the
     * center, corners blank -- the bidirectional validator is required. */
    SnapReset();
    SetOam(23, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 7, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_DOUBLE, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(23, 9);
    SetAffineMatrix(9, 0x100, 0, 0, 0x100);
    AddCommand(23, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    assert(sOut.capabilityFlags == NATIVE_OBJ_CAP_NONE);
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[23], 23, 0, false);

    /* Transparent texel: zero tile-7 (8,8) texel (0,0); identity double-size
     * center samples tex(4,4), so screen (44,44) draws, texel (0,0) maps to
     * screen (40,40) which must be absent. */
    SnapReset();
    SetOam(24, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 8, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_DOUBLE, 0, ST_OAM_4BPP, false, false);
    ZeroObjPixel(8, 0, 0);
    SetOamMatrixNum(24, 9);
    SetAffineMatrix(9, 0x100, 0, 0, 0x100);
    AddCommand(24, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertLayerAbsent(&sOut, 0, 40, 40); /* samples tex (0,0) which is 0 */
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[24], 24, 0, false);

    /* Affine ignores the matrixNum flip bits: matrixNum=9|8 pins matrix 9 with
     * the HFLIP bit set; matrix 9 is identity, so the sprite draws unmirrored. */
    SnapReset();
    SetOam(25, 80, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 9, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(25, 9 | 8);
    SetAffineMatrix(9, 0x100, 0, 0, 0x100);
    AddCommand(25, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[25], 25, 0, false);

    /* Matrix reuse: two sprites share matrix 3 (identity). */
    SnapReset();
    SetOam(26, 20, 80, ST_OAM_SQUARE, ST_OAM_SIZE_0, 10, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOam(27, 60, 80, ST_OAM_SQUARE, ST_OAM_SIZE_0, 11, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(26, 3);
    SetOamMatrixNum(27, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(26, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(27, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    RasterizeOk();
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[26], 26, 0, false);
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[27], 27, 0, false);

    /* Different matrix indices in one frame: 16 mirror (matrix 4) + 28 identity
     * (matrix 3). */
    SnapReset();
    SetOam(16, 20, 100, ST_OAM_SQUARE, ST_OAM_SIZE_0, 12, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOam(28, 100, 100, ST_OAM_SQUARE, ST_OAM_SIZE_0, 13, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(16, 4);
    SetOamMatrixNum(28, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    SetAffineMatrix(4, -0x100, 0, 0, 0x100);
    AddCommand(16, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(28, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    RasterizeOk();
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[16], 16, 0, false);
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[28], 28, 0, false);

    /* Edge/rounding: pa=0x180 (1.5x) exercises fractional 8.8 sampling. */
    SnapReset();
    SetOam(29, 30, 120, ST_OAM_SQUARE, ST_OAM_SIZE_1, 14, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(29, 5);
    SetAffineMatrix(5, 0x180, 0, 0, 0x180);
    AddCommand(29, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    RasterizeOk();
    AssertAffineSprite(&sSnap, &sOut, &sSnap.finalOam[29], 29, 0, false);

    /* Overlap with a non-affine OBJ at equal priority: OAM 30 (affine at (54,50),
     * higher index) partially overlaps OAM 10 (normal at (50,50), lower index).
     * The lower OAM index wins the shared region; the affine's exclusive region
     * (sx>=58) draws the affine sprite. Centers: normal (54,54), affine (58,54). */
    SnapReset();
    SetOam(10, 50, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 15, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    SetOam(30, 54, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 16, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(30, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(10, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    AddCommand(30, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    RasterizeOk();
    AssertLayerPresent(&sOut, 0, 56, 53, ExpectedColor(0, ExpectedPixelIndex(15, 6, 3)),
                       OwnOf(10, 0, false)); /* normal wins the overlap */
    AssertLayerPresent(&sOut, 0, 60, 53, ExpectedColor(0, ExpectedPixelIndex(16, 6, 3)),
                       OwnOf(30, 0, false)); /* affine's exclusive region */

    printf("3B: Stage 3D affine drawing (normal + double-size) ... ok\n");
}

/* ---- 10. 2D mapping + NULL input ---- */

static void Test2DMapping(void)
{
    SnapReset();
    sSnap.dispCnt = DISPCNT_OBJ_ON; /* 2D OBJ mapping */
    SetOam(0, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);

    assert(!NativeObjRender_Rasterize(&sSnap, &sOut));
    assert(sOut.produced == FALSE);
    assert(sOut.capabilityFlags & NATIVE_OBJ_CAP_2D_MAPPING);
    AssertLayerAbsent(&sOut, 0, 12, 12); /* no layers touched */

    assert(!NativeObjRender_Rasterize(NULL, &sOut));
    assert(!NativeObjRender_Rasterize(&sSnap, NULL));

    printf("3B: 2D mapping + NULL input ... ok\n");
}

/* ---- 10. blend config predicate ---- */

static void TestBlendConfig(void)
{
    SnapReset();
    SetOam(0, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 0, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);

    /* Persistent field blend 0x1E40 (alpha, TGT1 empty): neutral. */
    sSnap.bldCnt = 0x1E40;
    RasterizeOk();
    assert(!(sOut.capabilityFlags & NATIVE_OBJ_CAP_BLEND_CONFIG));
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[0], 0, 0, false);

    /* Genuine OBJ blend: TGT1_OBJ + alpha mode -> flagged, still produced. */
    sSnap.bldCnt = BLDCNT_TGT1_OBJ | BLDCNT_EFFECT_BLEND;
    RasterizeOk();
    assert(sOut.capabilityFlags & NATIVE_OBJ_CAP_BLEND_CONFIG);
    assert(sOut.produced == TRUE);
    AssertFullSprite(&sSnap, &sOut, &sSnap.finalOam[0], 0, 0, false);

    /* Brightness modes also recolor normal OBJ -> flagged. */
    sSnap.bldCnt = BLDCNT_TGT1_OBJ | BLDCNT_EFFECT_LIGHTEN;
    RasterizeOk();
    assert(sOut.capabilityFlags & NATIVE_OBJ_CAP_BLEND_CONFIG);

    printf("3B: blend config predicate ... ok\n");
}

int main(void)
{
    TestBasicSprite();
    TestShapeSizeMatrix();
    TestMultiTileAddressing();
    TestFlips();
    TestPositionClipping();
    TestOrderingOverlap();
    TestSubspriteMultiCommand();
    TestCapabilityRejection();
    TestAffineDrawing();
    Test2DMapping();
    TestBlendConfig();
    printf("native obj renderer unit test passed\n");
    return 0;
}

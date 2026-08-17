#ifndef GUARD_NATIVE_OBJ_RENDERER_TEST_SHARED_H
#define GUARD_NATIVE_OBJ_RENDERER_TEST_SHARED_H

/*
 * Shared Stage 3B test model: the module under test plus a deterministic OBJ
 * tile/palette model and snapshot-construction helpers. Included once per test
 * TU (module unit test, real-oracle harness), so the two can never drift:
 * both build the same snapshot, and the oracle harness additionally publishes
 * that snapshot into the live OAM/VRAM/PLTT/register state the real
 * gba_easy_draw.c DrawFrame reads.
 *
 * TILE MODEL: tile t's 4bpp pixel at texel (tx,ty) is
 *   index = 1 + ((t*7 + ty*5 + tx) % 15)            (1..15, never transparent)
 * so every tile is opaque and its identity is verifiable from a single pixel.
 * Palette bank b, index p -> color 0x0100 + b*16 + p (bank in bits 8-11,
 * index in bits 0-7, bit 15 clear). Tests override specific nibbles to 0 to
 * create transparent texels.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/platform/native_obj_renderer.c"

/* Large output is caller-allocated static storage (as in production). */
static struct NativeObjSnapshot sSnap;
static struct NativeObjRenderOutput sOut;

/* --- deterministic model --- */

static u8 ExpectedPixelIndex(int tile, int texX, int texY)
{
    return (u8)(1 + ((tile * 7 + texY * 5 + texX) % 15));
}

static u16 ExpectedColor(int paletteNum, int pixel)
{
    return (u16)(0x0100 + paletteNum * 16 + pixel);
}

static const u8 sTestSizes[][2] =
{
    {  8, 16 },
    {  8, 32 },
    { 16, 32 },
    { 32, 64 },
};

static void ModelFillObjVram(u8 *objVram)
{
    int t;
    for (t = 0; t < 1024; t++)
    {
        for (int ty = 0; ty < 8; ty++)
        {
            for (int k = 0; k < 4; k++)
            {
                u8 lo = ExpectedPixelIndex(t, 2 * k, ty);
                u8 hi = ExpectedPixelIndex(t, 2 * k + 1, ty);
                objVram[t * 32 + ty * 4 + k] = (u8)(lo | (hi << 4));
            }
        }
    }
}

/* Clear one 4bpp nibble (pixel index 0 -> transparent) in sSnap.objVram. */
static void ZeroObjPixel(int tile, int texX, int texY)
{
    u32 byteIdx = (u32)tile * 32 + texY * 4 + texX / 2;
    if (texX & 1)
        sSnap.objVram[byteIdx] &= 0x0F;
    else
        sSnap.objVram[byteIdx] &= 0xF0;
}

static void ModelFillObjPalette(u16 *objPal)
{
    int b;
    for (b = 0; b < 16; b++)
        for (int p = 0; p < 16; p++)
            objPal[b * 16 + p] = ExpectedColor(b, p);
}

static void SnapReset(void)
{
    struct OamData dummy;
    u8 i;

    memset(&sSnap, 0, sizeof(sSnap));
    sSnap.schemaVersion = NATIVE_OBJ_SNAPSHOT_SCHEMA_VERSION;
    sSnap.valid = TRUE;
    sSnap.dispCnt = DISPCNT_OBJ_1D_MAP | DISPCNT_OBJ_ON;
    sSnap.bldCnt = 0;
    ModelFillObjVram(sSnap.objVram);
    ModelFillObjPalette(sSnap.objPalette);

    /* Dummy filler entries (y=160, x=304, prio 3, 8x8): never presented. */
    memset(&dummy, 0, sizeof(dummy));
    dummy.y = DISPLAY_HEIGHT;
    dummy.x = DISPLAY_WIDTH + 64;
    dummy.priority = 3;
    dummy.shape = ST_OAM_SQUARE;
    dummy.size = ST_OAM_SIZE_0;
    for (i = 0; i < OAM_ENTRY_COUNT; i++)
        sSnap.finalOam[i] = dummy;
}

/* Packed 9-bit x / 8-bit y (the game's UpdateOamCoords wrap). Passing negative
 * values exercises the signed wrap: x=-1 -> 511, x=-7 -> 505, y=-1 -> 255. */
static void SetOam(u8 index, int x, int y, u8 shape, u8 size, u16 tileNum,
                   u8 paletteNum, u8 priority, u8 objMode, u8 affineMode,
                   u8 mosaic, u8 bpp, bool flipX, bool flipY)
{
    struct OamData *o = &sSnap.finalOam[index];

    memset(o, 0, sizeof(*o));
    o->y = (u32)(y & 0xFF);
    o->x = (u32)(x & 0x1FF);
    o->affineMode = affineMode;
    o->objMode = objMode;
    o->mosaic = mosaic;
    o->bpp = bpp;
    o->shape = shape;
    o->matrixNum = (flipX ? 8 : 0) | (flipY ? 16 : 0);
    o->size = size;
    o->tileNum = tileNum;
    o->priority = priority;
    o->paletteNum = paletteNum;
}

/* Stage 3D: point an OAM entry at affine matrix `matrixNum` (full 5-bit value;
 * for affine sprites bits 3-4 of matrixNum are part of the matrix index, NOT
 * flip bits -- the GBA packs them together). */
static void SetOamMatrixNum(u8 index, u8 matrixNum)
{
    sSnap.finalOam[index].matrixNum = (u32)(matrixNum & 0x1F);
}

/* Stage 3D: write matrix `matrixNum`'s 8.8 params into the presented OAM layout
 * the oracle reads (OAM[matrixNum*4 + k].affineParam). The matrix holder
 * entries keep the dummy offscreen y/x so they are never presented as sprites. */
static void SetAffineMatrix(u8 matrixNum, s16 pa, s16 pb, s16 pc, s16 pd)
{
    u32 m = (u32)matrixNum * 4;

    sSnap.finalOam[m + 0].affineParam = (u16)pa;
    sSnap.finalOam[m + 1].affineParam = (u16)pb;
    sSnap.finalOam[m + 2].affineParam = (u16)pc;
    sSnap.finalOam[m + 3].affineParam = (u16)pd;
}

static void AddCommand(u8 oamIndex, u8 sourceKind, u8 spriteId, u8 partIndex)
{
    struct NativeSpriteDrawCommand *c = &sSnap.commands[sSnap.commandCount++];
    assert(sSnap.commandCount <= NATIVE_SPRITE_COMMAND_MAX);
    memset(c, 0, sizeof(*c));
    c->commandId = sSnap.commandCount - 1;
    c->emissionOrdinal = sSnap.commandCount - 1;
    c->sourceKind = sourceKind;
    c->spriteId = spriteId;
    c->partIndex = partIndex;
    c->oamIndex = oamIndex;
}

/* Independent re-implementation of the sampler geometry (GBA OBJ spec + oracle
 * semantics, affine INCLUDED -- Stage 3D): returns TRUE and fills the
 * deterministic color when the entry would claim screen pixel (sx,sy). */
static bool SampleExpected(const struct OamData *oam, const struct NativeObjSnapshot *snap,
                           int sx, int sy, u16 *colorOut, u8 *pixelOut)
{
    int width, height;
    int32_t x, y;
    bool isAffine;
    int halfWidth, halfHeight;
    int localX, localY, texX, texY;

    if (oam->shape == 3)
        return false;
    isAffine = (oam->affineMode & 1) != 0;
    if (!isAffine && ((oam->affineMode >> 1) & 1))
        return false;

    if (oam->shape == ST_OAM_SQUARE)
    {
        width = (1 << oam->size) * 8;
        height = (1 << oam->size) * 8;
    }
    else if (oam->shape == ST_OAM_H_RECTANGLE)
    {
        width = sTestSizes[oam->size][1];
        height = sTestSizes[oam->size][0];
    }
    else
    {
        width = sTestSizes[oam->size][0];
        height = sTestSizes[oam->size][1];
    }

    halfWidth = width / 2;
    halfHeight = height / 2;
    if (isAffine && ((oam->affineMode >> 1) & 1))
    {
        // Affine double-size: raster half-extents double, texture stays original.
        halfWidth  *= 2;
        halfHeight *= 2;
    }

    x = oam->x;
    y = oam->y;
    if (x >= DISPLAY_WIDTH)
        x -= 512;
    if (y >= DISPLAY_HEIGHT)
        y -= 256;
    x += halfWidth;
    y += halfHeight;

    // vcount gate is HALF-OPEN at +half_height; the local_x loop is INCLUSIVE at
    // +half_width (the extra right column culls itself for the identity matrix).
    if (sy < y - halfHeight || sy >= y + halfHeight)
        return false;
    if (sx < x - halfWidth || sx > x + halfWidth)
        return false;
    if (sx < 0 || sx >= DISPLAY_WIDTH)
        return false;

    localX = sx - x;
    localY = sy - y;
    if (isAffine)
    {
        u32 m = (u32)oam->matrixNum * 4;
        int pa = (s16)snap->finalOam[m + 0].affineParam;
        int pb = (s16)snap->finalOam[m + 1].affineParam;
        int pc = (s16)snap->finalOam[m + 2].affineParam;
        int pd = (s16)snap->finalOam[m + 3].affineParam;

        texX = (pa * localX + pb * localY) >> 8;
        texY = (pc * localX + pd * localY) >> 8;
        texX += width / 2;
        texY += height / 2;
    }
    else
    {
        texX = localX + (width / 2);
        texY = localY + (height / 2);
    }
    if (texX < 0 || texX >= width || texY < 0 || texY >= height)
        return false;

    // Affine sprites ignore the matrixNum flip bits (they are matrix-index bits).
    if (!isAffine)
    {
        if ((oam->matrixNum >> 3) & 1)
            texX = width - texX - 1;
        if ((oam->matrixNum >> 4) & 1)
            texY = height - texY - 1;
    }

    int tileX = texX % 8;
    int tileY = texY % 8;
    int blockX = texX / 8;
    int blockY = texY / 8;
    int rowStride = (snap->dispCnt & DISPCNT_OBJ_1D_MAP) ? (width / 8) : 16;
    int blockOffset = blockY * rowStride + blockX;

    u8 byte = snap->objVram[(blockOffset + oam->tileNum) * 32 + tileY * 4 + tileX / 2];
    u8 pixel = (tileX & 1) ? (u8)(byte >> 4) : (u8)(byte & 0xF);
    if (pixel == 0)
        return false;

    *pixelOut = pixel;
    *colorOut = snap->objPalette[oam->paletteNum * 16 + pixel];
    return true;
}

static u16 OwnOf(u8 oamIndex, u8 priority, bool raw)
{
    u16 own = NATIVE_OBJ_OWN_PRESENT
            | ((u16)priority << NATIVE_OBJ_OWN_PRIORITY_SHIFT)
            | oamIndex;
    if (raw)
        own |= NATIVE_OBJ_OWN_RAW;
    return own;
}

static void AssertLayerPresent(const struct NativeObjRenderOutput *out, u8 priority,
                               u8 x, u8 y, u16 expColor, u16 expOwn)
{
    u32 idx = (u32)y * DISPLAY_WIDTH + x;
    u16 c = out->layers[priority].color[idx];
    u16 o = out->layers[priority].ownership[idx];

    if (c != (expColor | 0x8000) || o != expOwn)
    {
        fprintf(stderr,
                "layer prio=%u at (%u,%u): color=0x%04X exp=0x%04X own=0x%04X expOwn=0x%04X\n",
                priority, x, y, c, expColor | 0x8000, o, expOwn);
        assert(c == (expColor | 0x8000));
        assert(o == expOwn);
    }
}

static void AssertLayerAbsent(const struct NativeObjRenderOutput *out, u8 priority,
                              u8 x, u8 y)
{
    u32 idx = (u32)y * DISPLAY_WIDTH + x;
    assert(out->layers[priority].color[idx] == 0);
    assert(out->layers[priority].ownership[idx] == 0);
}

/* Rasterize sSnap into sOut and require full production. */
static void RasterizeOk(void)
{
    assert(NativeObjRender_Rasterize(&sSnap, &sOut));
    assert(sOut.produced == TRUE);
}

/* For every pixel of the OAM entry's on-screen raster rect, assert presence +
 * color against the independent model, and assert the pixels just outside the
 * rect are absent. ownership is built from the provided oamIndex/priority/raw.
 * The rect is the oracle's center-origin box: half-extents doubled for affine
 * double-size, and the right column INCLUSIVE for affine (the extra column
 * culls itself for the identity matrix). */
static void AssertFullSprite(const struct NativeObjSnapshot *snap,
                             const struct NativeObjRenderOutput *out,
                             const struct OamData *oam, u8 oamIndex, u8 priority,
                             bool raw)
{
    int width, height;
    int32_t x, y;
    int halfWidth, halfHeight;
    int leftX, rightX, topY, bottomY;
    int sx, sy;
    bool isAffine;
    u16 expOwn = OwnOf(oamIndex, priority, raw);

    if (oam->shape == ST_OAM_SQUARE)
    {
        width = (1 << oam->size) * 8;
        height = (1 << oam->size) * 8;
    }
    else if (oam->shape == ST_OAM_H_RECTANGLE)
    {
        width = sTestSizes[oam->size][1];
        height = sTestSizes[oam->size][0];
    }
    else
    {
        width = sTestSizes[oam->size][0];
        height = sTestSizes[oam->size][1];
    }

    isAffine = (oam->affineMode & 1) != 0;
    halfWidth = width / 2;
    halfHeight = height / 2;
    if (isAffine && ((oam->affineMode >> 1) & 1))
    {
        halfWidth  *= 2;
        halfHeight *= 2;
    }

    x = oam->x;
    y = oam->y;
    if (x >= DISPLAY_WIDTH)
        x -= 512;
    if (y >= DISPLAY_HEIGHT)
        y -= 256;
    x += halfWidth;
    y += halfHeight;

    leftX  = (int)(x - halfWidth);
    rightX = (int)(x + halfWidth);
    topY   = (int)(y - halfHeight);
    bottomY = (int)(y + halfHeight);
    if (isAffine)
        rightX++;

    for (sy = topY; sy < bottomY; sy++)
    {
        if (sy < 0 || sy >= DISPLAY_HEIGHT)
            continue;
        for (sx = leftX; sx < rightX; sx++)
        {
            u16 color;
            u8 pixel;
            if (sx < 0 || sx >= DISPLAY_WIDTH)
                continue;
            assert(SampleExpected(oam, snap, sx, sy, &color, &pixel));
            AssertLayerPresent(out, priority, (u8)sx, (u8)sy, color, expOwn);
        }
    }

    /* Just-outside columns/rows must be unclaimed (exact rect boundary). */
    if (topY - 1 >= 0 && leftX < DISPLAY_WIDTH && rightX > 0)
        for (sx = leftX; sx < rightX; sx++)
            if (sx >= 0 && sx < DISPLAY_WIDTH)
                AssertLayerAbsent(out, priority, (u8)sx, (u8)(topY - 1));
    if (bottomY < DISPLAY_HEIGHT)
        for (sx = leftX; sx < rightX; sx++)
            if (sx >= 0 && sx < DISPLAY_WIDTH)
                AssertLayerAbsent(out, priority, (u8)sx, (u8)bottomY);
    if (leftX - 1 >= 0)
        for (sy = topY; sy < bottomY; sy++)
            if (sy >= 0 && sy < DISPLAY_HEIGHT)
                AssertLayerAbsent(out, priority, (u8)(leftX - 1), (u8)sy);
    if (rightX < DISPLAY_WIDTH)
        for (sy = topY; sy < bottomY; sy++)
            if (sy >= 0 && sy < DISPLAY_HEIGHT)
                AssertLayerAbsent(out, priority, (u8)rightX, (u8)sy);
}

/* Bidirectional affine validator (Stage 3D): for EVERY pixel of the affine
 * raster box, require the native layer to agree exactly with the independent
 * model -- present when SampleExpected draws (correct color + ownership),
 * absent when it does not (scaled/rotated matrices leave box pixels blank when
 * the transform samples out-of-range texels). */
static void AssertAffineSprite(const struct NativeObjSnapshot *snap,
                               const struct NativeObjRenderOutput *out,
                               const struct OamData *oam, u8 oamIndex, u8 priority,
                               bool raw)
{
    int width, height;
    int32_t x, y;
    int halfWidth, halfHeight;
    int leftX, rightX, topY, bottomY;
    int sx, sy;
    u16 expOwn = OwnOf(oamIndex, priority, raw);

    if (oam->shape == ST_OAM_SQUARE)
    {
        width = (1 << oam->size) * 8;
        height = (1 << oam->size) * 8;
    }
    else if (oam->shape == ST_OAM_H_RECTANGLE)
    {
        width = sTestSizes[oam->size][1];
        height = sTestSizes[oam->size][0];
    }
    else
    {
        width = sTestSizes[oam->size][0];
        height = sTestSizes[oam->size][1];
    }

    halfWidth = width / 2;
    halfHeight = height / 2;
    if ((oam->affineMode >> 1) & 1)
    {
        halfWidth  *= 2;
        halfHeight *= 2;
    }

    x = oam->x;
    y = oam->y;
    if (x >= DISPLAY_WIDTH)
        x -= 512;
    if (y >= DISPLAY_HEIGHT)
        y -= 256;
    x += halfWidth;
    y += halfHeight;

    leftX   = (int)(x - halfWidth);
    rightX  = (int)(x + halfWidth) + 1; /* inclusive right column */
    topY    = (int)(y - halfHeight);
    bottomY = (int)(y + halfHeight);

    for (sy = topY; sy < bottomY; sy++)
    {
        if (sy < 0 || sy >= DISPLAY_HEIGHT)
            continue;
        for (sx = leftX; sx < rightX; sx++)
        {
            u16 color;
            u8 pixel;

            if (sx < 0 || sx >= DISPLAY_WIDTH)
                continue;
            if (SampleExpected(oam, snap, sx, sy, &color, &pixel))
                AssertLayerPresent(out, priority, (u8)sx, (u8)sy, color, expOwn);
            else
                AssertLayerAbsent(out, priority, (u8)sx, (u8)sy);
        }
    }

    /* Just-outside columns/rows must be unclaimed (exact box boundary). */
    if (topY - 1 >= 0 && leftX < DISPLAY_WIDTH && rightX > 0)
        for (sx = leftX; sx < rightX; sx++)
            if (sx >= 0 && sx < DISPLAY_WIDTH)
                AssertLayerAbsent(out, priority, (u8)sx, (u8)(topY - 1));
    if (bottomY < DISPLAY_HEIGHT)
        for (sx = leftX; sx < rightX; sx++)
            if (sx >= 0 && sx < DISPLAY_WIDTH)
                AssertLayerAbsent(out, priority, (u8)sx, (u8)bottomY);
    if (leftX - 1 >= 0)
        for (sy = topY; sy < bottomY; sy++)
            if (sy >= 0 && sy < DISPLAY_HEIGHT)
                AssertLayerAbsent(out, priority, (u8)(leftX - 1), (u8)sy);
    if (rightX < DISPLAY_WIDTH)
        for (sy = topY; sy < bottomY; sy++)
            if (sy >= 0 && sy < DISPLAY_HEIGHT)
                AssertLayerAbsent(out, priority, (u8)rightX, (u8)sy);
}

#endif // GUARD_NATIVE_OBJ_RENDERER_TEST_SHARED_H

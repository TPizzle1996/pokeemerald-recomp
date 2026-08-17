#if defined(PLATFORM_SDL2) && defined(LINUX64) && LINUX64

#include "global.h"
#include <stdbool.h>
#include "gba/io_reg.h"
#include "platform/native_obj_renderer.h"

/*
 * Stage 3B/3D OBJ-only CPU rasterizer (see the header for the contract).
 *
 * This module renders the 4bpp 1D-mapped half of the presentation -- NON-AFFINE
 * OBJ plus the AFFINE (normal and double-size) OBJ added by Stage 3D -- replay
 * the gba_easy_draw.c DrawSprites oracle (lines 489-699) over the snapshot's
 * FINAL packed OAM and captured OBJ VRAM / OBJ palette, producing the same
 * per-OBJ-priority layer buffers the oracle would write into its
 * scanline->spriteLayers -- four 240x160 layers of (color | 1<<15), plus a
 * compact per-pixel ownership marker for Stage 3C and the sample trace.
 *
 * DELIBERATE PARITY DECISIONS (mirror the oracle exactly):
 *
 *   - Iterate OAM index 127..0 with overwrite semantics, so at equal OBJ
 *     priority the LOWEST OAM index wins the pixel. No host-side re-sort.
 *   - Canonicalize x/y from the packed fields: x>=240 -> x-=512, y>=160 ->
 *     y-=256. The pre-wrap signedX/signedY in the snapshot are preserved and
 *     ignored for 240x160 geometry (the packed OAM is authoritative).
 *   - Skip non-affine entries with the double-size bit set SILENTLY (the oracle
 *     `continue`s; it is a disabled entry, not an unsupported feature).
 *   - No bounds-checking of the OBJ tile fetch: (blockOffset + tileNum) is
 *     indexed raw exactly like the oracle's tiledata[...] read. Tests must keep
 *     the effective tile within the 1024 OBJ tiles (0x8000 / 32 bytes).
 *   - Transparent index 0 never writes a pixel.
 *
 * AFFINE (Stage 3D): the matrix is read from the snapshot's OWN finalOam at
 *   entries [matrixNum*4 .. matrixNum*4+3].affineParam -- the exact layout the
 *   oracle reads from the live OAM region (sprite.c CopyMatricesToOamBuffer
 *   writes gOamMatrices there, LoadOam copies it, and the snapshot capture
 *   memcpy's OAM). The matrix is therefore the value COMMITTED for the same
 *   presented frame, never the live/next-frame gOamMatrices. Center-origin
 *   8.8 sampling: tex = ((pa*localX + pb*localY) >> 8) + width/2 (signed right
 *   shift), double-size bounds double the half-extents only, affine ignores the
 *   matrixNum flip bits, and the oracle's affine local_x loop is INCLUSIVE at
 *   +half_width while its scanline gate is half-open at +half_height -- both
 *   edges are reproduced exactly (a rotated/scaled matrix CAN draw in the extra
 *   right-hand column).
 *
 * CAPABILITY BOUNDARY (unsupported -> explicit reject record + flag):
 *
 *   Only primitives whose canonical bounding rect intersects the 240x160
 *   viewport are "presented" and capability-checked. Fully offscreen entries --
 *   including reserved offscreen affine matrices -- are skipped silently and
 *   never cause a fallback. A frame whose DISPCNT uses 2D OBJ mapping is not
 *   producible at all (produced=FALSE, NATIVE_OBJ_CAP_2D_MAPPING). 8bpp,
 *   OBJ-window, and mosaic primitives are recorded as rejects (also when carried
 *   by an otherwise-supported affine sprite). Semi-transparent OBJ (objMode==1)
 *   is SUPPORTED (Stage 3E): it is sampled with identical geometry/texel/
 *   priority semantics and its per-pixel mode is preserved in layer->semi[]; the
 *   sampler never alpha-blends -- the Stage 3E compositor does, at composite
 *   time, from the BG layers and BLDCNT/BLDALPHA. A frame whose BLDCNT would
 *   recolor NORMAL OBJ pixels (TGT1_OBJ set with a non-zero blend mode) is
 *   flagged NATIVE_OBJ_CAP_BLEND_CONFIG: this raster stays pre-blend. The
 *   persistent emerald BLDCNT=0x1E40 is neutral here (TGT1_OBJ clear), so real
 *   gameplay never sets the flag.
 */

// OBJ tile dimensions in bytes for a 4bpp tile (8x8, 32 bytes).
#define NATIVE_OBJ_TILE_BYTES 32

// Sprite size table identical to gba_easy_draw.c's spriteSizes.
static const u8 sSpriteSizes[][2] =
{
    {  8, 16 },
    {  8, 32 },
    { 16, 32 },
    { 32, 64 },
};

static void ObjDimensions(u8 shape, u8 size, int *width, int *height)
{
    if (shape == ST_OAM_SQUARE)
    {
        *width  = (1 << size) * 8;
        *height = (1 << size) * 8;
    }
    else if (shape == ST_OAM_H_RECTANGLE)
    {
        *width  = sSpriteSizes[size][1];
        *height = sSpriteSizes[size][0];
    }
    else // ST_OAM_V_RECTANGLE
    {
        *width  = sSpriteSizes[size][0];
        *height = sSpriteSizes[size][1];
    }
}

/*
 * Reconstruct the oracle's DrawSprites computation for ONE screen pixel of ONE
 * final OAM entry (affine or identity; no mosaic -- mosaic is rejected
 * upstream). Returns TRUE exactly when the oracle would claim this pixel in this
 * priority layer, and fills the sampled color plus geometry. This is the shared
 * seam between the rasterizer and the on-demand trace, so the two cannot drift.
 */
static bool SampleOamPixel(const struct NativeObjSnapshot *snap,
                           const struct OamData *oam,
                           int screenX, int screenY,
                           u8 *paletteNumOut, u16 *colorOut,
                           u16 *tileNumOut, u8 *texXOut, u8 *texYOut,
                           u8 *pixelIndexOut)
{
    int width, height;
    int32_t x;
    int32_t y;
    int halfWidth, halfHeight;
    int localX, localY, texX, texY;
    bool isAffine;

    if (oam->shape == 3)
        return FALSE; // prohibited -- never drawn by the oracle

    isAffine = (oam->affineMode & 1) != 0;
    // Non-affine with the double-size bit set: oracle-disabled, never drawn.
    if (!isAffine && ((oam->affineMode >> 1) & 1))
        return FALSE;

    ObjDimensions(oam->shape, oam->size, &width, &height);

    halfWidth  = width / 2;
    halfHeight = height / 2;
    if (isAffine && ((oam->affineMode >> 1) & 1))
    {
        // Affine double-size: the raster half-extents double, the texture stays
        // the original size (offset below uses the ORIGINAL width/2).
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

    // vcount range check (the oracle's per-scanline gate; HALF-OPEN bottom --
    // vcount < y+half_height, so the bottom row of the box is excluded).
    if (screenY < (y - halfHeight) || screenY >= (y + halfHeight))
        return FALSE;

    localX = screenX - x;
    localY = screenY - y;
    if ((localX + x) < 0 || (localX + x) >= DISPLAY_WIDTH) // == screenX
        return FALSE;

    if (isAffine)
    {
        // Matrix read from the SAME presented OAM entries the oracle uses
        // (signed 8.8); NOT from the command's pa/pb/pc/pd, which for GSPRITE
        // commands are the live/next-frame gOamMatrices copied at sink time.
        s16 pa, pb, pc, pd;
        u32 m = (u32)oam->matrixNum * 4;

        pa = (s16)snap->finalOam[m + 0].affineParam;
        pb = (s16)snap->finalOam[m + 1].affineParam;
        pc = (s16)snap->finalOam[m + 2].affineParam;
        pd = (s16)snap->finalOam[m + 3].affineParam;
        // Center-origin 8.8 fixed-point, identical arithmetic to the oracle
        // (signed `>> 8`; the extra right-hand column is decided by the cull).
        texX = ((int)pa * localX + (int)pb * localY) >> 8;
        texY = ((int)pc * localX + (int)pd * localY) >> 8;
        texX += width / 2;
        texY += height / 2;
        // Affine sprites ignore the matrixNum flip bits.
    }
    else
    {
        bool flipX = ((oam->matrixNum >> 3) & 1) != 0; // ST_OAM_HFLIP
        bool flipY = ((oam->matrixNum >> 4) & 1) != 0; // ST_OAM_VFLIP

        // Identity transform, no mosaic (mosaic is rejected upstream).
        texX = localX + (width / 2);
        texY = localY + (height / 2);
        if (flipX)
            texX = width - texX - 1;
        if (flipY)
            texY = height - texY - 1;
    }

    if (texX >= width || texY >= height || texX < 0 || texY < 0)
        return FALSE;

    int tileX = texX % 8;
    int tileY = texY % 8;
    int blockX = texX / 8;
    int blockY = texY / 8;
    int rowStride = (snap->dispCnt & DISPCNT_OBJ_1D_MAP) ? (width / 8) : 16;
    int blockOffset = blockY * rowStride + blockX;

    // 4bpp fetch only (8bpp is rejected upstream). Mirrors the oracle's raw
    // indexing; the effective tile is expected to stay within OBJ VRAM.
    u8 pixel = snap->objVram[(blockOffset + oam->tileNum) * NATIVE_OBJ_TILE_BYTES
                             + tileY * 4 + tileX / 2];
    if (tileX & 1)
        pixel >>= 4;
    else
        pixel &= 0xF;

    if (pixel == 0)
        return FALSE; // transparent index 0 never draws

    *paletteNumOut = oam->paletteNum;
    *colorOut = snap->objPalette[oam->paletteNum * 16 + pixel];
    *tileNumOut = oam->tileNum;
    *texXOut = (u8)texX;
    *texYOut = (u8)texY;
    *pixelIndexOut = pixel;
    return TRUE;
}

/*
 * Stage 4A: viewport-space twin of SampleOamPixel for an EXPANDED viewport.
 * The primitive's top-left placement (px, py) is already in viewport screen
 * coordinates (no 9/8-bit canonicalization -- the wrap happened when the
 * presentation command or the packed OAM was turned into placement), and the
 * affine matrix is passed in explicitly (authoritative finalOam for presented
 * entries, the command's sink-time pa/pb/pc/pd for revealed ones). Bounds are
 * the viewport dims, not the 240x160 display. Geometry/texel semantics
 * (affine inclusive-right-column + half-open-bottom gates, identity flips, 4bpp
 * fetch, transparent index 0) are identical to SampleOamPixel.
 */
static bool SampleObjPixelViewport(const struct NativeObjSnapshot *snap,
                                   const struct OamData *oam,
                                   s16 pa, s16 pb, s16 pc, s16 pd,
                                   s32 px, s32 py,
                                   int viewportWidth, int viewportHeight,
                                   int screenX, int screenY,
                                   u8 *paletteNumOut, u16 *colorOut,
                                   u16 *tileNumOut, u8 *texXOut, u8 *texYOut,
                                   u8 *pixelIndexOut)
{
    int width, height;
    int halfWidth, halfHeight;
    int localX, localY, texX, texY;
    bool isAffine;
    int32_t x, y;

    if (oam->shape == 3)
        return FALSE; // prohibited -- never drawn by the oracle

    isAffine = (oam->affineMode & 1) != 0;
    // Non-affine with the double-size bit set: oracle-disabled, never drawn.
    if (!isAffine && ((oam->affineMode >> 1) & 1))
        return FALSE;

    ObjDimensions(oam->shape, oam->size, &width, &height);

    halfWidth  = width / 2;
    halfHeight = height / 2;
    if (isAffine && ((oam->affineMode >> 1) & 1))
    {
        // Affine double-size: the raster half-extents double, the texture stays
        // the original size.
        halfWidth  *= 2;
        halfHeight *= 2;
    }

    x = px;
    y = py;
    x += halfWidth;
    y += halfHeight;

    // vcount range check (the oracle's per-scanline gate; HALF-OPEN bottom).
    if (screenY < (y - halfHeight) || screenY >= (y + halfHeight))
        return FALSE;

    localX = screenX - x;
    localY = screenY - y;
    if ((localX + x) < 0 || (localX + x) >= viewportWidth) // == screenX
        return FALSE;

    if (isAffine)
    {
        texX = ((int)pa * localX + (int)pb * localY) >> 8;
        texY = ((int)pc * localX + (int)pd * localY) >> 8;
        texX += width / 2;
        texY += height / 2;
        // Affine sprites ignore the matrixNum flip bits.
    }
    else
    {
        bool flipX = ((oam->matrixNum >> 3) & 1) != 0; // ST_OAM_HFLIP
        bool flipY = ((oam->matrixNum >> 4) & 1) != 0; // ST_OAM_VFLIP

        // Identity transform, no mosaic (mosaic is rejected upstream).
        texX = localX + (width / 2);
        texY = localY + (height / 2);
        if (flipX)
            texX = width - texX - 1;
        if (flipY)
            texY = height - texY - 1;
    }

    if (texX >= width || texY >= height || texX < 0 || texY < 0)
        return FALSE;

    int tileX = texX % 8;
    int tileY = texY % 8;
    int blockX = texX / 8;
    int blockY = texY / 8;
    int rowStride = (snap->dispCnt & DISPCNT_OBJ_1D_MAP) ? (width / 8) : 16;
    int blockOffset = blockY * rowStride + blockX;

    u8 pixel = snap->objVram[(blockOffset + oam->tileNum) * NATIVE_OBJ_TILE_BYTES
                             + tileY * 4 + tileX / 2];
    if (tileX & 1)
        pixel >>= 4;
    else
        pixel &= 0xF;

    if (pixel == 0)
        return FALSE; // transparent index 0 never draws

    *paletteNumOut = oam->paletteNum;
    *colorOut = snap->objPalette[oam->paletteNum * 16 + pixel];
    *tileNumOut = oam->tileNum;
    *texXOut = (u8)texX;
    *texYOut = (u8)texY;
    *pixelIndexOut = pixel;
    return TRUE;
}

static void BuildProvenanceMap(const struct NativeObjSnapshot *snap,
                               const struct NativeSpriteDrawCommand **map)
{
    u32 c;
    for (c = 0; c < OAM_ENTRY_COUNT; c++)
        map[c] = NULL;
    for (c = 0; c < snap->commandCount && c < NATIVE_SPRITE_COMMAND_MAX; c++)
    {
        const struct NativeSpriteDrawCommand *cmd = &snap->commands[c];
        if (cmd->oamIndex < OAM_ENTRY_COUNT)
            map[cmd->oamIndex] = cmd;
    }
}

static void RecordReject(struct NativeObjRenderOutput *out, u8 oamIndex, u8 reason,
                         const struct NativeSpriteDrawCommand *cmd)
{
    if (out->commandRejectedCount >= NATIVE_SPRITE_COMMAND_MAX)
        return;
    struct NativeObjRejectRecord *r = &out->rejects[out->commandRejectedCount++];
    r->oamIndex = oamIndex;
    r->reason = reason;
    r->commandId = (cmd != NULL) ? (u8)cmd->commandId : NATIVE_OBJ_NO_ID;
    r->reserved = 0;
}

// Does the canonical non-affine bounding rect intersect the 240x160 viewport?
static bool RectIntersectsViewport(const struct OamData *oam, int width, int height)
{
    int32_t x = oam->x;
    int32_t y = oam->y;
    if (x >= DISPLAY_WIDTH)
        x -= 512;
    if (y >= DISPLAY_HEIGHT)
        y -= 256;
    return (x < DISPLAY_WIDTH) && (y < DISPLAY_HEIGHT)
        && (x + width > 0) && (y + height > 0);
}

// Stage 4A: does a rect already placed in viewport screen coordinates (px/py)
// intersect the viewport? Drives "presented" (capability rejection) for the
// expanded path, where placement comes from the presentation commands.
static bool RectIntersectsScreenRect(s32 px, s32 py, int width, int height,
                                     int viewportWidth, int viewportHeight)
{
    return (px < viewportWidth) && (py < viewportHeight)
        && (px + width > 0) && (py + height > 0);
}

/*
 * Stage 4A: the exact historical 240x160 rasterization loop (byte-identical to
 * the pre-Stage-4A NativeObjRender_Rasterize). Iterates final OAM 127..0 with
 * overwrite semantics, canonicalizes x>=240->x-=512 / y>=160->y-=256, places by
 * the packed OAM (the 240x160 authority), and samples with SampleOamPixel. The
 * shared preamble (NULL check, output clear, 2D-mapping audit, blend-config
 * flag, provenance map) lives in NativeObjRender_RasterizeEx.
 */
static void RasterizeOam240x160(const struct NativeObjSnapshot *snap,
                                struct NativeObjRenderOutput *out,
                                const struct NativeSpriteDrawCommand *const *prov)
{
    int i;

    for (i = 127; i >= 0; i--)
    {
        const struct OamData *oam = &snap->finalOam[i];
        const struct NativeSpriteDrawCommand *cmd = prov[i];
        int width, height;
        bool isAffine;
        bool isDoubleSize;
        bool isSemiTransparent;
        bool isObjWin;
        bool rawSource;
        int halfWidth, halfHeight;
        int32_t x, y;
        int topY, bottomY, leftX, rightX, sy, sx;

        if (oam->shape == 3)
        {
            // Prohibited configuration: the oracle skips it silently. Stage 3B
            // records it explicitly so no unsupported feature is silent.
            out->capabilityFlags |= NATIVE_OBJ_CAP_PROHIBITED_SHAPE;
            RecordReject(out, (u8)i, NATIVE_OBJ_REJECT_PROHIBITED_SHAPE, cmd);
            continue;
        }

        isAffine = (oam->affineMode & 1) != 0;
        isDoubleSize = ((oam->affineMode >> 1) & 1) != 0;
        if (!isAffine && isDoubleSize)
            continue; // oracle-disabled entry -- silent, no reject

        ObjDimensions(oam->shape, oam->size, &width, &height);

        // Rect test drives "presented": offscreen entries (including reserved
        // offscreen affine matrices) are never capability-rejected. Affine
        // double-size bounds the rect at twice the sprite size (the oracle
        // doubles rect_width/rect_height before the scanline intersection).
        if (isAffine && isDoubleSize)
        {
            if (!RectIntersectsViewport(oam, width * 2, height * 2))
                continue;
        }
        else if (!RectIntersectsViewport(oam, width, height))
        {
            continue;
        }

        // Capability gates. Affine and semi-transparent are supported (normal +
        // double-size; objMode==1), so the remaining rejects are the per-
        // primitive features the raster does NOT draw: 8bpp, OBJ-window, mosaic.
        // A semi-transparent sprite (Stage 3E) is sampled with identical
        // geometry/texel/priority semantics and its mode is preserved in
        // layer->semi[]; the sampler does NOT perform the alpha blend itself --
        // the Stage 3E compositor does, from the BG layers and BLDCNT/BLDALPHA.
        // An affine sprite carrying one of the rejected features is rejected for
        // THAT reason, not for being affine.
        if (oam->bpp & 1)
        {
            out->capabilityFlags |= NATIVE_OBJ_CAP_8BPP;
            RecordReject(out, (u8)i, NATIVE_OBJ_REJECT_8BPP, cmd);
            continue;
        }

        isSemiTransparent = (oam->objMode == 1);
        isObjWin = (oam->objMode == 2);
        if (isObjWin)
        {
            out->capabilityFlags |= NATIVE_OBJ_CAP_OBJ_WINDOW;
            RecordReject(out, (u8)i, NATIVE_OBJ_REJECT_OBJ_WINDOW, cmd);
            continue;
        }
        if (oam->mosaic == 1)
        {
            out->capabilityFlags |= NATIVE_OBJ_CAP_MOSAIC;
            RecordReject(out, (u8)i, NATIVE_OBJ_REJECT_MOSAIC, cmd);
            continue;
        }

        // Supported primitive (normal or affine): rasterize over the clipped
        // on-screen rect, in the exact oracle order (lowest OAM index overwrites
        // at equal priority).
        x = oam->x;
        y = oam->y;
        if (x >= DISPLAY_WIDTH)
            x -= 512;
        if (y >= DISPLAY_HEIGHT)
            y -= 256;

        halfWidth  = width / 2;
        halfHeight = height / 2;
        if (isAffine && isDoubleSize)
        {
            halfWidth  *= 2;
            halfHeight *= 2;
        }
        x += halfWidth;
        y += halfHeight;

        leftX  = (int)(x - halfWidth);
        rightX = (int)(x + halfWidth);
        topY   = (int)(y - halfHeight);
        bottomY = (int)(y + halfHeight);
        // The oracle's affine loop is local_x=-half_width..+half_width INCLUSIVE
        // (the right column can draw for rotated/scaled matrices), while the
        // scanline gate is half-open at +half_height. Reproduce both edges
        // exactly; for identity the inclusive column is culled by the tex check.
        if (isAffine)
            rightX++;
        if (leftX < 0)
            leftX = 0;
        if (rightX > DISPLAY_WIDTH)
            rightX = DISPLAY_WIDTH;
        if (topY < 0)
            topY = 0;
        if (bottomY > DISPLAY_HEIGHT)
            bottomY = DISPLAY_HEIGHT;

        if (topY >= bottomY || leftX >= rightX)
            continue; // clipped away entirely

        rawSource = (cmd == NULL || cmd->sourceKind == NATIVE_SPRITE_SOURCE_RAW_OAM);
        if (rawSource)
            out->rawDrawnCount++;
        else
            out->gspriteDrawnCount++;

        {
            struct NativeObjLayer *layer = &out->layers[oam->priority];
            u16 ownBase = NATIVE_OBJ_OWN_PRESENT
                        | ((u16)(oam->priority) << NATIVE_OBJ_OWN_PRIORITY_SHIFT)
                        | (u16)i;
            if (rawSource)
                ownBase |= NATIVE_OBJ_OWN_RAW;

            for (sy = topY; sy < bottomY; sy++)
            {
                for (sx = leftX; sx < rightX; sx++)
                {
                    u16 color;
                    u8 paletteNum, pixelIndex;
                    u16 tileNum;
                    u8 texX, texY;
                    if (SampleOamPixel(snap, oam, sx, sy,
                                       &paletteNum, &color, &tileNum,
                                       &texX, &texY, &pixelIndex))
                    {
                        size_t idx = (size_t)sy * DISPLAY_WIDTH + sx;
                        layer->color[idx] = color | (1 << 15);
                        layer->ownership[idx] = ownBase;
                        // Stage 3E: preserve the winning primitive's objMode so
                        // the compositor alpha-blends semi pixels exactly where
                        // the oracle does (`|| isSemiTransparent` at draw time).
                        layer->semi[idx] = isSemiTransparent ? 1 : 0;
                    }
                }
            }
        }
    }
}

/*
 * Stage 4A: expanded-viewport rasterization of the presented final-OAM entries.
 * Iterates final OAM 127..0 for ordering (same overwrite semantics, so at equal
 * priority the lowest OAM index wins), but PLACES each entry from the Stage 3A
 * presentation commands when a validated GSPRITE WORLD/CAMERA_RELATIVE command
 * exists: px = signedX + margin (its world position moves outward with the
 * viewport). RAW_OAM / UNKNOWN / out-of-range commands are placed at
 * canonicalized-OAM + margin, keeping them in the 240x160 center (SCREEN_FIXED
 * policy). Affine matrices stay authoritative (read from the snapshot's own
 * finalOam). Capability gates are unchanged.
 */
static void RasterizeOamExpanded(const struct NativeObjSnapshot *snap,
                                 struct NativeObjRenderOutput *out,
                                 const struct NativeSpriteDrawCommand *const *prov,
                                 int viewportWidth, int viewportHeight,
                                 s32 marginX, s32 marginY)
{
    int i;

    for (i = 127; i >= 0; i--)
    {
        const struct OamData *oam = &snap->finalOam[i];
        const struct NativeSpriteDrawCommand *cmd = prov[i];
        int width, height;
        bool isAffine;
        bool isDoubleSize;
        bool isSemiTransparent;
        bool isObjWin;
        bool rawSource;
        int halfWidth, halfHeight;
        int32_t x, y, px, py;
        int topY, bottomY, leftX, rightX, sy, sx;
        s16 pa = 0, pb = 0, pc = 0, pd = 0;

        if (oam->shape == 3)
        {
            out->capabilityFlags |= NATIVE_OBJ_CAP_PROHIBITED_SHAPE;
            RecordReject(out, (u8)i, NATIVE_OBJ_REJECT_PROHIBITED_SHAPE, cmd);
            continue;
        }

        isAffine = (oam->affineMode & 1) != 0;
        isDoubleSize = ((oam->affineMode >> 1) & 1) != 0;
        if (!isAffine && isDoubleSize)
            continue; // oracle-disabled entry -- silent, no reject

        ObjDimensions(oam->shape, oam->size, &width, &height);

        // Placement. A validated GSPRITE WORLD/CAMERA_RELATIVE command with an
        // in-range signed coordinate (where signedX/Y == canonicalized final
        // OAM) drives the position; everything else stays in the 240x160
        // center via the canonicalized OAM + margin.
        x = oam->x;
        y = oam->y;
        if (x >= DISPLAY_WIDTH)
            x -= 512;
        if (y >= DISPLAY_HEIGHT)
            y -= 256;
        if (cmd != NULL
         && cmd->sourceKind == NATIVE_SPRITE_SOURCE_GSPRITE
         && (cmd->placement == NATIVE_SPRITE_PLACEMENT_WORLD
             || cmd->placement == NATIVE_SPRITE_PLACEMENT_CAMERA_RELATIVE)
         && cmd->signedX >= -272 && cmd->signedX <= 239
         && cmd->signedY >= -176 && cmd->signedY <= 159)
        {
            px = cmd->signedX + marginX;
            py = cmd->signedY + marginY;
        }
        else
        {
            px = x + marginX;
            py = y + marginY;
        }

        // Rect test drives "presented": entries not intersecting the viewport are
        // never capability-rejected. Affine double-size bounds at 2x.
        if (isAffine && isDoubleSize)
        {
            if (!RectIntersectsScreenRect(px, py, width * 2, height * 2,
                                          viewportWidth, viewportHeight))
                continue;
        }
        else if (!RectIntersectsScreenRect(px, py, width, height,
                                           viewportWidth, viewportHeight))
        {
            continue;
        }

        // Capability gates (identical to the 240x160 path).
        if (oam->bpp & 1)
        {
            out->capabilityFlags |= NATIVE_OBJ_CAP_8BPP;
            RecordReject(out, (u8)i, NATIVE_OBJ_REJECT_8BPP, cmd);
            continue;
        }
        isSemiTransparent = (oam->objMode == 1);
        isObjWin = (oam->objMode == 2);
        if (isObjWin)
        {
            out->capabilityFlags |= NATIVE_OBJ_CAP_OBJ_WINDOW;
            RecordReject(out, (u8)i, NATIVE_OBJ_REJECT_OBJ_WINDOW, cmd);
            continue;
        }
        if (oam->mosaic == 1)
        {
            out->capabilityFlags |= NATIVE_OBJ_CAP_MOSAIC;
            RecordReject(out, (u8)i, NATIVE_OBJ_REJECT_MOSAIC, cmd);
            continue;
        }

        // Affine matrix: authoritative finalOam (same entries the oracle reads).
        if (isAffine)
        {
            u32 m = (u32)oam->matrixNum * 4;
            pa = (s16)snap->finalOam[m + 0].affineParam;
            pb = (s16)snap->finalOam[m + 1].affineParam;
            pc = (s16)snap->finalOam[m + 2].affineParam;
            pd = (s16)snap->finalOam[m + 3].affineParam;
        }

        halfWidth  = width / 2;
        halfHeight = height / 2;
        if (isAffine && isDoubleSize)
        {
            halfWidth  *= 2;
            halfHeight *= 2;
        }
        x = px + halfWidth;
        y = py + halfHeight;

        leftX  = (int)(x - halfWidth);
        rightX = (int)(x + halfWidth);
        topY   = (int)(y - halfHeight);
        bottomY = (int)(y + halfHeight);
        // Same affine inclusive-right-column / half-open-bottom edges as the
        // oracle (and as the 240x160 loop above).
        if (isAffine)
            rightX++;
        if (leftX < 0)
            leftX = 0;
        if (rightX > viewportWidth)
            rightX = viewportWidth;
        if (topY < 0)
            topY = 0;
        if (bottomY > viewportHeight)
            bottomY = viewportHeight;

        if (topY >= bottomY || leftX >= rightX)
            continue; // clipped away entirely

        rawSource = (cmd == NULL || cmd->sourceKind == NATIVE_SPRITE_SOURCE_RAW_OAM);
        if (rawSource)
            out->rawDrawnCount++;
        else
            out->gspriteDrawnCount++;

        {
            struct NativeObjLayer *layer = &out->layers[oam->priority];
            u16 ownBase = NATIVE_OBJ_OWN_PRESENT
                        | ((u16)(oam->priority) << NATIVE_OBJ_OWN_PRIORITY_SHIFT)
                        | (u16)i;
            if (rawSource)
                ownBase |= NATIVE_OBJ_OWN_RAW;

            for (sy = topY; sy < bottomY; sy++)
            {
                for (sx = leftX; sx < rightX; sx++)
                {
                    u16 color;
                    u8 paletteNum, pixelIndex;
                    u16 tileNum;
                    u8 texX, texY;
                    if (SampleObjPixelViewport(snap, oam, pa, pb, pc, pd,
                                               px, py, viewportWidth, viewportHeight,
                                               sx, sy, &paletteNum, &color, &tileNum,
                                               &texX, &texY, &pixelIndex))
                    {
                        size_t idx = (size_t)sy * viewportWidth + sx;
                        layer->color[idx] = color | (1 << 15);
                        layer->ownership[idx] = ownBase;
                        layer->semi[idx] = isSemiTransparent ? 1 : 0;
                    }
                }
            }
        }
    }
}

/*
 * Stage 4A: reveal VANILLA_VIEWPORT_CULLED expandedEligible commands (sprites the
 * vanilla 240x160 viewport culled but the expanded apron now shows). Placed from
 * the command's pre-wrap presentation coordinates (signedX + margin), geometry
 * from the command fields (no final OAM slot; the affine matrix is the command's
 * sink-time pa/pb/pc/pd -- a documented <=1-frame approximation). Rasterizes
 * AFTER all final OAM entries and never overwrites a presented pixel at equal
 * priority, in unpresented[] order (deterministic).
 */
static void RasterizeRevealedCommands(const struct NativeObjSnapshot *snap,
                                      struct NativeObjRenderOutput *out,
                                      int viewportWidth, int viewportHeight,
                                      s32 marginX, s32 marginY)
{
    u32 c;

    for (c = 0; c < snap->unpresentedCount && c < NATIVE_SPRITE_UNPRESENTED_MAX; c++)
    {
        const struct NativeSpriteDrawCommand *cmd = &snap->unpresented[c];
        struct OamData oamLocal;
        int width, height;
        bool isAffine;
        bool isDoubleSize;
        bool isSemiTransparent;
        int halfWidth, halfHeight;
        s32 x, y;
        int topY, bottomY, leftX, rightX, sy, sx;

        if (cmd->visibility != NATIVE_SPRITE_VANILLA_VIEWPORT_CULLED
         || !cmd->expandedEligible)
            continue;

        isAffine = (cmd->affineMode & 1) != 0;
        isDoubleSize = ((cmd->affineMode >> 1) & 1) != 0;
        if (!isAffine && isDoubleSize)
            continue; // oracle-disabled configuration -- silent

        // Synthesize an OamData for the sampler geometry (the command has no
        // final OAM slot). Placement is passed separately as px/py.
        memset(&oamLocal, 0, sizeof(oamLocal));
        oamLocal.shape = cmd->shape;
        oamLocal.size = cmd->size;
        oamLocal.tileNum = cmd->tileNum;
        oamLocal.paletteNum = cmd->paletteNum;
        oamLocal.priority = cmd->priority;
        oamLocal.bpp = cmd->bpp;
        oamLocal.objMode = cmd->objMode;
        oamLocal.affineMode = cmd->affineMode;
        oamLocal.mosaic = cmd->mosaic;
        oamLocal.matrixNum = (u16)((cmd->flipX ? (1 << 3) : 0)
                                 | (cmd->flipY ? (1 << 4) : 0));

        ObjDimensions(cmd->shape, cmd->size, &width, &height);

        {
            // Command pre-wrap presentation coordinates + margin = viewport
            // screen position.
            s32 px = cmd->signedX + marginX;
            s32 py = cmd->signedY + marginY;

            // Capability gates apply to any revealed primitive intersecting the
            // viewport (same reasons as the presented path).
            if (isAffine && isDoubleSize)
            {
                if (!RectIntersectsScreenRect(px, py, width * 2, height * 2,
                                              viewportWidth, viewportHeight))
                    continue;
            }
            else if (!RectIntersectsScreenRect(px, py, width, height,
                                               viewportWidth, viewportHeight))
            {
                continue;
            }

            if (cmd->bpp & 1)
            {
                out->capabilityFlags |= NATIVE_OBJ_CAP_8BPP;
                RecordReject(out, NATIVE_OBJ_NO_ID, NATIVE_OBJ_REJECT_8BPP, cmd);
                continue;
            }
            isSemiTransparent = (cmd->objMode == 1);
            if (cmd->objMode == 2)
            {
                out->capabilityFlags |= NATIVE_OBJ_CAP_OBJ_WINDOW;
                RecordReject(out, NATIVE_OBJ_NO_ID, NATIVE_OBJ_REJECT_OBJ_WINDOW, cmd);
                continue;
            }
            if (cmd->mosaic == 1)
            {
                out->capabilityFlags |= NATIVE_OBJ_CAP_MOSAIC;
                RecordReject(out, NATIVE_OBJ_NO_ID, NATIVE_OBJ_REJECT_MOSAIC, cmd);
                continue;
            }

            halfWidth  = width / 2;
            halfHeight = height / 2;
            if (isAffine && isDoubleSize)
            {
                halfWidth  *= 2;
                halfHeight *= 2;
            }
            x = px + halfWidth;
            y = py + halfHeight;

            leftX  = (int)(x - halfWidth);
            rightX = (int)(x + halfWidth);
            topY   = (int)(y - halfHeight);
            bottomY = (int)(y + halfHeight);
            if (isAffine)
                rightX++;
            if (leftX < 0)
                leftX = 0;
            if (rightX > viewportWidth)
                rightX = viewportWidth;
            if (topY < 0)
                topY = 0;
            if (bottomY > viewportHeight)
                bottomY = viewportHeight;

            if (topY >= bottomY || leftX >= rightX)
                continue; // clipped away entirely

            out->gspriteDrawnCount++;

            {
                struct NativeObjLayer *layer = &out->layers[cmd->priority];
                // No final OAM slot: NATIVE_OBJ_NO_ID ownership. GSPRITE
                // provenance (revealed commands never carry the RAW bit).
                u16 ownBase = NATIVE_OBJ_OWN_PRESENT
                            | ((u16)(cmd->priority) << NATIVE_OBJ_OWN_PRIORITY_SHIFT)
                            | NATIVE_OBJ_NO_ID;

                for (sy = topY; sy < bottomY; sy++)
                {
                    for (sx = leftX; sx < rightX; sx++)
                    {
                        size_t idx = (size_t)sy * viewportWidth + sx;
                        u16 color;
                        u8 paletteNum, pixelIndex;
                        u16 tileNum;
                        u8 texX, texY;

                        // Presented (final OAM) sprites win at equal priority:
                        // never overwrite a pixel already claimed in this layer.
                        if (layer->ownership[idx] & NATIVE_OBJ_OWN_PRESENT)
                            continue;
                        if (!SampleObjPixelViewport(snap, &oamLocal,
                                                    cmd->pa, cmd->pb, cmd->pc, cmd->pd,
                                                    px, py, viewportWidth, viewportHeight,
                                                    sx, sy, &paletteNum, &color, &tileNum,
                                                    &texX, &texY, &pixelIndex))
                            continue;
                        layer->color[idx] = color | (1 << 15);
                        layer->ownership[idx] = ownBase;
                        layer->semi[idx] = isSemiTransparent ? 1 : 0;
                    }
                }
            }
        }
    }
}

bool32 NativeObjRender_Rasterize(const struct NativeObjSnapshot *snap,
                                 struct NativeObjRenderOutput *out)
{
    return NativeObjRender_RasterizeEx(snap, out, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0);
}

bool32 NativeObjRender_RasterizeEx(const struct NativeObjSnapshot *snap,
                                   struct NativeObjRenderOutput *out,
                                   u16 viewportWidth, u16 viewportHeight,
                                   s32 marginX, s32 marginY)
{
    const struct NativeSpriteDrawCommand *prov[OAM_ENTRY_COUNT];

    if (snap == NULL || out == NULL)
        return FALSE;
    // Bounds: the native buffers are max-capacity 360x240; a viewport must be
    // a valid non-zero rectangle within them.
    if (viewportWidth == 0 || viewportHeight == 0
     || viewportWidth > NATIVE_VIEWPORT_MAX_WIDTH
     || viewportHeight > NATIVE_VIEWPORT_MAX_HEIGHT)
        return FALSE;

    // Clear the full output (large; caller-provided storage) and set metadata.
    memset(out, 0, sizeof(*out));
    out->snapshotValid = snap->valid;

    // Frame-wide OBJ mapping audit: 2D character mapping is not producible.
    if (!(snap->dispCnt & DISPCNT_OBJ_1D_MAP))
    {
        out->produced = FALSE;
        out->capabilityFlags |= NATIVE_OBJ_CAP_2D_MAPPING;
        return FALSE;
    }

    out->produced = TRUE;

    // A non-zero blend mode with OBJ as a blend target recolors every normal OBJ
    // pixel in the oracle (alpha or brightness); this raster stays pre-blend.
    if ((snap->bldCnt & BLDCNT_TGT1_OBJ) && (((snap->bldCnt >> 6) & 3) != 0))
        out->capabilityFlags |= NATIVE_OBJ_CAP_BLEND_CONFIG;

    BuildProvenanceMap(snap, prov);

    // 240x160: the exact historical loop (byte-identical output).
    if (viewportWidth == DISPLAY_WIDTH && viewportHeight == DISPLAY_HEIGHT
     && marginX == 0 && marginY == 0)
    {
        RasterizeOam240x160(snap, out, prov);
        return TRUE;
    }

    // Expanded viewport: presented entries placed by command, then revealed
    // culled-but-eligible commands (presented wins at equal priority).
    RasterizeOamExpanded(snap, out, prov, viewportWidth, viewportHeight, marginX, marginY);
    RasterizeRevealedCommands(snap, out, viewportWidth, viewportHeight, marginX, marginY);
    return TRUE;
}

bool32 NativeObjRender_Trace(const struct NativeObjSnapshot *snap,
                             const struct NativeObjRenderOutput *out,
                             u8 priority, u8 x, u8 y,
                             struct NativeObjSampleTrace *trace)
{
    const struct NativeSpriteDrawCommand *cmd;
    const struct OamData *oam;
    u16 own;
    u8 oamIndex;
    u8 paletteNum, pixelIndex;
    u16 tileNum, color;
    u8 texX, texY;

    if (snap == NULL || out == NULL || trace == NULL)
        return FALSE;
    if (priority >= NATIVE_OBJ_PRIORITY_LAYERS || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT)
        return FALSE;

    own = out->layers[priority].ownership[(size_t)y * DISPLAY_WIDTH + x];
    if (!(own & NATIVE_OBJ_OWN_PRESENT))
    {
        trace->present = FALSE;
        return FALSE;
    }

    oamIndex = (u8)(own & NATIVE_OBJ_OWN_OAM_MASK);
    oam = &snap->finalOam[oamIndex];

    // Replay the sampler for the winning entry; ownership and geometry must
    // agree, otherwise the marker is inconsistent.
    if (!SampleOamPixel(snap, oam, x, y,
                        &paletteNum, &color, &tileNum, &texX, &texY, &pixelIndex))
        return FALSE;

    cmd = NULL;
    for (u32 c = 0; c < snap->commandCount && c < NATIVE_SPRITE_COMMAND_MAX; c++)
    {
        if (snap->commands[c].oamIndex == oamIndex)
        {
            cmd = &snap->commands[c];
            break;
        }
    }

    trace->present = TRUE;
    trace->priority = priority;
    trace->oamIndex = oamIndex;
    trace->commandId = (cmd != NULL) ? (u8)cmd->commandId : NATIVE_OBJ_NO_ID;
    trace->sourceKind = (cmd != NULL) ? cmd->sourceKind : NATIVE_SPRITE_SOURCE_RAW_OAM;
    trace->spriteId = (cmd != NULL) ? cmd->spriteId : NATIVE_OBJ_NO_ID;
    trace->partIndex = (cmd != NULL) ? cmd->partIndex : 0;
    trace->paletteNum = paletteNum;
    trace->pixelIndex = pixelIndex;
    trace->tileNum = tileNum;
    trace->texX = texX;
    trace->texY = texY;
    trace->shape = oam->shape;
    trace->size = oam->size;
    trace->affineMode = oam->affineMode;
    trace->objMode = oam->objMode;
    trace->mosaic = oam->mosaic;
    trace->bpp = oam->bpp;
    trace->flipX = ((oam->matrixNum >> 3) & 1) != 0;
    trace->flipY = ((oam->matrixNum >> 4) & 1) != 0;
    trace->color = color & 0x7FFF; // pre-blend, bit 15 clear
    return TRUE;
}

#endif // PLATFORM_SDL2 && LINUX64

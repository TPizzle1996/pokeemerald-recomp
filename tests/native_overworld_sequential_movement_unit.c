/*
 * Stage 4A §5 sequential-movement residency tests (A-G).
 *
 * Background: the ring's tracked world-coverage window (UpdateRingCoverageWindow)
 * is PHASE-DEPENDENT -- it can only be recovered across rendered frames, not from
 * a single snapshot, because the u8 tile offset wraps mod 32 while the player
 * walks. The load layout covers cells [pos.x, pos.x+15); after an EAST/SOUTH
 * crossing the ring holds cells [pos.x-1, pos.x+14) (window start pos*16-16);
 * after a WEST/NORTH crossing cells [pos.x, pos.x+15). A residency test that
 * re-derives the window from cameraMapX/xTileOffset alone is wrong for west /
 * north movement (the offset wraps 0 -> 30 -> 28 and shifts the claimed window
 * 256px west, so the margins show ring content of an unrelated world cell).
 *
 * These tests drive a synthetic field-camera simulation built from the REAL
 * field_camera.c mechanics -- CameraUpdate's crossing rules, the u8 mod-32 tile
 * offset, AddCameraTileOffset/PixelOffset, DrawWholeMapViewInternal /
 * RedrawMapSlice* cell-to-column geometry, and the one-frame latched-scroll lag
 * (bgHofs/bgVofs = the PREVIOUS frame's pixel offset; cameraX/Y the current) --
 * then render EVERY frame of a walk through the real DrawMapFrameWithMetaEx at
 * 300x200 and assert:
 *
 *   - every MARGIN pixel equals the independent world reference: resident cells
 *     (per the test's own phase tracking) show the true cell content (the ring is
 *     filled from the grid, so ring == world), non-resident cells show the uniform
 *     M0 grid fallback;
 *   - the central 240x160 window (spot-checked) matches the presentation-formula
 *     ring content -- the scroll-lagged GBA view, a hard parity invariant;
 *   - the ring physically holds the just-entered cell at its anchored column and
 *     the just-evicted cell is gone (AssertRingCell / eviction render checks).
 *
 * Cell content is encoded with BOTH palette and solid tile value so that cells 16
 * apart (the stale-aliasing distance) produce DIFFERENT colors: pal = (cellX+cellY)
 * mod 16, tile = 1 + (cellX/16 mod 4)*3 + (cellY/16 mod 4) with char[tile] solid
 * value == tile. A newly revealed coordinate can therefore never silently inherit a
 * stale physical ring cell from a different world coordinate. M0 (the grid
 * fallback) is tile 901 pal 0 (solid pixel 15), distinct from every ring cell.
 *
 * Expected colors are computed INDEPENDENTLY from the sim's ring/grid/palette data
 * and the GBA sampling formulas -- never via the renderer's resolve/sample
 * functions -- so the test cannot agree with the renderer on a wrong provider
 * choice. Self-contained: links only native_overworld_renderer.c + viewport.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"

#undef NDEBUG
#include <assert.h>

/* ---- Stubs of the hardware globals the renderer TU references ---- */

/* BG0 screenbase used by the snapshot's bg0Cnt (BG0 content gate only). */
#define SCREENBASE_BG0 15

unsigned char REG_BASE[0x400] __attribute__((aligned(4)));
unsigned char PLTT[PLTT_SIZE] __attribute__((aligned(4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));
unsigned char OAM[OAM_SIZE] __attribute__((aligned(4)));

typedef void (*IntrFunc)(void);
IntrFunc gIntrTable[8] = {0};

void RunDMAs(u32 type)
{
    (void)type;
}

/* ---- Scene scaffold ---- */

#define TEST_GRID_WIDTH  96
#define TEST_GRID_HEIGHT 64

/* Uniform M0 fallback metatile (see header comment): BG1 tile 901 pal 0. */
#define TEST_META_GRID 1

static struct NativeOverworldSnapshot sBgSnap;
static u16 sPrimaryMetatiles[NUM_METATILES_IN_PRIMARY * NUM_TILES_PER_METATILE];
static u16 sPrimaryAttributes[NUM_METATILES_IN_PRIMARY];
static u16 sSecondaryMetatiles[NUM_METATILES_TOTAL * NUM_TILES_PER_METATILE];
static u16 sSecondaryAttributes[NUM_METATILES_TOTAL];
static u16 sBorderMetatiles[4];

static u16 BgTile(u16 tile, u8 pal)
{
    return (u16)(tile | ((u16)pal << 12));
}

static void FillBgPalette(void)
{
    int b;
    int p;

    for (b = 0; b < 16; b++)
        for (p = 0; p < 16; p++)
            ((u16 *)PLTT)[b * 16 + p] = (u16)(0x0100 + b * 16 + p);
}

/* Solid-char model: tile 0 transparent; tiles 1..15 solid with pixel value ==
 * tile number; tile 901 solid 15 (M0). Every ring cell uses a tile in 1..13. */
static void FillBgCharMemory(void)
{
    int t;
    int i;

    memset(VRAM_, 0, VRAM_SIZE);
    for (t = 1; t <= 15; t++)
        for (i = 0; i < 32; i++)
            VRAM_[t * 32 + i] = (u8)(t * 0x11);
    for (i = 0; i < 32; i++)
        VRAM_[901 * 32 + i] = 0xFF;
}

static void SceneReset(void)
{
    int q;
    int ry;
    int rx;

    memset(VRAM_, 0, VRAM_SIZE);
    memset(PLTT, 0, PLTT_SIZE);
    memset(&sBgSnap, 0, sizeof(sBgSnap));
    memset(sPrimaryMetatiles, 0, sizeof(sPrimaryMetatiles));
    memset(sPrimaryAttributes, 0, sizeof(sPrimaryAttributes));
    memset(sSecondaryMetatiles, 0, sizeof(sSecondaryMetatiles));
    memset(sSecondaryAttributes, 0, sizeof(sSecondaryAttributes));
    memset(sBorderMetatiles, 0, sizeof(sBorderMetatiles));

    FillBgCharMemory();
    FillBgPalette();
    memcpy(sBgSnap.bgVram, VRAM_, 0x10000);
    memcpy(sBgSnap.palette, PLTT, PLTT_SIZE);

    sBgSnap.bg0Cnt = BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0)
                   | BGCNT_SCREENBASE(SCREENBASE_BG0) | BGCNT_16COLOR
                   | BGCNT_TXT256x256;
    sBgSnap.bgCnt[0] = BGCNT_PRIORITY(1) | BGCNT_CHARBASE(0)
                     | BGCNT_16COLOR | BGCNT_TXT256x256;
    sBgSnap.bgCnt[1] = BGCNT_PRIORITY(2) | BGCNT_CHARBASE(0)
                     | BGCNT_16COLOR | BGCNT_TXT256x256;
    sBgSnap.bgCnt[2] = BGCNT_PRIORITY(3) | BGCNT_CHARBASE(0)
                     | BGCNT_16COLOR | BGCNT_TXT256x256;

    sBgSnap.requiredCapabilities = NATIVE_CAPABILITY_MAP_BACKGROUND
                                 | NATIVE_CAPABILITY_LIVE_BG_RING
                                 | NATIVE_CAPABILITY_BG_VRAM
                                 | NATIVE_CAPABILITY_BG_PALETTE;
    sBgSnap.fallbackReason = NATIVE_FALLBACK_NONE;
    sBgSnap.bgRingValid = TRUE;
    sBgSnap.bgVramValid = TRUE;
    sBgSnap.mapGridValid = TRUE;

    sBgSnap.primaryMetatiles = sPrimaryMetatiles;
    sBgSnap.primaryMetatileAttributes = sPrimaryAttributes;
    sBgSnap.secondaryMetatiles = sSecondaryMetatiles;
    sBgSnap.secondaryMetatileAttributes = sSecondaryAttributes;
    sBgSnap.border = sBorderMetatiles;

    /* Uniform M0: metatile 1, BG1 = tile 901 pal 0, BG2/3 transparent/underlay. */
    for (q = 0; q < 4; q++)
        sPrimaryMetatiles[TEST_META_GRID * NUM_TILES_PER_METATILE + q] = 0;
    for (q = 0; q < 4; q++)
        sPrimaryMetatiles[TEST_META_GRID * NUM_TILES_PER_METATILE + 4 + q]
            = BgTile(901, 0);
    sPrimaryAttributes[TEST_META_GRID] = 0; /* NORMAL */
    sBorderMetatiles[0] = TEST_META_GRID;
    sBorderMetatiles[1] = TEST_META_GRID;
    sBorderMetatiles[2] = TEST_META_GRID;
    sBorderMetatiles[3] = TEST_META_GRID;

    sBgSnap.gridWidth = TEST_GRID_WIDTH;
    sBgSnap.gridHeight = TEST_GRID_HEIGHT;
    /* The real-map rectangle used by the dev-only provider classification's
     * grid-vs-connection apron test: every cell of this grid is real map, so a
     * non-resident in-grid coordinate classifies as GRID (blue), not CONNECTION. */
    sBgSnap.mapWidth = TEST_GRID_WIDTH;
    sBgSnap.mapHeight = TEST_GRID_HEIGHT;
    for (ry = 0; ry < TEST_GRID_HEIGHT; ry++)
        for (rx = 0; rx < TEST_GRID_WIDTH; rx++)
            sBgSnap.mapGrid[rx + TEST_GRID_WIDTH * ry] = TEST_META_GRID;
}

/* ---- Field-camera simulation (mirrors src/field_camera.c exactly) ---- */

/* Live ring (the torus the game fills): BG1 = per-cell content, BG2 transparent,
 * BG3 = the fixed 0x3014 underlay. 1024 tiles (32x32). */
static u16 ring[3][NATIVE_BG_RING_SIZE];

static int sPosX;
static int sPosY;
static u8 sXTile;
static u8 sYTile;
static s32 sCamX;   /* gFieldCamera.x -- s32, MAY go negative for west (% 16) */
static s32 sCamY;
static u8 sXPix;    /* gFieldCamera pixel offset (u8, wraps mod 256) */
static u8 sYPix;
static u8 sLatchedXPix;  /* previous frame's pixel offset (the latched bgHofs) */
static u8 sLatchedYPix;

enum TestPhase
{
    PHASE_LOAD,
    PHASE_EAST,
    PHASE_WEST,
    PHASE_SOUTH,
    PHASE_NORTH
};
static enum TestPhase sPhaseX;
static enum TestPhase sPhaseY;

static s32 RefFloorDiv(s32 value, s32 divisor)
{
    s32 q = value / divisor;
    s32 r = value % divisor;
    return (r != 0 && ((value < 0) != (divisor < 0))) ? q - 1 : q;
}

static s32 RefPosMod(s32 value, s32 divisor)
{
    s32 r = value % divisor;
    return r < 0 ? r + divisor : r;
}

/* Ring cell content encoding (see header comment): pal and tile chosen so that
 * cells 16 apart in either axis map to different colors. */
static u8 CellPal(s32 cellX, s32 cellY)
{
    return (u8)((RefPosMod(cellX, 256) + RefPosMod(cellY, 256)) & 15);
}

static u8 CellTile(s32 cellX, s32 cellY)
{
    s32 xb = (RefPosMod(cellX, 256) / 16) % 4;
    s32 yb = (RefPosMod(cellY, 256) / 16) % 4;
    return (u8)(1 + xb * 3 + yb);
}

/* True world-cell content color at world pixel (wx,wy), in the renderer's
 * palette output encoding (bit 15 = GBA alpha flag). */
static u16 TestCellColor(s32 wx, s32 wy)
{
    s32 cellX = RefFloorDiv(wx, 16);
    s32 cellY = RefFloorDiv(wy, 16);

    return (u16)(0x8100 | (CellPal(cellX, cellY) << 4) | CellTile(cellX, cellY));
}

/* Uniform grid fallback color: pal 0, tile 901 (solid pixel 15). */
#define M0_COLOR 0x810Fu

/* Write one world cell's 2x2-tile block to the ring at physical (col,row)
 * (DrawMetatileAt geometry: offset, offset+1, offset+0x20, offset+0x21). */
static void RingWriteCell(int col, int row, int cellX, int cellY)
{
    u16 entry = BgTile(CellTile(cellX, cellY), CellPal(cellX, cellY));
    int c = col & 31;
    int r = row & 31;
    int c1 = (c + 1) & 31;
    int r1 = (r + 1) & 31;

    ring[0][r * 32 + c]     = entry;
    ring[0][r * 32 + c1]    = entry;
    ring[0][r1 * 32 + c]    = entry;
    ring[0][r1 * 32 + c1]   = entry;
    ring[1][r * 32 + c]     = 0;
    ring[1][r * 32 + c1]    = 0;
    ring[1][r1 * 32 + c]    = 0;
    ring[1][r1 * 32 + c1]   = 0;
    ring[2][r * 32 + c]     = 0x3014;
    ring[2][r * 32 + c1]    = 0x3014;
    ring[2][r1 * 32 + c]    = 0x3014;
    ring[2][r1 * 32 + c1]   = 0x3014;
}

/* DrawWholeMapViewInternal: cells (posX + j/2, posY + i/2) at cols (xTile+j)%32,
 * rows (yTile+i)%32 (single-subtract mod 32, exactly as the game). */
static void FullDraw(void)
{
    int i;
    int j;

    for (i = 0; i < 32; i += 2)
    {
        int row = sYTile + i;
        if (row >= 32) row -= 32;
        for (j = 0; j < 32; j += 2)
        {
            int col = sXTile + j;
            if (col >= 32) col -= 32;
            RingWriteCell(col, row, sPosX + j / 2, sPosY + i / 2);
        }
    }
}

/* RedrawMapSliceWest: east crossing writes the just-entered cell posX+14 at the
 * ring column (xTile+28)%32. */
static void SliceWest(void)
{
    int i;
    int col = sXTile + 28;
    if (col >= 32) col -= 32;
    for (i = 0; i < 32; i += 2)
    {
        int row = sYTile + i;
        if (row >= 32) row -= 32;
        RingWriteCell(col, row, sPosX + 14, sPosY + i / 2);
    }
}

/* RedrawMapSliceEast: west crossing writes the just-entered cell posX at column
 * xTile. */
static void SliceEast(void)
{
    int i;
    for (i = 0; i < 32; i += 2)
    {
        int row = sYTile + i;
        if (row >= 32) row -= 32;
        RingWriteCell(sXTile, row, sPosX, sPosY + i / 2);
    }
}

/* RedrawMapSliceNorth: south crossing writes the just-entered cell posY+14 at
 * row (yTile+28)%32. */
static void SliceNorth(void)
{
    int i;
    int row = sYTile + 28;
    if (row >= 32) row -= 32;
    for (i = 0; i < 32; i += 2)
    {
        int col = sXTile + i;
        if (col >= 32) col -= 32;
        RingWriteCell(col, row, sPosX + i / 2, sPosY + 14);
    }
}

/* RedrawMapSliceSouth: north crossing writes the just-entered cell posY at row
 * yTile. */
static void SliceSouth(void)
{
    int i;
    for (i = 0; i < 32; i += 2)
    {
        int col = sXTile + i;
        if (col >= 32) col -= 32;
        RingWriteCell(col, sYTile, sPosX + i / 2, sPosY);
    }
}

/* CameraUpdate, replicated verbatim from field_camera.c (including the real
 * Y-branch bug at lines 406-412 that sets deltaX, not deltaY -- irrelevant for
 * pure cardinal movement, where the conditions never both fire). */
static void AdvanceCamera(s32 sx, s32 sy)
{
    s32 curX = sCamX;
    s32 curY = sCamY;
    s32 dx = 0;
    s32 dy = 0;

    if (curX == 0 && sx != 0)
        dx = sx > 0 ? 1 : -1;
    if (curY == 0 && sy != 0)
        dy = sy > 0 ? 1 : -1;
    if (curX != 0 && curX == -sx)
        dx = sx > 0 ? 1 : -1;
    if (curY != 0 && curY == -sy)
        dx = sy > 0 ? 1 : -1;   /* the real Y-branch bug, replicated faithfully */

    sCamX = (sCamX + sx) % 16;
    sCamY = (sCamY + sy) % 16;

    if (dx != 0 || dy != 0)
    {
        sPosX += dx;
        sPosY += dy;
        sXTile = (u8)(sXTile + dx * 2);
        sXTile %= 32;
        sYTile = (u8)(sYTile + dy * 2);
        sYTile %= 32;
        if (dx > 0) SliceWest();      /* RedrawMapSlicesForCameraUpdate */
        else if (dx < 0) SliceEast();
        if (dy > 0) SliceNorth();
        else if (dy < 0) SliceSouth();
        if (dx != 0)
            sPhaseX = dx > 0 ? PHASE_EAST : PHASE_WEST;
        if (dy != 0)
            sPhaseY = dy > 0 ? PHASE_SOUTH : PHASE_NORTH;
    }
    sXPix = (u8)(sXPix + sx);
    sYPix = (u8)(sYPix + sy);
}

static void FreshLoad(void)
{
    sPosX = 40;
    sPosY = 40;
    sXTile = 0;
    sYTile = 0;
    sCamX = 0;
    sCamY = 0;
    sXPix = 0;
    sYPix = 0;
    sLatchedXPix = 0;
    sLatchedYPix = 0;
    sPhaseX = PHASE_LOAD;
    sPhaseY = PHASE_LOAD;
    memset(ring, 0, sizeof(ring));
    FullDraw();
}

/* Build the snapshot a capture would take after this frame's CameraUpdate:
 * camera = logical camera AFTER movement (cameraY carries the GBA camera's +8
 * vertical offset, exactly as GetCameraOffsetWithPan); scroll = the LATCHED
 * previous frame's pixel offset (bgVofs likewise +8), i.e. one frame behind. */
static void BuildSnapshot(void)
{
    sBgSnap.cameraMapX = (s16)sPosX;
    sBgSnap.cameraMapY = (s16)sPosY;
    sBgSnap.cameraX = (s16)sXPix;
    sBgSnap.cameraY = (s16)(sYPix + 8);
    sBgSnap.cameraXTileOffset = sXTile;
    sBgSnap.cameraYTileOffset = sYTile;
    sBgSnap.bgHofs[0] = sLatchedXPix;
    sBgSnap.bgHofs[1] = sLatchedXPix;
    sBgSnap.bgHofs[2] = sLatchedXPix;
    sBgSnap.bgVofs[0] = (u16)(sLatchedYPix + 8);
    sBgSnap.bgVofs[1] = (u16)(sLatchedYPix + 8);
    sBgSnap.bgVofs[2] = (u16)(sLatchedYPix + 8);
    memcpy(sBgSnap.bgRing, ring, sizeof(ring));
}

static s32 TestOriginX(void) { return sPosX * 16 + sXPix; }
static s32 TestOriginY(void) { return sPosY * 16 + sYPix + 8; }

/* The test's OWN phase tracking of the coverage window -- the reference the
 * renderer's UpdateRingCoverageWindow must reproduce. */
static int TestResident(s32 wx, s32 wy)
{
    s32 cx = (sPhaseX == PHASE_EAST) ? sPosX * 16 - 16 : sPosX * 16;
    s32 cy = (sPhaseY == PHASE_SOUTH) ? sPosY * 16 - 16 : sPosY * 16;

    return wx >= cx && wx < cx + NATIVE_BG_RING_PIXELS
        && wy >= cy && wy < cy + NATIVE_BG_RING_PIXELS;
}

/* Presentation-formula central reference: the ring content the GBA shows at the
 * latched scroll for a central pixel (bgHofs + worldX - originX). Reads the sim
 * ring directly; the +8 in bgVofs cancels the +8 in originY. */
static u16 TestCentralColor(s32 wx, s32 wy)
{
    s32 h = sLatchedXPix + (wx - TestOriginX());
    s32 v = sLatchedYPix + 8 + (wy - TestOriginY());
    s32 idx = (((v & 0xFF) >> 3) * NATIVE_BG_RING_SIDE_TILES) + ((h & 0xFF) >> 3);
    u16 entry = ring[0][idx];

    return (u16)(0x8100 | (((entry >> 12) & 0xF) << 4) | (entry & 0x3FF));
}

/* ---- Full-viewport render + sweep ---- */

#define VP_W 300
#define VP_H 200

static u16 sFrame[VP_W * VP_H];
static u8 sWinner[VP_W * VP_H];

static void SpotCheckCentral(const char *tag, int frameNo)
{
    /* 4px inside each corner of the central window plus its center. The
     * right-edge column (originX+236) lands in the just-entered cell posX+14 for
     * sXPix in [0,4), so the newly-revealed column is verified every frame. */
    static const s32 dxs[5] = { 4, 236, 4, 236, 120 };
    static const s32 dys[5] = { 4, 4, 156, 156, 80 };
    s32 originX = TestOriginX();
    s32 originY = TestOriginY();
    int k;

    for (k = 0; k < 5; k++)
    {
        s32 wx = originX + dxs[k];
        s32 wy = originY + dys[k];
        u32 index = (u32)(dys[k] + 20) * VP_W + (dxs[k] + 30);
        u16 expected = TestCentralColor(wx, wy);

        if (sFrame[index] != expected || sWinner[index] != NATIVE_COMPOSITE_WIN_BG1)
        {
            fprintf(stderr,
                    "%s frame %d central (%d,%d): got=0x%04X win=%u expected=0x%04X\n",
                    tag, frameNo, wx, wy, sFrame[index], sWinner[index], expected);
            assert(0);
        }
    }
}

static void SweepFrame(const char *tag, int frameNo)
{
    s32 originX = TestOriginX();
    s32 originY = TestOriginY();
    s32 left = originX - 30;
    s32 top = originY - 20;
    s32 sx;
    s32 sy;

    assert(DrawMapFrameWithMetaEx(&sBgSnap, sFrame, sWinner, NULL,
                                  VP_W, VP_H, left, top));

    for (sy = 0; sy < VP_H; sy++)
    {
        for (sx = 0; sx < VP_W; sx++)
        {
            s32 wx = left + sx;
            s32 wy = top + sy;
            u32 index = (u32)sy * VP_W + sx;
            u16 expected;
            int resident;

            if (wx >= originX && wx < originX + DISPLAY_WIDTH
             && wy >= originY && wy < originY + DISPLAY_HEIGHT)
                continue;   /* central: spot-checked separately (parity window) */

            resident = TestResident(wx, wy);
            expected = resident ? TestCellColor(wx, wy) : M0_COLOR;
            if (sFrame[index] != expected || sWinner[index] != NATIVE_COMPOSITE_WIN_BG1)
            {
                fprintf(stderr,
                        "%s frame %d px(%d,%d) world(%d,%d): got=0x%04X win=%u "
                        "expected=0x%04X resident=%d\n",
                        tag, frameNo, sx, sy, wx, wy, sFrame[index],
                        sWinner[index], expected, resident);
                assert(0);
            }
        }
    }
    SpotCheckCentral(tag, frameNo);
}

/* ---- Direct ring-content checks (§5: just-entered / wrap / eviction) ---- */

/* Verifies the ring physically holds the true cell content at the ANCHORED
 * physical index of a resident world coordinate -- i.e. the newly-revealed
 * column contains the newly-entered cell, not a stale one from elsewhere. */
static void AssertRingCell(s32 worldX, s32 worldY, const char *what)
{
    s32 anchorX = sPosX * 16 - sXTile * 8;
    s32 anchorY = sPosY * 16 - sYTile * 8;
    s32 idx = (((worldY - anchorY) & 0xFF) >> 3) * NATIVE_BG_RING_SIDE_TILES
            + (((worldX - anchorX) & 0xFF) >> 3);
    u16 entry = ring[0][idx];
    u16 got = (u16)(0x8100 | (((entry >> 12) & 0xF) << 4) | (entry & 0x3FF));
    u16 expected = TestCellColor(worldX, worldY);

    if (got != expected)
    {
        fprintf(stderr, "%s: world (%d,%d): ring idx=%d entry=0x%04X got=0x%04X "
                        "expected=0x%04X\n",
                what, worldX, worldY, idx, entry, got, expected);
        assert(0);
    }
}

/* Renders one pixel through the real renderer and checks it against the
 * reference (resident -> cell color, else M0). Only valid for a NON-central
 * coordinate. */
static void AssertWorld(s32 wx, s32 wy, const char *what)
{
    u16 got;
    u8 win;
    u16 expected = TestResident(wx, wy) ? TestCellColor(wx, wy) : M0_COLOR;

    assert(DrawMapFrameWithMetaEx(&sBgSnap, &got, &win, NULL, 1, 1, wx, wy));
    if (got != expected || win != NATIVE_COMPOSITE_WIN_BG1)
    {
        fprintf(stderr, "%s: world (%d,%d): got=0x%04X win=%u expected=0x%04X\n",
                what, wx, wy, got, win, expected);
        assert(0);
    }
}

/* Reset the renderer's GLOBAL ring-coverage tracking between tests: render one
 * frame at a camera far from any test state so the next real load render (which
 * differs by a teleport, not a single crossing) re-seeds the window cleanly. */
static u16 sDummyFrame;
static u8 sDummyWinner;

static void ResetRendererCoverageTracking(void)
{
    sBgSnap.cameraMapX = 100;
    sBgSnap.cameraMapY = 100;
    sBgSnap.cameraX = 0;
    sBgSnap.cameraY = 0;
    sBgSnap.cameraXTileOffset = 5;
    sBgSnap.cameraYTileOffset = 5;
    sBgSnap.bgHofs[0] = 0;
    sBgSnap.bgHofs[1] = 0;
    sBgSnap.bgHofs[2] = 0;
    sBgSnap.bgVofs[0] = 0;
    sBgSnap.bgVofs[1] = 0;
    sBgSnap.bgVofs[2] = 0;
    DrawMapFrameWithMetaEx(&sBgSnap, &sDummyFrame, &sDummyWinner, NULL,
                           1, 1, 100 * 16, 100 * 16);
}

static void RunWalk(const char *tag, s32 sx, s32 sy, int frames)
{
    int f;

    ResetRendererCoverageTracking();
    FreshLoad();
    BuildSnapshot();
    SweepFrame(tag, 0);   /* load frame: seeds tracking at the load layout */

    for (f = 1; f <= frames; f++)
    {
        sLatchedXPix = sXPix;   /* the capture latches the PRE-movement scroll */
        sLatchedYPix = sYPix;
        AdvanceCamera(sx, sy);
        BuildSnapshot();
        SweepFrame(tag, f);
    }
    printf("%s: %d frames dir=(%+d,%+d) end pos=(%d,%d) tile=(%u,%u) "
           "cam=(%d,%d) ok\n",
           tag, frames, sx, sy, sPosX, sPosY, sXTile, sYTile, sCamX, sCamY);
}

/* H. §6: POKEEMERALD_NATIVE_EXPANDED_DEBUG=1 movement trace. The renderer's
 * coverage tracker emits ONE stderr line per coverage-window EVENT (load,
 * crossing, teleport reset) -- never per sub-crossing pan, so rate-limited by
 * construction. Drive a west walk with the env set, capture stderr, and assert
 * the trace shows the reset + the 4 west crossings with the correct window
 * shifts, no east events, and far fewer lines than frames (no per-pan spam).
 * The presentation frames rendered during the capture are the same margins A-G
 * validate, proving the dev-only diagnostics never alter presentation. */
static void TestMovementDebugTrace(void)
{
    char path[] = "/tmp/native-sm-trace.XXXXXX";
    int fd;
    int saved;
    char buf[8192];
    ssize_t n;
    const char *p;
    int moves;
    int west;
    int east;
    int f;

    fd = mkstemp(path);
    assert(fd >= 0);
    saved = dup(fileno(stderr));
    assert(saved >= 0);
    fflush(stderr);
    assert(dup2(fd, fileno(stderr)) >= 0);   /* returns the dest fd (2), not 0 */

    setenv("POKEEMERALD_NATIVE_EXPANDED_DEBUG", "1", 1);

    ResetRendererCoverageTracking();   /* teleport -> one RESET event */
    FreshLoad();
    BuildSnapshot();
    SweepFrame("H-trace", 0);          /* load render -> second RESET event */
    for (f = 1; f <= 64; f++)
    {
        sLatchedXPix = sXPix;
        sLatchedYPix = sYPix;
        AdvanceCamera(-1, 0);
        BuildSnapshot();
        SweepFrame("H-trace", f);
    }

    fflush(stderr);
    assert(dup2(saved, fileno(stderr)) >= 0);
    close(saved);
    lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, (size_t)sizeof(buf) - 1);
    buf[n] = '\0';
    close(fd);
    unlink(path);
    unsetenv("POKEEMERALD_NATIVE_EXPANDED_DEBUG");

    moves = 0;
    west = 0;
    east = 0;
    for (p = buf; (p = strstr(p, "ring coverage move")) != NULL; p += 1)
        moves++;
    for (p = buf; (p = strstr(p, "X:WEST")) != NULL; p += 1)
        west++;
    for (p = buf; (p = strstr(p, "X:EAST")) != NULL; p += 1)
        east++;

    if (moves < 1 || west < 4 || east != 0
     || strstr(buf, "window [640,+256)->[624,+256)") == NULL
     || strstr(buf, "window [592,+256)->[576,+256)") == NULL)
    {
        fprintf(stderr,
                "H: unexpected movement trace (moves=%d west=%d east=%d):\n%s\n",
                moves, west, east, buf);
        assert(0);
    }
    /* Load/reset + 4 crossings <= 6 lines for a 64-frame walk: a per-frame
     * (per-pan) trace would be 64 lines and this bound would fail. */
    if (moves > 8)
    {
        fprintf(stderr, "H: movement trace NOT rate-limited (%d lines in 64 frames):\n%s\n",
                moves, buf);
        assert(0);
    }
    printf("H: movement debug trace (env-driven, rate-limited, correct windows) ok\n");
}

/* I. §7: the provider debug image must be obtainable AFTER movement and reflect
 * the POST-movement tracked coverage window -- never the stale pre-walk window,
 * and never the presentation frame (the image is a separate caller buffer; the
 * walk's SweepFrames already proved the presentation frames are untouched).
 * Run a west walk, then classify the whole 300x200 viewport via
 * FillProviderDebugImage and assert every pixel is GREEN (ring) exactly where
 * the renderer resolves ring content: the central presentation window OR the
 * test's OWN phase-tracked residency window -- and non-green elsewhere. The
 * central window is always ring-presented (parity), so the discriminating
 * region is the LEFT margin (world ~[742,764]): at C's end the correct west
 * window [576,+256) makes those cells resident (green), while the BUGGY stale
 * anchor window [384,640) -- and any pre-walk window -- would classify them
 * GRID (blue), so this test fails with the bug. All region centers land in the
 * real-map rectangle, so a non-resident center is GRID (blue 0x001F); RING is
 * green 0x03E0. */
static int TestCentralContains(s32 wx, s32 wy)
{
    s32 ox = TestOriginX();
    s32 oy = TestOriginY();

    return wx >= ox && wx < ox + DISPLAY_WIDTH
        && wy >= oy && wy < oy + DISPLAY_HEIGHT;
}

static void AssertDebugImageFollowsMovement(const char *tag)
{
    static u16 debugImage[VP_W * VP_H];
    s32 originX = TestOriginX();
    s32 originY = TestOriginY();
    s32 left = originX - 30;
    s32 top = originY - 20;
    int vy;
    int vx;

    BuildSnapshot();
    assert(NativeOverworldRenderer_FillProviderDebugImage(
        &sBgSnap, left, top, VP_W, VP_H, 8, debugImage));

    for (vy = 0; vy < VP_H; vy++)
    {
        for (vx = 0; vx < VP_W; vx++)
        {
            s32 wx = left + (vx / 8) * 8 + 4;   /* the region center the image uses */
            s32 wy = top + (vy / 8) * 8 + 4;
            u16 got = debugImage[vy * VP_W + vx];
            int central = TestCentralContains(wx, wy);
            int resident = TestResident(wx, wy);
            u16 want = (central || resident) ? 0x03E0 : 0x001F;

            if (got != want)
            {
                fprintf(stderr,
                        "%s: debug image px(%d,%d) world(%d,%d): got=0x%04X "
                        "expected=%s 0x%04X central=%d resident=%d\n",
                        tag, vx, vy, wx, wy, got,
                        (central || resident) ? "RING" : "GRID", want,
                        central, resident);
                assert(0);
            }
        }
    }
    printf("%s: provider debug image follows moved coverage window ok\n", tag);
}

/* ---- The sequential tests (A-G, §5) + movement diagnostics (§6/§7) ---- */

int main(void)
{
    SceneReset();

    /* A. east 8 px: sub-crossing boundary, no crossing yet. The residency window
     * must stay put at the load layout while the sub-cell offset advances. */
RunWalk("A-east8", 1, 0, 8);

    /* B. east 16 px: exactly one crossing (posX 40->41, xTile 0->2). The just-
     * entered cell 55 must sit at the anchored ring col (2+28)%32 = 30. */
RunWalk("B-east16", 1, 0, 16);
AssertRingCell(55 * 16, 40 * 16, "B just-entered cell 55");

    /* C. west 64 px: four west crossings (posX 40->36, xTile 0->24). The u8
     * offset wraps; the window must track cells posX..posX+15 (never the shifted
     * anchor formula the bug produced). */
RunWalk("C-west64", -1, 0, 64);
AssertRingCell(36 * 16, 40 * 16, "C just-entered cell 36 (west)");
AssertDebugImageFollowsMovement("I-debug-west");   /* §7: image tracks the moved window */

    /* D. north 64 px: four north crossings. */
RunWalk("D-north64", 0, -1, 64);

    /* E. south 64 px: four south crossings. */
RunWalk("E-south64", 0, 1, 64);

    /* F. east 256 px: 16 crossings, xTile wraps 0->32->0 and the ring's physical
     * columns wrap 31->0. After the last crossing the ring holds cells 55..70;
     * cell 70 (posX+14) is written at col (0+28)%32 = 28 and cell 56 at col 0. */
    RunWalk("F-east256", 1, 0, 256);
AssertRingCell(70 * 16, 40 * 16, "F just-entered cell 70 (wrap)");
AssertRingCell(56 * 16, 40 * 16, "F cell 56 at ring col 0 after wrap");
AssertWorld(54 * 16 + 4, 40 * 16, "F just-evicted cell 54 -> M0");
AssertWorld(71 * 16 + 4, 40 * 16, "F beyond-coverage east cell 71 -> M0");

    /* G. east 64 then west 48: direction reversal. The window must flip phase
     * (east window -> west window) on the first west crossing and stay correct. */
RunWalk("G-east64", 1, 0, 64);
    {
        int f;
        s32 sx = -1;
        for (f = 65; f <= 112; f++)
        {
            sLatchedXPix = sXPix;
            sLatchedYPix = sYPix;
            AdvanceCamera(sx, 0);
            BuildSnapshot();
            SweepFrame("G-rev", f);
        }
        AssertRingCell(sPosX * 16, 40 * 16, "G just-entered west cell at col xTile");
        printf("G-west48: reversal end pos=(%d,%d) tile=(%u,%u) cam=(%d,%d) ok\n",
               sPosX, sPosY, sXTile, sYTile, sCamX, sCamY);
    }
    TestMovementDebugTrace();   /* H: §6 movement trace via the debug env var */
    printf("native overworld sequential movement unit test passed\n");
    return 0;
}

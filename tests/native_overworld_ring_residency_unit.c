/*
 * Stage 4A runtime expanded-world correctness hardening: stale-ring regression
 * tests (the A-J spec from the expanded-world directive).
 *
 * Background: the old ring gate treated the LOGICAL ORIGIN's neighborhood
 * ([origin-16, origin-16+272) x [origin-16, origin-16+192)) as ring-eligible
 * and then wrapped any coordinate through the &0xFF torus. Expanded-viewport
 * coordinates that the game never wrote into the ring (the ring holds exactly
 * the 256x256 world-pixel window anchored at the player's map cell -- see
 * MapPosToBgTilemapOffset, DrawWholeMapViewInternal, RedrawMapSlice*) aliased
 * onto an UNRELATED world cell's ring content: the left margin showed the ring
 * content 16 cells EAST, the right margin 16 cells WEST, etc. The screenshots
 * showed this as repeated/stale map content in the newly revealed world.
 *
 * The fix separates WORLD RESIDENCY (IsWorldCoordinateResidentInRing -- a plain
 * world-rectangle test, no wrap) from the PHYSICAL ring index
 * (WorldCoordinateToPhysicalRingIndex -- the &0xFF torus, only valid after
 * residency succeeds), and preserves the central 240x160 window's ring
 * precedence as a hard parity invariant.
 *
 * These tests build snapshots whose ring and grid carry OBVIOUSLY DIFFERENT
 * sentinel content, then assert the corrected provider choice per pixel:
 *
 *   A. central 240x160 resident ring wins
 *   B. 30px left margin outside residency uses grid (stale aliased ring tile is
 *      NOT shown)
 *   C. 30px right margin outside residency uses grid
 *   D. 20px top margin outside residency uses grid
 *   E. 20px bottom margin outside residency uses grid
 *   F. corner outside residency uses grid
 *   G. camera near the ring wrap boundary: a resident MARGIN coord maps to the
 *      WRAPPED physical index at the ring's WORLD ANCHOR (not the presentation
 *      scroll -- the scroll drifts from the anchor on moving frames, the
 *      moving-world coherency bug this Stage fixes)
 *   H. world coord ONE TILE beyond residency does NOT wrap back into the ring
 *   I. mutable/transient tile inside residency: the ring wins over the grid
 *   J. connection-strip world coord outside the ring: the grid (which carries
 *      the copied connection strip) wins over the border
 *
 * Expected colors are computed INDEPENDENTLY from the snapshot's ring/grid/
 * border data, BG char memory, and palette (never via the renderer's resolve/
 * sample functions), so the test cannot agree with the renderer on a wrong
 * provider choice.
 *
 * Self-contained: links only native_overworld_renderer.c + viewport (the
 * expanded composite is not exercised here -- BG tile provider only).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

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

#define TEST_GRID_WIDTH  40
#define TEST_GRID_HEIGHT 24

/* GRID metatile id 5 (tile base 100, pal 1), CONNECTION metatile id 6 (tile
 * base 150, pal 3), BORDER metatile id 4 (tile base 200, pal 2). Ring BG1
 * entries use pal 0 so every provider is distinguishable by palette. */
#define TEST_META_GRID        5
#define TEST_META_CONNECTION  6
#define TEST_META_BORDER      4
#define TEST_GRID_TILE_BASE  100
#define TEST_CONN_TILE_BASE  150
#define TEST_BORDER_TILE_BASE 200

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

static void FillBgCharMemory(void)
{
    int t;
    for (t = 1; t < 960; t++)
    {
        int ty;
        for (ty = 0; ty < 8; ty++)
        {
            int k;
            for (k = 0; k < 4; k++)
            {
                u8 lo = (u8)(1 + ((t * 7 + ty * 5 + 2 * k) % 15));
                u8 hi = (u8)(1 + ((t * 7 + ty * 5 + 2 * k + 1) % 15));
                VRAM_[t * 32 + ty * 4 + k] = (u8)(lo | (hi << 4));
            }
        }
    }
}

static void FillBgPalette(void)
{
    int b;
    for (b = 0; b < 16; b++)
    {
        int p;
        for (p = 0; p < 16; p++)
            ((u16 *)PLTT)[b * 16 + p] = (u16)(0x0100 + b * 16 + p);
    }
}

static void FillMetatile(u16 metatileId, u16 baseTile, u8 pal)
{
    int q;
    for (q = 0; q < 8; q++)
        sPrimaryMetatiles[metatileId * NUM_TILES_PER_METATILE + q]
            = BgTile((u16)(baseTile + q), pal);
}

static void SceneReset(void)
{
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
    memcpy(sBgSnap.bgVram, VRAM_, 0x8000);
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
    sBgSnap.bgHofs[0] = 0; sBgSnap.bgVofs[0] = 0;
    sBgSnap.bgHofs[1] = 0; sBgSnap.bgVofs[1] = 0;
    sBgSnap.bgHofs[2] = 0; sBgSnap.bgVofs[2] = 0;

    sBgSnap.requiredCapabilities = NATIVE_CAPABILITY_MAP_BACKGROUND
                                 | NATIVE_CAPABILITY_LIVE_BG_RING
                                 | NATIVE_CAPABILITY_BG_VRAM
                                 | NATIVE_CAPABILITY_BG_PALETTE;
    sBgSnap.fallbackReason = NATIVE_FALLBACK_NONE;
    sBgSnap.bgRingValid = TRUE;
    sBgSnap.bgVramValid = TRUE;

    sBgSnap.primaryMetatiles = sPrimaryMetatiles;
    sBgSnap.primaryMetatileAttributes = sPrimaryAttributes;
    sBgSnap.secondaryMetatiles = sSecondaryMetatiles;
    sBgSnap.secondaryMetatileAttributes = sSecondaryAttributes;
    sBgSnap.border = sBorderMetatiles;

    FillMetatile(TEST_META_GRID, TEST_GRID_TILE_BASE, 1);
    FillMetatile(TEST_META_CONNECTION, TEST_CONN_TILE_BASE, 3);
    FillMetatile(TEST_META_BORDER, TEST_BORDER_TILE_BASE, 2);
    sBorderMetatiles[0] = TEST_META_BORDER;
    sBorderMetatiles[1] = TEST_META_BORDER;
    sBorderMetatiles[2] = TEST_META_BORDER;
    sBorderMetatiles[3] = TEST_META_BORDER;

    sBgSnap.mapGridValid = TRUE;
    sBgSnap.gridWidth = TEST_GRID_WIDTH;
    sBgSnap.gridHeight = TEST_GRID_HEIGHT;
    /* Synthetic "real map" rectangle within the 40x24 grid: world cells
     * [MAP_OFFSET, MAP_OFFSET+20) x [MAP_OFFSET, MAP_OFFSET+12) are in-map
     * grid; the defined cells around them classify as the connection apron.
     * mapWidth/mapHeight are never read by the tile-provider resolution, only
     * by the dev-only provider classification. */
    sBgSnap.mapWidth = 20;
    sBgSnap.mapHeight = 12;
    for (ry = 0; ry < TEST_GRID_HEIGHT; ry++)
        for (rx = 0; rx < TEST_GRID_WIDTH; rx++)
            sBgSnap.mapGrid[rx + TEST_GRID_WIDTH * ry] = TEST_META_GRID;

    /* Ring defaults: BG1 = sentinel S, BG2/3 transparent. */
    for (ry = 0; ry < 32; ry++)
        for (rx = 0; rx < 32; rx++)
        {
            sBgSnap.bgRing[0][ry * 32 + rx] = BgTile(1, 0); /* S = tile 1 pal 0 */
            sBgSnap.bgRing[1][ry * 32 + rx] = 0;
            sBgSnap.bgRing[2][ry * 32 + rx] = 0;
        }

    sBgSnap.cameraMapX = 2;
    sBgSnap.cameraMapY = 2;
    sBgSnap.cameraX = 0;
    sBgSnap.cameraY = 0;
}

/* ---- Independent expected-color helpers (never call the renderer) ---- */

/* Deterministic 4bpp char model, identical to the one the BG char memory is
 * filled with (tests/native_obj_renderer_shared.h): tile t's texel (tx,ty) is
 * index 1 + ((t*7 + ty*5 + tx) % 15); palette bank b, index p -> color
 * 0x0100 + b*16 + p. Reimplemented here (not via the shared header) so this
 * TU does not pull in the OBJ renderer. */
static u8 ExpectedPixelIndex(int tile, int texX, int texY)
{
    return (u8)(1 + ((tile * 7 + texY * 5 + texX) % 15));
}

static u16 ExpectedColor(int paletteNum, int pixel)
{
    return (u16)(0x0100 + paletteNum * 16 + pixel);
}

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

/* 4bpp char-model color for a BG1 NORMAL-metatile entry at the logical world
 * sub-pixel: entry tile = base + quadrant, texel = (wx,wy) mod 8. */
static u16 ExpectedGridColor(s32 wx, s32 wy)
{
    s32 tx = RefPosMod(wx, 8);
    s32 ty = RefPosMod(wy, 8);
    s32 q = (RefPosMod(wy, 16) / 8) * 2 + RefPosMod(wx, 16) / 8;
    u8 pixel = (u8)(1 + (((TEST_GRID_TILE_BASE + 4 + q) * 7 + ty * 5 + tx) % 15));
    return (u16)(0x8000 | ExpectedColor(1, pixel));
}

static u16 ExpectedConnectionColor(s32 wx, s32 wy)
{
    s32 tx = RefPosMod(wx, 8);
    s32 ty = RefPosMod(wy, 8);
    s32 q = (RefPosMod(wy, 16) / 8) * 2 + RefPosMod(wx, 16) / 8;
    u8 pixel = (u8)(1 + (((TEST_CONN_TILE_BASE + 4 + q) * 7 + ty * 5 + tx) % 15));
    return (u16)(0x8000 | ExpectedColor(3, pixel));
}

static u16 ExpectedBorderColor(s32 wx, s32 wy)
{
    s32 tx = RefPosMod(wx, 8);
    s32 ty = RefPosMod(wy, 8);
    s32 q = (RefPosMod(wy, 16) / 8) * 2 + RefPosMod(wx, 16) / 8;
    u8 pixel = (u8)(1 + (((TEST_BORDER_TILE_BASE + 4 + q) * 7 + ty * 5 + tx) % 15));
    return (u16)(0x8000 | ExpectedColor(2, pixel));
}

/* Ring content for entry `entry` at the presentation-scroll sub-pixel. */
static u16 ExpectedRingColor(u16 entry, s32 sampleX, s32 sampleY)
{
    u16 tile = entry & 0x3FF;
    u8 pal = (u8)((entry >> 12) & 0xF);
    s32 tx = RefPosMod(sampleX, 8);
    s32 ty = RefPosMod(sampleY, 8);
    u8 pixel = (u8)(1 + ((tile * 7 + ty * 5 + tx) % 15));
    return (u16)(0x8000 | ExpectedColor(pal, pixel));
}

/* Sentinel ring tile (the value the OLD gate would wrongly show in a margin):
 * tile 1, pal 0. Every grid/border/connection color uses pal >= 1, so a
 * margin pixel equal to a pal-0 sentinel texel is the bug. */
#define SENTINEL_TILE 1u
#define SENTINEL_PAL 0u

/* ---- Sampler through the real renderer (1x1 viewport at one world px) ---- */

static u16 SampleWorld(s32 wx, s32 wy, u8 *winOut)
{
    static u16 frame;
    static u8 winner;

    assert(DrawMapFrameWithMetaEx(&sBgSnap, &frame, &winner, NULL,
                                  1, 1, wx, wy));
    if (winOut != NULL)
        *winOut = winner;
    return frame;
}

static void AssertColor(s32 wx, s32 wy, u16 expected, const char *what)
{
    u8 win;
    u16 got = SampleWorld(wx, wy, &win);

    if (got != expected || win != NATIVE_COMPOSITE_WIN_BG1)
    {
        fprintf(stderr,
                "%s: world (%d,%d): got=0x%04X win=%u expected=0x%04X\n",
                what, wx, wy, got, win, expected);
        assert(0);
    }
}

/* ---- A. Central 240x160 resident ring wins ---- */

static void TestCentralResidentRingWins(void)
{
    int wx;
    int wy;

    SceneReset();
    /* Ring = resident content R (tile 2, pal 0), NOT the sentinel, so a central
     * pixel equal to R proves the ring provider won (grid G is pal 1). */
    for (wy = 0; wy < 32; wy++)
        for (wx = 0; wx < 32; wx++)
            sBgSnap.bgRing[0][wy * 32 + wx] = BgTile(2, 0);

    /* Camera at (2,2) sub-cell 0: origin=(32,32), residency=[32,288)^2, and the
     * whole 240x160 central window [32,272)x[32,192) is resident. */
    AssertColor(50, 50,
                ExpectedRingColor(BgTile(2, 0),
                                  sBgSnap.bgHofs[0] + 50 - 32,
                                  sBgSnap.bgVofs[0] + 50 - 32),
                "A central resident ring");
    AssertColor(32, 32,
                ExpectedRingColor(BgTile(2, 0),
                                  sBgSnap.bgHofs[0] + 32 - 32,
                                  sBgSnap.bgVofs[0] + 32 - 32),
                "A central NW corner");
    AssertColor(271, 191,
                ExpectedRingColor(BgTile(2, 0),
                                  sBgSnap.bgHofs[0] + 271 - 32,
                                  sBgSnap.bgVofs[0] + 191 - 32),
                "A central SE corner");
    /* The ring color differs from the grid color at the same coordinate: */
    assert(SampleWorld(50, 50, NULL) != ExpectedGridColor(50, 50));
    printf("A: central 240x160 resident ring wins ok\n");
}

/* ---- B-E. Margins outside residency use grid, not the aliased ring ---- */

static void TestLeftMarginUsesGrid(void)
{
    /* Origin (32,32), residency [32,288). Left margin worldX in [2,32) of the
     * 300x200 viewport at left=2 is NOT resident. The OLD gate claimed
     * [16,32) as ring: worldX=24 -> ring col (0+(24-32))&0xFF=248>>3=31, where
     * the sentinel sits. The fix must show the grid (pal 1), not the sentinel
     * (pal 0). */
    SceneReset();
    AssertColor(24, 80, ExpectedGridColor(24, 80), "B left margin grid");
    printf("B: left margin (worldX<residency) uses grid, not stale ring ok\n");
}

static void TestRightMarginUsesGrid(void)
{
    /* cameraX=8 -> origin=(40,32). Right margin of the 300x200 viewport spans
     * worldX [280,310); residency ends at 288 and the OLD gate reached 296, so
     * worldX=292 was ring-aliased (ring col 31 = sentinel). The fix uses grid. */
    SceneReset();
    sBgSnap.cameraX = 8;
    AssertColor(292, 80, ExpectedGridColor(292, 80), "C right margin grid");
    printf("C: right margin (worldX>=residency) uses grid, not stale ring ok\n");
}

static void TestTopMarginUsesGrid(void)
{
    /* Origin (32,32). Top margin worldY in [12,32); the OLD gate claimed
     * [16,32) as ring (row 31 = sentinel). The fix uses grid. */
    SceneReset();
    AssertColor(80, 24, ExpectedGridColor(80, 24), "D top margin grid");
    printf("D: top margin (worldY<residency) uses grid, not stale ring ok\n");
}

static void TestBottomMarginUsesGrid(void)
{
    /* cameraY=88 -> origin=(32,120), residency [32,288). The bottom margin of
     * the 300x200 viewport (top=100) spans worldY [280,300); the OLD gate
     * reached 296 and ring-aliased worldY=290 (row 21 = sentinel). The fix uses
     * grid. The central window [120,280) stays resident. */
    SceneReset();
    sBgSnap.cameraY = 88;
    AssertColor(80, 290, ExpectedGridColor(80, 290), "E bottom margin grid");
    printf("E: bottom margin (worldY>=residency) uses grid, not stale ring ok\n");
}

/* ---- F. Corner outside residency uses grid ---- */

static void TestCornerOutsideResidencyUsesGrid(void)
{
    SceneReset();
    /* world (20,20): both axes below residency (32). Old gate (col/row 30)
     * claimed the ring; the fix uses grid. */
    AssertColor(20, 20, ExpectedGridColor(20, 20), "F corner grid");
    /* And a corner that is resident on both axes still uses the ring: */
    AssertColor(80, 80,
                ExpectedRingColor(BgTile(1, 0),
                                  sBgSnap.bgHofs[0] + 80 - 32,
                                  sBgSnap.bgVofs[0] + 80 - 32),
                "F resident center ring");
    printf("F: corner outside residency uses grid ok\n");
}

/* ---- G/H. Physical wrap vs residency ---- */

static void TestWrapBoundaryResident(void)
{
    /* A resident coordinate near the 256px ring edge maps through the ANCHORED
     * physical index -- the ring's world anchor (cameraMapX*16 - xTileOffset*8,
     * here 0) -- NOT the presentation scroll. bgHofs=13 shifts the OLD
     * presentation formula by 13 columns; the anchored formula is immune to the
     * scroll's drift, which is exactly the moving-world coherency fix. The ring
     * is a column-varying pattern (tile 2+rx, pal 0) so the exact sampled tile
     * is verifiable. */
    int ry;
    int rx;

    SceneReset();
    sBgSnap.cameraMapX = 0;
    sBgSnap.cameraMapY = 0;
    sBgSnap.bgHofs[0] = 13;
    sBgSnap.bgVofs[0] = 0;
    for (ry = 0; ry < 32; ry++)
        for (rx = 0; rx < 32; rx++)
            sBgSnap.bgRing[0][ry * 32 + rx] = BgTile((u16)(2 + rx), 0);

    /* worldX=240 is the first non-central column (origin 0 -> central window
     * x=[0,240)) and is resident ([0,256)). Anchored ring col = (240-0)>>3 = 30
     * -> tile 2+30 = 32; sub-pixel sampleX = worldX - anchorX = 240. */
    AssertColor(240, 0,
                ExpectedRingColor(BgTile(32, 0), 240, 0),
                "G anchored resident worldX=240");
    /* worldX=250 -> anchored ring col 31 -> tile 33 at sub-pixel 250. The OLD
     * presentation formula gave (13+250)&0xFF=7 -> ring col 0 -> tile 2 at
     * sampleX 263; the anchored tile differs, proving the scroll's drift no
     * longer mis-maps resident margin pixels. */
    AssertColor(250, 0,
                ExpectedRingColor(BgTile(33, 0), 250, 0),
                "G anchored resident worldX=250");
    assert(SampleWorld(250, 0, NULL) != ExpectedRingColor(BgTile(2, 0), 263, 0));
    printf("G: resident coords near the wrap map to the ANCHORED physical index ok\n");
}

static void TestOneTileBeyondResidency(void)
{
    /* Same scene as G. worldX=256 is ONE TILE beyond the residency window:
     * (256-0)&0xFF = 0, so a toroidal residency test would alias it onto ring
     * col 0 (tile 2, pal 0) as if it were worldX=0. It must NOT wrap back into
     * the ring: grid (pal 1) wins. */
    TestWrapBoundaryResident(); /* rebuilds the same scene */
    AssertColor(256, 0, ExpectedGridColor(256, 0), "H one tile beyond grid");
    /* Just inside: worldX=255 stays resident and maps to the anchored ring col
     * (255-0)>>3 = 31 -> tile 33 (pal 0), sub-pixel 255. */
    AssertColor(255, 0,
                ExpectedRingColor(BgTile(33, 0), 255, 0),
                "H last resident worldX=255");
    printf("H: one tile beyond residency does NOT wrap back into ring ok\n");
}

/* ---- I. Mutable/transient tile inside residency wins over grid ---- */

static void TestTransientRingWriteWins(void)
{
    SceneReset();
    /* Ring is the sentinel everywhere; place the transient R at ring tile 0. */
    sBgSnap.bgRing[0][0] = BgTile(3, 0);
    /* world (36,36): ring col (0+(36-32))>>3 = 0, row 0 -> samples ring[0] = R.
     * It is resident and the grid would give pal 1; the transient ring write
     * (pal 0) must win. */
    AssertColor(36, 36,
                ExpectedRingColor(BgTile(3, 0),
                                  sBgSnap.bgHofs[0] + 36 - 32,
                                  sBgSnap.bgVofs[0] + 36 - 32),
                "I transient ring write wins");
    /* A neighboring resident ring tile (no transient) is still ring content
     * (the sentinel), never grid. */
    AssertColor(44, 36,
                ExpectedRingColor(BgTile(1, 0),
                                  sBgSnap.bgHofs[0] + 44 - 32,
                                  sBgSnap.bgVofs[0] + 36 - 32),
                "I resident ring precedence");
    printf("I: transient ring write inside residency wins over grid ok\n");
}

/* ---- J. Connection-strip grid content wins outside the ring ---- */

static void TestConnectionStripGridWins(void)
{
    SceneReset();
    /* Put the connection metatile (distinct pal 3) into grid cell (0,8). world
     * (7,136) -> map(0,8): NOT ring-resident (worldX=7 < 32), in-grid, so the
     * grid's copied connection content must win over the border. */
    sBgSnap.mapGrid[0 + TEST_GRID_WIDTH * 8] = TEST_META_CONNECTION;
    AssertColor(7, 136, ExpectedConnectionColor(7, 136),
                "J connection-strip grid wins");
    /* Just past the grid edge: mapX=40 -> border metatile (pal 2). */
    AssertColor(40 * 16, 136, ExpectedBorderColor(40 * 16, 136),
                "J border beyond grid");
    printf("J: connection-strip grid content wins outside ring ok\n");
}

/* ---- Provider visualization (Stage 4A §6, dev-only diagnostics) ---- */

/* Scene: camera (2,2) phase 0 -> origin (32,32), residency [32,288)^2, grid
 * 40x24 all defined (metatile 5), real map rect = world cells
 * [7,27) x [7,19). The 300x200 viewport sits at world top-left (2,12)
 * (the odd-rounded placement of a viewport centered on origin). */
#define TEST_VP_LEFT  2
#define TEST_VP_TOP   12
#define TEST_VP_W     300
#define TEST_VP_H     200

static s32 RegionColForWorldX(s32 worldX) { return (worldX - TEST_VP_LEFT) / 16; }
static s32 RegionRowForWorldY(s32 worldY) { return (worldY - TEST_VP_TOP) / 16; }

static void TestProviderVisualization(void)
{
    static u8 regionMap[19 * 13];
    static u16 debugImage[TEST_VP_W * TEST_VP_H];
    s32 cols;
    s32 rows;
    u8 corners[4];
    u8 margins[4];

    SceneReset();

    assert(NativeOverworldRenderer_ExpandedProviderRegionMap(
        &sBgSnap, TEST_VP_LEFT, TEST_VP_TOP, TEST_VP_W, TEST_VP_H, 16,
        regionMap, &cols, &rows));
    assert(cols == 19 && rows == 13);

    /* Central region (5,3) center (90,68): resident -> R. */
    assert(regionMap[3 * cols + 5] == NATIVE_OVERWORLD_PROVIDER_RING);
    /* Left-margin region (0,8) center (10,148): not resident, grid cell (0,9),
     * west connection apron -> C. */
    assert(regionMap[8 * cols + 0] == NATIVE_OVERWORLD_PROVIDER_CONNECTION);
    /* East-of-residency region (18,8) center (298,148): grid cell (18,9) in
     * the real map rect -> G. */
    assert(regionMap[8 * cols + 18] == NATIVE_OVERWORLD_PROVIDER_GRID);
    /* Region (18,3) center (298,68): grid cell (18,4), north apron -> C. */
    assert(regionMap[3 * cols + 18] == NATIVE_OVERWORLD_PROVIDER_CONNECTION);

    /* A viewport placed 600px east: col 18 center (896,68) -> cell 56 >= grid
     * width 40 -> out of grid -> B. */
    assert(NativeOverworldRenderer_ExpandedProviderRegionMap(
        &sBgSnap, 600, TEST_VP_TOP, TEST_VP_W, TEST_VP_H, 16,
        regionMap, &cols, &rows));
    assert(regionMap[3 * cols + 18] == NATIVE_OVERWORLD_PROVIDER_BORDER);

    /* Compact debug image: one solid block per region. Region (5,3) covers
     * viewport-local x [80,96) y [48,64) (green R); region (0,8) local
     * x [0,16) y [128,144) (yellow C); region (18,8) local x [288,300)
     * y [128,144) (blue G). */
    assert(NativeOverworldRenderer_FillProviderDebugImage(
        &sBgSnap, TEST_VP_LEFT, TEST_VP_TOP, TEST_VP_W, TEST_VP_H, 16,
        debugImage));
    assert(debugImage[48 * TEST_VP_W + 80] == 0x03E0);
    assert(debugImage[128 * TEST_VP_W + 0] == 0x7FE0);
    assert(debugImage[128 * TEST_VP_W + 288] == 0x001F);

    /* Corner/margin provider metadata (the values the debug log prints):
     * corners (2,12)/(301,12)/(2,211)/(301,211) -> C C C G;
     * margin midpoints L(17,112)/R(286,112)/T(152,22)/B(152,201) -> C R C R. */
    assert(NativeOverworldRenderer_ExpandedProviderCorners(
        &sBgSnap, TEST_VP_LEFT, TEST_VP_TOP, TEST_VP_W, TEST_VP_H,
        corners, margins));
    assert(corners[0] == NATIVE_OVERWORLD_PROVIDER_CONNECTION);
    assert(corners[1] == NATIVE_OVERWORLD_PROVIDER_CONNECTION);
    assert(corners[2] == NATIVE_OVERWORLD_PROVIDER_CONNECTION);
    assert(corners[3] == NATIVE_OVERWORLD_PROVIDER_GRID);
    assert(margins[0] == NATIVE_OVERWORLD_PROVIDER_CONNECTION);
    assert(margins[1] == NATIVE_OVERWORLD_PROVIDER_RING);
    assert(margins[2] == NATIVE_OVERWORLD_PROVIDER_CONNECTION);
    assert(margins[3] == NATIVE_OVERWORLD_PROVIDER_RING);

    /* The ASCII dump must not crash (dev-only; prints to stderr). */
    NativeOverworldRenderer_DumpProviderRegionMap(regionMap, cols, rows);

    printf("K: dev-only provider visualization (region map + debug image + corner/margin metadata) ok\n");
}

int main(void)
{
    TestCentralResidentRingWins();         /* A */
    TestLeftMarginUsesGrid();              /* B */
    TestRightMarginUsesGrid();             /* C */
    TestTopMarginUsesGrid();               /* D */
    TestBottomMarginUsesGrid();            /* E */
    TestCornerOutsideResidencyUsesGrid();  /* F */
    TestWrapBoundaryResident();            /* G */
    TestOneTileBeyondResidency();          /* H */
    TestTransientRingWriteWins();          /* I */
    TestConnectionStripGridWins();         /* J */
    TestProviderVisualization();           /* K: §6 dev-only diagnostics */
    printf("native overworld ring residency unit test passed\n");
    return 0;
}

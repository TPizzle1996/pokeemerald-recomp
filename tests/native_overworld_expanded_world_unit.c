/*
 * Stage 4A expanded-viewport WORLD-coordinate correctness suite (tests A-K).
 *
 * Unlike the parity test (center crop == proven 240x160 composite), this suite
 * validates the EXPANDED world-to-screen coordinate model against an INDEPENDENT
 * reference: RefWorldPixel() recomputes every pixel directly from the snapshot's
 * ring / backup grid / border metatiles, BG char memory, palette, captured BGCNT
 * priorities, and the presentation scroll -- using its own floor-divide, modulo,
 * ring indexing, and GBA metatile-layer mapping. It NEVER calls a renderer
 * resolve/sample function (no ResolveBgTileEntries, GetSnapshotMetatileId,
 * SampleSnapshotTile, CompositeMapPixelWithPriority), so the renderer and the
 * reference can only agree by both implementing the GBA text-mode rule.
 *
 *   A. Static-origin full-frame sweep (300x200): every pixel == independent
 *      reference, exercising negative world coords, 8x8 sub-tiles, 16x16
 *      metatile boundaries, the ring-gate seam, grid-vs-border precedence, the
 *      repeating 2x2 border, and the ring torus in the right/bottom margins.
 *   B. Grid/border-only sweep (ring disabled): the full grid resolves directly,
 *      the 16x16 grid-cell boundaries and 4 quadrants, and the border repeat
 *      beyond the grid edge.
 *   C. Non-preset viewport dimensions: 245x163 and 271x181 sweeps match the
 *      same independent reference (continuous viewport, no preset enums).
 *   D. Moving frame (presentation scroll != logical camera): ring sampled at the
 *      latched scroll (Stage 2.1 timing preserved), grid/border at the logical
 *      coordinate; the ring column wrap through the &0xFF torus is exercised.
 *   I. Map-connection strip: a grid cell near the edge (the grid carries copied
 *      connection strips) resolves the GRID metatile, not the border.
 *   J. Priority winner across providers: captured BGCNT priorities decide the
 *      winner in the ring and in the grid; a transparent BG1 exposes BG2.
 *   K. Sprite expansion positions: WORLD commands move outward by the margin
 *      (signedX + margin), RAW_OAM stays in the center crop, and a
 *      VANILLA_VIEWPORT_CULLED expandedEligible command is REVEALED in the apron
 *      at signedX + margin -- all through the real expanded composite.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"
#include "native_obj_renderer_shared.h"
#include "../src/platform/native_field_compositor.c"

#undef NDEBUG
#include <assert.h>

/* ---- Stubs of the hardware globals the renderer TUs reference ---- */
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

/* ---- Stubs of game globals ---- */
struct MapHeader gMapHeader;
static struct SaveBlock1 sSaveBlock1;
struct SaveBlock1 *gSaveBlock1Ptr = &sSaveBlock1;
struct BackupMapLayout gBackupMapLayout;
u16 *gOverworldTilemapBuffer_Bg1;
u16 *gOverworldTilemapBuffer_Bg2;
u16 *gOverworldTilemapBuffer_Bg3;
struct ObjectEvent gObjectEvents[OBJECT_EVENTS_COUNT];
struct Sprite gSprites[MAX_SPRITES + 1];
s16 gSpriteCoordOffsetX;
s16 gSpriteCoordOffsetY;

static s16 sCameraX;
static s16 sCameraY;

bool32 Overworld_IsNativeExpandedRendererReady(void)
{
    return TRUE;
}

bool32 Overworld_IsNativeParityCaptureEligible(struct NativeOverworldSnapshot *snapshot)
{
    (void)snapshot;
    return TRUE;
}

void GetCameraOffsetWithPan(s16 *x, s16 *y)
{
    *x = sCameraX;
    *y = sCameraY;
}

u32 MapGridGetMetatileIdAt(int x, int y)
{
    (void)x;
    (void)y;
    return 0;
}

u8 MapGridGetMetatileLayerTypeAt(int x, int y)
{
    (void)x;
    (void)y;
    return 0;
}

void UpdateShadowFieldEffect(struct Sprite *sprite)
{
    (void)sprite;
}

void UpdateSurfBlobFieldEffect(struct Sprite *sprite)
{
    (void)sprite;
}

/* ---- Scene: hand-built BG snapshot + buffers ---- */

#define SCREENBASE_BG0 15

#define EXPANDED_WIDTH_MAX  NATIVE_VIEWPORT_MAX_WIDTH
#define EXPANDED_HEIGHT_MAX NATIVE_VIEWPORT_MAX_HEIGHT

static struct NativeOverworldSnapshot sBgSnap;
static u16 sExpandedBg[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
static u8 sExpandedBgWinner[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
static u16 sExpandedBgLayers[4 * EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
static u16 sExpandedFinal[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
static u8 sExpandedWinner[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];

static u16 sPrimaryMetatiles[NUM_METATILES_IN_PRIMARY * NUM_TILES_PER_METATILE];
static u16 sPrimaryAttributes[NUM_METATILES_IN_PRIMARY];
static u16 sSecondaryMetatiles[NUM_METATILES_TOTAL * NUM_TILES_PER_METATILE];
static u16 sSecondaryAttributes[NUM_METATILES_TOTAL];
static u16 sBorderMetatiles[4];

static u16 BgTile(u16 tile, u8 pal)
{
    return (u16)(tile | ((u16)pal << 12));
}

static void SetRingRect(u8 bg, s32 x0, s32 y0, s32 x1, s32 y1, u16 entry)
{
    s32 ry;
    for (ry = y0; ry <= y1; ry++)
    {
        s32 rx;
        for (rx = x0; rx <= x1; rx++)
            sBgSnap.bgRing[bg][ry * 32 + rx] = entry;
    }
}

/* Deterministic BG char memory (same model as the oracle harness): tile t's
 * pixel at texel (tx,ty) is 1 + ((t*7 + ty*5 + tx) % 15) (never transparent);
 * tiles 0 and 960..1023 stay zero (transparent). */
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
            ((u16 *)PLTT)[b * 16 + p] = ExpectedColor(b, p);
    }
}

/* Fill the 8 tiles of one metatile with a distinct opaque tile run. */
static void FillMetatile(u16 metatileId, u16 baseTile, u8 pal)
{
    int q;
    for (q = 0; q < 8; q++)
        sPrimaryMetatiles[metatileId * NUM_TILES_PER_METATILE + q]
            = BgTile((u16)(baseTile + q), pal);
}

/* Base scene (mirrors the expanded-parity scaffold): origin (0,0), scroll 0,
 * captured BGCNT priorities, border metatiles 1-4, BG0 screenbase 15. */
static void ExpandedSceneReset(u8 bg1Prio, u8 bg2Prio, u8 bg3Prio)
{
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

    memcpy(sBgSnap.bgVram, VRAM_, 0x8000); /* tiles 0..1023 char data */
    memcpy(sBgSnap.palette, PLTT, PLTT_SIZE);

    sBgSnap.bg0Cnt = BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0)
                   | BGCNT_SCREENBASE(SCREENBASE_BG0) | BGCNT_16COLOR
                   | BGCNT_TXT256x256;
    sBgSnap.bgCnt[0] = BGCNT_PRIORITY(bg1Prio) | BGCNT_CHARBASE(0)
                     | BGCNT_16COLOR | BGCNT_TXT256x256;
    sBgSnap.bgCnt[1] = BGCNT_PRIORITY(bg2Prio) | BGCNT_CHARBASE(0)
                     | BGCNT_16COLOR | BGCNT_TXT256x256;
    sBgSnap.bgCnt[2] = BGCNT_PRIORITY(bg3Prio) | BGCNT_CHARBASE(0)
                     | BGCNT_16COLOR | BGCNT_TXT256x256;

    sBgSnap.bgHofs[0] = 0; sBgSnap.bgVofs[0] = 0;
    sBgSnap.bgHofs[1] = 0; sBgSnap.bgVofs[1] = 0;
    sBgSnap.bgHofs[2] = 0; sBgSnap.bgVofs[2] = 0;

    sBgSnap.dispCnt = DISPCNT_MODE_0 | DISPCNT_BG0_ON | DISPCNT_BG1_ON
                    | DISPCNT_BG2_ON | DISPCNT_BG3_ON | DISPCNT_OBJ_1D_MAP
                    | DISPCNT_OBJ_ON;
    sBgSnap.bldCnt = 0; sBgSnap.bldAlpha = 0; sBgSnap.bldY = 0;

    sSaveBlock1.pos.x = 0; sSaveBlock1.pos.y = 0;
    sCameraX = 0; sCameraY = 0;
    sBgSnap.cameraMapX = 0; sBgSnap.cameraMapY = 0;
    sBgSnap.cameraX = 0; sBgSnap.cameraY = 0;

    sBgSnap.requiredCapabilities = NATIVE_CAPABILITY_MAP_BACKGROUND
                                 | NATIVE_CAPABILITY_LIVE_BG_RING
                                 | NATIVE_CAPABILITY_BG_VRAM
                                 | NATIVE_CAPABILITY_BG_PALETTE;
    sBgSnap.fallbackReason = NATIVE_FALLBACK_NONE;
    sBgSnap.bgRingValid = TRUE;
    sBgSnap.bgVramValid = TRUE;

    /* Snapshot border/metatile fallback (used outside the ring gate / grid). */
    sBgSnap.primaryMetatiles = sPrimaryMetatiles;
    sBgSnap.primaryMetatileAttributes = sPrimaryAttributes;
    sBgSnap.secondaryMetatiles = sSecondaryMetatiles;
    sBgSnap.secondaryMetatileAttributes = sSecondaryAttributes;
    sBgSnap.border = sBorderMetatiles;
    sBorderMetatiles[0] = 1;
    sBorderMetatiles[1] = 2;
    sBorderMetatiles[2] = 3;
    sBorderMetatiles[3] = 4;
}

#define GRID_WIDTH  12
#define GRID_HEIGHT 12

/* Scene + a valid backup map grid (metatile ids 5..10, NORMAL attributes) with
 * distinct border and grid metatile tiles. */
static void ExpandedSceneResetGrid(u8 bg1Prio, u8 bg2Prio, u8 bg3Prio)
{
    int mx;
    int my;

    ExpandedSceneReset(bg1Prio, bg2Prio, bg3Prio);

    FillMetatile(1, 10, 0);
    FillMetatile(2, 20, 0);
    FillMetatile(3, 30, 0);
    FillMetatile(4, 40, 0);
    for (mx = 5; mx <= 10; mx++)
        FillMetatile((u16)mx, (u16)(100 + mx * 8), 1);

    sBgSnap.mapGridValid = TRUE;
    sBgSnap.gridWidth = GRID_WIDTH;
    sBgSnap.gridHeight = GRID_HEIGHT;
    for (my = 0; my < GRID_HEIGHT; my++)
        for (mx = 0; mx < GRID_WIDTH; mx++)
            sBgSnap.mapGrid[mx + GRID_WIDTH * my]
                = (u16)(5 + ((mx * 3 + my * 5) % 6));
}

/* Ring scene for the sweeps: BG1 opaque vertical stripes with a transparent
 * band at ring columns [24,28) (so BG2 wins there), BG2 opaque horizontal
 * stripes, BG3 transparent. */
static void BuildSweepRing(void)
{
    int ry;
    int rx;
    for (ry = 0; ry < 32; ry++)
    {
        for (rx = 0; rx < 32; rx++)
        {
            u16 t1 = (rx >= 24 && rx < 28) ? 0 : (u16)(1 + rx % 4);
            sBgSnap.bgRing[0][ry * 32 + rx] = t1 ? BgTile(t1, 1) : 0;
            sBgSnap.bgRing[1][ry * 32 + rx] = BgTile((u16)(1 + ry % 3), 2);
            sBgSnap.bgRing[2][ry * 32 + rx] = 0;
        }
    }
}

/* ---- Independent world-coordinate reference (NOT the renderer's resolves) ---- */

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

/* 4bpp char-memory sample (GBA text-mode sampler, independent re-derivation). */
static bool32 RefSampleTile(const struct NativeOverworldSnapshot *snap, u16 entry,
                            s32 sampleX, s32 sampleY, u16 *color)
{
    const u8 *tiles = (const u8 *)snap->bgVram;
    const u16 *palette = snap->palette;
    u16 tileNum = entry & 0x3FF;
    u8 paletteNum = (entry >> 12) & 0xF;
    s32 tileX = RefPosMod(sampleX, 8);
    s32 tileY = RefPosMod(sampleY, 8);
    u8 packedPixel;
    u8 pixel;

    if (entry & (1 << 10))
        tileX = 7 - tileX;
    if (entry & (1 << 11))
        tileY = 7 - tileY;
    packedPixel = tiles[tileNum * 32 + tileY * 4 + tileX / 2];
    pixel = (tileX & 1) ? (packedPixel >> 4) : (packedPixel & 0xF);
    if (pixel == 0)
        return FALSE;
    *color = palette[paletteNum * 16 + pixel];
    return TRUE;
}

/* Metatile-id resolution (grid precedence, then the repeating 2x2 border). */
static u16 RefMetatileId(const struct NativeOverworldSnapshot *snap,
                         s32 mapX, s32 mapY)
{
    if (snap->mapGridValid
     && mapX >= 0 && mapY >= 0
     && mapX < snap->gridWidth && mapY < snap->gridHeight)
    {
        u16 block = snap->mapGrid[mapX + snap->gridWidth * mapY];
        if (block != MAPGRID_UNDEFINED)
            return block & MAPGRID_METATILE_ID_MASK;
    }
    {
        u8 borderIndex = (u8)(((mapX + 1) & 1) + ((mapY + 1) & 1) * 2);
        return snap->border[borderIndex] & MAPGRID_METATILE_ID_MASK;
    }
}

static u16 RefMetatileAttributes(const struct NativeOverworldSnapshot *snap,
                                 u16 metatileId)
{
    if (metatileId < NUM_METATILES_IN_PRIMARY
     && snap->primaryMetatileAttributes != NULL)
        return snap->primaryMetatileAttributes[metatileId];
    if (metatileId < NUM_METATILES_TOTAL
     && snap->secondaryMetatileAttributes != NULL)
        return snap->secondaryMetatileAttributes[metatileId - NUM_METATILES_IN_PRIMARY];
    return 0;
}

/* GBA metatile->layer mapping (split/covered/normal), independent of the
 * renderer's MetatileTileEntryForLayer. */
static u16 RefEntryForLayer(u8 bg, const u16 *tiles, u8 layerType, u8 quadrant)
{
    switch (layerType)
    {
    case METATILE_LAYER_TYPE_SPLIT:
        if (bg == 3)
            return tiles[quadrant];
        if (bg == 1)
            return tiles[quadrant + 4];
        return 0;
    case METATILE_LAYER_TYPE_COVERED:
        if (bg == 3)
            return tiles[quadrant];
        if (bg == 2)
            return tiles[quadrant + 4];
        return 0;
    case METATILE_LAYER_TYPE_NORMAL:
    default:
        if (bg == 3)
            return 0x3014;
        if (bg == 2)
            return tiles[quadrant];
        if (bg == 1)
            return tiles[quadrant + 4];
        return 0;
    }
}

/* Full independent composite of ONE world pixel: provider (ring vs
 * grid/border), per-layer entries, char sub-pixel, and the captured-priority
 * winner merge. Returns the renderer's exact color encoding (backdrop palette[0]
 * without the alpha flag, BG pixels with bit 15). */
static void RefWorldPixel(const struct NativeOverworldSnapshot *snap,
                          s32 worldX, s32 worldY, u16 *colorOut, u8 *winnerOut)
{
    s32 originX = snap->cameraMapX * 16 + snap->cameraX;
    s32 originY = snap->cameraMapY * 16 + snap->cameraY;
    /* Ring WORLD ANCHOR (Stage 4A): the s32 world-pixel origin of the 256x256
     * rectangle the field renderer actually filled -- cameraMap*16 minus the
     * camera's 8px tile offsets. At tile offset 0 this is the plain camera-cell
     * window [pos*16, +256); after each 8px tile crossing it retracts to the
     * first tile the renderer drew (cells pos-1..pos+14 once k>=1). Residency
     * and the margin ring sample use this anchor; the central window keeps the
     * presentation scroll. */
    s32 anchorX = snap->cameraMapX * 16 - snap->cameraXTileOffset * 8;
    s32 anchorY = snap->cameraMapY * 16 - snap->cameraYTileOffset * 8;
    /* Ring eligibility = the central 240x160 GBA window OR world residency:
     *   - the central window is the authoritative presented frame and is always
     *     ring-served (parity hard invariant; a pan can pull its edge outside
     *     the residency rectangle),
     *   - every other coordinate (expanded margins) is ring-served ONLY when
     *     genuinely resident in the ring's world window [anchor, anchor+256)^2
     *     (the game's ring-writer never touches cells outside it -- see
     *     MapPosToBgTilemapOffset). This is WORLD coverage, NOT the logical
     *     origin's neighborhood and NOT the &0xFF torus: a non-resident
     *     coordinate aliases an unrelated world cell's ring content if indexed
     *     through the physical index, which is the stale/repeated-margin bug,
     *     so it must fall through to grid/border. The reference models this
     *     independently of the renderer's physical-index wrap. */
    bool32 inCentral = worldX >= originX && worldX < originX + DISPLAY_WIDTH
                    && worldY >= originY && worldY < originY + DISPLAY_HEIGHT;
    bool32 resident = worldX >= anchorX && worldX < anchorX + NATIVE_BG_RING_PIXELS
                   && worldY >= anchorY && worldY < anchorY + NATIVE_BG_RING_PIXELS;
    bool32 ring = snap->bgRingValid && (inCentral || resident);
    s32 ringSx;
    s32 ringSy;
    u16 color = snap->palette[0];
    u8 bestPrio = 4;
    u8 bestWinner = 0;
    u8 bg;

    for (bg = 0; bg < 3; bg++)
    {
        u16 entry;
        s32 sampleX;
        s32 sampleY;
        u16 layerColor;
        u8 prio = (u8)(snap->bgCnt[bg] & 3);

        if (ring)
        {
            if (inCentral)
            {
                s32 sx;
                s32 sy;
                /* Central 240x160: PRESENTATION scroll, GBA-exact (parity).
                 * Both the tile entry and the texel are sampled at the scroll,
                 * exactly as the GBA does. */
                ringSx = worldX - originX;
                ringSy = worldY - originY;
                sx = snap->bgHofs[bg] + ringSx;
                sy = snap->bgVofs[bg] + ringSy;
                entry = snap->bgRing[bg][((sy & 0xFF) >> 3) * 32 + ((sx & 0xFF) >> 3)];
                sampleX = sx;
                sampleY = sy;
            }
            else
            {
                /* Resident margin: the ring's WORLD ANCHOR. The tile ENTRY is
                 * the anchored index (worldX - anchorX, worldY - anchorY),
                 * independent of the scroll -- the field renderer filled the
                 * ring at the anchor, and the scroll has no meaning for margin
                 * content the GBA never displays. The TEXEL follows the
                 * renderer's composite, which adds bgHofs[bg] to ringSx =
                 * worldX - anchorX - bgHofs[0]; under the capture gate's
                 * equal-scroll invariant that is worldX - anchorX exactly, and
                 * the two agree. */
                ringSx = worldX - anchorX - snap->bgHofs[0];
                ringSy = worldY - anchorY - snap->bgVofs[0];
                entry = snap->bgRing[bg][(((worldY - anchorY) & 0xFF) >> 3) * 32
                                       + (((worldX - anchorX) & 0xFF) >> 3)];
                sampleX = snap->bgHofs[bg] + ringSx;
                sampleY = snap->bgVofs[bg] + ringSy;
            }
        }
        else
        {
            s32 mapX = RefFloorDiv(worldX, 16);
            s32 mapY = RefFloorDiv(worldY, 16);
            s32 localX = RefPosMod(worldX, 16);
            s32 localY = RefPosMod(worldY, 16);
            u8 quadrant = (u8)((localY / 8) * 2 + localX / 8);
            u16 metatileId = RefMetatileId(snap, mapX, mapY);
            u8 layerType = (u8)UNPACK_LAYER_TYPE(RefMetatileAttributes(snap, metatileId));
            const u16 *tiles;

            if (metatileId < NUM_METATILES_IN_PRIMARY)
                tiles = snap->primaryMetatiles + metatileId * NUM_TILES_PER_METATILE;
            else
                tiles = snap->secondaryMetatiles
                      + (metatileId - NUM_METATILES_IN_PRIMARY) * NUM_TILES_PER_METATILE;
            entry = RefEntryForLayer((u8)(bg + 1), tiles, layerType, quadrant);
            sampleX = worldX;
            sampleY = worldY;
        }

        if (!RefSampleTile(snap, entry, sampleX, sampleY, &layerColor))
            continue;
        if (prio < bestPrio || (prio == bestPrio && (u8)(bg + 2) < bestWinner))
        {
            bestPrio = prio;
            bestWinner = (u8)(bg + 2);
            color = layerColor | 0x8000;
        }
    }
    *winnerOut = bestWinner;
    *colorOut = color;
}

/* Deterministic tile texel index for the BG char model. */
static u8 BgPixelIndex(int tile, int tx, int ty)
{
    return (u8)(1 + ((tile * 7 + ty * 5 + tx) % 15));
}

/* ---- Sweep runner ---- */

/* Render `width x height` at the given viewport top-left and require EVERY
 * pixel (color AND winner) to equal the independent reference. */
static void AssertFrameMatchesReference(s32 left, s32 top, u16 width, u16 height)
{
    static u16 frame[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
    static u8 winner[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
    u16 sy;
    u16 sx;

    assert(DrawMapFrameWithMetaEx(&sBgSnap, frame, winner, NULL,
                                  width, height, left, top));
    for (sy = 0; sy < height; sy++)
    {
        for (sx = 0; sx < width; sx++)
        {
            u16 color;
            u8 win;
            u32 idx = (u32)sy * width + sx;

            RefWorldPixel(&sBgSnap, left + sx, top + sy, &color, &win);
            if (frame[idx] != color || winner[idx] != win)
            {
                fprintf(stderr,
                        "MISMATCH screen (%u,%u) world (%d,%d): "
                        "frame=0x%04X ref=0x%04X win=%u refWin=%u\n",
                        sx, sy, left + sx, top + sy, frame[idx], color,
                        winner[idx], win);
                assert(0);
            }
        }
    }
}

/* ---- Tests A-D: full-frame independent-reference sweeps ---- */

static void TestSweepStaticOrigin(void)
{
    /* A. Static origin, scroll 0, ring on. The 300x200 viewport spans world
     * [-30,270)x[-20,180): negative coords in the left/top apron, the world-
     * residency seam at worldX=0 / worldY=0 (the ring window anchored at the
     * player's map cell [0,256)x[0,256)), the ring's physical-index torus
     * wrapping INSIDE that window, and grid/border on the outer apron. */
    ExpandedSceneResetGrid(1, 2, 3);
    BuildSweepRing();
    AssertFrameMatchesReference(-30, -20, 300, 200);

    /* Focused exact-color pins (hand-derived, independent of RefWorldPixel too):
     *   - ring pixel screen (70,80) = world (40,60): ring index ((0+40)&0xFF)>>3
     *     =5, ((0+60)&0xFF)>>3 =7 -> BG1 tile 1+(5%4)=2 pal 1; texel (0,4) ->
     *     pixel 1+((2*7+4*5+0)%15)=5 -> 0x0115 | 0x8000, winner BG1.
     *   - border pixel screen (0,0) = world (-30,-20): map(-2,-2) -> border
     *     index ((-1)&1)+((-1)&1)*2 = 3 -> metatile 4; local (2,12) -> quadrant
     *     2 -> BG1 tile 40+6=46 pal 0; texel (2,4) -> 1+((46*7+4*5+2)%15)=15 ->
     *     0x010F | 0x8000, winner BG1. */
    {
        static u16 frame[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
        static u8 winner[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];

        assert(DrawMapFrameWithMetaEx(&sBgSnap, frame, winner, NULL,
                                      300, 200, -30, -20));
        assert(frame[80 * 300 + 70] == (ExpectedColor(1, BgPixelIndex(2, 0, 4)) | 0x8000));
        assert(winner[80 * 300 + 70] == NATIVE_COMPOSITE_WIN_BG1);
        assert(frame[0] == (ExpectedColor(0, BgPixelIndex(46, 2, 4)) | 0x8000));
        assert(winner[0] == NATIVE_COMPOSITE_WIN_BG1);
    }

    /* The transparent BG1 ring band (ring cols 24-27 -> world x in [192,224))
     * exposes BG2 as the winner: screen x = world x + 30. */
    {
        static u16 frame[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
        static u8 winner[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
        u8 w;
        u16 c;
        int wx;

        assert(DrawMapFrameWithMetaEx(&sBgSnap, frame, winner, NULL,
                                      300, 200, -30, -20));
        for (wx = 192; wx < 224; wx++)
        {
            RefWorldPixel(&sBgSnap, wx, 60, &c, &w);
            assert(w == NATIVE_COMPOSITE_WIN_BG2);
            assert(winner[100 * 300 + (wx + 30)] == NATIVE_COMPOSITE_WIN_BG2);
        }
    }
    printf("A: static-origin 300x200 sweep == independent reference ok\n");
}

static void TestSweepGridOnly(void)
{
    /* B. Ring disabled: the tile provider is the backup grid then the border.
     * Every pixel of the same 300x200 viewport must match the independent
     * reference -- grid cells resolve directly (16x16 boundaries + quadrants),
     * grid wins over border, and the 2x2 border repeats beyond the grid edge. */
    ExpandedSceneResetGrid(1, 2, 3);
    sBgSnap.bgRingValid = FALSE;
    AssertFrameMatchesReference(-30, -20, 300, 200);
    printf("B: grid/border-only sweep == independent reference ok\n");
}

static void TestSweepNonPresetDims(void)
{
    /* C. Non-preset continuous viewport dimensions (Section 23): 245x163 and
     * 271x181 both render correctly. Their odd width/2 truncation puts the
     * viewport 2 and 15 px left of the origin (left = 120 - 122 = -2 / -15). */
    ExpandedSceneResetGrid(1, 2, 3);
    BuildSweepRing();
    AssertFrameMatchesReference(-2, -1, 245, 163);
    AssertFrameMatchesReference(-15, -10, 271, 181);
    printf("C: non-preset 245x163 and 271x181 sweeps == independent reference ok\n");
}

static void TestSweepMovingFrame(void)
{
    /* D. Moving frame: the CENTRAL 240x160 window samples the ring at the
     * PRESENTATION scroll (Stage 2.1 timing preserved -- the scroll lags the
     * logical camera), while grid/border resolves at the logical coordinate.
     * Distinct per-layer scrolls exercise the central window's wrap of ring
     * columns through the &0xFF torus (e.g. hofs=29 + ringSx=239 -> 268 & 0xFF =
     * 12 -> column 1). Resident MARGIN pixels instead sample the ring at its
     * WORLD ANCHOR (Stage 4A): the scroll's drift from the anchor would
     * otherwise mis-map them on moving frames. The reference mirrors the
     * renderer's split (central scroll, margin anchor). */
    ExpandedSceneResetGrid(1, 2, 3);
    BuildSweepRing();
    sBgSnap.bgHofs[0] = 13; sBgSnap.bgVofs[0] = 7;
    sBgSnap.bgHofs[1] = 29; sBgSnap.bgVofs[1] = 3;
    sBgSnap.bgHofs[2] = 0;  sBgSnap.bgVofs[2] = 0;
    AssertFrameMatchesReference(-30, -20, 300, 200);
    printf("D: moving-frame (presentation scroll) sweep == independent reference ok\n");
}

/* ---- Test I: map-connection strip (grid carries the copied connection) ---- */

static void TestGridConnectionStrip(void)
{
    static u16 frame[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
    static u8 winner[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
    u16 color;
    u8 win;

    /* Ring off; the grid carries the map's connection strips copied at load.
     * The 300x200 viewport reveals world [-30,270)x[-20,180), including the
     * grid's bottom-right edge cell (map row 10, col 11). A pixel inside the
     * grid edge must resolve the GRID metatile, not the border; a pixel just
     * outside (map col 12) must fall to the repeating 2x2 border. */
    ExpandedSceneResetGrid(1, 2, 3);
    sBgSnap.bgRingValid = FALSE;

    assert(DrawMapFrameWithMetaEx(&sBgSnap, frame, winner, NULL,
                                  300, 200, -30, -20));

    /* World (190,170) -> screen (220,190): grid cell (11,10) -> metatile
     * 5+((11*3+10*5)%6) = 5+(83%6) = 10; BG1 NORMAL quadrant 3 (local 14,10)
     * -> tile 100+10*8+7 = 187. The grid (which already holds the copied
     * connection strip) wins over the border here. */
    RefWorldPixel(&sBgSnap, 190, 170, &color, &win);
    assert(frame[190 * 300 + 220] == color);
    assert(winner[190 * 300 + 220] == win);
    assert(sPrimaryMetatiles[10 * NUM_TILES_PER_METATILE + 3 + 4] == BgTile(187, 1));
    {
        u8 p = BgPixelIndex(187, 6, 2);
        assert(frame[190 * 300 + 220] == (ExpectedColor(1, p) | 0x8000));
        assert(winner[190 * 300 + 220] == NATIVE_COMPOSITE_WIN_BG1);
    }

    /* Just outside the grid: world (192,170) -> screen (222,190): map(12,10)
     * -> outside grid -> border index ((13)&1)+((11)&1)*2 = 1+2 = 3 -> metatile
     * 4; local (0,10) -> quadrant 2 -> tile 40+6 = 46. */
    RefWorldPixel(&sBgSnap, 192, 170, &color, &win);
    assert(frame[190 * 300 + 222] == color);
    assert(winner[190 * 300 + 222] == win);
    {
        u8 p = BgPixelIndex(46, 0, 2);
        assert(frame[190 * 300 + 222] == (ExpectedColor(0, p) | 0x8000));
        assert(winner[190 * 300 + 222] == NATIVE_COMPOSITE_WIN_BG1);
    }
    printf("I: map-connection strip resolves grid, not border ok\n");
}

/* ---- Test J: priority winner across providers ---- */

static void TestPriorityWinnerAcrossProviders(void)
{
    static u16 frame[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
    static u8 winner[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];

    /* Ring: all three layers opaque at the same ring index. Fixed priorities
     * (1,2,3) -> BG1 wins; a transparent BG1 entry exposes BG2. */
    ExpandedSceneResetGrid(1, 2, 3);
    SetRingRect(0, 0, 0, 31, 31, BgTile(1, 1));
    SetRingRect(1, 0, 0, 31, 31, BgTile(2, 2));
    SetRingRect(2, 0, 0, 31, 31, BgTile(3, 3));
    assert(DrawMapFrameWithMetaEx(&sBgSnap, frame, winner, NULL, 240, 160, 0, 0));
    assert(winner[0] == NATIVE_COMPOSITE_WIN_BG1);

    ExpandedSceneResetGrid(1, 2, 3);
    SetRingRect(0, 0, 0, 31, 31, 0);              /* BG1 transparent */
    SetRingRect(1, 0, 0, 31, 31, BgTile(2, 2));
    SetRingRect(2, 0, 0, 31, 31, BgTile(3, 3));
    assert(DrawMapFrameWithMetaEx(&sBgSnap, frame, winner, NULL, 240, 160, 0, 0));
    assert(winner[0] == NATIVE_COMPOSITE_WIN_BG2);

    /* Reordered CAPTURED priorities (2,1,3): BG2 wins even though BG1 is opaque
     * -- the winner follows the captured priorities, not the fixed layer order. */
    ExpandedSceneResetGrid(2, 1, 3);
    SetRingRect(0, 0, 0, 31, 31, BgTile(1, 1));
    SetRingRect(1, 0, 0, 31, 31, BgTile(2, 2));
    SetRingRect(2, 0, 0, 31, 31, BgTile(3, 3));
    assert(DrawMapFrameWithMetaEx(&sBgSnap, frame, winner, NULL, 240, 160, 0, 0));
    assert(winner[0] == NATIVE_COMPOSITE_WIN_BG2);

    /* Grid provider: a transparent BG1 tile at a grid pixel exposes BG2. Grid
     * cell (0,0) -> metatile 5; make its BG1 NORMAL quadrant-0 tile transparent.
     * World (1,1) -> map(0,0), local(1,1) -> quadrant 0. */
    ExpandedSceneResetGrid(1, 2, 3);
    sBgSnap.bgRingValid = FALSE;
    sPrimaryMetatiles[5 * NUM_TILES_PER_METATILE + 0 + 4] = 0;
    assert(DrawMapFrameWithMetaEx(&sBgSnap, frame, winner, NULL, 240, 160, 0, 0));
    assert(winner[1 * 240 + 1] == NATIVE_COMPOSITE_WIN_BG2);

    /* Reordered priorities on the grid provider: world (8,1) -> quadrant 2, all
     * opaque; BG2 (prio 1) beats BG1 (prio 2). */
    ExpandedSceneResetGrid(2, 1, 3);
    sBgSnap.bgRingValid = FALSE;
    assert(DrawMapFrameWithMetaEx(&sBgSnap, frame, winner, NULL, 240, 160, 0, 0));
    assert(winner[1 * 240 + 8] == NATIVE_COMPOSITE_WIN_BG2);

    printf("J: priority winner across ring/grid providers ok\n");
}

/* ---- Test K: sprite expansion positions through the real composite ---- */

static struct NativeViewport sViewport300x200;

static void Make300x200Viewport(void)
{
    NativeOverworldViewport_Init(&sViewport300x200);
    sViewport300x200.scaleQ8 = 320; /* 1.25x -> 300x200 */
    NativeOverworldViewport_Recompute(&sViewport300x200);
    assert(sViewport300x200.width == 300);
    assert(sViewport300x200.height == 200);
}

static void RunExpandedComposite(struct NativeFieldCompositorReport *report)
{
    assert(NativeOverworldRenderer_DrawExpandedComposite(
        &sBgSnap, &sSnap, &sOut, &sViewport300x200,
        sExpandedBg, sExpandedBgWinner, sExpandedBgLayers,
        sExpandedFinal, sExpandedWinner, report));
    assert(report->reason == NATIVE_COMPOSITE_OK);
}

/* Assert the final 300x200 composite shows an 8x8 4bpp sprite whose top-left is
 * (sx0,sy0): the pixel at (px,py) samples the sprite's tile at texel
 * ((px-sx0)%8, (py-sy0)%8). */
static void AssertFinalSprite(u16 px, u16 py, u16 sx0, u16 sy0, u8 pal, u16 tile)
{
    u32 idx = (u32)py * 300 + px;
    u8 texX = (u8)((px - sx0) % 8);
    u8 texY = (u8)((py - sy0) % 8);
    u16 expected = ExpectedColor(pal, BgPixelIndex(tile, texX, texY)) | 0x8000;

    if (sExpandedFinal[idx] != expected)
    {
        fprintf(stderr, "sprite expected at (%u,%u) 0x%04X got 0x%04X win=%u\n",
                px, py, expected, sExpandedFinal[idx], sExpandedWinner[idx]);
        assert(0);
    }
}

static void TestSpriteExpansion(void)
{
    struct NativeFieldCompositorReport report;
    struct NativeSpriteDrawCommand *c;

    ExpandedSceneResetGrid(1, 2, 3);
    SetRingRect(0, 0, 0, 31, 31, BgTile(1, 1)); /* opaque BG (BG1 prio 1) */

    /* K1. A validated WORLD command at its pre-wrap top-left (40,60) is placed
     * at signedX + margin = (70,80): the world-anchored sprite keeps its world
     * position, which the expanded viewport shows 30/20 px right/down of the
     * 240x160 crop. */
    SnapReset();
    SetOam(0, 40, 60, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    c = &sSnap.commands[sSnap.commandCount - 1];
    c->placement = NATIVE_SPRITE_PLACEMENT_WORLD;
    c->visibility = NATIVE_SPRITE_GBA_EMITTED;
    c->expandedEligible = TRUE;
    c->signedX = 40;
    c->signedY = 60;
    Make300x200Viewport();
    RunExpandedComposite(&report);
    AssertFinalSprite(70, 80, 70, 80, 1, 4);
    AssertFinalSprite(70, 81, 70, 80, 1, 4);
    AssertFinalSprite(70, 87, 70, 80, 1, 4);

    /* K2. A raw-OAM entry with no command stays in the 240x160 center (screen
     * anchored): canonicalized OAM (200,120) + margin -> (230,140). */
    SnapReset();
    SetOam(1, 200, 120, ST_OAM_SQUARE, ST_OAM_SIZE_0, 6, 3, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    RunExpandedComposite(&report);
    AssertFinalSprite(230, 140, 230, 140, 3, 6);

    /* K3. A sprite the 240x160 view culled (world x = -20, off the left edge of
     * the old crop) is REVEALED in the expanded apron at signedX + margin =
     * (-20+30, 80+20) = (10,100): an object-event sprite outside the old crop. */
    SnapReset();
    c = &sSnap.unpresented[0];
    memset(c, 0, sizeof(*c));
    c->commandId = 0;
    c->sourceKind = NATIVE_SPRITE_SOURCE_GSPRITE;
    c->spriteId = 2;
    c->oamIndex = NATIVE_OBJ_NO_ID;
    c->placement = NATIVE_SPRITE_PLACEMENT_WORLD;
    c->visibility = NATIVE_SPRITE_VANILLA_VIEWPORT_CULLED;
    c->expandedEligible = TRUE;
    c->signedX = -20;
    c->signedY = 80;
    c->tileNum = 5;
    c->shape = ST_OAM_SQUARE;
    c->size = ST_OAM_SIZE_0;
    c->priority = 0;
    c->paletteNum = 2;
    c->bpp = ST_OAM_4BPP;
    c->objMode = ST_OAM_OBJ_NORMAL;
    c->affineMode = ST_OAM_AFFINE_OFF;
    c->mosaic = 0;
    c->flipX = 0;
    c->flipY = 0;
    sSnap.unpresentedCount = 1;
    RunExpandedComposite(&report);
    AssertFinalSprite(10, 100, 10, 100, 2, 5);

    printf("K: sprite expansion positions (WORLD + margin, RAW center, revealed) ok\n");
}

int main(void)
{
    TestSweepStaticOrigin();        /* A */
    TestSweepGridOnly();            /* B */
    TestSweepNonPresetDims();       /* C */
    TestSweepMovingFrame();         /* D */
    TestGridConnectionStrip();      /* I */
    TestPriorityWinnerAcrossProviders(); /* J */
    TestSpriteExpansion();          /* K */
    printf("native overworld expanded world unit test passed\n");
    return 0;
}

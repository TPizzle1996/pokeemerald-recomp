/*
 * Stage 4A expanded-viewport parity test: NativeOverworldRenderer_DrawExpandedComposite.
 *
 * This is the FIRST functional proof of the expanded entry point. A synthetic
 * field frame is built as a hand-constructed NativeOverworldSnapshot (BG ring,
 * BG char memory, BG palette, captured BGCNT priorities) plus a Stage 3B OBJ
 * snapshot, and the SAME data is fed through two paths:
 *
 *   1. The PROVEN 240x160 path: NativeObjRender_Rasterize() then
 *      NativeOverworldRenderer_DrawCompositeFrame() -> the oracle-hardened
 *      native composite + per-pixel winner.
 *   2. The Stage 4A EXPANDED path: NativeOverworldRenderer_DrawExpandedComposite()
 *      at 300x200 -> the full expanded composite + per-pixel winner.
 *
 * Then:
 *   - CENTER CROP PARITY (the Section 8 hard invariant at entry level): the
 *     expanded frame's 240x160 center crop (offset by the viewport margins)
 *     must equal the 240x160 composite pixel-for-pixel, color AND winner. This
 *     is byte-identity through the SAME loop the margins use, so any margin
 *     derivation, ring sampling, or placement bug in the expanded path breaks
 *     it.
 *   - MARGIN WORLD-REVEAL: the margins are NOT a memcpy of the center. Ring
 *     margin pixels are checked against an INDEPENDENT world-coordinate
 *     reference (direct ring index + char-memory/palette sampling, NOT the
 *     renderer's resolve/sample functions), and the left/top apron (outside the
 *     ring gate) resolves the snapshot border/metatile fallback correctly.
 *   - NAMED FALLBACK GATES: an inactive viewport, NULL input, BG0 overlay
 *     content, a firing blend, and an unsupported 8bpp OBJ each report the
 *     explicit fallback reason (Section 20).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"
#include "native_obj_renderer_shared.h"
#include "../src/platform/native_field_compositor.c"

/* config.h (pulled in by global.h via the included .c files above) defines
 * NDEBUG unconditionally; re-include <assert.h> last (NDEBUG cleared) so the
 * parity assertions below are actually enforced. */
#undef NDEBUG
#include <assert.h>

/* ---- Stubs of the hardware globals the native renderer TU references ---- */
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

#define SCREENBASE_BG1 29
#define SCREENBASE_BG2 28
#define SCREENBASE_BG3 30
#define SCREENBASE_BG0 15

#define EXPANDED_WIDTH_MAX  NATIVE_VIEWPORT_MAX_WIDTH
#define EXPANDED_HEIGHT_MAX NATIVE_VIEWPORT_MAX_HEIGHT

static struct NativeOverworldSnapshot sBgSnap;
static u16 sBg240[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u8 sBgWinner240[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u16 sBgLayers240[4 * DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u16 sFinal240[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u8 sFinalWinner240[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u16 sExpandedBg[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
static u8 sExpandedBgWinner[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
static u16 sExpandedBgLayers[4 * EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
static u16 sExpandedFinal[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];
static u8 sExpandedWinner[EXPANDED_WIDTH_MAX * EXPANDED_HEIGHT_MAX];

static u16 sPrimaryMetatiles[NUM_METATILES_IN_PRIMARY * NUM_TILES_PER_METATILE];
static u16 sPrimaryAttributes[NUM_METATILES_IN_PRIMARY];
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

/* Deterministic BG char memory: tiles 1..959 opaque with varied texels, tiles
 * 0 and 960..1023 transparent (so the BG0 screenbase-15 region stays zero and
 * the BG0-content gate passes). Mirrors the Stage 3C oracle harness. */
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

static void ExpandedSceneReset(u8 bg1Prio, u8 bg2Prio, u8 bg3Prio)
{
    memset(VRAM_, 0, VRAM_SIZE);
    memset(PLTT, 0, PLTT_SIZE);
    memset(&sBgSnap, 0, sizeof(sBgSnap));
    memset(sPrimaryMetatiles, 0, sizeof(sPrimaryMetatiles));
    memset(sPrimaryAttributes, 0, sizeof(sPrimaryAttributes));
    memset(sBorderMetatiles, 0, sizeof(sBorderMetatiles));

    FillBgCharMemory();
    FillBgPalette();

    memcpy(sBgSnap.bgVram, VRAM_, 0x8000); /* tiles 0..1023 char data */
    memcpy(sBgSnap.palette, PLTT, PLTT_SIZE);

    sBgSnap.bg0Cnt = BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0)
                   | BGCNT_SCREENBASE(SCREENBASE_BG0) | BGCNT_16COLOR
                   | BGCNT_TXT256x256;
    sBgSnap.bgCnt[0] = BGCNT_PRIORITY(bg1Prio) | BGCNT_CHARBASE(0)
                     | BGCNT_SCREENBASE(SCREENBASE_BG1) | BGCNT_16COLOR
                     | BGCNT_TXT256x256;
    sBgSnap.bgCnt[1] = BGCNT_PRIORITY(bg2Prio) | BGCNT_CHARBASE(0)
                     | BGCNT_SCREENBASE(SCREENBASE_BG2) | BGCNT_16COLOR
                     | BGCNT_TXT256x256;
    sBgSnap.bgCnt[2] = BGCNT_PRIORITY(bg3Prio) | BGCNT_CHARBASE(0)
                     | BGCNT_SCREENBASE(SCREENBASE_BG3) | BGCNT_16COLOR
                     | BGCNT_TXT256x256;

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

    /* Snapshot border/metatile fallback (used outside the ring gate). */
    sBgSnap.primaryMetatiles = sPrimaryMetatiles;
    sBgSnap.primaryMetatileAttributes = sPrimaryAttributes;
    sBgSnap.border = sBorderMetatiles;
    sBorderMetatiles[0] = 1;
    sBorderMetatiles[1] = 2;
    sBorderMetatiles[2] = 3;
    sBorderMetatiles[3] = 4;
}

/* The BG0-content gate samples the SNAPSHOT's bgVram at BG0's screenbase. */
static void MakeBg0Opaque(void)
{
    sBgSnap.bgVram[SCREENBASE_BG0 * 0x400] = 1; /* tile 1, opaque */
}

/* Stage 4A world-relative GSPRITE command at its pre-wrap top-left. */
static void AddWorldCommand(u8 oamIndex, u8 spriteId, s32 signedX, s32 signedY)
{
    struct NativeSpriteDrawCommand *c;

    AddCommand(oamIndex, NATIVE_SPRITE_SOURCE_GSPRITE, spriteId, 0);
    c = &sSnap.commands[sSnap.commandCount - 1];
    c->placement = NATIVE_SPRITE_PLACEMENT_WORLD;
    c->visibility = NATIVE_SPRITE_GBA_EMITTED;
    c->expandedEligible = TRUE;
    c->signedX = signedX;
    c->signedY = signedY;
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

/* 4bpp char-memory sample, mirroring the GBA text-mode sampler. */
static u16 RefSampleTile(const struct NativeOverworldSnapshot *snap, u16 entry,
                         s32 sampleX, s32 sampleY, bool32 *opaque)
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
    {
        *opaque = FALSE;
        return 0;
    }
    *opaque = TRUE;
    return palette[paletteNum * 16 + pixel];
}

/* Ring-path expected color: index at the PRESENTATION scroll, sub-pixel at the
 * same scroll -- exactly the renderer's ring convention. */
static u16 RefRingPixel(const struct NativeOverworldSnapshot *snap, u8 bg,
                        s32 worldX, s32 worldY, bool32 *opaque)
{
    s32 originX = snap->cameraMapX * 16 + snap->cameraX;
    s32 originY = snap->cameraMapY * 16 + snap->cameraY;
    s32 sampleX = snap->bgHofs[bg] + (worldX - originX);
    s32 sampleY = snap->bgVofs[bg] + (worldY - originY);
    u16 entry = snap->bgRing[bg][((sampleY & 0xFF) >> 3) * 32
                                 + ((sampleX & 0xFF) >> 3)];

    return RefSampleTile(snap, entry, sampleX, sampleY, opaque);
}

/* Grid/border-path expected color: entry from the snapshot border metatile at
 * the logical world coordinate (BG1 NORMAL layer), sub-pixel at worldX/worldY. */
static u16 RefBorderPixel(const struct NativeOverworldSnapshot *snap,
                          s32 worldX, s32 worldY, bool32 *opaque)
{
    s32 mapX = RefFloorDiv(worldX, 16);
    s32 mapY = RefFloorDiv(worldY, 16);
    s32 localX = RefPosMod(worldX, 16);
    s32 localY = RefPosMod(worldY, 16);
    u8 quadrant = (u8)((localY / 8) * 2 + localX / 8);
    u8 borderIndex = (u8)(((mapX + 1) & 1) + ((mapY + 1) & 1) * 2);
    u16 metatileId = snap->border[borderIndex] & MAPGRID_METATILE_ID_MASK;
    const u16 *tiles = snap->primaryMetatiles + metatileId * NUM_TILES_PER_METATILE;
    u16 entry = tiles[quadrant + 4]; /* BG1 NORMAL layer */

    return RefSampleTile(snap, entry, worldX, worldY, opaque);
}

/* ---- Runners ---- */

static void Run240Composite(void)
{
    struct NativeFieldCompositorReport report;

    assert(NativeObjRender_Rasterize(&sSnap, &sOut));
    assert(sOut.produced == TRUE);
    assert(sOut.capabilityFlags == 0);
    assert(NativeOverworldRenderer_DrawCompositeFrame(&sBgSnap, &sOut,
                                                      sBg240, sBgWinner240, sBgLayers240,
                                                      sFinal240, sFinalWinner240, &report));
    assert(report.reason == NATIVE_COMPOSITE_OK);
}

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

/* The 300x200 viewport's margins, derived with the SAME continuous formula the
 * expanded entry uses. */
#define EXPANDED_MARGIN_X ((300 - DISPLAY_WIDTH) / 2)
#define EXPANDED_MARGIN_Y ((200 - DISPLAY_HEIGHT) / 2)

/* ---- Tests ---- */

/* Section 8 hard invariant at the entry level: the expanded frame's 240x160
 * center crop must equal the proven 240x160 composite, color AND winner, across
 * the 8 scene types the runtime parity capture classifies. Each scene is rebuilt
 * as a distinct synthetic configuration (BG priorities, ring patterns, scroll,
 * OBJ kind); the crop is always ring-covered, so the invariant proves the
 * expanded path renders the center through the SAME loops the margins use,
 * byte-identically. */

static void FillThreeLayerRing(void)
{
    s32 ry;
    s32 rx;

    for (ry = 0; ry < 32; ry++)
    {
        for (rx = 0; rx < 32; rx++)
        {
            sBgSnap.bgRing[0][ry * 32 + rx] = BgTile((u16)(1 + (rx + ry) % 7), 1);
            sBgSnap.bgRing[1][ry * 32 + rx] = BgTile((u16)(1 + (rx * 3 + ry * 5) % 5), 2);
            sBgSnap.bgRing[2][ry * 32 + rx] = BgTile((u16)(1 + (rx + ry * 2) % 11), 3);
        }
    }
}

static void AssertCenterCropParity(const char *label)
{
    struct NativeFieldCompositorReport report;
    s32 my;
    s32 mx;

    Run240Composite();
    RunExpandedComposite(&report);
    for (my = 0; my < DISPLAY_HEIGHT; my++)
    {
        for (mx = 0; mx < DISPLAY_WIDTH; mx++)
        {
            u32 expandedIdx = (u32)(my + EXPANDED_MARGIN_Y) * 300
                             + (mx + EXPANDED_MARGIN_X);

            if ((sExpandedFinal[expandedIdx] & 0x7FFF)
                    != (sFinal240[my * DISPLAY_WIDTH + mx] & 0x7FFF)
             || sExpandedWinner[expandedIdx]
                    != sFinalWinner240[my * DISPLAY_WIDTH + mx])
            {
                fprintf(stderr,
                        "%s: crop pixel (%d,%d): expanded color=0x%04X win=%u "
                        "vs 240x160 color=0x%04X win=%u\n",
                        label, mx, my,
                        sExpandedFinal[expandedIdx], sExpandedWinner[expandedIdx],
                        sFinal240[my * DISPLAY_WIDTH + mx],
                        sFinalWinner240[my * DISPLAY_WIDTH + mx]);
                assert(0);
            }
        }
    }
    printf("%s: expanded center crop == 240x160 composite (color+winner) ok\n",
           label);
}

/* s1 stationary outdoor: the classic 3-layer field, camera at the origin, ring
 * at the logical position, two world-anchored sprites. */
static void TestSceneStationaryOutdoor(void)
{
    ExpandedSceneReset(1, 2, 3);
    FillThreeLayerRing();
    SnapReset();
    /* World-anchored sprites inside the 240x160 crop: their signedX/Y equal the
     * final OAM, so the expanded path moves them outward by the margin and they
     * land back in the center crop. */
    SetOam(0, 40, 60, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddWorldCommand(0, 0, 40, 60);
    SetOam(1, 200, 120, ST_OAM_SQUARE, ST_OAM_SIZE_0, 5, 2, 2, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddWorldCommand(1, 1, 200, 120);
    Make300x200Viewport();
    AssertCenterCropParity("s1 stationary outdoor");
}

/* s2 moving frame: the presentation scroll lags the logical camera (Stage 2.1),
 * so the ring is sampled at bgHofs/bgVofs != camera. The crop must still equal
 * the 240x160 path, which samples the same scroll. */
static void TestSceneMovingFrame(void)
{
    ExpandedSceneReset(1, 2, 3);
    FillThreeLayerRing();
    sBgSnap.bgHofs[0] = 3; sBgSnap.bgVofs[0] = 2;
    sBgSnap.bgHofs[1] = 5; sBgSnap.bgVofs[1] = 4;
    sBgSnap.bgHofs[2] = 7; sBgSnap.bgVofs[2] = 6;
    sBgSnap.cameraX = 8; sBgSnap.cameraY = 6;
    sCameraX = 8; sCameraY = 6;
    SnapReset();
    SetOam(0, 60, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 6, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddWorldCommand(0, 0, 60, 40);
    SetOam(1, 180, 100, ST_OAM_SQUARE, ST_OAM_SIZE_0, 7, 3, 2, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddWorldCommand(1, 1, 180, 100);
    Make300x200Viewport();
    AssertCenterCropParity("s2 moving frame");
}

/* s3 animated water: BG2 is the water layer (opaque, scrolled by bgHofs) under
 * BG1 land, which is transparent over a "shore" column. */
static void TestSceneAnimatedWater(void)
{
    s32 ry;
    s32 rx;

    ExpandedSceneReset(1, 2, 3);
    for (ry = 0; ry < 32; ry++)
    {
        for (rx = 0; rx < 32; rx++)
        {
            sBgSnap.bgRing[0][ry * 32 + rx] = (rx >= 20) ? 0
                : BgTile((u16)(1 + (rx % 4)), 1);
            sBgSnap.bgRing[1][ry * 32 + rx] = BgTile((u16)(1 + ((rx + ry) % 3)), 2);
            sBgSnap.bgRing[2][ry * 32 + rx] = 0;
        }
    }
    sBgSnap.bgHofs[1] = 4; sBgSnap.bgVofs[1] = 0;
    SnapReset();
    SetOam(0, 100, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 8, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddWorldCommand(0, 0, 100, 50);
    Make300x200Viewport();
    AssertCenterCropParity("s3 animated water");
}

/* s4 indoor: BG3 floor (farthest) under BG2 walls; BG1 absent. */
static void TestSceneIndoor(void)
{
    s32 ry;
    s32 rx;

    ExpandedSceneReset(1, 2, 3);
    for (ry = 0; ry < 32; ry++)
    {
        for (rx = 0; rx < 32; rx++)
        {
            bool32 isWall = (rx % 8 < 2 || ry % 8 < 2);

            sBgSnap.bgRing[0][ry * 32 + rx] = 0;
            sBgSnap.bgRing[1][ry * 32 + rx] = isWall ? BgTile((u16)(2 + rx % 3), 4) : 0;
            sBgSnap.bgRing[2][ry * 32 + rx] = BgTile((u16)(1 + ((rx + ry) % 5)), 5);
        }
    }
    SnapReset();
    SetOam(0, 50, 90, ST_OAM_SQUARE, ST_OAM_SIZE_1, 9, 2, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddWorldCommand(0, 0, 50, 90);
    Make300x200Viewport();
    AssertCenterCropParity("s4 indoor");
}

/* s5 cave: a single opaque dark-palette layer (BG1) + player sprite. */
static void TestSceneCave(void)
{
    s32 ry;
    s32 rx;

    ExpandedSceneReset(0, 1, 2);
    for (ry = 0; ry < 32; ry++)
    {
        for (rx = 0; rx < 32; rx++)
        {
            sBgSnap.bgRing[0][ry * 32 + rx] = BgTile((u16)(1 + ((rx * 7 + ry * 3) % 9)), 7);
            sBgSnap.bgRing[1][ry * 32 + rx] = 0;
            sBgSnap.bgRing[2][ry * 32 + rx] = 0;
        }
    }
    SnapReset();
    SetOam(0, 30, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 10, 0, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddWorldCommand(0, 0, 30, 40);
    Make300x200Viewport();
    AssertCenterCropParity("s5 cave");
}

/* s6 map connection: a two-layer seam (BG1 terrain with a transparent "road"
 * column filled by BG2) -- the pattern a map-connection edge produces. */
static void TestSceneMapConnection(void)
{
    s32 ry;
    s32 rx;

    ExpandedSceneReset(1, 2, 0);
    for (ry = 0; ry < 32; ry++)
    {
        for (rx = 0; rx < 32; rx++)
        {
            bool32 isRoad = (rx == 16 || rx == 17);

            sBgSnap.bgRing[0][ry * 32 + rx] = isRoad ? 0
                : BgTile((u16)(1 + ((rx + ry) % 6)), 1);
            sBgSnap.bgRing[1][ry * 32 + rx] = isRoad ? BgTile((u16)(1 + (ry % 4)), 2) : 0;
            sBgSnap.bgRing[2][ry * 32 + rx] = 0;
        }
    }
    SnapReset();
    SetOam(0, 128, 80, ST_OAM_SQUARE, ST_OAM_SIZE_0, 11, 3, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddWorldCommand(0, 0, 128, 80);
    Make300x200Viewport();
    AssertCenterCropParity("s6 map connection");
}

/* s7 affine sprite: a RAW-OAM affine OBJ (no command -> SCREEN_FIXED policy) is
 * kept in the 240x160 center by both paths, so the rotated texels must match
 * exactly. 90-degree rotation: pa=pd=0, pb=pc=256. */
static void TestSceneAffineSprite(void)
{
    s32 ry;
    s32 rx;

    ExpandedSceneReset(0, 1, 2);
    for (ry = 0; ry < 32; ry++)
    {
        for (rx = 0; rx < 32; rx++)
            sBgSnap.bgRing[0][ry * 32 + rx] = BgTile((u16)(1 + (rx % 3)), 1);
    }
    SnapReset();
    SetOam(0, 40, 60, ST_OAM_SQUARE, ST_OAM_SIZE_1, 12, 2, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(0, 1);
    SetAffineMatrix(1, 0, 0x100, 0x100, 0);
    Make300x200Viewport();
    AssertCenterCropParity("s7 affine sprite");
}

/* s8 semitransparent OBJ: objMode BLEND over an opaque BG1 target. TGT2 names
 * BG1 as the blend target; the effect stays NONE so every gate remains green
 * and only the OBJ-level semi flag drives the 50/50 alpha blend. RAW OAM (no
 * command) keeps the semi mode: a command would need cmd->objMode set. */
static void TestSceneSemiTransparentObj(void)
{
    s32 ry;
    s32 rx;

    ExpandedSceneReset(1, 2, 0);
    for (ry = 0; ry < 32; ry++)
    {
        for (rx = 0; rx < 32; rx++)
        {
            sBgSnap.bgRing[0][ry * 32 + rx] = BgTile((u16)(1 + ((rx + ry) % 6)), 1);
            sBgSnap.bgRing[1][ry * 32 + rx] = 0;
            sBgSnap.bgRing[2][ry * 32 + rx] = 0;
        }
    }
    sBgSnap.bldCnt = BLDCNT_TGT2_BG1;
    sBgSnap.bldAlpha = 0x1010; /* EVA=16, EVB=16 -> 50/50 */
    SnapReset();
    SetOam(0, 60, 70, ST_OAM_SQUARE, ST_OAM_SIZE_1, 13, 1, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    Make300x200Viewport();
    AssertCenterCropParity("s8 semitransparent OBJ");
}

/* The margins must render REAL world beyond the 240x160 crop -- not a memcpy of
 * the center. Ring margin pixels are checked against the independent reference;
 * the left/top apron (outside the ring gate) resolves the border fallback. */
static void TestExpandedMarginsRevealWorld(void)
{
    s32 ry;
    s32 rx;

    ExpandedSceneReset(1, 2, 3);
    /* BG1 only: vertical stripes so ring columns are distinguishable. BG2/3
     * ring is transparent everywhere. */
    for (ry = 0; ry < 32; ry++)
    {
        for (rx = 0; rx < 32; rx++)
        {
            sBgSnap.bgRing[0][ry * 32 + rx] = BgTile((u16)(1 + (rx % 4)), 1);
            sBgSnap.bgRing[1][ry * 32 + rx] = 0;
            sBgSnap.bgRing[2][ry * 32 + rx] = 0;
        }
    }
    /* Border fallback metatile 2, BG1 NORMAL layer, quadrant 2 (world -30,60
     * is local (2,12) within its 16x16 metatile -> quadrant 2): a distinct
     * opaque tile so the left/top apron has a verifiable color. Metatile 1
     * quadrant 0 is set too: world (-10,80) falls on border index 0 (map(-1,5)
     * -> borderIndex 0) with local (6,0) -> quadrant 0, and that coordinate is
     * now correctly resolved by the border fallback (it is NOT ring-resident:
     * the residency window starts at worldX=0). */
    sPrimaryMetatiles[2 * NUM_TILES_PER_METATILE + 2 + 4] = BgTile(7, 2);
    sPrimaryMetatiles[1 * NUM_TILES_PER_METATILE + 0 + 4] = BgTile(7, 2);
    SnapReset();

    Make300x200Viewport();
    {
        struct NativeFieldCompositorReport report;
        s32 sample;
        u16 expected;
        bool32 opaque;

        RunExpandedComposite(&report);

        /* Right margin (ring region): world (220,80) -> ring col 27, tile 4. */
        expected = RefRingPixel(&sBgSnap, 0, 220, 80, &opaque);
        assert(opaque);
        assert((sExpandedFinal[100 * 300 + 250] & 0x7FFF) == (expected & 0x7FFF));

        /* Bottom margin (ring region): world (120,160) -> row 20, col 15. */
        expected = RefRingPixel(&sBgSnap, 0, 120, 160, &opaque);
        assert(opaque);
        assert((sExpandedFinal[180 * 300 + 150] & 0x7FFF) == (expected & 0x7FFF));

        /* Left margin: world (-10,80) is OUTSIDE the ring's world-residency
         * window (residency starts at worldX=0, the player's map cell), so it
         * must resolve the border fallback (map(-1,5) -> borderIndex 0 ->
         * metatile 1) -- NOT the ring torus column 30 the old gate aliased. */
        expected = RefBorderPixel(&sBgSnap, -10, 80, &opaque);
        assert(opaque);
        assert((sExpandedFinal[100 * 300 + 20] & 0x7FFF) == (expected & 0x7FFF));

        /* Left/top apron (outside the residency window): world (-30,60) is on
         * the repeating border (borderIndex 1 -> metatile 2). */
        expected = RefBorderPixel(&sBgSnap, -30, 60, &opaque);
        assert(opaque);
        assert((sExpandedFinal[80 * 300 + 0] & 0x7FFF) == (expected & 0x7FFF));

        /* Reveal, not duplication: a border-fallback margin pixel must differ
         * from the ring pixel at the corresponding center crop column
         * (different providers -> different content). */
        {
            bool32 marginOpaque;
            bool32 centerOpaque;
            u16 marginColor = RefBorderPixel(&sBgSnap, -10, 80, &marginOpaque);
            u16 centerColor = RefRingPixel(&sBgSnap, 0, 140, 80, &centerOpaque);

            assert(marginOpaque && centerOpaque);
            assert(marginColor != centerColor);
        }

        /* The margin never leaks the old center-memcpy: the top-left corner is
         * world (-30,-20) (border), not a copy of the center's top-left. */
        assert((sExpandedFinal[0] & 0x7FFF) != (sExpandedFinal[20 * 300 + 30] & 0x7FFF)
            || (sExpandedFinal[0] & 0x8000) == 0);
    }
    printf("expanded margins reveal independent world (ring + border) ok\n");
}

/* Section 20: named fallback reasons -- never compose a frame that must fall
 * back, and report WHY. */
static void TestExpandedFallbackGates(void)
{
    struct NativeFieldCompositorReport report;
    struct NativeViewport inactive;

    ExpandedSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 31, 31, BgTile(1, 1));
    SnapReset();

    /* Inactive (240x160) viewport -> VIEWPORT_INVALID. */
    NativeOverworldViewport_Init(&inactive);
    assert(!NativeOverworldRenderer_DrawExpandedComposite(
        &sBgSnap, &sSnap, &sOut, &inactive,
        sExpandedBg, sExpandedBgWinner, sExpandedBgLayers,
        sExpandedFinal, sExpandedWinner, &report));
    assert(report.reason == NATIVE_COMPOSITE_FALLBACK_VIEWPORT_INVALID);

    /* NULL input -> NULL_INPUT. */
    Make300x200Viewport();
    assert(!NativeOverworldRenderer_DrawExpandedComposite(
        NULL, &sSnap, &sOut, &sViewport300x200,
        sExpandedBg, sExpandedBgWinner, sExpandedBgLayers,
        sExpandedFinal, sExpandedWinner, &report));
    assert(report.reason == NATIVE_COMPOSITE_FALLBACK_NULL_INPUT);

    /* BG0 would render a visible pixel -> BG0_OVERLAY. */
    MakeBg0Opaque();
    assert(!NativeOverworldRenderer_DrawExpandedComposite(
        &sBgSnap, &sSnap, &sOut, &sViewport300x200,
        sExpandedBg, sExpandedBgWinner, sExpandedBgLayers,
        sExpandedFinal, sExpandedWinner, &report));
    assert(report.reason == NATIVE_COMPOSITE_FALLBACK_BG0_OVERLAY);
    assert(report.bg0Content == 1);
    sBgSnap.bgVram[SCREENBASE_BG0 * 0x400] = 0;

    /* A blend that would recolor the winner -> BLEND_EFFECT. */
    sBgSnap.bldCnt = BLDCNT_EFFECT_BLEND | BLDCNT_TGT1_BG0 | BLDCNT_TGT2_BG1;
    assert(!NativeOverworldRenderer_DrawExpandedComposite(
        &sBgSnap, &sSnap, &sOut, &sViewport300x200,
        sExpandedBg, sExpandedBgWinner, sExpandedBgLayers,
        sExpandedFinal, sExpandedWinner, &report));
    assert(report.reason == NATIVE_COMPOSITE_FALLBACK_BLEND_EFFECT);
    sBgSnap.bldCnt = 0;

    /* An unsupported 8bpp OBJ presented into the viewport -> OBJ_UNSUPPORTED. */
    SnapReset();
    SetOam(0, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 1, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_8BPP, false, false);
    AddWorldCommand(0, 0, 10, 10);
    assert(!NativeOverworldRenderer_DrawExpandedComposite(
        &sBgSnap, &sSnap, &sOut, &sViewport300x200,
        sExpandedBg, sExpandedBgWinner, sExpandedBgLayers,
        sExpandedFinal, sExpandedWinner, &report));
    assert(report.reason == NATIVE_COMPOSITE_FALLBACK_OBJ_UNSUPPORTED);
    assert((report.objCapabilityFlags & NATIVE_OBJ_CAP_8BPP) != 0);

    printf("expanded named fallback gates ok\n");
}

int main(void)
{
    /* Section 8: the 240x160 center crop of the 300x200 expanded frame equals
     * the proven 240x160 composite (color AND winner) for all 8 scene types. */
    TestSceneStationaryOutdoor();
    TestSceneMovingFrame();
    TestSceneAnimatedWater();
    TestSceneIndoor();
    TestSceneCave();
    TestSceneMapConnection();
    TestSceneAffineSprite();
    TestSceneSemiTransparentObj();
    TestExpandedMarginsRevealWorld();
    TestExpandedFallbackGates();
    printf("native overworld expanded parity unit test passed\n");
    return 0;
}

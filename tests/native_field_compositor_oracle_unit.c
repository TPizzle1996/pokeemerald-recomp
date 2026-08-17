/*
 * Stage 3C real-oracle harness: native BG + normal-OBJ final composite vs the
 * ACTUAL gba_easy_draw.c DrawFrame composite.
 *
 * This is the authoritative Stage 3C proof. A synthetic field frame is built as
 * a hand-constructed NativeOverworldSnapshot (BG ring, BG char memory, BG
 * palette, captured BGCNT priorities -- including REORDERED priorities, which
 * the production capture gate rejects by design) plus a Stage 3B OBJ snapshot,
 * and the SAME data is published into the live VRAM / PLTT / OAM / registers
 * the real DrawFrame reads. Then:
 *
 *   1. Native: NativeOverworldRenderer_DrawCompositeFrame() renders the native
 *      final composite + per-pixel winner map (proven BG merge + proven OBJ
 *      layers + Stage 3C priority merge).
 *   2. Oracle: DrawFrame(gbaImage) with the gParityCompositeLayers seam fills
 *      the real GBA composite frame + its per-pixel winner map
 *      (0=backdrop, 1-4=BG0-3, 5-8=OBJ priority 0-3).
 *   3. Compare every pixel: color (masked 0x7FFF) AND winner must match.
 *
 * The winner comparison is the crux: it proves the native compositor selects
 * the SAME final-pixel source the GBA hardware selects for every BG/OBJ
 * priority and tie combination.
 *
 * Unsupported-feature frames must NOT be composited: semi-transparent OBJ,
 * blend that would fire, and BG0 overlay content all assert the explicit
 * fallback reason and a FALSE return. (Stage 3D: affine OBJ -- normal and
 * double-size -- is now supported and composites like any normal OBJ, so it is
 * compared here against the oracle instead of asserting a fallback.)
 *
 * gba_easy_draw.c is compiled separately with -DRENDERER_EASY_DRAW and linked
 * here; see tests/native_overworld_renderer_test.sh.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"
#include "native_obj_renderer_shared.h"
#include "../src/platform/native_field_compositor.c"

/* config.h (pulled in by global.h via the included .c files above) defines
 * NDEBUG unconditionally, and glibc's <assert.h> redefines assert on EVERY
 * include based on the CURRENT NDEBUG -- so the shared header's assert.h include
 * saw NDEBUG already defined and silently disabled every assert in this TU.
 * Re-include <assert.h> last (NDEBUG cleared) so the parity assertions below are
 * actually enforced. */
#undef NDEBUG
#include <assert.h>

/* ---- Real oracle symbols (defined in the separate gba_easy_draw.c TU) ---- */
extern u8 *gParityCompositeLayers;
extern u8 gParityCompositeProduced;
extern void DrawFrame(u16 *pixels);

/* ---- Stubs of the hardware globals gba_easy_draw.c reads ---- */
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

/* ---- Stubs of game globals the native renderer TU references ---- */
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

/* ---- Scene: hand-built BG snapshot + live state ---- */

#define SCREENBASE_BG1 29
#define SCREENBASE_BG2 28
#define SCREENBASE_BG3 30
#define SCREENBASE_BG0 15

static struct NativeOverworldSnapshot sBgSnap;
static u16 sNativeBg[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u8 sNativeBgWinner[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u16 sNativeBgLayers[4 * DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u16 sNativeFinal[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u8 sNativeFinalWinner[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u16 sGbaImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u8 sGbaCompositeLayers[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u8 sTraceBgPriority[4] = {0, 1, 2, 3}; /* BG0 unused, BG1/2/3 prio 1/2/3 */
static struct NativeCompositeBlendState sBlend; /* mirrors sBgSnap registers */

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
 * 0 and 960..1023 transparent (so the BG0 screenbase-15 region at bytes
 * 0x7800-0x8000 stays zero and the BG0-content gate passes). */
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

/* Base scene: all hardware state cleared, BG1/2/3 at the given priorities,
 * BG0 enabled but transparent, camera/scroll at the origin, blend neutral.
 * The BG snapshot is hand-built with the SAME data that is then published to
 * the live registers/VRAM the oracle reads. */
static void CompositeSceneReset(u8 bg1Prio, u8 bg2Prio, u8 bg3Prio)
{
    memset(VRAM_, 0, VRAM_SIZE);
    memset(PLTT, 0, PLTT_SIZE);
    memset(OAM, 0, OAM_SIZE);
    memset(&sBgSnap, 0, sizeof(sBgSnap));

    FillBgCharMemory();
    FillBgPalette();

    memcpy(sBgSnap.bgVram, VRAM_, 0x8000); /* tiles 0..1023 char data */
    memcpy(sBgSnap.palette, PLTT, PLTT_SIZE);

    REG_BG0CNT = BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0)
               | BGCNT_SCREENBASE(SCREENBASE_BG0) | BGCNT_16COLOR
               | BGCNT_TXT256x256;
    REG_BG1CNT = BGCNT_PRIORITY(bg1Prio) | BGCNT_CHARBASE(0)
               | BGCNT_SCREENBASE(SCREENBASE_BG1) | BGCNT_16COLOR
               | BGCNT_TXT256x256;
    REG_BG2CNT = BGCNT_PRIORITY(bg2Prio) | BGCNT_CHARBASE(0)
               | BGCNT_SCREENBASE(SCREENBASE_BG2) | BGCNT_16COLOR
               | BGCNT_TXT256x256;
    REG_BG3CNT = BGCNT_PRIORITY(bg3Prio) | BGCNT_CHARBASE(0)
               | BGCNT_SCREENBASE(SCREENBASE_BG3) | BGCNT_16COLOR
               | BGCNT_TXT256x256;
    sBgSnap.bg0Cnt = REG_BG0CNT;
    sBgSnap.bgCnt[0] = REG_BG1CNT;
    sBgSnap.bgCnt[1] = REG_BG2CNT;
    sBgSnap.bgCnt[2] = REG_BG3CNT;

    REG_BG0HOFS = 0; REG_BG0VOFS = 0;
    REG_BG1HOFS = 0; REG_BG1VOFS = 0;
    REG_BG2HOFS = 0; REG_BG2VOFS = 0;
    REG_BG3HOFS = 0; REG_BG3VOFS = 0;
    sBgSnap.bgHofs[0] = 0; sBgSnap.bgVofs[0] = 0;
    sBgSnap.bgHofs[1] = 0; sBgSnap.bgVofs[1] = 0;
    sBgSnap.bgHofs[2] = 0; sBgSnap.bgVofs[2] = 0;

    REG_DISPCNT = DISPCNT_MODE_0 | DISPCNT_BG0_ON | DISPCNT_BG1_ON
                | DISPCNT_BG2_ON | DISPCNT_BG3_ON | DISPCNT_OBJ_1D_MAP
                | DISPCNT_OBJ_ON;
    sBgSnap.dispCnt = REG_DISPCNT;
    REG_MOSAIC = 0;
    REG_BLDCNT = 0; REG_BLDALPHA = 0; REG_BLDY = 0;
    sBgSnap.bldCnt = 0; sBgSnap.bldAlpha = 0; sBgSnap.bldY = 0;
    sBlend.bldCnt = 0; sBlend.bldAlpha = 0; sBlend.bldY = 0;
    sBlend.dispCnt = REG_DISPCNT;
    sBlend.backdropColor = sBgSnap.palette[0];
    REG_DISPSTAT = 0;
    REG_WIN0H = 0; REG_WIN0V = 0; REG_WIN1H = 0; REG_WIN1V = 0;
    REG_WININ = 0; REG_WINOUT = 0;

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
}

/* Publish the hand-built BG ring into the VRAM screen-base blocks the BGCNT
 * registers point at, so the real DrawFrame reads the same tilemaps. */
static void PublishBgTilemaps(void)
{
    memcpy(VRAM_ + 0x800 * SCREENBASE_BG1, sBgSnap.bgRing[0],
           NATIVE_BG_RING_SIZE * sizeof(u16));
    memcpy(VRAM_ + 0x800 * SCREENBASE_BG2, sBgSnap.bgRing[1],
           NATIVE_BG_RING_SIZE * sizeof(u16));
    memcpy(VRAM_ + 0x800 * SCREENBASE_BG3, sBgSnap.bgRing[2],
           NATIVE_BG_RING_SIZE * sizeof(u16));
}

/* Publish the Stage 3B OBJ snapshot into the live OBJ state. */
static void PublishObjState(void)
{
    memcpy(VRAM_ + 0x10000, sSnap.objVram, OBJ_VRAM0_SIZE);
    memcpy(PLTT + 0x200, sSnap.objPalette,
           OBJ_PALETTE_ENTRY_COUNT * sizeof(u16));
    memcpy(OAM, sSnap.finalOam, OAM_ENTRY_COUNT * sizeof(struct OamData));
    REG_BLDCNT = sSnap.bldCnt;
}

/* Stage 3E: apply a frame-wide blend config to the BG snapshot (native
 * composite source), the OBJ snapshot (published back into REG_BLDCNT by
 * PublishObjState), the trace-side mirror, AND the live registers the oracle
 * reads. All must agree for native and oracle to blend identically. */
static void SetBlendConfig(u16 bldCnt, u16 bldAlpha)
{
    sBgSnap.bldCnt = bldCnt;
    sBgSnap.bldAlpha = bldAlpha;
    sSnap.bldCnt = bldCnt;
    sSnap.bldAlpha = bldAlpha;
    sBlend.bldCnt = bldCnt;
    sBlend.bldAlpha = bldAlpha;
    REG_BLDCNT = bldCnt;
    REG_BLDALPHA = bldAlpha;
}

/* The BG0-content gate samples the SNAPSHOT's bgVram at BG0's screenbase. To
 * make BG0 render visible content, poke an opaque-tile entry into that region
 * in both the snapshot and the live VRAM. */
static void MakeBg0Opaque(void)
{
    sBgSnap.bgVram[SCREENBASE_BG0 * 0x400] = 1; /* tile 1, opaque */
    ((u16 *)VRAM_)[SCREENBASE_BG0 * 0x400] = 1;
}

/* ---- comparison ---- */

static void CompareComposite(const char *name)
{
    struct NativeFieldCompositorReport report;
    u32 mismatches = 0;
    u32 firstIdx = 0;
    u16 firstN = 0;
    u16 firstG = 0;
    u8 firstNw = 0;
    u8 firstGw = 0;
    u32 i;

    /* The rasterize is a side-effecting call -- invoke it OUTSIDE assert so the
     * comparison still runs even if NDEBUG ever re-disables assert here. */
    {
        bool32 rasterized = NativeObjRender_Rasterize(&sSnap, &sOut);
        assert(rasterized);
    }
    assert(sOut.produced == TRUE);
    assert(sOut.capabilityFlags == 0);

    assert(NativeOverworldRenderer_DrawCompositeFrame(&sBgSnap, &sOut,
                                                      sNativeBg, sNativeBgWinner,
                                                      sNativeBgLayers,
                                                      sNativeFinal, sNativeFinalWinner,
                                                      &report));
    assert(report.reason == NATIVE_COMPOSITE_OK);

    memset(sGbaImage, 0, sizeof(sGbaImage));
    gParityCompositeLayers = sGbaCompositeLayers;
    gParityCompositeProduced = 0;
    DrawFrame(sGbaImage);
    gParityCompositeLayers = NULL;
    assert(gParityCompositeProduced == 1);

    for (i = 0; i < (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT); i++)
    {
        if ((sNativeFinal[i] & 0x7FFF) != (sGbaImage[i] & 0x7FFF)
         || sNativeFinalWinner[i] != sGbaCompositeLayers[i])
        {
            if (mismatches == 0)
            {
                firstIdx = i;
                firstN = sNativeFinal[i];
                firstG = sGbaImage[i];
                firstNw = sNativeFinalWinner[i];
                firstGw = sGbaCompositeLayers[i];
            }
            mismatches++;
        }
    }

    if (mismatches != 0)
    {
        fprintf(stderr,
                "composite oracle MISMATCH %s first=(%u,%u) "
                "native=0x%04X win=%u gba=0x%04X win=%u\n",
                name, firstIdx % DISPLAY_WIDTH, firstIdx / DISPLAY_WIDTH,
                firstN & 0x7FFF, firstNw, firstG & 0x7FFF, firstGw);
        fflush(stderr);
    }
    printf("%-34s mismatches=%u (0 expected)\n", name, mismatches);
    assert(mismatches == 0);
}

static void AssertFallback(const char *name, enum NativeCompositeFallbackReason exp)
{
    struct NativeFieldCompositorReport report;
    bool32 rasterized = NativeObjRender_Rasterize(&sSnap, &sOut);

    assert(rasterized);
    assert(!NativeOverworldRenderer_DrawCompositeFrame(&sBgSnap, &sOut,
                                                       sNativeBg, sNativeBgWinner,
                                                       sNativeBgLayers,
                                                       sNativeFinal, sNativeFinalWinner,
                                                       &report));
    assert(report.reason == exp);
    printf("%-34s fallback=%d ok\n", name, (int)exp);
}

/* ---- scenarios ---- */

static void ScenarioBasicBgOnly(void)
{
    CompositeSceneReset(1, 2, 3);
    /* Overlapping regions: BG1 left, BG2 middle, BG3 right. */
    SetRingRect(0, 0, 0, 14, 19, BgTile(1, 1));
    SetRingRect(1, 8, 0, 22, 19, BgTile(2, 2));
    SetRingRect(2, 16, 0, 29, 19, BgTile(3, 3));
    PublishBgTilemaps();
    SnapReset();
    PublishObjState();
    CompareComposite("composite: BG only, no OBJ");
}

static void ScenarioObjOverBackdrop(void)
{
    CompositeSceneReset(1, 2, 3);
    /* All BG transparent. */
    PublishBgTilemaps();
    SnapReset();
    SetOam(3, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 3, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(3, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: OBJ over backdrop");
}

static void ScenarioObjOverBg(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetOam(4, 5, 5, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(4, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: OBJ prio0 over BG1 prio1");
}

static void ScenarioObjTransparentOverBg(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    /* 16x16 over BG1; zero the top-left texel (screen 5,5) so the BG shows
     * through there while the rest of the sprite covers the BG. */
    ZeroObjPixel(4, 0, 0);
    SetOam(4, 5, 5, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(4, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: OBJ transparent texel over BG");
}

static void ScenarioReorderedPriorities(void)
{
    CompositeSceneReset(3, 1, 2);
    SetRingRect(0, 0, 0, 14, 19, BgTile(1, 1));
    SetRingRect(1, 8, 0, 22, 19, BgTile(2, 2));
    SetRingRect(2, 16, 0, 29, 19, BgTile(3, 3));
    PublishBgTilemaps();
    SnapReset();
    PublishObjState();
    /* BG3 (prio 2) now beats BG1 (prio 3) on its overlap; BG2 (prio 1) is
     * foremost where it is opaque. The winner map must match the oracle. */
    CompareComposite("composite: reordered BG priorities");
}

static void ScenarioEqualBgPriorityTie(void)
{
    CompositeSceneReset(2, 2, 2);
    /* All three opaque everywhere at equal priority: BG1 (lowest bgnum) wins
     * every pixel. */
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    SetRingRect(1, 0, 0, 29, 19, BgTile(2, 2));
    SetRingRect(2, 0, 0, 29, 19, BgTile(3, 3));
    PublishBgTilemaps();
    SnapReset();
    PublishObjState();
    CompareComposite("composite: equal BG priority -> BG1");
}

static void ScenarioObjVsBgPriorities(void)
{
    CompositeSceneReset(0, 1, 2);
    /* Reordered priorities; BG regions only rows 0-9 so rows 10-19 are
     * backdrop. */
    SetRingRect(0, 0, 0, 9, 9, BgTile(1, 1));   /* BG1 prio 0 */
    SetRingRect(1, 10, 0, 19, 9, BgTile(2, 2)); /* BG2 prio 1 */
    SetRingRect(2, 20, 0, 29, 9, BgTile(3, 3)); /* BG3 prio 2 */
    PublishBgTilemaps();
    SnapReset();
    /* OBJ0 prio 0 over BG1 prio 0: equal -> OBJ wins. */
    SetOam(0, 2, 2, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    /* OBJ2 prio 2 over BG2 prio 1: BG wins. */
    SetOam(1, 12, 2, ST_OAM_SQUARE, ST_OAM_SIZE_0, 5, 5, 2, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    /* OBJ3 prio 3 over BG3 prio 2: BG wins. */
    SetOam(2, 22, 2, ST_OAM_SQUARE, ST_OAM_SIZE_0, 6, 6, 3, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(2, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    /* OBJ1 prio 1 and OBJ3 prio 3 over backdrop (rows 10-19): OBJ wins. */
    SetOam(3, 2, 90, ST_OAM_SQUARE, ST_OAM_SIZE_0, 7, 7, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(3, NATIVE_SPRITE_SOURCE_GSPRITE, 3, 0);
    SetOam(4, 12, 90, ST_OAM_SQUARE, ST_OAM_SIZE_0, 8, 8, 3, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(4, NATIVE_SPRITE_SOURCE_GSPRITE, 4, 0);
    PublishObjState();
    CompareComposite("composite: OBJ0-3 vs BG0-3 prios");
}

static void ScenarioTwoObjSamePriorityOverBg(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    /* Two 8x8 OBJ at the same priority overlapping the BG and each other:
     * lowest OAM index wins the overlap, both beat the BG. */
    SetOam(20, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(20, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    SetOam(10, 44, 40, ST_OAM_SQUARE, ST_OAM_SIZE_0, 5, 5, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(10, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    PublishObjState();
    CompareComposite("composite: 2 same-prio OBJ + transparent + BG");
}

static void ScenarioSpriteGeometry(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    SetRingRect(1, 0, 0, 29, 19, BgTile(2, 2));
    PublishBgTilemaps();
    SnapReset();
    SetOam(1, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    SetOam(2, 40, 10, ST_OAM_SQUARE, ST_OAM_SIZE_2, 5, 5, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, true, true);
    AddCommand(2, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    SetOam(3, 90, 10, ST_OAM_H_RECTANGLE, ST_OAM_SIZE_1, 6, 6, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(3, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    SetOam(4, 130, 10, ST_OAM_V_RECTANGLE, ST_OAM_SIZE_1, 7, 7, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(4, NATIVE_SPRITE_SOURCE_GSPRITE, 3, 0);
    /* 64x64 multi-tile at prio 2, partially off the right/bottom edge. */
    SetOam(5, 190, 110, ST_OAM_SQUARE, ST_OAM_SIZE_3, 8, 8, 2, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 4, 0);
    /* Clipped at the left edge. */
    SetOam(6, -4, 100, ST_OAM_SQUARE, ST_OAM_SIZE_1, 9, 9, 3, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(6, NATIVE_SPRITE_SOURCE_GSPRITE, 5, 0);
    PublishObjState();
    CompareComposite("composite: sprite geometry + clip");
}

static void ScenarioMovingFrame(void)
{
    CompositeSceneReset(1, 2, 3);
    /* Whole ring filled per layer with a different opaque tile; the scroll
     * registers are set to (5,3) while the camera is at the origin, so both
     * the native (ring at presentation scroll) and the oracle (live registers)
     * sample the identical shifted, sub-tile-rolled tilemap. */
    SetRingRect(0, 0, 0, 31, 31, BgTile(1, 1));
    SetRingRect(1, 0, 0, 31, 31, BgTile(2, 2));
    SetRingRect(2, 0, 0, 31, 31, BgTile(3, 3));
    REG_BG1HOFS = 5; REG_BG1VOFS = 3;
    REG_BG2HOFS = 5; REG_BG2VOFS = 3;
    REG_BG3HOFS = 5; REG_BG3VOFS = 3;
    sBgSnap.bgHofs[0] = 5; sBgSnap.bgVofs[0] = 3;
    sBgSnap.bgHofs[1] = 5; sBgSnap.bgVofs[1] = 3;
    sBgSnap.bgHofs[2] = 5; sBgSnap.bgVofs[2] = 3;
    PublishBgTilemaps();
    SnapReset();
    SetOam(0, 60, 60, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: moving-frame scroll");
}

static void ScenarioBg0PresentTransparent(void)
{
    /* BG0 enabled with an all-transparent tilemap: the oracle draws nothing
     * for it, the BG0 gate passes, and the composite is unaffected. */
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetOam(0, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: BG0 enabled transparent");
}

static void ScenarioBlendNeutral0x1E40(void)
{
    /* The persistent field blend config (alpha, TGT1 empty) must remain
     * composite-neutral for both BG and OBJ. */
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    SetRingRect(1, 0, 0, 29, 19, BgTile(2, 2));
    PublishBgTilemaps();
    SnapReset();
    sSnap.bldCnt = 0x1E40;
    sBgSnap.bldCnt = 0x1E40;
    REG_BLDCNT = 0x1E40;
    SetOam(0, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(0, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: field blend 0x1E40 neutral");
}

static void ScenarioTraceCheck(void)
{
    struct NativeFieldCompositeTrace t;
    u32 idx;

    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetOam(9, 20, 20, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(9, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    {
        struct NativeFieldCompositorReport report;
        bool32 rasterized = NativeObjRender_Rasterize(&sSnap, &sOut);

        assert(rasterized);
        assert(NativeOverworldRenderer_DrawCompositeFrame(&sBgSnap, &sOut,
                                                          sNativeBg, sNativeBgWinner,
                                                          sNativeBgLayers,
                                                          sNativeFinal, sNativeFinalWinner,
                                                          &report));
        assert(report.reason == NATIVE_COMPOSITE_OK);
    }
    /* OBJ pixel: winner + color must agree with the composite output. */
    assert(NativeFieldCompositor_Trace(sNativeBg, sNativeBgWinner, sTraceBgPriority,
                                       sNativeBgLayers, &sSnap, &sOut, &sBlend,
                                       20, 20, &t));
    idx = 20 * DISPLAY_WIDTH + 20;
    assert(t.objPresent == TRUE);
    assert(t.finalWinner == sNativeFinalWinner[idx]);
    assert(t.finalColor == sNativeFinal[idx]);
    assert(t.objTrace.oamIndex == 9);
    /* BG pixel well away from the sprite. */
    assert(NativeFieldCompositor_Trace(sNativeBg, sNativeBgWinner, sTraceBgPriority,
                                       sNativeBgLayers, &sSnap, &sOut, &sBlend,
                                       200, 100, &t));
    assert(t.objPresent == FALSE);
    assert(t.bgWinner == sNativeBgWinner[100 * DISPLAY_WIDTH + 200]);
    printf("%-34s ok\n", "composite: trace attributes winner");
}

static void ScenarioFallbacks(void)
{
    /* Stage 3E: semi-transparent OBJ is now composited (the dedicated blend
     * scenarios below prove parity); what still falls back is a blend config
     * that would recolor normal OBJ/BG pixels, BG0 overlay content, and an
     * unsupported BG snapshot. */

    /* A blend config that would recolor pixels -> BLEND_EFFECT. */
    CompositeSceneReset(1, 2, 3);
    PublishBgTilemaps();
    SnapReset();
    sBgSnap.bldCnt = BLDCNT_EFFECT_BLEND | BLDCNT_TGT1_BG0 | BLDCNT_TGT2_BG1;
    REG_BLDCNT = sBgSnap.bldCnt;
    PublishObjState();
    AssertFallback("fallback: blend effect", NATIVE_COMPOSITE_FALLBACK_BLEND_EFFECT);

    /* BG0 with opaque content -> BG0_OVERLAY. */
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    MakeBg0Opaque();
    PublishBgTilemaps();
    SnapReset();
    PublishObjState();
    AssertFallback("fallback: BG0 overlay content", NATIVE_COMPOSITE_FALLBACK_BG0_OVERLAY);

    /* Unsupported BG snapshot -> BG_UNSUPPORTED. */
    CompositeSceneReset(1, 2, 3);
    sBgSnap.fallbackReason = NATIVE_FALLBACK_SCENE_NOT_OVERWORLD;
    PublishBgTilemaps();
    SnapReset();
    PublishObjState();
    AssertFallback("fallback: BG snapshot unsupported", NATIVE_COMPOSITE_FALLBACK_BG_UNSUPPORTED);
}

/* ---- Stage 3E §12: semi-transparent OBJ blend (scenarios A-O) ----
 *
 * The oracle's rule (gba_easy_draw.c DrawSprites): a semi-transparent OBJ
 * ALWAYS blends when drawn (`|| isSemiTransparent`), independent of the blend
 * effect / TGT1 / windows. targetB is chosen by alphaBlendSelectTargetB with
 * prnum=oam->priority, prsub=0, spriteBlendEnabled=false: BG layers only, from
 * the OBJ's priority toward the background, ascending bgnum at equal priority,
 * bailing (no blend) on the first opaque enabled non-TGT2 BG strictly above
 * the OBJ's priority, and the backdrop when BLDCNT_TGT2_BD is set. EVA/EVB
 * come from BLDALPHA: ((A*eva)+(B*evb)) >> 4, each channel clamped to 31.
 * Every scenario compares the native composite (color AND winner) against the
 * real DrawFrame -- the oracle is the authority for the math.
 */

/* A: semi OBJ over an opaque TGT2 BG1 below it. */
static void ScenarioBlendA_SemiOverBg1(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend A: semi over BG1");
}

/* B: semi OBJ over BG2 -- BG1 transparent, opaque TGT2 BG2 at prio 2. */
static void ScenarioBlendB_SemiOverBg2(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(1, 0, 0, 29, 19, BgTile(2, 2));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend B: semi over BG2");
}

/* C: semi OBJ over BG3 -- opaque TGT2 BG3 at prio 3. */
static void ScenarioBlendC_SemiOverBg3(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(2, 0, 0, 29, 19, BgTile(3, 3));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend C: semi over BG3");
}

/* D: semi OBJ over the backdrop -- no opaque BG, TGT2_BD set -> targetB is
 * the backdrop color (PLTT[0]). */
static void ScenarioBlendD_SemiOverBackdrop(void)
{
    CompositeSceneReset(1, 2, 3);
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend D: semi over backdrop");
}

/* E: semi OBJ in front of a NORMAL OBJ at lower priority. The lower OBJ is
 * NOT a blend targetB (spriteBlendEnabled=false in the semi path): targetB is
 * the backdrop, and the front semi OBJ wins the overlap. */
static void ScenarioBlendE_SemiOverLowerObj(void)
{
    CompositeSceneReset(1, 2, 3);
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    /* Back normal OBJ prio 1 at (40,40). */
    SetOam(2, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 5, 5, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(2, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    /* Front semi OBJ prio 0 overlapping it at (44,40). */
    SetOam(5, 44, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend E: semi over normal OBJ");
}

/* F: no TGT2 target at all -- the walk finds no target and the semi OBJ writes
 * its raw color unchanged (semi does NOT blend without a targetB). */
static void ScenarioBlendF_Tgt2Disabled(void)
{
    CompositeSceneReset(1, 2, 3);
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND, BLDALPHA_BLEND(13, 7));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend F: no TGT2 -> raw color");
}

/* F2: an opaque enabled non-TGT2 BG strictly above the OBJ's priority makes
 * the walk bail (no blend) even with TGT2_BD set. */
static void ScenarioBlendF2_BailNonTgt2(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1)); /* BG1 prio 1, NOT TGT2 */
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend F2: bail on non-TGT2 BG");
}

/* G1: EVA=16/EVB=0 reproduces targetA exactly -- semi OBJ shows its raw color
 * even with a valid targetB below. */
static void ScenarioBlendG1_Eva16Evb0(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(16, 0));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend G1: EVA=16/EVB=0 -> targetA");
}

/* G2: EVA=0/EVB=16 reproduces targetB exactly -- semi OBJ over BG1 becomes the
 * BG1 pixel (the weather fog behavior). */
static void ScenarioBlendG2_Eva0Evb16(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(0, 16));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend G2: EVA=0/EVB=16 -> targetB");
}

/* G3: a second EVA/EVB pair, (5,10), exercising the weighted path. */
static void ScenarioBlendG3_Weighted(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(5, 10));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend G3: EVA=5/EVB=10 weighted");
}

/* H: saturation -- EVA=EVB=16 over identical bright A/B pixels: each channel's
 * 31*16+31*16 >> 4 = 62 is clamped to 31. Bright BGR555 entries are set
 * explicitly so the clamp (not just a pass-through) is exercised. */
static void ScenarioBlendH_Saturation(void)
{
    int p;

    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    /* Tile 1 pal 1 (BG1) and tile 4 pal 4 (OBJ) both use pixels 1..15. */
    for (p = 1; p < 16; p++)
    {
        sSnap.objPalette[4 * 16 + p] = (u16)0x2A14; /* r20 g15 b10 */
        sBgSnap.palette[1 * 16 + p] = (u16)0x2A14;
        ((u16 *)PLTT)[1 * 16 + p] = (u16)0x2A14;
    }
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(16, 16));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend H: saturation clamp");
}

/* I: a transparent texel inside a semi OBJ is not drawn -- the underlying BG
 * shows through there while the opaque texels still blend. */
static void ScenarioBlendI_TransparentTexel(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    ZeroObjPixel(4, 0, 0); /* tile 4 top-left texel */
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend I: semi transparent texel");
}

/* J: affine semi OBJ (identity matrix, 16x16) -- affine sampling is unchanged
 * and the winning pixels blend exactly like a non-affine semi OBJ. */
static void ScenarioBlendJ_AffineSemi(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    SetOam(60, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend J: affine semi OBJ");
}

/* K: semi OBJ clipped at the right/bottom screen edge -- only in-bounds pixels
 * are drawn and blended; the off-screen remainder is simply absent. */
static void ScenarioBlendK_Clipped(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    SetOam(5, 230, 150, ST_OAM_SQUARE, ST_OAM_SIZE_3, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend K: semi clipped at edge");
}

/* L: overlapping NORMAL + SEMI OBJ at equal priority over an opaque BG: lowest
 * OAM index wins the OBJ overlap; the winning semi pixel blends against BG1,
 * the winning normal pixel stays raw. */
static void ScenarioBlendL_OverlapNormalSemi(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    SetOam(4, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 1, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(4, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    SetOam(3, 44, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 5, 5, 1, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(3, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    PublishObjState();
    CompareComposite("blend L: same-prio normal+semi");
}

/* M: priority tie -- semi OBJ and BG1 at EQUAL priority: the OBJ wins the
 * composite (Stage 3C rule), and the equal-priority opaque BG1 below is the
 * blend targetB. */
static void ScenarioBlendM_PriorityTie(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 1, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend M: equal-prio semi vs BG1");
}

/* N: the persistent field config 0x1E40 (no TGT1, TGT2=BG1|BG2|BG3|OBJ, no BD)
 * with a NORMAL OBJ stays composite-neutral, while a SEMI OBJ blends only where
 * an opaque TGT2 BG sits below it (0x1E40 has no TGT2_BD, so over the backdrop
 * the semi OBJ keeps its raw color). */
static void ScenarioBlendN_Persistent1E40(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(0x1E40, BLDALPHA_BLEND(13, 7));
    /* Normal OBJ over the same BG1: unchanged by 0x1E40 (no TGT1). */
    SetOam(1, 90, 90, ST_OAM_SQUARE, ST_OAM_SIZE_1, 6, 6, 3, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(1, NATIVE_SPRITE_SOURCE_GSPRITE, 2, 0);
    /* Semi OBJ prio 0 over BG1: targetB=BG1 -> blended. */
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend N: 0x1E40 normal+semi");
}

/* O: semi OBJ with EVA=0/EVB=31 (the ash coefficients) over BG1 -- the max EVB
 * path, distinct from the (0,16) fog case. */
static void ScenarioBlendO_Evb31(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(0, 31));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("blend O: EVA=0/EVB=31 ash");
}

/* ---- Stage 3E §13: real weather field-effect fixtures ----
 * Modeled on field_weather_effect.c: the weather OBJ are 64x64
 * semi-transparent (objMode=ST_OAM_OBJ_BLEND) with the actual priorities and
 * palettes (Cloud=3/0, FogH=2/0, FogDiagonal=2/0, Ash=1/15, Sandstorm=1/0),
 * under the persistent field blend BLDCNT=0x1E40 (no TGT1, TGT2=BG1|BG2|BG3|
 * OBJ) and the weather BLDALPHA: (0,16) init for fog/sandstorm/cloud, and the
 * invalid-but-actual ash override BLDALPHA_BLEND(64,63) -> EVA=0, EVB=31.
 * With no TGT2_BD the weather OBJ blends only where a TGT2 BG is opaque below;
 * over the backdrop it keeps its raw color. The synthetic map BG1 is put at
 * prio 3 so each weather priority wins the composite. */

static void ScenarioWeatherFogH(void)
{
    CompositeSceneReset(3, 3, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(0x1E40, BLDALPHA_BLEND(0, 16));
    SetOam(30, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_3, 0, 0, 2, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(30, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("weather: fog-horizontal semi");
}

static void ScenarioWeatherFogDiagonal(void)
{
    CompositeSceneReset(3, 3, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(0x1E40, BLDALPHA_BLEND(0, 16));
    SetOam(31, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_3, 0, 0, 2, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(31, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("weather: fog-diagonal semi");
}

static void ScenarioWeatherSandstorm(void)
{
    CompositeSceneReset(3, 3, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(0x1E40, BLDALPHA_BLEND(0, 16));
    SetOam(32, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_3, 0, 0, 1, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(32, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("weather: sandstorm semi");
}

static void ScenarioWeatherAsh(void)
{
    CompositeSceneReset(3, 3, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(0x1E40, BLDALPHA_BLEND(64, 63)); /* EVA=0, EVB=31 */
    SetOam(33, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_3, 0, 15, 1, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(33, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("weather: ash semi pal15");
}

static void ScenarioWeatherCloud(void)
{
    /* No opaque BG at all: with 0x1E40 (no TGT2_BD) the semi cloud has no
     * targetB and keeps its raw color over the backdrop. */
    CompositeSceneReset(3, 3, 3);
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(0x1E40, BLDALPHA_BLEND(0, 16));
    SetOam(34, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_3, 0, 0, 3, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(34, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("weather: cloud over backdrop");
}

/* Stage 3E §10/§12: the blend diagnostics trace -- for a winning semi OBJ
 * pixel, assert every blend field the on-demand dump reports (BLDCNT/BLDALPHA/
 * mode/EVA/EVB/targetB/preBlend/final) against a hand-computed alpha blend. */
static void ScenarioBlendTrace(void)
{
    struct NativeFieldCompositeTrace t;
    struct NativeFieldCompositorReport report;
    u16 targetA, targetB;
    u32 eva, evb, r, g, b;
    u16 expected;

    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    SetBlendConfig(BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD,
                   BLDALPHA_BLEND(13, 7));
    SetOam(5, 30, 30, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_BLEND,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(5, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    {
        bool32 rasterized = NativeObjRender_Rasterize(&sSnap, &sOut);
        assert(rasterized);
        assert(NativeOverworldRenderer_DrawCompositeFrame(&sBgSnap, &sOut,
                                                          sNativeBg, sNativeBgWinner,
                                                          sNativeBgLayers,
                                                          sNativeFinal, sNativeFinalWinner,
                                                          &report));
        assert(report.reason == NATIVE_COMPOSITE_OK);
    }
    /* Sprite center (38,38): OBJ tile 4 pal 4 prio 0, BG1 below. */
    assert(NativeFieldCompositor_Trace(sNativeBg, sNativeBgWinner, sTraceBgPriority,
                                       sNativeBgLayers, &sSnap, &sOut, &sBlend,
                                       38, 38, &t));
    assert(t.objPresent == TRUE);
    assert(t.finalWinner == sNativeFinalWinner[38 * DISPLAY_WIDTH + 38]);
    assert(t.semiObj == 1);
    assert(t.blendApplied == TRUE);
    assert(t.targetBKind == 2); /* BG1 */
    assert(t.bldCnt == (BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BD));
    assert(t.bldAlpha == BLDALPHA_BLEND(13, 7));
    assert(t.blendMode == 1);
    assert(t.eva == 13 && t.evb == 7);
    assert((t.tgt2Bits & (1u << 1)) != 0); /* TGT2_BG1 (bit 9 >> 8) */
    assert((t.bldCnt & BLDCNT_TGT2_BD) != 0); /* TGT2_BD (bit 13, not in tgt2Bits) */

    /* Hand-computed alpha blend: targetA = OBJ texel, targetB = BG1 texel. */
    targetA = sOut.layers[0].color[38 * DISPLAY_WIDTH + 38];
    targetB = sNativeBgLayers[1 * (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT)
                              + 38 * DISPLAY_WIDTH + 38];
    eva = 13; evb = 7;
    r = ((((u32)(targetA >>  0) & 0x1F) * eva) + (((u32)(targetB >>  0) & 0x1F) * evb)) >> 4;
    g = ((((u32)(targetA >>  5) & 0x1F) * eva) + (((u32)(targetB >>  5) & 0x1F) * evb)) >> 4;
    b = ((((u32)(targetA >> 10) & 0x1F) * eva) + (((u32)(targetB >> 10) & 0x1F) * evb)) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;
    expected = (u16)(r | (g << 5) | (b << 10));
    assert((t.preBlendColor & 0x7FFF) == (targetA & 0x7FFF));
    assert((t.targetBColor & 0x7FFF) == (targetB & 0x7FFF));
    assert((t.finalColor & 0x7FFF) == (expected & 0x7FFF));
    assert((sNativeFinal[38 * DISPLAY_WIDTH + 38] & 0x7FFF) == (expected & 0x7FFF));
    printf("%-34s ok\n", "blend trace: semi targetB + math");
}

/* Stage 3D: affine OBJ now composites (final color AND winner must match the
 * oracle). The matrix is loaded into the presented OAM layout (SetAffineMatrix)
 * so both the native sampler and the oracle read the identical state. */

static void ScenarioAffineOverBackdrop(void)
{
    CompositeSceneReset(1, 2, 3);
    PublishBgTilemaps();
    SnapReset();
    SetOam(60, 10, 10, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: affine OBJ over backdrop");
}

static void ScenarioAffineOverBg(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    /* 16x16 identity affine prio 0 over BG1 prio 1: OBJ wins. */
    SetOam(60, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: affine prio0 over BG prio1");
}

static void ScenarioAffineBehindBg(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    /* Affine 16x16 identity prio 2 over BG1 prio 1: BG wins. */
    SetOam(60, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 2, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: affine prio2 behind BG prio1");
}

static void ScenarioAffineEqualPriorityTie(void)
{
    CompositeSceneReset(0, 1, 2);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1)); /* BG1 prio 0 */
    PublishBgTilemaps();
    SnapReset();
    /* Affine prio 0 vs BG1 prio 0: equal -> OBJ wins (OBJ beats BG at a tie). */
    SetOam(60, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: affine OBJ ties BG -> OBJ");
}

static void ScenarioAffineOverlapNormalObj(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    /* Normal 8x8 (OAM 40) + affine 8x8 (OAM 60, identity) at equal prio; the
     * lower OAM index wins the overlap, both beat the BG. */
    SetOam(40, 50, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_OFF, 0, ST_OAM_4BPP, false, false);
    AddCommand(40, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    SetOam(60, 54, 50, ST_OAM_SQUARE, ST_OAM_SIZE_0, 5, 5, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 1, 0);
    PublishObjState();
    CompareComposite("composite: affine overlap normal OBJ");
}

static void ScenarioAffineTransparentOverBg(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    /* 16x16 affine identity over BG1; zero texel (0,0) (screen 40,40) so the
     * BG shows through there, the rest covers it. */
    ZeroObjPixel(4, 0, 0);
    SetOam(60, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 3);
    SetAffineMatrix(3, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: affine transparent texel over BG");
}

static void ScenarioAffineDoubleSizeOverBg(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    /* Double-size affine 16x16 identity at prio 0: the texture covers only the
     * center of the doubled box, so BG shows through the corners. */
    SetOam(60, 40, 40, ST_OAM_SQUARE, ST_OAM_SIZE_1, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_DOUBLE, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 9);
    SetAffineMatrix(9, 0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: affine double-size over BG");
}

static void ScenarioAffineMirrorOverBg(void)
{
    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 29, 19, BgTile(1, 1));
    PublishBgTilemaps();
    SnapReset();
    /* H-mirror affine 8x8 at prio 0 over BG1 prio 1. */
    SetOam(60, 100, 100, ST_OAM_SQUARE, ST_OAM_SIZE_0, 4, 4, 0, ST_OAM_OBJ_NORMAL,
           ST_OAM_AFFINE_NORMAL, 0, ST_OAM_4BPP, false, false);
    SetOamMatrixNum(60, 4);
    SetAffineMatrix(4, -0x100, 0, 0, 0x100);
    AddCommand(60, NATIVE_SPRITE_SOURCE_GSPRITE, 0, 0);
    PublishObjState();
    CompareComposite("composite: affine H-mirror over BG");
}

static void ScenarioDrawMapFrameWithMetaRegression(void)
{
    static u16 fixedFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u32 i;

    CompositeSceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 14, 19, BgTile(1, 1));
    SetRingRect(1, 8, 0, 22, 19, BgTile(2, 2));
    SetRingRect(2, 16, 0, 29, 19, BgTile(3, 3));
    PublishBgTilemaps();
    assert(NativeOverworldRenderer_DrawMapFrame(&sBgSnap, fixedFrame));
    assert(NativeOverworldRenderer_DrawMapFrameWithMeta(&sBgSnap, sNativeBg,
                                                        sNativeBgWinner,
                                                        sNativeBgLayers));
    for (i = 0; i < (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT); i++)
        assert(sNativeBg[i] == fixedFrame[i]);
    /* Stage 3E: the per-layer buffer must carry the winning BG's layer color at
     * every opaque pixel, and be zero at backdrop pixels. */
    for (i = 0; i < (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT); i++)
    {
        if (sNativeBgWinner[i] >= NATIVE_COMPOSITE_WIN_BG1
         && sNativeBgWinner[i] <= NATIVE_COMPOSITE_WIN_BG3)
        {
            /* Winner encoding 2/3/4 = BG1/2/3 -> layer slot winner-1 = 1/2/3. */
            u32 slot = (u32)sNativeBgWinner[i] - 1;
            assert(sNativeBgLayers[slot * (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT) + i]
                   == sNativeBg[i]);
        }
        else
        {
            /* Backdrop: no layer drew, every slot stays transparent. */
            assert(sNativeBgLayers[1 * (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT) + i] == 0
                && sNativeBgLayers[2 * (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT) + i] == 0
                && sNativeBgLayers[3 * (u32)(DISPLAY_WIDTH * DISPLAY_HEIGHT) + i] == 0);
        }
    }
    printf("%-34s ok\n", "regression: WithMeta == DrawMapFrame (1/2/3)");
}

int main(void)
{
    ScenarioBasicBgOnly();
    ScenarioObjOverBackdrop();
    ScenarioObjOverBg();
    ScenarioObjTransparentOverBg();
    ScenarioReorderedPriorities();
    ScenarioEqualBgPriorityTie();
    ScenarioObjVsBgPriorities();
    ScenarioTwoObjSamePriorityOverBg();
    ScenarioSpriteGeometry();
    ScenarioMovingFrame();
    ScenarioBg0PresentTransparent();
    ScenarioBlendNeutral0x1E40();
    ScenarioTraceCheck();
    ScenarioFallbacks();
    ScenarioBlendA_SemiOverBg1();
    ScenarioBlendB_SemiOverBg2();
    ScenarioBlendC_SemiOverBg3();
    ScenarioBlendD_SemiOverBackdrop();
    ScenarioBlendE_SemiOverLowerObj();
    ScenarioBlendF_Tgt2Disabled();
    ScenarioBlendF2_BailNonTgt2();
    ScenarioBlendG1_Eva16Evb0();
    ScenarioBlendG2_Eva0Evb16();
    ScenarioBlendG3_Weighted();
    ScenarioBlendH_Saturation();
    ScenarioBlendI_TransparentTexel();
    ScenarioBlendJ_AffineSemi();
    ScenarioBlendK_Clipped();
    ScenarioBlendL_OverlapNormalSemi();
    ScenarioBlendM_PriorityTie();
    ScenarioBlendN_Persistent1E40();
    ScenarioBlendO_Evb31();
    ScenarioWeatherFogH();
    ScenarioWeatherFogDiagonal();
    ScenarioWeatherSandstorm();
    ScenarioWeatherAsh();
    ScenarioWeatherCloud();
    ScenarioBlendTrace();
    ScenarioAffineOverBackdrop();
    ScenarioAffineOverBg();
    ScenarioAffineBehindBg();
    ScenarioAffineEqualPriorityTie();
    ScenarioAffineOverlapNormalObj();
    ScenarioAffineTransparentOverBg();
    ScenarioAffineDoubleSizeOverBg();
    ScenarioAffineMirrorOverBg();
    ScenarioDrawMapFrameWithMetaRegression();
    fprintf(stderr, "field compositor real-oracle harness passed\n");
    return 0;
}

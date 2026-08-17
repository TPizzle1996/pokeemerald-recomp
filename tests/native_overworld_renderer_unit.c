/*
 * Native overworld renderer unit test (Stage 4A).
 *
 * Covers the pure coordinate helpers (FloorDivide / PositiveModulo), the
 * continuous NativeViewport module (scale derivation, env init, continuous
 * zoom steps with clamping, Active at exactly 240x160, TopLeft odd rounding),
 * and the Stage 4A generalized map renderer DrawMapFrameWithMetaEx:
 *
 *   - byte-identity with DrawMapFrame for the 240x160 path (same ring sampling,
 *     same per-pixel BG-winner metadata, same flat layer colors),
 *   - priority-aware winner selection from the CAPTURED BGCNT priorities
 *     (reordering BG1/BG2 changes the winner, unlike the fixed field order),
 *   - the expanded-viewport center crop equals the 240x160 output at a shifted
 *     viewport origin (true reveal, not distortion), while the margin apron
 *     resolves the snapshot border fallback (world the 240x160 view never sees),
 *   - NULL / unsupported-snapshot rejection.
 *
 * The old Stage-1 experimental object renderer (NativeOverworldRenderer_Draw,
 * RenderMapPixel, DrawObjectEvents, ...) was DELETED in Stage 4A: the OBJ path
 * is now entirely the Stage 3A/3B command snapshot + RasterizeEx, covered by
 * native_obj_renderer_unit.c and native_overworld_expanded_parity_unit.c.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"

/* config.h (pulled in by global.h via the included .c files above) defines
 * NDEBUG unconditionally; re-include <assert.h> last so the assertions below
 * are actually enforced. */
#undef NDEBUG
#include <assert.h>

/* ---- Stubs of hardware globals the renderer TU references ---- */
unsigned char PLTT[PLTT_SIZE] __attribute__((aligned(4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));

/* ---- Stubs of game globals ---- */
struct MapHeader gMapHeader;
static struct SaveBlock1 sSaveBlock1;
struct SaveBlock1 *gSaveBlock1Ptr = &sSaveBlock1;

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

/* ---- Snapshot scaffold (hand-built, like the expanded-parity harness) ---- */

static u16 sPrimaryMetatiles[NUM_METATILES_IN_PRIMARY * NUM_TILES_PER_METATILE];
static u16 sPrimaryAttributes[NUM_METATILES_IN_PRIMARY];
static u16 sBorderMetatiles[4];

static struct NativeOverworldSnapshot sSnap;

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
            sSnap.bgRing[bg][ry * 32 + rx] = entry;
    }
}

/* Deterministic BG char memory: tiles 1..959 opaque with varied texels, tiles
 * 0 and 960..1023 transparent. Same model as the Stage 3C oracle harness. */
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

static u16 ExpectedColor(int bank, int pixel)
{
    return (u16)(0x0100 + bank * 16 + pixel);
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

static void SceneReset(u8 bg1Prio, u8 bg2Prio, u8 bg3Prio)
{
    memset(VRAM_, 0, VRAM_SIZE);
    memset(PLTT, 0, PLTT_SIZE);
    memset(&sSnap, 0, sizeof(sSnap));
    memset(sPrimaryMetatiles, 0, sizeof(sPrimaryMetatiles));
    memset(sPrimaryAttributes, 0, sizeof(sPrimaryAttributes));
    memset(sBorderMetatiles, 0, sizeof(sBorderMetatiles));

    FillBgCharMemory();
    FillBgPalette();
    memcpy(sSnap.bgVram, VRAM_, 0x8000); /* tiles 0..1023 char data */
    memcpy(sSnap.palette, PLTT, PLTT_SIZE);

    sSnap.bg0Cnt = BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0) | BGCNT_16COLOR
                   | BGCNT_TXT256x256;
    sSnap.bgCnt[0] = BGCNT_PRIORITY(bg1Prio) | BGCNT_CHARBASE(0) | BGCNT_16COLOR
                     | BGCNT_TXT256x256;
    sSnap.bgCnt[1] = BGCNT_PRIORITY(bg2Prio) | BGCNT_CHARBASE(0) | BGCNT_16COLOR
                     | BGCNT_TXT256x256;
    sSnap.bgCnt[2] = BGCNT_PRIORITY(bg3Prio) | BGCNT_CHARBASE(0) | BGCNT_16COLOR
                     | BGCNT_TXT256x256;

    sSnap.bgHofs[0] = 0; sSnap.bgVofs[0] = 0;
    sSnap.bgHofs[1] = 0; sSnap.bgVofs[1] = 0;
    sSnap.bgHofs[2] = 0; sSnap.bgVofs[2] = 0;

    sSaveBlock1.pos.x = 0; sSaveBlock1.pos.y = 0;
    sCameraX = 0; sCameraY = 0;
    sSnap.cameraMapX = 0; sSnap.cameraMapY = 0;
    sSnap.cameraX = 0; sSnap.cameraY = 0;

    sSnap.requiredCapabilities = NATIVE_CAPABILITY_MAP_BACKGROUND
                               | NATIVE_CAPABILITY_LIVE_BG_RING
                               | NATIVE_CAPABILITY_BG_VRAM
                               | NATIVE_CAPABILITY_BG_PALETTE;
    sSnap.fallbackReason = NATIVE_FALLBACK_NONE;
    sSnap.bgRingValid = TRUE;
    sSnap.bgVramValid = TRUE;

    /* Snapshot border/metatile fallback (used outside the ring gate). */
    sSnap.primaryMetatiles = sPrimaryMetatiles;
    sSnap.primaryMetatileAttributes = sPrimaryAttributes;
    sSnap.border = sBorderMetatiles;
    sBorderMetatiles[0] = 1;
    sBorderMetatiles[1] = 2;
    sBorderMetatiles[2] = 3;
    sBorderMetatiles[3] = 4;
}

/* ---- Pure coordinate math (kept from Stage 0/1) ---- */

static void TestCoordinateMath(void)
{
    assert(FloorDivide(31, 16) == 1);
    assert(FloorDivide(0, 16) == 0);
    assert(FloorDivide(-1, 16) == -1);
    assert(FloorDivide(-16, 16) == -1);
    assert(FloorDivide(-17, 16) == -2);
    assert(PositiveModulo(17, 16) == 1);
    assert(PositiveModulo(-1, 16) == 15);
    assert(PositiveModulo(-17, 16) == 15);
}

/* ---- Continuous viewport module ---- */

static void TestViewportScaling(void)
{
    struct NativeViewport viewport;

    /* Default viewport is exactly the base 240x160 view (1.0x, not Active). */
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);
    assert(viewport.width == NATIVE_VIEWPORT_MIN_WIDTH);
    assert(viewport.height == NATIVE_VIEWPORT_MIN_HEIGHT);
    assert(!NativeOverworldViewport_Active(&viewport));

    /* Continuous scale math (no preset enums): 1.25x -> 300x200,
     * 1.5x -> 360x240. */
    viewport.scaleQ8 = 320;
    NativeOverworldViewport_Recompute(&viewport);
    assert(viewport.width == 300 && viewport.height == 200);
    assert(NativeOverworldViewport_Active(&viewport));

    viewport.scaleQ8 = NATIVE_VIEWPORT_SCALE_MAX;
    NativeOverworldViewport_Recompute(&viewport);
    assert(viewport.width == NATIVE_VIEWPORT_MAX_WIDTH);
    assert(viewport.height == NATIVE_VIEWPORT_MAX_HEIGHT);
    assert(NativeOverworldViewport_Active(&viewport));

    /* ZoomOut (Ctrl+Minus) increases the scale; ZoomIn (Ctrl+Equals) decreases
     * it by the continuous step, clamped at both ends. */
    NativeOverworldViewport_Reset(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);
    assert(viewport.width == NATIVE_VIEWPORT_MIN_WIDTH);
    assert(viewport.height == NATIVE_VIEWPORT_MIN_HEIGHT);
    assert(NativeOverworldViewport_ZoomOut(&viewport));
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0 + NATIVE_VIEWPORT_SCALE_STEP);
    assert(NativeOverworldViewport_ZoomIn(&viewport));
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);
    assert(!NativeOverworldViewport_ZoomIn(&viewport));

    viewport.scaleQ8 = NATIVE_VIEWPORT_SCALE_MAX;
    NativeOverworldViewport_Recompute(&viewport);
    assert(!NativeOverworldViewport_ZoomOut(&viewport));
    assert(viewport.width == NATIVE_VIEWPORT_MAX_WIDTH);
    assert(viewport.height == NATIVE_VIEWPORT_MAX_HEIGHT);
}

static void TestViewportTopLeft(void)
{
    struct NativeViewport viewport;
    s32 left;
    s32 top;

    /* 300x200 viewport centered on the 240x160 focus: +30 L/R, +20 T/B. */
    NativeOverworldViewport_Init(&viewport);
    viewport.scaleQ8 = 320;
    NativeOverworldViewport_Recompute(&viewport);
    NativeOverworldViewport_TopLeft(&viewport, 160, 320, &left, &top);
    assert(viewport.centerX == 160 + DISPLAY_WIDTH / 2);
    assert(viewport.centerY == 320 + DISPLAY_HEIGHT / 2);
    assert(left == 160 + DISPLAY_WIDTH / 2 - 150);
    assert(top == 320 + DISPLAY_HEIGHT / 2 - 100);

    /* 1.0x viewport collapses onto the normal origin (no margins). */
    NativeOverworldViewport_Reset(&viewport);
    NativeOverworldViewport_TopLeft(&viewport, 100, 200, &left, &top);
    assert(left == 100 && top == 200);
}

/* ---- Stage 4A generalized map renderer ---- */

static void TestWithMeta240ByteIdentity(void)
{
    static u16 out240[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u8 win240[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 layers240[4 * DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 ref240[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    int i;

    /* All three layers opaque over the whole ring, distinct tiles. */
    SceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 31, 31, BgTile(1, 0));
    SetRingRect(1, 0, 0, 31, 31, BgTile(2, 0));
    SetRingRect(2, 0, 0, 31, 31, BgTile(3, 0));

    /* DrawMapFrameWithMeta (the wrapper) and the raw Ex at the same 240x160
     * rectangle both start at the logical origin, so they must be byte-identical
     * to the plain DrawMapFrame output. */
    assert(NativeOverworldRenderer_DrawMapFrameWithMeta(&sSnap, out240, win240,
                                                        layers240));
    assert(DrawMapFrameWithMetaEx(&sSnap, ref240, win240, layers240,
                                  DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0));
    assert(NativeOverworldRenderer_DrawMapFrame(&sSnap, ref240));
    for (i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        assert(out240[i] == ref240[i]);
        assert(win240[i] == NATIVE_COMPOSITE_WIN_BG1); /* BG1 top priority */
    }

    /* The flat layer colors (oracle scanline.layers[bgnum] encoding) are filled
     * for every opaque layer with bit 15 set; the GBA BG0 slot stays 0. */
    assert((layers240[1 * DISPLAY_WIDTH * DISPLAY_HEIGHT] & 0x8000) != 0);
    assert((layers240[2 * DISPLAY_WIDTH * DISPLAY_HEIGHT] & 0x8000) != 0);
    assert((layers240[3 * DISPLAY_WIDTH * DISPLAY_HEIGHT] & 0x8000) != 0);
    assert(layers240[0] == 0);
}

static void TestWithMetaCapturedPriorities(void)
{
    static u16 frame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u8 win[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    /* All three layers opaque at the same ring index. With the field's fixed
     * priorities (BG1=1, BG2=2, BG3=3) BG1 wins. */
    SceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 31, 31, BgTile(1, 0));
    SetRingRect(1, 0, 0, 31, 31, BgTile(2, 0));
    SetRingRect(2, 0, 0, 31, 31, BgTile(3, 0));
    assert(DrawMapFrameWithMetaEx(&sSnap, frame, win, NULL,
                                  DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0));
    assert(win[0] == NATIVE_COMPOSITE_WIN_BG1);

    /* Reorder the CAPTURED BGCNT priorities (BG2 now priority 1): the winner
     * follows the captured priorities, NOT the fixed field order. */
    SceneReset(2, 1, 3);
    SetRingRect(0, 0, 0, 31, 31, BgTile(1, 0));
    SetRingRect(1, 0, 0, 31, 31, BgTile(2, 0));
    SetRingRect(2, 0, 0, 31, 31, BgTile(3, 0));
    assert(DrawMapFrameWithMetaEx(&sSnap, frame, win, NULL,
                                  DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0));
    assert(win[0] == NATIVE_COMPOSITE_WIN_BG2);

    /* BG3 at priority 1 wins. */
    SceneReset(2, 3, 1);
    SetRingRect(0, 0, 0, 31, 31, BgTile(1, 0));
    SetRingRect(1, 0, 0, 31, 31, BgTile(2, 0));
    SetRingRect(2, 0, 0, 31, 31, BgTile(3, 0));
    assert(DrawMapFrameWithMetaEx(&sSnap, frame, win, NULL,
                                  DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0));
    assert(win[0] == NATIVE_COMPOSITE_WIN_BG3);
}

static void TestWithMetaExExpandedReveal(void)
{
    static u16 out240[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u8 win240[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 out300[NATIVE_VIEWPORT_MAX_WIDTH * NATIVE_VIEWPORT_MAX_HEIGHT];
    static u8 win300[NATIVE_VIEWPORT_MAX_WIDTH * NATIVE_VIEWPORT_MAX_HEIGHT];
    int sy;
    int sx;

    /* Ring fully opaque (BG1 tile 1 on top); the 300x200 view sits 30 left /
     * 20 above the logical origin (300x200 margins). The center crop must equal
     * the 240x160 output at the SAME world focus. */
    SceneReset(1, 2, 3);
    SetRingRect(0, 0, 0, 31, 31, BgTile(1, 0));
    SetRingRect(1, 0, 0, 31, 31, BgTile(2, 0));
    SetRingRect(2, 0, 0, 31, 31, BgTile(3, 0));
    assert(DrawMapFrameWithMetaEx(&sSnap, out240, win240, NULL,
                                  DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0));

    /* viewportLeftX = originX - 30 = -30, viewportTopY = -20. */
    assert(DrawMapFrameWithMetaEx(&sSnap, out300, win300, NULL,
                                  300, 200, -30, -20));

    /* Center crop byte-identical (color AND winner): screen (sx+30, sy+20)
     * samples world (sx, sy), the exact ring index the 240x160 path used. */
    for (sy = 0; sy < DISPLAY_HEIGHT; sy++)
    {
        for (sx = 0; sx < DISPLAY_WIDTH; sx++)
        {
            u32 expandedIndex = (u32)(sy + 20) * 300 + (sx + 30);
            assert(out300[expandedIndex] == out240[sy * DISPLAY_WIDTH + sx]);
            assert(win300[expandedIndex] == win240[sy * DISPLAY_WIDTH + sx]);
        }
    }

    /* Margin reveal: screen (0,20) is world (-30,0), outside the ring gate
     * (worldX < originX - 16), so it resolves the snapshot border fallback --
     * world the 240x160 view never sees. Border index for map (-2,0):
     * ((mapX+1)&1) + ((mapY+1)&1)*2 = ((-1)&1) + (1)*2 = 3 -> border metatile 4.
     * NORMAL layer BG1 quadrant 0 -> sPrimaryMetatiles[4*8+4]. */
    sPrimaryMetatiles[4 * NUM_TILES_PER_METATILE + 4] = BgTile(100, 0);
    /* Tile 100 solid opaque pixel 1 -> palette[1]. */
    memset((u8 *)sSnap.bgVram + 100 * 32, 0x11, 32);
    memset(VRAM_ + 100 * 32, 0x11, 32);
    assert(DrawMapFrameWithMetaEx(&sSnap, out300, win300, NULL,
                                  300, 200, -30, -20));
    /* BG pixels carry the GBA alpha flag (bit 15), so compare against the
     * opaque-encoded color. */
    assert(out300[20 * 300 + 0] == (ExpectedColor(0, 1) | 0x8000));
    assert(out300[20 * 300 + 0] != out240[0]);
}

static void TestWithMetaExRejections(void)
{
    static u16 frame[NATIVE_VIEWPORT_MAX_WIDTH * NATIVE_VIEWPORT_MAX_HEIGHT];
    static u8 winner[NATIVE_VIEWPORT_MAX_WIDTH * NATIVE_VIEWPORT_MAX_HEIGHT];

    SceneReset(1, 2, 3);
    assert(!DrawMapFrameWithMetaEx(NULL, frame, winner, NULL, 240, 160, 0, 0));
    assert(!DrawMapFrameWithMetaEx(&sSnap, NULL, winner, NULL, 240, 160, 0, 0));
    assert(!DrawMapFrameWithMetaEx(&sSnap, frame, NULL, NULL, 240, 160, 0, 0));

    /* Unsupported snapshot: non-NONE fallback reason. */
    sSnap.fallbackReason = NATIVE_FALLBACK_SCENE_NOT_OVERWORLD;
    assert(!DrawMapFrameWithMetaEx(&sSnap, frame, winner, NULL, 240, 160, 0, 0));

    /* Unsupported snapshot: missing a required capability. */
    SceneReset(1, 2, 3);
    sSnap.requiredCapabilities &= ~NATIVE_CAPABILITY_BG_VRAM;
    assert(!DrawMapFrameWithMetaEx(&sSnap, frame, winner, NULL, 240, 160, 0, 0));
}

int main(void)
{
    TestCoordinateMath();
    TestViewportScaling();
    TestViewportTopLeft();
    TestWithMeta240ByteIdentity();
    TestWithMetaCapturedPriorities();
    TestWithMetaExExpandedReveal();
    TestWithMetaExRejections();
    printf("%s\n", "native overworld renderer unit test passed");
    return 0;
}

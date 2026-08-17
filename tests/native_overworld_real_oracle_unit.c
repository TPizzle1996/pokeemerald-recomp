/*
 * Real-oracle parity harness: native 240x160 map background vs the ACTUAL
 * gba_easy_draw.c DrawFrame BG-only oracle.
 *
 * The Stage-1 synthetic harness (native_overworld_parity_unit.c) compares the
 * native renderer against a hand-port of gba_easy_draw's BG scanline path. A
 * port can drift from the real renderer, so this harness instead compiles the
 * production gba_easy_draw.c (as a separate translation unit, exactly like the
 * real build, so each TU keeps its own view of gIntrTable and the like) and
 * drives its debug-only oracle seam (gParityBGPixelsBuffer /
 * gParityBGPixelLayers, skipSprites=true) on the same fabricated frame state,
 * then compares pixel-exactly against the native renderer's output. OAM is
 * zeroed so sprites are inert; the oracle excludes OBJ by construction.
 *
 * Scenarios additionally validate the Stage-2 capability-gate changes:
 *   - screenbase variation (the ring is published at whatever screenbase BGCNT
 *     points at; the native ring read and the real VRAM read must agree),
 *   - in-place palette fade (PLTT mutated as a NORMAL/FAST fade does; both
 *     paths sample the same PLTT and must agree).
 *
 * gba_easy_draw.c is compiled separately with -DRENDERER_EASY_DRAW and linked
 * here; see tests/native_overworld_renderer_test.sh.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"

/* ---- Real oracle symbols (defined in the separate gba_easy_draw.c TU) ---- */
extern u16 *gParityBGPixelsBuffer;
extern u8 *gParityBGPixelLayers;
extern void DrawFrame(u16 *pixels);

/* ---- Stubs of game globals the production .c files reference ---- */

unsigned char REG_BASE[0x400] __attribute__((aligned(4)));
unsigned char PLTT[PLTT_SIZE] __attribute__((aligned(4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));
unsigned char OAM[OAM_SIZE] __attribute__((aligned(4)));

/* main.h declares `extern IntrFunc gIntrTable[]` (non-const); gba_easy_draw.c
 * sees `extern void (*const gIntrTable[])(void)`. Those views are incompatible
 * in a single TU, which is exactly why the real build keeps the oracle in its
 * own TU. Define the symbol here (non-const, matching main.h) for the link;
 * the oracle TU's const declaration binds to it at link time. Never invoked
 * (DISPSTAT intr-enable bits stay zero). */
IntrFunc gIntrTable[8] = {0};

/* gba_easy_draw.c DMA hook. The BG-only oracle path performs no DMA and the
 * normal gbaImage path is not part of the comparison; a no-op is sufficient
 * and keeps VRAM/PLTT untouched between the native and oracle renders. */
void RunDMAs(u32 type)
{
    (void)type;
}

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

/* Stub of the Stage-2 lean parity gate (defined in overworld.c in the real
 * build). The harness fabricates eligible scenes, so the gate always passes;
 * the point is to exercise NativeOverworld_CaptureParitySnapshot() through the
 * shared capture body, decoupled from the zoom/expanded predicate. */
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

/* Ring-world-anchor tile offsets: the oracle scenes stand at tile offset 0
 * (k=0), so the anchor is the plain camera-cell window. */
void GetCameraTileOffsets(u8 *xTileOffset, u8 *yTileOffset)
{
    *xTileOffset = 0;
    *yTileOffset = 0;
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

/* ---- Scene construction (mirrors the Stage-1 synthetic harness) ---- */

#define SCENE_GRID_W 38
#define SCENE_GRID_H 38
#define SCENE_MAX_LAYOUT 24
#define TILE_ENTRY(tile, hFlip, vFlip, pal) \
    ((u16)((tile) | ((hFlip) << 10) | ((vFlip) << 11) | ((pal) << 12)))

struct TestScene
{
    struct Tileset primaryTileset;
    struct Tileset secondaryTileset;
    struct MapLayout layout;
    u16 primaryMetatiles[NUM_METATILES_IN_PRIMARY * NUM_TILES_PER_METATILE];
    u16 secondaryMetatiles[NUM_METATILES_IN_PRIMARY * NUM_TILES_PER_METATILE];
    u16 primaryAttrs[NUM_METATILES_IN_PRIMARY];
    u16 secondaryAttrs[NUM_METATILES_IN_PRIMARY];
    u16 border[4];
    u16 grid[SCENE_GRID_W * SCENE_GRID_H];
    u16 ring[3][NATIVE_BG_RING_SIZE];
    s32 layoutW;
    s32 layoutH;
};

static struct TestScene sScene;

static u16 SceneGridMetatileIdAt(s32 gx, s32 gy)
{
    if (gx >= 0 && gy >= 0 && gx < sScene.layoutW + MAP_OFFSET_W
     && gy < sScene.layoutH + MAP_OFFSET_H)
    {
        u16 block = sScene.grid[gx + (sScene.layoutW + MAP_OFFSET_W) * gy];

        if (block != MAPGRID_UNDEFINED)
            return block & MAPGRID_METATILE_ID_MASK;
    }
    return sScene.border[((gx + 1) & 1) + ((gy + 1) & 1) * 2] & MAPGRID_METATILE_ID_MASK;
}

static u16 SceneMetatileAttributes(u16 metatileId)
{
    if (metatileId < NUM_METATILES_IN_PRIMARY)
        return sScene.primaryAttrs[metatileId];
    if (metatileId < NUM_METATILES_TOTAL)
        return sScene.secondaryAttrs[metatileId - NUM_METATILES_IN_PRIMARY];
    return 0;
}

/* Game-style metatile drawer: exactly field_camera.c DrawMetatile, writing the
 * four 8x8 quadrant tile entries of one metatile into the live BG ring. */
static void DrawSceneMetatile(int offset, s32 gx, s32 gy)
{
    u16 metatileId = SceneGridMetatileIdAt(gx, gy);
    u8 layerType = UNPACK_LAYER_TYPE(SceneMetatileAttributes(metatileId));
    const u16 *tiles = (metatileId < NUM_METATILES_IN_PRIMARY)
        ? sScene.primaryMetatiles + metatileId * NUM_TILES_PER_METATILE
        : sScene.secondaryMetatiles
            + (metatileId - NUM_METATILES_IN_PRIMARY) * NUM_TILES_PER_METATILE;

    switch (layerType)
    {
    case METATILE_LAYER_TYPE_SPLIT:
        sScene.ring[2][offset] = tiles[0];
        sScene.ring[2][offset + 1] = tiles[1];
        sScene.ring[2][offset + 0x20] = tiles[2];
        sScene.ring[2][offset + 0x21] = tiles[3];
        sScene.ring[1][offset] = 0;
        sScene.ring[1][offset + 1] = 0;
        sScene.ring[1][offset + 0x20] = 0;
        sScene.ring[1][offset + 0x21] = 0;
        sScene.ring[0][offset] = tiles[4];
        sScene.ring[0][offset + 1] = tiles[5];
        sScene.ring[0][offset + 0x20] = tiles[6];
        sScene.ring[0][offset + 0x21] = tiles[7];
        break;
    case METATILE_LAYER_TYPE_COVERED:
        sScene.ring[2][offset] = tiles[0];
        sScene.ring[2][offset + 1] = tiles[1];
        sScene.ring[2][offset + 0x20] = tiles[2];
        sScene.ring[2][offset + 0x21] = tiles[3];
        sScene.ring[1][offset] = tiles[4];
        sScene.ring[1][offset + 1] = tiles[5];
        sScene.ring[1][offset + 0x20] = tiles[6];
        sScene.ring[1][offset + 0x21] = tiles[7];
        sScene.ring[0][offset] = 0;
        sScene.ring[0][offset + 1] = 0;
        sScene.ring[0][offset + 0x20] = 0;
        sScene.ring[0][offset + 0x21] = 0;
        break;
    case METATILE_LAYER_TYPE_NORMAL:
    default:
        sScene.ring[2][offset] = 0x3014;
        sScene.ring[2][offset + 1] = 0x3014;
        sScene.ring[2][offset + 0x20] = 0x3014;
        sScene.ring[2][offset + 0x21] = 0x3014;
        sScene.ring[1][offset] = tiles[0];
        sScene.ring[1][offset + 1] = tiles[1];
        sScene.ring[1][offset + 0x20] = tiles[2];
        sScene.ring[1][offset + 0x21] = tiles[3];
        sScene.ring[0][offset] = tiles[4];
        sScene.ring[0][offset + 1] = tiles[5];
        sScene.ring[0][offset + 0x20] = tiles[6];
        sScene.ring[0][offset + 0x21] = tiles[7];
        break;
    }
}

/* Populate the live 32x32 ring exactly as DrawWholeMapViewInternal does: 16x16
 * metatiles, each drawn once into its 2x2 ring quadrant block. */
static void DrawSceneRing(s32 cameraMapX, s32 cameraMapY)
{
    for (s32 my = 0; my < 16; my++)
        for (s32 mx = 0; mx < 16; mx++)
            DrawSceneMetatile((int)(my * 64 + mx * 2),
                              cameraMapX + mx, cameraMapY + my);
}

/* Mimic DoScheduledBgTilemapCopiesToVram: publish the ring into the VRAM
 * screen-base blocks the CURRENT BGCNT registers point at. This keeps the ring
 * (native read) and the VRAM tilemap (real DrawFrame read) coherent for
 * whatever screenbase is configured. */
static void PublishSceneTilemaps(void)
{
    int screenBase[3] = {(REG_BG1CNT >> 8) & 0x1F,
                         (REG_BG2CNT >> 8) & 0x1F,
                         (REG_BG3CNT >> 8) & 0x1F};

    for (int bg = 0; bg < 3; bg++)
        memcpy(VRAM_ + 0x800 * screenBase[bg], sScene.ring[bg],
               NATIVE_BG_RING_SIZE * sizeof(u16));
}

static void SceneReset(s32 layoutW, s32 layoutH)
{
    memset(&sScene, 0, sizeof(sScene));
    sScene.layoutW = layoutW;
    sScene.layoutH = layoutH;
    sScene.layout.width = layoutW;
    sScene.layout.height = layoutH;
    sScene.layout.border = sScene.border;
    sScene.layout.primaryTileset = &sScene.primaryTileset;
    sScene.layout.secondaryTileset = &sScene.secondaryTileset;
    sScene.primaryTileset.metatiles = sScene.primaryMetatiles;
    sScene.primaryTileset.metatileAttributes = sScene.primaryAttrs;
    sScene.secondaryTileset.metatiles = sScene.secondaryMetatiles;
    sScene.secondaryTileset.metatileAttributes = sScene.secondaryAttrs;
    gMapHeader.mapLayout = &sScene.layout;
    gBackupMapLayout.width = layoutW + MAP_OFFSET_W;
    gBackupMapLayout.height = layoutH + MAP_OFFSET_H;
    gBackupMapLayout.map = sScene.grid;
    gOverworldTilemapBuffer_Bg1 = sScene.ring[0];
    gOverworldTilemapBuffer_Bg2 = sScene.ring[1];
    gOverworldTilemapBuffer_Bg3 = sScene.ring[2];

    for (int i = 0; i < SCENE_GRID_W * SCENE_GRID_H; i++)
        sScene.grid[i] = MAPGRID_UNDEFINED;
    for (int i = 0; i < 4; i++)
        sScene.border[i] = 0;

    REG_DISPCNT = DISPCNT_MODE_0 | DISPCNT_BG1_ON | DISPCNT_BG2_ON | DISPCNT_BG3_ON;
    REG_DISPSTAT = 0;
    REG_BG1CNT = BGCNT_PRIORITY(1) | BGCNT_CHARBASE(0)
               | BGCNT_SCREENBASE(29) | BGCNT_16COLOR | BGCNT_TXT256x256;
    REG_BG2CNT = BGCNT_PRIORITY(2) | BGCNT_CHARBASE(0)
               | BGCNT_SCREENBASE(28) | BGCNT_16COLOR | BGCNT_TXT256x256;
    REG_BG3CNT = BGCNT_PRIORITY(3) | BGCNT_CHARBASE(0)
               | BGCNT_SCREENBASE(30) | BGCNT_16COLOR | BGCNT_TXT256x256;
    REG_MOSAIC = 0;
    REG_BLDCNT = 0;
    REG_BLDALPHA = 0;
    REG_BLDY = 0;
    REG_WININ = 0;
    REG_WINOUT = 0;
    REG_WIN0H = 0;
    REG_WIN0V = 0;
    REG_WIN1H = 0;
    REG_WIN1V = 0;
}

static void SceneSetMapMetatile(s32 mx, s32 my, u16 metatileId)
{
    s32 gx = mx + MAP_OFFSET;
    s32 gy = my + MAP_OFFSET;

    sScene.grid[gx + (sScene.layoutW + MAP_OFFSET_W) * gy] = metatileId;
}

static void SetMetatileContent(int tileset, u16 metatileId, u16 layerType,
                               u16 t0, u16 t1, u16 t2, u16 t3,
                               u16 t4, u16 t5, u16 t6, u16 t7)
{
    u16 *metatiles = (tileset == 0) ? sScene.primaryMetatiles : sScene.secondaryMetatiles;
    u16 *attrs = (tileset == 0) ? sScene.primaryAttrs : sScene.secondaryAttrs;
    u16 base = metatileId * NUM_TILES_PER_METATILE;

    metatiles[base + 0] = t0;
    metatiles[base + 1] = t1;
    metatiles[base + 2] = t2;
    metatiles[base + 3] = t3;
    metatiles[base + 4] = t4;
    metatiles[base + 5] = t5;
    metatiles[base + 6] = t6;
    metatiles[base + 7] = t7;
    attrs[metatileId] = (u16)(layerType << 12);
}

/* Deterministic BG character memory: every tile 1..1023 has distinct nonzero
 * 4bpp pixel data, so every sampled map tile is opaque and tile identity is
 * verifiable from a single pixel. Tile 0 is deliberately left blank: the
 * cleared-window filler and any unfilled ring cell reference entry 0 (tile 0),
 * and both the BG0-content capture gate and the oracle treat a zero entry as
 * transparent -- an opaque tile 0 would make the enabled BG0 layer (which the
 * native map renderer does not reproduce) render over the field and break the
 * BG0-on parity scenarios. */
static void FillTileData(void)
{
    for (int t = 1; t < 1024; t++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x += 2)
            {
                u8 lo = (u8)(1 + ((t * 3 + y * 5 + x / 2) % 15));
                u8 hi = (u8)(1 + ((t * 5 + y * 7 + x / 2 + 1) % 15));

                VRAM_[t * 32 + y * 4 + x / 2] = (u8)(lo | (hi << 4));
            }
}

static void FillPalette(void)
{
    for (int b = 0; b < 16; b++)
        for (int p = 0; p < 16; p++)
            ((u16 *)PLTT)[b * 16 + p] = (u16)((b << 8) | (p << 1));
}

static void SetCamera(s32 cameraMapX, s32 cameraMapY, s16 camX, s16 camY)
{
    sSaveBlock1.pos.x = (s16)cameraMapX;
    sSaveBlock1.pos.y = (s16)cameraMapY;
    sCameraX = camX;
    sCameraY = camY;
    REG_BG1HOFS = (u16)camX;
    REG_BG2HOFS = (u16)camX;
    REG_BG3HOFS = (u16)camX;
    REG_BG1VOFS = (u16)camY;
    REG_BG2VOFS = (u16)camY;
    REG_BG3VOFS = (u16)camY;
}

/* A moving frame: the logical camera has advanced this frame but the latched BG
 * scroll registers still hold the previous frame's offsets. The native samples
 * the ring at the scroll (presentation origin), and the real DrawFrame oracle
 * reads the SAME live REG_BG*HOFS/VOFS registers, so the two must agree even
 * when scroll != camera. Sets all three layers to the same scroll, as
 * FieldUpdateBgTilemapScroll does, so the capture gate accepts the frame. */
static void SetCameraScrolled(s32 cameraMapX, s32 cameraMapY, s16 camX, s16 camY,
                              s16 hofs, s16 vofs)
{
    sSaveBlock1.pos.x = (s16)cameraMapX;
    sSaveBlock1.pos.y = (s16)cameraMapY;
    sCameraX = camX;
    sCameraY = camY;
    REG_BG1HOFS = (u16)hofs;
    REG_BG2HOFS = (u16)hofs;
    REG_BG3HOFS = (u16)hofs;
    REG_BG1VOFS = (u16)vofs;
    REG_BG2VOFS = (u16)vofs;
    REG_BG3VOFS = (u16)vofs;
}

static void SetupCommonMetatiles(void)
{
    /* 0: NORMAL grass -- BG2 bottom tiles, BG1 top tiles (top covers OBJ). */
    SetMetatileContent(0, 0, METATILE_LAYER_TYPE_NORMAL,
                       TILE_ENTRY(10, 0, 0, 0), TILE_ENTRY(11, 0, 0, 0),
                       TILE_ENTRY(12, 0, 0, 0), TILE_ENTRY(13, 0, 0, 0),
                       TILE_ENTRY(20, 0, 0, 0), TILE_ENTRY(21, 0, 0, 0),
                       TILE_ENTRY(22, 0, 0, 0), TILE_ENTRY(23, 0, 0, 0));
    /* 1: animated water -- NORMAL bottom layer with flipped tiles. */
    SetMetatileContent(0, 1, METATILE_LAYER_TYPE_NORMAL,
                       TILE_ENTRY(30, 1, 0, 0), TILE_ENTRY(31, 0, 0, 0),
                       TILE_ENTRY(32, 0, 1, 0), TILE_ENTRY(33, 1, 1, 0),
                       TILE_ENTRY(20, 0, 0, 0), TILE_ENTRY(21, 0, 0, 0),
                       TILE_ENTRY(22, 0, 0, 0), TILE_ENTRY(23, 0, 0, 0));
    /* 2: cave -- COVERED, cave bottom covers BG3, ceiling on BG2. */
    SetMetatileContent(0, 2, METATILE_LAYER_TYPE_COVERED,
                       TILE_ENTRY(50, 0, 0, 1), TILE_ENTRY(51, 0, 0, 1),
                       TILE_ENTRY(52, 0, 0, 1), TILE_ENTRY(53, 0, 0, 1),
                       TILE_ENTRY(60, 0, 0, 1), TILE_ENTRY(61, 0, 0, 1),
                       TILE_ENTRY(62, 0, 0, 1), TILE_ENTRY(63, 0, 0, 1));
    /* 3: building floor -- SPLIT, floor on BG3, ceiling/walls on BG1. */
    SetMetatileContent(0, 3, METATILE_LAYER_TYPE_SPLIT,
                       TILE_ENTRY(70, 0, 0, 2), TILE_ENTRY(71, 0, 0, 2),
                       TILE_ENTRY(72, 0, 0, 2), TILE_ENTRY(73, 0, 0, 2),
                       TILE_ENTRY(80, 0, 0, 2), TILE_ENTRY(81, 0, 0, 2),
                       TILE_ENTRY(82, 0, 0, 2), TILE_ENTRY(83, 0, 0, 2));
    /* Secondary 0: NORMAL with a palette bank on the top layer. */
    SetMetatileContent(1, 0, METATILE_LAYER_TYPE_NORMAL,
                       TILE_ENTRY(90, 0, 0, 3), TILE_ENTRY(91, 0, 0, 3),
                       TILE_ENTRY(92, 0, 0, 3), TILE_ENTRY(93, 0, 0, 3),
                       TILE_ENTRY(100, 1, 1, 4), TILE_ENTRY(101, 0, 0, 4),
                       TILE_ENTRY(102, 0, 0, 4), TILE_ENTRY(103, 0, 0, 4));
    /* Secondary 1: COVERED. */
    SetMetatileContent(1, 1, METATILE_LAYER_TYPE_COVERED,
                       TILE_ENTRY(110, 0, 0, 5), TILE_ENTRY(111, 0, 0, 5),
                       TILE_ENTRY(112, 0, 0, 5), TILE_ENTRY(113, 0, 0, 5),
                       TILE_ENTRY(114, 0, 0, 5), TILE_ENTRY(115, 0, 0, 5),
                       TILE_ENTRY(116, 0, 0, 5), TILE_ENTRY(117, 0, 0, 5));
}

static void FillMapGrid(s32 layoutW, s32 layoutH, int pattern)
{
    for (s32 my = 0; my < layoutH; my++)
        for (s32 mx = 0; mx < layoutW; mx++)
        {
            u16 id;

            switch (pattern)
            {
            case 0: id = (mx + my) % 2;                    break; /* grass/water   */
            case 1: id = ((mx / 2 + my / 2) % 2) ? 0 : 1;  break; /* water blocks  */
            case 2: id = 2;                                 break; /* cave          */
            case 3: id = 3;                                 break; /* building floor*/
            default: id = 0;                                break;
            }
            SceneSetMapMetatile(mx, my, id);
        }
}

/* ---- Real-oracle comparison ------------------------------------------ */

static void AssertRealOracleParityCapture(const char *name, bool32 useParity)
{
    struct NativeOverworldSnapshot snap;
    static u16 nativeFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 gbaImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 oracleFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u8 oracleLayers[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u32 mismatches = 0;
    u32 layerHist[5] = {0, 0, 0, 0, 0};
    u32 firstIdx = 0;
    u16 firstNative = 0;
    u16 firstGba = 0;

    memset(gbaImage, 0, sizeof(gbaImage));
    memset(oracleFrame, 0, sizeof(oracleFrame));

    /* Stage-2 uses the lean parity capture (decoupled from the zoom/expanded
     * predicate); Stage-1 uses the strict snapshot. Both share the capture body
     * and must accept the field's real display setup (BG0 enabled). */
    if (useParity)
        assert(NativeOverworld_CaptureParitySnapshot(&snap));
    else
        assert(NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_NONE);
    assert(NativeOverworldRenderer_DrawMapFrame(&snap, nativeFrame));

    gParityBGPixelsBuffer = oracleFrame;
    gParityBGPixelLayers = oracleLayers;
    DrawFrame(gbaImage);          /* real production renderer + oracle seam */
    gParityBGPixelsBuffer = NULL;
    gParityBGPixelLayers = NULL;

    /* Same masked comparison the runtime parity module performs (bit 15 is the
     * alpha flag and is not part of color parity). */
    for (u32 i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        if ((nativeFrame[i] & 0x7FFF) != (oracleFrame[i] & 0x7FFF))
        {
            if (mismatches == 0)
            {
                firstIdx = i;
                firstNative = nativeFrame[i];
                firstGba = oracleFrame[i];
            }
            mismatches++;
            if (oracleLayers[i] <= 4)
                layerHist[oracleLayers[i]]++;
        }
    }

    if (mismatches != 0)
    {
        fprintf(stderr,
                "real-oracle MISMATCH %s first=(%u,%u) native=0x%04X oracle=0x%04X "
                "layer bd:%u bg1:%u bg2:%u bg3:%u\n",
                name, firstIdx % DISPLAY_WIDTH, firstIdx / DISPLAY_WIDTH,
                firstNative & 0x7FFF, firstGba & 0x7FFF,
                layerHist[0], layerHist[2], layerHist[3], layerHist[4]);
        fflush(stderr);
    }
    printf("%-26s total=%d mismatches=%u (%.2f%%)\n", name,
           DISPLAY_WIDTH * DISPLAY_HEIGHT, mismatches,
           (double)mismatches * 100.0 / (double)(DISPLAY_WIDTH * DISPLAY_HEIGHT));
    assert(mismatches == 0);
}

static void AssertRealOracleParity(const char *name)
{
    AssertRealOracleParityCapture(name, FALSE);
}

/* ---- Scenarios ------------------------------------------------------- */

static void TestRealOracleOutdoor(void)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertRealOracleParity("real oracle: outdoor");
}

static void TestRealOracleIndoor(void)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 3);
    SetCamera(15, 15, 2, 3);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertRealOracleParity("real oracle: indoor split");
}

static void TestRealOracleCave(void)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 2);
    SetCamera(15, 15, 5, 7);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertRealOracleParity("real oracle: cave covered");
}

static void TestRealOracleCameraPan(void)
{
    /* Sub-tile pan plus an across-metatile pan; every 8x8 tile in the window
     * must be sampled identically by the native ring path and the real VRAM
     * path across the full pan range. */
    for (int camY = 0; camY < 8; camY++)
        for (int camX = 0; camX < 8; camX++)
        {
            char name[64];

            SceneReset(24, 24);
            SetupCommonMetatiles();
            FillMapGrid(24, 24, 0);
            SetCamera(15, 15, (s16)camX, (s16)camY);
            DrawSceneRing(15, 15);
            PublishSceneTilemaps();
            snprintf(name, sizeof(name), "real oracle: pan (%d,%d)", camX, camY);
            AssertRealOracleParity(name);
        }
}

/* Directive 6: synthetic moving-frame cases A-K against the ACTUAL gba_easy_draw
 * oracle. The ring is drawn around the current camera map position; the scroll
 * registers are set independently to the previous frame's latched offsets. The
 * native samples the ring at the scroll (presentation origin) and the real
 * DrawFrame samples VRAM at the same live registers, so scroll != camera must
 * still yield pixel-exact parity. Pattern 1 gives spatially varied metatiles. */
static void RealOracleMovingFrameScene(s32 cameraMapX, s32 cameraMapY, s16 camX,
                                       s16 camY, s16 hofs, s16 vofs)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 1);
    SetCameraScrolled(cameraMapX, cameraMapY, camX, camY, hofs, vofs);
    DrawSceneRing(cameraMapX, cameraMapY);
    PublishSceneTilemaps();
}

static void TestRealOracleMovingFrameParity(void)
{
    char name[64];

    /* A: stationary baseline. */
    RealOracleMovingFrameScene(15, 15, 0, 0, 0, 0);
    AssertRealOracleParity("real oracle: A stationary");

    /* B: +1px horizontal movement. */
    RealOracleMovingFrameScene(15, 15, 1, 0, 0, 0);
    AssertRealOracleParity("real oracle: B +1px H");

    /* C: +1px vertical movement. */
    RealOracleMovingFrameScene(15, 15, 0, 1, 0, 0);
    AssertRealOracleParity("real oracle: C +1px V");

    /* D: diagonal. */
    RealOracleMovingFrameScene(15, 15, 1, 1, 0, 0);
    AssertRealOracleParity("real oracle: D diagonal");

    /* E: sub-tile 0..7 -- every 8px scroll phase, camera one ahead. */
    for (int d = 0; d < 8; d++)
    {
        snprintf(name, sizeof(name), "real oracle: E sub-tile %d", d);
        RealOracleMovingFrameScene(15, 15, (s16)(d + 1), (s16)(d + 1), (s16)d, (s16)d);
        AssertRealOracleParity(name);
    }

    /* F: 8px tile boundary. */
    RealOracleMovingFrameScene(15, 15, 8, 0, 7, 0);
    AssertRealOracleParity("real oracle: F 8px bnd H");
    RealOracleMovingFrameScene(15, 15, 0, 8, 0, 7);
    AssertRealOracleParity("real oracle: F 8px bnd V");

    /* G: 16px metatile boundary. */
    RealOracleMovingFrameScene(15, 15, 16, 16, 15, 15);
    AssertRealOracleParity("real oracle: G 16px bnd");

    /* H: ring wrap 255->0. */
    RealOracleMovingFrameScene(15, 15, 0, 0, 0xFF, 0xFF);
    AssertRealOracleParity("real oracle: H wrap 255->0");

    /* I: ring wrap 0->255 reverse. */
    RealOracleMovingFrameScene(15, 15, 15, 15, 0x1FF, 0x1FF);
    AssertRealOracleParity("real oracle: I wrap 0->255");

    /* J: camera pan/shake. */
    RealOracleMovingFrameScene(15, 15, 5, 3, 3, 2);
    AssertRealOracleParity("real oracle: J pan +2,+1");
    RealOracleMovingFrameScene(15, 15, -2, 4, -3, 3);
    AssertRealOracleParity("real oracle: J shake -1,+1");

    /* K: map-connection edge, moving one frame. */
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    for (s32 gy = 7; gy < 23; gy++)
        for (s32 gx = 23; gx < 27; gx++)
            sScene.grid[gx + (16 + MAP_OFFSET_W) * gy] = 1;
    SetCameraScrolled(21, 15, 1, 0, 0, 0);
    DrawSceneRing(21, 15);
    PublishSceneTilemaps();
    AssertRealOracleParity("real oracle: K map-connection");
}

/* Stage 2.1 dev scroll-fixture round trip. The runtime's fixture mechanism
 * (POKEEMERALD_NATIVE_PARITY_SCROLL_FIXTURES) serializes a bg-scroll-divergent
 * COMPARED frame under build/native-parity/scroll/ so a REAL moving frame can be
 * reproduced offline from its snapshot alone. This test proves the exact runtime
 * fixture format is self-contained and real-oracle faithful: it fabricates a
 * moving-frame scene, renders native + the real oracle, writes the fixture set
 * byte-for-byte as the runtime does, reads it back, re-renders native from the
 * deserialized snapshot, and asserts the re-render reproduces both the runtime's
 * own native frame and the captured oracle (0 mismatches). */
static void TestScrollFixtureRoundTrip(void)
{
    char tmpDir[] = "/tmp/native-overworld-scroll-fixture.XXXXXX";
    char path[512];
    char oldCwd[1024];
    struct NativeOverworldSnapshot snap;
    struct NativeOverworldSnapshot snapBack;
    static u16 nativeFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 gbaImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 oracleFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u8 oracleLayers[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 reRendered[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    FILE *f;
    u32 mismatches = 0;
    u32 i;

    /* A genuine moving frame: uniform scroll one frame of motion behind camera. */
    RealOracleMovingFrameScene(15, 15, 1, 0, 0, 0);
    assert(NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_NONE);
    assert(NativeOverworldRenderer_DrawMapFrame(&snap, nativeFrame));
    gParityBGPixelsBuffer = oracleFrame;
    gParityBGPixelLayers = oracleLayers;
    DrawFrame(gbaImage);
    gParityBGPixelsBuffer = NULL;
    gParityBGPixelLayers = NULL;
    assert((snap.bgHofs[0] & 0x1FF) != (snap.cameraX & 0x1FF)
        || (snap.bgVofs[0] & 0x1FF) != (snap.cameraY & 0x1FF));

    assert(getcwd(oldCwd, sizeof(oldCwd)) != NULL);
    assert(mkdtemp(tmpDir) != NULL);
    assert(chdir(tmpDir) == 0);
    assert(mkdir("build", 0755) == 0);
    assert(mkdir("build/native-parity", 0755) == 0);
    assert(mkdir("build/native-parity/scroll", 0755) == 0);

    /* Write the fixture set exactly as the runtime WriteScrollFixture does. */
    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-snap-100.bin");
    f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(&snap, 1, sizeof(snap), f) == sizeof(snap));
    fclose(f);
    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-native-100.bin");
    f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(nativeFrame, 2, DISPLAY_WIDTH * DISPLAY_HEIGHT, f)
           == (size_t)(DISPLAY_WIDTH * DISPLAY_HEIGHT));
    fclose(f);
    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-gba-100.bin");
    f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(oracleFrame, 2, DISPLAY_WIDTH * DISPLAY_HEIGHT, f)
           == (size_t)(DISPLAY_WIDTH * DISPLAY_HEIGHT));
    fclose(f);

    /* Read the snapshot back and re-render: the snapshot alone must reproduce the
     * runtime's native frame and the captured oracle (real-oracle path). */
    memset(&snapBack, 0, sizeof(snapBack));
    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-snap-100.bin");
    f = fopen(path, "rb");
    assert(f != NULL);
    assert(fread(&snapBack, 1, sizeof(snapBack), f) == sizeof(snapBack));
    fclose(f);
    assert(NativeOverworldSnapshotSupportsMap(&snapBack));
    assert(NativeOverworldRenderer_DrawMapFrame(&snapBack, reRendered));

    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-native-100.bin");
    f = fopen(path, "rb");
    assert(f != NULL);
    assert(fread(nativeFrame, 2, DISPLAY_WIDTH * DISPLAY_HEIGHT, f)
           == (size_t)(DISPLAY_WIDTH * DISPLAY_HEIGHT));
    fclose(f);
    for (i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
        assert(reRendered[i] == nativeFrame[i]); /* snapshot is self-contained */

    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-gba-100.bin");
    f = fopen(path, "rb");
    assert(f != NULL);
    assert(fread(oracleFrame, 2, DISPLAY_WIDTH * DISPLAY_HEIGHT, f)
           == (size_t)(DISPLAY_WIDTH * DISPLAY_HEIGHT));
    fclose(f);
    for (i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        if ((reRendered[i] & 0x7FFF) != (oracleFrame[i] & 0x7FFF))
            mismatches++;
    }
    assert(mismatches == 0);

    assert(chdir(oldCwd) == 0);
    printf("scroll fixture round trip: total=%d mismatches=%u (0.00%%)\n",
           DISPLAY_WIDTH * DISPLAY_HEIGHT, mismatches);
}

static void TestRealOracleMapConnection(void)
{
    /* Camera near the map edge so the window samples the border (connection)
     * region, exercising the border fallback against the real renderer. */
    SceneReset(8, 8);
    SetupCommonMetatiles();
    FillMapGrid(8, 8, 0);
    sScene.border[0] = 0x01;
    sScene.border[1] = 0x02;
    sScene.border[2] = 0x03;
    sScene.border[3] = 0x04;
    SetCamera(7, 7, 4, 6);
    DrawSceneRing(7, 7);
    PublishSceneTilemaps();
    AssertRealOracleParity("real oracle: map connection");
}

/* Validates the task-11 capability gate empirically: the renderer samples the
 * tilemap from the live ring, not the VRAM screenbase, so arbitrary (non-
 * overlapping, non-default) screenbase blocks must not change the output. The
 * real DrawFrame reads whatever screenbase BGCNT points at; publishing the ring
 * there keeps both coherent and parity must hold. */
static void TestRealOracleScreenbaseVariation(void)
{
    for (int sb = 24; sb <= 30; sb++)
    {
        /* Distinct per-layer blocks: PublishSceneTilemaps writes each ring to its
         * own screenbase, so the capture gate's ring-vs-screenbase coherence check
         * (and a real oracle comparison) require BG1/BG2/BG3 to map to DIFFERENT
         * blocks. A shared block makes the last publish win and the earlier rings
         * diverge from the block -- a genuine transitional condition, not the
         * "non-overlapping, non-default" variation this test intends. All three
         * companions stay in blocks 8..31 and outside charblock 0. */
        int sb2 = 8 + ((sb - 24 + 1) % 24);
        int sb3 = 8 + ((sb - 24 + 2) % 24);
        char name[64];

        SceneReset(16, 16);
        SetupCommonMetatiles();
        FillMapGrid(16, 16, 1);
        SetCamera(15, 15, 3, 5);
        REG_BG1CNT = (REG_BG1CNT & ~BGCNT_SCREENBASE(31)) | BGCNT_SCREENBASE(sb);
        REG_BG2CNT = (REG_BG2CNT & ~BGCNT_SCREENBASE(31)) | BGCNT_SCREENBASE(sb2);
        REG_BG3CNT = (REG_BG3CNT & ~BGCNT_SCREENBASE(31)) | BGCNT_SCREENBASE(sb3);
        DrawSceneRing(15, 15);
        PublishSceneTilemaps();
        snprintf(name, sizeof(name), "real oracle: screenbase %d", sb);
        AssertRealOracleParity(name);
    }
}

/* Validates the task-11 palette-fade capability: a NORMAL/FAST fade mutates
 * PLTT in place (gPlttBufferFaded -> PLTT), and both the native snapshot
 * (captured from PLTT) and the real oracle read the same faded palette. BLD
 * stays off, so no blend is applied by either path. */
static void TestRealOraclePaletteFade(void)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();

    /* Simulate a software fade mid-way to black: PLTT colors are darkened in
     * place exactly as UpdateNormalPaletteFade would leave gPlttBufferFaded
     * before TransferPlttBuffer copies it to PLTT. */
    for (int b = 0; b < 16; b++)
        for (int p = 0; p < 16; p++)
        {
            u16 c = ((u16 *)PLTT)[b * 16 + p];
            u16 r = ((c >> 0) & 0x1F) * 8 / 16;
            u16 g = ((c >> 5) & 0x1F) * 8 / 16;
            u16 bl = ((c >> 10) & 0x1F) * 8 / 16;

            ((u16 *)PLTT)[b * 16 + p] = (u16)((bl << 10) | (g << 5) | r);
        }

    AssertRealOracleParity("real oracle: palette fade (PLTT in place)");
}

/* The oracle seam's per-pixel winner-layer map must agree with the native
 * renderer's own winner classification at every pixel. */
static void TestRealOracleLayerMap(void)
{
    struct NativeOverworldSnapshot snap;
    static u16 nativeFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 gbaImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 oracleFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u8 oracleLayers[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u32 disagreed = 0;

    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 1, 2);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();

    assert(NativeOverworld_CaptureSnapshot(&snap));
    assert(NativeOverworldRenderer_DrawMapFrame(&snap, nativeFrame));

    gParityBGPixelsBuffer = oracleFrame;
    gParityBGPixelLayers = oracleLayers;
    DrawFrame(gbaImage);
    gParityBGPixelsBuffer = NULL;
    gParityBGPixelLayers = NULL;

    for (s32 y = 0; y < DISPLAY_HEIGHT; y++)
    {
        for (s32 x = 0; x < DISPLAY_WIDTH; x++)
        {
            u32 idx = (u32)(y * DISPLAY_WIDTH + x);
            u8 oracleWinner = oracleLayers[idx];
            u8 nativeWinner;

            if ((oracleFrame[idx] & 0x7FFF) == (nativeFrame[idx] & 0x7FFF))
            {
                /* Both agree on color; also require the oracle's layer map to
                 * match the native winner for a backdrop row (oracleLayers 0
                 * means backdrop; a BG pixel here would mean the winner map is
                 * not matching the composite). */
                if (oracleWinner == 0)
                    continue;
            }
            {
                u16 entries[3];
                u16 color;
                s32 wx = snap.cameraMapX * 16 + snap.cameraX + x;
                s32 wy = snap.cameraMapY * 16 + snap.cameraY + y;

                ResolveBgTileEntries(&snap, wx, wy, entries);
                nativeWinner = 4; /* backdrop */
                for (int bg = 0; bg < 3; bg++)
                {
                    if (SampleSnapshotTile(&snap, entries[bg], wx, wy, &color))
                    {
                        nativeWinner = (u8)bg;
                        break;
                    }
                }
            }
            /* The oracle seam's layer map is 0=backdrop, 1=BG0, 2=BG1, 3=BG2,
             * 4=BG3 (bgnum + 1, matching the full-composite winner encoding).
             * The native winner is 4=backdrop, 0=BG1, 1=BG2, 2=BG3, so the BG
             * mapping is native + 2; backdrop maps to oracle 0. */
            if (nativeWinner == 4)
            {
                if (oracleWinner != 0)
                    disagreed++;
            }
            else if (oracleWinner != (u8)(nativeWinner + 2))
                disagreed++;
        }
    }

    printf("%-26s layer-map disagreements=%u\n", "real oracle: layer map", disagreed);
    assert(disagreed == 0);
}

/* Stage-2 regression: the field enables all four BG layers (ShowBg(0..3) in
 * overworld.c), so every real overworld frame has REG_DISPCNT with BG0 ON. The
 * capture gate historically required BG0 OFF and therefore rejected EVERY field
 * frame at DISPLAY_LAYERS -- the reported failure (scheduled=128, compared=0,
 * no pixels compared). These scenarios prove the capture now accepts BG0-on
 * frames. BG0's own tilemap is kept transparent (all-zero tile entries), so BG0
 * contributes nothing visible and the oracle still equals the native
 * map-background output. */
static void EnableFieldBg0(void)
{
    /* overworld.c ResetAllBgsCoordinatesAndBgCnt configures BG0 as a 256x256
     * 16-color text layer. Screenbase 13 mirrors the field's BG0 screenbase;
     * its VRAM tilemap is zeroed (transparent). */
    REG_BG0CNT = BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0)
               | BGCNT_SCREENBASE(13) | BGCNT_16COLOR | BGCNT_TXT256x256;
    memset(VRAM_ + 0x800 * 13, 0, NATIVE_BG_RING_SIZE * sizeof(u16));
    REG_DISPCNT |= DISPCNT_BG0_ON;
}

static void TestRealOracleBg0On(void)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    EnableFieldBg0();
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertRealOracleParityCapture("real oracle: BG0 on (field)", FALSE);
    fprintf(stderr, "real-oracle BG0-on capture accepted (Stage-1 path)\n");
}

static void TestRealOracleParityPathBg0On(void)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 2, 1);
    EnableFieldBg0();
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertRealOracleParityCapture("real oracle: BG0 on, parity path", TRUE);
    fprintf(stderr, "real-oracle BG0-on capture accepted (parity path)\n");
}

/* Stage-2 regression: the field's persistent blend config -- overworld.c
 * InitOverworldGraphicsRegisters sets BLDCNT = TGT2(BG1|BG2|BG3|OBJ) |
 * EFFECT_BLEND (0x1E40) with an EMPTY TGT1 and BLDALPHA 13/7 at every map load
 * -- must not change the BG-only oracle. The GBA alpha blend needs a TGT1 pixel
 * as the top blended layer; with TGT1 empty it never fires, so the oracle is
 * pixel-identical to REG_BLDCNT == 0 even when the map carries alpha-flagged
 * colors. A genuine map-affecting config (a map layer in TGT1) MUST change the
 * oracle, which proves the harness can actually detect real blending and that
 * the field state's neutrality is not just an artifact of the test scene. */
static void TestRealOraclePersistentFieldBlend(void)
{
    static u16 gbaImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 oracleBase[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 oracleFieldBlend[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 oracleGenuineBlend[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u32 neutralMismatches = 0;
    u32 genuineMismatches = 0;

    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    FillTileData();
    FillPalette();
    /* Mark every BG palette color alpha (bit 15) so the blend path engages. */
    for (int b = 0; b < 16; b++)
        for (int p = 0; p < 16; p++)
            ((u16 *)PLTT)[b * 16 + p] |= 0x8000;

    /* Baseline oracle with no blend configured. */
    REG_BLDCNT = 0;
    REG_BLDALPHA = 0;
    REG_BLDY = 0;
    gParityBGPixelsBuffer = oracleBase;
    gParityBGPixelLayers = NULL;
    DrawFrame(gbaImage);
    gParityBGPixelsBuffer = NULL;

    /* The persistent field blend state: alpha mode, TGT1 empty. */
    REG_BLDCNT = BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3
               | BLDCNT_TGT2_OBJ | BLDCNT_EFFECT_BLEND;
    REG_BLDALPHA = BLDALPHA_BLEND(13, 7);
    REG_BLDY = 0;
    gParityBGPixelsBuffer = oracleFieldBlend;
    gParityBGPixelLayers = NULL;
    DrawFrame(gbaImage);
    gParityBGPixelsBuffer = NULL;

    for (u32 i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
        if ((oracleBase[i] & 0x7FFF) != (oracleFieldBlend[i] & 0x7FFF))
            neutralMismatches++;

    /* A genuine map-affecting config: BG1 in TGT1 blends with BG2 in TGT2. */
    REG_BLDCNT = BLDCNT_TGT1_BG1 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG2;
    REG_BLDALPHA = BLDALPHA_BLEND(13, 7);
    REG_BLDY = 0;
    gParityBGPixelsBuffer = oracleGenuineBlend;
    gParityBGPixelLayers = NULL;
    DrawFrame(gbaImage);
    gParityBGPixelsBuffer = NULL;

    for (u32 i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
        if ((oracleBase[i] & 0x7FFF) != (oracleGenuineBlend[i] & 0x7FFF))
            genuineMismatches++;

    printf("%-26s neutral_mismatches=%u genuine_mismatches=%u\n",
           "real oracle: field blend", neutralMismatches, genuineMismatches);
    assert(neutralMismatches == 0); /* persistent config is oracle-neutral */
    assert(genuineMismatches != 0); /* genuine blend is detectable */
    fprintf(stderr, "real-oracle persistent field blend is oracle-neutral\n");
}

int main(void)
{
    /* Every real-oracle scenario must sample REAL, opaque, distinct tile and
     * palette data, or both renderer and oracle read empty VRAM and agree
     * vacuously (all-backdrop, 0 mismatches) -- a silent no-op comparison. Fill
     * char data and PLTT up front; EnableFieldBg0 re-zeroes BG0's screenbase 13
     * after this, so the BG0-on scenarios stay provably transparent. */
    FillTileData();
    FillPalette();
    TestRealOracleOutdoor();
    TestRealOracleIndoor();
    TestRealOracleCave();
    TestRealOracleCameraPan();
    TestRealOracleMovingFrameParity();
    TestScrollFixtureRoundTrip();
    TestRealOracleMapConnection();
    TestRealOracleScreenbaseVariation();
    TestRealOraclePaletteFade();
    TestRealOracleLayerMap();
    TestRealOracleBg0On();
    TestRealOracleParityPathBg0On();
    TestRealOraclePersistentFieldBlend();
    fprintf(stderr, "real oracle parity harness passed\n");
    return 0;
}

/*
 * Stage 0 parity harness: native 240x160 map background vs DrawFrame BG path.
 *
 * (A) The native renderer reads the immutable snapshot's live BG ring and BG
 *     VRAM/palette (NativeOverworld_CaptureSnapshot + DrawMapFrame).
 * (B) The reference below is a faithful port of gba_easy_draw's text-mode BG
 *     scanline path (RenderBGScanline + the priority 3->0 last-write-wins
 *     composite). OBJ pixels are excluded by construction: this harness renders
 *     only the BG3/BG2/BG1 layers and never the sprite layers.
 *
 * Every test scene fabricates a self-consistent overworld frame the way the
 * game would: the live BG ring is drawn from the backup map grid + metatile
 * tables exactly as field_camera.c DrawMetatile does, the ring is then copied
 * into the VRAM screen-base tilemaps (mimicking DoScheduledBgTilemapCopiesToVram),
 * and the camera scroll registers are set to GetCameraOffsetWithPan().
 *
 * The harness compares the two 240x160 u16 outputs pixel-exactly (GBA 15-bit
 * color, bit15 alpha flag). A mismatch is classified by which BG layer (or
 * backdrop) won at that pixel, so failures are attributable to layer selection,
 * palette, tile content, or camera math.
 *
 * Run via tests/native_overworld_renderer_test.sh.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"

/* ---- Stubs of game globals the production .c file references ---- */

unsigned char REG_BASE[0x400] __attribute__((aligned(4)));
unsigned char PLTT[PLTT_SIZE] __attribute__((aligned(4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));

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
static bool32 sRendererReady = TRUE;

/* Ring-world-anchor tile offsets (Stage 4A). The parity scenes all stand at
 * tile offset 0 (k=0), so these stay 0: anchor = cameraMap*16 - 0*8 = the
 * plain camera-cell window the existing residency assertions expect. */
static u8 sCameraXTileOffset;
static u8 sCameraYTileOffset;

bool32 Overworld_IsNativeExpandedRendererReady(void)
{
    return sRendererReady;
}

void GetCameraOffsetWithPan(s16 *x, s16 *y)
{
    *x = sCameraX;
    *y = sCameraY;
}

void GetCameraTileOffsets(u8 *xTileOffset, u8 *yTileOffset)
{
    *xTileOffset = sCameraXTileOffset;
    *yTileOffset = sCameraYTileOffset;
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

/* ---- Scene construction --------------------------------------------- */

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
 * metatiles, each drawn once into its 2x2 ring quadrant block, ring metatile
 * (mx,my) showing grid coord (cameraMapX + mx, cameraMapY + my). */
static void DrawSceneRing(s32 cameraMapX, s32 cameraMapY)
{
    for (s32 my = 0; my < 16; my++)
        for (s32 mx = 0; mx < 16; mx++)
            DrawSceneMetatile((int)(my * 64 + mx * 2),
                              cameraMapX + mx, cameraMapY + my);
}

/* Mimic DoScheduledBgTilemapCopiesToVram: the ring IS the VRAM tilemap for the
 * 240x160 window, so publish it into the BG1/BG2/BG3 screen-base blocks. */
static void PublishSceneTilemaps(void)
{
    memcpy(VRAM_ + 0x800 * 29, sScene.ring[0], NATIVE_BG_RING_SIZE * sizeof(u16));
    memcpy(VRAM_ + 0x800 * 28, sScene.ring[1], NATIVE_BG_RING_SIZE * sizeof(u16));
    memcpy(VRAM_ + 0x800 * 30, sScene.ring[2], NATIVE_BG_RING_SIZE * sizeof(u16));
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

    /* Default grid: MAPGRID_UNDEFINED everywhere (like InitMapLayoutData's
     * CpuFastFill16), so coords outside the map fall back to the border. */
    for (int i = 0; i < SCENE_GRID_W * SCENE_GRID_H; i++)
        sScene.grid[i] = MAPGRID_UNDEFINED;
    for (int i = 0; i < 4; i++)
        sScene.border[i] = 0;

    REG_DISPCNT = DISPCNT_MODE_0 | DISPCNT_BG1_ON | DISPCNT_BG2_ON | DISPCNT_BG3_ON;
    REG_BG1CNT = BGCNT_PRIORITY(1) | BGCNT_CHARBASE(0) | BGCNT_MOSAIC
               | BGCNT_SCREENBASE(29) | BGCNT_16COLOR | BGCNT_TXT256x256;
    REG_BG2CNT = BGCNT_PRIORITY(2) | BGCNT_CHARBASE(0) | BGCNT_MOSAIC
               | BGCNT_SCREENBASE(28) | BGCNT_16COLOR | BGCNT_TXT256x256;
    REG_BG3CNT = BGCNT_PRIORITY(3) | BGCNT_CHARBASE(0) | BGCNT_MOSAIC
               | BGCNT_SCREENBASE(30) | BGCNT_16COLOR | BGCNT_TXT256x256;
    /* BG0 carries the field's UI/window layer. Its real field config is
     * charBase 2 / mapBase 31 (sOverworldBgTemplates); every scene models that
     * so the BG0-content gate (which only runs when DISPCNT_BG0_ON) sees the
     * realistic screen base. Scenes leave BG0's screen block empty, i.e. a
     * field frame with no message box / text window open. */
    REG_BG0CNT = BGCNT_CHARBASE(2) | BGCNT_SCREENBASE(31) | BGCNT_MOSAIC
               | BGCNT_16COLOR | BGCNT_TXT256x256;
    REG_MOSAIC = 0;
    REG_BLDCNT = 0;
    REG_WININ = 0;
    REG_WINOUT = 0;
    REG_WIN0H = 0;
    REG_WIN0V = 0;
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

/* Deterministic BG character memory: every tile 0..1023 has distinct nonzero
 * 4bpp pixel data, so every sampled tile is opaque and tile identity is
 * verifiable from a single pixel. */
static void FillTileData(void)
{
    for (int t = 0; t < 1024; t++)
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

/* The palette entry for a (paletteBank, pixel) pair, matching FillPalette. */
static u16 PaletteColor(int paletteBank, int pixel)
{
    return (u16)((paletteBank << 8) | (pixel << 1));
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

/* A moving frame: the logical camera (sCameraX/cameraX, sCameraY/cameraY) has
 * advanced this frame, but the latched BG scroll registers still hold the
 * previous frame's offsets (FieldUpdateBgTilemapScroll runs in the VBlank
 * handler AFTER CameraUpdate). The native renderer must sample the ring at the
 * scroll (the presentation origin), NOT at the camera; the parity harness's
 * reference already reads the scroll registers. Sets all three BG scrolls to
 * the same value, as FieldUpdateBgTilemapScroll does, so the capture gate
 * (which requires the three layers to agree) accepts the frame. */
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

/* ---- Reference: faithful port of gba_easy_draw BG path (OBJ excluded) ---- */

static void RefRenderBGScanline(const struct NativeOverworldSnapshot *snap,
                                int bg, int y, u16 *line)
{
    static const int kScreenBase[3] = {29, 28, 30};
    u8 *bgtiles = (u8 *)VRAM_;
    u16 *pal = (u16 *)PLTT;
    u16 hoffs = snap->bgHofs[bg] & 0x1FF;
    u16 voffs = snap->bgVofs[bg] & 0x1FF;
    u16 *bgmap = (u16 *)(VRAM_ + 0x800 * kScreenBase[bg]);

    for (int x = 0; x < DISPLAY_WIDTH; x++)
    {
        unsigned int xx = (x + hoffs) & 0x1FF;
        unsigned int yy = (y + voffs) & 0x1FF;
        /* gba_easy_draw.c masks the 9-bit scroll into the 256px tilemap for
         * 256x256 text mode before indexing the 32x32 ring. */
        xx &= 0xFF;
        yy &= 0xFF;
        unsigned int mapX = xx / 8;
        unsigned int mapY = yy / 8;
        u16 entry = bgmap[mapY * 32 + mapX];
        unsigned int tileNum = entry & 0x3FF;
        unsigned int paletteNum = (entry >> 12) & 0xF;
        unsigned int tileX = xx % 8;
        unsigned int tileY = yy % 8;
        u8 pixel;

        if (entry & (1 << 10))
            tileX = 7 - tileX;
        if (entry & (1 << 11))
            tileY = 7 - tileY;
        pixel = bgtiles[tileNum * 32 + tileY * 4 + tileX / 2];
        if (tileX & 1)
            pixel >>= 4;
        else
            pixel &= 0xF;
        if (pixel != 0)
            line[x] = pal[16 * paletteNum + pixel] | 0x8000;
    }
}

/* Composite BG3/BG2/BG1 exactly as DrawScanline does (priority 3->0, last
 * opaque write wins), with the backdrop PLTT[0] as the default. Records which
 * layer won per pixel for diagnostics: 0=BG1, 1=BG2, 2=BG3, 3=backdrop. */
static void RefRenderMap(const struct NativeOverworldSnapshot *snap,
                         u16 *out, u8 *winnerOut)
{
    u16 backdrop = *(u16 *)PLTT;

    for (int y = 0; y < DISPLAY_HEIGHT; y++)
    {
        u16 layers[3][DISPLAY_WIDTH];

        memset(layers, 0, sizeof(layers));
        for (int bg = 0; bg < 3; bg++)
            RefRenderBGScanline(snap, bg, y, layers[bg]);
        for (int x = 0; x < DISPLAY_WIDTH; x++)
        {
            u16 color = backdrop;
            u8 winner = 3;

            for (int bg = 0; bg < 3; bg++)
            {
                if (layers[bg][x] & 0x8000)
                {
                    color = layers[bg][x];
                    winner = (u8)bg;
                    break;
                }
            }
            out[y * DISPLAY_WIDTH + x] = color;
            winnerOut[y * DISPLAY_WIDTH + x] = winner;
        }
    }
}

/* ---- Parity runner + diagnostics -------------------------------------- */

struct ParityResult
{
    long total;
    long mismatches;
    long layerSelectMismatches;
    long sameLayerMismatches;
    long backdropMismatches;
    int firstX;
    int firstY;
    u16 firstNative;
    u16 firstRef;
    u8 firstNativeWinner;
    u8 firstRefWinner;
};

static u8 NativeWinner(const struct NativeOverworldSnapshot *snap, s32 wx, s32 wy)
{
    u16 entries[3];
    u16 color;
    bool32 ringWon;
    s32 originX;
    s32 originY;
    s32 sx;
    s32 sy;
    int bg;

    /* Mirror the renderer's CompositeMapPixel exactly: a ring-won tile samples
     * its char sub-pixel at the presentation scroll ((bgHofs[bg]+sx)&7), a
     * grid/border-won tile at the logical world position. Mixing the two would
     * mis-classify which layer owns a pixel on a moving frame. */
    originX = snap->cameraMapX * 16 + snap->cameraX;
    originY = snap->cameraMapY * 16 + snap->cameraY;
    sx = wx - originX;
    sy = wy - originY;
    ringWon = ResolveBgTileEntries(snap, wx, wy, entries);
    for (bg = 0; bg < 3; bg++)
    {
        s32 sampleX = ringWon ? (snap->bgHofs[bg] + sx) : wx;
        s32 sampleY = ringWon ? (snap->bgVofs[bg] + sy) : wy;

        if (SampleSnapshotTile(snap, entries[bg], sampleX, sampleY, &color))
            return (u8)bg;
    }
    return 3;
}

static struct ParityResult RunParity(void)
{
    struct NativeOverworldSnapshot snap;
    struct ParityResult r;
    static u16 nativeFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 refFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u8 refWinner[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    s32 originX;
    s32 originY;

    memset(&r, 0, sizeof(r));
    assert(NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_NONE);
    assert(NativeOverworldRenderer_DrawMapFrame(&snap, nativeFrame));
    RefRenderMap(&snap, refFrame, refWinner);

    originX = snap.cameraMapX * 16 + snap.cameraX;
    originY = snap.cameraMapY * 16 + snap.cameraY;
    r.total = DISPLAY_WIDTH * DISPLAY_HEIGHT;
    for (int y = 0; y < DISPLAY_HEIGHT; y++)
    {
        for (int x = 0; x < DISPLAY_WIDTH; x++)
        {
            int idx = y * DISPLAY_WIDTH + x;

            if (nativeFrame[idx] != refFrame[idx])
            {
                u8 nw = NativeWinner(&snap, originX + x, originY + y);

                if (r.mismatches == 0)
                {
                    r.firstX = x;
                    r.firstY = y;
                    r.firstNative = nativeFrame[idx];
                    r.firstRef = refFrame[idx];
                    r.firstNativeWinner = nw;
                    r.firstRefWinner = refWinner[idx];
                }
                r.mismatches++;
                if (nw == refWinner[idx])
                    r.sameLayerMismatches++;
                else if (nw == 3 || refWinner[idx] == 3)
                    r.backdropMismatches++;
                else
                    r.layerSelectMismatches++;
            }
        }
    }
    return r;
}

static const char *WinnerName(u8 w)
{
    switch (w)
    {
    case 0: return "BG1";
    case 1: return "BG2";
    case 2: return "BG3";
    default: return "backdrop";
    }
}

/* ASCII row diff: '.' match, 'X' color mismatch, 'L' layer-select mismatch. */
static void PrintDiff(const struct NativeOverworldSnapshot *snap,
                      const struct ParityResult *r)
{
    static u16 nativeFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 refFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u8 refWinner[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u8 winRef[DISPLAY_WIDTH];
    s32 originX = snap->cameraMapX * 16 + snap->cameraX;
    s32 originY = snap->cameraMapY * 16 + snap->cameraY;

    NativeOverworldRenderer_DrawMapFrame(snap, nativeFrame);
    RefRenderMap(snap, refFrame, refWinner);
    for (int y = 0; y < DISPLAY_HEIGHT; y++)
    {
        for (int x = 0; x < DISPLAY_WIDTH; x++)
            winRef[x] = refWinner[y * DISPLAY_WIDTH + x];
        printf("%03d ", y);
        for (int x = 0; x < DISPLAY_WIDTH; x++)
        {
            int idx = y * DISPLAY_WIDTH + x;
            char c = '.';

            if (nativeFrame[idx] != refFrame[idx])
            {
                u8 nw = NativeWinner(snap, originX + x, originY + y);

                c = (nw == winRef[x]) ? 'X' : 'L';
            }
            putchar(c);
        }
        putchar('\n');
    }
    printf("first mismatch at (%d,%d): native=0x%04X (%s) ref=0x%04X (%s)\n",
           r->firstX, r->firstY, r->firstNative,
           WinnerName(r->firstNativeWinner), r->firstRef,
           WinnerName(r->firstRefWinner));
}

static void ReportParity(const char *name, struct ParityResult r)
{
    double density = (double)r.mismatches * 100.0 / (double)r.total;

    printf("%-24s total=%ld mismatches=%ld (%.2f%%)  layer-select=%ld "
           "same-layer-color=%ld backdrop=%ld\n",
           name, r.total, r.mismatches, density, r.layerSelectMismatches,
           r.sameLayerMismatches, r.backdropMismatches);
}

static void AssertParity(const char *name)
{
    struct ParityResult r = RunParity();
    struct NativeOverworldSnapshot snap;

    ReportParity(name, r);
    if (r.mismatches != 0)
    {
        assert(NativeOverworld_CaptureSnapshot(&snap));
        PrintDiff(&snap, &r);
    }
    assert(r.mismatches == 0);
}

/* ---- Tile-provider precedence unit checks ----------------------------- */

/* Precedence 1 (ring) vs precedence 2 (grid): in a clean scene the live ring,
 * drawn game-style from the grid, must equal the grid/metatile reconstruction
 * for every 8x8 tile in the 240x160 window. */
static void TestRingMatchesGridReconstruction(void)
{
    struct NativeOverworldSnapshot snap;

    SceneReset(16, 16);
    for (int my = 0; my < 16; my++)
        for (int mx = 0; mx < 16; mx++)
            SceneSetMapMetatile(mx, my, (mx + my) % 4);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    assert(NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_NONE);

    /* Assert ring==grid only inside the ring's world residency window
     * [cameraMapX*16, cameraMapX*16+256) x [cameraMapY*16, +256) -- the 256x256
     * world-pixel span the game's ring-writer actually maintains. Outside that
     * span the ring torus aliases unrelated world content, so the two paths may
     * differ (and rendered pixels never leave the residency window for normal
     * pans). */
    for (s32 wy = 15 * 16; wy < 15 * 16 + 256; wy += 8)
    {
        for (s32 wx = 15 * 16; wx < 15 * 16 + 256; wx += 8)
        {
            u16 ringEntries[3];
            u16 gridEntries[3];

            ResolveBgTileEntries(&snap, wx, wy, ringEntries);
            gridEntries[0] = GetMetatileTileEntryFromSnapshot(&snap, 1, wx, wy);
            gridEntries[1] = GetMetatileTileEntryFromSnapshot(&snap, 2, wx, wy);
            gridEntries[2] = GetMetatileTileEntryFromSnapshot(&snap, 3, wx, wy);
            for (int bg = 0; bg < 3; bg++)
                assert(ringEntries[bg] == gridEntries[bg]);
        }
    }
    /* The 240x160 window, including sub-tile pans, always lies inside the
     * residency window, so rendered pixels always take precedence 1 (the ring);
     * a coordinate one pixel west of the residency window is NOT resident. */
    assert(IsWorldCoordinateResidentInRing(&snap, 15 * 16, 15 * 16));
    assert(IsWorldCoordinateResidentInRing(&snap, 15 * 16 + 239, 15 * 16 + 159));
    assert(!IsWorldCoordinateResidentInRing(&snap, 15 * 16 - 1, 15 * 16));
    assert(!IsWorldCoordinateResidentInRing(&snap, 15 * 16 + 256, 15 * 16));
    printf("%-24s ok\n", "ring == grid reconstruction");
}

/* Precedence 2 (grid) and 4 (border): outside the map the backup grid still
 * returns border blocks; out-of-grid coords fall back to the repeating 2x2
 * border with no hardcoded map identity. */
static void TestGridAndBorderFallback(void)
{
    struct NativeOverworldSnapshot snap;

    SceneReset(8, 8);
    for (int my = 0; my < 8; my++)
        for (int mx = 0; mx < 8; mx++)
            SceneSetMapMetatile(mx, my, 2);
    sScene.border[0] = 0x03;
    sScene.border[1] = 0x04;
    sScene.border[2] = 0x05;
    sScene.border[3] = 0x06;
    SetCamera(7, 7, 0, 0);
    DrawSceneRing(7, 7);
    PublishSceneTilemaps();
    assert(NativeOverworld_CaptureSnapshot(&snap));

    /* Map occupies grid [7,15) for the 8x8 layout. Inside the map: grid
     * metatile. Grid coords (gx,gy) are world/grid metatile coords. */
    assert(GetSnapshotMetatileId(&snap, 7, 7) == 2);
    assert(GetSnapshotMetatileId(&snap, 8, 8) == 2);
    assert(GetSnapshotMetatileId(&snap, 14, 14) == 2);
    /* Outside the map, still inside the grid: MAPGRID_UNDEFINED -> border. */
    assert(GetSnapshotMetatileId(&snap, 6, 6) == sScene.border[3]);
    assert(GetSnapshotMetatileId(&snap, 15, 15) == sScene.border[0]);
    assert(GetSnapshotMetatileId(&snap, 16, 15) == sScene.border[1]);
    assert(GetSnapshotMetatileId(&snap, 15, 16) == sScene.border[2]);
    assert(GetSnapshotMetatileId(&snap, 16, 16) == sScene.border[3]);
    /* Far outside the grid: repeating 2x2 border. */
    assert(GetSnapshotMetatileId(&snap, -1, -1) == sScene.border[0]);
    assert(GetSnapshotMetatileId(&snap, 40, 40) == sScene.border[3]);
    assert(GetSnapshotMetatileId(&snap, 100, -3) == sScene.border[1]);
    printf("%-24s ok\n", "grid + border fallback");
}

/* The negative control: the harness must detect divergence between the ring
 * (what the native reads) and the VRAM tilemap (what DrawFrame reads). Since
 * the Stage 3C transitional-tilemap fix, a ring/VRAM divergence mid-frame is
 * caught at CAPTURE time with the precise NATIVE_FALLBACK_BG_TILEMAP_IN_FLIGHT
 * reason -- the frame is a tilemap-in-flight the native would otherwise
 * present differently from the GBA -- instead of surfacing later as an opaque
 * comparison mismatch. A coherent frame still passes the harness clean. */
static void TestHarnessDetectsMismatch(void)
{
    struct NativeOverworldSnapshot snap;
    struct ParityResult r;

    SceneReset(16, 16);
    for (int my = 0; my < 16; my++)
        for (int mx = 0; mx < 16; mx++)
            SceneSetMapMetatile(mx, my, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();

    r = RunParity();
    ReportParity("clean (control)", r);
    assert(r.mismatches == 0);

    /* Corrupt one VRAM tilemap entry the ring does not know about: the visible
     * ring tile (15,15) (BG1 index 15*32+15 == 495) still holds the scene's
     * 0x0000 entry while the live screenbase carries TILE_ENTRY(999,0,0,0). The
     * capture-time detector must reject the divergent frame with the named
     * fallback rather than let it reach the comparison. */
    assert(NativeOverworld_CaptureSnapshot(&snap));
    ((u16 *)(VRAM_ + 0x800 * 29))[15 * 32 + 15] = TILE_ENTRY(999, 0, 0, 0);
    memset(&snap, 0, sizeof(snap));
    assert(!NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_BG_TILEMAP_IN_FLIGHT);
    assert(snap.requiredCapabilities == 0);
    printf("%-24s capture rejects divergence\n", "harness negative control");
}

/* ---- Required test scenes --------------------------------------------- */

static void SetupCommonMetatiles(void)
{
    /* 0: NORMAL grass -- BG2 bottom tiles, BG1 top tiles (top covers OBJ). */
    SetMetatileContent(0, 0, METATILE_LAYER_TYPE_NORMAL,
                       TILE_ENTRY(10, 0, 0, 0), TILE_ENTRY(11, 0, 0, 0),
                       TILE_ENTRY(12, 0, 0, 0), TILE_ENTRY(13, 0, 0, 0),
                       TILE_ENTRY(20, 0, 0, 0), TILE_ENTRY(21, 0, 0, 0),
                       TILE_ENTRY(22, 0, 0, 0), TILE_ENTRY(23, 0, 0, 0));
    /* 1: animated water -- NORMAL bottom layer with an H-flipped tile. */
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
            case 0: /* uniform grass */
                id = 0;
                break;
            case 1: /* water lake in the middle */
                id = (mx >= 6 && mx < 10 && my >= 6 && my < 10) ? 1 : 0;
                break;
            case 2: /* cave everywhere */
                id = 2;
                break;
            case 3: /* building: floor SPLIT, outer ring NORMAL */
                id = (mx == 0 || my == 0 || mx == layoutW - 1 || my == layoutH - 1) ? 3 : 0;
                break;
            default:
                id = 0;
                break;
            }
            SceneSetMapMetatile(mx, my, id);
        }
}

static void TestParityOutdoor(void)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertParity("outdoor map");

    /* Independent spot check, not via any shared helper: camera origin is
     * (15*16, 15*16), screen pixel (0,0) is world pixel (240,240), grid
     * metatile (15,15) which is map metatile 0 (NORMAL grass). BG1 top tile =
     * tile 20 palette 0; FillTileData gives tile 20 pixel(0,0) a low nibble of
     * 1, so the pixel must be palette[1] (== PaletteColor(0,1)) with bit15 set. */
    {
        struct NativeOverworldSnapshot snap;
        static u16 nativeFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];

        assert(NativeOverworld_CaptureSnapshot(&snap));
        assert(NativeOverworldRenderer_DrawMapFrame(&snap, nativeFrame));
        assert(nativeFrame[0] == (PaletteColor(0, 1) | 0x8000));
    }
}

static void TestParityAnimatedWater(void)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 1);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertParity("animated water");
}

static void TestParityCave(void)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 2);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertParity("cave (covered)");
}

static void TestParityIndoorBuilding(void)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 3);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertParity("indoor building (split)");
}

static void TestParityPokemonCenter(void)
{
    SceneReset(12, 10);
    SetupCommonMetatiles();
    /* Interior with secondary-tileset floor and counter SPLIT blocks. */
    for (s32 my = 0; my < 10; my++)
        for (s32 mx = 0; mx < 12; mx++)
        {
            if (mx == 5 && my == 5)
                SceneSetMapMetatile(mx, my, 0x200); /* secondary 0 */
            else if (mx == 5 && my == 6)
                SceneSetMapMetatile(mx, my, 0x201); /* secondary 1 */
            else
                SceneSetMapMetatile(mx, my, 0);
        }
    SetCamera(11, 9, 0, 0);
    DrawSceneRing(11, 9);
    PublishSceneTilemaps();
    AssertParity("pokemon center");
}

static void TestParityMapBorder(void)
{
    /* Camera at the map's top-left corner: the window extends past the map
     * into the repeating border. Use a SPLIT metatile for the border so it is
     * visibly distinct from the grass map. */
    SceneReset(8, 8);
    SetupCommonMetatiles();
    FillMapGrid(8, 8, 0);
    sScene.border[0] = 3;
    sScene.border[1] = 3;
    sScene.border[2] = 3;
    sScene.border[3] = 3;
    SetCamera(7, 7, 0, 0);
    DrawSceneRing(7, 7);
    PublishSceneTilemaps();
    AssertParity("map border");
}

static void TestParityCardinalConnection(void)
{
    /* A 16x16 map with an eastward connection strip baked into the backup
     * grid at grid coords [23,27), exactly as Emerald copies incoming
     * connection data next to the map (which occupies grid [7,23)). Camera
     * near the east edge (grid 21) shows the connection metatiles. */
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    for (s32 gy = 7; gy < 23; gy++)
        for (s32 gx = 23; gx < 27; gx++)
            sScene.grid[gx + (16 + MAP_OFFSET_W) * gy] = 1;
    SetCamera(21, 15, 0, 0);
    DrawSceneRing(21, 15);
    PublishSceneTilemaps();
    AssertParity("cardinal connection");
}

static void TestParityTransientTilemapWrite(void)
{
    /* A door/transient write: the ring (and its VRAM copy) carries a tile the
     * backup grid never knew about. Precedence 1 must win and parity must hold
     * because both the native ring read and the DrawFrame VRAM read see it. */
    struct NativeOverworldSnapshot snap;
    u16 ringEntries[3];
    u16 gridEntries[3];
    s32 wx;
    s32 wy;

    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    /* Open the door: overwrite ring tile (0,0) -- the BG1 tile of metatile
     * (15,15) quadrant 0 at world pixel (240,240) -- with a "door open" tile
     * that the backup grid never contained. */
    sScene.ring[0][0] = TILE_ENTRY(999, 0, 0, 0);
    PublishSceneTilemaps();
    assert(NativeOverworld_CaptureSnapshot(&snap));

    /* Precedence 1 (ring) differs from precedence 2 (grid) at this tile. */
    wx = 15 * 16 + 0;
    wy = 15 * 16 + 0;
    ResolveBgTileEntries(&snap, wx, wy, ringEntries);
    gridEntries[0] = GetMetatileTileEntryFromSnapshot(&snap, 1, wx, wy);
    assert(ringEntries[0] == TILE_ENTRY(999, 0, 0, 0));
    assert(gridEntries[0] != TILE_ENTRY(999, 0, 0, 0));

    AssertParity("door/transient tilemap write");
}

static void TestParityRuntimeMetatile(void)
{
    /* A field effect rewrites a metatile's content at runtime. The game redraws
     * the affected ring metatiles from the modified tables; the native renderer
     * must reflect the new content (via precedence 1) rather than a stale frame,
     * and parity with the DrawFrame VRAM read must hold. */
    struct NativeOverworldSnapshot snap;
    static u16 beforeFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 afterFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    bool32 differ = FALSE;
    int i;

    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0); /* uniform grass = metatile 0 */
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertParity("runtime metatile (before)");
    assert(NativeOverworld_CaptureSnapshot(&snap));
    assert(NativeOverworldRenderer_DrawMapFrame(&snap, beforeFrame));

    /* Runtime rewrite: metatile 0 goes NORMAL grass -> SPLIT with different
     * tiles. Redraw the ring from the modified tables, as the game does. */
    SetMetatileContent(0, 0, METATILE_LAYER_TYPE_SPLIT,
                       TILE_ENTRY(500, 0, 0, 6), TILE_ENTRY(501, 0, 0, 6),
                       TILE_ENTRY(502, 0, 0, 6), TILE_ENTRY(503, 0, 0, 6),
                       TILE_ENTRY(504, 0, 0, 6), TILE_ENTRY(505, 0, 0, 6),
                       TILE_ENTRY(506, 0, 0, 6), TILE_ENTRY(507, 0, 0, 6));
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertParity("runtime metatile (after)");
    assert(NativeOverworld_CaptureSnapshot(&snap));
    assert(NativeOverworldRenderer_DrawMapFrame(&snap, afterFrame));

    for (i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        if (beforeFrame[i] != afterFrame[i])
        {
            differ = TRUE;
            break;
        }
    }
    assert(differ);
    printf("%-24s frame changed after metatile rewrite\n", "runtime metatile");
}

static void TestParityCameraPan(void)
{
    /* Non-zero and negative sub-tile camera offsets must produce identical
     * output from the native sampler and the DrawFrame VRAM sampler. */
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 1);
    SetCamera(15, 15, 3, 5);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertParity("camera pan +3,+5");

    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 1);
    SetCamera(15, 15, -4, 2);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertParity("camera pan -4,+2");
}

/* Build a scene for one moving-frame parity case (A-K below). The ring is
 * drawn around the CURRENT camera map position, as DrawWholeMapViewInternal
 * does every frame; the scroll registers are set independently to the previous
 * frame's latched offsets, exactly as FieldUpdateBgTilemapScroll leaves them on
 * a moving frame. Pattern 1 (water lake) gives the window spatially varied
 * metatiles so a ring-tile or sub-tile sampling error is visible. */
static void MovingFrameScene(s32 cameraMapX, s32 cameraMapY, s16 camX, s16 camY,
                             s16 hofs, s16 vofs)
{
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 1);
    SetCameraScrolled(cameraMapX, cameraMapY, camX, camY, hofs, vofs);
    DrawSceneRing(cameraMapX, cameraMapY);
    PublishSceneTilemaps();
}

/* Directive 6: synthetic moving-frame cases A-K. The GBA presents the ring at
 * the latched scroll registers, which lag the logical camera by one frame of
 * motion on a moving frame; the native renderer must reproduce exactly that.
 * Each case sets scroll != camera and asserts pixel-exact parity against the
 * reference (which samples the VRAM tilemap at the scroll). */
static void TestMovingFrameParity(void)
{
    /* A: stationary baseline -- scroll == camera. */
    MovingFrameScene(15, 15, 0, 0, 0, 0);
    AssertParity("A stationary baseline");

    /* B: +1px horizontal movement -- camera advanced 1px, scroll latched at 0. */
    MovingFrameScene(15, 15, 1, 0, 0, 0);
    AssertParity("B +1px horizontal");

    /* C: +1px vertical movement. */
    MovingFrameScene(15, 15, 0, 1, 0, 0);
    AssertParity("C +1px vertical");

    /* D: diagonal +1,+1. */
    MovingFrameScene(15, 15, 1, 1, 0, 0);
    AssertParity("D diagonal");

    /* E: sub-tile 0..7 -- every 8px scroll phase with the camera one ahead. */
    for (int d = 0; d < 8; d++)
    {
        char name[32];

        snprintf(name, sizeof(name), "E sub-tile %d", d);
        MovingFrameScene(15, 15, (s16)(d + 1), (s16)(d + 1), (s16)d, (s16)d);
        AssertParity(name);
    }

    /* F: 8px tile boundary -- scroll just before the tile boundary, camera on
     * it, so the presented ring crosses an 8px tile edge mid-window. */
    MovingFrameScene(15, 15, 8, 0, 7, 0);
    AssertParity("F 8px tile boundary H");
    MovingFrameScene(15, 15, 0, 8, 0, 7);
    AssertParity("F 8px tile boundary V");

    /* G: 16px metatile boundary -- scroll one pixel behind the boundary, camera
     * at it. The ring metatile edge falls inside the presented window. */
    MovingFrameScene(15, 15, 16, 16, 15, 15);
    AssertParity("G 16px metatile boundary");

    /* H: ring wrap 255->0 -- scroll at 0xFF with camera at 0: the presented
     * window spans ring col 255 then wraps to col 0 (torus seam). */
    MovingFrameScene(15, 15, 0, 0, 0xFF, 0xFF);
    AssertParity("H ring wrap 255->0");

    /* I: ring wrap 0->255 reverse -- scroll at 0x1FF (-1 in 9-bit) with camera
     * moved left (cameraX 15). Exercises the 9-bit scroll masking and the seam
     * from the opposite direction. */
    MovingFrameScene(15, 15, 15, 15, 0x1FF, 0x1FF);
    AssertParity("I ring wrap 0->255 reverse");

    /* J: camera pan/shake -- several-pixel offsets and a negative pan. */
    MovingFrameScene(15, 15, 5, 3, 3, 2);
    AssertParity("J camera pan +2,+1");
    MovingFrameScene(15, 15, -2, 4, -3, 3);
    AssertParity("J camera shake -1,+1");

    /* K: map-connection edge -- camera at the east edge of the map next to a
     * cardinal connection strip (grid [23,27)), moving one frame; the window
     * samples connection/border metatiles. */
    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    for (s32 gy = 7; gy < 23; gy++)
        for (s32 gx = 23; gx < 27; gx++)
            sScene.grid[gx + (16 + MAP_OFFSET_W) * gy] = 1;
    SetCameraScrolled(21, 15, 1, 0, 0, 0);
    DrawSceneRing(21, 15);
    PublishSceneTilemaps();
    AssertParity("K map-connection edge");
}

static void TestParityMapTransition(void)
{
    /* An ordinary map transition: two different maps in one session. Both must
     * render with parity and their outputs must actually differ. */
    static u16 firstFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static u16 secondFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    struct NativeOverworldSnapshot snap;
    bool32 differ = FALSE;

    SceneReset(16, 16);
    SetupCommonMetatiles();
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    AssertParity("map transition (map A)");
    assert(NativeOverworld_CaptureSnapshot(&snap));
    assert(NativeOverworldRenderer_DrawMapFrame(&snap, firstFrame));

    SceneReset(12, 10);
    SetupCommonMetatiles();
    FillMapGrid(12, 10, 3);
    SetCamera(11, 9, 0, 0);
    DrawSceneRing(11, 9);
    PublishSceneTilemaps();
    AssertParity("map transition (map B)");
    assert(NativeOverworld_CaptureSnapshot(&snap));
    assert(NativeOverworldRenderer_DrawMapFrame(&snap, secondFrame));

    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        if (firstFrame[i] != secondFrame[i])
        {
            differ = TRUE;
            break;
        }
    }
    assert(differ);
    printf("%-24s frames differ across transition\n", "map transition");
}

/* ---- Fallback conditions ---------------------------------------------- */

static void TestFallbackReasons(void)
{
    struct NativeOverworldSnapshot snap;

    SetupCommonMetatiles();

    /* Not an eligible overworld scene. */
    SceneReset(16, 16);
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    sRendererReady = FALSE;
    assert(!NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_SCENE_NOT_OVERWORLD);
    sRendererReady = TRUE;

    /* Missing layout. */
    SceneReset(16, 16);
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    gMapHeader.mapLayout = NULL;
    assert(!NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_MISSING_LAYOUT);
    gMapHeader.mapLayout = &sScene.layout;

    /* Missing tilesets. */
    SceneReset(16, 16);
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    sScene.primaryTileset.metatiles = NULL;
    assert(!NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_MISSING_TILESETS);

    /* Missing backup map grid. */
    SceneReset(16, 16);
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    gBackupMapLayout.map = NULL;
    assert(!NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_MISSING_MAP_GRID);
    gBackupMapLayout.map = sScene.grid;

    /* Missing live BG ring. */
    SceneReset(16, 16);
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    gOverworldTilemapBuffer_Bg1 = NULL;
    assert(!NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_MISSING_BG_RING);
    gOverworldTilemapBuffer_Bg1 = sScene.ring[0];

    /* Wrong display mode. */
    SceneReset(16, 16);
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    REG_DISPCNT = (REG_DISPCNT & ~7) | 1; /* mode 1 */
    assert(!NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_DISPLAY_MODE);

    /* Display layers: a map layer missing still rejects... */
    SceneReset(16, 16);
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    REG_DISPCNT &= ~DISPCNT_BG1_ON;
    assert(!NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_DISPLAY_LAYERS);

    /* ...but BG0 enabled (the field's real ShowBg(0..3) setup) is legal and
     * must capture. This is the Stage-2 regression: the gate previously required
     * BG0 OFF and rejected every real field frame. */
    SceneReset(16, 16);
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    REG_DISPCNT |= DISPCNT_BG0_ON;
    assert(NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_NONE);

    /* ...but BG0 with visible UI content (field message box / text window) must
     * fall back: the oracle renders it, the native map renderer does not. The
     * stage-2 runtime mismatches (frames 1131-1138) were all BG0-overlay frames.
     * An opaque 8x8 tile in BG0's screen block is enough. */
    SceneReset(16, 16);
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    REG_DISPCNT |= DISPCNT_BG0_ON;
    {
        u16 *screenBlock31 = (u16 *)(VRAM_ + 0xF800);
        u8 *charBase2 = VRAM_ + 0x8000;

        screenBlock31[0] = 0x0001;              /* tile 1, palette 0 at screen (0,0) */
        charBase2[1 * 32 + 4] = 0x0F;           /* opaque nibble in tile 1 */
    }
    assert(!NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_BG0_OVERLAY);

    /* Unexpected BG control configuration. */
    SceneReset(16, 16);
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    REG_BG2CNT = BGCNT_PRIORITY(2) | BGCNT_CHARBASE(1) | BGCNT_SCREENBASE(28)
               | BGCNT_16COLOR | BGCNT_TXT256x256;
    assert(!NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_BG_CONFIG);

    /* Internally inconsistent scroll latch: BG1 differs from BG2/BG3 (only one
     * layer's HOFS is disturbed). The field's VBlank latch writes ONE value to
     * all three layers, so a frame whose scrolls disagree has a torn
     * presentation and must fall back. A UNIFORM scroll that merely differs
     * from the logical camera is a NORMAL moving frame and must NOT fall back
     * (covered by TestMovingFrameParity, which asserts capture succeeds). */
    SceneReset(16, 16);
    FillMapGrid(16, 16, 0);
    SetCamera(15, 15, 0, 0);
    DrawSceneRing(15, 15);
    PublishSceneTilemaps();
    REG_BG1HOFS = 1;
    assert(!NativeOverworld_CaptureSnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_BG_SCROLL);

    printf("%-24s fallback reasons reported\n", "fallback conditions");
}

/* ---- Blend capability predicate (Stage-2 regression) ------------------- */

/* The field's persistent blend config must be allowed: BLDCNT=0x1E40
 * (TGT2=BG1|BG2|BG3|OBJ | EFFECT_BLEND) has an EMPTY TGT1, so the alpha blend
 * never fires and the BG-only oracle renders map pixels unblended. This is the
 * exact state InitOverworldGraphicsRegisters installs at every map load; the
 * old gate's blanket REG_BLDCNT != 0 check rejected every real field frame. */
static void TestBlendPredicate(void)
{
    /* The persistent normal-runtime field blend state. */
    assert(!NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3
               | BLDCNT_TGT2_OBJ | BLDCNT_EFFECT_BLEND, /* 0x1E40 */
               0, BLDALPHA_BLEND(13, 7)));
    /* Effect is alpha, so a stale nonzero BLDY is irrelevant. */
    assert(!NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3
               | BLDCNT_TGT2_OBJ | BLDCNT_EFFECT_BLEND,
               16, BLDALPHA_BLEND(13, 7)));

    /* No blend configured at all. */
    assert(!NativeOverworld_BlendAffectsMapBackground(0, 0, 0));
    /* EFFECT_BLEND with no targets in either group. */
    assert(!NativeOverworld_BlendAffectsMapBackground(BLDCNT_EFFECT_BLEND, 0, 0));
    /* EFFECT_NONE with targets present: flags but no effect. */
    assert(!NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT2_BG1 | BLDCNT_TGT1_BG1 | BLDCNT_EFFECT_NONE, 16, 0));

    /* Genuine map-affecting alpha configs must be rejected: a map layer in TGT1
     * with a rendered TGT2 target below it. */
    assert(NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT1_BG1 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG2,
               0, BLDALPHA_BLEND(13, 7)));
    assert(NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT1_BG3 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BD,
               0, BLDALPHA_BLEND(13, 7)));

    /* OBJ is excluded from the oracle, so OBJ-only TGT2 can never fire even
     * with a map layer in TGT1; and EVA=16/EVB=0 reproduces target A exactly. */
    assert(!NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT1_BG1 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_OBJ,
               0, BLDALPHA_BLEND(13, 7)));
    assert(!NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT1_BG1 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG2,
               0, BLDALPHA_BLEND(16, 0)));

    /* Brightness: BLDY==0 is a no-op; BLDY!=0 on a TGT1 map/backdrop layer is a
     * genuine reject; OBJ-only TGT1 is masked. */
    assert(!NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT1_ALL | BLDCNT_EFFECT_LIGHTEN, 0, 0));
    assert(!NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT1_OBJ | BLDCNT_EFFECT_DARKEN, 16, 0));
    assert(NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT1_BG1 | BLDCNT_EFFECT_LIGHTEN, 16, 0));
    assert(NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT1_BD | BLDCNT_EFFECT_DARKEN, 16, 0));

    /* The transient overworld blend effects: Rayquaza spotlight (TGT1_BG0 +
     * TGT2 map) and the orb effect (TGT1_BG{layer} + TGT2 map) both genuinely
     * change map pixels and must be rejected. */
    assert(NativeOverworld_BlendAffectsMapBackground(
               BLDCNT_TGT1_BG0 | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1
               | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_OBJ
               | BLDCNT_TGT2_BD,
               0, BLDALPHA_BLEND(14, 14)));

    printf("%-24s blend capability predicate\n", "blend predicate");
}

int main(void)
{
    FillTileData();
    FillPalette();

    TestRingMatchesGridReconstruction();
    TestGridAndBorderFallback();
    TestHarnessDetectsMismatch();

    TestParityOutdoor();
    TestParityAnimatedWater();
    TestParityCave();
    TestParityIndoorBuilding();
    TestParityPokemonCenter();
    TestParityMapBorder();
    TestParityCardinalConnection();
    TestParityTransientTilemapWrite();
    TestParityRuntimeMetatile();
    TestParityCameraPan();
    TestMovingFrameParity();
    TestParityMapTransition();

    TestFallbackReasons();
    TestBlendPredicate();

    printf("native overworld parity harness passed\n");
    return 0;
}

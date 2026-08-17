/*
 * Stage 3C transitional-tilemap fallback unit tests (synthetic, no fixtures).
 *
 * The 7 runtime composite-parity mismatch frames (178, 610, 614, 620, 2753,
 * 2798, 3084) share ONE root cause: the composite compared the native BG render
 * (sampled from the software ring, gOverworldTilemapBuffer_BgN) against the
 * REAL DrawFrame composite (sampled from the live VRAM screenbase tilemap) on
 * frames where the two tilemaps had diverged mid-transition:
 *
 *   - Class A (178, 614, 2753, 2798, 3084): the captured BG palette was fully
 *     black (fade) and the live screenbase had gone fully transparent, while the
 *     software ring still carried opaque entries, so the native painted BG1/2/3
 *     in black while the oracle showed backdrop.
 *   - Class B (610, 620): the ring and the screenbase disagreed on the actual
 *     tile entries (frame 610's first mismatch at (123,73) is ring index
 *     ((40+73)&0xFF)/8 * 32 + ((0+123)&0xFF)/8 == 463).
 *
 * The fix is a capture-time detector, BgRingDivergesFromScreenbase(), that
 * compares the captured ring against the live VRAM screenbase tilemaps for the
 * visible 240x160 window using the oracle's exact index formula, and falls back
 * with NATIVE_FALLBACK_BG_TILEMAP_IN_FLIGHT when they diverge -- so these frames
 * become a precise named fallback instead of opaque mismatches. These tests
 * pin the detector's math and the capture-gate wiring end to end:
 *
 *   1. a ring matching the live screenbase on the visible window is NOT flagged
 *      (normal settled frame);
 *   2. a ring diverging at a visible tile IS flagged, including the exact
 *      frame-610 index 463 reconstructed from the report (scroll voffs=40);
 *   3. a screenbase that went fully transparent while the ring stayed opaque
 *      (the Class-A fade) IS flagged;
 *   4. a divergence only off the 240x160 window (tile (31,31), index 1023) is
 *      NOT flagged (only the presented window is compared);
 *   5. end to end, NativeOverworld_CaptureParitySnapshot rejects a divergent
 *      frame with the named reason and clears requiredCapabilities, so the
 *      composite parity gate (fallbackReason != NATIVE_FALLBACK_NONE) never
 *      compares it -- and accepts an identical but settled frame.
 */
#include <assert.h>
#include <string.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"

#undef NDEBUG
#include <assert.h>

/* ---- Stubs of the hardware/game globals the renderer TUs reference ---- */
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

struct MapHeader gMapHeader;
static struct SaveBlock1 sSaveBlock1;
struct SaveBlock1 *gSaveBlock1Ptr = &sSaveBlock1;
struct BackupMapLayout gBackupMapLayout;
static u16 sRing1[NATIVE_BG_RING_SIZE];
static u16 sRing2[NATIVE_BG_RING_SIZE];
static u16 sRing3[NATIVE_BG_RING_SIZE];
u16 *gOverworldTilemapBuffer_Bg1 = sRing1;
u16 *gOverworldTilemapBuffer_Bg2 = sRing2;
u16 *gOverworldTilemapBuffer_Bg3 = sRing3;
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

/* Ring-world-anchor tile offsets: the transition scenes all stand at tile
 * offset 0 (k=0), so the anchor is the plain camera-cell window. */
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

/* ---- Capture environment ---- */

// Field BGCNT values as captured from the runtime (screenbases 29/28/30, the
// priorities 1/2/3, charbase 0, 16-color, 256x256 the renderer requires).
#define BG1_CNT 0x1D41u
#define BG2_CNT 0x1C42u
#define BG3_CNT 0x1E43u

static struct Tileset sPrimaryTileset;
static struct Tileset sSecondaryTileset;
static struct MapLayout sLayout;
static u16 sPrimaryMetatiles[NUM_METATILES_IN_PRIMARY * NUM_TILES_PER_METATILE];
static u16 sSecondaryMetatiles[NUM_METATILES_IN_PRIMARY * NUM_TILES_PER_METATILE];
static u16 sPrimaryAttributes[NUM_METATILES_IN_PRIMARY];
static u16 sSecondaryAttributes[NUM_METATILES_IN_PRIMARY];
static u16 sBorder[4];
static u16 sGridMap[1];
static struct BackupMapLayout sBackup;

// Standard settled-field register setup: mode 0, BG1/2/3 on, BG0 off (so the
// BG0-content gate is skipped and the tilemap gate is the one under test),
// uniform scroll 0, camera at the origin.
static void SetupCaptureRegisters(void)
{
    memset(REG_BASE, 0, sizeof(REG_BASE));
    REG_DISPCNT = DISPCNT_BG1_ON | DISPCNT_BG2_ON | DISPCNT_BG3_ON;
    REG_BG1CNT = BG1_CNT;
    REG_BG2CNT = BG2_CNT;
    REG_BG3CNT = BG3_CNT;
    REG_BG1HOFS = 0;
    REG_BG2HOFS = 0;
    REG_BG3HOFS = 0;
    REG_BG1VOFS = 0;
    REG_BG2VOFS = 0;
    REG_BG3VOFS = 0;
    sSaveBlock1.pos.x = 1;
    sSaveBlock1.pos.y = 2;
    sCameraX = 0;
    sCameraY = 0;
}

// Minimal valid layout/grid/ring globals so CaptureSnapshotCommon passes every
// config gate and reaches the tilemap-divergence check.
static void SetupCaptureLayout(void)
{
    gMapHeader.mapLayout = &sLayout;
    sLayout.width = 40;
    sLayout.height = 40;
    sLayout.border = sBorder;
    sLayout.primaryTileset = &sPrimaryTileset;
    sLayout.secondaryTileset = &sSecondaryTileset;
    sPrimaryTileset.metatiles = sPrimaryMetatiles;
    sPrimaryTileset.metatileAttributes = sPrimaryAttributes;
    sSecondaryTileset.metatiles = sSecondaryMetatiles;
    sSecondaryTileset.metatileAttributes = sSecondaryAttributes;

    gBackupMapLayout = sBackup;
    gBackupMapLayout.map = sGridMap;
    gBackupMapLayout.width = 1;
    gBackupMapLayout.height = 1;
}

static const u16 *ScreenbasePtr(u8 bg)
{
    const u16 *const vram16 = (const u16 *)VRAM_;
    u16 cnt = (bg == 0) ? BG1_CNT : (bg == 1) ? BG2_CNT : BG3_CNT;

    return vram16 + ((cnt >> 8) & 0x1F) * 0x400;
}

// Fill ring[bg] and the matching screenbase with `entry` for every ring index.
static void FillLayer(u8 bg, u16 entry)
{
    u16 *ring;
    u16 *const screenbase = (u16 *)ScreenbasePtr(bg);
    u32 i;

    switch (bg)
    {
    case 0: ring = gOverworldTilemapBuffer_Bg1; break;
    case 1: ring = gOverworldTilemapBuffer_Bg2; break;
    default: ring = gOverworldTilemapBuffer_Bg3; break;
    }
    for (i = 0; i < NATIVE_BG_RING_SIZE; i++)
    {
        ring[i] = entry;
        screenbase[i] = entry;
    }
}

// A pre-built snapshot whose bgCnt/bgHofs/bgVofs/bgRing mirror what
// CaptureSnapshotCommon would capture from the current globals/registers.
static void BuildSnapshotLikeCapture(struct NativeOverworldSnapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->bgCnt[0] = REG_BG1CNT;
    snap->bgCnt[1] = REG_BG2CNT;
    snap->bgCnt[2] = REG_BG3CNT;
    snap->bgHofs[0] = REG_BG1HOFS;
    snap->bgHofs[1] = REG_BG2HOFS;
    snap->bgHofs[2] = REG_BG3HOFS;
    snap->bgVofs[0] = REG_BG1VOFS;
    snap->bgVofs[1] = REG_BG2VOFS;
    snap->bgVofs[2] = REG_BG3VOFS;
    memcpy(snap->bgRing[0], gOverworldTilemapBuffer_Bg1, NATIVE_BG_RING_SIZE * sizeof(u16));
    memcpy(snap->bgRing[1], gOverworldTilemapBuffer_Bg2, NATIVE_BG_RING_SIZE * sizeof(u16));
    memcpy(snap->bgRing[2], gOverworldTilemapBuffer_Bg3, NATIVE_BG_RING_SIZE * sizeof(u16));
}

/* ---- Detector tests (BgRingDivergesFromScreenbase) ---- */

// 1. Settled frame: ring and screenbase agree on every visible tile -> no flag.
static void TestSettledFrameNotFlagged(void)
{
    struct NativeOverworldSnapshot snap;

    SetupCaptureRegisters();
    FillLayer(0, 0x3014);
    FillLayer(1, 0x83BF);
    FillLayer(2, 0x10AF);
    BuildSnapshotLikeCapture(&snap);

    assert(!BgRingDivergesFromScreenbase(&snap));
}

// 2. Class B: ring and screenbase disagree at the exact frame-610 first-mismatch
//    tile (index 463, from scroll voffs=40 hoffs=0: tile (15,14)). Frame 610 was
//    the BG2 layer; reconstruct its scroll and tile divergence.
static void TestFrame610TileDivergenceFlagged(void)
{
    struct NativeOverworldSnapshot snap;

    SetupCaptureRegisters();
    FillLayer(1, 0x83BF);            // settled BG2: ring == screenbase
    REG_BG2VOFS = 40;                // frame 610's BG2 vertical scroll
    snap.bgVofs[1] = 40;

    // The reported first mismatch (123,73): ring index
    // ((40+73)&0xFF)/8 * 32 + ((0+123)&0xFF)/8 = 14*32 + 15 = 463.
    assert(((40 + 73) & 0xFF) / 8 * 32 + ((0 + 123) & 0xFF) / 8 == 463);

    // Settled at the new scroll: still agree -> no flag.
    BuildSnapshotLikeCapture(&snap);
    assert(!BgRingDivergesFromScreenbase(&snap));

    // Transitional: the screenbase still carries the settled entry at 463 while
    // the ring has already advanced -- the two tilemaps diverge.
    ((u16 *)ScreenbasePtr(1))[463] = 0x0000;
    assert(BgRingDivergesFromScreenbase(&snap));
}

// 3. Class A: the live screenbase went fully transparent (fade) while the ring
//    still carries opaque entries -> flagged.
static void TestFadeTransparentScreenbaseFlagged(void)
{
    struct NativeOverworldSnapshot snap;

    SetupCaptureRegisters();
    FillLayer(0, 0x3014);
    FillLayer(1, 0x83BF);
    FillLayer(2, 0x10AF);
    memset(VRAM_, 0, VRAM_SIZE);     // live screenbases all transparent
    BuildSnapshotLikeCapture(&snap);

    assert(BgRingDivergesFromScreenbase(&snap));
}

// 4. A divergence confined to an off-window ring tile (index 1023 = tile (31,31))
//    is NOT flagged: only the presented 240x160 window is compared.
static void TestOffWindowDivergenceNotFlagged(void)
{
    struct NativeOverworldSnapshot snap;

    SetupCaptureRegisters();
    FillLayer(0, 0x3014);
    FillLayer(1, 0x83BF);
    FillLayer(2, 0x10AF);
    BuildSnapshotLikeCapture(&snap);

    ((u16 *)ScreenbasePtr(2))[1023] = 0x0000;
    assert(!BgRingDivergesFromScreenbase(&snap));
}

/* ---- Capture-gate integration (NativeOverworld_CaptureParitySnapshot) ---- */

// 5. End to end: a divergent frame is rejected at capture with the named reason
//    and requiredCapabilities cleared; an identical but settled frame is
//    accepted with fallbackReason NONE.
static void TestCaptureGateRejectsTransition(void)
{
    struct NativeOverworldSnapshot snap;
    u16 *screenbase2;

    SetupCaptureRegisters();
    SetupCaptureLayout();
    FillLayer(0, 0x3014);
    FillLayer(1, 0x83BF);
    FillLayer(2, 0x10AF);

    // Settled: capture succeeds, no fallback.
    memset(&snap, 0, sizeof(snap));
    assert(NativeOverworld_CaptureParitySnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_NONE);
    assert(snap.requiredCapabilities & NATIVE_CAPABILITY_LIVE_BG_RING);

    // Transitional: one visible screenbase tile disagrees with the ring.
    screenbase2 = (u16 *)ScreenbasePtr(1);
    screenbase2[463] = 0x0000;
    memset(&snap, 0, sizeof(snap));
    assert(!NativeOverworld_CaptureParitySnapshot(&snap));
    assert(snap.fallbackReason == NATIVE_FALLBACK_BG_TILEMAP_IN_FLIGHT);
    assert(snap.requiredCapabilities == 0);
    // The composite parity gate predicate: a non-NONE fallback is never compared.
    assert(snap.fallbackReason != NATIVE_FALLBACK_NONE);
}

int main(void)
{
    TestSettledFrameNotFlagged();
    TestFrame610TileDivergenceFlagged();
    TestFadeTransparentScreenbaseFlagged();
    TestOffWindowDivergenceNotFlagged();
    TestCaptureGateRejectsTransition();
    printf("native field compositor transition (bg-tilemap-in-flight) unit test passed\n");
    return 0;
}

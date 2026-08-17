#if defined(PLATFORM_SDL2) && defined(LINUX64) && LINUX64

#include <stdio.h>
#include <stdlib.h>

#include "global.h"
#include "field_camera.h"
#include "fieldmap.h"
#include "overworld.h"
#include "platform/native_field_compositor.h"
#include "platform/native_overworld_renderer.h"

/*
 * See native_overworld_renderer.h for the full contract. In short: the GBA
 * DrawFrame oracle renders BG0/BG1/BG2/BG3 + backdrop but excludes OBJ, so a
 * blend configuration can only change map pixels when an oracle-rendered layer
 * is in TGT1 and (for alpha) a rendered TGT2 target sits below it. The field's
 * persistent BLDCNT=0x1E40 (TGT1 empty, alpha mode) therefore never fires and is
 * legal; a genuine map-affecting blend (brightness with BLDY!=0, or alpha with a
 * map layer in TGT1) is not.
 */
bool32 NativeOverworld_BlendAffectsMapBackground(u16 bldCnt, u16 bldY, u16 bldAlpha)
{
    u32 effect = (bldCnt >> 6) & 3;
    u32 eva = bldAlpha & 0x1F;
    u32 evb = (bldAlpha >> 8) & 0x1F;

    switch (effect)
    {
    case 0: // BLDCNT_EFFECT_NONE: flags present but no effect applied
        return FALSE;
    case 1: // BLDCNT_EFFECT_BLEND (alpha): top TGT1 pixel blends with a TGT2 target below
        // EVA==16/EVB==0 reproduces the TGT1 color exactly even when the blend fires.
        if (eva == 16 && evb == 0)
            return FALSE;
        // The top pixel's layer must be in TGT1 for the oracle's blend gate; only
        // oracle-rendered layers (BG0/BG1/BG2/BG3; the backdrop is never a top
        // alpha layer) can change. A TGT2 target below must be rendered too (BG0-BG3
        // or the backdrop); OBJ is excluded from the oracle and cannot fire.
        return ((bldCnt & (BLDCNT_TGT1_BG0 | BLDCNT_TGT1_BG1
                         | BLDCNT_TGT1_BG2 | BLDCNT_TGT1_BG3)) != 0)
            && ((bldCnt & (BLDCNT_TGT2_BG0 | BLDCNT_TGT2_BG1
                         | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3
                         | BLDCNT_TGT2_BD)) != 0);
    case 2: // BLDCNT_EFFECT_LIGHTEN
    case 3: // BLDCNT_EFFECT_DARKEN: top TGT1 pixel brightened/darkened by BLDY
        // BLDY==0 is a no-op on every channel. OBJ is masked. Backdrop is a valid
        // TGT1 target for these effects (the oracle fills it per scanline).
        if (bldY == 0)
            return FALSE;
        return ((bldCnt & (BLDCNT_TGT1_BG0 | BLDCNT_TGT1_BG1
                         | BLDCNT_TGT1_BG2 | BLDCNT_TGT1_BG3
                         | BLDCNT_TGT1_BD)) != 0);
    default: // reserved effect value: unknown, err on the side of rejecting
        return TRUE;
    }
}

/*
 * True when the snapshot's BG0 layer would render a visible pixel in the frame.
 *
 * BG0 is the field's UI/text layer (charBase 2, mapBase 31, 16-color): the GBA
 * oracle renders it, the native map-background renderer does not. The field keeps
 * BG0 enabled for the whole of ordinary gameplay (ShowBg(0) in
 * ResetAllBgsCoordinatesAndBgCnt), and cleared field windows leave 0xE000 tilemap
 * entries -- tile 0 with no opaque pixels -- behind, so an entry-nonzero test
 * would over-reject every clean frame. This samples the actual char tiles and
 * returns TRUE only when a screen entry references a tile with at least one
 * opaque pixel, i.e. the oracle would render a visible BG0 pixel the native frame
 * cannot reproduce (field message box, text windows).
 *
 * The whole 32x32 screen block is scanned. Field BG0 HOFS/VOFS are always zero,
 * so the visible 240x160 window is block rows 0-19 and off-screen opaque UI never
 * exists; scanning the full block errs on the side of falling back. An
 * out-of-range tile reference (cannot prove the tile empty) also falls back.
 */
bool32 NativeOverworld_SnapshotHasBg0Content(const struct NativeOverworldSnapshot *snapshot)
{
    u32 screenBaseEntries;
    u32 charBaseBytes;
    u32 tileBytes;
    const u8 *charMem;
    u32 r;
    u32 c;

    if (snapshot == NULL || !snapshot->bgVramValid)
        return TRUE; // cannot prove BG0 empty -> fall back rather than risk a mismatch

    screenBaseEntries = ((snapshot->bg0Cnt >> 8) & 0x1F) * 0x400; // u16 screen entries
    charBaseBytes = ((snapshot->bg0Cnt >> 2) & 0x3) * 0x4000;     // byte offset into BG VRAM
    tileBytes = ((snapshot->bg0Cnt >> 7) & 1) ? 64u : 32u;        // 8bpp vs 16-color
    charMem = (const u8 *)snapshot->bgVram;

    for (r = 0; r < 32; r++)
    {
        for (c = 0; c < 32; c++)
        {
            u16 entry = snapshot->bgVram[screenBaseEntries + r * 32 + c];
            u32 offset;
            u32 j;

            if (entry == 0)
                continue;
            offset = charBaseBytes + (entry & 0x3FF) * tileBytes;
            if (offset + tileBytes > NATIVE_BG_VRAM_U16 * sizeof(u16))
                return TRUE; // tile reference outside the captured VRAM: cannot prove empty
            for (j = 0; j < tileBytes; j++)
            {
                if (charMem[offset + j] != 0)
                    return TRUE;
            }
        }
    }
    return FALSE;
}

static s32 FloorDivide(s32 value, s32 divisor)
{
    if (value >= 0)
        return value / divisor;
    return -((-value + divisor - 1) / divisor);
}

static s32 PositiveModulo(s32 value, s32 divisor)
{
    s32 result = value % divisor;

    return result < 0 ? result + divisor : result;
}

static u16 MetatileTileEntryForLayer(u8 bg, const u16 *tiles, u8 layerType, u8 quadrant)
{
    // Exact split/covered/normal BG3/BG2/BG1 mapping from field_camera.c's
    // DrawMetatile. This is the pure rule both the live ring path and the
    // snapshot metatile fallback use.
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

// ---------------------------------------------------------------------------
// Stage 0/1: immutable snapshot + complete native 240x160 map background.
// This path renders every map-background pixel itself; it never reads a
// DrawFrame pixel. See native_overworld_snapshot.h for the capture contract.
// ---------------------------------------------------------------------------

static void SetMapFallback(struct NativeOverworldSnapshot *snapshot,
                           enum NativeOverworldFallbackReason reason)
{
    snapshot->fallbackReason = reason;
    snapshot->requiredCapabilities = 0;
}

/*
 * True when any BG's live VRAM screenbase tilemap diverges from the captured
 * software ring (snapshot->bgRing) for a tile the 240x160 window actually
 * presents. The GBA composite oracle (gba_easy_draw.c RenderBGScanline) samples
 * the tilemap from the live VRAM screenbase; the native renderer samples the
 * software ring the game DMA-copies into that screenbase. On every settled frame
 * the copy is already flushed (DoScheduledBgTilemapCopiesToVram ran before the
 * capture point), so the two agree entry-for-entry; during a transition (map
 * load, fade, or a tilemap DMA deferred past the capture point) the ring can be
 * one write ahead of VRAM and the composite would compare two different
 * tilemaps. Detecting that here and falling back with a precise reason turns
 * those frames from opaque mismatches into a named transitional fallback.
 *
 * The index math mirrors the oracle exactly: screen pixel (sx,sy) maps to
 * tilemap index ((voffs + sy) & 0xFF)/8 * 32 + ((hoffs + sx) & 0xFF)/8, which
 * for the renderer's 256x256 (size-0, TXT256x256) BGCNT contract is also the
 * native ring index (WorldCoordinateToPhysicalRingIndex samples at the same
 * presentation scroll). Only the visible 30x20 window is compared, never the
 * whole 32x32
 * ring, so a lazily-redrawn off-screen ring edge cannot cause a false fallback.
 */
static bool32 BgRingDivergesFromScreenbase(const struct NativeOverworldSnapshot *snapshot)
{
    const u16 *const vram16 = (const u16 *)VRAM_;
    u8 bg;

    for (bg = 0; bg < 3; bg++)
    {
        const u16 *screenbase = vram16 + ((snapshot->bgCnt[bg] >> 8) & 0x1F) * 0x400;
        s32 ty;

        for (ty = 0; ty < DISPLAY_HEIGHT / 8; ty++)
        {
            s32 mapY = ((snapshot->bgVofs[bg] + ty * 8) & 0xFF) >> 3;
            s32 tx;

            for (tx = 0; tx < DISPLAY_WIDTH / 8; tx++)
            {
                s32 mapX = ((snapshot->bgHofs[bg] + tx * 8) & 0xFF) >> 3;
                s32 index = mapY * 32 + mapX;

                if (snapshot->bgRing[bg][index] != screenbase[index])
                    return TRUE;
            }
        }
    }
    return FALSE;
}

// Shared body of the snapshot capture: the display-state checks and the bounded
// copies that both the Stage-1 display path and the Stage-2 parity path require.
// Callers run their own eligibility gate first; this body leaves
// fallbackReason == NATIVE_FALLBACK_NONE on success.
static bool32 CaptureSnapshotCommon(struct NativeOverworldSnapshot *snapshot)
{
    const struct MapLayout *layout;

    if (gSaveBlock1Ptr == NULL)
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_MISSING_CAMERA);
        return FALSE;
    }

    layout = gMapHeader.mapLayout;
    if (layout == NULL || layout->width <= 0 || layout->height <= 0
     || layout->border == NULL)
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_MISSING_LAYOUT);
        return FALSE;
    }
    if (layout->primaryTileset == NULL || layout->secondaryTileset == NULL
     || layout->primaryTileset->metatiles == NULL
     || layout->secondaryTileset->metatiles == NULL
     || layout->primaryTileset->metatileAttributes == NULL
     || layout->secondaryTileset->metatileAttributes == NULL)
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_MISSING_TILESETS);
        return FALSE;
    }
    if (gBackupMapLayout.map == NULL)
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_MISSING_MAP_GRID);
        return FALSE;
    }
    if (gOverworldTilemapBuffer_Bg1 == NULL
     || gOverworldTilemapBuffer_Bg2 == NULL
     || gOverworldTilemapBuffer_Bg3 == NULL)
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_MISSING_BG_RING);
        return FALSE;
    }
    if ((REG_DISPCNT & 7) != DISPCNT_MODE_0)
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_DISPLAY_MODE);
        return FALSE;
    }
    // The map background needs BG1/BG2/BG3 on. BG0 is also enabled by the field
    // itself (ShowBg(0) in ResetAllBgsCoordinatesAndBgCnt) and carries the field's
    // UI/window content, so it is NOT required to be off here -- that would reject
    // every real field frame. Instead, after the VRAM copy below, the BG0-content
    // gate rejects exactly the frames where BG0 renders visible pixels (message
    // box, text windows) the native map renderer does not reproduce.
    if ((REG_DISPCNT & (DISPCNT_BG1_ON | DISPCNT_BG2_ON | DISPCNT_BG3_ON))
        != (DISPCNT_BG1_ON | DISPCNT_BG2_ON | DISPCNT_BG3_ON))
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_DISPLAY_LAYERS);
        return FALSE;
    }
    if ((REG_MOSAIC & 0xFF) != 0)
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_MOSAIC);
        return FALSE;
    }
    // The renderer's contract is priorities 1/2/3, charbase 0, 16-color and a
    // 256x256 ring (see NATIVE_BGCNT_CAPABILITY_*). Screenbase, mosaic and wrap
    // bits are deliberately ignored: the tilemap is sampled from the live ring,
    // which is exactly what is copied to the screenbase each worker frame.
    if ((REG_BG1CNT & NATIVE_BGCNT_CAPABILITY_MASK) != NATIVE_BGCNT_CAPABILITY_BG1
     || (REG_BG2CNT & NATIVE_BGCNT_CAPABILITY_MASK) != NATIVE_BGCNT_CAPABILITY_BG2
     || (REG_BG3CNT & NATIVE_BGCNT_CAPABILITY_MASK) != NATIVE_BGCNT_CAPABILITY_BG3)
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_BG_CONFIG);
        return FALSE;
    }
    GetCameraOffsetWithPan(&snapshot->cameraX, &snapshot->cameraY);
    // Ring-world anchor (§4): capture the field camera's tile offsets so the
    // renderer can reconstruct the exact world rectangle the live BG ring is
    // filled with (anchorX = cameraMapX*16 - cameraXTileOffset*8). Without
    // these, residency and margin ring sampling would have to guess the ring's
    // coverage from the newest logical camera -- exactly the guess that drifts
    // as the player walks (see the snapshot header coordinate comment).
    GetCameraTileOffsets(&snapshot->cameraXTileOffset, &snapshot->cameraYTileOffset);
    // The GBA presents the ring at the scroll registers latched by the previous
    // VBlank (FieldUpdateBgTilemapScroll), which runs after CameraUpdate has
    // advanced the logical camera. On a moving frame the scroll is therefore
    // one frame of motion behind cameraX/cameraY -- a NORMAL, supported state
    // the renderer reproduces by sampling the ring at the scroll. The old gate
    // rejected every such frame as NATIVE_FALLBACK_BG_SCROLL (the runtime walk
    // skipped 1223 frames). What is genuinely unsupported is an INTERNALLY
    // INCONSISTENT latch: FieldUpdateBgTilemapScroll writes one value to all
    // three layers, so differing scrolls mean a torn presentation the
    // layer-coherent model does not describe. Require the three layers to agree
    // with each other (not with the camera).
    if ((REG_BG1HOFS & 0x1FF) != (REG_BG2HOFS & 0x1FF)
     || (REG_BG2HOFS & 0x1FF) != (REG_BG3HOFS & 0x1FF)
     || (REG_BG1VOFS & 0x1FF) != (REG_BG2VOFS & 0x1FF)
     || (REG_BG2VOFS & 0x1FF) != (REG_BG3VOFS & 0x1FF))
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_BG_SCROLL);
        return FALSE;
    }

    snapshot->cameraMapX = gSaveBlock1Ptr->pos.x;
    snapshot->cameraMapY = gSaveBlock1Ptr->pos.y;
    snapshot->mapWidth = layout->width;
    snapshot->mapHeight = layout->height;
    snapshot->primaryMetatiles = layout->primaryTileset->metatiles;
    snapshot->secondaryMetatiles = layout->secondaryTileset->metatiles;
    snapshot->primaryMetatileAttributes = layout->primaryTileset->metatileAttributes;
    snapshot->secondaryMetatileAttributes = layout->secondaryTileset->metatileAttributes;
    snapshot->border = layout->border;

    snapshot->gridWidth = gBackupMapLayout.width;
    snapshot->gridHeight = gBackupMapLayout.height;
    if ((u32)snapshot->gridWidth * snapshot->gridHeight > NATIVE_MAP_GRID_MAX)
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_MISSING_MAP_GRID);
        return FALSE;
    }
    memcpy(snapshot->mapGrid, gBackupMapLayout.map,
           snapshot->gridWidth * snapshot->gridHeight * sizeof(*snapshot->mapGrid));
    snapshot->mapGridValid = TRUE;

    memcpy(snapshot->bgRing[0], gOverworldTilemapBuffer_Bg1, NATIVE_BG_RING_SIZE * sizeof(u16));
    memcpy(snapshot->bgRing[1], gOverworldTilemapBuffer_Bg2, NATIVE_BG_RING_SIZE * sizeof(u16));
    memcpy(snapshot->bgRing[2], gOverworldTilemapBuffer_Bg3, NATIVE_BG_RING_SIZE * sizeof(u16));
    snapshot->bgRingValid = TRUE;

    memcpy(snapshot->bgVram, VRAM_, BG_VRAM_SIZE);
    snapshot->bgVramValid = TRUE;
    memcpy(snapshot->palette, PLTT, PLTT_SIZE);

    snapshot->bgCnt[0] = REG_BG1CNT;
    snapshot->bgCnt[1] = REG_BG2CNT;
    snapshot->bgCnt[2] = REG_BG3CNT;
    snapshot->bgHofs[0] = REG_BG1HOFS;
    snapshot->bgHofs[1] = REG_BG2HOFS;
    snapshot->bgHofs[2] = REG_BG3HOFS;
    snapshot->bgVofs[0] = REG_BG1VOFS;
    snapshot->bgVofs[1] = REG_BG2VOFS;
    snapshot->bgVofs[2] = REG_BG3VOFS;
    snapshot->dispCnt = REG_DISPCNT;
    snapshot->bg0Cnt = REG_BG0CNT;

    // BG0 carries the field's UI/window layers (field message box, text windows),
    // which the GBA oracle renders and the native map renderer does not reproduce.
    // The field leaves BG0 enabled for every real frame, so only a frame where BG0
    // actually renders visible pixels becomes incomparable; this opacity-aware
    // check detects exactly that (transparent cleared-window filler does not fire).
    // Gated on DISPCNT_BG0_ON: when BG0 is disabled the oracle cannot render it, so
    // there is nothing to fall back for.
    if ((REG_DISPCNT & DISPCNT_BG0_ON)
     && NativeOverworld_SnapshotHasBg0Content(snapshot))
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_BG0_OVERLAY);
        return FALSE;
    }

    // Transitional-tilemap check: the ring and the live screenbase must agree on
    // the visible window or the frame is mid-transition (see the detector's
    // comment). This runs last so a frame that is both transitional AND hits an
    // earlier config gate reports the earlier, more specific reason.
    if (BgRingDivergesFromScreenbase(snapshot))
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_BG_TILEMAP_IN_FLIGHT);
        return FALSE;
    }

    snapshot->requiredCapabilities = NATIVE_CAPABILITY_MAP_BACKGROUND
                                   | NATIVE_CAPABILITY_LIVE_BG_RING
                                   | NATIVE_CAPABILITY_BG_VRAM
                                   | NATIVE_CAPABILITY_BG_PALETTE;
    snapshot->fallbackReason = NATIVE_FALLBACK_NONE;
    return TRUE;
}

bool32 NativeOverworld_CaptureSnapshot(struct NativeOverworldSnapshot *snapshot)
{
    if (snapshot == NULL)
        return FALSE;
    memset(snapshot, 0, sizeof(*snapshot));

    // Scene identity gate for the Stage-0/1 display + zoom/expanded path. The
    // parity path uses the lean Overworld_IsNativeParityCaptureEligible() gate
    // instead; keeping this full predicate here preserves Stage-1 behavior.
    if (!Overworld_IsNativeExpandedRendererReady())
    {
        SetMapFallback(snapshot, NATIVE_FALLBACK_SCENE_NOT_OVERWORLD);
        return FALSE;
    }
    return CaptureSnapshotCommon(snapshot);
}

bool32 NativeOverworld_CaptureParitySnapshot(struct NativeOverworldSnapshot *snapshot)
{
    if (snapshot == NULL)
        return FALSE;
    memset(snapshot, 0, sizeof(*snapshot));

    // Stage-2 runtime parity uses its own lean gate, deliberately decoupled
    // from the zoom/expanded renderer eligibility predicate.
    if (!Overworld_IsNativeParityCaptureEligible(snapshot))
        return FALSE;
    return CaptureSnapshotCommon(snapshot);
}

static bool32 NativeOverworldSnapshotSupportsMap(const struct NativeOverworldSnapshot *snapshot)
{
    const enum NativeOverworldCapability mapCapabilities = NATIVE_CAPABILITY_MAP_BACKGROUND
                                                         | NATIVE_CAPABILITY_LIVE_BG_RING
                                                         | NATIVE_CAPABILITY_BG_VRAM
                                                         | NATIVE_CAPABILITY_BG_PALETTE;

    return snapshot != NULL
        && snapshot->fallbackReason == NATIVE_FALLBACK_NONE
        && (snapshot->requiredCapabilities & mapCapabilities) == mapCapabilities;
}

static s32 WorldCoordinateToPhysicalRingIndex(const struct NativeOverworldSnapshot *snapshot,
                                              u8 bg, s32 worldX, s32 worldY)
{
    // PHYSICAL ring indexing: maps a world pixel to the 32x32 tile index of
    // the live ring torus. This is the GBA's sampling formula and may WRAP
    // (&0xFF) -- but only AFTER world residency has been established
    // (IsWorldCoordinateResidentInRing). For a non-resident coordinate the
    // wrapped index addresses an unrelated world cell's ring content; callers
    // must never index the ring without first passing the residency test.
    //
    //     screen offset sx = worldX - (cameraMapX*16 + cameraX)
    //     ring column       = (bgHofs[bg] + sx) & 0xFF
    //     ring tile index   = ((ring column) & 0xFF) >> 3
    //
    // FieldUpdateBgTilemapScroll latches the scroll inside the VBlank handler,
    // which runs AFTER CameraUpdate has advanced the logical camera, so on a
    // moving frame the two differ (the GBA presents one frame of motion
    // behind the camera). The oracle (gba_easy_draw.c RenderBGScanline) uses
    // exactly this formula; sampling the ring at the scroll is what reproduces
    // it. See the snapshot header's coordinate-convention comment.
    s32 originX = snapshot->cameraMapX * 16 + snapshot->cameraX;
    s32 originY = snapshot->cameraMapY * 16 + snapshot->cameraY;
    s32 ringX = (snapshot->bgHofs[bg] + (worldX - originX)) & 0xFF;
    s32 ringY = (snapshot->bgVofs[bg] + (worldY - originY)) & 0xFF;

    return ((ringY >> 3) * NATIVE_BG_RING_SIDE_TILES) + (ringX >> 3);
}

/* ---- Ring-world coverage tracking (Stage 4A §4) ----
 *
 * The live ring (field_camera.c) is a 32x32-tile (256x256 px) text-mode torus
 * that the game fills with a 16x16-cell window of the world FOLLOWING the
 * camera. The window's world-pixel start ("coverage start") is:
 *
 *   - at load (full map view)        : [cameraMap*16, +256)   cells pos.x..pos.x+15
 *   - after an EAST/NORTH crossing   : [cameraMap*16-16, +240) cells pos.x-1..pos.x+14
 *   - after a WEST/SOUTH crossing    : [cameraMap*16, +256)   cells pos.x..pos.x+15
 *
 * The east-vs-west phase is NOT recoverable from a single snapshot: the u8
 * tile offset wraps mod 32, so `cameraMapX*16 - cameraXTileOffset*8` as an
 * s32 jumps by 256 the moment westward movement wraps the offset (0 -> 30 ->
 * 28 ...), putting the residency window over cells the ring does NOT hold --
 * the "expanded world becomes inconsistent after walking" bug. The phase is
 * history-dependent, so it is TRACKED across rendered frames.
 *
 * The direction of a crossing is detected from the change in
 * (cameraMap, cameraXTileOffset) since the previous render. Any change that is
 * not a single +/-1 crossing (map load / teleport / warp) resets the window to
 * the load layout [cameraMap*16, +256). A fresh full-draw always starts at
 * tile offset 0, so the first render after a map transition self-corrects. */
static bool32 sRingCoverageInit = FALSE;
static s32 sRingCoverageStartX = 0;
static s32 sRingCoverageStartY = 0;
static s16 sLastCamMapX = 0;
static s16 sLastCamMapY = 0;
static u8 sLastXTile = 0;
static u8 sLastYTile = 0;

// DEV-ONLY (§6): POKEEMERALD_NATIVE_EXPANDED_DEBUG=1 ring-coverage movement
// trace. Fires ONLY when the tracked coverage window changes -- a tile crossing
// (east/west/south/north), a load, or a teleport/warp reset -- never on
// sub-crossing pans, so it is rate-limited by construction (~4 lines/sec while
// walking). Prints the camera-map / tile-offset before->after and the tracked
// coverage window before->after per changed axis, so a developer can SEE the
// residency window phase-flip and shift as the player moves (the runtime symptom
// the Stage 4A §5 residency fix addressed). Reads the env var directly -- this
// function is called only on window changes, so there is no per-frame cost and
// unit tests can toggle it at runtime. Dev-only; never used by the presentation
// path and never alters presentation state.
static void DebugLogRingCoverageMove(s16 camX, s16 camY, u8 xTile, u8 yTile,
                                     const char *xEvent, s32 xWinBefore, s32 xWinAfter,
                                     const char *yEvent, s32 yWinBefore, s32 yWinAfter)
{
    if (getenv("POKEEMERALD_NATIVE_EXPANDED_DEBUG") == NULL)
        return;
    fprintf(stderr,
            "[POKEEMERALD_NATIVE_EXPANDED_DEBUG] ring coverage move: "
            "cam=(%d,%d) tile=(%u,%u)",
            (int)camX, (int)camY, (unsigned int)xTile, (unsigned int)yTile);
    if (xEvent != NULL)
        fprintf(stderr, "  X:%s window [%d,+256)->[%d,+256)",
                xEvent, xWinBefore, xWinAfter);
    if (yEvent != NULL)
        fprintf(stderr, "  Y:%s window [%d,+256)->[%d,+256)",
                yEvent, yWinBefore, yWinAfter);
    fputc('\n', stderr);
}

static void UpdateRingCoverageWindow(const struct NativeOverworldSnapshot *snapshot)
{
    s16 camX = snapshot->cameraMapX;
    s16 camY = snapshot->cameraMapY;
    u8 xTile = snapshot->cameraXTileOffset;
    u8 yTile = snapshot->cameraYTileOffset;

    if (!sRingCoverageInit)
    {
        sRingCoverageStartX = camX * 16;
        sRingCoverageStartY = camY * 16;
        sLastCamMapX = camX;
        sLastCamMapY = camY;
        sLastXTile = xTile;
        sLastYTile = yTile;
        sRingCoverageInit = TRUE;
        DebugLogRingCoverageMove(camX, camY, xTile, yTile,
                                 "LOAD", sRingCoverageStartX, sRingCoverageStartX,
                                 "LOAD", sRingCoverageStartY, sRingCoverageStartY);
        return;
    }
    {
        s32 oldStartX;
        s32 oldStartY;
        const char *xEvent;
        const char *yEvent;
        // Signed deltas in the 32-tile torus: the stored offset is a u8 that
        // wraps mod 32, so unwrap to [-16, 16]. The (u8) cast folds the wrap
        // into [0,255] (e.g. 30 -> 0 reads as 226), so first re-sign to the
        // range [-128,128], THEN collapse the 32-tile torus onto [-16,16].
        // Without the re-sign step a 30->0 tile wrap would read as +194, not
        // the +2 it really is.
        s32 dx = (s32)camX - (s32)sLastCamMapX;
        s32 dy = (s32)camY - (s32)sLastCamMapY;
        s32 dxt = (s32)(u8)(xTile - sLastXTile);
        s32 dyt = (s32)(u8)(yTile - sLastYTile);
        if (dxt >= 128) dxt -= 256; else if (dxt < -128) dxt += 256;
        if (dyt >= 128) dyt -= 256; else if (dyt < -128) dyt += 256;
        if (dxt > 16) dxt -= 32; else if (dxt < -16) dxt += 32;
        if (dyt > 16) dyt -= 32; else if (dyt < -16) dyt += 32;

        oldStartX = sRingCoverageStartX;
        oldStartY = sRingCoverageStartY;

        if ((dx == 1 && dxt == 2) || (dx == -1 && dxt == -2) || (dx == 0 && dxt == 0))
        {
            if (dx == 1)
            {
                sRingCoverageStartX = camX * 16 - 16;   /* east crossing */
                xEvent = "EAST";
            }
            else if (dx == -1)
            {
                sRingCoverageStartX = camX * 16;        /* west crossing */
                xEvent = "WEST";
            }
            else
            {
                xEvent = NULL;   /* no X change, window unchanged */
            }
        }
        else
        {
            // Not a single +/-1 tile crossing: map load / teleport / warp.
            sRingCoverageStartX = camX * 16;
            xEvent = "RESET";
        }

        if ((dy == 1 && dyt == 2) || (dy == -1 && dyt == -2) || (dy == 0 && dyt == 0))
        {
            if (dy == 1)
            {
                sRingCoverageStartY = camY * 16 - 16;   /* south crossing */
                yEvent = "SOUTH";
            }
            else if (dy == -1)
            {
                sRingCoverageStartY = camY * 16;        /* north crossing */
                yEvent = "NORTH";
            }
            else
            {
                yEvent = NULL;
            }
        }
        else
        {
            sRingCoverageStartY = camY * 16;
            yEvent = "RESET";
        }

        // DEV-ONLY (§6): movement trace on actual window changes only (see
        // DebugLogRingCoverageMove). Never used by the presentation path.
        if (xEvent != NULL || yEvent != NULL)
            DebugLogRingCoverageMove(camX, camY, xTile, yTile,
                                     xEvent, oldStartX, sRingCoverageStartX,
                                     yEvent, oldStartY, sRingCoverageStartY);
    }
    sLastCamMapX = camX;
    sLastCamMapY = camY;
    sLastXTile = xTile;
    sLastYTile = yTile;
}

static bool32 IsWorldCoordinateResidentInRing(const struct NativeOverworldSnapshot *snapshot,
                                              s32 worldX, s32 worldY)
{
    // WORLD residency: is this world pixel genuinely represented by the live
    // ring? The ring holds exactly the world-pixel rectangle
    // [sRingCoverageStartX, +256) x [sRingCoverageStartY, +256) -- the
    // 16x16-cell window the field renderer has actually filled, as tracked by
    // UpdateRingCoverageWindow (see its comment for the load / east / west
    // layout). This is a WORLD-COVERAGE test, deliberately NOT wrapped: the
    // &0xFF torus in WorldCoordinateToPhysicalRingIndexAnchored only addresses
    // tiles WITHIN this window. Treating a non-resident coordinate as
    // ring-eligible aliases it onto the ring content of an unrelated world cell
    // -- the stale/repeated-map-content bug seen in the expanded margins.
    //
    // Normal pans keep the whole 240x160 window inside this rectangle
    // (cameraX in [0,16), cameraY in [0,96] -- the sub-tile phase plus the
    // pan-ahead offset), so the central window's ring precedence is unchanged;
    // only expanded-viewport coordinates outside the rectangle fall through to
    // the grid/connection/border fallback.
    //
    // Self-priming: residency is tracked across rendered frames, so any direct
    // caller (unit tests, provider classification) must advance the window from
    // THIS snapshot before answering. The update is idempotent for an unchanged
    // camera, so the per-pixel call from within DrawMapFrameWithMetaEx (which
    // already advanced at entry) is a no-op.
    UpdateRingCoverageWindow(snapshot);
    return worldX >= sRingCoverageStartX && worldX < sRingCoverageStartX + NATIVE_BG_RING_PIXELS
        && worldY >= sRingCoverageStartY && worldY < sRingCoverageStartY + NATIVE_BG_RING_PIXELS;
}

static s32 WorldCoordinateToPhysicalRingIndexAnchored(
    const struct NativeOverworldSnapshot *snapshot, u8 bg, s32 worldX, s32 worldY)
{
    // ANCHORED physical ring indexing for RESIDENT MARGIN coordinates: maps a
    // world pixel to the 32x32 tile index of the live ring torus using the RING
    // WORLD ANCHOR (not the presentation scroll). The presentation formula
    // (bgHofs + worldX - originX) is correct for the central 240x160 window --
    // there worldX - originX is the screen offset and the GBA samples the ring
    // at the scroll -- but for a resident MARGIN pixel it is off by the scroll's
    // drift from the ring's true anchor (16k + speed columns after k tile
    // crossings), which is exactly the "expanded world gradually becomes
    // inconsistent" error. The anchored formula samples the cell the ring is
    // actually FILLED with:
    //
    //     ring column = (worldX - anchorX) & 0xFF
    //
    // The ring-writer stores cell (P0+c) at physical tile (2c mod 32), so column
    // (worldX - anchorX)>>3 addresses exactly the cell holding world tile
    // worldX>>3. bg is unused by the formula (the ring is indexed uniformly for
    // all three layers) but kept for signature symmetry with the presentation
    // indexer. Callers must pass only coordinates already proven resident.
    (void)bg;
    s32 anchorX = snapshot->cameraMapX * 16 - snapshot->cameraXTileOffset * 8;
    s32 anchorY = snapshot->cameraMapY * 16 - snapshot->cameraYTileOffset * 8;
    s32 ringX = (worldX - anchorX) & 0xFF;
    s32 ringY = (worldY - anchorY) & 0xFF;

    return ((ringY >> 3) * NATIVE_BG_RING_SIDE_TILES) + (ringX >> 3);
}

static bool32 IsInCentralWindow(const struct NativeOverworldSnapshot *snapshot,
                                s32 worldX, s32 worldY)
{
    // The 240x160 GBA window the field presents: world pixels
    // [origin, origin+240) x [origin, origin+160) at the LOGICAL origin
    // (cameraMap*16 + camera). Every central-window pixel keeps the EXACT
    // presentation-formula ring sampling (WorldCoordinateToPhysicalRingIndex)
    // -- parity with the proven 240x160 renderer is a hard invariant.
    s32 originX = snapshot->cameraMapX * 16 + snapshot->cameraX;
    s32 originY = snapshot->cameraMapY * 16 + snapshot->cameraY;

    return worldX >= originX && worldX < originX + DISPLAY_WIDTH
        && worldY >= originY && worldY < originY + DISPLAY_HEIGHT;
}

static u16 GetSnapshotMetatileId(const struct NativeOverworldSnapshot *snapshot,
                                 s32 mapX, s32 mapY)
{
    // Precedence 2: the mutable backup map grid, which already contains the
    // cardinal connection strips copied at map load. Precedence 4: the
    // repeating 2x2 map border when the coordinate is outside the grid.
    if (snapshot->mapGridValid
     && mapX >= 0 && mapY >= 0
     && mapX < snapshot->gridWidth && mapY < snapshot->gridHeight)
    {
        u16 block = snapshot->mapGrid[mapX + snapshot->gridWidth * mapY];

        if (block != MAPGRID_UNDEFINED)
            return block & MAPGRID_METATILE_ID_MASK;
    }
    {
        u8 borderIndex = ((mapX + 1) & 1) + ((mapY + 1) & 1) * 2;

        return snapshot->border[borderIndex] & MAPGRID_METATILE_ID_MASK;
    }
}

static u16 GetSnapshotMetatileAttributes(const struct NativeOverworldSnapshot *snapshot,
                                         u16 metatileId)
{
    if (metatileId < NUM_METATILES_IN_PRIMARY
     && snapshot->primaryMetatileAttributes != NULL)
        return snapshot->primaryMetatileAttributes[metatileId];
    if (metatileId < NUM_METATILES_TOTAL
     && snapshot->secondaryMetatileAttributes != NULL)
        return snapshot->secondaryMetatileAttributes[metatileId - NUM_METATILES_IN_PRIMARY];
    return 0;
}

static u16 GetMetatileTileEntryFromSnapshot(const struct NativeOverworldSnapshot *snapshot,
                                            u8 bg, s32 worldX, s32 worldY)
{
    s32 mapX = FloorDivide(worldX, 16);
    s32 mapY = FloorDivide(worldY, 16);
    s32 localX = PositiveModulo(worldX, 16);
    s32 localY = PositiveModulo(worldY, 16);
    u8 quadrant = (localY / 8) * 2 + localX / 8;
    u16 metatileId = GetSnapshotMetatileId(snapshot, mapX, mapY);
    u8 layerType = UNPACK_LAYER_TYPE(GetSnapshotMetatileAttributes(snapshot, metatileId));
    const u16 *tiles;

    if (metatileId < NUM_METATILES_IN_PRIMARY)
        tiles = snapshot->primaryMetatiles + metatileId * NUM_TILES_PER_METATILE;
    else
        tiles = snapshot->secondaryMetatiles
              + (metatileId - NUM_METATILES_IN_PRIMARY) * NUM_TILES_PER_METATILE;
    return MetatileTileEntryForLayer(bg, tiles, layerType, quadrant);
}

// World tile provider: returns the BG1/BG2/BG3 tile entries for the world
// 8x8 tile containing (worldX, worldY), and reports which source won.
//
// Precedence:
//   1. The final live BG ring, but ONLY when (worldX, worldY) is genuinely
//      resident in it (IsWorldCoordinateResidentInRing). Residency is a plain
//      world-rectangle test anchored at the ring's true world anchor
//      (cameraMap*16 - tileOffset*8; see the snapshot header) -- NOT at the
//      player's map cell -- and it NEVER wraps. The physical ring index may
//      wrap through the &0xFF torus, but that wrap is only applied after
//      residency succeeds. This preserves transient writes (doors, direct
//      tilemap edits) for resident coordinates while keeping stale aliased
//      ring content out of the expanded margins.
//
//      The ring is SAMPLED DIFFERENTLY depending on which window the pixel is
//      in (the two origins differ on a moving frame):
//        - CENTRAL 240x160 window (IsInCentralWindow): sample at the
//          PRESENTATION scroll (bgHofs), exactly as the GBA samples it. This is
//          the parity-invariant path and must never change.
//        - RESIDENT MARGIN outside the central window: sample at the ring's
//          WORLD ANCHOR (WorldCoordinateToPhysicalRingIndexAnchored). The
//          presentation scroll drifts from the anchor on moving frames, so the
//          central formula would mis-map margin pixels to the wrong ring cell
//          (the moving-world coherency bug). The anchored formula maps every
//          resident world coordinate to the exact ring cell the field renderer
//          filled for it.
//   2-4. The snapshot backup map grid (which carries connection strips), then
//      the repeating border, via the metatile tables when outside the ring.
//      This fallback is resolved at the LOGICAL world coordinate.
//
// Returns TRUE when the ring won and FALSE when the grid/border fallback won.
// The caller must use the matching sub-pixel convention: presentation-scroll
// for central pixels, ring-anchor for resident margin pixels, logical-world
// for grid/border pixels.
static bool32 ResolveBgTileEntries(const struct NativeOverworldSnapshot *snapshot,
                                   s32 worldX, s32 worldY, u16 *entries)
{
    if (snapshot->bgRingValid)
    {
        bool32 central = IsInCentralWindow(snapshot, worldX, worldY);
        bool32 resident = central
                       || IsWorldCoordinateResidentInRing(snapshot, worldX, worldY);

        if (resident)
        {
            s32 index;

            if (central)
            {
                index = WorldCoordinateToPhysicalRingIndex(snapshot, 0, worldX, worldY);
                entries[0] = snapshot->bgRing[0][index];
                index = WorldCoordinateToPhysicalRingIndex(snapshot, 1, worldX, worldY);
                entries[1] = snapshot->bgRing[1][index];
                index = WorldCoordinateToPhysicalRingIndex(snapshot, 2, worldX, worldY);
                entries[2] = snapshot->bgRing[2][index];
            }
            else
            {
                index = WorldCoordinateToPhysicalRingIndexAnchored(snapshot, 0, worldX, worldY);
                entries[0] = snapshot->bgRing[0][index];
                index = WorldCoordinateToPhysicalRingIndexAnchored(snapshot, 1, worldX, worldY);
                entries[1] = snapshot->bgRing[1][index];
                index = WorldCoordinateToPhysicalRingIndexAnchored(snapshot, 2, worldX, worldY);
                entries[2] = snapshot->bgRing[2][index];
            }
            return TRUE;
        }
    }
    entries[0] = GetMetatileTileEntryFromSnapshot(snapshot, 1, worldX, worldY);
    entries[1] = GetMetatileTileEntryFromSnapshot(snapshot, 2, worldX, worldY);
    entries[2] = GetMetatileTileEntryFromSnapshot(snapshot, 3, worldX, worldY);
    return FALSE;
}

/* ---- Stage 4A dev-only provider visualization (§6) ---- */

// Provider codes for the dev-only region classification. R = the live ring
// won (coordinate ring-eligible), G = backup-map-grid content, C = grid
// connection strip (a DEFINED grid cell outside the real map rectangle -- the
// cardinal apron copied at map load), B = repeating border. These are
// DIAGNOSTIC ONLY: the presentation path never calls them and they never alter
// presentation colors.
#define NATIVE_OVERWORLD_PROVIDER_RING       'R'
#define NATIVE_OVERWORLD_PROVIDER_GRID       'G'
#define NATIVE_OVERWORLD_PROVIDER_CONNECTION 'C'
#define NATIVE_OVERWORLD_PROVIDER_BORDER     'B'

// Classifies the world tile provider for one world pixel, using exactly the
// precedence the renderer resolves with (ResolveBgTileEntries -> ring; else
// the backup grid when in bounds and defined; else the repeating border). The
// ring test is the central-window-or-anchored-world-residency eligibility the
// resolver uses, so the map shows precisely which provider each pixel would
// display.
static u8 GetWorldTileProvider(const struct NativeOverworldSnapshot *snapshot,
                               s32 worldX, s32 worldY)
{
    u16 entries[3];
    bool32 ringWon;
    s32 mapX;
    s32 mapY;
    u16 block;

    ringWon = ResolveBgTileEntries(snapshot, worldX, worldY, entries);
    if (ringWon)
        return NATIVE_OVERWORLD_PROVIDER_RING;

    mapX = FloorDivide(worldX, 16);
    mapY = FloorDivide(worldY, 16);
    if (snapshot->mapGridValid
     && mapX >= 0 && mapY >= 0
     && mapX < snapshot->gridWidth && mapY < snapshot->gridHeight)
    {
        block = snapshot->mapGrid[mapX + snapshot->gridWidth * mapY];
        if (block != MAPGRID_UNDEFINED)
        {
            // The backup layout holds the layout at grid offset MAP_OFFSET
            // (InitBackupMapLayoutData: dest starts at row 7, col 7) with the
            // cardinal connection strips copied into the apron around it, so a
            // defined cell OUTSIDE [MAP_OFFSET, MAP_OFFSET+mapWidth) x
            // [MAP_OFFSET, MAP_OFFSET+mapHeight) is connection-strip content.
            if (mapX < MAP_OFFSET || mapY < MAP_OFFSET
             || mapX >= MAP_OFFSET + snapshot->mapWidth
             || mapY >= MAP_OFFSET + snapshot->mapHeight)
                return NATIVE_OVERWORLD_PROVIDER_CONNECTION;
            return NATIVE_OVERWORLD_PROVIDER_GRID;
        }
    }
    return NATIVE_OVERWORLD_PROVIDER_BORDER;
}

// DEV-ONLY (§6/§11): provider codes for the 4 expanded corners (TL/TR/BL/BR)
// and the midpoints of the 4 margins (L/R/T/B). Where a margin is zero the
// margin midpoint is the viewport edge (no apron), reported as BORDER so
// callers can ignore it. Never used by the presentation path.
bool32 NativeOverworldRenderer_ExpandedProviderCorners(
    const struct NativeOverworldSnapshot *bgSnap,
    s32 viewportLeftX, s32 viewportTopY,
    s32 viewportWidth, s32 viewportHeight,
    u8 *outCorners, u8 *outMargins)
{
    s32 w;
    s32 h;
    s32 marginX;
    s32 marginY;

    if (bgSnap == NULL || outCorners == NULL || outMargins == NULL
     || viewportWidth <= 0 || viewportHeight <= 0)
        return FALSE;

    // Keep the tracked ring-world coverage current for the provider
    // classification (idempotent; this may run before the map render).
    UpdateRingCoverageWindow(bgSnap);

    w = viewportWidth;
    h = viewportHeight;
    marginX = (w - DISPLAY_WIDTH) / 2;
    marginY = (h - DISPLAY_HEIGHT) / 2;

    outCorners[0] = GetWorldTileProvider(bgSnap, viewportLeftX, viewportTopY);
    outCorners[1] = GetWorldTileProvider(bgSnap, viewportLeftX + w - 1, viewportTopY);
    outCorners[2] = GetWorldTileProvider(bgSnap, viewportLeftX, viewportTopY + h - 1);
    outCorners[3] = GetWorldTileProvider(bgSnap, viewportLeftX + w - 1, viewportTopY + h - 1);

    outMargins[0] = (marginX > 0)
        ? GetWorldTileProvider(bgSnap, viewportLeftX + marginX / 2, viewportTopY + h / 2)
        : NATIVE_OVERWORLD_PROVIDER_BORDER;
    outMargins[1] = (marginX > 0)
        ? GetWorldTileProvider(bgSnap, viewportLeftX + w - 1 - marginX / 2, viewportTopY + h / 2)
        : NATIVE_OVERWORLD_PROVIDER_BORDER;
    outMargins[2] = (marginY > 0)
        ? GetWorldTileProvider(bgSnap, viewportLeftX + w / 2, viewportTopY + marginY / 2)
        : NATIVE_OVERWORLD_PROVIDER_BORDER;
    outMargins[3] = (marginY > 0)
        ? GetWorldTileProvider(bgSnap, viewportLeftX + w / 2, viewportTopY + h - 1 - marginY / 2)
        : NATIVE_OVERWORLD_PROVIDER_BORDER;
    return TRUE;
}

// DEV-ONLY (§6): classifies every `regionPx`-square region of the viewport by
// the provider that serves its CENTER (regionPx is 8 or 16). Fills
// outRegionProvider (rows*cols codes) and reports the region dimensions. The
// center sample keeps region classification stable on sub-region boundary
// coordinates. Never used by the presentation path.
bool32 NativeOverworldRenderer_ExpandedProviderRegionMap(
    const struct NativeOverworldSnapshot *bgSnap,
    s32 viewportLeftX, s32 viewportTopY,
    s32 viewportWidth, s32 viewportHeight,
    s32 regionPx,
    u8 *outRegionProvider,
    s32 *outRegionCols, s32 *outRegionRows)
{
    s32 cols;
    s32 rows;
    s32 row;
    s32 col;

    if (bgSnap == NULL || outRegionProvider == NULL
     || regionPx <= 0 || viewportWidth <= 0 || viewportHeight <= 0)
        return FALSE;

    // Keep the tracked ring-world coverage current for the provider
    // classification (idempotent; this may run before the map render).
    UpdateRingCoverageWindow(bgSnap);

    cols = (viewportWidth + regionPx - 1) / regionPx;
    rows = (viewportHeight + regionPx - 1) / regionPx;
    for (row = 0; row < rows; row++)
    {
        for (col = 0; col < cols; col++)
        {
            s32 worldX = viewportLeftX + col * regionPx + regionPx / 2;
            s32 worldY = viewportTopY + row * regionPx + regionPx / 2;
            outRegionProvider[row * cols + col]
                = GetWorldTileProvider(bgSnap, worldX, worldY);
        }
    }
    if (outRegionCols != NULL)
        *outRegionCols = cols;
    if (outRegionRows != NULL)
        *outRegionRows = rows;
    return TRUE;
}

// DEV-ONLY (§6): prints the provider region map as an ASCII grid (one char per
// region: R=ring, G=grid, C=connection, B=border). Never used by the
// presentation path.
void NativeOverworldRenderer_DumpProviderRegionMap(const u8 *regionProvider,
                                                   s32 cols, s32 rows)
{
    s32 row;
    s32 col;

    if (regionProvider == NULL || cols <= 0 || rows <= 0)
        return;
    fprintf(stderr, "  provider region map (%dx%d):\n", cols, rows);
    for (row = 0; row < rows; row++)
    {
        fputc(' ', stderr);
        for (col = 0; col < cols; col++)
            fputc(regionProvider[row * cols + col], stderr);
        fputc('\n', stderr);
    }
}

// DEV-ONLY (§6): fills a viewport-sized RGB555 framebuffer with a SOLID color
// per provider region (R=green 0x03E0, G=blue 0x001F, C=yellow 0x7FE0,
// B=red 0x7C00), one regionPx block per region, so the provider layout is
// visible at a glance as a compact debug image. Never used by the presentation
// path and never alters presentation colors.
bool32 NativeOverworldRenderer_FillProviderDebugImage(
    const struct NativeOverworldSnapshot *bgSnap,
    s32 viewportLeftX, s32 viewportTopY,
    s32 viewportWidth, s32 viewportHeight,
    s32 regionPx, u16 *outFrame)
{
    s32 y;
    s32 x;

    if (bgSnap == NULL || outFrame == NULL
     || regionPx <= 0 || viewportWidth <= 0 || viewportHeight <= 0)
        return FALSE;

    // Keep the tracked ring-world coverage current for the provider
    // classification (idempotent; this may run before the map render).
    UpdateRingCoverageWindow(bgSnap);

    for (y = 0; y < viewportHeight; y++)
    {
        for (x = 0; x < viewportWidth; x++)
        {
            u8 provider = GetWorldTileProvider(
                bgSnap,
                viewportLeftX + (x / regionPx) * regionPx + regionPx / 2,
                viewportTopY + (y / regionPx) * regionPx + regionPx / 2);
            u16 color = 0x7C00; /* border: red (fallback default) */

            switch (provider)
            {
            case NATIVE_OVERWORLD_PROVIDER_RING:       color = 0x03E0; break; /* green */
            case NATIVE_OVERWORLD_PROVIDER_GRID:       color = 0x001F; break; /* blue */
            case NATIVE_OVERWORLD_PROVIDER_CONNECTION: color = 0x7FE0; break; /* yellow */
            case NATIVE_OVERWORLD_PROVIDER_BORDER:     color = 0x7C00; break; /* red */
            default: break;
            }
            outFrame[y * viewportWidth + x] = color;
        }
    }
    return TRUE;
}

bool32 NativeOverworldRenderer_BuildFallbackCanvas(
    const u32 *coreFrame, s32 coreWidth, s32 coreHeight,
    const struct NativeViewport *viewport,
    u32 *outCanvas)
{
    s32 marginX;
    s32 marginY;
    s32 y;
    s32 x;

    if (coreFrame == NULL || outCanvas == NULL || viewport == NULL
     || coreWidth <= 0 || coreHeight <= 0)
        return FALSE;

    // Stage 4A Issue B (directive 9-12): the fallback canvas is the SELECTED
    // viewport's size with the authoritative 240x160 frame centered 1:1 and
    // PURE-BLACK margins. viewport is read-only -- the user's selected
    // continuous zoom/scale is NEVER mutated by a capability fallback. Margins
    // are deterministic integer truncation of half the extra size, matching the
    // TopLeft odd-rounding contract (300x200 -> (30, 20)).
    marginX = (viewport->width - coreWidth) / 2;
    marginY = (viewport->height - coreHeight) / 2;
    if (marginX < 0 || marginY < 0)
        return FALSE;

    // Every output pixel is written each call (fresh margins, never stale
    // expanded pixels from a prior frame -- directive 9/12).
    for (y = 0; y < viewport->height; y++)
    {
        bool32 inCoreY = (y >= marginY && y < marginY + coreHeight);
        for (x = 0; x < viewport->width; x++)
        {
            if (inCoreY && x >= marginX && x < marginX + coreWidth)
                outCanvas[y * viewport->width + x]
                    = coreFrame[(y - marginY) * coreWidth + (x - marginX)];
            else
                outCanvas[y * viewport->width + x] = 0xFF000000u; /* pure black */
        }
    }
    return TRUE;
}

static bool32 SampleSnapshotTile(const struct NativeOverworldSnapshot *snapshot,
                                 u16 entry, s32 sampleX, s32 sampleY, u16 *color)
{
    const u8 *tiles = (const u8 *)snapshot->bgVram;
    const u16 *palette = snapshot->palette;
    u16 tileNum = entry & 0x3FF;
    u8 paletteNum = (entry >> 12) & 0xF;
    u8 tileX = PositiveModulo(sampleX, 8);
    u8 tileY = PositiveModulo(sampleY, 8);
    u8 packedPixel;
    u8 pixel;

    if (entry & (1 << 10))
        tileX = 7 - tileX;
    if (entry & (1 << 11))
        tileY = 7 - tileY;
    packedPixel = tiles[tileNum * 32 + tileY * 4 + tileX / 2];
    pixel = tileX & 1 ? packedPixel >> 4 : packedPixel & 0xF;
    if (pixel == 0)
        return FALSE;
    *color = palette[paletteNum * 16 + pixel];
    return TRUE;
}

static u16 CompositeMapPixel(const struct NativeOverworldSnapshot *snapshot,
                             const u16 *entries, bool32 ringWon,
                             s32 sx, s32 sy, s32 worldX, s32 worldY)
{
    // entries[0]=BG1 (priority 1, top), [1]=BG2 (priority 2), [2]=BG3
    // (priority 3). The highest-priority layer with a non-transparent pixel
    // wins; otherwise the backdrop PLTT[0] is shown. This is the exact GBA
    // text-mode composition for the normal overworld with no blend/window
    // masking active (those capabilities trigger fallback).
    //
    // The char-tile sub-pixel must follow the same origin as the entry: the
    // presentation scroll for a ring entry ((bgHofs[bg]+sx)&7 as the GBA does),
    // the logical world position for a grid/border entry. Mixing them would
    // tear on a moving frame.
    u16 color = snapshot->palette[0];
    u8 bg;

    for (bg = 0; bg < 3; bg++)
    {
        u16 layerColor;
        s32 sampleX = ringWon ? (snapshot->bgHofs[bg] + sx) : worldX;
        s32 sampleY = ringWon ? (snapshot->bgVofs[bg] + sy) : worldY;

        if (SampleSnapshotTile(snapshot, entries[bg], sampleX, sampleY, &layerColor))
        {
            color = layerColor | 0x8000; // BG pixels carry the GBA alpha flag
            break;
        }
    }
    return color;
}

bool32 NativeOverworldRenderer_DrawMapFrame(const struct NativeOverworldSnapshot *snapshot,
                                            u16 *outFrame)
{
    s32 originX;
    s32 originY;
    s32 sx;
    s32 sy;

    if (!NativeOverworldSnapshotSupportsMap(snapshot) || outFrame == NULL)
        return FALSE;

    // Logical world origin: the game's model of where the 240x160 window
    // points. The ring path derives the screen offset from it; the grid /
    // border / connection fallback resolves in it directly.
    originX = snapshot->cameraMapX * 16 + snapshot->cameraX;
    originY = snapshot->cameraMapY * 16 + snapshot->cameraY;

    for (sy = 0; sy < DISPLAY_HEIGHT; sy++)
    {
        for (sx = 0; sx < DISPLAY_WIDTH; sx++)
        {
            u16 entries[3];
            bool32 ringWon;
            s32 worldX = originX + sx;
            s32 worldY = originY + sy;

            // Per-pixel entry resolution: the ring is sampled at the
            // presentation scroll, so on a moving frame the ring-tile index
            // shifts within the 8x8 world tile (the ring grid and the logical
            // grid are misaligned by the scroll-vs-camera delta). A once-per-
            // world-tile batch would sample the wrong ring tiles.
            ringWon = ResolveBgTileEntries(snapshot, worldX, worldY, entries);
            outFrame[sy * DISPLAY_WIDTH + sx] =
                CompositeMapPixel(snapshot, entries, ringWon, sx, sy, worldX, worldY);
        }
    }
    return TRUE;
}

u8 NativeOverworldSnapshotGetBgPriority(const struct NativeOverworldSnapshot *snapshot,
                                        u8 bg)
{
    if (snapshot == NULL || bg >= 3)
        return 0;
    return (u8)(snapshot->bgCnt[bg] & 3);
}

/*
 * Stage 3C priority-aware per-pixel BG composite. Identical sampling to
 * CompositeMapPixel but merges the three captured layers by their BGCNT
 * priorities (lower priority number wins; at equal priority the lower bgnum
 * wins -- BG1 < BG2 < BG3), exactly as the GBA text-mode compositor does.
 * *winner is 0 (backdrop) or 2/3/4 (BG1/2/3), matching the oracle layerOut
 * encoding. On the field's fixed priorities (1/2/3) this is byte-identical to
 * CompositeMapPixel.
 */
static u16 CompositeMapPixelWithPriority(const struct NativeOverworldSnapshot *snapshot,
                                         const u16 *entries, bool32 ringWon,
                                         s32 ringSx, s32 ringSy, s32 worldX, s32 worldY,
                                         u16 *bgLayers, u32 bgLayerPixels, u32 pixelIndex,
                                         u8 *winner)
{
    u16 color = snapshot->palette[0];
    u8 bestPrio = 4; // backdrop is "priority infinity"
    u8 bestWinner = 0;
    u8 bg;

    for (bg = 0; bg < 3; bg++)
    {
        u16 layerColor;
        u8 prio = (u8)(snapshot->bgCnt[bg] & 3);
        // The ring sub-pixel offset is worldX - logicalOrigin (NOT
        // worldX - viewportLeft): the ring torus is anchored to the logical
        // origin, which the expanded viewport may sit left/top of.
        s32 sampleX = ringWon ? (snapshot->bgHofs[bg] + ringSx) : worldX;
        s32 sampleY = ringWon ? (snapshot->bgVofs[bg] + ringSy) : worldY;

        if (!SampleSnapshotTile(snapshot, entries[bg], sampleX, sampleY, &layerColor))
            continue;
        // Stage 3E: per-layer raw color in the oracle's scanline.layers[bgnum]
        // encoding (bit 15 = opaque, 0 = transparent) for the compositor's alpha
        // targetB walk. bg is the native index 0..2 for GBA BG1/2/3 (bgnum =
        // bg + 1); the GBA BG0 slot is pre-zeroed and stays 0 (the BG0 overlay
        // gate guarantees BG0 never draws on a supported frame). Transparent
        // layer pixels keep the slot's initial 0.
        if (bgLayers != NULL)
            bgLayers[((u32)(bg + 1) * bgLayerPixels) + pixelIndex] = layerColor | 0x8000;
        if (prio < bestPrio || (prio == bestPrio && (u8)(bg + 2) < bestWinner))
        {
            bestPrio = prio;
            bestWinner = (u8)(bg + 2);
            color = layerColor | 0x8000; // BG pixels carry the GBA alpha flag
        }
    }
    *winner = bestWinner;
    return color;
}

/*
 * Stage 4A generalized priority-aware map renderer. Identical sampling to
 * DrawMapFrameWithMeta but renders an ARBITRARY viewport rectangle of the
 * overworld: width x height logical pixels whose top-left world pixel is
 * (viewportLeftX, viewportTopY). bgLayerPixels matches width*height so the flat
 * per-bgnum layer arrays size with the viewport, not with the 240x160 display.
 *
 * The ring sub-pixel offset depends on which ring path won (all three layers
 * carry equal scroll registers under the capture gate, so a single bgHofs[0]
 * drives them):
 *
 *   - CENTRAL 240x160 window: ringSx = worldX - logical origin. The presentation
 *     scroll is then bgHofs + worldX - originX = bgHofs + sx, the GBA's exact
 *     sampling (parity hard invariant).
 *   - RESIDENT MARGIN (ring-eligible outside the central window): ringSx =
 *     worldX - ringWorldAnchor - bgHofs[0], so the sample lands at
 *     worldX - anchorX -- the RING WORLD ANCHOR the field renderer filled, NOT
 *     the presentation scroll. The scroll drifts from the anchor on moving
 *     frames, so the presentation offset would sub-sample the wrong texel of an
 *     unrelated world tile (the moving-world coherency bug).
 *   - Grid / border / connection: ringWon is FALSE and CompositeMapPixelWithPriority
 *     samples worldX directly; ringSx is ignored.
 */
static bool32 DrawMapFrameWithMetaEx(const struct NativeOverworldSnapshot *snapshot,
                                     u16 *outFrame, u8 *bgWinner, u16 *bgLayers,
                                     u16 width, u16 height,
                                     s32 viewportLeftX, s32 viewportTopY)
{
    s32 originX;
    s32 originY;
    s32 anchorX;
    s32 anchorY;
    u16 sx;
    u16 sy;
    u32 bgLayerPixels;

    if (!NativeOverworldSnapshotSupportsMap(snapshot)
     || outFrame == NULL || bgWinner == NULL)
        return FALSE;

    // Advance the tracked ring-world coverage window from this frame's camera
    // state (Stage 4A §4). Runs on EVERY native map render -- the 240x160
    // parity wrapper and the expanded path both come through here -- so the
    // residency state stays current even when the expanded path falls back.
    UpdateRingCoverageWindow(snapshot);

    // Logical world origin of the 240x160 view (same as DrawMapFrame).
    originX = snapshot->cameraMapX * 16 + snapshot->cameraX;
    originY = snapshot->cameraMapY * 16 + snapshot->cameraY;
    // Ring WORLD ANCHOR: the s32 world-pixel origin of the 256x256 rectangle
    // the live ring is actually filled with (cameraMap*16 - tileOffset*8; see
    // the snapshot header). Resident margin pixels sample the ring here, not at
    // the presentation scroll.
    anchorX = snapshot->cameraMapX * 16 - snapshot->cameraXTileOffset * 8;
    anchorY = snapshot->cameraMapY * 16 - snapshot->cameraYTileOffset * 8;
    bgLayerPixels = (u32)width * (u32)height;

    // Stage 3E: optional per-GBA-bgnum layer colors (flat [4][pixels], index 0
    // = BG0 slot, always 0). Used by the compositor's alpha targetB walk.
    if (bgLayers != NULL)
        memset(bgLayers, 0, (size_t)(4 * bgLayerPixels) * sizeof(u16));

    for (sy = 0; sy < height; sy++)
    {
        for (sx = 0; sx < width; sx++)
        {
            u16 entries[3];
            bool32 ringWon;
            s32 worldX = viewportLeftX + sx;
            s32 worldY = viewportTopY + sy;
            s32 ringSx;
            s32 ringSy;
            u32 index = (u32)sy * (u32)width + sx;

            ringWon = ResolveBgTileEntries(snapshot, worldX, worldY, entries);
            if (IsInCentralWindow(snapshot, worldX, worldY))
            {
                ringSx = worldX - originX;
                ringSy = worldY - originY;
            }
            else if (IsWorldCoordinateResidentInRing(snapshot, worldX, worldY))
            {
                ringSx = worldX - anchorX - snapshot->bgHofs[0];
                ringSy = worldY - anchorY - snapshot->bgVofs[0];
            }
            else
            {
                // Grid/border won; ringWon is FALSE so the composite samples
                // worldX directly. Keep a defined value regardless.
                ringSx = worldX - originX;
                ringSy = worldY - originY;
            }
            outFrame[index] =
                CompositeMapPixelWithPriority(snapshot, entries, ringWon,
                                              ringSx, ringSy, worldX, worldY,
                                              bgLayers, bgLayerPixels, index,
                                              &bgWinner[index]);
        }
    }
    return TRUE;
}

bool32 NativeOverworldRenderer_DrawMapFrameWithMeta(const struct NativeOverworldSnapshot *snapshot,
                                                    u16 *outFrame, u8 *bgWinner,
                                                    u16 *bgLayers)
{
    s32 originX;
    s32 originY;

    if (!NativeOverworldSnapshotSupportsMap(snapshot)
     || outFrame == NULL || bgWinner == NULL)
        return FALSE;

    // The 240x160 view is the logical origin rectangle, so this wrapper is
    // exactly today's behavior (byte-identical output).
    originX = snapshot->cameraMapX * 16 + snapshot->cameraX;
    originY = snapshot->cameraMapY * 16 + snapshot->cameraY;
    return DrawMapFrameWithMetaEx(snapshot, outFrame, bgWinner, bgLayers,
                                  DISPLAY_WIDTH, DISPLAY_HEIGHT, originX, originY);
}

/*
 * Stage 4A runtime source diagnostics (§11), driven by
 * POKEEMERALD_NATIVE_EXPANDED_DEBUG=1. Logs the expanded viewport's static
 * configuration on activation (first expanded frame, size change, or map
 * transition) -- NEVER per camera pan, so the log is rate-limited by
 * construction. Contents: logical viewport size, expanded world top-left,
 * logical camera/focus, the live-ring world residency rectangle, the physical
 * ring index mapping (presentation scroll -> ring tile origin of the 32x32
 * torus), and the provider choice at the 4 corners and each margin midpoint.
 * Dev-only; never affects the presentation path.
 */
static void NativeOverworld_DebugLogExpandedState(
    const struct NativeOverworldSnapshot *bgSnap,
    const struct NativeViewport *viewport,
    s32 originX, s32 originY,
    s32 viewportLeftX, s32 viewportTopY)
{
    static bool32 sEnabled = FALSE;
    static bool32 sChecked = FALSE;
    static s32 sLastW = -1;
    static s32 sLastH = -1;
    static s32 sLastCamMapX = -1;
    static s32 sLastCamMapY = -1;
    u8 corners[4];
    u8 margins[4];
    // Keep the tracked ring-world coverage current (this runs before the map
    // render on the expanded path; UpdateRingCoverageWindow is idempotent, so a
    // second call from DrawMapFrameWithMetaEx is a no-op).
    UpdateRingCoverageWindow(bgSnap);
    // The ring's tracked world coverage window -- the 16x16-cell rectangle the
    // field renderer actually filled (see UpdateRingCoverageWindow) and what
    // residency tests against.
    s32 residencyMinX = sRingCoverageStartX;
    s32 residencyMinY = sRingCoverageStartY;
    s32 residencyMaxX = residencyMinX + NATIVE_BG_RING_PIXELS;
    s32 residencyMaxY = residencyMinY + NATIVE_BG_RING_PIXELS;

    if (!sChecked)
    {
        sEnabled = getenv("POKEEMERALD_NATIVE_EXPANDED_DEBUG") != NULL;
        sChecked = TRUE;
    }
    if (!sEnabled)
        return;

    // Rate-limit: log on activation / size change / map transition only.
    if (sLastW == viewport->width && sLastH == viewport->height
     && sLastCamMapX == bgSnap->cameraMapX && sLastCamMapY == bgSnap->cameraMapY)
        return;

    fprintf(stderr, "[POKEEMERALD_NATIVE_EXPANDED_DEBUG]\n");
    fprintf(stderr, "  logical viewport: %dx%d (margins %d,%d)\n",
            viewport->width, viewport->height,
            (viewport->width - DISPLAY_WIDTH) / 2,
            (viewport->height - DISPLAY_HEIGHT) / 2);
    fprintf(stderr, "  expanded world top-left: (%d,%d)\n",
            viewportLeftX, viewportTopY);
    fprintf(stderr, "  logical camera/focus: map=(%d,%d) phase=(%d,%d) origin=(%d,%d)\n",
            bgSnap->cameraMapX, bgSnap->cameraMapY,
            bgSnap->cameraX, bgSnap->cameraY, originX, originY);
    fprintf(stderr, "  live-ring world residency rect: [%d,%d) x [%d,%d)\n",
            residencyMinX, residencyMaxX, residencyMinY, residencyMaxY);
    fprintf(stderr, "  physical ring mapping: scroll=(%d,%d) -> ring tile origin=(%d,%d) in the 32x32 torus\n",
            bgSnap->bgHofs[0], bgSnap->bgVofs[0],
            (bgSnap->bgHofs[0] & 0xFF) >> 3,
            (bgSnap->bgVofs[0] & 0xFF) >> 3);

    if (NativeOverworldRenderer_ExpandedProviderCorners(
            bgSnap, viewportLeftX, viewportTopY,
            viewport->width, viewport->height, corners, margins))
    {
        fprintf(stderr, "  providers corners (TL/TR/BL/BR): %c %c %c %c\n",
                corners[0], corners[1], corners[2], corners[3]);
        fprintf(stderr, "  providers margin midpoints (L/R/T/B): %c %c %c %c\n",
                margins[0], margins[1], margins[2], margins[3]);
    }

    sLastW = viewport->width;
    sLastH = viewport->height;
    sLastCamMapX = bgSnap->cameraMapX;
    sLastCamMapY = bgSnap->cameraMapY;
}

bool32 NativeOverworldRenderer_DrawExpandedComposite(
    const struct NativeOverworldSnapshot *bgSnap,
    const struct NativeObjSnapshot *objSnap,
    struct NativeObjRenderOutput *objOut,
    const struct NativeViewport *viewport,
    u16 *bgFrame, u8 *bgWinner, u16 *bgLayers,
    u16 *outFrame, u8 *outWinner,
    struct NativeFieldCompositorReport *report)
{
    struct NativeCompositeBlendState blend;
    u8 bgPriority[4];
    u8 bg;
    s32 originX;
    s32 originY;
    s32 viewportLeftX;
    s32 viewportTopY;
    s32 marginX;
    s32 marginY;
    u32 pixelCount;

    if (report == NULL)
        return FALSE;
    memset(report, 0, sizeof(*report));
    if (bgSnap == NULL || objSnap == NULL || objOut == NULL || viewport == NULL
     || bgFrame == NULL || bgWinner == NULL || bgLayers == NULL
     || outFrame == NULL)
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_NULL_INPUT;
        return FALSE;
    }
    if (!NativeOverworldViewport_Active(viewport))
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_VIEWPORT_INVALID;
        return FALSE;
    }

    // Viewport placement is derived from the continuous viewport (no per-mode
    // logic): the margins (half the extra width/height) and the world top-left
    // shifted left/up by them. This is the same deterministic odd-rounding
    // NativeOverworldViewport_TopLeft computes (center - (width/2, height/2)),
    // so the expanded view shares the 240x160 view's world focus.
    originX = bgSnap->cameraMapX * 16 + bgSnap->cameraX;
    originY = bgSnap->cameraMapY * 16 + bgSnap->cameraY;
    marginX = (viewport->width - DISPLAY_WIDTH) / 2;
    marginY = (viewport->height - DISPLAY_HEIGHT) / 2;
    viewportLeftX = originX - marginX;
    viewportTopY = originY - marginY;
    pixelCount = (u32)viewport->width * (u32)viewport->height;

    // Stage 4A §11: POKEEMERALD_NATIVE_EXPANDED_DEBUG=1 runtime source
    // diagnostics (rate-limited; dev-only; never affects the frame).
    NativeOverworld_DebugLogExpandedState(bgSnap, viewport, originX, originY,
                                          viewportLeftX, viewportTopY);

    // BG capability gates (identical to DrawCompositeFrame): map support is
    // checked inside DrawMapFrameWithMetaEx; the BG0 overlay gate rejects any
    // frame whose BG0 would render a visible pixel.
    if (!DrawMapFrameWithMetaEx(bgSnap, bgFrame, bgWinner, bgLayers,
                                viewport->width, viewport->height,
                                viewportLeftX, viewportTopY))
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_BG_UNSUPPORTED;
        return FALSE;
    }
    if (NativeOverworld_SnapshotHasBg0Content(bgSnap))
    {
        report->bg0Content = 1;
        report->reason = NATIVE_COMPOSITE_FALLBACK_BG0_OVERLAY;
        return FALSE;
    }
    if (NativeField_BlendAffectsComposite(bgSnap->bldCnt, bgSnap->bldY,
                                          bgSnap->bldAlpha))
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_BLEND_EFFECT;
        return FALSE;
    }

    // OBJ capability gates from the Stage 4A viewport rasterize. RasterizeEx
    // returns FALSE on NULL input / 2D mapping (produced=FALSE) / bad viewport
    // dimensions (already gated above) -- each a named fallback.
    if (!NativeObjRender_RasterizeEx(objSnap, objOut, viewport->width,
                                     viewport->height, marginX, marginY))
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_OBJ_NOT_PRODUCED;
        return FALSE;
    }
    if (!objOut->produced)
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_OBJ_NOT_PRODUCED;
        return FALSE;
    }
    if (objOut->capabilityFlags != 0)
    {
        report->objCapabilityFlags = objOut->capabilityFlags;
        report->objRejectCount = objOut->commandRejectedCount;
        report->reason = NATIVE_COMPOSITE_FALLBACK_OBJ_UNSUPPORTED;
        return FALSE;
    }

    // BG priorities from the captured BGCNT values (BG0 never wins a supported
    // frame; its slot is filled but unused).
    for (bg = 0; bg < 3; bg++)
        bgPriority[bg + 1] = NativeOverworldSnapshotGetBgPriority(bgSnap, bg);
    bgPriority[0] = (u8)(bgSnap->bg0Cnt & 3);

    // Stage 3E blend state: frame-wide register captures + the backdrop color.
    blend.bldCnt = bgSnap->bldCnt;
    blend.bldAlpha = bgSnap->bldAlpha;
    blend.bldY = bgSnap->bldY;
    blend.dispCnt = bgSnap->dispCnt;
    blend.backdropColor = bgSnap->palette[0];

    if (!NativeFieldCompositor_CompositeEx(bgFrame, bgWinner, bgPriority,
                                           bgLayers, objOut, &blend,
                                           outFrame, outWinner, pixelCount))
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_NULL_INPUT;
        return FALSE;
    }
    report->reason = NATIVE_COMPOSITE_OK;
    return TRUE;
}

#endif

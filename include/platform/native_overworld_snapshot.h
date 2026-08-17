#ifndef GUARD_PLATFORM_NATIVE_OVERWORLD_SNAPSHOT_H
#define GUARD_PLATFORM_NATIVE_OVERWORLD_SNAPSHOT_H

#include "global.h"
#include "gba/defines.h"
#include "fieldmap.h"

/*
 * NativeOverworldSnapshot
 * ----------------------
 *
 * An immutable description of ONE coherent overworld presentation frame for
 * the PC-native map-background renderer. Emerald remains authoritative for all
 * simulation; this snapshot is a read-only capture taken after the frame's map,
 * tile-animation, VRAM, palette, and tilemap state has been finalized and
 * before any native rendering consumes it.
 *
 * Capture timing
 * --------------
 * Call NativeOverworld_CaptureSnapshot() at the top of the host video draw for
 * a frame, i.e. after CB2_Overworld -> OverworldBasic() has run for that frame:
 *
 *     ScriptContext_RunScript / RunTasks / AnimateSprites / CameraUpdate /
 *     UpdateCameraPanning / BuildOamBuffer / UpdatePaletteFade /
 *     UpdateTilesetAnimations / DoScheduledBgTilemapCopiesToVram
 *
 * DoScheduledBgTilemapCopiesToVram() has already flushed the live BG1/BG2/BG3
 * ring buffers into BG VRAM and UpdateTilesetAnimations() has materialized the
 * frame's animated tiles into BG character memory, so the captured state is the
 * exact state the GBA DrawFrame path will read. Capturing here does not alter
 * gameplay timing; it only reads state that the game has already settled.
 *
 * The mutable backup map grid, the live BG ring, BG VRAM, and BG palette RAM
 * are captured as bounded copies so the renderer never reads a mutable global
 * that could change mid-frame. Metatile tables, metatile attributes, and the
 * map border are immutable ROM data and are referenced by pointer.
 *
 * Coordinate convention
 * ---------------------
 * World pixels are pixels in the global map tile grid, where metatile (mx,my)
 * occupies world pixels [mx*16,(mx+1)*16) x [my*16,(my+1)*16).
 *
 * The snapshot carries TWO distinct origins, because the GBA presents the ring
 * at the latched scroll registers while the game's logic advances the camera:
 *
 *   - LOGICAL world origin: (cameraMapX*16 + cameraX, cameraMapY*16 + cameraY),
 *     where cameraX/cameraY are the GetCameraOffsetWithPan() result at capture.
 *     This is the game's model of where the window points. Screen pixel (sx,sy)
 *     is world pixel (logicalOriginX + sx, logicalOriginY + sy). The grid /
 *     border / connection fallback is resolved in this space.
 *
 *   - PRESENTATION origin: the latched BG scroll registers (bgHofs[bg],
 *     bgVofs[bg]). FieldUpdateBgTilemapScroll writes them from the camera in the
 *     VBlank handler, which runs AFTER OverworldBasic has advanced the logical
 *     camera, so on a moving frame the scroll is one frame of motion behind
 *     cameraX/cameraY. The GBA physically displays the ring sampled at these
 *     registers, and the DrawFrame oracle reproduces exactly that. The live BG
 *     ring is therefore indexed at the presentation origin, as the GBA samples
 *     it (32x32 text-mode ring, screen pixel (sx,sy)):
 *
 *         ringX = (bgHofs[bg - 1] + sx) & 0xFF        >> 3
 *         ringY = (bgVofs[bg - 1] + sy) & 0xFF        >> 3
 *         entry = bgRing[bg - 1][ringY*32 + ringX]
 *
 *         tile sub-pixel:  (bgHofs[bg - 1] + sx) & 7, (bgVofs[bg - 1] + sy) & 7
 *
 *   For a world pixel (wx,wy) the screen offset is sx = wx - logicalOriginX, so
 *   a query by world coordinate reduces to the same formula. On a stationary
 *   frame the scroll and the camera agree (HOFS == VOFS == camera offset) and
 *   both conventions coincide; on a moving frame the presentation origin is
 *   authoritative for the ring, the logical origin for grid lookup.
 */

// 32x32 text-mode ring per BG, one u16 tile entry per 8x8 tile.
#define NATIVE_BG_RING_SIZE 1024
#define NATIVE_BG_RING_SIDE_TILES 32
// World-pixel span of one ring side (32 tiles * 8 px). The live ring holds
// exactly a 256x256 world-pixel window; a world coordinate is ring-resident
// only inside that window (see IsWorldCoordinateResidentInRing).
#define NATIVE_BG_RING_PIXELS (NATIVE_BG_RING_SIDE_TILES * 8)

#define NATIVE_BG_VRAM_U16 (BG_VRAM_SIZE / 2)   // full BG character memory
#define NATIVE_PALETTE_U16 (PLTT_SIZE / 2)      // full BG + OBJ palette RAM
#define NATIVE_MAP_GRID_MAX MAX_MAP_DATA_SIZE   // mutable backup map grid

// Capabilities a scene can require. Stage 1 only renders the map background;
// future stages add sprites, blend/windows, affine, weather, and viewport
// extension capabilities. The native backend is used only when it supports
// every required capability.
enum NativeOverworldCapability
{
    NATIVE_CAPABILITY_MAP_BACKGROUND = (1 << 0), // BG3/BG2/BG1 field rendering
    NATIVE_CAPABILITY_LIVE_BG_RING   = (1 << 1), // live 32x32 BG ring present
    NATIVE_CAPABILITY_BG_VRAM        = (1 << 2), // BG character memory present
    NATIVE_CAPABILITY_BG_PALETTE     = (1 << 3), // BG palette RAM present

    // Reserved for later stages.
    NATIVE_CAPABILITY_SPRITES        = (1 << 4),
    NATIVE_CAPABILITY_WINDOWS        = (1 << 5),
};

// Why the native map-background renderer cannot safely render a frame. A
// non-NONE reason requests a GBA DrawFrame fallback without changing game
// state.
enum NativeOverworldFallbackReason
{
    NATIVE_FALLBACK_NONE = 0,
    NATIVE_FALLBACK_MISSING_LAYOUT,
    NATIVE_FALLBACK_MISSING_TILESETS,
    NATIVE_FALLBACK_MISSING_MAP_GRID,
    NATIVE_FALLBACK_MISSING_BG_RING,
    NATIVE_FALLBACK_MISSING_BG_VRAM,
    NATIVE_FALLBACK_MISSING_BG_PALETTE,
    NATIVE_FALLBACK_MISSING_CAMERA,
    NATIVE_FALLBACK_DISPLAY_MODE,
    NATIVE_FALLBACK_DISPLAY_LAYERS,
    NATIVE_FALLBACK_BG_CONFIG,
    // Internally inconsistent BG scroll registers. The field's VBlank latch
    // (FieldUpdateBgTilemapScroll) writes one value to BG1/2/3; a frame whose
    // three layers disagree has a torn presentation the native layer-coherent
    // model does not support. A UNIFORM scroll that merely differs from the
    // logical camera is NORMAL on a moving frame (the GBA presents the ring one
    // frame of motion behind the camera) and must NOT fall back.
    NATIVE_FALLBACK_BG_SCROLL,
    // Active blend that CAN change BG1/BG2/BG3 map-background pixels the native
    // renderer does not reproduce. A nonzero REG_BLDCNT alone is NOT a reason to
    // fall back: the field's persistent config (TGT1 empty, alpha blend) and any
    // EFFECT_NONE setup never alter map pixels, so only a config that actually
    // blends/brightens/darkens the map is rejected.
    NATIVE_FALLBACK_HARDWARE_BLEND,
    NATIVE_FALLBACK_WINDOWS,        // active window masking
    NATIVE_FALLBACK_MOSAIC,         // BG mosaic active
    NATIVE_FALLBACK_SCENE_NOT_OVERWORLD,
    // BG0 renders visible UI/window content (field message box, text windows) that
    // the oracle includes but the native map-background renderer does not reproduce.
    NATIVE_FALLBACK_BG0_OVERLAY,
    // The software BG ring (gOverworldTilemapBuffer_BgN, captured as bgRing) has
    // diverged from the live VRAM screenbase tilemap the GBA composite oracle
    // samples. The game flushes the ring into the screenbase each settled frame
    // (DoScheduledBgTilemapCopiesToVram), so the two agree on every normal frame;
    // a frame where a visible 240x160 tile differs is mid-transition (map load,
    // fade, or a tilemap DMA deferred past the capture point) and the native
    // renderer would present tilemap content the GBA does not. Falling back with
    // this reason turns those transitional frames into a named fallback instead
    // of comparing two different tilemaps.
    NATIVE_FALLBACK_BG_TILEMAP_IN_FLIGHT,
    NATIVE_FALLBACK_REASON_COUNT,
};

struct NativeOverworldSnapshot
{
    // --- Camera / world origin (Stage 1 renders the exact 240x160 window) ---
    s32 cameraMapX;  // gSaveBlock1Ptr->pos.x
    s32 cameraMapY;  // gSaveBlock1Ptr->pos.y
    s16 cameraX;     // GetCameraOffsetWithPan() horizontal offset (LOGICAL origin)
    s16 cameraY;     // GetCameraOffsetWithPan() vertical offset (LOGICAL origin)

    // --- Map identity / layout (immutable ROM data, referenced not copied) ---
    s32 mapWidth;               // layout width in metatiles
    s32 mapHeight;              // layout height in metatiles
    const u16 *primaryMetatiles;
    const u16 *secondaryMetatiles;
    const u16 *primaryMetatileAttributes;
    const u16 *secondaryMetatileAttributes;
    const u16 *border;          // repeating 2x2 map border

    // --- Mutable backup map grid (bounded copy) ---
    bool32 mapGridValid;
    s32 gridWidth;              // layout width + MAP_OFFSET_W
    s32 gridHeight;             // layout height + MAP_OFFSET_H
    u16 mapGrid[NATIVE_MAP_GRID_MAX];

    // --- Final live BG ring tilemaps (bounded copy) ---
    // [0]=BG1, [1]=BG2, [2]=BG3. Highest-precedence override in the world
    // tile provider; preserves transient writes (doors, direct tilemap edits).
    bool32 bgRingValid;
    u16 bgRing[3][NATIVE_BG_RING_SIZE];

    // --- Current BG character memory and palette RAM (bounded copies) ---
    // Carries animated tiles and palette changes already materialized by
    // Emerald. bgVram is byte-addressable through (u8*)bgVram.
    bool32 bgVramValid;
    u16 bgVram[NATIVE_BG_VRAM_U16];
    u16 palette[NATIVE_PALETTE_U16];

    // --- Relevant BG control / scroll registers (diagnostics + validation) ---
    u16 bgCnt[3];   // [0]=BG1CNT, [1]=BG2CNT, [2]=BG3CNT
    // The latched BG scroll registers -- the PRESENTATION origin the ring is
    // sampled at (see the coordinate-convention comment above). On a moving
    // frame these lag the logical camera by one frame of motion.
    u16 bgHofs[3];  // [0]=BG1HOFS, [1]=BG2HOFS, [2]=BG3HOFS
    u16 bgVofs[3];  // [0]=BG1VOFS, [1]=BG2VOFS, [2]=BG3VOFS
    u16 dispCnt;

    // --- Blend registers (diagnostics for the hardware-blend fallback) ---
    // Captured verbatim from the live registers when the parity gate rejects a
    // frame so the one-time skip diagnostic can report exactly which blend setup
    // was present and why it is unsupported.
    u16 bldCnt;
    u16 bldAlpha;
    u16 bldY;

    // --- Capability / fallback state ---
    enum NativeOverworldCapability requiredCapabilities;
    enum NativeOverworldFallbackReason fallbackReason;

    // BG0CNT, captured verbatim from REG_BG0CNT so the BG0-content gate
    // (NativeOverworld_SnapshotHasBg0Content) can be validated offline from a
    // serialized snapshot. Appended last so existing serialized fixtures keep
    // their byte layout for every preceding field.
    u16 bg0Cnt;

    // --- RING WORLD ANCHOR (Stage 4A moving-world coherency) ---
    // The tilemap ring is a 256x256 world-pixel window written by the field
    // renderer. Its world anchor is NOT cameraMapX/cameraMapY: the ring's
    // coverage shifts by one 8px tile column/row every time the field renderer
    // crosses an 8px boundary (xTileOffset/yTileOffset increments), and the
    // anchor therefore derives from the tile offsets captured here:
    //
    //     anchorX = cameraMapX*16 - cameraXTileOffset*8   (s32, NOT &0xFF)
    //     anchorY = cameraMapY*16 - cameraYTileOffset*8
    //
    // At load (tile offsets 0) the ring covers world px
    // [cameraMapX*16, cameraMapX*16+256); after k east crossings it covers
    // [anchorX, anchorX+256) with anchorX = pos.x*16 - k*8, i.e. the ring is
    // filled with the world rectangle anchored at the FIRST tile the renderer
    // drew (cells cameraMapX-1..cameraMapX+14 once k>=1). Residency, margin
    // ring sampling, and the physical cell lookup must all use this anchor.
    // Captured from the field camera's sFieldCameraOffset via
    // GetCameraTileOffsets(). Appended after bg0Cnt so older serialized
    // fixtures read these as zero (valid for their k=0 captures).
    u8 cameraXTileOffset;
    u8 cameraYTileOffset;
};

#endif // GUARD_PLATFORM_NATIVE_OVERWORLD_SNAPSHOT_H

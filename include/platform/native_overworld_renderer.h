#ifndef GUARD_PLATFORM_NATIVE_OVERWORLD_RENDERER_H
#define GUARD_PLATFORM_NATIVE_OVERWORLD_RENDERER_H

#include "global.h"
#include "platform/native_overworld_snapshot.h"
#include "platform/native_overworld_viewport.h"

// Aliases so desktop_video.c buffer declarations (unchanged) stay valid; the
// authoritative limits now live in native_overworld_viewport.h.
#define NATIVE_OVERWORLD_MAX_WIDTH  NATIVE_VIEWPORT_MAX_WIDTH
#define NATIVE_OVERWORLD_MAX_HEIGHT NATIVE_VIEWPORT_MAX_HEIGHT

/*
 * BGCNT capability contract for the map-background renderer.
 *
 * The renderer reproduces the GBA's BG1/BG2/BG3 composite from a snapshot and
 * depends on exactly these BGCNT fields per layer:
 *   - priority 1/2/3  (fixed layer draw order, BG1 on top of BG2 on top of BG3)
 *   - charbase 0      (tile numbers index into BG VRAM from its base)
 *   - 16-color        (4bpp, 32 bytes per tile)
 *   - 256x256         (ring tilemap wraps at 32x32)
 * It does NOT depend on the screenbase: the tilemap is sampled from the live
 * overworld ring buffers (gOverworldTilemapBuffer_Bg1/2/3), and the ring is
 * what gets copied to whatever screenbase BGCNT points at each worker frame,
 * so the two always agree. It also does not depend on the BG mosaic flag,
 * because REG_MOSAIC is separately required to be zero. Gates therefore mask
 * each BGCNT to these capability bits instead of requiring an exact value.
 */
#define NATIVE_BGCNT_CAPABILITY_MASK \
    (BGCNT_PRIORITY(3) | BGCNT_CHARBASE(3) | BGCNT_256COLOR \
     | BGCNT_TXT512x256 | BGCNT_TXT256x512 | BGCNT_TXT512x512)
#define NATIVE_BGCNT_CAPABILITY_BG1 \
    (BGCNT_PRIORITY(1) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_TXT256x256)
#define NATIVE_BGCNT_CAPABILITY_BG2 \
    (BGCNT_PRIORITY(2) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_TXT256x256)
#define NATIVE_BGCNT_CAPABILITY_BG3 \
    (BGCNT_PRIORITY(3) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_TXT256x256)

/*
 * Stage 0/1 map-background backend.
 *
 * NativeOverworld_CaptureSnapshot() must be called at the documented capture
 * point (top of the host video draw for a frame, after the game has finalized
 * its tile animation, VRAM, palette, and tilemap state). It fills an immutable
 * snapshot and a capability/fallback report. NativeOverworldRenderer_DrawMapFrame()
 * then renders the complete 240x160 map background from the snapshot alone; no
 * DrawFrame pixel is used by that path.
 */
bool32 NativeOverworld_CaptureSnapshot(struct NativeOverworldSnapshot *snapshot);
/*
 * Stage-2 runtime parity capture.
 *
 * Same snapshot body as NativeOverworld_CaptureSnapshot(), but gated by the
 * lean Overworld_IsNativeParityCaptureEligible() predicate instead of the
 * zoom/expanded renderer predicate. This is the capture the runtime parity
 * harness uses; it must not depend on whether the old experimental expanded
 * renderer can activate for a frame. Runs in the field with BG0 enabled (the
 * DISPLAY_LAYERS check requires only BG1/BG2/BG3 on).
 */
bool32 NativeOverworld_CaptureParitySnapshot(struct NativeOverworldSnapshot *snapshot);
bool32 NativeOverworldRenderer_DrawMapFrame(const struct NativeOverworldSnapshot *snapshot,
                                            u16 *outFrame);

/*
 * Stage 3C: priority-aware map renderer with per-pixel BG-winner metadata.
 *
 * Produces the same 240x160 BG frame as NativeOverworldRenderer_DrawMapFrame
 * PLUS a per-pixel BG-winner byte: 0=backdrop, 2=BG1, 3=BG2, 4=BG3 (BG0 is
 * never rendered by the native path). Unlike DrawMapFrame -- which merges the
 * three layers in the field's fixed BG1>BG2>BG3 order -- the merge here uses
 * the CAPTURED BGCNT priorities, so reordered BGCNT priorities (and equal-
 * priority ties, broken by lower bgnum) compose exactly as the GBA compositor
 * does. On the fixed field priorities (BG1=1, BG2=2, BG3=3) the output is
 * byte-identical to DrawMapFrame. bgWinner is required and must be a
 * DISPLAY_WIDTH*DISPLAY_HEIGHT u8 buffer. bgLayers is optional (Stage 3E): when
 * non-NULL it receives the flat per-GBA-bgnum layer colors [4][pixels] in the
 * oracle's scanline.layers[bgnum] encoding (bit 15 = opaque, 0 = transparent) --
 * slot 0 (BG0) is always 0, slots 1/2/3 are BG1/2/3. Returns FALSE on the same
 * inputs DrawMapFrame rejects (unsupported snapshot / NULL).
 */
bool32 NativeOverworldRenderer_DrawMapFrameWithMeta(const struct NativeOverworldSnapshot *snapshot,
                                                    u16 *outFrame, u8 *bgWinner,
                                                    u16 *bgLayers);

/*
 * BGCNT draw priority (0-3) of a captured BG layer. bg is the native renderer's
 * index: 0=BG1, 1=BG2, 2=BG3. Stage 3C composes from these captured values
 * rather than hardcoding layer order.
 */
u8 NativeOverworldSnapshotGetBgPriority(const struct NativeOverworldSnapshot *snapshot,
                                        u8 bg);

/*
 * Predicate for the runtime parity capture gate: can this blend configuration
 * change BG1/BG2/BG3 map-background pixels (or the backdrop) in the BG-only
 * GBA oracle the native renderer must reproduce?
 *
 * The GBA DrawFrame oracle renders BG0/BG1/BG2/BG3 and the backdrop but EXCLUDES
 * OBJ, so:
 *   - EFFECT_NONE (bits 6-7 == 00) never changes a pixel.
 *   - EFFECT_BLEND (01) blends the top TGT1 pixel at a position with the
 *     topmost TGT2 pixel below it. A map pixel changes only where an
 *     oracle-rendered layer (BG0/BG1/BG2/BG3) is in TGT1 AND an oracle-rendered
 *     TGT2 target (BG0-BG3 or backdrop) is present below it. OBJ is masked, so
 *     OBJ-only TGT2 can never fire; EVA==16/EVB==0 reproduces target A exactly.
 *   - EFFECT_LIGHTEN/DARKEN (10/11) brightens/darkens the top TGT1 pixel by
 *     BLDY. BLDY==0 is a no-op; OBJ is masked. The backdrop is a valid TGT1
 *     target for these effects (the oracle fills it per scanline).
 *
 * This is what makes the field's persistent blend config (BLDCNT=0x1E40, TGT1
 * empty, alpha mode) legal: with no oracle-rendered layer in TGT1 the blend
 * never fires and the map renders unblended, identical to REG_BLDCNT == 0.
 */
bool32 NativeOverworld_BlendAffectsMapBackground(u16 bldCnt, u16 bldY, u16 bldAlpha);

/*
 * True when the snapshot's BG0 layer would render a visible pixel in the frame
 * (field message box, text windows). The GBA oracle includes BG0; the native
 * map-background renderer does not, so such frames must fall back. Opacity-aware:
 * cleared field windows leave transparent tile-0 (0xE000) entries that do NOT
 * count, so clean frames are not over-rejected. See the implementation for the
 * sampling rules.
 */
bool32 NativeOverworld_SnapshotHasBg0Content(const struct NativeOverworldSnapshot *snapshot);

/*
 * Stage 4A dev-only provider visualization (§6). Classifies each region of an
 * expanded viewport by the world-tile provider that serves it:
 *
 *   R  the live ring won (coordinate ring-eligible: central 240x160 or
 *      resident in the ring's 256x256 world window)
 *   G  the backup map grid won (defined grid cell inside the real map rect)
 *   C  the grid's connection strip won (defined cell outside the real map
 *      rect -- the cardinal apron copied at map load)
 *   B  the repeating border won (out of grid, or undefined grid cell)
 *
 * These are DIAGNOSTIC ONLY: the presentation path never calls them and they
 * never alter presentation colors. The classification is derived from the same
 * eligibility + metatile logic the renderer resolves with, so the map shows
 * exactly what each pixel displays.
 */
#define NATIVE_OVERWORLD_PROVIDER_RING       'R'
#define NATIVE_OVERWORLD_PROVIDER_GRID       'G'
#define NATIVE_OVERWORLD_PROVIDER_CONNECTION 'C'
#define NATIVE_OVERWORLD_PROVIDER_BORDER     'B'

/* Provider codes for the 4 expanded corners (TL/TR/BL/BR) and the midpoints of
 * the 4 margins (L/R/T/B); a zero margin reports BORDER at its midpoint. */
bool32 NativeOverworldRenderer_ExpandedProviderCorners(
    const struct NativeOverworldSnapshot *bgSnap,
    s32 viewportLeftX, s32 viewportTopY,
    s32 viewportWidth, s32 viewportHeight,
    u8 *outCorners, u8 *outMargins);

/* Classifies every `regionPx`-square region of the viewport by the provider
 * serving its center (regionPx is 8 or 16); fills outRegionProvider
 * (rows*cols codes) and reports the region dimensions. */
bool32 NativeOverworldRenderer_ExpandedProviderRegionMap(
    const struct NativeOverworldSnapshot *bgSnap,
    s32 viewportLeftX, s32 viewportTopY,
    s32 viewportWidth, s32 viewportHeight,
    s32 regionPx,
    u8 *outRegionProvider,
    s32 *outRegionCols, s32 *outRegionRows);

/* Prints the region map as an ASCII grid (one char per region). */
void NativeOverworldRenderer_DumpProviderRegionMap(const u8 *regionProvider,
                                                   s32 cols, s32 rows);

/* Fills a viewport-sized RGB555 framebuffer with a SOLID color per provider
 * region (R=green, G=blue, C=yellow, B=red), one regionPx block per region. */
bool32 NativeOverworldRenderer_FillProviderDebugImage(
    const struct NativeOverworldSnapshot *bgSnap,
    s32 viewportLeftX, s32 viewportTopY,
    s32 viewportWidth, s32 viewportHeight,
    s32 regionPx, u16 *outFrame);

/*
 * Stage 4A Issue B stable-canvas fallback (directive 9-12/14): fills a
 * viewport-sized ARGB framebuffer (outCanvas) with the AUTHORITATIVE
 * coreFrame (240x160) centered 1:1 and PURE-BLACK margins.
 *
 * Why this shape: when the expanded composite cannot draw while the user has
 * SELECTED a continuous viewport (viewportActive), the presentation must stay
 * at the SELECTED canvas size -- a capability failure must not zoom the picture
 * by stretching the 240x160 core to fill the window. The canvas is exactly the
 * expanded render's dimensions, so the presented scale never changes on a
 * fallback; only the CONTENT MODE changes (native expanded world -> authoritative
 * 240x160 core + black margins).
 *
 * Invariants:
 *  - The SELECTED viewport is never mutated (read-only input; caller owns it).
 *  - Every margin pixel is freshly written 0xFF000000 -- never a stale expanded
 *    pixel from a previous frame.
 *  - No texture is created or resized here; desktop_video.c reuses the existing
 *    expanded texture, so SDL never recreates the 240x160 texture for a fallback.
 *
 * coreWidth/coreHeight are the authoritative frame's dimensions (240x160);
 * viewport supplies canvasWidth/canvasHeight; margins are
 * ((width - coreWidth)/2, (height - coreHeight)/2). Returns FALSE on NULL /
 * degenerate (margin-negative) input; on success outCanvas has exactly
 * width*height pixels and is fully written (no stale rows).
 */
bool32 NativeOverworldRenderer_BuildFallbackCanvas(
    const u32 *coreFrame, s32 coreWidth, s32 coreHeight,
    const struct NativeViewport *viewport,
    u32 *outCanvas);

#endif // GUARD_PLATFORM_NATIVE_OVERWORLD_RENDERER_H

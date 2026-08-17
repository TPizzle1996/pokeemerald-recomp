#ifndef GUARD_PLATFORM_NATIVE_OBJ_RENDERER_H
#define GUARD_PLATFORM_NATIVE_OBJ_RENDERER_H

#include "global.h"
#include "platform/native_sprite_snapshot.h"
#include "platform/native_overworld_viewport.h"

/*
 * Stage 3B/3D: native OBJ sampler (non-affine + affine).
 *
 * A CPU rasterizer that consumes a captured NativeObjSnapshot and produces an
 * OBJ-ONLY raster: four per-OBJ-priority layers (0 = foremost .. 3), each with
 * the winning pre-blend OBJ color and a compact ownership marker per screen
 * pixel. It deliberately does NOT composite with BG, apply blend/window/mosaic,
 * or touch the production presentation path -- Stage 3C merges this output with
 * the parity-hardened BG frame. Stage 3D added affine OBJ (normal + double-size)
 * with exact gba_easy_draw.c sampling semantics (see the .c file).
 *
 * AUTHORITY AND TIMING:
 *
 *   - The final packed OAM (snap->finalOam) is the 240x160 authority, exactly
 *     like the gba_easy_draw.c oracle: coordinates are canonicalized from the
 *     packed 9-bit/8-bit fields, NOT from the pre-wrap signedX/signedY. The
 *     snapshot's signed/pre-wrap fields are preserved for future expanded
 *     viewports and are never destroyed here -- they simply do not drive 240x160
 *     clipping. (E.g. signedX=280 packs to OAM x=280, which the 240x160
 *     interpretation canonicalizes to -232; parity requires -232.)
 *   - Exact OAM ordering: primitives are rasterized by OAM index 127..0 with
 *     overwrite semantics, so at equal OBJ priority the lowest OAM index wins
 *     the pixel -- identical to the oracle's scanline loop. No host-side
 *     priority/subpriority re-sort is performed.
 *   - Provenance (spriteId, partIndex, sourceKind, ...) is attached from the
 *     published Stage 3A command stream via the oamIndex -> command map, purely
 *     for diagnostics and the sample trace. Geometry and pixels always come
 *     from finalOam + captured OBJ VRAM + OBJ palette.
 *
 * CAPABILITY BOUNDARY:
 *
 *   Stage 3B/3D/3E renders 4bpp, 1D-mapped OBJ -- non-affine with H/V flips and
 *   affine (normal + double-size, exact 8.8 center-origin sampling, matrix read
 *   from the snapshot's own finalOam) -- for all legal shape/size combinations,
 *   transparent index 0, and exact 240x160 clipping. Semi-transparent objMode==1
 *   (Stage 3E) is sampled with identical geometry/texel/priority semantics and
 *   its per-pixel mode is preserved in NativeObjLayer.semi[]; the sampler does
 *   NOT perform the alpha blend itself -- the Stage 3E compositor does, at
 *   composite time, from the per-BG-layer colors and the BLDCNT/BLDALPHA
 *   registers (exactly where gba_easy_draw.c blends the winning semi OBJ pixel).
 *   Any presented (would-draw) primitive that uses an unsupported feature --
 *   8bpp, OBJ-window objMode, mosaic, or a prohibited shape -- is skipped and
 *   recorded as an explicit reject with a capability flag. Offscreen entries
 *   are never capability-rejected (a reserved offscreen affine matrix must not
 *   force a fallback). A frame with 2D OBJ mapping is not producible at all and
 *   returns produced=FALSE with OBJ_MAPPING_2D.
 *
 * The output struct is large (4 layers x 240x160 x 2 buffers); it is
 * caller-allocated (tests use a static global) and is never embedded in the
 * production frame path.
 */

#define NATIVE_OBJ_PRIORITY_LAYERS 4

// Snapshot-level / per-primitive capability flags (bitmask). Only set when a
// presented, would-draw primitive (or the display config) uses the feature.
enum NativeObjCapabilityFlags
{
    NATIVE_OBJ_CAP_NONE           = 0,
    NATIVE_OBJ_CAP_2D_MAPPING     = (1 << 0), // DISPCNT_OBJ_1D_MAP clear (frame-wide)
    NATIVE_OBJ_CAP_8BPP           = (1 << 1),
    NATIVE_OBJ_CAP_AFFINE         = (1 << 2),
    NATIVE_OBJ_CAP_AFFINE_DOUBLE  = (1 << 3), // affine with double-size bounds
    NATIVE_OBJ_CAP_OBJ_WINDOW     = (1 << 4), // OBJ-window objMode==2
    NATIVE_OBJ_CAP_MOSAIC         = (1 << 5),
    NATIVE_OBJ_CAP_PROHIBITED_SHAPE = (1 << 6),
    // Semi-transparent objMode==1 is SUPPORTED (Stage 3E) and is never a
    // capability reject; the alpha blend happens in the compositor. The only
    // blend-related reject is BLEND_CONFIG: an alpha/brightness blend that would
    // recolor NORMAL OBJ (TGT1_OBJ set with a non-zero blend mode) -- that raster
    // stays pre-blend and falls back.
    NATIVE_OBJ_CAP_BLEND_CONFIG   = (1 << 7), // alpha blend would recolor normal OBJ
};

// Per-primitive reject reason (diagnostic; pairs with a capability flag).
enum NativeObjRejectReason
{
    NATIVE_OBJ_REJECT_NONE = 0,
    NATIVE_OBJ_REJECT_AFFINE,
    NATIVE_OBJ_REJECT_AFFINE_DOUBLE,
    NATIVE_OBJ_REJECT_8BPP,
    // Semi-transparent objMode==1 is supported (Stage 3E) and has no reject
    // reason; the unsupported-blend case is the frame-level BLEND_CONFIG flag.
    NATIVE_OBJ_REJECT_OBJ_WINDOW,
    NATIVE_OBJ_REJECT_MOSAIC,
    NATIVE_OBJ_REJECT_PROHIBITED_SHAPE,
    // Non-affine + double-size entries are oracle-disabled and skipped silently
    // (no reject record) exactly like the oracle's `continue`.
    NATIVE_OBJ_REJECT_REASON_COUNT,
};

struct NativeObjRejectRecord
{
    u8 oamIndex;
    u8 reason;      // enum NativeObjRejectReason
    u8 commandId;   // provenance command, NATIVE_OBJ_NO_ID if unmapped
    u8 reserved;
};

// Compact per-pixel ownership marker (u16):
//   bit 15    present -- an OBJ pixel claimed this screen pixel in this layer
//   bit 14    sourceKind: 1 = RAW_OAM, 0 = GSPRITE
//   bits 13-12 OBJ priority (0-3)
//   bits 7-0   winning OAM index (0-127)
#define NATIVE_OBJ_OWN_PRESENT        0x8000u
#define NATIVE_OBJ_OWN_RAW            0x4000u
#define NATIVE_OBJ_OWN_PRIORITY_SHIFT 12u
#define NATIVE_OBJ_OWN_PRIORITY_MASK  0x3000u
#define NATIVE_OBJ_OWN_OAM_SHIFT       0u
#define NATIVE_OBJ_OWN_OAM_MASK       0x00FFu

// One OBJ priority layer. color[] mirrors the oracle's spriteLayers: a winning
// pre-blend BGR555 color with bit 15 set (the oracle's presence marker), 0 where
// no OBJ claimed the pixel. ownership[] is the packed marker above. semi[i] is 1
// exactly where the winning primitive at i was semi-transparent (objMode==1);
// the compositor alpha-blends those pixels against the BG layers below, mirroring
// the oracle's `|| isSemiTransparent` blend at OBJ draw time.
//
// Stage 4A: the arrays are sized to the MAXIMUM native viewport (360x240), NOT
// the 240x160 GBA display, so the same raster can serve the 240x160 center and
// an expanded viewport (the 240x160 path simply touches the top-left 240x160
// sub-rectangle of each layer). Caller-allocated only; never on the stack.
struct NativeObjLayer
{
    u16 color[NATIVE_VIEWPORT_MAX_WIDTH * NATIVE_VIEWPORT_MAX_HEIGHT];
    u16 ownership[NATIVE_VIEWPORT_MAX_WIDTH * NATIVE_VIEWPORT_MAX_HEIGHT];
    u8  semi[NATIVE_VIEWPORT_MAX_WIDTH * NATIVE_VIEWPORT_MAX_HEIGHT];
};

struct NativeObjRenderOutput
{
    u32 capabilityFlags;        // bitmask of every unsupported feature used
    u8 snapshotValid;           // informational: snap->valid
    u8 produced;                // TRUE when a supported OBJ-only raster was made
    u8 reserved0;
    u8 reserved1;
    u16 commandRejectedCount;   // rejected presented primitives
    u16 gspriteDrawnCount;      // drawn primitives with GSPRITE provenance
    u16 rawDrawnCount;          // drawn primitives with RAW_OAM provenance
    struct NativeObjRejectRecord rejects[NATIVE_SPRITE_COMMAND_MAX];
    struct NativeObjLayer layers[NATIVE_OBJ_PRIORITY_LAYERS]; // priority 0..3
};

// Per-pixel on-demand sample trace (recomputed, not stored per pixel).
struct NativeObjSampleTrace
{
    bool32 present;
    u8 priority;
    u8 oamIndex;        // winning final OAM entry
    u8 commandId;       // provenance command (NATIVE_OBJ_NO_ID if none)
    u8 sourceKind;      // enum NativeSpriteSourceKind
    u8 spriteId;        // source gSprite (NATIVE_OBJ_NO_ID for raw)
    u8 partIndex;
    u8 paletteNum;
    u8 pixelIndex;      // 4-bit palette index fetched from OBJ VRAM
    u16 tileNum;        // final (subsprite-adjusted) tile number
    u8 texX;            // source texel coordinate (after flip)
    u8 texY;
    u8 shape;
    u8 size;
    u8 affineMode;
    u8 objMode;
    u8 mosaic;
    u8 bpp;
    u8 flipX;
    u8 flipY;
    u16 color;          // pre-blend BGR555 (bit 15 clear)
};

/*
 * Rasterize the snapshot's OBJ primitives into the caller-provided output
 * (out->layers[priority]). Returns FALSE only on NULL input or when the
 * snapshot's display config uses 2D OBJ mapping (out->produced=FALSE,
 * capabilityFlags |= OBJ_MAPPING_2D, no layers touched). Unsupported
 * primitives are skipped and recorded; supported ones are rasterized with exact
 * oracle semantics.
 */
bool32 NativeObjRender_Rasterize(const struct NativeObjSnapshot *snap,
                                 struct NativeObjRenderOutput *out);

/*
 * Stage 4A: rasterize into an ARBITRARY viewport rectangle. marginX/marginY is
 * how far the viewport's top-left sits left/top of the 240x160 origin
 * ((width-240)/2, (height-160)/2). Rasterize() is the 240x160 wrapper.
 *
 * Placement in an expanded viewport follows the Stage 3A pre-wrap/presentation
 * commands, not just the wrapped OAM: a validated GSPRITE command with
 * WORLD/CAMERA_RELATIVE placement and an in-range signedX/signedY
 * ([-272,239]/[-176,159] -- where signedX/Y == canonicalized final OAM) is
 * placed at signedX+margin (its world position moves outward). RAW_OAM / UNKNOWN
 * / out-of-range commands are placed at canonicalized-OAM+margin (they stay in
 * the 240x160 center: SCREEN_FIXED policy). Sprites culled by the vanilla
 * 240x160 viewport but marked expandedEligible are revealed from the command's
 * pre-wrap geometry (affine matrix from the command's own pa/pb/pc/pd -- the
 * sink-time gOamMatrices, a documented <=1-frame approximation); they rasterize
 * AFTER all final OAM entries and never overwrite a presented pixel at equal
 * priority. Capability rejects (8bpp/OBJ-window/mosaic) apply to any presented
 * or revealed primitive intersecting the viewport.
 */
bool32 NativeObjRender_RasterizeEx(const struct NativeObjSnapshot *snap,
                                   struct NativeObjRenderOutput *out,
                                   u16 viewportWidth, u16 viewportHeight,
                                   s32 marginX, s32 marginY);

/*
 * Reconstruct the sample for one screen pixel of one OBJ priority layer from
 * the ownership marker (winning OAM index), replaying the sampler's geometry
 * and OBJ VRAM fetch. Returns TRUE and fills *trace when the pixel is claimed;
 * FALSE when it is unclaimed or arguments are NULL.
 */
bool32 NativeObjRender_Trace(const struct NativeObjSnapshot *snap,
                             const struct NativeObjRenderOutput *out,
                             u8 priority, u8 x, u8 y,
                             struct NativeObjSampleTrace *trace);

#endif // GUARD_PLATFORM_NATIVE_OBJ_RENDERER_H

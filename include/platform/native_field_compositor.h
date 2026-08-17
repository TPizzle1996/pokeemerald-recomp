#ifndef GUARD_PLATFORM_NATIVE_FIELD_COMPOSITOR_H
#define GUARD_PLATFORM_NATIVE_FIELD_COMPOSITOR_H

#include "global.h"
#include "platform/native_overworld_snapshot.h"
#include "platform/native_obj_renderer.h"

/*
 * Stage 3C: exact native BG + normal-OBJ final compositing at 240x160.
 *
 * This module composes the PROVEN Stage 2 native BG frame (with its per-pixel
 * BG-winner metadata) and the PROVEN Stage 3B normal-OBJ layers into the final
 * GBA frame, selecting the same per-pixel winner the real gba_easy_draw.c
 * composite selects. It never replaces those proven outputs -- it merges them.
 *
 * FINAL-PIXEL WINNER RULES (the exact rules the GBA compositor applies, from
 * gba_easy_draw.c DrawScanline; authoritative for this module):
 *
 *   1. Per scanline the backdrop (PLTT[0]) is pre-filled; anything drawn
 *      overwrites it. Backdrop is "priority infinity": behind every BG and OBJ.
 *   2. The compositor draws priorities 3 down to 0 (LOWER priority number wins,
 *      processed later). Within one priority the BGs of that priority are drawn
 *      first, highest bgnum first, then the OBJ of that priority -- so at equal
 *      priority the OBJ beats the BG, and among BGs at equal priority the LOWER
 *      bgnum (BG0 < BG1 < BG2 < BG3) wins.
 *   3. Net winner per pixel:
 *        - BG candidate: the opaque BG with lexicographically smallest
 *          (priority, bgnum). Transparent BG pixels (alpha bit clear) never draw.
 *        - OBJ candidate: the lowest OBJ-priority layer (0 = foremost) with an
 *          opaque pixel; the Stage 3B layers already resolve lowest-OAM-wins
 *          within a priority.
 *        - OBJ wins over the BG candidate iff OBJ priority <= BG priority
 *          (backdrop priority = 4, so any OBJ beats it).
 *
 * The per-pixel winner encoding below is byte-identical to the GBA composite
 * oracle seam (layerOut in DrawScanline): 0 = backdrop, 1-4 = BG0-3, 5-8 = OBJ
 * priority 0-3. The native side can never emit 1 (BG0) because the BG0 overlay
 * gate falls back on BG0 content, so supported frames agree with the oracle on
 * every pixel.
 *
 * CAPABILITY BOUNDARY (same philosophy as Stage 3B): this module only composes
 * what is proven. It does not implement affine / affine-double OBJ, 8bpp,
 * OBJ-window, mosaic, general BG/normal-OBJ alpha blending, brightness, or
 * OBJ/BG window masking. Any of these (surfaced via the Stage 3B
 * capabilityFlags or the composite blend predicate) makes the frame fall back
 * with an explicit reason -- a frame with unsupported features is never
 * partially composed and silently presented.
 *
 * Stage 3E ADDS semi-transparent OBJ (objMode==1): a winning semi OBJ pixel is
 * alpha-blended exactly as the oracle blends it at OBJ draw time (EVA/EVB from
 * BLDALPHA, BG-only targetB walk from the OBJ's priority with the oracle's
 * bail-on-opaque-non-TGT2 rule, backdrop fallback on TGT2_BD). Supported blend
 * state is exactly the frames NativeField_BlendAffectsComposite lets through
 * (no-TGT1 configs like the field's persistent 0x1E40); frames whose blend
 * would recolor a normal OBJ or BG pixel still fall back.
 *
 * The compositor is logical-240x160 only. It is not wired into frontend
 * scaling, zoom, or widescreen; those concerns are explicitly out of scope.
 */

/*
 * Per-pixel final-pixel winner. Values are identical to the GBA composite
 * oracle seam's layerOut encoding so a native composite and the real DrawFrame
 * can be compared winner-for-winner.
 */
enum NativeCompositeWinner
{
    NATIVE_COMPOSITE_WIN_BACKDROP = 0,
    NATIVE_COMPOSITE_WIN_BG0      = 1, // never emitted by the native BG renderer
    NATIVE_COMPOSITE_WIN_BG1      = 2,
    NATIVE_COMPOSITE_WIN_BG2      = 3,
    NATIVE_COMPOSITE_WIN_BG3      = 4,
    NATIVE_COMPOSITE_WIN_OBJ0     = 5, // OBJ priority 0 (foremost)
    NATIVE_COMPOSITE_WIN_OBJ1     = 6,
    NATIVE_COMPOSITE_WIN_OBJ2     = 7,
    NATIVE_COMPOSITE_WIN_OBJ3     = 8,
};

/* Explicit reason a frame was NOT composited (report->reason). */
enum NativeCompositeFallbackReason
{
    NATIVE_COMPOSITE_OK = 0,
    NATIVE_COMPOSITE_FALLBACK_NULL_INPUT,      // required argument NULL
    NATIVE_COMPOSITE_FALLBACK_BG_UNSUPPORTED,  // BG snapshot not map-supported
    NATIVE_COMPOSITE_FALLBACK_BG0_OVERLAY,     // BG0 would render visible pixels
    NATIVE_COMPOSITE_FALLBACK_BLEND_EFFECT,    // blend would alter final pixels
    NATIVE_COMPOSITE_FALLBACK_OBJ_NOT_PRODUCED,// OBJ rasterize produced==FALSE
    NATIVE_COMPOSITE_FALLBACK_OBJ_UNSUPPORTED, // OBJ capabilityFlags != 0
    NATIVE_COMPOSITE_FALLBACK_VIEWPORT_INVALID,// viewport NULL or not expanded-active
};

struct NativeFieldCompositorReport
{
    enum NativeCompositeFallbackReason reason;
    u32 objCapabilityFlags;   // the OBJ capabilityFlags when OBJ unsupported
    u16 objRejectCount;       // rejected presented OBJ primitives
    u8 bg0Content;            // NativeOverworld_SnapshotHasBg0Content() observed
    u8 reserved;
};

/*
 * Blend registers + backdrop color the compositor needs to reproduce the GBA's
 * alpha blend for a winning semi-transparent OBJ pixel (Stage 3E). The oracle
 * ALWAYS blends a semi OBJ pixel (`isSemiTransparent`, independent of effect
 * mode / TGT1 / windows) at OBJ draw time, using EVA/EVB from REG_BLDALPHA and
 * a targetB selected by the BG-only walk starting at the OBJ's priority
 * (alphaBlendSelectTargetB with spriteBlendEnabled=false), falling back to the
 * backdrop when TGT2_BD is set. All values are frame-wide captures.
 */
struct NativeCompositeBlendState
{
    u16 bldCnt;        // REG_BLDCNT (TGT2_BG0-3 bits 8-11, TGT2_BD bit 13)
    u16 bldAlpha;      // REG_BLDALPHA (EVA bits 0-4, EVB bits 8-12)
    u16 bldY;          // REG_BLDY (diagnostics only; semi OBJ never uses it)
    u16 dispCnt;       // REG_DISPCNT (layer enable bits 8-11 for isbgEnabled)
    u16 backdropColor; // PLTT[0], the backdrop targetB
};

/*
 * Pure merge. Consumes a native BG frame with per-pixel BG-winner metadata
 * (produced by NativeOverworldRenderer_DrawMapFrameWithMeta) and the Stage 3B
 * OBJ layers, and writes the final 240x160 composite + per-pixel winner.
 * A winning SEMI-TRANSPARENT OBJ pixel is alpha-blended exactly like the
 * oracle (see NativeCompositeBlendState).
 *
 *   bgFrame    BG frame: backdrop color where bgWinner[i]==0, a BG pixel (bit
 *              15 set) where bgWinner[i] is 2/3/4.
 *   bgWinner   per-pixel BG winner: 0=backdrop, 2=BG1, 3=BG2, 4=BG3.
 *   bgPriority 4-entry array indexed by GBA bgnum (0=BG0..3=BG3): each layer's
 *              BGCNT priority (0-3). bgPriority[0] is never consulted because
 *              BG0 cannot win a supported frame.
 *   bgLayers   flat per-GBA-bgnum layer colors [4][pixels] in the oracle's
 *              scanline.layers[bgnum] encoding (bit 15 = opaque, 0 =
 *              transparent); slot 0 (BG0) is always 0 on a supported frame.
 *   objOut     Stage 3B/3E rasterize output (layers + ownership + semi).
 *   blend      the frame's blend state (required; semi OBJ blend needs it).
 *   outFrame   final composite; outWinner per-pixel enum NativeCompositeWinner.
 *              outWinner may be NULL to skip the metadata output.
 *
 * Returns FALSE only on NULL input (outFrame may be NULL only to skip metadata;
 * bgFrame/bgWinner/bgPriority/bgLayers/objOut/blend are all required).
 */
bool32 NativeFieldCompositor_Composite(const u16 *bgFrame, const u8 *bgWinner,
                                       const u8 *bgPriority, const u16 *bgLayers,
                                       const struct NativeObjRenderOutput *objOut,
                                       const struct NativeCompositeBlendState *blend,
                                       u16 *outFrame, u8 *outWinner);

/*
 * Stage 4A: the same pure merge over an ARBITRARY pixel count (viewport width x
 * height). The OBJ layers and bgLayers are indexed with pixelCount as the flat
 * stride; Composite() is the 240x160 wrapper (byte-identical output). Winner
 * encoding and the semi-OBJ blend path are unchanged.
 */
bool32 NativeFieldCompositor_CompositeEx(const u16 *bgFrame, const u8 *bgWinner,
                                         const u8 *bgPriority, const u16 *bgLayers,
                                         const struct NativeObjRenderOutput *objOut,
                                         const struct NativeCompositeBlendState *blend,
                                         u16 *outFrame, u8 *outWinner,
                                         u32 pixelCount);

/*
 * Complete Stage 3C compositor for a captured frame: applies the capability
 * gates (BG map support, BG0 overlay gate, composite blend predicate, Stage 3B
 * OBJ capability flags), renders the native BG with winner metadata into the
 * caller-provided scratch (bgFrame/bgWinner), composites with the OBJ layers,
 * and reports the outcome. outFrame/outWinner are the final outputs; report is
 * filled with the reason when the frame cannot be composed (in which case
 * outFrame/outWinner are untouched).
 *
 * All buffers are caller-allocated: bgFrame/bgWinner/bgLayers are scratch for
 * the intermediate BG render and remain valid for diagnostics afterwards.
 */
bool32 NativeOverworldRenderer_DrawCompositeFrame(
    const struct NativeOverworldSnapshot *bgSnap,
    const struct NativeObjRenderOutput *objOut,
    u16 *bgFrame, u8 *bgWinner, u16 *bgLayers,
    u16 *outFrame, u8 *outWinner,
    struct NativeFieldCompositorReport *report);

/*
 * Stage 4A: complete compositor for an EXPANDED native viewport.
 *
 * The same capability gates as DrawCompositeFrame (BG map support, BG0 overlay
 * gate, composite blend predicate, Stage 3B/3E OBJ capability flags), but the
 * whole pipeline runs at viewport resolution: DrawMapFrameWithMetaEx renders
 * the world rectangle the viewport sees, RasterizeEx rasterizes the OBJ
 * commands at viewport resolution (Stage 4A placement: world-relative sprites
 * move outward by the viewport margin, SCREEN_FIXED/raw stay in the 240x160
 * center), and CompositeEx merges them over width*height pixels.
 *
 * viewport must be Active (wider or taller than 240x160); a 240x160 viewport is
 * rejected with NATIVE_COMPOSITE_FALLBACK_VIEWPORT_INVALID (the proven 240x160
 * path stays the DrawCompositeFrame wrapper). viewportLeftX/TopY are derived as
 * origin - ((width-240)/2, (height-160)/2) -- the deterministic odd-rounding
 * that NativeOverworldViewport_TopLeft also produces -- so the expanded view is
 * centered on exactly the same world focus as the 240x160 view.
 *
 * Buffers are caller-allocated at MAX capacity: bgFrame / bgWinner / bgLayers
 * scratch for the intermediate BG render, objOut scratch for the OBJ rasterize.
 * outFrame/outWinner are the final outputs; outWinner may be NULL to skip the
 * winner metadata. report is filled with the reason when the frame cannot be
 * composed (in which case the outputs are untouched).
 */
bool32 NativeOverworldRenderer_DrawExpandedComposite(
    const struct NativeOverworldSnapshot *bgSnap,
    const struct NativeObjSnapshot *objSnap,
    struct NativeObjRenderOutput *objOut,
    const struct NativeViewport *viewport,
    u16 *bgFrame, u8 *bgWinner, u16 *bgLayers,
    u16 *outFrame, u8 *outWinner,
    struct NativeFieldCompositorReport *report);

/*
 * Composite blend predicate (superset of NativeOverworld_BlendAffectsMapBackground
 * for the FULL composite, which includes OBJ):
 *
 *   - EFFECT_NONE: never changes a pixel.
 *   - TGT1 empty: never changes a pixel (the field's persistent BLDCNT=0x1E40
 *     is TGT1 empty, alpha mode -- legal and neutral).
 *   - alpha with EVA==16/EVB==0: reproduces target A exactly -- neutral.
 *   - alpha with no TGT2 target at all: the GBA's alphaBlendSelectTargetB bails
 *     and target A is written unchanged -- neutral.
 *   - anything else with a TGT1 target (BG0-3, OBJ, or backdrop) can recolor or
 *     brighten the winner and is rejected.
 */
bool32 NativeField_BlendAffectsComposite(u16 bldCnt, u16 bldY, u16 bldAlpha);

/*
 * On-demand per-pixel composite trace (item 13): reconstructs both sides of one
 * final-pixel decision from the already-produced buffers (no stored per-pixel
 * state). BG side: winning BG + priority + color from the metadata buffers.
 * OBJ side: foremost OBJ priority + color, with the full Stage 3B sample trace
 * via NativeObjRender_Trace. Stage 3E: when the winning OBJ pixel is semi-
 * transparent, also reproduces the blend decision (BLDCNT/BLDALPHA, targetB
 * type+color, EVA/EVB, applied final). Returns FALSE on NULL input or when x/y
 * are out of range; otherwise fills *trace (finalWinner/finalColor always
 * valid).
 */
struct NativeFieldCompositeTrace
{
    u8 bgWinner;      // 0=backdrop, 2/3/4 = BG1/2/3 (the BG-side candidate)
    u8 bgPriority;    // 0-3, or 0xFF when the candidate is the backdrop
    u16 bgColor;      // BG-side color (bit 15 set for a BG pixel, clear backdrop)
    bool32 objPresent;// an OBJ claimed the pixel at objPriority
    u8 objPriority;   // winning OBJ priority layer (0-3)
    u16 objColor;     // bit 15 clear
    struct NativeObjSampleTrace objTrace; // Stage 3B provenance/sample trace
    u8 finalWinner;   // enum NativeCompositeWinner
    u16 finalColor;   // final composite color (bit 15 set for BG/OBJ)
    // Stage 3E blend diagnostics (meaningful when the final winner was a semi
    // OBJ pixel; blendMode/tgt2Bits/eva/evb are always filled from the state).
    u16 bldCnt;       // REG_BLDCNT
    u16 bldAlpha;     // REG_BLDALPHA
    u16 bldY;         // REG_BLDY
    u8  blendMode;    // (bldCnt >> 6) & 3
    u8  semiObj;      // 1 = the winning OBJ pixel is semi-transparent
    u8  tgt2Bits;     // (bldCnt >> 8) & 0x1F (TGT2 BG0-3 + OBJ target bits)
    u8  eva;          // bldAlpha & 0x1F
    u8  evb;          // (bldAlpha >> 8) & 0x1F
    u8  targetBKind;  // 0=none, 1=BG0, 2=BG1, 3=BG2, 4=BG3, 5=backdrop
    u16 targetBColor; // the targetB found (bit 15 set for a BG/OBJ layer)
    u16 preBlendColor;// the semi OBJ's raw pre-blend color (bit 15 clear)
    bool32 blendApplied; // 1 = a targetB was found and the blend computed
};

bool32 NativeFieldCompositor_Trace(const u16 *bgFrame, const u8 *bgWinner,
                                   const u8 *bgPriority, const u16 *bgLayers,
                                   const struct NativeObjSnapshot *objSnap,
                                   const struct NativeObjRenderOutput *objOut,
                                   const struct NativeCompositeBlendState *blend,
                                   u8 x, u8 y,
                                   struct NativeFieldCompositeTrace *trace);

#endif // GUARD_PLATFORM_NATIVE_FIELD_COMPOSITOR_H

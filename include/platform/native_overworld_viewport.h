#ifndef GUARD_PLATFORM_NATIVE_OVERWORLD_VIEWPORT_H
#define GUARD_PLATFORM_NATIVE_OVERWORLD_VIEWPORT_H

#include "global.h"

/*
 * Stage 4A: continuous native viewport state.
 *
 * Replaces the Stage 1 experimental enum zoom (1.0x / 1.25x / 1.5x) with a
 * single continuous view scale. width/height are ALWAYS derived from scaleQ8 by
 * NativeOverworldViewport_Recompute() -- (240*scaleQ8 + 0x7F) >> 8 for width,
 * (160*scaleQ8 + 0x7F) >> 8 for height -- so arbitrary view scales (1.00, 1.02,
 * 1.05, 1.13, 1.27, 1.50, ...) produce arbitrary logical viewport dimensions.
 * There are no preset zoom enums and the renderer contains no hardcoded per-mode
 * logic: every render parameter comes from this struct.
 *
 * centerX/centerY is the world-pixel point the viewport stays centered on (the
 * same gameplay focus as the 240x160 view). The default focus is the 240x160
 * logical origin plus (DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2); it is filled in by
 * NativeOverworldViewport_TopLeft() each frame.
 *
 * The zoom controls manipulate the SAME scaleQ8 that future touchpad pinch will
 * use (documented contract, Stage 4A directive 16/17): there is no other zoom
 * state, so pinch and keyboard cannot drift apart. Ctrl+Minus increases the
 * scale (reveals more world), Ctrl+Equals decreases it (back toward the normal
 * 240x160 view), Ctrl+0 resets to exactly 1.0x / 240x160.
 */
#define NATIVE_VIEWPORT_MIN_WIDTH   240
#define NATIVE_VIEWPORT_MIN_HEIGHT  160
#define NATIVE_VIEWPORT_MAX_WIDTH   360
#define NATIVE_VIEWPORT_MAX_HEIGHT  240

// 8.8 fixed-point view scale. 256 == 1.0x (240x160). 384 == 1.5x (360x240),
// the maximum-capacity native buffers allow.
#define NATIVE_VIEWPORT_SCALE_1_0   256
#define NATIVE_VIEWPORT_SCALE_MAX   384
// Continuous keyboard step, ~0.05 viewport-scale units (13/256 = 0.05078...).
#define NATIVE_VIEWPORT_SCALE_STEP  13

struct NativeViewport
{
    u16 width;      // (240*scaleQ8 + 0x7F) >> 8 -- always derived, see Recompute
    u16 height;     // (160*scaleQ8 + 0x7F) >> 8
    s16 scaleQ8;    // 8.8 fixed point; 256 == 1.0x
    s32 centerX;    // world-pixel viewport focus (updated each frame by TopLeft)
    s32 centerY;
};

/*
 * Parse POKEEMERALD_NATIVE_VIEWPORT (e.g. "300x200") and set the initial scale.
 * A valid expanded viewport requires WxH inside [240,360]x[160,240]; otherwise
 * the viewport starts at exactly 1.0x / 240x160. scaleQ8 is derived from the
 * requested width so Recompute() round-trips it exactly (300 -> 1.25x -> 300).
 */
void NativeOverworldViewport_Init(struct NativeViewport *viewport);

/* Recompute width/height from scaleQ8 (clamped into [1.0x, 1.5x]). */
void NativeOverworldViewport_Recompute(struct NativeViewport *viewport);

/*
 * Continuous zoom controls (same variable future pinch uses).
 * ZoomOut = Ctrl+Minus = increase the scale (reveal more world).
 * ZoomIn  = Ctrl+Equals = decrease the scale (back toward 240x160).
 * Both return FALSE when already at the respective bound.
 */
bool32 NativeOverworldViewport_ZoomOut(struct NativeViewport *viewport);
bool32 NativeOverworldViewport_ZoomIn(struct NativeViewport *viewport);

/* Ctrl+0: reset to exactly 1.0x / 240x160. */
void NativeOverworldViewport_Reset(struct NativeViewport *viewport);

/* True when the viewport is wider or taller than the base 240x160 view. */
bool32 NativeOverworldViewport_Active(const struct NativeViewport *viewport);

/*
 * Top-left world pixel of the viewport, centered on the same focus as the
 * 240x160 view: center = (originX + DISPLAY_WIDTH/2, originY + DISPLAY_HEIGHT/2),
 * then topLeft = center - (width/2, height/2) with integer truncation, giving
 * deterministic odd-dimension rounding. Also stores the focus back into
 * viewport->centerX/centerY. For 300x200 this yields margins (30, 20).
 */
void NativeOverworldViewport_TopLeft(struct NativeViewport *viewport,
                                     s32 originX, s32 originY,
                                     s32 *left, s32 *top);

#endif // GUARD_PLATFORM_NATIVE_OVERWORLD_VIEWPORT_H

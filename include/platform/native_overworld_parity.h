#ifndef GUARD_PLATFORM_NATIVE_OVERWORLD_PARITY_H
#define GUARD_PLATFORM_NATIVE_OVERWORLD_PARITY_H

#include "global.h"
#include "platform/native_overworld_snapshot.h"

/*
 * Runtime parity capture for the native 240x160 map-background renderer.
 * Dev-only, enabled by POKEEMERALD_NATIVE_PARITY=1.
 *
 * Flow per host presentation frame (all in the host thread, worker blocked at
 * VBlankIntrWait, so the frame state is settled and stable):
 *
 *   1. NativeOverworldParity_BeginFrame()      -- before the native snapshot,
 *                                                 gates on the interval env and
 *                                                 returns TRUE when this frame is
 *                                                 captured (counted as scheduled).
 *   2. NativeOverworld_CaptureParitySnapshot() + NativeOverworldRenderer_DrawMapFrame()
 *                                               -- produce the native frame from
 *                                                 this frame's coherent state,
 *                                                 gated by the lean parity
 *                                                 eligibility predicate (NOT the
 *                                                 old zoom/expanded predicate).
 *   3. DrawFrame(gbaImage)                      -- with gParityBGPixelsBuffer /
 *                                                 gParityBGPixelLayers set, also
 *                                                 fills the BG-only oracle and
 *                                                 winner-layer map; sets
 *                                                 gParityOracleProduced = 1.
 *   4. NativeOverworldParity_EndFrame(...)      -- accounts the frame by outcome
 *                                                 and, for the COMPARED outcome,
 *                                                 compares native vs oracle on the
 *                                                 SAME presentation frame,
 *                                                 classifies mismatches, and
 *                                                 writes diagnostics to
 *                                                 build/native-parity/ on
 *                                                 mismatch.
 *
 * Every scheduled capture is accounted: the final summary distinguishes
 * scheduled / snapshot-ok / snapshot-failed / native-ok / native-failed /
 * oracle-ok / oracle-missing / compared / clean / mismatch / skipped and prints
 * a per-reason skip histogram. No capture disappears silently.
 *
 * Dev fixture capture (optional): POKEEMERALD_NATIVE_PARITY_SCROLL_FIXTURES=N
 * (default 0, off) serializes up to N bg-scroll-divergent COMPARED frames (a
 * uniform scroll that lags the logical camera -- the normal moving-frame
 * presentation) as offline fixtures under build/native-parity/scroll/. The
 * write is independent of parity success: it never counts as a clean or
 * mismatch frame and never alters the parity summary. Use 1-2 during one manual
 * run to obtain a real captured moving-frame fixture to reproduce offline.
 *
 * Normal builds (env off) add a single static-branch check per frame.
 */

enum NativeParityCaptureOutcome
{
    PARITY_OUTCOME_NONE = 0,    // no capture scheduled this frame
    PARITY_OUTCOME_COMPARED,    // snapshot + native + oracle all produced, compared
    PARITY_OUTCOME_SNAPSHOT_FAILED, // capture gate rejected the frame (fallbackReason set)
    PARITY_OUTCOME_NATIVE_FAILED,   // snapshot ok, map renderer rejected the frame
    PARITY_OUTCOME_ORACLE_MISSING,  // snapshot + native ok, oracle not produced
    PARITY_OUTCOME_ABORTED,         // capture scheduled but the pipeline never ran
};

// Advance the per-frame counter; returns TRUE when the current frame is a
// capture frame (env enabled + interval gate). Call at the top of the host
// video draw, before the native snapshot capture.
bool32 NativeOverworldParity_BeginFrame(void);

// Account one scheduled capture and, for PARITY_OUTCOME_COMPARED, compare the
// native 240x160 map frame against the GBA BG-only oracle frame for the current
// frame. snapshot is the coherent pre-DrawFrame capture; nativeFrame is the
// native map frame (NULL when the native render was not produced). oracleFrame
// is the DISPLAY_WIDTH*DISPLAY_HEIGHT BG-only oracle from DrawFrame, oracleLayers
// its per-pixel winner layer (0=backdrop,1=BG0,2=BG1,3=BG2,4=BG3), or NULL.
// Returns the mismatched pixel count (0 when the capture is inactive, skipped,
// or parity is clean). Emits concise one-time-per-reason skip diagnostics and
// saves artifacts to build/native-parity/ on mismatch (bounded).
u32 NativeOverworldParity_EndFrame(const struct NativeOverworldSnapshot *snapshot,
                                   const u16 *nativeFrame,
                                   const u16 *oracleFrame,
                                   const u8 *oracleLayers,
                                   enum NativeParityCaptureOutcome outcome);

// Print a final summary (all capture-outcome counters + per-reason skip
// histogram) to stderr. Call once at scheduler shutdown.
void NativeOverworldParity_Shutdown(void);

#if defined(LINUX64) && LINUX64
struct NativeObjSnapshot;

// Stage 3A OBJ diagnostic (additive to EndFrame; does not change its signature
// or accounting). On a captured frame it logs a rate-limited "PARITY OBJ" state
// line (command provenance counts, presentation sequence, camera origin, the
// relevant registers) and, when POKEEMERALD_NATIVE_PARITY_OBJ_FIXTURES=N is set,
// serializes up to N presented frames with a non-empty OBJ command stream under
// build/native-parity/obj/ as offline fixtures. Returns the provenance-mismatch
// count (0 when parity is disabled or the snapshot is NULL).
u32 NativeOverworldParity_ObjFrame(const struct NativeObjSnapshot *objSnapshot);

// Stage 3C composite parity (final BG + normal-OBJ composite vs the REAL
// DrawFrame main-pass composite). Dev-only (POKEEMERALD_NATIVE_OBJ_PARITY=1),
// independent of the BG-only POKEEMERALD_NATIVE_PARITY mode; the two may run on
// the same frame. Shares the module's per-frame counter so frame numbers stay
// consistent whichever mode is active.
enum NativeCompositeParityOutcome
{
    COMPOSITE_PARITY_NONE = 0,            // env off / not a composite capture frame
    COMPOSITE_PARITY_COMPARED,            // native composite + oracle composite produced
    COMPOSITE_PARITY_BG_SNAPSHOT_FAILED,  // BG snapshot rejected by the capture gate
    COMPOSITE_PARITY_OBJ_SNAPSHOT_FAILED, // OBJ snapshot not valid
    COMPOSITE_PARITY_OBJ_UNSUPPORTED,     // OBJ capabilityFlags != 0 (flags + rejects named)
    COMPOSITE_PARITY_BG_UNSUPPORTED,      // DrawCompositeFrame: BG not map-supported
    COMPOSITE_PARITY_BG0_OVERLAY,         // DrawCompositeFrame: BG0 overlay gate
    COMPOSITE_PARITY_BLEND_EFFECT,        // DrawCompositeFrame: blend would alter pixels
    COMPOSITE_PARITY_ORACLE_MISSING,      // composite oracle seam did not produce
    COMPOSITE_PARITY_ABORTED,             // scheduled but the pipeline never ran
};

// Env gate + capture-frame selection (shares the per-frame counter). Returns
// TRUE when the current frame is a composite capture frame. Call it every frame
// alongside NativeOverworldParity_BeginFrame() so the BG snapshot + OBJ snapshot
// are both captured for the composite.
bool32 NativeOverworldParity_CompositeBeginFrame(void);

// Account one composite capture and, when the full pipeline produced, compare
// the native final composite (proven BG frame+metadata and proven Stage 3B OBJ
// layers merged by real hardware priority) against the REAL DrawFrame main-pass
// composite frame (gbaComposite) and its per-pixel winner map (gbaCompositeLayers,
// 0=backdrop,1-4=BG0-3,5-8=OBJ0-3, from the gParityCompositeLayers seam).
// oracleProduced is gParityCompositeProduced captured around DrawFrame. Every
// outcome is accounted (no silent early return). On mismatch, writes bounded
// artifacts (native/gba frames + winner maps + BG/OBJ snapshot serializations +
// a composite trace at the first mismatching pixel) under
// build/native-parity/objcomp/. Returns the mismatched pixel count (0 when
// inactive / skipped / clean).
u32 NativeOverworldParity_CompositeFrame(const struct NativeOverworldSnapshot *bgSnap,
                                         const struct NativeObjSnapshot *objSnap,
                                         const u16 *gbaComposite,
                                         const u8 *gbaCompositeLayers,
                                         bool32 oracleProduced);
#endif


#endif // GUARD_PLATFORM_NATIVE_OVERWORLD_PARITY_H

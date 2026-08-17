#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "global.h"
#include "platform/native_overworld_parity.h"
#if defined(_WIN32)
/* MinGW's io.h declares mkdir() with one argument; POSIX takes a mode. */
static int ParityMkdir(const char *path)
{
    return mkdir(path);
}
#else
static int ParityMkdir(const char *path)
{
    return mkdir(path, 0755);
}
#endif
#if defined(LINUX64) && LINUX64
#include "platform/native_sprite_snapshot.h"
#include "platform/native_obj_renderer.h"
#include "platform/native_field_compositor.h"
#endif

/*
 * Runtime parity capture for the native 240x160 map-background renderer.
 *
 * Dev-only (POKEEMERALD_NATIVE_PARITY=1). Each capture frame compares the native
 * map frame, rendered from a coherent pre-DrawFrame snapshot, against the
 * BG-only oracle produced by DrawFrame on the SAME presentation frame state
 * (worker blocked at VBlankIntrWait throughout). Only per-frame diagnostics are
 * emitted when parity is clean; mismatching frames write bounded artifacts under
 * build/native-parity/.
 *
 * Every scheduled capture is accounted for. EndFrame receives an explicit
 * outcome describing where the native-vs-oracle pipeline stopped, so a capture
 * can never disappear silently: the summary distinguishes scheduled captures,
 * snapshot captures that succeeded / were rejected by the gate, native renders,
 * oracle renders, comparison frames, clean frames, mismatch frames, skipped
 * captures, and aborted captures. For each skip reason only the FIRST occurrence
 * is logged inline (no per-frame spam); the full histogram is printed at
 * shutdown.
 *
 * Classification of a mismatch is by the GBA winning layer at the pixel
 * (backdrop / BG0 / BG1 / BG2 / BG3 from the oracle's layer map) plus the first
 * mismatch coordinate and both colors, together with map identity, camera /
 * sub-tile offset, and the relevant BG control/scroll registers.
 *
 * The inline mismatch line reports the per-layer histogram in the order
 * bd / b0 / b1 / b2 / b3 == layerHist[0..4], matching the report file's full
 * "backdrop bg0 bg1 bg2 bg3" histogram (which the layer-N.bin maps confirm).
 */

#define PARITY_DIR "build/native-parity"
#define SCROLL_FIXTURE_DIR "build/native-parity/scroll"

static bool32 sChecked;
static bool32 sEnabled;
static u32 sEvery = 1;
static u32 sDumpCap = 8;
// Dev capture (POKEEMERALD_NATIVE_PARITY_SCROLL_FIXTURES=N, default 0 off):
// serialize a bg-scroll-divergent COMPARED frame (uniform scroll that lags the
// logical camera -- the normal moving-frame presentation) as an offline fixture
// under build/native-parity/scroll/. Bounded to N (1-2 recommended). The write
// is independent of parity success: it never counts as a clean or mismatch frame
// and never alters the parity summary; it exists so a real moving frame can be
// reproduced offline from its snapshot alone.
static u32 sScrollFixtureCap;
static u32 sScrollFixturesWritten;
// Rate-limited PARITY SCROLL state diagnostic (parity enabled only). Moving-frame
// (scroll-divergent) capture frames log immediately -- they are transient and are
// the whole point of the diagnostic; stationary frames repeat every capture, so
// they log at most once per SCROLL_DIAG_PERIOD capture frames.
#define SCROLL_DIAG_PERIOD 60
static u32 sScrollDiagEmitCounter;

#if defined(LINUX64) && LINUX64
// Stage 3A OBJ diagnostic. POKEEMERALD_NATIVE_PARITY_OBJ_FIXTURES=N (default 0
// off) serializes up to N presented frames with a NON-EMPTY OBJ command stream
// (commandCount > 0 -- i.e. frames with real sprite/OAM primitives) as offline
// fixtures under build/native-parity/obj/, so a real OBJ presentation frame can
// be reproduced offline. Independent of parity success, like the scroll fixture
// mechanism. The PARITY OBJ state line is rate-limited: steady frames log at most
// once per OBJ_DIAG_PERIOD capture frames; frames with any provenance mismatch,
// raw-OAM, or overflow anomalies log immediately (they are the point).
#define OBJ_FIXTURE_DIR "build/native-parity/obj"
#define OBJ_DIAG_PERIOD 60
static u32 sObjFixtureCap;
static u32 sObjFixturesWritten;
static u32 sObjDiagEmitCounter;
static HOST_DATA u8 sObjSerializeBuffer[NATIVE_OBJ_SERIALIZE_HEADER
                                        + sizeof(struct NativeObjSnapshot)];

    // Stage 3C composite parity: the final BG + normal-OBJ composite vs the REAL
    // DrawFrame main-pass composite. POKEEMERALD_NATIVE_OBJ_PARITY=1 (default 0,
    // off), independent of the BG-only POKEEMERALD_NATIVE_PARITY mode; the two may
    // run on the same frame and share sFrameCounter so frame numbers agree
    // whichever is active. On mismatch, bounded artifacts go under
    // build/native-parity/objcomp/.
#define COMPOSITE_DIR "build/native-parity/objcomp"
    static bool32 sCompositeChecked;
    static bool32 sCompositeEnabled;
    static u32 sCompositeEvery = 1;
    static u32 sCompositeDumpCap = 8;
    static u32 sCompScheduled;
    static u32 sCompCompared;
    static u32 sCompClean;
    static u32 sCompMismatchFrames;
    static u64 sCompMismatchPixels;
    static u64 sCompComparedPixels;
    static u32 sCompBgSnapFailed;
    static u32 sCompObjSnapFailed;
    static u32 sCompObjUnsupported;
    static u32 sCompBgUnsupported;
    static u32 sCompBg0Overlay;
    static u32 sCompBlendEffect;
    static u32 sCompOracleMissing;
    static u32 sCompAborted;
    static u32 sCompDumpsWritten;
    // First-occurrence inline logging (no per-frame spam; the full counts are in
    // the shutdown summary), mirroring the BG-only sSkipMessageLogged pattern.
    static bool32 sCompBgSnapLogged;
    static bool32 sCompObjSnapLogged;
    static bool32 sCompObjUnsupportedLogged;
    static bool32 sCompBgUnsupportedLogged;
    static bool32 sCompBg0OverlayLogged;
    static bool32 sCompBlendLogged;
    static bool32 sCompOracleMissingLogged;
    static bool32 sCompAbortedLogged;
    // Stage 3D capability histogram (Section 3): per-reject-reason counts for
    // OBJ-unsupported frames AND for the rejected presented OAM primitives.
    // A frame is counted once per DISTINCT reason it triggered (multi-reason
    // frames bump several bins) but only once in the total sCompObjUnsupported.
    // Primitive counts come from the reject records, so a 40-primitive frame
    // contributes 40 to one primitive bin while the frame bin increments once.
    // BLEND_CONFIG is a frame-level flag (no primitive reject record); 2D
    // mapping is counted as obj_snap_failed upstream, not here.
    static u32 sObjUnsupportedFrameByReason[NATIVE_OBJ_REJECT_REASON_COUNT];
    static u32 sObjUnsupportedPrimByReason[NATIVE_OBJ_REJECT_REASON_COUNT];
    static u32 sObjUnsupportedFrameBlendConfig;
    // Composite scratch, module-owned so the compare, the on-demand trace and the
    // artifact writes all observe the SAME native composite for the frame.
    static HOST_DATA struct NativeObjRenderOutput sCompObjOut;
    static HOST_DATA u16 sCompBgFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static HOST_DATA u8 sCompBgWinner[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static HOST_DATA u16 sCompBgLayers[4 * DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static HOST_DATA u16 sCompNative[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static HOST_DATA u8 sCompNativeWinner[DISPLAY_WIDTH * DISPLAY_HEIGHT];
#endif

static u32 sFrameCounter;
static u32 sScheduledCaptures;   // BeginFrame returned TRUE
static u32 sSnapshotSucceeded;   // snapshot capture produced (gate + common body ok)
static u32 sSnapshotFailed;      // capture gate rejected the frame
static u32 sNativeRendered;      // native map frame rendered
static u32 sNativeFailed;        // snapshot ok, renderer rejected
static u32 sOracleProduced;      // oracle seam filled the oracle buffer
static u32 sOracleMissing;       // snapshot + native ok, oracle not produced
static u32 sComparedFrames;      // reached the pixel compare loop
static u32 sCleanFrames;         // compared with zero mismatches
static u32 sMismatchFrames;      // compared with >= 1 mismatches
static u64 sMismatchPixels;
static u64 sComparedPixels;
static u32 sAbortedCaptures;     // scheduled but pipeline never ran / anomalous
static u32 sDumpsWritten;
static u32 sSkippedByReason[NATIVE_FALLBACK_REASON_COUNT];
static bool32 sSkipMessageLogged[NATIVE_FALLBACK_REASON_COUNT];
static bool32 sOracleMissingLogged;

static const char *FallbackName(enum NativeOverworldFallbackReason reason)
{
    switch (reason)
    {
    case NATIVE_FALLBACK_NONE:                return "none";
    case NATIVE_FALLBACK_MISSING_LAYOUT:      return "missing-layout";
    case NATIVE_FALLBACK_MISSING_TILESETS:    return "missing-tilesets";
    case NATIVE_FALLBACK_MISSING_MAP_GRID:    return "missing-map-grid";
    case NATIVE_FALLBACK_MISSING_BG_RING:     return "missing-bg-ring";
    case NATIVE_FALLBACK_MISSING_BG_VRAM:     return "missing-bg-vram";
    case NATIVE_FALLBACK_MISSING_BG_PALETTE:  return "missing-bg-palette";
    case NATIVE_FALLBACK_MISSING_CAMERA:      return "missing-camera";
    case NATIVE_FALLBACK_DISPLAY_MODE:        return "display-mode";
    case NATIVE_FALLBACK_DISPLAY_LAYERS:      return "display-layers";
    case NATIVE_FALLBACK_BG_CONFIG:           return "bg-config";
    case NATIVE_FALLBACK_BG_SCROLL:           return "bg-scroll";
    case NATIVE_FALLBACK_HARDWARE_BLEND:      return "hardware-blend";
    case NATIVE_FALLBACK_WINDOWS:             return "windows";
    case NATIVE_FALLBACK_MOSAIC:              return "mosaic";
    case NATIVE_FALLBACK_SCENE_NOT_OVERWORLD: return "scene-not-overworld";
    case NATIVE_FALLBACK_BG0_OVERLAY:         return "bg0-overlay";
    case NATIVE_FALLBACK_BG_TILEMAP_IN_FLIGHT: return "bg-tilemap-in-flight";
    }
    return "unknown";
}

static const char *ObjRejectName(enum NativeObjRejectReason reason)
{
    switch (reason)
    {
    case NATIVE_OBJ_REJECT_NONE:              return "none";
    case NATIVE_OBJ_REJECT_AFFINE:            return "affine";
    case NATIVE_OBJ_REJECT_AFFINE_DOUBLE:     return "affine-double";
    case NATIVE_OBJ_REJECT_8BPP:              return "8bpp";
    case NATIVE_OBJ_REJECT_OBJ_WINDOW:        return "obj-window";
    case NATIVE_OBJ_REJECT_MOSAIC:            return "mosaic";
    case NATIVE_OBJ_REJECT_PROHIBITED_SHAPE:  return "prohibited-shape";
    }
    return "unknown";
}

static const char *EnvOr(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return (value != NULL && value[0] != '\0') ? value : fallback;
}

static enum NativeOverworldFallbackReason ClampReason(enum NativeOverworldFallbackReason reason)
{
    if ((int)reason < 0 || (int)reason >= NATIVE_FALLBACK_REASON_COUNT)
        return NATIVE_FALLBACK_SCENE_NOT_OVERWORLD;
    return reason;
}

static void ReportSkipOnce(const struct NativeOverworldSnapshot *snapshot,
                           enum NativeOverworldFallbackReason reason,
                           const char *stage, u32 frameNumber)
{
    reason = ClampReason(reason);
    sSkippedByReason[reason]++;
    if (!sSkipMessageLogged[reason])
    {
        sSkipMessageLogged[reason] = TRUE;
        fprintf(stderr, "PARITY SKIP: %s: %s (frame=%u)\n",
                stage, FallbackName(reason), frameNumber);
        if (reason == NATIVE_FALLBACK_HARDWARE_BLEND && snapshot != NULL)
        {
            // The gate captured these live registers before rejecting, so the
            // first hardware-blend skip pinpoints the exact unsupported config.
            fprintf(stderr,
                    "PARITY SKIP:   bldCnt=0x%04x tgt1=0x%02x tgt2=0x%02x effect=%u "
                    "bldAlpha=0x%04x bldY=%u bg1Cnt=0x%04x bg2Cnt=0x%04x bg3Cnt=0x%04x "
                    "dispCnt=0x%04x\n",
                    snapshot->bldCnt, snapshot->bldCnt & 0x3F,
                    (snapshot->bldCnt >> 8) & 0x3F, (snapshot->bldCnt >> 6) & 3,
                    snapshot->bldAlpha, snapshot->bldY & 0x1F,
                    snapshot->bgCnt[0], snapshot->bgCnt[1], snapshot->bgCnt[2],
                    snapshot->dispCnt);
            fprintf(stderr,
                    "PARITY SKIP:   unsupported because the active blend can change "
                    "BG1/BG2/BG3 map-background pixels the native renderer does not "
                    "reproduce (map layer in TGT1 with a rendered TGT2 target for "
                    "alpha, or nonzero BLDY brightness on a TGT1 map layer)\n");
        }
        fflush(stderr);
    }
}

bool32 NativeOverworldParity_BeginFrame(void)
{
    if (!sChecked)
    {
        const char *flag = getenv("POKEEMERALD_NATIVE_PARITY");

        sChecked = TRUE;
        sEnabled = (flag != NULL && strcmp(flag, "0") != 0);
        if (sEnabled)
        {
            sEvery = (u32)strtoul(EnvOr("POKEEMERALD_NATIVE_PARITY_EVERY", "1"), NULL, 10);
            if (sEvery == 0)
                sEvery = 1;
            sDumpCap = (u32)strtoul(EnvOr("POKEEMERALD_NATIVE_PARITY_DUMPS", "8"), NULL, 10);
            sScrollFixtureCap = (u32)strtoul(EnvOr("POKEEMERALD_NATIVE_PARITY_SCROLL_FIXTURES", "0"),
                                             NULL, 10);
#if defined(LINUX64) && LINUX64
            sObjFixtureCap = (u32)strtoul(EnvOr("POKEEMERALD_NATIVE_PARITY_OBJ_FIXTURES", "0"),
                                          NULL, 10);
#endif
            fprintf(stderr, "native overworld runtime parity enabled (every %u frames, dump cap %u",
                    sEvery, sDumpCap);
            if (sScrollFixtureCap != 0)
                fprintf(stderr, ", scroll fixtures %u", sScrollFixtureCap);
#if defined(LINUX64) && LINUX64
            if (sObjFixtureCap != 0)
                fprintf(stderr, ", obj fixtures %u", sObjFixtureCap);
#endif
            fprintf(stderr, ")\n");
            fflush(stderr);
        }
    }
    if (!sEnabled)
        return FALSE;
    sFrameCounter++;
    if (sFrameCounter % sEvery != 0)
        return FALSE;
    sScheduledCaptures++;
    return TRUE;
}

/* A frame is "bg-scroll-divergent" when the three BG layers carry a UNIFORM
 * scroll latch that differs from the logical camera -- the normal moving-frame
 * presentation (the ring is latched one frame of motion behind the camera; see
 * the snapshot coordinate-convention comment). A torn latch whose three layers
 * disagree is internally inconsistent and is rejected by the capture gate, so it
 * never reaches the fixture mechanism. */
static bool32 IsScrollDivergent(const struct NativeOverworldSnapshot *snapshot)
{
    u16 h;
    u16 v;
    int bg;

    h = snapshot->bgHofs[0] & 0x1FF;
    v = snapshot->bgVofs[0] & 0x1FF;
    for (bg = 1; bg < 3; bg++)
    {
        if ((snapshot->bgHofs[bg] & 0x1FF) != h || (snapshot->bgVofs[bg] & 0x1FF) != v)
            return FALSE;
    }
    return (h != (u16)(snapshot->cameraX & 0x1FF))
        || (v != (u16)(snapshot->cameraY & 0x1FF));
}

/* Rate-limited PARITY SCROLL state diagnostic: reports the logical camera origin
 * (cameraMapX/Y + sub-tile cameraX/Y) and the three BG scroll latches (the
 * presentation origin) side by side on a compared capture frame. Divergent
 * (moving-frame) frames log every time; stationary frames at most once per
 * SCROLL_DIAG_PERIOD capture frames. Emitted only when parity is enabled (the
 * caller has already returned early otherwise). */
static void MaybeEmitScrollDiagnostic(const struct NativeOverworldSnapshot *snapshot)
{
    bool32 divergent = IsScrollDivergent(snapshot);

    if (!divergent)
    {
        sScrollDiagEmitCounter++;
        if (sScrollDiagEmitCounter < SCROLL_DIAG_PERIOD)
            return;
        sScrollDiagEmitCounter = 0;
    }
    fprintf(stderr,
            "PARITY SCROLL frame=%u state logical=(%ld,%ld)+(%d,%d) "
            "bg1=(0x%04x,0x%04x) bg2=(0x%04x,0x%04x) bg3=(0x%04x,0x%04x) %s\n",
            sFrameCounter,
            (long)snapshot->cameraMapX, (long)snapshot->cameraMapY,
            snapshot->cameraX, snapshot->cameraY,
            snapshot->bgHofs[0], snapshot->bgVofs[0],
            snapshot->bgHofs[1], snapshot->bgVofs[1],
            snapshot->bgHofs[2], snapshot->bgVofs[2],
            divergent ? "divergent" : "stationary");
    fflush(stderr);
}

/* Serialize a bg-scroll-divergent COMPARED frame as an offline fixture under
 * build/native-parity/scroll/ (distinct prefix/dir so it can never be confused
 * with a mismatch artifact). The write is bounded by sScrollFixtureCap and does
 * NOT touch the parity success accounting: this is a dev capture mechanism, not
 * a parity outcome. The manifest reports the frame's scroll vs camera state and
 * the mismatch count so an offline consumer knows whether the fixture is clean. */
static void WriteScrollFixture(const struct NativeOverworldSnapshot *snapshot,
                               const u16 *nativeFrame, const u16 *oracleFrame,
                               const u8 *oracleLayers,
                               u32 frameNumber, u32 mismatches, u64 comparedPixels)
{
    char path[512];
    FILE *f;

    if (ParityMkdir(PARITY_DIR) != 0 && errno != EEXIST)
    {
        fprintf(stderr, "PARITY cannot create artifact dir %s: %s\n",
                PARITY_DIR, strerror(errno));
        return;
    }
    if (ParityMkdir(SCROLL_FIXTURE_DIR) != 0 && errno != EEXIST)
    {
        fprintf(stderr, "PARITY cannot create scroll fixture dir %s: %s\n",
                SCROLL_FIXTURE_DIR, strerror(errno));
        return;
    }

    snprintf(path, sizeof(path), "%s/scroll-snap-%u.bin", SCROLL_FIXTURE_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(snapshot, 1, sizeof(*snapshot), f);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/scroll-native-%u.bin", SCROLL_FIXTURE_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(nativeFrame, 2, DISPLAY_WIDTH * DISPLAY_HEIGHT, f);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/scroll-gba-%u.bin", SCROLL_FIXTURE_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(oracleFrame, 2, DISPLAY_WIDTH * DISPLAY_HEIGHT, f);
        fclose(f);
    }

    if (oracleLayers != NULL)
    {
        snprintf(path, sizeof(path), "%s/scroll-layers-%u.bin", SCROLL_FIXTURE_DIR, frameNumber);
        if ((f = fopen(path, "wb")) != NULL)
        {
            fwrite(oracleLayers, 1, DISPLAY_WIDTH * DISPLAY_HEIGHT, f);
            fclose(f);
        }
    }

    snprintf(path, sizeof(path), "%s/scroll-report-%u.txt", SCROLL_FIXTURE_DIR, frameNumber);
    if ((f = fopen(path, "w")) != NULL)
    {
        fprintf(f, "native overworld scroll-divergent fixture (dev capture)\n");
        fprintf(f, "frame=%u\n", frameNumber);
        fprintf(f, "divergent=yes\n");
        fprintf(f, "mismatches=%u\n", mismatches);
        fprintf(f, "comparedPixels=%llu\n", (unsigned long long)comparedPixels);
        fprintf(f, "map=%ldx%ld (metatiles)\n",
                (long)snapshot->mapWidth, (long)snapshot->mapHeight);
        fprintf(f, "cameraMapX=%ld cameraMapY=%ld\n",
                (long)snapshot->cameraMapX, (long)snapshot->cameraMapY);
        fprintf(f, "cameraX=%d cameraY=%d (logical)\n", snapshot->cameraX, snapshot->cameraY);
        fprintf(f, "scrollHofs=0x%04x scrollVofs=0x%04x (presentation)\n",
                snapshot->bgHofs[0] & 0x1FF, snapshot->bgVofs[0] & 0x1FF);
        fprintf(f, "bg1Cnt=0x%04x bg1Hofs=0x%04x bg1Vofs=0x%04x\n",
                snapshot->bgCnt[0], snapshot->bgHofs[0], snapshot->bgVofs[0]);
        fprintf(f, "bg2Cnt=0x%04x bg2Hofs=0x%04x bg2Vofs=0x%04x\n",
                snapshot->bgCnt[1], snapshot->bgHofs[1], snapshot->bgVofs[1]);
        fprintf(f, "bg3Cnt=0x%04x bg3Hofs=0x%04x bg3Vofs=0x%04x\n",
                snapshot->bgCnt[2], snapshot->bgHofs[2], snapshot->bgVofs[2]);
        fprintf(f, "dispCnt=0x%04x\n", snapshot->dispCnt);
        fclose(f);
    }
    sScrollFixturesWritten++;
    fprintf(stderr,
            "PARITY SCROLL-FIXTURE: frame=%u captured bg-scroll-divergent frame "
            "(scroll=0x%04x,0x%04x cam=(%ld,%ld)+(%d,%d) mismatches=%u) -> %s/scroll-*\n",
            frameNumber, snapshot->bgHofs[0] & 0x1FF, snapshot->bgVofs[0] & 0x1FF,
            (long)snapshot->cameraMapX, (long)snapshot->cameraMapY,
            snapshot->cameraX, snapshot->cameraY, mismatches, SCROLL_FIXTURE_DIR);
    fflush(stderr);
}

static void WriteArtifacts(const struct NativeOverworldSnapshot *snapshot,
                           const u16 *nativeFrame, const u16 *oracleFrame,
                           const u8 *oracleLayers,
                           u32 frameNumber, u32 mismatches, u64 comparedPixels,
                           u32 firstX, u32 firstY, u16 firstNative, u16 firstGba,
                           const u32 *layerHist)
{
    char path[512];
    u8 diff[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    FILE *f;
    int i;

    if (ParityMkdir(PARITY_DIR) != 0 && errno != EEXIST)
    {
        fprintf(stderr, "PARITY cannot create artifact dir %s: %s\n",
                PARITY_DIR, strerror(errno));
        return;
    }

    for (i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
        diff[i] = (nativeFrame[i] & 0x7FFF) != (oracleFrame[i] & 0x7FFF) ? 1 : 0;

    snprintf(path, sizeof(path), "%s/snap-%u.bin", PARITY_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(snapshot, 1, sizeof(*snapshot), f);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/native-%u.bin", PARITY_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(nativeFrame, 2, DISPLAY_WIDTH * DISPLAY_HEIGHT, f);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/gba-%u.bin", PARITY_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(oracleFrame, 2, DISPLAY_WIDTH * DISPLAY_HEIGHT, f);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/layers-%u.bin", PARITY_DIR, frameNumber);
    if (oracleLayers != NULL && (f = fopen(path, "wb")) != NULL)
    {
        fwrite(oracleLayers, 1, DISPLAY_WIDTH * DISPLAY_HEIGHT, f);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/diff-%u.bin", PARITY_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(diff, 1, DISPLAY_WIDTH * DISPLAY_HEIGHT, f);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/report-%u.txt", PARITY_DIR, frameNumber);
    if ((f = fopen(path, "w")) != NULL)
    {
        fprintf(f, "native overworld runtime parity mismatch report\n");
        fprintf(f, "frame=%u\n", frameNumber);
        fprintf(f, "map=%ldx%ld (metatiles)\n",
                (long)snapshot->mapWidth, (long)snapshot->mapHeight);
        fprintf(f, "cameraMapX=%ld\n", (long)snapshot->cameraMapX);
        fprintf(f, "cameraMapY=%ld\n", (long)snapshot->cameraMapY);
        fprintf(f, "cameraX=%d (sub-tile pan)\n", snapshot->cameraX);
        fprintf(f, "cameraY=%d\n", snapshot->cameraY);
        fprintf(f, "bg1Cnt=0x%04x bg1Hofs=0x%04x bg1Vofs=0x%04x\n",
                snapshot->bgCnt[0], snapshot->bgHofs[0], snapshot->bgVofs[0]);
        fprintf(f, "bg2Cnt=0x%04x bg2Hofs=0x%04x bg2Vofs=0x%04x\n",
                snapshot->bgCnt[1], snapshot->bgHofs[1], snapshot->bgVofs[1]);
        fprintf(f, "bg3Cnt=0x%04x bg3Hofs=0x%04x bg3Vofs=0x%04x\n",
                snapshot->bgCnt[2], snapshot->bgHofs[2], snapshot->bgVofs[2]);
        fprintf(f, "dispCnt=0x%04x\n", snapshot->dispCnt);
        fprintf(f, "mismatches=%u\n", mismatches);
        fprintf(f, "comparedPixels=%llu\n", (unsigned long long)comparedPixels);
        fprintf(f, "firstMismatch=(%u,%u) native=0x%04x gba=0x%04x\n",
                firstX, firstY, firstNative & 0x7FFF, firstGba & 0x7FFF);
        if (oracleLayers != NULL)
            fprintf(f, "layerHistogram: backdrop=%u bg0=%u bg1=%u bg2=%u bg3=%u\n",
                    layerHist[0], layerHist[1], layerHist[2], layerHist[3], layerHist[4]);
        fclose(f);
    }
    sDumpsWritten++;
}

u32 NativeOverworldParity_EndFrame(const struct NativeOverworldSnapshot *snapshot,
                                   const u16 *nativeFrame,
                                   const u16 *oracleFrame,
                                   const u8 *oracleLayers,
                                   enum NativeParityCaptureOutcome outcome)
{
    static const u32 required = NATIVE_CAPABILITY_MAP_BACKGROUND
                              | NATIVE_CAPABILITY_LIVE_BG_RING
                              | NATIVE_CAPABILITY_BG_VRAM
                              | NATIVE_CAPABILITY_BG_PALETTE;
    u32 layerHist[5];
    u32 mismatches = 0;
    u32 firstX = 0;
    u32 firstY = 0;
    u16 firstNative = 0;
    u16 firstGba = 0;
    u32 x;
    u32 y;
    u32 i;

    if (!sEnabled)
        return 0;

    switch (outcome)
    {
    case PARITY_OUTCOME_ABORTED:
        sAbortedCaptures++;
        if (sAbortedCaptures <= 8 || (sAbortedCaptures % 256) == 0)
        {
            fprintf(stderr, "PARITY SKIP: capture aborted (pipeline never ran) frame=%u\n",
                    sFrameCounter);
            fflush(stderr);
        }
        return 0;
    case PARITY_OUTCOME_SNAPSHOT_FAILED:
        sSnapshotFailed++;
        if (snapshot != NULL)
            ReportSkipOnce(snapshot, snapshot->fallbackReason, "snapshot capture failed", sFrameCounter);
        return 0;
    case PARITY_OUTCOME_NATIVE_FAILED:
        sNativeFailed++;
        {
            enum NativeOverworldFallbackReason reason = (snapshot != NULL)
                ? snapshot->fallbackReason : NATIVE_FALLBACK_NONE;
            if (reason == NATIVE_FALLBACK_NONE)
                reason = NATIVE_FALLBACK_BG_CONFIG;
            ReportSkipOnce(snapshot, reason, "native renderer rejected capability", sFrameCounter);
        }
        return 0;
    case PARITY_OUTCOME_ORACLE_MISSING:
        sOracleMissing++;
        if (!sOracleMissingLogged)
        {
            sOracleMissingLogged = TRUE;
            fprintf(stderr, "PARITY SKIP: oracle not produced (frame=%u)\n", sFrameCounter);
            fflush(stderr);
        }
        return 0;
    case PARITY_OUTCOME_COMPARED:
        break;
    case PARITY_OUTCOME_NONE:
    default:
        return 0;
    }

    if (snapshot == NULL || nativeFrame == NULL || oracleFrame == NULL)
    {
        // Defensive: outcome == COMPARED but a required buffer is missing. The
        // caller contract forbids this, but account it rather than drop it.
        sAbortedCaptures++;
        fprintf(stderr, "PARITY SKIP: compared outcome without all buffers (frame=%u)\n",
                sFrameCounter);
        fflush(stderr);
        return 0;
    }
    sSnapshotSucceeded++;
    sNativeRendered++;
    sOracleProduced++;
    sComparedFrames++;

    if (snapshot->fallbackReason != NATIVE_FALLBACK_NONE
     || (snapshot->requiredCapabilities & required) != required)
    {
        // The capture gate and common body set these on success; reaching here
        // means game state changed mid-frame. Account it explicitly.
        sAbortedCaptures++;
        fprintf(stderr,
                "PARITY SKIP: capture rejected at compare stage frame=%u reason=%s (state changed mid-frame)\n",
                sFrameCounter, FallbackName(ClampReason(snapshot->fallbackReason)));
        fflush(stderr);
        return 0;
    }

    MaybeEmitScrollDiagnostic(snapshot);

    memset(layerHist, 0, sizeof(layerHist));
    for (y = 0; y < DISPLAY_HEIGHT; y++)
    {
        for (x = 0; x < DISPLAY_WIDTH; x++)
        {
            i = y * DISPLAY_WIDTH + x;
            sComparedPixels++;
            if ((nativeFrame[i] & 0x7FFF) != (oracleFrame[i] & 0x7FFF))
            {
                if (mismatches == 0)
                {
                    firstX = x;
                    firstY = y;
                    firstNative = nativeFrame[i];
                    firstGba = oracleFrame[i];
                }
                mismatches++;
                if (oracleLayers != NULL)
                {
                    u8 layer = oracleLayers[i];

                    if (layer > 4)
                        layer = 4;
                    layerHist[layer]++;
                }
            }
        }
    }

    // Dev capture of a real moving-frame presentation, independent of parity
    // success: serialize a bg-scroll-divergent COMPARED frame (uniform scroll
    // that lags the logical camera) so it can be reproduced offline from its
    // snapshot alone. Bounded by sScrollFixtureCap; does not count as clean or
    // mismatch and does not write a mismatch report.
    if (sScrollFixtureCap != 0 && sScrollFixturesWritten < sScrollFixtureCap
     && IsScrollDivergent(snapshot))
    {
        WriteScrollFixture(snapshot, nativeFrame, oracleFrame, oracleLayers,
                           sFrameCounter, mismatches, sComparedPixels);
    }

    if (mismatches == 0)
    {
        sCleanFrames++;
        return 0;
    }

    sMismatchFrames++;
    sMismatchPixels += mismatches;

    if (oracleLayers != NULL)
    {
        fprintf(stderr,
                "PARITY MISMATCH frame=%u map=%ldx%ld cam=(%ld,%ld)+(%d,%d) "
                "mm=%u first=(%u,%u) nat=0x%04x gba=0x%04x bd:%u b0:%u b1:%u b2:%u b3:%u\n",
                sFrameCounter,
                (long)snapshot->mapWidth, (long)snapshot->mapHeight,
                (long)snapshot->cameraMapX, (long)snapshot->cameraMapY,
                snapshot->cameraX, snapshot->cameraY,
                mismatches, firstX, firstY,
                firstNative & 0x7FFF, firstGba & 0x7FFF,
                layerHist[0], layerHist[1], layerHist[2], layerHist[3], layerHist[4]);
    }
    else
    {
        fprintf(stderr,
                "PARITY MISMATCH frame=%u map=%ldx%ld cam=(%ld,%ld)+(%d,%d) "
                "mm=%u first=(%u,%u) nat=0x%04x gba=0x%04x\n",
                sFrameCounter,
                (long)snapshot->mapWidth, (long)snapshot->mapHeight,
                (long)snapshot->cameraMapX, (long)snapshot->cameraMapY,
                snapshot->cameraX, snapshot->cameraY,
                mismatches, firstX, firstY,
                firstNative & 0x7FFF, firstGba & 0x7FFF);
    }
    fflush(stderr);

    if (sDumpsWritten < sDumpCap)
    {
        WriteArtifacts(snapshot, nativeFrame, oracleFrame, oracleLayers,
                       sFrameCounter, mismatches, sComparedPixels,
                       firstX, firstY, firstNative, firstGba, layerHist);
    }
    return mismatches;
}

#if defined(LINUX64) && LINUX64
/* Serialize a presented OBJ frame as an offline fixture under
 * build/native-parity/obj/ (distinct dir from mismatch artifacts). Bounded by
 * sObjFixtureCap; does not touch the parity outcome accounting, mirroring the
 * scroll fixture mechanism. */
static void WriteObjFixture(const struct NativeObjSnapshot *obj, u32 frameNumber)
{
    size_t written = 0;
    char path[512];
    FILE *f;

    if (sObjFixturesWritten >= sObjFixtureCap)
        return;
    if (ParityMkdir(OBJ_FIXTURE_DIR) != 0 && errno != EEXIST)
    {
        fprintf(stderr, "PARITY cannot create obj fixture dir %s: %s\n",
                OBJ_FIXTURE_DIR, strerror(errno));
        return;
    }
    if (!NativeObjSnapshot_Serialize(obj, sObjSerializeBuffer,
                                     sizeof(sObjSerializeBuffer), &written))
    {
        fprintf(stderr, "PARITY OBJ-FIXTURE: serialize failed frame=%u\n", frameNumber);
        return;
    }

    snprintf(path, sizeof(path), "%s/obj-snap-%u.bin", OBJ_FIXTURE_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(sObjSerializeBuffer, 1, written, f);
        fclose(f);
    }
    sObjFixturesWritten++;
    fprintf(stderr,
            "PARITY OBJ-FIXTURE: frame=%u wrote %s (%zu bytes, seq=%llu "
            "commands=%u gSprite=%u raw=%u unpresented=%u)\n",
            frameNumber, path, written,
            (unsigned long long)obj->presentationSequence,
            obj->commandCount, obj->gSpriteCommandCount, obj->rawOamCount,
            obj->unpresentedCount);
    fflush(stderr);
}

u32 NativeOverworldParity_ObjFrame(const struct NativeObjSnapshot *objSnapshot)
{
    bool32 anomaly;

    if (!sEnabled || objSnapshot == NULL)
        return 0;

    anomaly = (objSnapshot->provenanceMismatchCount != 0)
           || (objSnapshot->rawOamCount != 0)
           || (objSnapshot->commandOverflow != 0);

    if (!anomaly)
    {
        sObjDiagEmitCounter++;
        if (sObjDiagEmitCounter < OBJ_DIAG_PERIOD)
            return 0;
        sObjDiagEmitCounter = 0;
    }

    fprintf(stderr,
            "PARITY OBJ frame=%u seq=%llu valid=%d oamAuthority=%d "
            "commands=%u gSprite=%u validated=%u mismatch=%u raw=%u "
            "unpresented=%u unmappedNonPresented=%u overflow=%d "
            "origin=(%ld,%ld) dispCnt=0x%04x bldCnt=0x%04x %s\n",
            sFrameCounter,
            (unsigned long long)objSnapshot->presentationSequence,
            objSnapshot->valid, objSnapshot->oamAuthorityValid,
            objSnapshot->commandCount, objSnapshot->gSpriteCommandCount,
            objSnapshot->validatedCommandCount, objSnapshot->provenanceMismatchCount,
            objSnapshot->rawOamCount, objSnapshot->unpresentedCount,
            objSnapshot->unmappedNonPresentedCount, objSnapshot->commandOverflow,
            (long)objSnapshot->commandCameraOriginX,
            (long)objSnapshot->commandCameraOriginY,
            objSnapshot->dispCnt, objSnapshot->bldCnt,
            anomaly ? "anomaly" : "steady");
    fflush(stderr);

    // Fixture capture: only frames that actually presented OBJ primitives.
    if (sObjFixtureCap != 0 && objSnapshot->commandCount != 0)
        WriteObjFixture(objSnapshot, sFrameCounter);
    return objSnapshot->provenanceMismatchCount;
}
#endif

#if defined(LINUX64) && LINUX64
/* Serialize the composite mismatch artifacts under COMPOSITE_DIR: the native
 * final composite and the real DrawFrame composite (raw u16 frames), both
 * per-pixel winner maps, the BG snapshot (raw struct) and the OBJ snapshot
 * (serialized), and a report with the winner histogram, the captured BG
 * control/scroll/blend registers and the composite trace at the first
 * mismatching pixel. */
static void WriteCompositeArtifacts(const struct NativeOverworldSnapshot *bgSnap,
                                    const struct NativeObjSnapshot *objSnap,
                                    const u16 *gbaComposite,
                                    const u8 *gbaCompositeLayers,
                                    const u8 *bgPriority,
                                    u32 frameNumber, u32 mismatches,
                                    u32 firstX, u32 firstY,
                                    u16 firstNative, u16 firstGba,
                                    u8 firstNativeWin, u8 firstGbaWin,
                                    const u64 *winHist)
{
    char path[512];
    size_t written = 0;
    FILE *f;

    if (ParityMkdir(COMPOSITE_DIR) != 0 && errno != EEXIST)
    {
        fprintf(stderr, "PARITY cannot create composite dir %s: %s\n",
                COMPOSITE_DIR, strerror(errno));
        return;
    }

    snprintf(path, sizeof(path), "%s/native-%u.bin", COMPOSITE_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(sCompNative, 2, DISPLAY_WIDTH * DISPLAY_HEIGHT, f);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/gba-%u.bin", COMPOSITE_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(gbaComposite, 2, DISPLAY_WIDTH * DISPLAY_HEIGHT, f);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/winner-native-%u.bin", COMPOSITE_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(sCompNativeWinner, 1, DISPLAY_WIDTH * DISPLAY_HEIGHT, f);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/winner-gba-%u.bin", COMPOSITE_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(gbaCompositeLayers, 1, DISPLAY_WIDTH * DISPLAY_HEIGHT, f);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/bg-snap-%u.bin", COMPOSITE_DIR, frameNumber);
    if ((f = fopen(path, "wb")) != NULL)
    {
        fwrite(bgSnap, 1, sizeof(*bgSnap), f);
        fclose(f);
    }

    if (NativeObjSnapshot_Serialize(objSnap, sObjSerializeBuffer,
                                    sizeof(sObjSerializeBuffer), &written))
    {
        snprintf(path, sizeof(path), "%s/obj-snap-%u.bin", COMPOSITE_DIR, frameNumber);
        if ((f = fopen(path, "wb")) != NULL)
        {
            fwrite(sObjSerializeBuffer, 1, written, f);
            fclose(f);
        }
    }

    snprintf(path, sizeof(path), "%s/report-%u.txt", COMPOSITE_DIR, frameNumber);
    if ((f = fopen(path, "w")) != NULL)
    {
        struct NativeFieldCompositeTrace trace;

        fprintf(f, "native overworld composite parity mismatch report\n");
        fprintf(f, "frame=%u\n", frameNumber);
        fprintf(f, "mismatches=%u\n", mismatches);
        fprintf(f, "firstMismatch=(%u,%u) native=0x%04x win=%u gba=0x%04x win=%u\n",
                firstX, firstY, firstNative & 0x7FFF, firstNativeWin,
                firstGba & 0x7FFF, firstGbaWin);
        fprintf(f, "winHistogram: backdrop=%llu bg0=%llu bg1=%llu bg2=%llu bg3=%llu "
                   "obj0=%llu obj1=%llu obj2=%llu obj3=%llu\n",
                (unsigned long long)winHist[0], (unsigned long long)winHist[1],
                (unsigned long long)winHist[2], (unsigned long long)winHist[3],
                (unsigned long long)winHist[4], (unsigned long long)winHist[5],
                (unsigned long long)winHist[6], (unsigned long long)winHist[7],
                (unsigned long long)winHist[8]);
        fprintf(f, "bg0Cnt=0x%04x bg1Cnt=0x%04x bg2Cnt=0x%04x bg3Cnt=0x%04x\n",
                bgSnap->bg0Cnt, bgSnap->bgCnt[0], bgSnap->bgCnt[1], bgSnap->bgCnt[2]);
        fprintf(f, "dispCnt=0x%04x bldCnt=0x%04x bldAlpha=0x%04x bldY=%u\n",
                bgSnap->dispCnt, bgSnap->bldCnt, bgSnap->bldAlpha, bgSnap->bldY & 0x1F);
        fprintf(f, "cameraMapX=%ld cameraMapY=%ld cameraX=%d cameraY=%d\n",
                (long)bgSnap->cameraMapX, (long)bgSnap->cameraMapY,
                bgSnap->cameraX, bgSnap->cameraY);
        fprintf(f, "scrollHofs=0x%04x scrollVofs=0x%04x (presentation)\n",
                bgSnap->bgHofs[0] & 0x1FF, bgSnap->bgVofs[0] & 0x1FF);
        fprintf(f, "objSeq=%llu objCommands=%u objGSprites=%u objRaw=%u "
                   "objUnpresented=%u objOverflow=%d\n",
                (unsigned long long)objSnap->presentationSequence,
                objSnap->commandCount, objSnap->gSpriteCommandCount,
                objSnap->rawOamCount, objSnap->unpresentedCount,
                objSnap->commandOverflow);
        fprintf(f, "objCapabilityFlags=0x%x objRejectCount=%u\n",
                sCompObjOut.capabilityFlags, sCompObjOut.commandRejectedCount);
        {
            u16 k;
            for (k = 0; k < sCompObjOut.commandRejectedCount && k < 8; k++)
            {
                fprintf(f, "objReject[%u] oam=%u reason=%s command=%u\n",
                        (unsigned)k, sCompObjOut.rejects[k].oamIndex,
                        ObjRejectName((enum NativeObjRejectReason)
                                      sCompObjOut.rejects[k].reason),
                        sCompObjOut.rejects[k].commandId);
            }
        }
        {
            struct NativeCompositeBlendState blend;

            blend.bldCnt = bgSnap->bldCnt;
            blend.bldAlpha = bgSnap->bldAlpha;
            blend.bldY = bgSnap->bldY;
            blend.dispCnt = bgSnap->dispCnt;
            blend.backdropColor = bgSnap->palette[0];

            if (NativeFieldCompositor_Trace(sCompBgFrame, sCompBgWinner,
                                            bgPriority, sCompBgLayers,
                                            objSnap, &sCompObjOut, &blend,
                                            (u8)firstX, (u8)firstY, &trace))
            {
                fprintf(f,
                        "trace: bgWinner=%u bgPriority=%u bgColor=0x%04x "
                        "objPresent=%d objPriority=%u objColor=0x%04x objOam=%u "
                        "finalWinner=%u finalColor=0x%04x\n",
                        trace.bgWinner, trace.bgPriority, trace.bgColor & 0x7FFF,
                        trace.objPresent != FALSE, trace.objPriority,
                        trace.objColor & 0x7FFF, trace.objTrace.oamIndex,
                        trace.finalWinner, trace.finalColor & 0x7FFF);
                fprintf(f,
                        "blend: bldCnt=0x%04x bldAlpha=0x%04x bldY=%u mode=%u "
                        "eva=%u evb=%u tgt2=0x%02x semi=%u tgtB=%u "
                        "tgtBColor=0x%04x pre=0x%04x applied=%d\n",
                        trace.bldCnt, trace.bldAlpha, trace.bldY & 0x1F,
                        trace.blendMode, trace.eva, trace.evb, trace.tgt2Bits,
                        trace.semiObj, trace.targetBKind,
                        trace.targetBColor & 0x7FFF, trace.preBlendColor & 0x7FFF,
                        trace.blendApplied != FALSE);
            }
        }
        fclose(f);
    }
    sCompDumpsWritten++;
}

bool32 NativeOverworldParity_CompositeBeginFrame(void)
{
    if (!sCompositeChecked)
    {
        const char *flag = getenv("POKEEMERALD_NATIVE_OBJ_PARITY");

        sCompositeChecked = TRUE;
        sCompositeEnabled = (flag != NULL && strcmp(flag, "0") != 0);
        if (sCompositeEnabled)
        {
            sCompositeEvery = (u32)strtoul(EnvOr("POKEEMERALD_NATIVE_OBJ_PARITY_EVERY", "1"),
                                           NULL, 10);
            if (sCompositeEvery == 0)
                sCompositeEvery = 1;
            sCompositeDumpCap = (u32)strtoul(EnvOr("POKEEMERALD_NATIVE_OBJ_PARITY_DUMPS", "8"),
                                             NULL, 10);
            fprintf(stderr,
                    "native composite parity enabled (every %u frames, dump cap %u)\n",
                    sCompositeEvery, sCompositeDumpCap);
            fflush(stderr);
        }
    }
    if (!sCompositeEnabled)
        return FALSE;
    // Share the per-frame counter with the BG-only parity mode: it advances the
    // counter only when IT is enabled, so advance here when it is not. Never
    // double-increment when both modes are active.
    if (!sEnabled)
        sFrameCounter++;
    if (sFrameCounter % sCompositeEvery != 0)
        return FALSE;
    sCompScheduled++;
    return TRUE;
}

u32 NativeOverworldParity_CompositeFrame(const struct NativeOverworldSnapshot *bgSnap,
                                         const struct NativeObjSnapshot *objSnap,
                                         const u16 *gbaComposite,
                                         const u8 *gbaCompositeLayers,
                                         bool32 oracleProduced)
{
    static const u32 required = NATIVE_CAPABILITY_MAP_BACKGROUND
                              | NATIVE_CAPABILITY_LIVE_BG_RING
                              | NATIVE_CAPABILITY_BG_VRAM
                              | NATIVE_CAPABILITY_BG_PALETTE;
    struct NativeFieldCompositorReport report;
    struct NativeFieldCompositeTrace trace;
    struct NativeCompositeBlendState blend;
    u8 bgPriority[4];
    u64 winHist[9];
    u32 mismatches = 0;
    u32 firstX = 0;
    u32 firstY = 0;
    u16 firstNative = 0;
    u16 firstGba = 0;
    u8 firstNativeWin = 0;
    u8 firstGbaWin = 0;
    u32 x;
    u32 y;
    u32 i;
    bool32 rasterized;

    if (!sCompositeEnabled)
        return 0;

    // Every outcome below is accounted (item 15): no capture ever disappears.
    if (bgSnap == NULL || objSnap == NULL || gbaComposite == NULL
     || gbaCompositeLayers == NULL)
    {
        sCompAborted++;
        if (!sCompAbortedLogged)
        {
            sCompAbortedLogged = TRUE;
            fprintf(stderr, "PARITY COMPOSITE SKIP: missing buffers frame=%u\n",
                    sFrameCounter);
            fflush(stderr);
        }
        return 0;
    }
    if (bgSnap->fallbackReason != NATIVE_FALLBACK_NONE
     || (bgSnap->requiredCapabilities & required) != required)
    {
        sCompBgSnapFailed++;
        if (!sCompBgSnapLogged)
        {
            sCompBgSnapLogged = TRUE;
            fprintf(stderr,
                    "PARITY COMPOSITE SKIP: BG snapshot rejected frame=%u reason=%s\n",
                    sFrameCounter, FallbackName(ClampReason(bgSnap->fallbackReason)));
            fflush(stderr);
        }
        return 0;
    }
    if (!objSnap->valid)
    {
        sCompObjSnapFailed++;
        if (!sCompObjSnapLogged)
        {
            sCompObjSnapLogged = TRUE;
            fprintf(stderr, "PARITY COMPOSITE SKIP: OBJ snapshot invalid frame=%u\n",
                    sFrameCounter);
            fflush(stderr);
        }
        return 0;
    }
    if (!oracleProduced)
    {
        sCompOracleMissing++;
        if (!sCompOracleMissingLogged)
        {
            sCompOracleMissingLogged = TRUE;
            fprintf(stderr,
                    "PARITY COMPOSITE SKIP: composite oracle not produced frame=%u\n",
                    sFrameCounter);
            fflush(stderr);
        }
        return 0;
    }

    // Rasterize OUTSIDE any assert (side effect; the harness-assert footgun
    // documented in the offline oracle unit applies to any future edit).
    rasterized = NativeObjRender_Rasterize(objSnap, &sCompObjOut);
    if (!rasterized || !sCompObjOut.produced)
    {
        sCompObjSnapFailed++;
        if (!sCompObjSnapLogged)
        {
            sCompObjSnapLogged = TRUE;
            fprintf(stderr, "PARITY COMPOSITE SKIP: OBJ rasterize failed frame=%u\n",
                    sFrameCounter);
            fflush(stderr);
        }
        return 0;
    }
    if (sCompObjOut.capabilityFlags != 0)
    {
        u32 seenReasons = 0; // multi-reason frames: one frame per distinct reason
        u32 r;
        u16 k;

        sCompObjUnsupported++;
        if (!sCompObjUnsupportedLogged)
        {
            sCompObjUnsupportedLogged = TRUE;
            fprintf(stderr,
                    "PARITY COMPOSITE SKIP: OBJ capability unsupported frame=%u "
                    "flags=0x%x rejects=%u\n",
                    sFrameCounter, sCompObjOut.capabilityFlags,
                    sCompObjOut.commandRejectedCount);
            fflush(stderr);
        }
        // Stage 3D histogram: frame bins (once per distinct reason) + primitive
        // bins (every rejected presented primitive, from the reject records).
        for (k = 0; k < sCompObjOut.commandRejectedCount
                    && k < NATIVE_SPRITE_COMMAND_MAX; k++)
        {
            r = sCompObjOut.rejects[k].reason;
            if (r == NATIVE_OBJ_REJECT_NONE || r >= NATIVE_OBJ_REJECT_REASON_COUNT)
                continue;
            sObjUnsupportedPrimByReason[r]++;
            if (!(seenReasons & (1u << r)))
            {
                seenReasons |= 1u << r;
                sObjUnsupportedFrameByReason[r]++;
            }
        }
        if (sCompObjOut.capabilityFlags & NATIVE_OBJ_CAP_BLEND_CONFIG)
            sObjUnsupportedFrameBlendConfig++;
        return 0;
    }
    if (!NativeOverworldRenderer_DrawCompositeFrame(bgSnap, &sCompObjOut,
                                                    sCompBgFrame, sCompBgWinner,
                                                    sCompBgLayers,
                                                    sCompNative, sCompNativeWinner,
                                                    &report))
    {
        switch (report.reason)
        {
        case NATIVE_COMPOSITE_FALLBACK_BG_UNSUPPORTED:
            sCompBgUnsupported++;
            break;
        case NATIVE_COMPOSITE_FALLBACK_BG0_OVERLAY:
            sCompBg0Overlay++;
            break;
        case NATIVE_COMPOSITE_FALLBACK_BLEND_EFFECT:
            sCompBlendEffect++;
            break;
        case NATIVE_COMPOSITE_FALLBACK_OBJ_NOT_PRODUCED:
        case NATIVE_COMPOSITE_FALLBACK_OBJ_UNSUPPORTED:
            sCompObjUnsupported++;
            break;
        case NATIVE_COMPOSITE_FALLBACK_NULL_INPUT:
        default:
            sCompAborted++;
            break;
        }
        // The gates above normally short-circuit these, so a fallback here means
        // game state changed mid-frame; log the first occurrence of each kind.
        if ((report.reason == NATIVE_COMPOSITE_FALLBACK_BG_UNSUPPORTED
             && !sCompBgUnsupportedLogged)
         || (report.reason == NATIVE_COMPOSITE_FALLBACK_BG0_OVERLAY
             && !sCompBg0OverlayLogged)
         || (report.reason == NATIVE_COMPOSITE_FALLBACK_BLEND_EFFECT
             && !sCompBlendLogged))
        {
            if (report.reason == NATIVE_COMPOSITE_FALLBACK_BG_UNSUPPORTED)
                sCompBgUnsupportedLogged = TRUE;
            else if (report.reason == NATIVE_COMPOSITE_FALLBACK_BG0_OVERLAY)
                sCompBg0OverlayLogged = TRUE;
            else
                sCompBlendLogged = TRUE;
            fprintf(stderr,
                    "PARITY COMPOSITE SKIP: DrawCompositeFrame reason=%u frame=%u "
                    "objFlags=0x%x bg0Content=%u\n",
                    report.reason, sFrameCounter, report.objCapabilityFlags,
                    report.bg0Content);
            fflush(stderr);
        }
        return 0;
    }

    sCompCompared++;
    memset(winHist, 0, sizeof(winHist));
    for (y = 0; y < DISPLAY_HEIGHT; y++)
    {
        for (x = 0; x < DISPLAY_WIDTH; x++)
        {
            i = y * DISPLAY_WIDTH + x;
            sCompComparedPixels++;
            if ((sCompNative[i] & 0x7FFF) != (gbaComposite[i] & 0x7FFF)
             || sCompNativeWinner[i] != gbaCompositeLayers[i])
            {
                if (mismatches == 0)
                {
                    firstX = x;
                    firstY = y;
                    firstNative = sCompNative[i];
                    firstGba = gbaComposite[i];
                    firstNativeWin = sCompNativeWinner[i];
                    firstGbaWin = gbaCompositeLayers[i];
                }
                mismatches++;
                if (gbaCompositeLayers[i] < 9)
                    winHist[gbaCompositeLayers[i]]++;
            }
        }
    }

    if (mismatches == 0)
    {
        sCompClean++;
        return 0;
    }

    sCompMismatchFrames++;
    sCompMismatchPixels += mismatches;

    // The winning BG priorities from the CAPTURED BGCNT registers, indexed by
    // GBA bgnum (matching NativeFieldCompositor_Trace's contract).
    bgPriority[0] = (u8)(bgSnap->bg0Cnt & 3);
    bgPriority[1] = (u8)(bgSnap->bgCnt[0] & 3);
    bgPriority[2] = (u8)(bgSnap->bgCnt[1] & 3);
    bgPriority[3] = (u8)(bgSnap->bgCnt[2] & 3);

    // Stage 3E: the frame-wide blend state for the trace (same values the
    // compositor used on this frame).
    blend.bldCnt = bgSnap->bldCnt;
    blend.bldAlpha = bgSnap->bldAlpha;
    blend.bldY = bgSnap->bldY;
    blend.dispCnt = bgSnap->dispCnt;
    blend.backdropColor = bgSnap->palette[0];

    // Runtime exposure of the composite trace (item 13): reconstruct the first
    // mismatching pixel's decision on BOTH sides from the produced buffers.
    // Stage 3E item 16: the dump carries the top OBJ trace (oamIndex), the
    // second-target trace (targetBKind/targetBColor), and the blend registers
    // (BLDCNT/BLDALPHA/BLDY/EVA/EVB + TGT2 source bits).
    if (NativeFieldCompositor_Trace(sCompBgFrame, sCompBgWinner, bgPriority,
                                    sCompBgLayers, objSnap, &sCompObjOut, &blend,
                                    (u8)firstX, (u8)firstY, &trace))
    {
        fprintf(stderr,
                "PARITY COMPOSITE MISMATCH frame=%u mm=%u first=(%u,%u) "
                "nat=0x%04x win=%u gba=0x%04x win=%u "
                "bd:%llu b0:%llu b1:%llu b2:%llu b3:%llu o0:%llu o1:%llu o2:%llu o3:%llu\n",
                sFrameCounter, mismatches, firstX, firstY,
                firstNative & 0x7FFF, firstNativeWin, firstGba & 0x7FFF, firstGbaWin,
                (unsigned long long)winHist[0], (unsigned long long)winHist[1],
                (unsigned long long)winHist[2], (unsigned long long)winHist[3],
                (unsigned long long)winHist[4], (unsigned long long)winHist[5],
                (unsigned long long)winHist[6], (unsigned long long)winHist[7],
                (unsigned long long)winHist[8]);
        fprintf(stderr,
                "  trace: bgWinner=%u bgPrio=%u bgColor=0x%04x obj=%d objPrio=%u "
                "objColor=0x%04x objOam=%u final=%u finalColor=0x%04x\n",
                trace.bgWinner, trace.bgPriority, trace.bgColor & 0x7FFF,
                trace.objPresent != FALSE, trace.objPriority,
                trace.objColor & 0x7FFF, trace.objTrace.oamIndex,
                trace.finalWinner, trace.finalColor & 0x7FFF);
        fprintf(stderr,
                "  blend: bldCnt=0x%04x bldAlpha=0x%04x bldY=0x%02x mode=%u "
                "eva=%u evb=%u tgt2=0x%02x semi=%u tgtB=%u tgtBColor=0x%04x "
                "pre=0x%04x applied=%d\n",
                trace.bldCnt, trace.bldAlpha, trace.bldY & 0xFF, trace.blendMode,
                trace.eva, trace.evb, trace.tgt2Bits, trace.semiObj,
                trace.targetBKind, trace.targetBColor & 0x7FFF,
                trace.preBlendColor & 0x7FFF, trace.blendApplied != FALSE);
    }
    else
    {
        fprintf(stderr,
                "PARITY COMPOSITE MISMATCH frame=%u mm=%u first=(%u,%u) "
                "nat=0x%04x win=%u gba=0x%04x win=%u "
                "bd:%llu b0:%llu b1:%llu b2:%llu b3:%llu o0:%llu o1:%llu o2:%llu o3:%llu\n",
                sFrameCounter, mismatches, firstX, firstY,
                firstNative & 0x7FFF, firstNativeWin, firstGba & 0x7FFF, firstGbaWin,
                (unsigned long long)winHist[0], (unsigned long long)winHist[1],
                (unsigned long long)winHist[2], (unsigned long long)winHist[3],
                (unsigned long long)winHist[4], (unsigned long long)winHist[5],
                (unsigned long long)winHist[6], (unsigned long long)winHist[7],
                (unsigned long long)winHist[8]);
    }
    fflush(stderr);

    if (sCompDumpsWritten < sCompositeDumpCap)
    {
        WriteCompositeArtifacts(bgSnap, objSnap, gbaComposite, gbaCompositeLayers,
                                bgPriority, sFrameCounter, mismatches,
                                firstX, firstY, firstNative, firstGba,
                                firstNativeWin, firstGbaWin, winHist);
    }
    return mismatches;
}
#endif

void NativeOverworldParity_Shutdown(void)
{
    u32 skipped;
    int r;

#if defined(LINUX64) && LINUX64
    if (!sEnabled && !sCompositeEnabled)
        return;
#else
    if (!sEnabled)
        return;
#endif
    skipped = sSnapshotFailed + sNativeFailed + sOracleMissing + sAbortedCaptures;
    fprintf(stderr,
            "PARITY SUMMARY frames=%u scheduled=%u snapshots_ok=%u snapshots_failed=%u "
            "native_ok=%u native_failed=%u oracle_ok=%u oracle_missing=%u "
            "compared=%u clean=%u mismatch_frames=%u mismatch_pixels=%llu "
            "compared_pixels=%llu skipped=%u aborted=%u dumps=%u scroll_fixtures=%u\n",
            sFrameCounter, sScheduledCaptures, sSnapshotSucceeded, sSnapshotFailed,
            sNativeRendered, sNativeFailed, sOracleProduced, sOracleMissing,
            sComparedFrames, sCleanFrames, sMismatchFrames,
            (unsigned long long)sMismatchPixels,
            (unsigned long long)sComparedPixels,
            skipped, sAbortedCaptures, sDumpsWritten, sScrollFixturesWritten);
    for (r = 1; r < NATIVE_FALLBACK_REASON_COUNT; r++)
    {
        if (sSkippedByReason[r] != 0)
        {
            fprintf(stderr, "PARITY SKIP reason=%-20s count=%u\n",
                    FallbackName((enum NativeOverworldFallbackReason)r), sSkippedByReason[r]);
        }
    }
#if defined(LINUX64) && LINUX64
    if (sCompositeEnabled)
    {
        fprintf(stderr,
                "PARITY COMPOSITE SUMMARY scheduled=%u compared=%u clean=%u "
                "mismatch_frames=%u mismatch_pixels=%llu compared_pixels=%llu "
                "bg_snap_failed=%u obj_snap_failed=%u obj_unsupported=%u "
                "bg_unsupported=%u bg0_overlay=%u blend_effect=%u "
                "oracle_missing=%u aborted=%u dumps=%u\n",
                sCompScheduled, sCompCompared, sCompClean, sCompMismatchFrames,
                (unsigned long long)sCompMismatchPixels,
                (unsigned long long)sCompComparedPixels,
                sCompBgSnapFailed, sCompObjSnapFailed, sCompObjUnsupported,
                sCompBgUnsupported, sCompBg0Overlay, sCompBlendEffect,
                sCompOracleMissing, sCompAborted, sCompDumpsWritten);
        for (r = 1; r < NATIVE_OBJ_REJECT_REASON_COUNT; r++)
        {
            if (sObjUnsupportedFrameByReason[r] != 0 || sObjUnsupportedPrimByReason[r] != 0)
            {
                fprintf(stderr,
                        "PARITY OBJ CAPABILITY reason=%-15s frame=%u prim=%u\n",
                        ObjRejectName((enum NativeObjRejectReason)r),
                        sObjUnsupportedFrameByReason[r],
                        sObjUnsupportedPrimByReason[r]);
            }
        }
        if (sObjUnsupportedFrameBlendConfig != 0)
        {
            fprintf(stderr, "PARITY OBJ CAPABILITY reason=%s frame=%u prim=0\n",
                    "blend-config", sObjUnsupportedFrameBlendConfig);
        }
    }
#endif
    fflush(stderr);
}

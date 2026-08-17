/*
 * Unit test for the runtime parity capture module (native_overworld_parity.c).
 *
 * The module is included directly so its internal counters can be driven
 * deterministically without env plumbing: we set sEnabled / sEvery / sDumpCap /
 * sFrameCounter and call BeginFrame / EndFrame / Shutdown directly.
 *
 * Covers the interval gate, the explicit capture-outcome accounting (the Stage-2
 * regression: a scheduled capture must never disappear silently), clean/mismatch
 * accounting, bit-15 alpha masking, winner-layer classification, one-time skip
 * diagnostics, and artifact writing.
 *
 * Run via tests/native_overworld_renderer_test.sh.
 */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "../src/platform/native_overworld_parity.c"

#define CAPABLE (NATIVE_CAPABILITY_MAP_BACKGROUND | NATIVE_CAPABILITY_LIVE_BG_RING \
               | NATIVE_CAPABILITY_BG_VRAM | NATIVE_CAPABILITY_BG_PALETTE)

static struct NativeOverworldSnapshot MakeSnapshot(enum NativeOverworldFallbackReason reason)
{
    struct NativeOverworldSnapshot snap;

    memset(&snap, 0, sizeof(snap));
    snap.mapWidth = 40;
    snap.mapHeight = 40;
    snap.cameraMapX = 10;
    snap.cameraMapY = 12;
    snap.cameraX = 3;
    snap.cameraY = 5;
    snap.bgCnt[0] = 0x2411;
    snap.bgCnt[1] = 0x2414;
    snap.bgCnt[2] = 0x2418;
    snap.bgHofs[0] = 3;
    snap.bgVofs[0] = 5;
    snap.dispCnt = 0x0100;
    snap.requiredCapabilities = CAPABLE;
    snap.fallbackReason = reason;
    return snap;
}

/* Composite fake-render toggles (declared before ResetModule so it can reset
 * them; the stub functions below read them). */
static u32 gFakeObjFlags = 0;       /* OBJ capabilityFlags the fake reports */
static u16 gFakeNativeFill = 0x7B9C;/* native composite color the fake writes */
static u8 gFakeNativeWin = 2;       /* native winner the fake writes (BG1) */
/* Fake reject records: reason is the value at rejects[k].reason; a NONE reason
 * terminates the list (up to gFakeRejectCount entries). */
static u8 gFakeRejectReasons[4] = { 0, 0, 0, 0 };
static u8 gFakeRejectCount = 0;     /* number of fake reject records */

static void ResetModule(void)
{
    sChecked = TRUE;
    sEnabled = TRUE;
    sEvery = 1;
    sDumpCap = 0;
    sFrameCounter = 0;
    sScheduledCaptures = 0;
    sSnapshotSucceeded = 0;
    sSnapshotFailed = 0;
    sNativeRendered = 0;
    sNativeFailed = 0;
    sOracleProduced = 0;
    sOracleMissing = 0;
    sComparedFrames = 0;
    sCleanFrames = 0;
    sMismatchFrames = 0;
    sMismatchPixels = 0;
    sComparedPixels = 0;
    sAbortedCaptures = 0;
    sDumpsWritten = 0;
    sScrollFixtureCap = 0;
    sScrollFixturesWritten = 0;
    sScrollDiagEmitCounter = 0;
    memset(sSkippedByReason, 0, sizeof(sSkippedByReason));
    memset(sSkipMessageLogged, 0, sizeof(sSkipMessageLogged));
    sOracleMissingLogged = FALSE;
    /* Stage 3C composite parity state. Off by default; tests enable it. */
    sCompositeChecked = TRUE;
    sCompositeEnabled = FALSE;
    sCompositeEvery = 1;
    sCompositeDumpCap = 8;
    sCompScheduled = 0;
    sCompCompared = 0;
    sCompClean = 0;
    sCompMismatchFrames = 0;
    sCompMismatchPixels = 0;
    sCompComparedPixels = 0;
    sCompBgSnapFailed = 0;
    sCompObjSnapFailed = 0;
    sCompObjUnsupported = 0;
    sCompBgUnsupported = 0;
    sCompBg0Overlay = 0;
    sCompBlendEffect = 0;
    sCompOracleMissing = 0;
    sCompAborted = 0;
    sCompDumpsWritten = 0;
    sCompBgSnapLogged = FALSE;
    sCompObjSnapLogged = FALSE;
    sCompObjUnsupportedLogged = FALSE;
    sCompBgUnsupportedLogged = FALSE;
    sCompBg0OverlayLogged = FALSE;
    sCompBlendLogged = FALSE;
    sCompOracleMissingLogged = FALSE;
    sCompAbortedLogged = FALSE;
    gFakeObjFlags = 0;
    gFakeNativeFill = 0x7B9C;
    gFakeNativeWin = 2;
}

/* Stubs for the renderer entry points NativeOverworldParity_CompositeFrame calls.
 * This test links ONLY native_overworld_parity.c (via the include above); the
 * real renderers (native_obj_renderer.c / native_field_compositor.c /
 * native_overworld_renderer.c / native_sprite_snapshot.c) are not linked. These
 * fakes let the module's own accounting be driven deterministically: the fake
 * "render" writes the module's native composite buffers from the toggles below,
 * so clean and mismatching compares can be forced without any tile/OAM state.
 * (The toggles themselves are declared above, before ResetModule.) The stub
 * definitions take the REAL symbol names (NativeObjRender_Rasterize etc.):
 * the test links only native_overworld_parity.c, so these are the sole
 * definitions in the link. */
bool32 NativeObjRender_Rasterize(const struct NativeObjSnapshot *snap,
                                 struct NativeObjRenderOutput *out)
{
    (void)snap;
    out->produced = TRUE;
    out->capabilityFlags = gFakeObjFlags;
    out->commandRejectedCount = 0;
    memset(out->rejects, 0, sizeof(out->rejects));
    {
        u8 k;
        for (k = 0; k < gFakeRejectCount && k < 4
                    && gFakeRejectReasons[k] != 0; k++)
        {
            out->rejects[k].oamIndex = k;
            out->rejects[k].reason = gFakeRejectReasons[k];
            out->rejects[k].commandId = k + 10;
            out->commandRejectedCount++;
        }
    }
    return TRUE;
}

bool32 NativeObjSnapshot_Serialize(const struct NativeObjSnapshot *obj,
                                   u8 *buf, size_t capacity, size_t *written)
{
    (void)obj;
    (void)buf;
    (void)capacity;
    *written = 0;
    return FALSE; /* no obj-snap artifact file in this test */
}

bool32 NativeOverworldRenderer_DrawCompositeFrame(const struct NativeOverworldSnapshot *bgSnap,
                                                  const struct NativeObjRenderOutput *objOut,
                                                  u16 *bgFrame, u8 *bgWinner, u16 *bgLayers,
                                                  u16 *outFrame, u8 *outWinner,
                                                  struct NativeFieldCompositorReport *report)
{
    int i;

    (void)bgSnap;
    (void)objOut;
    (void)bgLayers;
    report->reason = NATIVE_COMPOSITE_OK;
    for (i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        bgFrame[i] = 0x7B9C;
        bgWinner[i] = 2;
        outFrame[i] = gFakeNativeFill;
        outWinner[i] = gFakeNativeWin;
    }
    return TRUE;
}

bool32 NativeFieldCompositor_Trace(const u16 *bgFrame, const u8 *bgWinner,
                                   const u8 *bgPriority, const u16 *bgLayers,
                                   const struct NativeObjSnapshot *objSnap,
                                   const struct NativeObjRenderOutput *objOut,
                                   const struct NativeCompositeBlendState *blend,
                                   u8 x, u8 y,
                                   struct NativeFieldCompositeTrace *trace)
{
    (void)bgFrame;
    (void)bgWinner;
    (void)bgPriority;
    (void)bgLayers;
    (void)objSnap;
    (void)objOut;
    (void)blend;
    (void)x;
    (void)y;
    (void)trace;
    return FALSE; /* trace unavailable: exercises the non-trace mismatch line */
}

static void TestDisabled(void)
{
    ResetModule();
    sEnabled = FALSE;
    assert(NativeOverworldParity_BeginFrame() == FALSE);
    assert(sFrameCounter == 0);
    assert(NativeOverworldParity_EndFrame(NULL, NULL, NULL, NULL, PARITY_OUTCOME_COMPARED) == 0);
    assert(NativeOverworldParity_EndFrame(NULL, NULL, NULL, NULL, PARITY_OUTCOME_SNAPSHOT_FAILED) == 0);
    fprintf(stderr, "parity module disabled path OK\n");
}

static void TestIntervalGate(void)
{
    ResetModule();
    sEvery = 3;
    assert(NativeOverworldParity_BeginFrame() == FALSE); /* frame 1 */
    assert(NativeOverworldParity_BeginFrame() == FALSE); /* frame 2 */
    assert(NativeOverworldParity_BeginFrame() == TRUE);  /* frame 3 */
    assert(NativeOverworldParity_BeginFrame() == FALSE); /* frame 4 */
    assert(NativeOverworldParity_BeginFrame() == FALSE); /* frame 5 */
    assert(NativeOverworldParity_BeginFrame() == TRUE);  /* frame 6 */
    assert(sScheduledCaptures == 2);
    assert(sFrameCounter == 6);
    fprintf(stderr, "parity module interval gate OK\n");
}

/* Stage-2 regression: a scheduled capture must never disappear silently. This
 * reproduces the observed failure shape (scheduled > 0 but compared == 0): the
 * capture gate rejected the frame, and the module must account it explicitly as
 * a snapshot failure with a skip reason instead of returning 0 with no trace. */
static void TestScheduledNotSilentlyDropped(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_DISPLAY_LAYERS);
    u16 native[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    ResetModule();
    memset(native, 0, sizeof(native));
    memset(gba, 0, sizeof(gba));

    assert(NativeOverworldParity_BeginFrame() == TRUE); /* frame 1, scheduled */
    assert(sScheduledCaptures == 1);
    assert(NativeOverworldParity_EndFrame(&snap, NULL, gba, NULL,
                                          PARITY_OUTCOME_SNAPSHOT_FAILED) == 0);
    assert(sSnapshotFailed == 1);
    assert(sScheduledCaptures == 1);
    assert(sComparedFrames == 0);
    assert(sComparedPixels == 0);
    assert(sMismatchFrames == 0);
    assert(sSkippedByReason[NATIVE_FALLBACK_DISPLAY_LAYERS] == 1);
    assert(sSkipMessageLogged[NATIVE_FALLBACK_DISPLAY_LAYERS]);

    /* A second identical failure increments the histogram but does NOT spam a
     * second skip message (one-time-per-reason diagnostics). */
    assert(NativeOverworldParity_EndFrame(&snap, NULL, gba, NULL,
                                          PARITY_OUTCOME_SNAPSHOT_FAILED) == 0);
    assert(sSnapshotFailed == 2);
    assert(sSkippedByReason[NATIVE_FALLBACK_DISPLAY_LAYERS] == 2);
    fprintf(stderr, "parity module scheduled capture explicitly accounted OK\n");
}

/* A different skip reason gets its own one-time message slot. */
static void TestDistinctSkipReasons(void)
{
    struct NativeOverworldSnapshot snapHw = MakeSnapshot(NATIVE_FALLBACK_HARDWARE_BLEND);
    struct NativeOverworldSnapshot snapScene = MakeSnapshot(NATIVE_FALLBACK_SCENE_NOT_OVERWORLD);
    u16 native[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    ResetModule();
    memset(native, 0, sizeof(native));
    memset(gba, 0, sizeof(gba));

    assert(NativeOverworldParity_EndFrame(&snapHw, NULL, gba, NULL,
                                          PARITY_OUTCOME_SNAPSHOT_FAILED) == 0);
    assert(NativeOverworldParity_EndFrame(&snapScene, NULL, gba, NULL,
                                          PARITY_OUTCOME_SNAPSHOT_FAILED) == 0);
    assert(NativeOverworldParity_EndFrame(&snapHw, NULL, gba, NULL,
                                          PARITY_OUTCOME_SNAPSHOT_FAILED) == 0);
    assert(sSnapshotFailed == 3);
    assert(sSkippedByReason[NATIVE_FALLBACK_HARDWARE_BLEND] == 2);
    assert(sSkippedByReason[NATIVE_FALLBACK_SCENE_NOT_OVERWORLD] == 1);
    fprintf(stderr, "parity module distinct skip reasons OK\n");
}

static void TestNativeFailed(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    u16 native[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    ResetModule();
    memset(native, 0, sizeof(native));
    memset(gba, 0, sizeof(gba));
    assert(NativeOverworldParity_EndFrame(&snap, NULL, gba, NULL,
                                          PARITY_OUTCOME_NATIVE_FAILED) == 0);
    assert(sNativeFailed == 1);
    assert(sSnapshotSucceeded == 0);
    assert(sComparedFrames == 0);
    /* No fallbackReason set -> reported under the bg-config catch-all. */
    assert(sSkippedByReason[NATIVE_FALLBACK_BG_CONFIG] == 1);
    fprintf(stderr, "parity module native-failed accounting OK\n");
}

static void TestOracleMissing(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    u16 native[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    ResetModule();
    memset(native, 0, sizeof(native));
    memset(gba, 0, sizeof(gba));
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, NULL,
                                          PARITY_OUTCOME_ORACLE_MISSING) == 0);
    assert(sOracleMissing == 1);
    assert(sComparedFrames == 0);
    assert(sOracleMissingLogged);
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, NULL,
                                          PARITY_OUTCOME_ORACLE_MISSING) == 0);
    assert(sOracleMissing == 2);
    fprintf(stderr, "parity module oracle-missing accounting OK\n");
}

static void TestAborted(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    u16 native[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    ResetModule();
    memset(native, 0, sizeof(native));
    memset(gba, 0, sizeof(gba));
    assert(NativeOverworldParity_EndFrame(&snap, NULL, NULL, NULL,
                                          PARITY_OUTCOME_ABORTED) == 0);
    assert(sAbortedCaptures == 1);
    assert(sComparedFrames == 0);
    fprintf(stderr, "parity module aborted-capture accounting OK\n");
}

static void TestClean(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    u16 native[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    ResetModule();
    memset(native, 0x1F, sizeof(native)); /* white everywhere */
    memset(gba, 0x1F, sizeof(gba));
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, NULL,
                                          PARITY_OUTCOME_COMPARED) == 0);
    assert(sSnapshotSucceeded == 1);
    assert(sNativeRendered == 1);
    assert(sOracleProduced == 1);
    assert(sComparedFrames == 1);
    assert(sCleanFrames == 1);
    assert(sComparedPixels == DISPLAY_WIDTH * DISPLAY_HEIGHT);
    fprintf(stderr, "parity module clean frame OK\n");
}

static void TestBit15Masking(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    u16 native[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    ResetModule();
    memset(native, 0x7B9C, sizeof(native));
    memset(gba, 0x7B9C, sizeof(gba));
    /* Oracle pixels carry the alpha flag (bit 15); native does not. Masked
     * comparison must still see them as identical. */
    gba[0] |= 0x8000;
    gba[12345] |= 0x8000;
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, NULL,
                                          PARITY_OUTCOME_COMPARED) == 0);
    assert(sCleanFrames == 1);
    fprintf(stderr, "parity module bit-15 alpha masking OK\n");
}

static void TestMismatchCountAndLayers(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    u16 native[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u8 layers[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    int i;

    ResetModule();
    memset(native, 0x7B9C, sizeof(native));
    memset(gba, 0x7B9C, sizeof(gba));
    memset(layers, 3, sizeof(layers)); /* default BG2 */

    /* Three mismatches, one per distinct winning layer. */
    native[10] = 0x0001;  gba[10] = 0x0002;  layers[10] = 0;       /* backdrop */
    native[20] = 0x0003;  gba[20] = 0x0004;  layers[20] = 2;       /* BG1     */
    native[30] = 0x0005;  gba[30] = 0x0006;  layers[30] = 4;       /* BG3     */

    assert(NativeOverworldParity_EndFrame(&snap, native, gba, layers,
                                          PARITY_OUTCOME_COMPARED) == 3);
    assert(sMismatchFrames == 1);
    assert(sMismatchPixels == 3);
    assert(sCleanFrames == 0);
    assert(sComparedFrames == 1);
    assert(sDumpsWritten == 0); /* dump cap is 0 in this test */
    fprintf(stderr, "parity module mismatch count + accounting OK\n");
}

static bool32 FileExists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return FALSE;
    fclose(f);
    return TRUE;
}

static void TestArtifacts(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    u16 native[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u8 layers[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    char oldCwd[1024];
    char tmpDir[] = "/tmp/native-parity-test.XXXXXX";
    char path[512];
    FILE *f;
    u32 reportedMismatches = 0;

    ResetModule();
    sDumpCap = 1;
    sFrameCounter = 42;
    memset(native, 0x1234, sizeof(native));
    memset(gba, 0x1234, sizeof(gba));
    memset(layers, 2, sizeof(layers));
    native[0] = 0x0001;
    gba[0] = 0x0002;   /* single mismatch, backdrop layer, first pixel */

    assert(getcwd(oldCwd, sizeof(oldCwd)) != NULL);
    assert(mkdtemp(tmpDir) != NULL);
    assert(chdir(tmpDir) == 0);
    /* The module's WriteArtifacts only creates the final leaf directory;
     * provide the parent it expects under the test cwd. */
    assert(mkdir("build", 0755) == 0);
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, layers,
                                          PARITY_OUTCOME_COMPARED) == 1);
    assert(sDumpsWritten == 1);

    snprintf(path, sizeof(path), "build/native-parity/report-42.txt");
    f = fopen(path, "r");
    assert(f != NULL);
    {
        char line[512];
        while (fgets(line, sizeof(line), f) != NULL)
        {
            if (sscanf(line, "mismatches=%u", &reportedMismatches) == 1)
                break;
        }
    }
    fclose(f);
    assert(reportedMismatches == 1);

    snprintf(path, sizeof(path), "build/native-parity/snap-42.bin");
    assert(FileExists(path));
    snprintf(path, sizeof(path), "build/native-parity/native-42.bin");
    assert(FileExists(path));
    snprintf(path, sizeof(path), "build/native-parity/gba-42.bin");
    assert(FileExists(path));
    snprintf(path, sizeof(path), "build/native-parity/layers-42.bin");
    assert(FileExists(path));
    snprintf(path, sizeof(path), "build/native-parity/diff-42.bin");
    assert(FileExists(path));

    /* A second mismatching frame must NOT write artifacts (dump cap reached). */
    native[5] = 0x0007;
    gba[5] = 0x0008;
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, layers,
                                          PARITY_OUTCOME_COMPARED) >= 1);
    assert(sDumpsWritten == 1);
    snprintf(path, sizeof(path), "build/native-parity/report-43.txt");
    assert(!FileExists(path));

    /* No artifact when parity is clean. */
    ResetModule();
    sDumpCap = 8;
    sFrameCounter = 50;
    memset(native, 0x1111, sizeof(native));
    memset(gba, 0x1111, sizeof(gba));
    memset(layers, 2, sizeof(layers));
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, layers,
                                          PARITY_OUTCOME_COMPARED) == 0);
    snprintf(path, sizeof(path), "build/native-parity/report-50.txt");
    assert(!FileExists(path));

    assert(chdir(oldCwd) == 0);
    fprintf(stderr, "parity module artifacts OK\n");
}

/* Stage 2.1: the dev capture mechanism for a bg-scroll-divergent frame. A
 * COMPARED frame whose three layers carry a UNIFORM scroll that lags the logical
 * camera (the normal moving-frame presentation) is serialized as an offline
 * fixture under build/native-parity/scroll/, bounded by sScrollFixtureCap, and
 * WITHOUT counting as parity success (clean/mismatch accounting untouched). */
static void TestScrollFixtureCapture(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    u16 native[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    char oldCwd[1024];
    char tmpDir[] = "/tmp/native-parity-scroll.XXXXXX";
    char path[512];
    FILE *f;
    u32 reportedMismatches;

    ResetModule();
    sScrollFixtureCap = 1;
    sDumpCap = 0; /* do not write mismatch artifacts; only the scroll fixture */
    sFrameCounter = 70;
    /* Divergent: uniform scroll across all three layers that lags the camera
     * (cameraX=3, cameraY=5 from MakeSnapshot). */
    snap.bgHofs[0] = snap.bgHofs[1] = snap.bgHofs[2] = 4;
    snap.bgVofs[0] = snap.bgVofs[1] = snap.bgVofs[2] = 6;
    memset(native, 0x1234, sizeof(native));
    memset(gba, 0x1234, sizeof(gba));

    assert(getcwd(oldCwd, sizeof(oldCwd)) != NULL);
    assert(mkdtemp(tmpDir) != NULL);
    assert(chdir(tmpDir) == 0);
    assert(mkdir("build", 0755) == 0);

    /* Clean divergent frame: parity accounting reports clean, and the fixture
     * mechanism serializes it (without a mismatch report). */
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, NULL,
                                          PARITY_OUTCOME_COMPARED) == 0);
    assert(sCleanFrames == 1);
    assert(sScrollFixturesWritten == 1);

    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-snap-70.bin");
    assert(FileExists(path));
    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-native-70.bin");
    assert(FileExists(path));
    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-gba-70.bin");
    assert(FileExists(path));
    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-report-70.txt");
    f = fopen(path, "r");
    assert(f != NULL);
    reportedMismatches = 999;
    while (fgets(path, sizeof(path), f) != NULL)
    {
        if (sscanf(path, "mismatches=%u", &reportedMismatches) == 1)
            break;
    }
    fclose(f);
    assert(reportedMismatches == 0);
    /* No mismatch report is written by the fixture mechanism (clean frame). */
    snprintf(path, sizeof(path), "build/native-parity/report-70.txt");
    assert(!FileExists(path));

    /* Second divergent frame: fixture cap reached, no more serialized. */
    sFrameCounter = 71;
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, NULL,
                                          PARITY_OUTCOME_COMPARED) == 0);
    assert(sCleanFrames == 2);
    assert(sScrollFixturesWritten == 1);
    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-snap-71.bin");
    assert(!FileExists(path));

    /* Stationary frame (scroll == camera) is NOT divergent -> no fixture. */
    ResetModule();
    sScrollFixtureCap = 1;
    sDumpCap = 0;
    sFrameCounter = 72;
    snap.bgHofs[0] = snap.bgHofs[1] = snap.bgHofs[2] = (u16)(snap.cameraX & 0x1FF);
    snap.bgVofs[0] = snap.bgVofs[1] = snap.bgVofs[2] = (u16)(snap.cameraY & 0x1FF);
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, NULL,
                                          PARITY_OUTCOME_COMPARED) == 0);
    assert(sScrollFixturesWritten == 0);
    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-snap-72.bin");
    assert(!FileExists(path));

    /* Torn latch (layers disagree) is NOT divergent -> no fixture. */
    sFrameCounter = 73;
    snap.bgHofs[0] = 4; snap.bgHofs[1] = 9; snap.bgHofs[2] = 4;
    snap.bgVofs[0] = snap.bgVofs[1] = snap.bgVofs[2] = 6;
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, NULL,
                                          PARITY_OUTCOME_COMPARED) == 0);
    assert(sScrollFixturesWritten == 0);
    snprintf(path, sizeof(path), "build/native-parity/scroll/scroll-snap-73.bin");
    assert(!FileExists(path));

    assert(chdir(oldCwd) == 0);
    fprintf(stderr, "parity module scroll-fixture capture OK\n");
}

/* Stage 2.1: the rate-limited PARITY SCROLL state diagnostic. Moving-frame
 * (scroll-divergent) capture frames log immediately; stationary frames log at
 * most once per SCROLL_DIAG_PERIOD capture frames. The line reports the logical
 * camera (cameraMapX/Y + sub-tile cameraX/Y) and the three BG scroll latches. */
static long CountScrollDiag(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    long count = 0;

    if (f == NULL)
        return -1;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strstr(line, "PARITY SCROLL") != NULL)
            count++;
    }
    fclose(f);
    return count;
}

static void TestScrollDiagnostic(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    u16 native[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    char diagPath[] = "/tmp/native-parity-scroll-diag.XXXXXX";
    char line[512];
    int fd;
    int savedStderr;
    FILE *f;
    long count;
    int i;
    int truncated;

    ResetModule();
    sDumpCap = 0;
    memset(native, 0x5A5A, sizeof(native));
    memset(gba, 0x5A5A, sizeof(gba));

    savedStderr = dup(2);
    assert(savedStderr >= 0);
    fd = mkstemp(diagPath);
    assert(fd >= 0);
    assert(dup2(fd, 2) >= 0); /* route stderr to the capture file */
    close(fd);

    /* Divergent frame logs immediately and reports the divergent state. */
    sFrameCounter = 80;
    snap.bgHofs[0] = snap.bgHofs[1] = snap.bgHofs[2] = 7;
    snap.bgVofs[0] = snap.bgVofs[1] = snap.bgVofs[2] = 9;
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, NULL,
                                          PARITY_OUTCOME_COMPARED) == 0);
    fflush(stderr);
    assert(CountScrollDiag(diagPath) == 1);
    f = fopen(diagPath, "r");
    assert(f != NULL);
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strstr(line, "PARITY SCROLL") != NULL)
        {
            assert(strstr(line, "logical=(10,12)+(3,5)") != NULL);
            assert(strstr(line, "bg1=(0x0007,0x0009)") != NULL);
            assert(strstr(line, "divergent") != NULL);
        }
    }
    fclose(f);

    /* Stationary frames are rate-limited: nothing until the 60th capture frame. */
    ResetModule();
    sDumpCap = 0;
    sFrameCounter = 81;
    snap.bgHofs[0] = snap.bgHofs[1] = snap.bgHofs[2] = (u16)(snap.cameraX & 0x1FF);
    snap.bgVofs[0] = snap.bgVofs[1] = snap.bgVofs[2] = (u16)(snap.cameraY & 0x1FF);
    truncated = truncate(diagPath, 0);
    assert(truncated == 0);
    lseek(fd = dup(2), 0, SEEK_SET); /* keep fd valid for later restore */
    /* Re-point stderr at the now-empty file. */
    fd = open(diagPath, O_WRONLY);
    assert(fd >= 0);
    assert(dup2(fd, 2) >= 0);
    close(fd);

    for (i = 0; i < SCROLL_DIAG_PERIOD - 1; i++)
    {
        assert(NativeOverworldParity_EndFrame(&snap, native, gba, NULL,
                                              PARITY_OUTCOME_COMPARED) == 0);
    }
    fflush(stderr);
    count = CountScrollDiag(diagPath);
    assert(count == 0); /* 59 stationary frames: no diagnostic yet */
    assert(NativeOverworldParity_EndFrame(&snap, native, gba, NULL,
                                          PARITY_OUTCOME_COMPARED) == 0); /* 60th */
    fflush(stderr);
    count = CountScrollDiag(diagPath);
    assert(count == 1);

    assert(dup2(savedStderr, 2) >= 0);
    close(savedStderr);
    unlink(diagPath);
    fprintf(stderr, "parity module scroll diagnostic OK\n");
}

/* Stage 3C composite parity. CompositeBeginFrame returns FALSE when the mode is
 * off, with no counter or accounting effect. */
static void TestCompositeDisabled(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    struct NativeObjSnapshot obj;
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u8 layers[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    ResetModule(); /* sCompositeEnabled = FALSE */
    memset(&obj, 0, sizeof(obj));
    obj.valid = TRUE;
    memset(gba, 0, sizeof(gba));
    memset(layers, 2, sizeof(layers));
    assert(NativeOverworldParity_CompositeBeginFrame() == FALSE);
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE) == 0);
    assert(sFrameCounter == 0);
    assert(sCompScheduled == 0);
    assert(sCompCompared == 0);
    assert(sCompAborted == 0); /* inactive frames are not accounted at all */
    fprintf(stderr, "parity module composite disabled OK\n");
}

/* Env gate + per-frame counter sharing (never double-increments the shared
 * counter when both parity modes are active; advances it itself when the BG-only
 * mode is off). */
static void TestCompositeEnvGateAndCounterSharing(void)
{
    /* Composite-only mode: BG parity off (sEnabled FALSE). The composite gate
     * must advance the shared counter itself and capture every frame at
     * every=1. */
    ResetModule();
    sEnabled = FALSE;
    sCompositeChecked = FALSE;
    sCompositeEnabled = FALSE;
    unsetenv("POKEEMERALD_NATIVE_OBJ_PARITY");
    assert(NativeOverworldParity_CompositeBeginFrame() == FALSE);
    assert(sFrameCounter == 0);
    assert(sCompScheduled == 0);
    setenv("POKEEMERALD_NATIVE_OBJ_PARITY", "1", 1);
    sCompositeChecked = FALSE; /* re-read the env on the next call */
    assert(NativeOverworldParity_CompositeBeginFrame() == TRUE);
    assert(sFrameCounter == 1);
    assert(sCompScheduled == 1);
    assert(NativeOverworldParity_CompositeBeginFrame() == TRUE);
    assert(sFrameCounter == 2);
    assert(sCompScheduled == 2);

    /* Both modes active: BeginFrame advances the counter; CompositeBeginFrame
     * must NOT advance it again (single increment per frame). */
    ResetModule(); /* sEnabled TRUE */
    sCompositeChecked = TRUE;
    sCompositeEnabled = TRUE;
    sCompositeEvery = 1;
    assert(NativeOverworldParity_BeginFrame() == TRUE);          /* frame 1 */
    assert(NativeOverworldParity_CompositeBeginFrame() == TRUE); /* frame 1 */
    assert(sFrameCounter == 1);
    assert(NativeOverworldParity_BeginFrame() == TRUE);          /* frame 2 */
    assert(NativeOverworldParity_CompositeBeginFrame() == TRUE); /* frame 2 */
    assert(sFrameCounter == 2);
    assert(sCompScheduled == 2);
    fprintf(stderr, "parity module composite env gate + counter sharing OK\n");
}

/* Every early-return outcome is accounted explicitly (item 15: no silent early
 * return). All four stop BEFORE the render pipeline runs. */
static void TestCompositeEarlyReturns(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    struct NativeOverworldSnapshot snapRejected = MakeSnapshot(NATIVE_FALLBACK_BG0_OVERLAY);
    struct NativeObjSnapshot obj;
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u8 layers[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    ResetModule();
    sCompositeEnabled = TRUE;
    sCompositeChecked = TRUE;
    sCompositeEvery = 1;
    memset(&obj, 0, sizeof(obj));
    memset(gba, 0, sizeof(gba));
    memset(layers, 2, sizeof(layers));

    /* Missing buffers -> aborted, named, not silent. */
    assert(NativeOverworldParity_CompositeFrame(NULL, &obj, gba, layers, TRUE) == 0);
    assert(sCompAborted == 1);
    assert(sCompCompared == 0);

    /* BG snapshot rejected by the capture gate -> bg_snap_failed (NOT the
     * composite overlay gate: the gate rejects pre-composite). */
    assert(NativeOverworldParity_CompositeFrame(&snapRejected, &obj, gba, layers, TRUE) == 0);
    assert(sCompBgSnapFailed == 1);
    assert(sCompBg0Overlay == 0);
    assert(sCompCompared == 0);

    /* OBJ snapshot invalid -> obj_snap_failed. */
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE) == 0);
    assert(sCompObjSnapFailed == 1);
    assert(sCompCompared == 0);

    /* Oracle not produced -> oracle_missing. */
    obj.valid = TRUE;
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, FALSE) == 0);
    assert(sCompOracleMissing == 1);
    assert(sCompCompared == 0);

    /* OBJ capability unsupported -> obj_unsupported (the fake rasterizer reports
     * nonzero flags), with the capability named. A frame with NO reject records
     * (e.g. the 2D-mapping frame gate) contributes to the total but to no
     * per-reason bin. */
    gFakeObjFlags = 1;
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE) == 0);
    assert(sCompObjUnsupported == 1);
    assert(sCompCompared == 0);
    gFakeObjFlags = 0;

    /* A second identical skip increments the counter but does NOT spam a second
     * inline message (one-time-per-outcome diagnostics). */
    assert(NativeOverworldParity_CompositeFrame(NULL, &obj, gba, layers, TRUE) == 0);
    assert(sCompAborted == 2);
    assert(sCompCompared == 0);
    fprintf(stderr, "parity module composite early-return accounting OK\n");
}

/* Stage 3D histogram: per-reason frame + primitive bins for capability-rejected
 * frames, multi-reason preservation (one frame per distinct reason), and the
 * blend-config frame bin. */
static void TestCompositeObjCapabilityHistogram(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    struct NativeObjSnapshot obj;
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u8 layers[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    int i;

    ResetModule();
    sCompositeEnabled = TRUE;
    sCompositeChecked = TRUE;
    sCompositeEvery = 1;
    memset(&obj, 0, sizeof(obj));
    obj.valid = TRUE;
    for (i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
        gba[i] = 0x7B9C;

    /* One presented affine primitive + one presented 8bpp primitive in the same
     * frame: the frame counts once per distinct reason, each primitive counts
     * once per record. */
    gFakeObjFlags = NATIVE_OBJ_CAP_AFFINE | NATIVE_OBJ_CAP_8BPP;
    gFakeRejectReasons[0] = NATIVE_OBJ_REJECT_AFFINE;
    gFakeRejectReasons[1] = NATIVE_OBJ_REJECT_8BPP;
    gFakeRejectReasons[2] = 0;
    gFakeRejectCount = 2;
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE) == 0);
    assert(sCompObjUnsupported == 1);
    assert(sObjUnsupportedFrameByReason[NATIVE_OBJ_REJECT_AFFINE] == 1);
    assert(sObjUnsupportedFrameByReason[NATIVE_OBJ_REJECT_8BPP] == 1);
    assert(sObjUnsupportedPrimByReason[NATIVE_OBJ_REJECT_AFFINE] == 1);
    assert(sObjUnsupportedPrimByReason[NATIVE_OBJ_REJECT_8BPP] == 1);
    assert(sObjUnsupportedFrameBlendConfig == 0);

    /* A second frame with only the 8bpp primitive: 8bpp frame bin grows, affine
     * stays at 1 (frame counted once per distinct reason per frame). */
    gFakeObjFlags = NATIVE_OBJ_CAP_8BPP;
    gFakeRejectReasons[0] = NATIVE_OBJ_REJECT_8BPP;
    gFakeRejectReasons[1] = 0;
    gFakeRejectCount = 1;
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE) == 0);
    assert(sCompObjUnsupported == 2);
    assert(sObjUnsupportedFrameByReason[NATIVE_OBJ_REJECT_8BPP] == 2);
    assert(sObjUnsupportedPrimByReason[NATIVE_OBJ_REJECT_8BPP] == 2);
    assert(sObjUnsupportedFrameByReason[NATIVE_OBJ_REJECT_AFFINE] == 1);
    assert(sObjUnsupportedPrimByReason[NATIVE_OBJ_REJECT_AFFINE] == 1);

    /* A frame whose only reject record is a 2D-mapping frame gate (no reject
     * records): total grows, no per-reason bin, and the blend-config bin is
     * counted once when the gate flag is set. */
    gFakeObjFlags = NATIVE_OBJ_CAP_2D_MAPPING | NATIVE_OBJ_CAP_BLEND_CONFIG;
    gFakeRejectReasons[0] = 0;
    gFakeRejectCount = 0;
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE) == 0);
    assert(sCompObjUnsupported == 3);
    assert(sObjUnsupportedFrameByReason[NATIVE_OBJ_REJECT_AFFINE] == 1);
    assert(sObjUnsupportedFrameByReason[NATIVE_OBJ_REJECT_8BPP] == 2);
    assert(sObjUnsupportedFrameBlendConfig == 1);

    gFakeObjFlags = 0;
    gFakeRejectCount = 0;
    fprintf(stderr, "parity module composite OBJ capability histogram OK\n");
}

/* Stage 3E accounting: semi-transparent OBJ (objMode==1) is SUPPORTED, so a
 * frame whose raster carries no capability flags goes straight to the compare
 * loop -- it must never be counted as obj_unsupported (the pre-Stage-3E
 * obj-blend bin is gone). The ONLY blend-related unsupported case is a blend
 * config that would recolor NORMAL OBJ (BLEND_CONFIG: TGT1_OBJ set with a
 * non-zero blend mode); such a frame is accounted as obj_unsupported with the
 * explicit blend-config bin and is never compared. */
static void TestCompositeSemiSupportAccounting(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    struct NativeObjSnapshot obj;
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u8 layers[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    int i;

    ResetModule();
    /* ResetModule does not touch the histogram arrays; clear them here. */
    memset(sObjUnsupportedFrameByReason, 0, sizeof(sObjUnsupportedFrameByReason));
    memset(sObjUnsupportedPrimByReason, 0, sizeof(sObjUnsupportedPrimByReason));
    sObjUnsupportedFrameBlendConfig = 0;
    sCompositeEnabled = TRUE;
    sCompositeChecked = TRUE;
    sCompositeEvery = 1;
    memset(&obj, 0, sizeof(obj));
    obj.valid = TRUE;
    for (i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
        gba[i] = 0x7B9C;
    memset(layers, 2, sizeof(layers)); /* match gFakeNativeWin for a clean compare */

    /* Supported semi OBJ (weather sandstorm/fog/ash/cloud, or any objMode==1
     * sprite): the real sampler reports zero capability flags for it, exactly
     * like this fake, so the frame reaches the compare loop and is NOT
     * obj_unsupported. */
    gFakeObjFlags = 0;
    gFakeRejectCount = 0;
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE) == 0);
    assert(sCompObjUnsupported == 0);
    assert(sCompCompared == 1);
    assert(sObjUnsupportedFrameBlendConfig == 0);
    assert(sObjUnsupportedFrameByReason[NATIVE_OBJ_REJECT_OBJ_WINDOW] == 0);

    /* Unsupported blend config (normal-OBJ alpha blend): accounted as
     * obj_unsupported with the explicit blend-config bin, never compared. */
    gFakeObjFlags = NATIVE_OBJ_CAP_BLEND_CONFIG;
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE) == 0);
    assert(sCompObjUnsupported == 1);
    assert(sObjUnsupportedFrameBlendConfig == 1);
    assert(sCompCompared == 1); /* unchanged: fallback, not compared */

    /* The blend-config bin counts one per flagged frame. */
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE) == 0);
    assert(sCompObjUnsupported == 2);
    assert(sObjUnsupportedFrameBlendConfig == 2);
    assert(sCompCompared == 1);

    gFakeObjFlags = 0;
    gFakeRejectCount = 0;
    fprintf(stderr, "parity module semi-support accounting OK\n");
}

/* Clean + mismatch compare through the full module pipeline, with bounded
 * artifacts under build/native-parity/objcomp/ and the winner histogram in the
 * report. The fake render writes the module's native composite buffers, so the
 * compare, the trace call and the dump are all exercised. */
static void TestCompositeCompareAndDump(void)
{
    struct NativeOverworldSnapshot snap = MakeSnapshot(NATIVE_FALLBACK_NONE);
    struct NativeObjSnapshot obj;
    u16 gba[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u8 layers[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    char oldCwd[1024];
    char tmpDir[] = "/tmp/native-objcomp-test.XXXXXX";
    char path[512];
    FILE *f;
    u32 reportedMismatches = 0;
    int i;

    ResetModule();
    sCompositeEnabled = TRUE;
    sCompositeChecked = TRUE;
    sCompositeEvery = 1;
    sCompositeDumpCap = 1;
    sFrameCounter = 100;
    memset(&obj, 0, sizeof(obj));
    obj.valid = TRUE;
    for (i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
        gba[i] = 0x7B9C; /* a real u16 fill (memset would set each BYTE) */
    memset(layers, 2, sizeof(layers));

    /* Clean frame: the fake native fill matches the gba buffer + winner map. */
    gFakeNativeFill = 0x7B9C;
    gFakeNativeWin = 2;
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE) == 0);
    assert(sCompCompared == 1);
    assert(sCompClean == 1);
    assert(sCompMismatchFrames == 0);
    assert(sCompComparedPixels == DISPLAY_WIDTH * DISPLAY_HEIGHT);
    assert(sCompDumpsWritten == 0); /* clean frames never write artifacts */

    /* Mismatch: the fake native fill differs at every pixel (color AND winner). */
    assert(getcwd(oldCwd, sizeof(oldCwd)) != NULL);
    assert(mkdtemp(tmpDir) != NULL);
    assert(chdir(tmpDir) == 0);
    assert(mkdir("build", 0755) == 0);
    /* The module's WriteCompositeArtifacts only creates the final leaf
     * (build/native-parity/objcomp); provide the parent it expects. */
    assert(mkdir("build/native-parity", 0755) == 0);

    sFrameCounter = 101;
    gFakeNativeFill = 0x0001;
    gFakeNativeWin = 3;
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE)
           == DISPLAY_WIDTH * DISPLAY_HEIGHT);
    assert(sCompMismatchFrames == 1);
    assert(sCompMismatchPixels == DISPLAY_WIDTH * DISPLAY_HEIGHT);
    assert(sCompDumpsWritten == 1);

    snprintf(path, sizeof(path), "build/native-parity/objcomp/report-101.txt");
    f = fopen(path, "r");
    assert(f != NULL);
    while (fgets(path, sizeof(path), f) != NULL)
    {
        if (sscanf(path, "mismatches=%u", &reportedMismatches) == 1)
            break;
    }
    fclose(f);
    assert(reportedMismatches == DISPLAY_WIDTH * DISPLAY_HEIGHT);
    snprintf(path, sizeof(path), "build/native-parity/objcomp/native-101.bin");
    assert(FileExists(path));
    snprintf(path, sizeof(path), "build/native-parity/objcomp/gba-101.bin");
    assert(FileExists(path));
    snprintf(path, sizeof(path), "build/native-parity/objcomp/winner-native-101.bin");
    assert(FileExists(path));
    snprintf(path, sizeof(path), "build/native-parity/objcomp/winner-gba-101.bin");
    assert(FileExists(path));
    snprintf(path, sizeof(path), "build/native-parity/objcomp/bg-snap-101.bin");
    assert(FileExists(path));
    /* The obj-snap serialization is skipped because the fake serializer returns
     * FALSE (the module keeps the write optional). */
    snprintf(path, sizeof(path), "build/native-parity/objcomp/obj-snap-101.bin");
    assert(!FileExists(path));

    /* Dump cap reached: a second mismatching frame writes no new artifacts. */
    sFrameCounter = 102;
    gFakeNativeFill = 0x0002;
    assert(NativeOverworldParity_CompositeFrame(&snap, &obj, gba, layers, TRUE)
           == DISPLAY_WIDTH * DISPLAY_HEIGHT);
    assert(sCompDumpsWritten == 1);
    snprintf(path, sizeof(path), "build/native-parity/objcomp/report-102.txt");
    assert(!FileExists(path));

    assert(chdir(oldCwd) == 0);
    gFakeNativeFill = 0x7B9C;
    gFakeNativeWin = 2;
    fprintf(stderr, "parity module composite compare + bounded dumps OK\n");
}

static void TestCompositeShutdown(void)
{
    ResetModule();
    sCompositeEnabled = TRUE;
    sCompositeChecked = TRUE;
    NativeOverworldParity_Shutdown(); /* prints the composite summary line too */
    sCompositeEnabled = FALSE;
    NativeOverworldParity_Shutdown();
    fprintf(stderr, "parity module composite shutdown OK\n");
}

/* Stage 2.1 closeout: every fallback reason must have a concrete diagnostic
 * label. The runtime run revealed 8 skips reported as "unknown" purely because
 * NATIVE_FALLBACK_BG0_OVERLAY was missing from FallbackName()'s switch; this
 * test pins every reason 0..REASON_COUNT-1 to a name other than "unknown" so a
 * future enum addition cannot silently degrade the skip histogram again. */
static void TestEveryReasonHasLabel(void)
{
    int r;

    for (r = 1; r < NATIVE_FALLBACK_REASON_COUNT; r++)
    {
        const char *name = FallbackName((enum NativeOverworldFallbackReason)r);

        assert(name != NULL);
        assert(strcmp(name, "unknown") != 0);
        assert(strcmp(name, "none") != 0); /* every skip reason names a real state */
    }
    /* BG0-overlay in particular must not regress to "unknown". */
    assert(strcmp(FallbackName(NATIVE_FALLBACK_BG0_OVERLAY), "bg0-overlay") == 0);
    fprintf(stderr, "parity module every reason has a label OK\n");
}

static void TestShutdown(void)
{
    ResetModule();
    NativeOverworldParity_Shutdown();
    sEnabled = FALSE;
    NativeOverworldParity_Shutdown();
    fprintf(stderr, "parity module shutdown OK\n");
}

int main(void)
{
    TestDisabled();
    TestIntervalGate();
    TestScheduledNotSilentlyDropped();
    TestDistinctSkipReasons();
    TestNativeFailed();
    TestOracleMissing();
    TestAborted();
    TestClean();
    TestBit15Masking();
    TestMismatchCountAndLayers();
    TestArtifacts();
    TestScrollFixtureCapture();
    TestScrollDiagnostic();
    TestCompositeDisabled();
    TestCompositeEnvGateAndCounterSharing();
    TestCompositeEarlyReturns();
    TestCompositeObjCapabilityHistogram();
    TestCompositeSemiSupportAccounting();
    TestCompositeCompareAndDump();
    TestCompositeShutdown();
    TestEveryReasonHasLabel();
    TestShutdown();
    fprintf(stderr, "native overworld parity module unit test passed\n");
    return 0;
}

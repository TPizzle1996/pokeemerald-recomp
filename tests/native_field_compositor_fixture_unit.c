/*
 * Stage 3C runtime-fixture offline reproducer.
 *
 * Reproduces the 7 runtime composite-parity mismatch frames OFFLINE from the
 * captured artifacts (written to build/native-parity/objcomp/ at runtime; the
 * permanent copies live in tests/fixtures/native-parity/objcomp/). No game, no
 * DrawFrame oracle, no live VRAM -- pure deserialize + re-render.
 *
 * For each frame N (178, 610, 614, 620, 2753, 2798, 3084):
 *
 *   1. Deserialize the captured BG snapshot (raw struct fread of bg-snap-N.bin)
 *      and the captured OBJ snapshot (NativeObjSnapshot_Deserialize of
 *      obj-snap-N.bin).
 *   2. Re-run the EXACT runtime composite pipeline:
 *        NativeObjRender_Rasterize + NativeOverworldRenderer_DrawCompositeFrame.
 *   3. Consistency: the re-rendered native composite must be bit-exact (color
 *      masked 0x7FFF + per-pixel winner) with the captured native-N.bin /
 *      winner-native-N.bin. A mismatch here means the snapshot does not fully
 *      reproduce the runtime native composite -- a capture-coherence failure.
 *   4. Reproduce the runtime comparison: re-render vs captured gba-N.bin /
 *      winner-gba-N.bin, computing the mismatch count, first-mismatch pixel,
 *      first native/gba color+winner, and the GBA winner histogram -- exactly
 *      as NativeOverworldParity_CompositeFrame does.
 *
 * The expected per-frame runtime numbers are hard-coded below from report-N.txt
 * (this is the "prove the harness reproduces the runtime" step). Pre-fix this
 * asserts the reproduction matches the runtime numbers.
 *
 * Post-fix the SAME binary also handles the transitional-fallback fixtures: a
 * bg-snap-N.bin whose fallbackReason is NATIVE_FALLBACK_BG_TILEMAP_IN_FLIGHT was
 * captured mid-transition (map load / fade / deferred tilemap DMA) and rejected
 * by the capture gate BEFORE any comparison, so the consumer asserts that named
 * fallback instead of reproducing the old mismatch counts. Which path runs is
 * decided entirely by the fixture's own fallbackReason field, so the committed
 * fixtures can be re-captured post-fix without changing this file.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"
#include "native_obj_renderer_shared.h"
#include "../src/platform/native_field_compositor.c"
#include "../src/platform/native_sprite_snapshot.c"

/* config.h (pulled in by global.h via the included .c files above) defines
 * NDEBUG unconditionally, and glibc's <assert.h> redefines assert on EVERY
 * include based on the CURRENT NDEBUG -- so re-include <assert.h> last (NDEBUG
 * cleared) so the parity assertions below are actually enforced. */
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
u16 *gOverworldTilemapBuffer_Bg1;
u16 *gOverworldTilemapBuffer_Bg2;
u16 *gOverworldTilemapBuffer_Bg3;
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

/* ---- buffers ---- */

#define PIXEL_COUNT (DISPLAY_WIDTH * DISPLAY_HEIGHT)

static struct NativeOverworldSnapshot sBgSnap;
static u16 sBgFrame[PIXEL_COUNT];
static u8 sBgWinner[PIXEL_COUNT];
static u16 sBgLayers[4 * PIXEL_COUNT]; /* Stage 3E per-layer BG colors */
static u16 sNative[PIXEL_COUNT];
static u8 sNativeWinner[PIXEL_COUNT];
static u16 sExpNative[PIXEL_COUNT];     /* captured native-N.bin */
static u8 sExpNativeWinner[PIXEL_COUNT];/* captured winner-native-N.bin */
static u16 sGba[PIXEL_COUNT];           /* captured gba-N.bin */
static u8 sGbaWinner[PIXEL_COUNT];      /* captured winner-gba-N.bin */

static const char *HistLabel(u32 b)
{
    static const char *const names[] =
    {
        "backdrop", "bg0", "bg1", "bg2", "bg3",
        "obj0", "obj1", "obj2", "obj3",
    };
    return (b < 9) ? names[b] : "?";
}

/* Read a whole file into a caller buffer (returns bytes read, or -1). */
static long ReadWholeFile(const char *path, void *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    long got;
    if (f == NULL)
        return -1;
    got = (long)fread(buf, 1, cap, f);
    fclose(f);
    return got;
}

/*
 * Expected per-frame runtime numbers, transcribed from the report-N.txt files
 * the runtime wrote (build/native-parity/objcomp/). Pre-fix these are the
 * reproduction targets; post-fix they become "0 mismatches, no first pixel".
 */
struct FixtureExpect
{
    int frame;
    u32 mm;
    u32 firstX;
    u32 firstY;
    u16 firstNative;    /* masked 0x7FFF */
    u16 firstGba;       /* masked 0x7FFF */
    u8 firstNativeWin;
    u8 firstGbaWin;
    u64 gbaHist[9];     /* winner histogram of GBA winner over mismatching px */
};

static const struct FixtureExpect sExpects[] =
{
    { 178,  38400, 0,   0, 0x0000, 0x0000, 3, 0, {38400,0,0,0,0,0,0,0,0} },
    { 610,    107, 123, 73, 0x20c6, 0x316b, 3, 3, {0,0,0,107,0,0,0,0,0} },
    { 614,  38400, 0,   0, 0x0000, 0x0000, 3, 0, {38400,0,0,0,0,0,0,0,0} },
    { 620,    214, 96,  63, 0x6778, 0x354a, 2, 2, {0,0,66,148,0,0,0,0,0} },
    { 2753, 38400, 0,   0, 0x0000, 0x0000, 3, 0, {38400,0,0,0,0,0,0,0,0} },
    { 2798, 38400, 0,   0, 0x0000, 0x0000, 3, 0, {38400,0,0,0,0,0,0,0,0} },
    { 3084, 38400, 0,   0, 0x0000, 0x0000, 3, 0, {38400,0,0,0,0,0,0,0,0} },
};

#define EXPECT_COUNT (sizeof(sExpects) / sizeof(sExpects[0]))

static void PrintHist(const u64 *h)
{
    u32 b;
    for (b = 0; b < 9; b++)
    {
        if (h[b] != 0)
            printf("%s=%llu ", HistLabel(b), (unsigned long long)h[b]);
    }
    printf("\n");
}

/* Load all six artifacts for one frame. Returns 0 on success, -1 on failure. */
static int LoadFixture(const char *dir, int frame)
{
    char path[1024];
    long got;
    u8 *objBuf;
    long objSize;

    snprintf(path, sizeof(path), "%s/bg-snap-%d.bin", dir, frame);
    got = ReadWholeFile(path, &sBgSnap, sizeof(sBgSnap));
    if (got != (long)sizeof(sBgSnap))
    {
        fprintf(stderr, "FAIL: %s read %ld bytes, expected %zu\n",
                path, got, sizeof(sBgSnap));
        return -1;
    }

    snprintf(path, sizeof(path), "%s/obj-snap-%d.bin", dir, frame);
    {
        FILE *f = fopen(path, "rb");
        if (f == NULL)
        {
            fprintf(stderr, "FAIL: missing %s\n", path);
            return -1;
        }
        fseek(f, 0, SEEK_END);
        objSize = ftell(f);
        fseek(f, 0, SEEK_SET);
        objBuf = (u8 *)malloc((size_t)objSize);
        if (objBuf == NULL)
        {
            fclose(f);
            fprintf(stderr, "FAIL: out of memory reading %s\n", path);
            return -1;
        }
        if (fread(objBuf, 1, (size_t)objSize, f) != (size_t)objSize)
        {
            fclose(f);
            free(objBuf);
            fprintf(stderr, "FAIL: short read %s\n", path);
            return -1;
        }
        fclose(f);
    }
    if (!NativeObjSnapshot_Deserialize(objBuf, (size_t)objSize, &sSnap))
    {
        free(objBuf);
        fprintf(stderr, "FAIL: NativeObjSnapshot_Deserialize(%s, %ld bytes)\n",
                path, objSize);
        return -1;
    }
    free(objBuf);

    snprintf(path, sizeof(path), "%s/native-%d.bin", dir, frame);
    got = ReadWholeFile(path, sExpNative, sizeof(sExpNative));
    if (got != (long)sizeof(sExpNative))
    {
        fprintf(stderr, "FAIL: %s read %ld bytes, expected %zu\n",
                path, got, sizeof(sExpNative));
        return -1;
    }
    snprintf(path, sizeof(path), "%s/winner-native-%d.bin", dir, frame);
    got = ReadWholeFile(path, sExpNativeWinner, sizeof(sExpNativeWinner));
    if (got != (long)sizeof(sExpNativeWinner))
    {
        fprintf(stderr, "FAIL: %s read %ld bytes, expected %zu\n",
                path, got, sizeof(sExpNativeWinner));
        return -1;
    }
    snprintf(path, sizeof(path), "%s/gba-%d.bin", dir, frame);
    got = ReadWholeFile(path, sGba, sizeof(sGba));
    if (got != (long)sizeof(sGba))
    {
        fprintf(stderr, "FAIL: %s read %ld bytes, expected %zu\n",
                path, got, sizeof(sGba));
        return -1;
    }
    snprintf(path, sizeof(path), "%s/winner-gba-%d.bin", dir, frame);
    got = ReadWholeFile(path, sGbaWinner, sizeof(sGbaWinner));
    if (got != (long)sizeof(sGbaWinner))
    {
        fprintf(stderr, "FAIL: %s read %ld bytes, expected %zu\n",
                path, got, sizeof(sGbaWinner));
        return -1;
    }
    return 0;
}

static int ReproduceFrame(const char *dir, const struct FixtureExpect *exp)
{
    struct NativeFieldCompositorReport report;
    u32 consistency = 0;
    u32 mm = 0;
    u32 firstIdx = 0;
    u16 firstNative = 0;
    u16 firstGba = 0;
    u8 firstNativeWin = 0;
    u8 firstGbaWin = 0;
    u64 gbaHist[9];
    u64 nativeHist[9];
    u32 i;
    bool32 rasterized;
    bool32 composed;

    if (LoadFixture(dir, exp->frame) != 0)
        return 1;

    if (sBgSnap.fallbackReason == NATIVE_FALLBACK_BG_TILEMAP_IN_FLIGHT)
    {
        /* Post-fix fixture: the frame was captured mid-transition and the capture
         * gate rejected it with the precise named fallback, so the composite is
         * never compared. Assert that gate -- mirror of the parity gate
         * (NativeOverworldParity_CompositeFrame: fallbackReason != NONE -> the
         * frame is accounted as BG_SNAPSHOT_FAILED with this exact reason name). */
        if (sBgSnap.requiredCapabilities != 0)
        {
            fprintf(stderr,
                    "FAIL: frame %d transitional fallback left requiredCapabilities=0x%x "
                    "(SetMapFallback must clear them)\n",
                    exp->frame, sBgSnap.requiredCapabilities);
            return 1;
        }
        printf("frame=%d transitional fallback=bg-tilemap-in-flight "
               "(composite gated, not compared)\n", exp->frame);
        return 0;
    }

    if (sBgSnap.fallbackReason != NATIVE_FALLBACK_NONE)
    {
        fprintf(stderr, "FAIL: frame %d BG snapshot fallbackReason=%d "
                "(expected NONE for a compared fixture, or BG_TILEMAP_IN_FLIGHT "
                "for a transitional fallback fixture)\n",
                exp->frame, (int)sBgSnap.fallbackReason);
        return 1;
    }

    /* Re-run the exact runtime pipeline (side-effecting call outside assert). */
    rasterized = NativeObjRender_Rasterize(&sSnap, &sOut);
    if (!rasterized || !sOut.produced)
    {
        fprintf(stderr, "FAIL: frame %d OBJ rasterize produced=%d\n",
                exp->frame, (int)sOut.produced);
        return 1;
    }
    composed = NativeOverworldRenderer_DrawCompositeFrame(&sBgSnap, &sOut,
                                                          sBgFrame, sBgWinner,
                                                          sBgLayers,
                                                          sNative, sNativeWinner,
                                                          &report);
    if (!composed)
    {
        fprintf(stderr, "FAIL: frame %d DrawCompositeFrame reason=%d\n",
                exp->frame, (int)report.reason);
        return 1;
    }

    /* 1. Deterministic reproduction of the runtime NATIVE composite. */
    for (i = 0; i < PIXEL_COUNT; i++)
    {
        if ((sNative[i] & 0x7FFF) != (sExpNative[i] & 0x7FFF)
         || sNativeWinner[i] != sExpNativeWinner[i])
            consistency++;
    }
    if (consistency != 0)
    {
        fprintf(stderr,
                "FAIL: frame %d native re-render differs from captured native "
                "on %u pixels -- snapshot does not reproduce the runtime "
                "composite (capture-coherence regression?)\n",
                exp->frame, consistency);
        return 1;
    }

    /* 2. Reproduce the runtime comparison against the captured oracle. */
    memset(gbaHist, 0, sizeof(gbaHist));
    memset(nativeHist, 0, sizeof(nativeHist));
    for (i = 0; i < PIXEL_COUNT; i++)
    {
        if ((sNative[i] & 0x7FFF) != (sGba[i] & 0x7FFF)
         || sNativeWinner[i] != sGbaWinner[i])
        {
            if (mm == 0)
            {
                firstIdx = i;
                firstNative = sNative[i];
                firstGba = sGba[i];
                firstNativeWin = sNativeWinner[i];
                firstGbaWin = sGbaWinner[i];
            }
            mm++;
            if (sGbaWinner[i] < 9)
                gbaHist[sGbaWinner[i]]++;
            if (sNativeWinner[i] < 9)
                nativeHist[sNativeWinner[i]]++;
        }
    }

    printf("frame=%d mm=%u first=(%u,%u) nat=0x%04x win=%u gba=0x%04x win=%u\n",
           exp->frame, mm, firstIdx % DISPLAY_WIDTH, firstIdx / DISPLAY_WIDTH,
           firstNative & 0x7FFF, firstNativeWin, firstGba & 0x7FFF, firstGbaWin);
    printf("  native winner hist over mismatches: ");
    PrintHist(nativeHist);
    printf("  gba    winner hist over mismatches: ");
    PrintHist(gbaHist);

    /* 3. The reproduction must equal the runtime numbers (report-N.txt). */
    if (mm != exp->mm)
    {
        fprintf(stderr,
                "FAIL: frame %d reproduced mm=%u, runtime report says %u\n",
                exp->frame, mm, exp->mm);
        return 1;
    }
    if (firstIdx % DISPLAY_WIDTH != exp->firstX || firstIdx / DISPLAY_WIDTH != exp->firstY)
    {
        fprintf(stderr,
                "FAIL: frame %d reproduced first=(%u,%u), runtime report says (%u,%u)\n",
                exp->frame, firstIdx % DISPLAY_WIDTH, firstIdx / DISPLAY_WIDTH,
                exp->firstX, exp->firstY);
        return 1;
    }
    if ((firstNative & 0x7FFF) != exp->firstNative
     || firstNativeWin != exp->firstNativeWin)
    {
        fprintf(stderr,
                "FAIL: frame %d first native=0x%04x win=%u, runtime says "
                "0x%04x win=%u\n",
                exp->frame, firstNative & 0x7FFF, firstNativeWin,
                exp->firstNative, exp->firstNativeWin);
        return 1;
    }
    if ((firstGba & 0x7FFF) != exp->firstGba || firstGbaWin != exp->firstGbaWin)
    {
        fprintf(stderr,
                "FAIL: frame %d first gba=0x%04x win=%u, runtime says "
                "0x%04x win=%u\n",
                exp->frame, firstGba & 0x7FFF, firstGbaWin,
                exp->firstGba, exp->firstGbaWin);
        return 1;
    }
    for (i = 0; i < 9; i++)
    {
        if (gbaHist[i] != exp->gbaHist[i])
        {
            fprintf(stderr,
                    "FAIL: frame %d gba winner histogram[%u]=%llu, runtime "
                    "report says %llu\n",
                    exp->frame, i, (unsigned long long)gbaHist[i],
                    (unsigned long long)exp->gbaHist[i]);
            return 1;
        }
    }

    printf("  reproduced deterministically (matches runtime report)\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "build/native-parity/objcomp";
    u32 i;
    int failures = 0;

    printf("Stage 3C runtime-fixture offline reproducer (dir: %s)\n", dir);
    for (i = 0; i < EXPECT_COUNT; i++)
        failures += ReproduceFrame(dir, &sExpects[i]);

    if (failures != 0)
    {
        fprintf(stderr, "FIXTURE REPRODUCER: %u frame(s) failed\n", failures);
        return 1;
    }
    printf("ALL %zu RUNTIME FIXTURES REPRODUCED (mismatch or named fallback)\n",
           EXPECT_COUNT);
    return 0;
}

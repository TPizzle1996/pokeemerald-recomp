#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"

/*
 * Offline regression for the Stage 2.1 moving-frame (presentation-origin) fix,
 * driven by TWO REAL runtime captures taken during the one manual parity run.
 *
 * The runtime's dev fixture mechanism (POKEEMERALD_NATIVE_PARITY_SCROLL_FIXTURES)
 * serialized the first two bg-scroll-divergent compared frames -- frames where
 * the latched BG scroll lags the logical camera by one frame of motion, i.e. the
 * exact moving-frame state the old HOFS==cameraX gate used to reject as
 * `bg-scroll` fallback. Committed under tests/fixtures/native-parity/scroll/ as
 * scroll-snap-N.bin / scroll-native-N.bin / scroll-gba-N.bin.
 *
 * This harness proves, entirely offline and deterministically:
 *   1. each snapshot represents logical/presentation divergence (uniform scroll
 *      across BG1/2/3 that differs from the logical camera), so it is a genuine
 *      moving-frame fixture;
 *   2. the snapshot is self-contained: re-rendering it with
 *      NativeOverworldRenderer_DrawMapFrame reproduces the runtime's own native
 *      frame bit-for-bit;
 *   3. the re-render matches the captured GBA oracle (DrawFrame output at the
 *      same presentation frame) pixel-for-pixel with 0 mismatches -- the real
 *      runtime produced these frames clean, and the fix must keep them clean.
 *
 * These fixtures permanently protect the Stage 2.1 timing fix from regression.
 */

static const char *const sFixtureFrames[] = { "218", "219" };

// Mirror the bg0-gate harness's stub environment so the renderer TU links
// identically. The runtime snapshots carry bgRingValid/mapGridValid and the ring
// covers the whole 240x160 window, so the re-render never dereferences the
// metatile pointers (which are meaningless after deserialization).
unsigned char PLTT[PLTT_SIZE] __attribute__((aligned(4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));

struct MapHeader gMapHeader;
static struct SaveBlock1 sSaveBlock1;
struct SaveBlock1 *gSaveBlock1Ptr = &sSaveBlock1;
struct ObjectEvent gObjectEvents[OBJECT_EVENTS_COUNT];
struct Sprite gSprites[MAX_SPRITES + 1];
s16 gSpriteCoordOffsetX;
s16 gSpriteCoordOffsetY;

static u16 sPrimaryMetatiles[NUM_METATILES_IN_PRIMARY * NUM_TILES_PER_METATILE];
static u16 sSecondaryMetatiles[NUM_METATILES_IN_PRIMARY * NUM_TILES_PER_METATILE];
static u16 sMetatileId;
static u8 sLayerType;
static bool32 sCoordinateGridMode;
static s16 sCameraX;
static s16 sCameraY;
static bool32 sRendererReady = TRUE;

u32 MapGridGetMetatileIdAt(int x, int y)
{
    (void)x;
    (void)y;
    return sMetatileId;
}

u8 MapGridGetMetatileLayerTypeAt(int x, int y)
{
    (void)x;
    (void)y;
    return sLayerType;
}

bool32 Overworld_IsNativeExpandedRendererReady(void)
{
    return sRendererReady;
}

void GetCameraOffsetWithPan(s16 *x, s16 *y)
{
    *x = sCameraX;
    *y = sCameraY;
}

void UpdateShadowFieldEffect(struct Sprite *sprite)
{
    (void)sprite;
}

void UpdateSurfBlobFieldEffect(struct Sprite *sprite)
{
    (void)sprite;
}

static bool32 LoadFixture(const char *dir, const char *frame, const char *suffix,
                          void *buf, size_t bufSize, size_t *bytesRead)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/scroll-%s-%s.bin", dir, suffix, frame);
    f = fopen(path, "rb");
    if (f == NULL)
    {
        fprintf(stderr, "cannot open fixture %s\n", path);
        return FALSE;
    }
    *bytesRead = fread(buf, 1, bufSize, f);
    fclose(f);
    return TRUE;
}

static void TestScrollFixturesOffline(const char *dir)
{
    size_t f;

    for (f = 0; f < ARRAY_COUNT(sFixtureFrames); f++)
    {
        struct NativeOverworldSnapshot snap;
        static u16 reRendered[DISPLAY_WIDTH * DISPLAY_HEIGHT];
        static u16 capturedNative[DISPLAY_WIDTH * DISPLAY_HEIGHT];
        static u16 capturedGba[DISPLAY_WIDTH * DISPLAY_HEIGHT];
        size_t readSnap;
        size_t readNative;
        size_t readGba;
        u32 mismatches = 0;
        u32 h;
        u32 v;
        u32 j;

        memset(&snap, 0, sizeof(snap));
        if (!LoadFixture(dir, sFixtureFrames[f], "snap", &snap, sizeof(snap), &readSnap))
            exit(1);
        if (readSnap != sizeof(snap))
        {
            fprintf(stderr, "scroll fixture %s: snapshot size mismatch "
                    "(got %zu, want %zu)\n", sFixtureFrames[f], readSnap, sizeof(snap));
            exit(1);
        }
        if (!LoadFixture(dir, sFixtureFrames[f], "native", capturedNative,
                         sizeof(capturedNative), &readNative))
            exit(1);
        if (readNative != sizeof(capturedNative))
            exit(1);
        if (!LoadFixture(dir, sFixtureFrames[f], "gba", capturedGba,
                         sizeof(capturedGba), &readGba))
            exit(1);
        if (readGba != sizeof(capturedGba))
            exit(1);

        // 1. Genuine moving-frame fixture: the presentation scroll differs from
        //    the logical camera, with a uniform latch across all three layers
        //    (a torn latch would have been rejected by the capture gate).
        assert(snap.bgHofs[0] == snap.bgHofs[1] && snap.bgHofs[1] == snap.bgHofs[2]);
        assert(snap.bgVofs[0] == snap.bgVofs[1] && snap.bgVofs[1] == snap.bgVofs[2]);
        assert((snap.bgHofs[0] & 0x1FF) != (u16)(snap.cameraX & 0x1FF)
            || (snap.bgVofs[0] & 0x1FF) != (u16)(snap.cameraY & 0x1FF));

        // The ring and grid must be present so the re-render is fully
        // self-contained (it samples the ring; the grid/border fallback uses the
        // copied map grid -- never the deserialized metatile pointers).
        assert(snap.bgRingValid);
        assert(snap.mapGridValid);

        // 2 + 3. Re-render from the snapshot alone and compare both ways.
        assert(NativeOverworldSnapshotSupportsMap(&snap));
        assert(NativeOverworldRenderer_DrawMapFrame(&snap, reRendered));
        for (j = 0; j < DISPLAY_WIDTH * DISPLAY_HEIGHT; j++)
        {
            // Reproduces the runtime's own native frame bit-for-bit.
            assert(reRendered[j] == capturedNative[j]);
            // Matches the captured GBA oracle pixel-for-pixel (bit 15 is the
            // alpha flag, excluded exactly as the runtime parity compares).
            if ((reRendered[j] & 0x7FFF) != (capturedGba[j] & 0x7FFF))
                mismatches++;
        }
        assert(mismatches == 0);

        fprintf(stderr, "scroll fixture %s: cam=(%ld,%ld)+(%d,%d) scroll=(0x%04x,0x%04x) "
                "reproduced bit-for-bit, mismatches=%u\n",
                sFixtureFrames[f],
                (long)snap.cameraMapX, (long)snap.cameraMapY,
                snap.cameraX, snap.cameraY,
                snap.bgHofs[0] & 0x1FF, snap.bgVofs[0] & 0x1FF,
                mismatches);
    }
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "tests/fixtures/native-parity/scroll";

    TestScrollFixturesOffline(dir);
    printf("native overworld scroll fixture offline repro unit test passed\n");
    return 0;
}

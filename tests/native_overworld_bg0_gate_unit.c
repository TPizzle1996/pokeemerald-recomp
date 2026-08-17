#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"

/*
 * Regression + offline-repro harness for the BG0-content gate.
 *
 * The final manual Stage-2 parity run found 20 mismatch frames, all of them
 * frames where BG0 rendered visible field UI/window content (field message box,
 * text windows) that the native map-background renderer does not reproduce but
 * the GBA oracle does. Every mismatch pixel was on oracle layer BG0.
 *
 * This harness does three things entirely offline (no game run):
 *   1. Re-renders the native map frame from each serialized runtime snapshot and
 *      asserts it matches the native frame the runtime actually produced --
 *      proving the snapshot is a faithful, self-contained input.
 *   2. Re-renders against the captured GBA oracle frame and asserts the exact
 *      pre-fix mismatch counts / first-mismatch coordinates from the runtime
 *      reports reproduce -- proving the failure is deterministic from the
 *      snapshot alone.
 *   3. Runs the fixed gate (NativeOverworld_SnapshotHasBg0Content) on each
 *      snapshot with the field's live BG0CNT and asserts it rejects the frame,
 *      plus synthetic clean/opaque fixtures to prove the gate does NOT over-reject
 *      transparent cleared-window filler (0xE000 entries referencing tile 0).
 *
 * Fixtures live in tests/fixtures/native-parity/ (frame numbers 1131..1138,
 * one per dump the runtime wrote). The runtime serialized snapshots before the
 * bg0Cnt field existed, so the field's BG0CNT value is supplied here exactly as
 * the live capture would: 0x1F08 == mapBase 31 | charBase 2 | 16-color.
 */

// BG0CNT captured from REG_BG0CNT during the manual run: the field's BG0 layer.
#define FIELD_BG0CNT 0x1F08u

static const struct
{
    const char *name;
    u32 mismatches;   // from report-N.txt: native-vs-oracle mismatching pixels
    u32 firstX;
    u32 firstY;
} sRuntimeFrames[] =
{
    { "1131", 9948u,  6u, 115u },
    { "1132", 12428u, 162u, 65u },
    { "1133", 12428u, 162u, 65u },
    { "1134", 12428u, 162u, 65u },
    { "1135", 12428u, 162u, 65u },
    { "1136", 12428u, 162u, 65u },
    { "1137", 12428u, 162u, 65u },
    { "1138", 12428u, 162u, 65u },
};

// Mirror the renderer unit test's stub environment so the TU links identically.
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

    snprintf(path, sizeof(path), "%s/%s-%s.bin", dir, suffix, frame);
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

static void TestRuntimeFixturesOffline(const char *dir)
{
    size_t i;

    for (i = 0; i < ARRAY_COUNT(sRuntimeFrames); i++)
    {
        struct NativeOverworldSnapshot snap;
        static u16 nativeRendered[DISPLAY_WIDTH * DISPLAY_HEIGHT];
        static u16 capturedNative[DISPLAY_WIDTH * DISPLAY_HEIGHT];
        static u16 capturedGba[DISPLAY_WIDTH * DISPLAY_HEIGHT];
        size_t readSnap;
        size_t readNative;
        size_t readGba;
        u32 mismatches = 0;
        u32 firstX = 0;
        u32 firstY = 0;
        u32 j;

        memset(&snap, 0, sizeof(snap));
        if (!LoadFixture(dir, sRuntimeFrames[i].name, "snap", &snap, sizeof(snap), &readSnap))
            exit(1);
        // The runtime serialized snapshots before the bg0Cnt field existed, so
        // accept any read that covers every field up to (but not including) the
        // appended bg0Cnt; the tail is supplied below.
        if (readSnap < offsetof(struct NativeOverworldSnapshot, bg0Cnt))
            exit(1);
        if (!LoadFixture(dir, sRuntimeFrames[i].name, "native", capturedNative,
                         sizeof(capturedNative), &readNative))
            exit(1);
        if (readNative != sizeof(capturedNative))
            exit(1);
        if (!LoadFixture(dir, sRuntimeFrames[i].name, "gba", capturedGba,
                         sizeof(capturedGba), &readGba))
            exit(1);
        if (readGba != sizeof(capturedGba))
            exit(1);

        // The snapshots predate the bg0Cnt field; supply the captured live value.
        snap.bg0Cnt = FIELD_BG0CNT;

        // 1. The snapshot is self-contained: re-rendering it must reproduce the
        //    exact native frame the runtime produced.
        assert(NativeOverworldSnapshotSupportsMap(&snap));
        assert(NativeOverworldRenderer_DrawMapFrame(&snap, nativeRendered));
        for (j = 0; j < ARRAY_COUNT(nativeRendered); j++)
            assert(nativeRendered[j] == capturedNative[j]);

        // 2. Native-vs-oracle reproduces the runtime mismatch count and the
        //    first-mismatch coordinate from the report (0x7FFF compare, as the
        //    parity harness does).
        for (j = 0; j < ARRAY_COUNT(nativeRendered); j++)
        {
            if ((nativeRendered[j] & 0x7FFF) != (capturedGba[j] & 0x7FFF))
            {
                if (mismatches == 0)
                {
                    firstX = j % DISPLAY_WIDTH;
                    firstY = j / DISPLAY_WIDTH;
                }
                mismatches++;
            }
        }
        assert(mismatches == sRuntimeFrames[i].mismatches);
        assert(firstX == sRuntimeFrames[i].firstX);
        assert(firstY == sRuntimeFrames[i].firstY);

        // 3. The fixed gate rejects this frame: BG0 has visible content.
        assert(NativeOverworld_SnapshotHasBg0Content(&snap));
    }
}

static void TestTransparentFillerDoesNotTrigger(void)
{
    struct NativeOverworldSnapshot snap;
    u32 r;
    u32 c;

    // A field window that has been cleared leaves 0xE000 tilemap entries
    // (tile 0, palette 14) everywhere. Tile 0 is transparent, so this must NOT
    // count as BG0 content.
    memset(&snap, 0, sizeof(snap));
    snap.bgVramValid = TRUE;
    snap.bg0Cnt = FIELD_BG0CNT;
    for (r = 0; r < 32; r++)
        for (c = 0; c < 32; c++)
            snap.bgVram[31 * 0x400 + r * 32 + c] = 0xE000;
    assert(!NativeOverworld_SnapshotHasBg0Content(&snap));
}

static void TestOpaqueContentTriggers(void)
{
    struct NativeOverworldSnapshot snap;
    u8 *tile;

    memset(&snap, 0, sizeof(snap));
    snap.bgVramValid = TRUE;
    snap.bg0Cnt = FIELD_BG0CNT; // charBase 2 -> byte offset 0x8000
    // Entry 0x0001 at screen entry (0,0) of block 31: tile 1, palette 0.
    snap.bgVram[31 * 0x400] = 0x0001;
    tile = (u8 *)snap.bgVram + 0x8000 + 1 * 32;
    tile[4] = 0x0F; // opaque pixel (nibble 0xF) in tile 1
    assert(NativeOverworld_SnapshotHasBg0Content(&snap));

    // 8bpp variant: BG0CNT bit 7 -> 64 bytes per tile, and an opaque byte past
    // the 4bpp tile boundary proves the tile width is honored.
    memset(&snap, 0, sizeof(snap));
    snap.bgVramValid = TRUE;
    snap.bg0Cnt = FIELD_BG0CNT | (1 << 7);
    snap.bgVram[31 * 0x400] = 0x0001;
    tile = (u8 *)snap.bgVram + 0x8000 + 1 * 64;
    tile[40] = 0x01; // byte 40 is inside an 8bpp tile but outside a 4bpp tile
    assert(NativeOverworld_SnapshotHasBg0Content(&snap));

    // An out-of-range tile reference (charBase 3, 8bpp, tile 0x3FF) overflows
    // captured VRAM: cannot prove empty -> must fall back.
    memset(&snap, 0, sizeof(snap));
    snap.bgVramValid = TRUE;
    snap.bg0Cnt = (3 << 2) | (1 << 7) | (31 << 8); // charBase 3, 8bpp, mapBase 31
    snap.bgVram[31 * 0x400] = 0x3FF;
    assert(NativeOverworld_SnapshotHasBg0Content(&snap));
}

static void TestEmptyAndMissingVram(void)
{
    struct NativeOverworldSnapshot snap;

    memset(&snap, 0, sizeof(snap));
    snap.bgVramValid = TRUE;
    snap.bg0Cnt = FIELD_BG0CNT;
    assert(!NativeOverworld_SnapshotHasBg0Content(&snap));

    // No captured VRAM: cannot prove BG0 empty.
    memset(&snap, 0, sizeof(snap));
    assert(NativeOverworld_SnapshotHasBg0Content(&snap));
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "tests/fixtures/native-parity";

    TestRuntimeFixturesOffline(dir);
    TestTransparentFillerDoesNotTrigger();
    TestOpaqueContentTriggers();
    TestEmptyAndMissingVram();
    printf("native overworld BG0 gate + offline repro unit test passed\n");
    return 0;
}

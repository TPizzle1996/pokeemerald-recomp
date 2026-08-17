/*
 * Stage 4A Issue B stable-canvas fallback tests A-F (directive 13).
 *
 * Pins NativeOverworldRenderer_BuildFallbackCanvas, the presentation helper
 * behind the "STOP VISUAL ZOOMING DURING FALLBACK" fix:
 *
 *   A. the output is exactly viewport-sized and FULLY written (no sentinel
 *      rows left over -- the canvas never carries stale pixels), for the
 *      300x200 test viewport, a non-preset 245x163 viewport, and a REAL
 *      derived NativeViewport (1.25x -> 300x200), the integration seam the
 *      presentation path passes in,
 *   B. the AUTHORITATIVE 240x160 core frame is centered 1:1 and bit-exact in
 *      the 300x200 canvas (margins (30,20)),
 *   C. every margin pixel is PURE black 0xFF000000 on all four sides and the
 *      corners, while the core is genuine non-black content (not a wipe),
 *   D. the helper is deterministic with no hidden state (two identical calls
 *      produce identical canvases) and never writes back into coreFrame,
 *   E. the SELECTED viewport is never mutated (bit-identical after the call),
 *      so a capability fallback cannot change the user's chosen zoom/scale,
 *   F. non-preset / odd viewports (245x163 -> margins (2,1)) center the core
 *      deterministically; a 240x160 viewport collapses to the bare core (no
 *      margins); and a viewport narrower than the core is rejected (FALSE)
 *      rather than producing an invalid canvas.
 *
 * Self-contained: links only native_overworld_viewport.c +
 * native_overworld_renderer.c (gc-sections keeps only the leaf helper).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/platform/native_overworld_viewport.c"
#include "../src/platform/native_overworld_renderer.c"

#undef NDEBUG
#include <assert.h>

#define CORE_W     DISPLAY_WIDTH      /* 240 */
#define CORE_H     DISPLAY_HEIGHT     /* 160 */
#define ARGB_BLACK 0xFF000000u

static u32 sCore[CORE_W * CORE_H];
static u32 sCanvas[NATIVE_VIEWPORT_MAX_WIDTH * NATIVE_VIEWPORT_MAX_HEIGHT];

/* Deterministic, non-black ARGB pattern for the authoritative core frame.
 * R/G/B are all >= 1, so no core pixel is ever pure black (the margins'
 * discriminator in test C). */
static void BuildCore(void)
{
    int i;
    for (i = 0; i < CORE_W * CORE_H; i++)
        sCore[i] = 0xFF000000u
                 | ((u32)((i % 7) + 1) << 16)
                 | ((u32)((i % 5) + 1) << 8)
                 | ((u32)((i % 3) + 1));
}

/* Direct-set a viewport (the helper only reads width/height; scale/center are
 * exercised as untouched state in test E). */
static void SetViewport(struct NativeViewport *viewport, int width, int height)
{
    memset(viewport, 0, sizeof(*viewport));
    viewport->width = (u16)width;
    viewport->height = (u16)height;
    viewport->scaleQ8 = NATIVE_VIEWPORT_SCALE_1_0;
}

static void TestAViewportSizedOutput(void)
{
    struct NativeViewport viewport;
    struct NativeViewport derived;
    int i;

    /* Real derived viewport: 1.25x -> 300x200 (the integration seam). */
    NativeOverworldViewport_Init(&derived);
    derived.scaleQ8 = 320;
    NativeOverworldViewport_Recompute(&derived);
    assert(derived.width == 300 && derived.height == 200);

    /* Every pixel of the 300x200 canvas is written (no sentinel rows left). */
    SetViewport(&viewport, 300, 200);
    memset(sCanvas, 0xDE, sizeof(sCanvas));
    assert(NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &viewport, sCanvas));
    for (i = 0; i < 300 * 200; i++)
        assert(sCanvas[i] != 0xDEDEDEDEu);

    /* Same for a non-preset 245x163 viewport. */
    SetViewport(&viewport, 245, 163);
    memset(sCanvas, 0xDE, sizeof(sCanvas));
    assert(NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &viewport, sCanvas));
    for (i = 0; i < 245 * 163; i++)
        assert(sCanvas[i] != 0xDEDEDEDEu);

    /* The helper accepts the real derived viewport struct unchanged. */
    memset(sCanvas, 0xDE, sizeof(sCanvas));
    assert(NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &derived, sCanvas));
    for (i = 0; i < 300 * 200; i++)
        assert(sCanvas[i] != 0xDEDEDEDEu);

    printf("A: viewport-sized canvas fully written (300x200, 245x163, derived) ok\n");
}

static void TestBCoreCenteredBitExact(void)
{
    struct NativeViewport viewport;
    int y;
    int x;

    SetViewport(&viewport, 300, 200);   /* margins (30, 20) */
    assert(NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &viewport, sCanvas));

    for (y = 0; y < CORE_H; y++)
        for (x = 0; x < CORE_W; x++)
            assert(sCanvas[(y + 20) * 300 + (x + 30)] == sCore[y * CORE_W + x]);
    printf("B: 240x160 core centered 1:1 and bit-exact in the 300x200 canvas ok\n");
}

static void TestCMarginsPureBlack(void)
{
    struct NativeViewport viewport;
    int y;
    int x;

    SetViewport(&viewport, 300, 200);
    assert(NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &viewport, sCanvas));

    /* Top margin: y in [0,20), every column. */
    for (y = 0; y < 20; y++)
        for (x = 0; x < 300; x++)
            assert(sCanvas[y * 300 + x] == ARGB_BLACK);
    /* Bottom margin: y in [180,200). */
    for (y = 180; y < 200; y++)
        for (x = 0; x < 300; x++)
            assert(sCanvas[y * 300 + x] == ARGB_BLACK);
    /* Left margin: x in [0,30), the core band rows. */
    for (y = 20; y < 180; y++)
        for (x = 0; x < 30; x++)
            assert(sCanvas[y * 300 + x] == ARGB_BLACK);
    /* Right margin: x in [270,300), the core band rows. */
    for (y = 20; y < 180; y++)
        for (x = 270; x < 300; x++)
            assert(sCanvas[y * 300 + x] == ARGB_BLACK);
    /* The four corners. */
    assert(sCanvas[0] == ARGB_BLACK);
    assert(sCanvas[299] == ARGB_BLACK);
    assert(sCanvas[199 * 300 + 0] == ARGB_BLACK);
    assert(sCanvas[199 * 300 + 299] == ARGB_BLACK);
    /* The core is genuine non-black content (not a wiped canvas). */
    assert(sCanvas[20 * 300 + 30] == sCore[0]);
    assert(sCanvas[20 * 300 + 30] != ARGB_BLACK);
    printf("C: margins pure black (4 sides + corners), core non-black content ok\n");
}

static void TestDDeterministicNoHiddenState(void)
{
    struct NativeViewport viewport;
    u32 coreCopy[CORE_W * CORE_H];
    u32 first[300 * 200];
    int i;

    SetViewport(&viewport, 300, 200);

    /* coreFrame is never written back into (no aliasing write). */
    memcpy(coreCopy, sCore, sizeof(coreCopy));
    memset(sCanvas, 0xDE, sizeof(sCanvas));
    assert(NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &viewport, sCanvas));
    assert(memcmp(coreCopy, sCore, sizeof(coreCopy)) == 0);

    /* Identical inputs -> identical canvas (no hidden per-call state). */
    memcpy(first, sCanvas, sizeof(first));
    memset(sCanvas, 0xDE, sizeof(sCanvas));
    assert(NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &viewport, sCanvas));
    assert(memcmp(first, sCanvas, sizeof(first)) == 0);
    printf("D: deterministic (no hidden state); coreFrame never written back ok\n");
}

static void TestEViewportNotMutated(void)
{
    struct NativeViewport viewport;
    struct NativeViewport snapshot;
    int i;

    SetViewport(&viewport, 300, 200);
    viewport.scaleQ8 = 320;
    viewport.centerX = 12345;
    viewport.centerY = -6789;
    snapshot = viewport;

    assert(NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &viewport, sCanvas));
    assert(memcmp(&viewport, &snapshot, sizeof(viewport)) == 0);

    /* A spread of distinct viewports is never mutated by the build. */
    for (i = 0; i < 8; i++)
    {
        struct NativeViewport vp;
        struct NativeViewport before;

        SetViewport(&vp, 240 + i, 160 + i);
        vp.scaleQ8 = (s16)(NATIVE_VIEWPORT_SCALE_1_0 + i);
        vp.centerX = i;
        vp.centerY = -i;
        before = vp;
        assert(NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &vp, sCanvas));
        assert(memcmp(&vp, &before, sizeof(vp)) == 0);
    }
    printf("E: selected viewport never mutated by a fallback canvas build ok\n");
}

static void TestFNonPresetAndDegenerate(void)
{
    struct NativeViewport viewport;
    int y;
    int x;
    int i;

    /* 245x163 -> margins (2,1): core at (2,1), deterministic odd rounding. */
    SetViewport(&viewport, 245, 163);
    assert(NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &viewport, sCanvas));
    for (y = 0; y < 163; y++)
        for (x = 0; x < 245; x++)
        {
            u32 got = sCanvas[y * 245 + x];
            if (y >= 1 && y < 1 + CORE_H && x >= 2 && x < 2 + CORE_W)
                assert(got == sCore[(y - 1) * CORE_W + (x - 2)]);
            else
                assert(got == ARGB_BLACK);
        }

    /* 240x160 (1.0x, no margins): the canvas IS the bare authoritative core. */
    SetViewport(&viewport, 240, 160);
    assert(NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &viewport, sCanvas));
    for (i = 0; i < 240 * 160; i++)
        assert(sCanvas[i] == sCore[i]);

    /* A viewport narrower than the core is rejected (negative margin), never
     * producing an invalid canvas. */
    SetViewport(&viewport, 238, 160);
    assert(!NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &viewport, sCanvas));

    /* NULL / degenerate inputs are rejected. */
    SetViewport(&viewport, 300, 200);
    assert(!NativeOverworldRenderer_BuildFallbackCanvas(NULL, CORE_W, CORE_H, &viewport, sCanvas));
    assert(!NativeOverworldRenderer_BuildFallbackCanvas(sCore, 0, CORE_H, &viewport, sCanvas));
    assert(!NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, NULL, sCanvas));
    assert(!NativeOverworldRenderer_BuildFallbackCanvas(sCore, CORE_W, CORE_H, &viewport, NULL));
    printf("F: non-preset 245x163 (2,1) + bare 240x160 + degenerate rejected ok\n");
}

int main(void)
{
    BuildCore();
    TestAViewportSizedOutput();
    TestBCoreCenteredBitExact();
    TestCMarginsPureBlack();
    TestDDeterministicNoHiddenState();
    TestEViewportNotMutated();
    TestFNonPresetAndDegenerate();
    printf("native overworld fallback canvas unit test passed\n");
    return 0;
}

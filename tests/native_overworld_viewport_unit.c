/*
 * Stage 4A continuous NativeViewport module test.
 *
 * Pins the continuous (non-enum) viewport contract:
 *   - the default viewport is exactly the base 240x160 view (1.0x, not Active),
 *   - arbitrary view scales derive arbitrary dimensions from scaleQ8
 *     (1.25x -> 300x200, 1.5x -> 360x240, and non-preset scales like 1.02x ->
 *     245x163 and 1.13x -> 271x181), with no preset zoom enums anywhere,
 *   - the POKEEMERALD_NATIVE_VIEWPORT=WxH env var (the Stage 4A test driver)
 *     parses and clamps [240,360]x[160,240]; invalid requests leave 240x160,
 *   - Ctrl+-/Ctrl+= step the SAME scaleQ8 by the continuous step, clamped at
 *     1.0x and 1.5x, and Ctrl+0 resets to exactly 240x160,
 *   - Active() is TRUE only when strictly wider/taller than 240x160,
 *   - TopLeft() centers on the 240x160 focus with deterministic integer
 *     truncation (300x200 -> margins (30,20); odd dimensions round down).
 *
 * Self-contained: links only native_overworld_viewport.c.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../src/platform/native_overworld_viewport.c"

#undef NDEBUG
#include <assert.h>

static void TestDefaultInactive(void)
{
    struct NativeViewport viewport;

    /* No env override: starts at exactly 1.0x / 240x160 and is not Active. */
    unsetenv("POKEEMERALD_NATIVE_VIEWPORT");
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);
    assert(viewport.width == NATIVE_VIEWPORT_MIN_WIDTH);
    assert(viewport.height == NATIVE_VIEWPORT_MIN_HEIGHT);
    assert(!NativeOverworldViewport_Active(&viewport));
}

static void TestScaleDerivation(void)
{
    struct NativeViewport viewport;

    NativeOverworldViewport_Init(&viewport);

    /* 1.25x -> 300x200 (the Stage 4A test viewport). */
    viewport.scaleQ8 = 320;
    NativeOverworldViewport_Recompute(&viewport);
    assert(viewport.width == 300 && viewport.height == 200);
    assert(NativeOverworldViewport_Active(&viewport));

    /* 1.5x -> 360x240, the maximum-capacity native buffers allow. */
    viewport.scaleQ8 = NATIVE_VIEWPORT_SCALE_MAX;
    NativeOverworldViewport_Recompute(&viewport);
    assert(viewport.width == NATIVE_VIEWPORT_MAX_WIDTH);
    assert(viewport.height == NATIVE_VIEWPORT_MAX_HEIGHT);
    assert(NativeOverworldViewport_Active(&viewport));

    /* Non-preset continuous scales (no enums): 1.02x -> 245x163 and
     * 1.13x -> 271x181 must both be expressible (Section 23 accepts multiple
     * non-preset dimensions). */
    viewport.scaleQ8 = 261; /* ~1.0195x */
    NativeOverworldViewport_Recompute(&viewport);
    assert(viewport.width == 245 && viewport.height == 163);
    assert(NativeOverworldViewport_Active(&viewport));

    viewport.scaleQ8 = 289; /* ~1.1289x */
    NativeOverworldViewport_Recompute(&viewport);
    assert(viewport.width == 271 && viewport.height == 181);
    assert(NativeOverworldViewport_Active(&viewport));

    /* Recompute clamps out-of-range scales back into [1.0x, 1.5x]. */
    viewport.scaleQ8 = 128; /* 0.5x */
    NativeOverworldViewport_Recompute(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);
    assert(viewport.width == NATIVE_VIEWPORT_MIN_WIDTH);
    assert(viewport.height == NATIVE_VIEWPORT_MIN_HEIGHT);

    viewport.scaleQ8 = 640; /* 2.5x */
    NativeOverworldViewport_Recompute(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_MAX);
    assert(viewport.width == NATIVE_VIEWPORT_MAX_WIDTH);
    assert(viewport.height == NATIVE_VIEWPORT_MAX_HEIGHT);
}

static void TestEnvParse(void)
{
    struct NativeViewport viewport;

    /* "300x200" (the Stage 4A test driver) -> 1.25x -> exact 300x200. */
    setenv("POKEEMERALD_NATIVE_VIEWPORT", "300x200", 1);
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == 320);
    assert(viewport.width == 300 && viewport.height == 200);

    /* "245x163" / "271x181": non-preset dimensions parse and round-trip. */
    setenv("POKEEMERALD_NATIVE_VIEWPORT", "245x163", 1);
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == 261);
    assert(viewport.width == 245 && viewport.height == 163);

    setenv("POKEEMERALD_NATIVE_VIEWPORT", "271x181", 1);
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.width == 271 && viewport.height == 181);

    /* Max capacity. */
    setenv("POKEEMERALD_NATIVE_VIEWPORT", "360x240", 1);
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_MAX);
    assert(viewport.width == 360 && viewport.height == 240);

    /* Below-min width -> reject -> 240x160. */
    setenv("POKEEMERALD_NATIVE_VIEWPORT", "200x200", 1);
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);
    assert(viewport.width == 240 && viewport.height == 160);

    /* Above-max width -> reject -> 240x160. */
    setenv("POKEEMERALD_NATIVE_VIEWPORT", "400x200", 1);
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);
    assert(viewport.width == 240 && viewport.height == 160);

    /* Above-max height -> reject -> 240x160. */
    setenv("POKEEMERALD_NATIVE_VIEWPORT", "240x320", 1);
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);
    assert(viewport.width == 240 && viewport.height == 160);

    /* Malformed / empty -> reject -> 240x160. */
    setenv("POKEEMERALD_NATIVE_VIEWPORT", "abc", 1);
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);

    setenv("POKEEMERALD_NATIVE_VIEWPORT", "", 1);
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);

    setenv("POKEEMERALD_NATIVE_VIEWPORT", "300 200", 1);
    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);

    unsetenv("POKEEMERALD_NATIVE_VIEWPORT");
}

static void TestContinuousZoomControls(void)
{
    struct NativeViewport viewport;

    NativeOverworldViewport_Init(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);

    /* Ctrl+- (ZoomOut) increases the scale by the continuous step. */
    assert(NativeOverworldViewport_ZoomOut(&viewport));
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0 + NATIVE_VIEWPORT_SCALE_STEP);
    assert(viewport.width == 252 && viewport.height == 168);

    /* Ctrl+= (ZoomIn) decreases it back. */
    assert(NativeOverworldViewport_ZoomIn(&viewport));
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);
    assert(viewport.width == NATIVE_VIEWPORT_MIN_WIDTH);
    assert(viewport.height == NATIVE_VIEWPORT_MIN_HEIGHT);
    assert(!NativeOverworldViewport_Active(&viewport));

    /* ZoomIn at 1.0x is a no-op (already at the lower bound). */
    assert(!NativeOverworldViewport_ZoomIn(&viewport));

    /* ZoomOut at 1.5x is a no-op (already at the upper bound). */
    viewport.scaleQ8 = NATIVE_VIEWPORT_SCALE_MAX;
    NativeOverworldViewport_Recompute(&viewport);
    assert(!NativeOverworldViewport_ZoomOut(&viewport));
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_MAX);
    assert(viewport.width == NATIVE_VIEWPORT_MAX_WIDTH);
    assert(viewport.height == NATIVE_VIEWPORT_MAX_HEIGHT);

    /* Ctrl+0 (Reset) returns to exactly 1.0x / 240x160. */
    NativeOverworldViewport_Reset(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);
    assert(viewport.width == NATIVE_VIEWPORT_MIN_WIDTH);
    assert(viewport.height == NATIVE_VIEWPORT_MIN_HEIGHT);
    assert(!NativeOverworldViewport_Active(&viewport));

    /* Reset at 1.0x is a no-op. */
    NativeOverworldViewport_Reset(&viewport);
    assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0);

    /* Every continuous step is reachable from 1.0x to 1.5x (no preset enums):
     * the scale walks 256 -> 269 -> ... -> 373 -> 384 (clamped), so exactly 10
     * successful ZoomOuts land on the 1.5x bound and the 11th is a no-op. */
    {
        u8 steps = 0;
        while (NativeOverworldViewport_ZoomOut(&viewport))
        {
            steps++;
            assert(viewport.scaleQ8 <= NATIVE_VIEWPORT_SCALE_MAX);
        }
        assert(steps == 10);
        assert(viewport.scaleQ8 == NATIVE_VIEWPORT_SCALE_MAX);
    }
}

static void TestTopLeftCentering(void)
{
    struct NativeViewport viewport;
    s32 left;
    s32 top;

    /* 300x200 centered on the 240x160 focus (origin (160,320)): the margins are
     * (30,20) -- center - (width/2, height/2) with integer truncation. */
    NativeOverworldViewport_Init(&viewport);
    viewport.scaleQ8 = 320;
    NativeOverworldViewport_Recompute(&viewport);
    NativeOverworldViewport_TopLeft(&viewport, 160, 320, &left, &top);
    assert(viewport.centerX == 160 + DISPLAY_WIDTH / 2);
    assert(viewport.centerY == 320 + DISPLAY_HEIGHT / 2);
    assert(left == 160 + DISPLAY_WIDTH / 2 - 150);
    assert(top == 320 + DISPLAY_HEIGHT / 2 - 100);

    /* Odd-dimension rounding: 245x163 -> center - (122, 81) (122 = 245/2, not
     * 122.5) so the top-left is deterministic (truncation, not banker's). */
    viewport.scaleQ8 = 261;
    NativeOverworldViewport_Recompute(&viewport);
    assert(viewport.width == 245 && viewport.height == 163);
    NativeOverworldViewport_TopLeft(&viewport, 0, 0, &left, &top);
    assert(viewport.centerX == DISPLAY_WIDTH / 2);
    assert(viewport.centerY == DISPLAY_HEIGHT / 2);
    assert(left == DISPLAY_WIDTH / 2 - 122);
    assert(top == DISPLAY_HEIGHT / 2 - 81);

    /* 240x160 collapses onto the normal origin (no margins). */
    NativeOverworldViewport_Reset(&viewport);
    NativeOverworldViewport_TopLeft(&viewport, 100, 200, &left, &top);
    assert(left == 100 && top == 200);
    assert(viewport.centerX == 100 + DISPLAY_WIDTH / 2);
    assert(viewport.centerY == 200 + DISPLAY_HEIGHT / 2);
}

int main(void)
{
    TestDefaultInactive();
    TestScaleDerivation();
    TestEnvParse();
    TestContinuousZoomControls();
    TestTopLeftCentering();
    printf("native overworld viewport unit test passed\n");
    return 0;
}

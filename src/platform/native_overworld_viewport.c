#if defined(PLATFORM_SDL2) && defined(LINUX64) && LINUX64

#include <stdio.h>
#include <stdlib.h>

#include "global.h"
#include "platform/native_overworld_viewport.h"

/*
 * Stage 4A continuous viewport state. Replaces the Stage 1 enum zoom. There are
 * no preset zoom levels: scaleQ8 is a plain 8.8 fixed-point view scale and every
 * dimension is derived from it. The zoom controls manipulate this SAME scaleQ8
 * that future touchpad pinch will use (directive 16/17), so the keyboard and
 * pinch cannot drift apart.
 *
 * The POKEEMERALD_NATIVE_VIEWPORT env var (e.g. "300x200") is the Stage 4A test
 * driver; it sets the initial scale only. Width is authoritative: scaleQ8 is
 * derived from the requested width and height follows, so "300x200" round-trips
 * to exactly 300x200. Requests inside [240,360]x[160,240] are accepted; anything
 * else (or an unparsable value) leaves the viewport at exactly 1.0x / 240x160.
 */

static void ReportViewportChange(const struct NativeViewport *viewport)
{
    fprintf(stderr, "native viewport -> %ux%u (%.2fx)\n",
            viewport->width, viewport->height,
            (double)viewport->scaleQ8 / (double)NATIVE_VIEWPORT_SCALE_1_0);
    fflush(stderr);
}

/* Manual "WxH" parser (avoids sscanf). Returns TRUE and fills *w/*h on a valid
 * expanded request; FALSE otherwise. */
static bool32 ParseViewportRequest(const char *str, u16 *w, u16 *h)
{
    u32 width = 0;
    u32 height = 0;
    u32 digit;

    if (str == NULL || *str == '\0' || *str < '0' || *str > '9')
        return FALSE;
    while (*str >= '0' && *str <= '9')
    {
        digit = (u32)(*str - '0');
        if (width > (NATIVE_VIEWPORT_MAX_WIDTH - digit) / 10u)
            return FALSE; // would overflow the max-width bound anyway
        width = width * 10u + digit;
        str++;
    }
    if (*str != 'x' && *str != 'X')
        return FALSE;
    str++;
    if (*str < '0' || *str > '9')
        return FALSE;
    while (*str >= '0' && *str <= '9')
    {
        digit = (u32)(*str - '0');
        if (height > (NATIVE_VIEWPORT_MAX_HEIGHT - digit) / 10u)
            return FALSE;
        height = height * 10u + digit;
        str++;
    }
    if (*str != '\0')
        return FALSE;
    if (width < NATIVE_VIEWPORT_MIN_WIDTH || width > NATIVE_VIEWPORT_MAX_WIDTH)
        return FALSE;
    if (height < NATIVE_VIEWPORT_MIN_HEIGHT || height > NATIVE_VIEWPORT_MAX_HEIGHT)
        return FALSE;
    *w = (u16)width;
    *h = (u16)height;
    return TRUE;
}

void NativeOverworldViewport_Recompute(struct NativeViewport *viewport)
{
    if (viewport->scaleQ8 < NATIVE_VIEWPORT_SCALE_1_0)
        viewport->scaleQ8 = NATIVE_VIEWPORT_SCALE_1_0;
    if (viewport->scaleQ8 > NATIVE_VIEWPORT_SCALE_MAX)
        viewport->scaleQ8 = NATIVE_VIEWPORT_SCALE_MAX;
    viewport->width  = (u16)((NATIVE_VIEWPORT_MIN_WIDTH  * viewport->scaleQ8 + 0x7F) >> 8);
    viewport->height = (u16)((NATIVE_VIEWPORT_MIN_HEIGHT * viewport->scaleQ8 + 0x7F) >> 8);
}

void NativeOverworldViewport_Init(struct NativeViewport *viewport)
{
    u16 requestedWidth = 0;
    u16 requestedHeight = 0;
    const char *env = NULL;

    viewport->width = NATIVE_VIEWPORT_MIN_WIDTH;
    viewport->height = NATIVE_VIEWPORT_MIN_HEIGHT;
    viewport->scaleQ8 = NATIVE_VIEWPORT_SCALE_1_0;
    viewport->centerX = DISPLAY_WIDTH / 2;
    viewport->centerY = DISPLAY_HEIGHT / 2;

    env = getenv("POKEEMERALD_NATIVE_VIEWPORT");
    if (ParseViewportRequest(env, &requestedWidth, &requestedHeight))
    {
        // Derive the scale from the requested width (rounded up to the nearest
        // 1/256), then Recompute so width/height are the exact derived values.
        // 300 -> 320/256 = 1.25x -> 300x200; 360 -> 384/256 = 1.5x -> 360x240.
        viewport->scaleQ8 = (s16)(((u32)requestedWidth * NATIVE_VIEWPORT_SCALE_1_0
                                   + (NATIVE_VIEWPORT_MIN_WIDTH / 2)) / NATIVE_VIEWPORT_MIN_WIDTH);
        NativeOverworldViewport_Recompute(viewport);
    }
}

bool32 NativeOverworldViewport_ZoomOut(struct NativeViewport *viewport)
{
    if (viewport->scaleQ8 >= NATIVE_VIEWPORT_SCALE_MAX)
        return FALSE;
    viewport->scaleQ8 += NATIVE_VIEWPORT_SCALE_STEP;
    NativeOverworldViewport_Recompute(viewport);
    ReportViewportChange(viewport);
    return TRUE;
}

bool32 NativeOverworldViewport_ZoomIn(struct NativeViewport *viewport)
{
    if (viewport->scaleQ8 <= NATIVE_VIEWPORT_SCALE_1_0)
        return FALSE;
    viewport->scaleQ8 -= NATIVE_VIEWPORT_SCALE_STEP;
    NativeOverworldViewport_Recompute(viewport);
    ReportViewportChange(viewport);
    return TRUE;
}

void NativeOverworldViewport_Reset(struct NativeViewport *viewport)
{
    if (viewport->scaleQ8 == NATIVE_VIEWPORT_SCALE_1_0)
        return;
    viewport->scaleQ8 = NATIVE_VIEWPORT_SCALE_1_0;
    NativeOverworldViewport_Recompute(viewport);
    ReportViewportChange(viewport);
}

bool32 NativeOverworldViewport_Active(const struct NativeViewport *viewport)
{
    return viewport->width > DISPLAY_WIDTH || viewport->height > DISPLAY_HEIGHT;
}

void NativeOverworldViewport_TopLeft(struct NativeViewport *viewport,
                                     s32 originX, s32 originY,
                                     s32 *left, s32 *top)
{
    s32 centerX = originX + (DISPLAY_WIDTH / 2);
    s32 centerY = originY + (DISPLAY_HEIGHT / 2);

    viewport->centerX = centerX;
    viewport->centerY = centerY;
    if (left != NULL)
        *left = centerX - (viewport->width / 2);
    if (top != NULL)
        *top = centerY - (viewport->height / 2);
}

#endif

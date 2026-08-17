#ifdef PLATFORM_SDL2

#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#include "global.h"
#include "platform.h"
#include "platform/framedraw.h"
#include "platform/desktop_config.h"
#include "platform/desktop_video.h"
#include "platform/native_overworld_renderer.h"
#include "platform/native_overworld_parity.h"
#include "platform/native_sprite_snapshot.h"
#include "platform/native_field_compositor.h"

HOST_DATA SDL_Window *sdlWindow;
HOST_DATA SDL_Renderer *sdlRenderer;
HOST_DATA SDL_Texture *sdlTexture;

#if defined(LINUX64) && LINUX64
HOST_DATA static SDL_Texture *sNativeOverworldTexture;
HOST_DATA static u32 sNativeOverworldFramebuffer[NATIVE_OVERWORLD_MAX_WIDTH
                                                * NATIVE_OVERWORLD_MAX_HEIGHT];
// Stage 4A continuous viewport state (replaces the Stage 1 enum zoom). width/
// height are always derived from scaleQ8 by NativeOverworldViewport_Recompute().
HOST_DATA static struct NativeViewport sViewport;
// Stage 4A expanded-composite scratch at MAX capacity. The buffer-audit
// constraint: ONLY native-renderer buffers grow (360x240); GBA buffers stay
// 240x160. bgFrame/bgWinner/bgLayers hold the intermediate BG render, objOut the
// OBJ rasterize, and report the named fallback reason when a gate fires.
HOST_DATA static u16 sExpandedBgFrame[NATIVE_OVERWORLD_MAX_WIDTH * NATIVE_OVERWORLD_MAX_HEIGHT];
HOST_DATA static u8  sExpandedBgWinner[NATIVE_OVERWORLD_MAX_WIDTH * NATIVE_OVERWORLD_MAX_HEIGHT];
HOST_DATA static u16 sExpandedBgLayers[4 * NATIVE_OVERWORLD_MAX_WIDTH * NATIVE_OVERWORLD_MAX_HEIGHT];
HOST_DATA static struct NativeObjRenderOutput sExpandedObjOut;
HOST_DATA static struct NativeFieldCompositorReport sExpandedReport;
#endif
HOST_DATA static u32 sFramebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];

#if defined(NATIVE_LINUX) || defined(_WIN32)
/* The game image is presented in the largest aspect-correct rectangle that
 * fits the actual renderer output. Matching aspect ratios fill the window
 * edge-to-edge; mismatched ones get plain black letterbox/pillarbox bars.
 * Source dimensions always come from the active logical framebuffer, never
 * from a hardcoded aspect ratio. */
static void GetGameViewport(int sourceWidth, int sourceHeight, SDL_Rect *viewport)
{
    int outputWidth;
    int outputHeight;

    SDL_GetRendererOutputSize(sdlRenderer, &outputWidth, &outputHeight);
    if (Platform_GetSetting(PLATFORM_SETTING_INTEGER_SCALE))
    {
        int scale = outputWidth / sourceWidth;
        if (outputHeight / sourceHeight < scale)
            scale = outputHeight / sourceHeight;
        if (scale < 1)
            scale = 1;
        viewport->w = sourceWidth * scale;
        viewport->h = sourceHeight * scale;
    }
    else if (outputWidth * sourceHeight <= outputHeight * sourceWidth)
    {
        viewport->w = outputWidth;
        viewport->h = outputWidth * sourceHeight / sourceWidth;
    }
    else
    {
        viewport->w = outputHeight * sourceWidth / sourceHeight;
        viewport->h = outputHeight;
    }
    viewport->x = (outputWidth - viewport->w) / 2;
    viewport->y = (outputHeight - viewport->h) / 2;
}
#endif

static void RenderCurrentTexture(void)
{
    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sdlRenderer);
#if defined(NATIVE_LINUX) || defined(_WIN32)
    {
        SDL_Rect gameViewport;
        GetGameViewport(DISPLAY_WIDTH, DISPLAY_HEIGHT, &gameViewport);
        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &gameViewport);
    }
#else
    SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
#endif
}

#if defined(LINUX64) && LINUX64
static void RenderNativeOverworldTexture(u16 frameWidth, u16 frameHeight)
{
    SDL_Rect source = {0, 0, frameWidth, frameHeight};
    static HOST_DATA u16 sLastPresentedWidth;
    static HOST_DATA u16 sLastPresentedHeight;

    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sdlRenderer);
#if defined(NATIVE_LINUX) || defined(_WIN32)
    {
        SDL_Rect gameViewport;

        GetGameViewport(frameWidth, frameHeight, &gameViewport);
        if (sLastPresentedWidth != frameWidth || sLastPresentedHeight != frameHeight)
        {
            fprintf(stderr,
                    "expanded texture present: source=%dx%d destination=%dx%d\n",
                    frameWidth, frameHeight, gameViewport.w, gameViewport.h);
            fflush(stderr);
            sLastPresentedWidth = frameWidth;
            sLastPresentedHeight = frameHeight;
        }
        SDL_RenderCopy(sdlRenderer, sNativeOverworldTexture, &source, &gameViewport);
    }
#else
    if (sLastPresentedWidth != frameWidth || sLastPresentedHeight != frameHeight)
    {
        fprintf(stderr, "expanded texture present: source=%dx%d\n", frameWidth, frameHeight);
        fflush(stderr);
        sLastPresentedWidth = frameWidth;
        sLastPresentedHeight = frameHeight;
    }
    SDL_RenderCopy(sdlRenderer, sNativeOverworldTexture, &source, NULL);
#endif
}
#endif

static void ApplyPlatformSettings(void)
{
    SDL_RenderSetVSync(sdlRenderer, Platform_GetSetting(PLATFORM_SETTING_VSYNC));
#if defined(NATIVE_LINUX) || defined(_WIN32)
    SDL_SetWindowFullscreen(sdlWindow, Platform_GetSetting(PLATFORM_SETTING_FULLSCREEN)
                                      ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!Platform_GetSetting(PLATFORM_SETTING_FULLSCREEN))
    {
        int scale = Platform_GetSetting(PLATFORM_SETTING_WINDOW_SCALE);
        SDL_SetWindowSize(sdlWindow, 320 * scale, 180 * scale);
        SDL_SetWindowPosition(sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
#endif
}

bool32 Platform_VideoInit(void)
{
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
#if defined(NATIVE_LINUX) || defined(_WIN32)
    sdlWindow = SDL_CreateWindow("Pokemon Emerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#else
    sdlWindow = SDL_CreateWindow("pokeemerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                 SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#endif
    if (sdlWindow == NULL)
    {
        DBGPRINTF("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return FALSE;
    }

#ifdef __ANDROID__
    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_ACCELERATED);
#else
    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_PRESENTVSYNC);
#endif
    if (sdlRenderer == NULL)
    {
        DBGPRINTF("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return FALSE;
    }
    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sdlRenderer);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

#if defined(NATIVE_LINUX) || defined(_WIN32)
    SDL_RenderSetLogicalSize(sdlRenderer, 0, 0);
#else
    SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    SDL_RenderSetIntegerScale(sdlRenderer, SDL_TRUE);
#endif
    ApplyPlatformSettings();

    sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (sdlTexture == NULL)
    {
        DBGPRINTF("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return FALSE;
    }
    SDL_SetTextureBlendMode(sdlTexture, SDL_BLENDMODE_NONE);
#if defined(LINUX64) && LINUX64
    NativeOverworldViewport_Init(&sViewport);
    sNativeOverworldTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                                SDL_TEXTUREACCESS_STREAMING,
                                                NATIVE_OVERWORLD_MAX_WIDTH,
                                                NATIVE_OVERWORLD_MAX_HEIGHT);
    if (sNativeOverworldTexture == NULL)
    {
        DBGPRINTF("Native overworld texture could not be created: %s\n", SDL_GetError());
    }
    else
    {
        SDL_SetTextureBlendMode(sNativeOverworldTexture, SDL_BLENDMODE_NONE);
        // Nearest-neighbor for the expanded viewport presentation (no smoothing).
        SDL_SetTextureScaleMode(sNativeOverworldTexture, SDL_ScaleModeNearest);
    }
#endif
    return TRUE;
}

void Platform_VideoDrawFrame(void)
{
    static HOST_DATA uint16_t gbaImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
#if defined(LINUX64) && LINUX64
    static HOST_DATA uint16_t nativeOverworldImage[NATIVE_OVERWORLD_MAX_WIDTH
                                                  * NATIVE_OVERWORLD_MAX_HEIGHT];
    bool32 viewportActive = NativeOverworldViewport_Active(&sViewport);
    static HOST_DATA u16 sLastActiveWidth;
    static HOST_DATA u16 sLastActiveHeight;
    // Last named composite-fallback reason logged (rate-limit; 0 == OK sentinel).
    static HOST_DATA u8 sLastExpandedFallback;
    // Last stable-canvas fallback canvas dimensions logged (rate-limit).
    static HOST_DATA u16 sLastFallbackCanvasWidth;
    static HOST_DATA u16 sLastFallbackCanvasHeight;
    bool32 nativeOverworldDrawn;

    // Stage 1 map-background backend (env-gated, off by default). When enabled,
    // every 240x160 map-background pixel is produced by the native renderer from
    // an immutable snapshot; DrawFrame output is only used as the fallback.
    static HOST_DATA struct NativeOverworldSnapshot sNativeOverworldSnapshot;
    // Stage 3A OBJ snapshot captured at the SAME coherent capture point as the
    // map snapshot (worker blocked at VBlankIntrWait): the PUBLISHED command
    // frame plus final OAM, OBJ VRAM, OBJ palette and registers. Consumed by the
    // parity OBJ diagnostic; used by Stage 3B for OBJ compositing.
    static HOST_DATA struct NativeObjSnapshot sNativeObjSnapshot;
    static HOST_DATA u16 sNativeOverworldMapFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static HOST_DATA bool32 sNativeOverworldEnabled;
    static HOST_DATA bool32 sNativeOverworldChecked;
    static HOST_DATA bool32 sNativeOverworldActiveLogged;
    // Runtime parity capture (POKEEMERALD_NATIVE_PARITY=1). DrawFrame fills the
    // BG-only oracle and winner-layer map into these buffers on capture frames.
    static HOST_DATA u16 sParityOracleFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static HOST_DATA u8 sParityOracleLayers[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    bool32 needNativeRender;
    bool32 needExpandedRender;
    bool32 sParityCaptureActive;
    bool32 sParitySnapshotValid;
    bool32 sExpandedSnapshotValid;
    bool32 sParityNativeValid;
    bool32 sParityOracleProduced;
    // Stage 3C composite parity (POKEEMERALD_NATIVE_OBJ_PARITY=1): the REAL
    // DrawFrame main-pass composite + per-pixel winner map land in these buffers
    // on composite capture frames (gParityCompositeLayers seam).
    static HOST_DATA u8 sParityCompositeLayers[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    bool32 sCompositeCaptureActive;
    bool32 nativeMapDrawn;
#endif

#if defined(LINUX64) && LINUX64
    if (!sNativeOverworldChecked)
    {
        sNativeOverworldChecked = TRUE;
        sNativeOverworldEnabled = getenv("POKEEMERALD_NATIVE_OVERWORLD") != NULL;
        if (sNativeOverworldEnabled)
        {
            fprintf(stderr, "native overworld map renderer enabled (Stage 1; sprites not yet composited)\n");
            fflush(stderr);
        }
    }
#endif

    memset(gbaImage, 0, sizeof(gbaImage));
#if defined(LINUX64) && LINUX64
    // One coherent snapshot point for the frame. The worker is blocked at
    // VBlankIntrWait; the VBlank handler with its tilemap/animation DMA
    // transfers runs only later in Platform_SchedulerCompleteFrame. Capturing
    // here, BEFORE DrawFrame, means the native renderer and the GBA DrawFrame
    // path observe the identical frame state (the architecture-doc capture
    // point). The same snapshot drives the Stage-1 display path and the runtime
    // parity capture.
    sParityCaptureActive = NativeOverworldParity_BeginFrame();
    sCompositeCaptureActive = NativeOverworldParity_CompositeBeginFrame();
    sParitySnapshotValid = FALSE;
    sParityNativeValid = FALSE;
    sParityOracleProduced = FALSE;
    sExpandedSnapshotValid = FALSE;
    nativeMapDrawn = FALSE;
    needNativeRender = sNativeOverworldEnabled && !viewportActive;
    needExpandedRender = viewportActive && sNativeOverworldTexture != NULL;
    // Both begin-frame gates are called unconditionally (never short-circuited):
    // CompositeBeginFrame shares the module per-frame counter and must advance it
    // even when the BG-only mode is inactive.
    if (needNativeRender || needExpandedRender || sParityCaptureActive || sCompositeCaptureActive)
    {
        // The Stage-1 display path uses the strict zoom/expanded predicate so it
        // never draws the native map during scenes it cannot reproduce. The
        // Stage-2 parity path is deliberately decoupled: it uses the lean parity
        // predicate and never depends on whether the old experimental expanded
        // renderer can activate. The Stage-4A expanded path shares the parity
        // snapshot because it is the moving-frame-safe (Stage 2.1 presentation-
        // origin) capture. When several are active, the stricter gate wins for
        // the shared snapshot (safe for all uses).
        if ((sParityCaptureActive || sCompositeCaptureActive || needExpandedRender) && !needNativeRender)
            sParitySnapshotValid = NativeOverworld_CaptureParitySnapshot(&sNativeOverworldSnapshot);
        else
            sParitySnapshotValid = NativeOverworld_CaptureSnapshot(&sNativeOverworldSnapshot);
        // OBJ presentation snapshot (published command frame + final OAM/OBJ
        // VRAM/OBJ palette/registers), coherent with the map snapshot above.
        NativeObjSnapshot_Capture(&sNativeObjSnapshot);
        // Stage 4A: the expanded composite additionally requires DISPCNT OBJ
        // enabled. When OBJ is off the captured OBJ snapshot holds stale
        // OAM/VRAM entries the GBA is not drawing, so it must never composite
        // (the parity capture omits this check by design; the presented
        // expanded path must not).
        if (needExpandedRender && sParitySnapshotValid)
            sExpandedSnapshotValid = (sNativeOverworldSnapshot.dispCnt & DISPCNT_OBJ_ON) != 0;
        if (sParitySnapshotValid)
            sParityNativeValid = NativeOverworldRenderer_DrawMapFrame(&sNativeOverworldSnapshot,
                                                                      sNativeOverworldMapFrame);
        if (needNativeRender && sParitySnapshotValid)
        {
            nativeMapDrawn = TRUE;
            if (!sNativeOverworldActiveLogged)
            {
                sNativeOverworldActiveLogged = TRUE;
                fprintf(stderr, "native map background rendering at 240x160 (independent of DrawFrame)\n");
                fflush(stderr);
            }
        }
    }
    if (sParityCaptureActive)
    {
        gParityBGPixelsBuffer = sParityOracleFrame;
        gParityBGPixelLayers = sParityOracleLayers;
        gParityOracleProduced = 0;
    }
    if (sCompositeCaptureActive)
    {
        // The final-composite oracle seam: DrawFrame's main pass additionally
        // records the per-pixel final winner (0=backdrop,1-4=BG0-3,5-8=OBJ0-3)
        // into the caller-provided layer buffer, which the composite parity
        // compares against the native composite winner-for-winner.
        gParityCompositeLayers = sParityCompositeLayers;
        gParityCompositeProduced = 0;
    }
#endif
    DrawFrame(gbaImage);
#if defined(LINUX64) && LINUX64
    if (sParityCaptureActive)
    {
        sParityOracleProduced = (gParityOracleProduced != 0);
        gParityBGPixelsBuffer = NULL;
        gParityBGPixelLayers = NULL;
        // Explicit per-frame outcome accounting: no capture may disappear.
        NativeOverworldParity_EndFrame(&sNativeOverworldSnapshot,
                                       sParityNativeValid ? sNativeOverworldMapFrame : NULL,
                                       sParityOracleFrame, sParityOracleLayers,
                                       sParitySnapshotValid
                                           ? (sParityNativeValid
                                                  ? (sParityOracleProduced
                                                         ? PARITY_OUTCOME_COMPARED
                                                         : PARITY_OUTCOME_ORACLE_MISSING)
                                                  : PARITY_OUTCOME_NATIVE_FAILED)
                                           : PARITY_OUTCOME_SNAPSHOT_FAILED);
        // Stage 3A OBJ diagnostic (additive: rate-limited state line + env-gated
        // fixture writer). Never changes EndFrame's accounting.
        NativeOverworldParity_ObjFrame(&sNativeObjSnapshot);
    }
    if (sCompositeCaptureActive)
    {
        // Account the composite capture: rasterize the OBJ snapshot, gate the BG
        // snapshot + OBJ capability flags, run the proven BG+OBJ compositor, and
        // compare color AND per-pixel winner against the real DrawFrame composite
        // + its winner map. On mismatch it emits a composite trace at the first
        // differing pixel and writes bounded artifacts under
        // build/native-parity/objcomp/. Never alters the BG-only accounting.
        NativeOverworldParity_CompositeFrame(&sNativeOverworldSnapshot,
                                             &sNativeObjSnapshot,
                                             gbaImage,
                                             sParityCompositeLayers,
                                             gParityCompositeProduced != 0);
        gParityCompositeLayers = NULL;
        gParityCompositeProduced = 0;
    }
#endif
    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        uint16_t color = gbaImage[i];
#if defined(LINUX64) && LINUX64
        if (nativeMapDrawn)
            color = sNativeOverworldMapFrame[i];
#endif
        uint32_t r = (color & 0x1F) * 255 / 31;
        uint32_t g = ((color >> 5) & 0x1F) * 255 / 31;
        uint32_t b = ((color >> 10) & 0x1F) * 255 / 31;
        sFramebuffer[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    SDL_UpdateTexture(sdlTexture, NULL, sFramebuffer, DISPLAY_WIDTH * sizeof(Uint32));
#if defined(LINUX64) && LINUX64
    // Stage 4A: the expanded viewport is rendered ENTIRELY natively (no
    // center+margin reconstruction of the 240x160 GBA frame). DrawExpandedComposite
    // derives viewport placement from the continuous viewport and runs the same
    // capability gates as the 240x160 path, reporting an explicit named reason
    // when it cannot compose a frame.
    nativeOverworldDrawn = viewportActive
                       && sNativeOverworldTexture != NULL
                       && sExpandedSnapshotValid
                       && NativeOverworldRenderer_DrawExpandedComposite(
                               &sNativeOverworldSnapshot, &sNativeObjSnapshot,
                               &sExpandedObjOut, &sViewport,
                               sExpandedBgFrame, sExpandedBgWinner, sExpandedBgLayers,
                               nativeOverworldImage, NULL, &sExpandedReport);
    if (nativeOverworldDrawn)
    {
        SDL_Rect source = {0, 0, sViewport.width, sViewport.height};

        if (sLastActiveWidth != sViewport.width || sLastActiveHeight != sViewport.height)
        {
            fprintf(stderr, "expanded renderer active: %dx%d\n", sViewport.width, sViewport.height);
            fflush(stderr);
            sLastActiveWidth = sViewport.width;
            sLastActiveHeight = sViewport.height;
        }

        for (int i = 0; i < sViewport.width * sViewport.height; i++)
        {
            uint16_t color = nativeOverworldImage[i];
            uint32_t r = (color & 0x1F) * 255 / 31;
            uint32_t g = ((color >> 5) & 0x1F) * 255 / 31;
            uint32_t b = ((color >> 10) & 0x1F) * 255 / 31;
            sNativeOverworldFramebuffer[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
        SDL_UpdateTexture(sNativeOverworldTexture, &source, sNativeOverworldFramebuffer,
                          sViewport.width * sizeof(Uint32));
        RenderNativeOverworldTexture(sViewport.width, sViewport.height);
        REG_VCOUNT = 161;
        return;
    }
    // Named fallback (Stage 4A directive 20): the expanded texture is NEVER
    // presented with stale or partially-composed pixels. When a capability gate
    // fired after the snapshot validated, log the named reason once per
    // distinct reason; snapshot-invalid frames (off-overworld scenes, OBJ off)
    // skip the reason log (DrawExpandedComposite never ran).
    else if (sExpandedSnapshotValid)
    {
        if (sLastExpandedFallback != sExpandedReport.reason)
        {
            fprintf(stderr, "expanded composite fallback: reason=%d\n", sExpandedReport.reason);
            fflush(stderr);
            sLastExpandedFallback = sExpandedReport.reason;
        }
    }
    // Stage 4A Issue B (directives 9-12/14): STOP VISUAL ZOOMING DURING
    // FALLBACK. While a continuous viewport is SELECTED but the expanded
    // composite cannot draw (capability gate OR invalid snapshot), present a
    // viewport-SIZED canvas: the AUTHORITATIVE 240x160 frame centered 1:1 with
    // PURE-BLACK margins. The presented canvas is exactly the expanded render's
    // dimensions, so the on-screen scale NEVER changes on a fallback -- only the
    // content mode does (native expanded world -> authoritative core + black
    // margins). The SELECTED viewport sViewport is never mutated (a capability
    // failure must not change the user's chosen zoom), the margins are freshly
    // written every frame (never stale expanded pixels), and the existing
    // expanded texture is reused (SDL must NOT recreate the 240x160 texture
    // just for a fallback -- directive 14).
    if (viewportActive && sNativeOverworldTexture != NULL)
    {
        if (NativeOverworldRenderer_BuildFallbackCanvas(
                sFramebuffer, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                &sViewport, sNativeOverworldFramebuffer))
        {
            SDL_Rect source = {0, 0, sViewport.width, sViewport.height};

            // The expanded renderer is not active while the canvas is presented.
            sLastActiveWidth = 0;
            sLastActiveHeight = 0;
            if (sLastFallbackCanvasWidth != sViewport.width
             || sLastFallbackCanvasHeight != sViewport.height)
            {
                fprintf(stderr, "expanded fallback canvas: %dx%d (240x160 core + black margins)\n",
                        sViewport.width, sViewport.height);
                fflush(stderr);
                sLastFallbackCanvasWidth = sViewport.width;
                sLastFallbackCanvasHeight = sViewport.height;
            }
            SDL_UpdateTexture(sNativeOverworldTexture, &source, sNativeOverworldFramebuffer,
                              sViewport.width * sizeof(Uint32));
            RenderNativeOverworldTexture(sViewport.width, sViewport.height);
            REG_VCOUNT = 161;
            return;
        }
        // BuildFallbackCanvas can only fail on a degenerate viewport (canvas
        // smaller than the 240x160 core); fall through to the 240x160 path
        // defensively.
    }
    sLastActiveWidth = 0;
    sLastActiveHeight = 0;
#endif
    SDL_RenderClear(sdlRenderer);
#if defined(NATIVE_LINUX) || defined(_WIN32)
    {
        SDL_Rect gameViewport;
        GetGameViewport(DISPLAY_WIDTH, DISPLAY_HEIGHT, &gameViewport);
        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &gameViewport);
    }
#else
    SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
#endif
    REG_VCOUNT = 161;
}

void Platform_VideoRenderFramebuffer(void)
{
    if (sdlRenderer != NULL && sdlTexture != NULL)
        RenderCurrentTexture();
}

void Platform_VideoPresent(void)
{
    SDL_RenderPresent(sdlRenderer);
}

void Platform_VideoSetStatus(const char *status)
{
    if (sdlWindow != NULL)
        SDL_SetWindowTitle(sdlWindow, status != NULL && status[0] != '\0'
                                      ? status : "Pokemon Emerald");
}

void Platform_VideoSetFastForward(bool32 active)
{
    /* SDL's PRESENTVSYNC renderer blocks inside SDL_RenderPresent. Disable it
     * for the duration of fast-forward so the host presentation cadence cannot
     * become the simulation clock. The host loop presents the latest texture at
     * its own approximately-60 Hz cadence while the worker continues to run. */
    if (sdlRenderer != NULL)
        SDL_RenderSetVSync(sdlRenderer, active ? 0 : Platform_GetSetting(PLATFORM_SETTING_VSYNC));
}

#if defined(LINUX64) && LINUX64
void Platform_VideoZoomIn(void)
{
    NativeOverworldViewport_ZoomIn(&sViewport);
}

void Platform_VideoZoomOut(void)
{
    NativeOverworldViewport_ZoomOut(&sViewport);
}

void Platform_VideoZoomReset(void)
{
    NativeOverworldViewport_Reset(&sViewport);
}
#endif

bool32 Platform_VideoCopyFramebuffer(void *dest, u32 size)
{
    if (dest == NULL || size != sizeof(sFramebuffer))
        return FALSE;
    memcpy(dest, sFramebuffer, sizeof(sFramebuffer));
    return TRUE;
}

bool32 Platform_VideoRestoreFramebuffer(const void *source, u32 size)
{
    if (source == NULL || size != sizeof(sFramebuffer) || sdlTexture == NULL)
        return FALSE;
    memcpy(sFramebuffer, source, sizeof(sFramebuffer));
    SDL_UpdateTexture(sdlTexture, NULL, sFramebuffer, DISPLAY_WIDTH * sizeof(Uint32));
    RenderCurrentTexture();
    return TRUE;
}

void Platform_VideoBeginHostUi(void)
{
    SDL_RenderSetLogicalSize(sdlRenderer, 960, 540);
}

void Platform_VideoEndHostUi(void)
{
#if defined(NATIVE_LINUX) || defined(_WIN32)
    SDL_RenderSetLogicalSize(sdlRenderer, 0, 0);
#else
    SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    SDL_RenderSetIntegerScale(sdlRenderer, SDL_TRUE);
#endif
}

void Platform_VideoApplySetting(enum PlatformSetting setting, u8 value)
{
    if (setting == PLATFORM_SETTING_VSYNC)
        SDL_RenderSetVSync(sdlRenderer, value);
#if defined(NATIVE_LINUX) || defined(_WIN32)
    else if (setting == PLATFORM_SETTING_FULLSCREEN)
    {
        SDL_SetWindowFullscreen(sdlWindow, value ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        if (!value)
        {
            int scale = Platform_GetSetting(PLATFORM_SETTING_WINDOW_SCALE);
            SDL_SetWindowSize(sdlWindow, 320 * scale, 180 * scale);
            SDL_SetWindowPosition(sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    }
    else if (setting == PLATFORM_SETTING_WINDOW_SCALE && !Platform_GetSetting(PLATFORM_SETTING_FULLSCREEN))
    {
        SDL_SetWindowSize(sdlWindow, 320 * value, 180 * value);
        SDL_SetWindowPosition(sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
#endif
}

void Platform_VideoShutdown(void)
{
    SDL_DestroyTexture(sdlTexture);
#if defined(LINUX64) && LINUX64
    SDL_DestroyTexture(sNativeOverworldTexture);
    sNativeOverworldTexture = NULL;
#endif
    SDL_DestroyRenderer(sdlRenderer);
    SDL_DestroyWindow(sdlWindow);
    sdlTexture = NULL;
    sdlRenderer = NULL;
    sdlWindow = NULL;
}

#endif

#if defined(PLATFORM_SDL2) && defined(LINUX64) && LINUX64

#include "global.h"
#include "platform/native_field_compositor.h"
#include "platform/native_overworld_renderer.h"

/*
 * Stage 3C final compositor. See native_field_compositor.h for the winner
 * rules (identical to gba_easy_draw.c DrawScanline) and the capability
 * boundary. This composes the proven BG frame+metadata and the proven Stage 3B
 * OBJ layers; it never renders BG or OBJ pixels itself.
 */

/* Stage 3E blend helpers -- faithful ports of the gba_easy_draw.c oracle
 * alphaBlendColor() and alphaBlendSelectTargetB() (called with
 * spriteBlendEnabled=false from the semi-transparent OBJ path). See
 * gba_easy_draw.c:385-477. The compositor runs after the BG layers are fully
 * resolved, exactly the state the oracle blends against at OBJ draw time. */

static u16 NativeComposite_AlphaBlendColor(u16 targetA, u16 targetB, u16 bldAlpha)
{
    u32 eva = bldAlpha & 0x1F;
    u32 evb = (bldAlpha >> 8) & 0x1F;
    // Oracle arithmetic: ((A*eva)+(B*evb)) >> 4 -- divide by 16, then clamp
    // each channel to 31. Inputs' bit 15 (presence) is masked out per channel.
    u32 r = ((((u32)(targetA >>  0) & 0x1F) * eva) + (((u32)(targetB >>  0) & 0x1F) * evb)) >> 4;
    u32 g = ((((u32)(targetA >>  5) & 0x1F) * eva) + (((u32)(targetB >>  5) & 0x1F) * evb)) >> 4;
    u32 b = ((((u32)(targetA >> 10) & 0x1F) * eva) + (((u32)(targetB >> 10) & 0x1F) * evb)) >> 4;

    if (r > 31)
        r = 31;
    if (g > 31)
        g = 31;
    if (b > 31)
        b = 31;

    return (u16)(r | (g << 5) | (b << 10) | 0x8000);
}

static bool32 NativeComposite_FindTargetB(const u16 *bgLayers, const u8 *bgPriority,
                                          const struct NativeCompositeBlendState *blend,
                                          u8 startPriority, u32 pixelIndex,
                                          u32 pixelCount,
                                          u16 *out, u8 *outKind)
{
    u32 blndprnum;
    u32 bgnum;

    // Walk priorities startPriority..3 (toward the background), exactly the
    // oracle's alphaBlendSelectTargetB loop. Sprite blending is disabled for
    // the semi path, so only BG layers are candidates. Per priority the oracle
    // iterates prioritySortedBgs in ASCENDING bgnum order (built bgnum 0..3);
    // matching bgnum-by-priority reproduces that order. BG0's layer slot is
    // always 0 on a supported frame, so it is never selected and never bails.
    for (blndprnum = startPriority; blndprnum <= 3; blndprnum++)
    {
        for (bgnum = 0; bgnum < 4; bgnum++)
        {
            u16 layerColor;
            bool32 opaque;
            bool32 enabled;

            if (bgPriority[bgnum] != blndprnum)
                continue;
            layerColor = bgLayers[bgnum * pixelCount + pixelIndex];
            opaque = (layerColor & 0x8000) != 0;
            enabled = (((blend->dispCnt >> 8) & 0xF) & (1u << bgnum)) != 0;

            if (opaque && (blend->bldCnt & (1u << (8 + bgnum))) && enabled)
            {
                *out = layerColor;
                if (outKind != NULL)
                    *outKind = (u8)(bgnum + 1); // oracle bgnum+1: 1=BG0..4=BG3
                return TRUE;
            }
            // Bail on the first opaque enabled BG at a priority ABOVE the OBJ's
            // (blndprnum != startPriority) that is not a TGT2 target -- the
            // oracle's `prnum != blndprnum` early return.
            if (opaque && enabled && blndprnum != startPriority)
                return FALSE;
        }
    }
    // No BG selected: the backdrop is a valid targetB when TGT2_BD is set.
    if (blend->bldCnt & BLDCNT_TGT2_BD)
    {
        *out = blend->backdropColor;
        if (outKind != NULL)
            *outKind = 5; // backdrop
        return TRUE;
    }
    return FALSE;
}

bool32 NativeFieldCompositor_Composite(const u16 *bgFrame, const u8 *bgWinner,
                                       const u8 *bgPriority, const u16 *bgLayers,
                                       const struct NativeObjRenderOutput *objOut,
                                       const struct NativeCompositeBlendState *blend,
                                       u16 *outFrame, u8 *outWinner)
{
    return NativeFieldCompositor_CompositeEx(bgFrame, bgWinner, bgPriority, bgLayers,
                                             objOut, blend, outFrame, outWinner,
                                             (u32)DISPLAY_WIDTH * DISPLAY_HEIGHT);
}

/*
 * Stage 4A: the same pure merge over an ARBITRARY pixel count (viewport width x
 * height). The OBJ layers and bgLayers are indexed with pixelCount as the flat
 * stride; for the 240x160 case this is byte-identical to Composite() (the
 * wrapper above). Winner encoding and the semi-OBJ blend path are unchanged.
 */
bool32 NativeFieldCompositor_CompositeEx(const u16 *bgFrame, const u8 *bgWinner,
                                         const u8 *bgPriority, const u16 *bgLayers,
                                         const struct NativeObjRenderOutput *objOut,
                                         const struct NativeCompositeBlendState *blend,
                                         u16 *outFrame, u8 *outWinner,
                                         u32 pixelCount)
{
    u32 i;

    if (bgFrame == NULL || bgWinner == NULL || bgPriority == NULL
     || bgLayers == NULL || objOut == NULL || blend == NULL
     || outFrame == NULL)
        return FALSE;

    for (i = 0; i < pixelCount; i++)
    {
        u16 bgColor = bgFrame[i];
        u8 bgW = bgWinner[i];
        u8 objPrio = NATIVE_OBJ_PRIORITY_LAYERS; // sentinel: no OBJ claimed
        u8 pr;

        // Foremost OBJ: the lowest OBJ-priority layer (0 = foremost) with an
        // opaque pixel. Stage 3B already resolves lowest-OAM-wins within a
        // priority, exactly like the oracle's scanline loop.
        for (pr = 0; pr < NATIVE_OBJ_PRIORITY_LAYERS; pr++)
        {
            if (objOut->layers[pr].color[i] & 0x8000)
            {
                objPrio = pr;
                break;
            }
        }
        if (objPrio < NATIVE_OBJ_PRIORITY_LAYERS)
        {
            // OBJ wins iff its priority <= the winning BG's priority; the
            // backdrop is "priority infinity" (4), so any OBJ beats it.
            u8 bgPrio = (bgW == NATIVE_COMPOSITE_WIN_BACKDROP)
                            ? (u8)(NATIVE_OBJ_PRIORITY_LAYERS)
                            : bgPriority[bgW - 1];

            if (objPrio <= bgPrio)
            {
                u16 objColor = objOut->layers[objPrio].color[i];

                // Stage 3E: a winning SEMI-TRANSPARENT OBJ pixel ALWAYS blends
                // in the oracle (`(blendMode == 1 && TGT1_OBJ && win) ||
                // isSemiTransparent` -- isSemiTransparent alone is sufficient),
                // regardless of effect mode / TGT1 / windows. EVA/EVB come from
                // BLDALPHA; targetB comes from the BG-only walk starting at the
                // OBJ's priority. When no targetB is found the oracle writes
                // target A unchanged (FindTargetB returned FALSE).
                if (objOut->layers[objPrio].semi[i])
                {
                    u16 targetB = 0;
                    if (NativeComposite_FindTargetB(bgLayers, bgPriority, blend,
                                                    objPrio, i, pixelCount,
                                                    &targetB, NULL))
                        objColor = NativeComposite_AlphaBlendColor(objColor, targetB,
                                                                   blend->bldAlpha);
                }
                outFrame[i] = objColor;
                if (outWinner != NULL)
                    outWinner[i] = (u8)(NATIVE_COMPOSITE_WIN_OBJ0 + objPrio);
                continue;
            }
        }
        outFrame[i] = bgColor;
        if (outWinner != NULL)
            outWinner[i] = bgW; // 0=backdrop, 2/3/4=BG1/2/3 (oracle encoding)
    }
    return TRUE;
}

bool32 NativeField_BlendAffectsComposite(u16 bldCnt, u16 bldY, u16 bldAlpha)
{
    u32 effect = (bldCnt >> 6) & 3;
    u32 eva = bldAlpha & 0x1F;
    u32 evb = (bldAlpha >> 8) & 0x1F;

    switch (effect)
    {
    case 0: // BLDCNT_EFFECT_NONE: flags present but no effect applied
        return FALSE;
    case 1: // BLDCNT_EFFECT_BLEND (alpha): top TGT1 pixel blends with a TGT2 target below
        // EVA==16/EVB==0 reproduces the TGT1 color exactly even when the blend
        // fires. With no TGT1 layer (BG0-3/OBJ) the blend never fires -- the
        // backdrop is never an alpha TGT1 target in the compositor. With no
        // TGT2 target at all, alphaBlendSelectTargetB bails and target A is
        // written unchanged.
        if (eva == 16 && evb == 0)
            return FALSE;
        if ((bldCnt & (BLDCNT_TGT1_BG_ALL | BLDCNT_TGT1_OBJ)) == 0)
            return FALSE;
        if ((bldCnt & BLDCNT_TGT2_ALL) == 0)
            return FALSE;
        return TRUE;
    case 2: // BLDCNT_EFFECT_LIGHTEN
    case 3: // BLDCNT_EFFECT_DARKEN: top TGT1 pixel brightened/darkened by BLDY
        // BLDY==0 is a no-op on every channel. The backdrop IS a valid TGT1
        // target for these effects (the compositor fills it per scanline).
        if (bldY == 0)
            return FALSE;
        if ((bldCnt & BLDCNT_TGT1_ALL) == 0)
            return FALSE;
        return TRUE;
    default: // reserved effect value: unknown, err on the side of rejecting
        return TRUE;
    }
}

bool32 NativeOverworldRenderer_DrawCompositeFrame(
    const struct NativeOverworldSnapshot *bgSnap,
    const struct NativeObjRenderOutput *objOut,
    u16 *bgFrame, u8 *bgWinner, u16 *bgLayers,
    u16 *outFrame, u8 *outWinner,
    struct NativeFieldCompositorReport *report)
{
    struct NativeCompositeBlendState blend;
    u8 bgPriority[4];
    u8 bg;

    if (report == NULL)
        return FALSE;
    memset(report, 0, sizeof(*report));
    if (bgSnap == NULL || objOut == NULL || bgFrame == NULL || bgWinner == NULL
     || bgLayers == NULL || outFrame == NULL)
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_NULL_INPUT;
        return FALSE;
    }

    // BG capability gates: map support (checked by DrawMapFrameWithMeta) and
    // the BG0 overlay gate (the native BG path renders BG1/2/3 only).
    if (!NativeOverworldRenderer_DrawMapFrameWithMeta(bgSnap, bgFrame, bgWinner,
                                                      bgLayers))
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_BG_UNSUPPORTED;
        return FALSE;
    }
    if (NativeOverworld_SnapshotHasBg0Content(bgSnap))
    {
        report->bg0Content = 1;
        report->reason = NATIVE_COMPOSITE_FALLBACK_BG0_OVERLAY;
        return FALSE;
    }
    if (NativeField_BlendAffectsComposite(bgSnap->bldCnt, bgSnap->bldY,
                                          bgSnap->bldAlpha))
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_BLEND_EFFECT;
        return FALSE;
    }

    // OBJ capability gates from the Stage 3B rasterize output.
    if (!objOut->produced)
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_OBJ_NOT_PRODUCED;
        return FALSE;
    }
    if (objOut->capabilityFlags != 0)
    {
        report->objCapabilityFlags = objOut->capabilityFlags;
        report->objRejectCount = objOut->commandRejectedCount;
        report->reason = NATIVE_COMPOSITE_FALLBACK_OBJ_UNSUPPORTED;
        return FALSE;
    }

    // BG priorities from the captured BGCNT values (BG0 never wins a supported
    // frame; its slot is filled but unused).
    for (bg = 0; bg < 3; bg++)
        bgPriority[bg + 1] = NativeOverworldSnapshotGetBgPriority(bgSnap, bg);
    bgPriority[0] = (u8)(bgSnap->bg0Cnt & 3);

    // Stage 3E blend state: frame-wide register captures + the backdrop color
    // (PLTT[0], what the oracle reads as `*(uint16_t*)PLTT`).
    blend.bldCnt = bgSnap->bldCnt;
    blend.bldAlpha = bgSnap->bldAlpha;
    blend.bldY = bgSnap->bldY;
    blend.dispCnt = bgSnap->dispCnt;
    blend.backdropColor = bgSnap->palette[0];

    if (!NativeFieldCompositor_Composite(bgFrame, bgWinner, bgPriority, bgLayers,
                                         objOut, &blend, outFrame, outWinner))
    {
        report->reason = NATIVE_COMPOSITE_FALLBACK_NULL_INPUT;
        return FALSE;
    }
    report->reason = NATIVE_COMPOSITE_OK;
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
    const u32 i = (u32)y * DISPLAY_WIDTH + x;
    u8 objPrio = NATIVE_OBJ_PRIORITY_LAYERS;
    u8 pr;

    if (bgFrame == NULL || bgWinner == NULL || bgPriority == NULL
     || bgLayers == NULL || objSnap == NULL || objOut == NULL || blend == NULL
     || trace == NULL)
        return FALSE;
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT)
        return FALSE;
    memset(trace, 0, sizeof(*trace));

    // Blend state (always filled; the on-demand dump needs the raw registers).
    trace->bldCnt = blend->bldCnt;
    trace->bldAlpha = blend->bldAlpha;
    trace->bldY = blend->bldY;
    trace->blendMode = (u8)((blend->bldCnt >> 6) & 3);
    trace->tgt2Bits = (u8)((blend->bldCnt >> 8) & 0x1F);
    trace->eva = (u8)(blend->bldAlpha & 0x1F);
    trace->evb = (u8)((blend->bldAlpha >> 8) & 0x1F);

    // BG side from the produced metadata (winner byte 0/2/3/4, priority from
    // the captured BGCNT values, color from the native BG frame).
    trace->bgWinner = bgWinner[i];
    if (bgWinner[i] == NATIVE_COMPOSITE_WIN_BACKDROP)
    {
        trace->bgPriority = 0xFF;
    }
    else
    {
        trace->bgPriority = bgPriority[bgWinner[i] - 1];
        trace->bgColor = bgFrame[i];
    }

    // OBJ side: foremost OBJ-priority layer with an opaque pixel, plus the full
    // Stage 3B sample trace for provenance/command attribution.
    for (pr = 0; pr < NATIVE_OBJ_PRIORITY_LAYERS; pr++)
    {
        if (objOut->layers[pr].color[i] & 0x8000)
        {
            objPrio = pr;
            break;
        }
    }
    if (objPrio < NATIVE_OBJ_PRIORITY_LAYERS)
    {
        trace->objPresent = TRUE;
        trace->objPriority = objPrio;
        trace->objColor = objOut->layers[objPrio].color[i] & 0x7FFF;
        NativeObjRender_Trace(objSnap, objOut, objPrio, x, y, &trace->objTrace);
    }

    // Final decision (identical rule to Composite), including the Stage 3E
    // semi-OBJ blend for the winning pixel.
    if (trace->objPresent)
    {
        u8 bgPrio = (trace->bgWinner == NATIVE_COMPOSITE_WIN_BACKDROP)
                        ? (u8)(NATIVE_OBJ_PRIORITY_LAYERS)
                        : trace->bgPriority;

        if (trace->objPriority <= bgPrio)
        {
            u16 objColor = objOut->layers[objPrio].color[i];
            trace->finalWinner = (u8)(NATIVE_COMPOSITE_WIN_OBJ0 + objPrio);

            if (objOut->layers[objPrio].semi[i])
            {
                u16 targetB = 0;
                trace->semiObj = 1;
                trace->preBlendColor = objColor & 0x7FFF;
                if (NativeComposite_FindTargetB(bgLayers, bgPriority, blend,
                                                objPrio, i,
                                                (u32)DISPLAY_WIDTH * DISPLAY_HEIGHT,
                                                &targetB,
                                                &trace->targetBKind))
                {
                    trace->targetBColor = targetB;
                    trace->blendApplied = TRUE;
                    objColor = NativeComposite_AlphaBlendColor(objColor, targetB,
                                                               blend->bldAlpha);
                }
                trace->finalColor = objColor;
            }
            else
            {
                trace->finalColor = objColor;
            }
            return TRUE;
        }
    }
    trace->finalWinner = trace->bgWinner;
    trace->finalColor = bgFrame[i];
    return TRUE;
}

#endif // PLATFORM_SDL2 && LINUX64

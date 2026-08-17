#if defined(PLATFORM_SDL2) && defined(LINUX64) && LINUX64

#include "global.h"
#include "field_camera.h"
#include "platform/native_sprite_snapshot.h"

/*
 * Stage 3A OBJ snapshot module: presentation-coherent sprite/OBJ capture,
 * pre-wrap command capture, validation, and serialization (see the header for
 * the contract and timing rules).
 *
 * The command sink is the only writer of the double-buffered command frames:
 * sprite.c's OAM construction builds sPending during BuildOamBuffer, LoadOam
 * publishes it to sPublished on a successful copy, and the host OBJ snapshot
 * capture reads only sPublished. sPending and sPublished are deliberately
 * separate so a host capture never observes a half-built frame.
 */

// Subsprite dimension table mirroring sprite.c's sOamDimensions. Used only for
// the flip-adjusted subsprite offset; kept local so the sink and the OAM build
// cannot drift.
static const struct { s8 width; s8 height; } sObjDimensions[3][4] =
{
    [ST_OAM_SQUARE] =
    {
        {  8,  8 }, { 16, 16 }, { 32, 32 }, { 64, 64 },
    },
    [ST_OAM_H_RECTANGLE] =
    {
        { 16,  8 }, { 32,  8 }, { 32, 16 }, { 64, 32 },
    },
    [ST_OAM_V_RECTANGLE] =
    {
        {  8, 16 }, {  8, 32 }, { 16, 32 }, { 32, 64 },
    },
};

struct NativeObjCommandFrame
{
    u64 presentationSequence;
    bool32 valid;               // published only (Commit); pending starts FALSE
    bool32 commandOverflow;     // a bounded array was exceeded
    s32 commandCameraOriginX;   // latched at Begin (logical origin + pan)
    s32 commandCameraOriginY;
    u16 commandCount;
    struct NativeSpriteDrawCommand commands[NATIVE_SPRITE_COMMAND_MAX];
    u16 unpresentedCount;
    struct NativeSpriteDrawCommand unpresented[NATIVE_SPRITE_UNPRESENTED_MAX];
};

static HOST_DATA struct NativeObjCommandFrame sPending;
static HOST_DATA struct NativeObjCommandFrame sPublished;
static HOST_DATA u64 sPresentationSequence;   // reset to 0 on state load
static HOST_DATA u8 sSpriteToObjectEvent[MAX_SPRITES]; // spriteId -> ObjectEvent index
static HOST_DATA bool8 sEmittedSprite[MAX_SPRITES];    // sprites that reached Append

/* -------------------------------------------------------------------------
 * Pure coordinate helpers (shared with UpdateOamCoords via sprite.c so the
 * command stream and the OAM build agree by construction).
 * ---------------------------------------------------------------------- */

s32 NativeSprite_GetSignedTopLeftX(const struct Sprite *sprite)
{
    return sprite->x + sprite->x2 + sprite->centerToCornerVecX
         + (sprite->coordOffsetEnabled ? gSpriteCoordOffsetX : 0);
}

s32 NativeSprite_GetSignedTopLeftY(const struct Sprite *sprite)
{
    return sprite->y + sprite->y2 + sprite->centerToCornerVecY
         + (sprite->coordOffsetEnabled ? gSpriteCoordOffsetY : 0);
}

s32 NativeSprite_PackSignedX(s32 signedX)
{
    return signedX & 0x1FF;  // GBA x is a 9-bit field: -20 -> 492, 280 -> 280
}

s32 NativeSprite_PackSignedY(s32 signedY)
{
    return signedY & 0xFF;  // GBA y is an 8-bit field: -20 -> 236, 200 -> 200
}

s32 NativeSprite_ComputeSubspriteOffset(s8 offset, s8 dimension, bool32 flip)
{
    if (!flip)
        return offset;
    // Mirror of AddSubspritesToOamBuffer's `x = ~(x + width) + 1`, which is
    // -(offset + dimension) in two's complement.
    return -(offset + dimension);
}

// Sprite logical center (x+x2+coordOffset), i.e. the subsprite base anchor.
static s32 LogicalCenterX(const struct Sprite *sprite)
{
    return NativeSprite_GetSignedTopLeftX(sprite) - sprite->centerToCornerVecX;
}

static s32 LogicalCenterY(const struct Sprite *sprite)
{
    return NativeSprite_GetSignedTopLeftY(sprite) - sprite->centerToCornerVecY;
}

/* -------------------------------------------------------------------------
 * World-position derivation.
 * ---------------------------------------------------------------------- */

// For object-event (WORLD) sprites this mirrors the Stage-1 expanded renderer's
// GetObjectSpriteAnchor (currentCoords anchor + logical sprite delta). For
// camera-relative effects it is the latched camera origin plus the signed
// presentation top-left. Unknown placement carries no trustworthy anchor.
static void FillCommandWorldPosition(struct NativeSpriteDrawCommand *cmd,
                                     const struct Sprite *sprite,
                                     s32 signedX, s32 signedY)
{
    if (cmd->placement == NATIVE_SPRITE_PLACEMENT_WORLD)
    {
        const struct ObjectEvent *oe = &gObjectEvents[cmd->objectEventId];
        s32 worldX = oe->currentCoords.x * 16 + 8;
        s32 worldY = oe->currentCoords.y * 16 + 16 + sprite->centerToCornerVecY;
        s32 expectedScreenX = worldX - sPending.commandCameraOriginX;
        s32 expectedScreenY = worldY - sPending.commandCameraOriginY;
        s32 actualScreenX = LogicalCenterX(sprite);
        s32 actualScreenY = LogicalCenterY(sprite);

        cmd->worldX = worldX + actualScreenX - expectedScreenX;
        cmd->worldY = worldY + actualScreenY - expectedScreenY;
    }
    else if (cmd->placement == NATIVE_SPRITE_PLACEMENT_CAMERA_RELATIVE)
    {
        cmd->worldX = signedX + sPending.commandCameraOriginX;
        cmd->worldY = signedY + sPending.commandCameraOriginY;
    }
    else
    {
        cmd->worldX = 0;
        cmd->worldY = 0;
    }
}

static u8 ClassifyPlacement(const struct Sprite *sprite, u8 objectEventId)
{
    if (objectEventId != NATIVE_OBJ_NO_ID)
        return NATIVE_SPRITE_PLACEMENT_WORLD;
    if (sprite->coordOffsetEnabled)
        return NATIVE_SPRITE_PLACEMENT_CAMERA_RELATIVE;
    return NATIVE_SPRITE_PLACEMENT_UNKNOWN;
}

/* -------------------------------------------------------------------------
 * Command sink.
 * ---------------------------------------------------------------------- */

void NativeSpriteCommandSink_Begin(void)
{
    s16 cameraX;
    s16 cameraY;
    u8 i;

    sPending.commandCount = 0;
    sPending.unpresentedCount = 0;
    sPending.valid = FALSE;
    sPending.commandOverflow = FALSE;
    memset(sEmittedSprite, 0, sizeof(sEmittedSprite));

    // Latch the logical camera origin WITH the pending frame. The published
    // frame therefore always carries the origin it was built against; capture
    // never re-reads the live camera (one frame late).
    GetCameraOffsetWithPan(&cameraX, &cameraY);
    sPending.commandCameraOriginX = gSaveBlock1Ptr->pos.x * 16 + cameraX;
    sPending.commandCameraOriginY = gSaveBlock1Ptr->pos.y * 16 + cameraY;

    // Object-event -> sprite association for visibility/placement classification.
    for (i = 0; i < MAX_SPRITES; i++)
        sSpriteToObjectEvent[i] = NATIVE_OBJ_NO_ID;
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        if (gObjectEvents[i].active && gObjectEvents[i].spriteId < MAX_SPRITES)
            sSpriteToObjectEvent[gObjectEvents[i].spriteId] = i;
    }
}

void NativeSpriteCommandSink_Append(const struct Sprite *sprite, u8 oamIndex,
                                    u8 partIndex, u8 partShape, u8 partSize,
                                    u16 partTileNum, u8 partPriority)
{
    struct NativeSpriteDrawCommand *cmd;
    u8 spriteId;
    u8 objectEventId;
    bool32 subspritePart;
    const struct SubspriteTable *table;

    if (sPending.commandCount >= NATIVE_SPRITE_COMMAND_MAX)
    {
        sPending.commandOverflow = TRUE;
        return;
    }

    spriteId = (u8)(sprite - gSprites);
    if (spriteId < MAX_SPRITES)
    {
        sEmittedSprite[spriteId] = TRUE;
        objectEventId = sSpriteToObjectEvent[spriteId];
    }
    else
    {
        spriteId = NATIVE_OBJ_NO_ID;
        objectEventId = NATIVE_OBJ_NO_ID;
    }

    cmd = &sPending.commands[sPending.commandCount];
    memset(cmd, 0, sizeof(*cmd));
    cmd->commandId = sPending.commandCount;
    cmd->emissionOrdinal = sPending.commandCount;
    cmd->sourceKind = NATIVE_SPRITE_SOURCE_GSPRITE;
    cmd->spriteId = spriteId;
    cmd->objectEventId = objectEventId;
    cmd->partIndex = partIndex;
    cmd->oamIndex = oamIndex;
    cmd->placement = ClassifyPlacement(sprite, objectEventId);
    cmd->visibility = NATIVE_SPRITE_GBA_EMITTED;
    cmd->expandedEligible = (cmd->placement == NATIVE_SPRITE_PLACEMENT_WORLD
                          || cmd->placement == NATIVE_SPRITE_PLACEMENT_CAMERA_RELATIVE);

    // Signed pre-wrap top-left. For a subsprite part the position is the
    // unwrapped parent center plus the (flip-adjusted) subsprite offset; this
    // packs to exactly the value AddSubspritesToOamBuffer writes into OAM.
    subspritePart = (sprite->subspriteTables != NULL
                  && sprite->subspriteMode != SUBSPRITES_OFF);
    table = subspritePart ? &sprite->subspriteTables[sprite->subspriteTableNum] : NULL;
    if (subspritePart && table != NULL && table->subsprites != NULL
     && partIndex < table->subspriteCount)
    {
        const struct Subsprite *sub = &table->subsprites[partIndex];
        bool32 hFlip = (sprite->oam.matrixNum & ST_OAM_HFLIP) != 0;
        bool32 vFlip = (sprite->oam.matrixNum & ST_OAM_VFLIP) != 0;

        cmd->signedX = LogicalCenterX(sprite)
            + NativeSprite_ComputeSubspriteOffset(sub->x,
                sObjDimensions[sub->shape][sub->size].width, hFlip);
        cmd->signedY = LogicalCenterY(sprite)
            + NativeSprite_ComputeSubspriteOffset(sub->y,
                sObjDimensions[sub->shape][sub->size].height, vFlip);
    }
    else
    {
        cmd->signedX = NativeSprite_GetSignedTopLeftX(sprite);
        cmd->signedY = NativeSprite_GetSignedTopLeftY(sprite);
    }
    FillCommandWorldPosition(cmd, sprite, cmd->signedX, cmd->signedY);

    cmd->tileNum = partTileNum;
    cmd->shape = partShape;
    cmd->size = partSize;
    cmd->priority = partPriority;
    cmd->subpriority = sprite->subpriority;
    cmd->paletteNum = sprite->oam.paletteNum;
    cmd->bpp = sprite->oam.bpp;
    cmd->objMode = sprite->oam.objMode;
    cmd->affineMode = sprite->oam.affineMode;
    cmd->mosaic = sprite->oam.mosaic;
    cmd->flipX = (sprite->oam.matrixNum & ST_OAM_HFLIP) != 0;
    cmd->flipY = (sprite->oam.matrixNum & ST_OAM_VFLIP) != 0;

    if (sprite->oam.affineMode & ST_OAM_AFFINE_ON_MASK)
    {
        const struct OamMatrix *m = &gOamMatrices[sprite->oam.matrixNum];

        cmd->pa = m->a;
        cmd->pb = m->b;
        cmd->pc = m->c;
        cmd->pd = m->d;
    }
    sPending.commandCount++;
}

static void AppendUnpresented(u8 spriteId, u8 objectEventId, u8 visibility,
                              bool32 expandedEligible, const struct Sprite *sprite)
{
    struct NativeSpriteDrawCommand *cmd = &sPending.unpresented[sPending.unpresentedCount];

    memset(cmd, 0, sizeof(*cmd));
    cmd->commandId = sPending.unpresentedCount;
    cmd->emissionOrdinal = sPending.unpresentedCount;
    cmd->sourceKind = NATIVE_SPRITE_SOURCE_GSPRITE;
    cmd->spriteId = spriteId;
    cmd->objectEventId = objectEventId;
    cmd->partIndex = 0;
    cmd->oamIndex = NATIVE_OBJ_NO_ID;
    cmd->placement = ClassifyPlacement(sprite, objectEventId);
    cmd->visibility = visibility;
    cmd->expandedEligible = expandedEligible;
    cmd->signedX = NativeSprite_GetSignedTopLeftX(sprite);
    cmd->signedY = NativeSprite_GetSignedTopLeftY(sprite);
    FillCommandWorldPosition(cmd, sprite, cmd->signedX, cmd->signedY);
    cmd->tileNum = sprite->oam.tileNum;
    cmd->shape = sprite->oam.shape;
    cmd->size = sprite->oam.size;
    cmd->priority = sprite->oam.priority;
    cmd->subpriority = sprite->subpriority;
    cmd->paletteNum = sprite->oam.paletteNum;
    cmd->bpp = sprite->oam.bpp;
    cmd->objMode = sprite->oam.objMode;
    cmd->affineMode = sprite->oam.affineMode;
    cmd->mosaic = sprite->oam.mosaic;
    cmd->flipX = (sprite->oam.matrixNum & ST_OAM_HFLIP) != 0;
    cmd->flipY = (sprite->oam.matrixNum & ST_OAM_VFLIP) != 0;
    sPending.unpresentedCount++;
}

void NativeSpriteCommandSink_End(void)
{
    u8 i;

    // Classify every in-use sprite that the emission never reached. A hidden
    // sprite is intentionally-hidden (objectEvent.invisible), viewport-culled
    // (objectEvent.offScreen && !invisible), or hidden-unknown (reason
    // unprovable). A visible sprite with no OAM primitive was dropped by the
    // gOamLimit budget.
    for (i = 0; i < MAX_SPRITES; i++)
    {
        struct Sprite *sprite = &gSprites[i];
        u8 objectEventId;
        u8 visibility;
        bool32 expandedEligible;

        if (!sprite->inUse || sEmittedSprite[i])
            continue;
        if (sPending.unpresentedCount >= NATIVE_SPRITE_UNPRESENTED_MAX)
        {
            sPending.commandOverflow = TRUE;
            break;
        }

        objectEventId = sSpriteToObjectEvent[i];
        if (sprite->invisible)
        {
            if (objectEventId != NATIVE_OBJ_NO_ID
             && gObjectEvents[objectEventId].invisible)
            {
                visibility = NATIVE_SPRITE_INTENTIONALLY_HIDDEN;
                expandedEligible = FALSE;
            }
            else if (objectEventId != NATIVE_OBJ_NO_ID
                  && gObjectEvents[objectEventId].offScreen)
            {
                visibility = NATIVE_SPRITE_VANILLA_VIEWPORT_CULLED;
                expandedEligible = TRUE;
            }
            else
            {
                visibility = NATIVE_SPRITE_HIDDEN_UNKNOWN;
                expandedEligible = FALSE;
            }
        }
        else
        {
            visibility = NATIVE_SPRITE_OAM_LIMIT_DROPPED;
            expandedEligible = TRUE;
        }
        AppendUnpresented(i, objectEventId, visibility, expandedEligible, sprite);
    }
}

void NativeSpriteCommandSink_Commit(void)
{
    sPresentationSequence++;
    sPublished = sPending;
    sPublished.presentationSequence = sPresentationSequence;
    sPublished.valid = TRUE;
}

void NativeSpriteCommandSink_Invalidate(void)
{
    // A state load restored OAM (and gMain.oamBuffer) directly; the published
    // command frame still describes the pre-restore frame. Drop it so capture
    // falls back to raw-OAM until the next successful LoadOam commit. The
    // presentation sequence is reset together with the counter so a snapshot
    // taken after the load reports sequence 0 + valid=FALSE (the "no frame"
    // sentinel), and the next commit starts the count over -- a consumer that
    // tracks monotonicity sees the discontinuity instead of a reused number.
    sPresentationSequence = 0;
    sPending.valid = FALSE;
    sPending.commandCount = 0;
    sPending.unpresentedCount = 0;
    sPending.presentationSequence = 0;
    sPublished.valid = FALSE;
    sPublished.commandCount = 0;
    sPublished.unpresentedCount = 0;
    sPublished.presentationSequence = 0;
}

/* -------------------------------------------------------------------------
 * Capture: final OAM / OBJ VRAM / palette / registers + published command
 * frame, with per-command OAM validation and raw-OAM classification.
 * ---------------------------------------------------------------------- */

// A final OAM entry counts as a "presented primitive" when the oracle would
// attempt to composite it at a potentially visible location. Dummy filler and
// offscreen/stale entries are accounted as non-presented, never silently
// classified as raw sprites.
static bool32 IsPresentedFinalOamEntry(const struct OamData *oam)
{
    s32 x;
    s32 y;
    s32 width;
    s32 height;

    // Oracle skip conditions: prohibited shape (3) or non-affine with the
    // double-size bit set (the hardware reads it as a disable for non-affine).
    if (oam->shape == 3)
        return FALSE;
    if (!(oam->affineMode & ST_OAM_AFFINE_ON_MASK)
     && (oam->affineMode & ST_OAM_AFFINE_DOUBLE_MASK))
        return FALSE;

    // Dummy filler pattern (gDummyOamData: y=160, x=304, priority 3, 8x8).
    if (oam->y == DISPLAY_HEIGHT && oam->x == DISPLAY_WIDTH + 64
     && oam->priority == 3 && oam->shape == ST_OAM_SQUARE
     && oam->size == ST_OAM_SIZE_0)
        return FALSE;

    width = sObjDimensions[oam->shape][oam->size].width;
    height = sObjDimensions[oam->shape][oam->size].height;
    if (oam->affineMode & ST_OAM_AFFINE_DOUBLE_MASK)
    {
        width *= 2;
        height *= 2;
    }
    x = oam->x;
    if (x >= DISPLAY_WIDTH)
        x -= 512;
    y = oam->y;
    if (y >= DISPLAY_HEIGHT)
        y -= 256;

    return x < DISPLAY_WIDTH && x + width > 0
        && y < DISPLAY_HEIGHT && y + height > 0;
}

static void RawOamCommand(const struct OamData *finalOam, u8 oamIndex,
                          const struct OamData *oam,
                          struct NativeSpriteDrawCommand *cmd, u32 ordinal)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->commandId = ordinal;
    cmd->emissionOrdinal = ordinal;
    cmd->sourceKind = NATIVE_SPRITE_SOURCE_RAW_OAM;
    cmd->spriteId = NATIVE_OBJ_NO_ID;
    cmd->objectEventId = NATIVE_OBJ_NO_ID;
    cmd->partIndex = 0;
    cmd->oamIndex = oamIndex;
    cmd->placement = NATIVE_SPRITE_PLACEMENT_UNKNOWN;
    cmd->visibility = NATIVE_SPRITE_GBA_EMITTED;
    cmd->expandedEligible = FALSE;   // provenance unknown: not expandable
    cmd->signedX = oam->x;
    if (cmd->signedX >= DISPLAY_WIDTH)
        cmd->signedX -= 512;
    cmd->signedY = oam->y;
    if (cmd->signedY >= DISPLAY_HEIGHT)
        cmd->signedY -= 256;
    cmd->worldX = 0;
    cmd->worldY = 0;
    cmd->tileNum = oam->tileNum;
    cmd->shape = oam->shape;
    cmd->size = oam->size;
    cmd->priority = oam->priority;
    cmd->subpriority = 0;
    cmd->paletteNum = oam->paletteNum;
    cmd->bpp = oam->bpp;
    cmd->objMode = oam->objMode;
    cmd->affineMode = oam->affineMode;
    cmd->mosaic = oam->mosaic;
    cmd->flipX = (oam->matrixNum & ST_OAM_HFLIP) != 0;
    cmd->flipY = (oam->matrixNum & ST_OAM_VFLIP) != 0;
    if (oam->affineMode & ST_OAM_AFFINE_ON_MASK)
    {
        cmd->pa = finalOam[oam->matrixNum * 4 + 0].affineParam;
        cmd->pb = finalOam[oam->matrixNum * 4 + 1].affineParam;
        cmd->pc = finalOam[oam->matrixNum * 4 + 2].affineParam;
        cmd->pd = finalOam[oam->matrixNum * 4 + 3].affineParam;
    }
}

// Every published GSPRITE command must agree with its final OAM entry on the
// packed signed X/Y, shape/size, tileNum, priority, palette/bpp/modes/mosaic
// and the non-affine flip bits. (The affine matrix values are copied into the
// command but validated separately by the unit tests; the matrix itself does
// not live in the sprite's own OAM entry.)
static bool32 CommandMatchesOam(const struct NativeSpriteDrawCommand *cmd,
                                const struct OamData *oam)
{
    return NativeSprite_PackSignedX(cmd->signedX) == (s32)oam->x
        && NativeSprite_PackSignedY(cmd->signedY) == (s32)oam->y
        && cmd->shape == oam->shape
        && cmd->size == oam->size
        && cmd->tileNum == oam->tileNum
        && cmd->priority == oam->priority
        && cmd->paletteNum == oam->paletteNum
        && cmd->bpp == oam->bpp
        && cmd->objMode == oam->objMode
        && cmd->affineMode == oam->affineMode
        && cmd->mosaic == oam->mosaic
        && cmd->flipX == ((oam->matrixNum & ST_OAM_HFLIP) != 0)
        && cmd->flipY == ((oam->matrixNum & ST_OAM_VFLIP) != 0);
}

bool32 NativeObjSnapshot_Capture(struct NativeObjSnapshot *obj)
{
    u8 oamReferenced[OAM_ENTRY_COUNT];
    u16 i;
    u8 oamIndex;

    if (obj == NULL)
        return FALSE;

    memset(obj, 0, sizeof(*obj));
    memset(oamReferenced, 0, sizeof(oamReferenced));

    obj->schemaVersion = NATIVE_OBJ_SNAPSHOT_SCHEMA_VERSION;
    obj->presentationSequence = sPublished.presentationSequence;
    obj->valid = sPublished.valid;
    obj->oamAuthorityValid = TRUE;
    obj->dispCnt = REG_DISPCNT;
    obj->mosaic = REG_MOSAIC;
    obj->bldCnt = REG_BLDCNT;
    obj->bldAlpha = REG_BLDALPHA;
    obj->bldY = REG_BLDY;
    obj->win0H = REG_WIN0H;
    obj->win0V = REG_WIN0V;
    obj->win1H = REG_WIN1H;
    obj->win1V = REG_WIN1V;
    obj->winIn = REG_WININ;
    obj->winOut = REG_WINOUT;
    obj->logicalViewportWidth = DISPLAY_WIDTH;
    obj->logicalViewportHeight = DISPLAY_HEIGHT;
    obj->commandCameraOriginX = sPublished.commandCameraOriginX;
    obj->commandCameraOriginY = sPublished.commandCameraOriginY;

    memcpy(obj->finalOam, OAM, sizeof(obj->finalOam));
    memcpy(obj->objVram, OBJ_VRAM0, sizeof(obj->objVram));
    memcpy(obj->objPalette, OBJ_PLTT, sizeof(obj->objPalette));

    if (sPublished.valid)
    {
        for (i = 0; i < sPublished.commandCount; i++)
        {
            const struct NativeSpriteDrawCommand *src = &sPublished.commands[i];

            if (obj->commandCount >= NATIVE_SPRITE_COMMAND_MAX)
            {
                obj->commandOverflow = TRUE;
                break;
            }
            obj->commands[obj->commandCount] = *src;
            obj->gSpriteCommandCount++;
            if (src->oamIndex < OAM_ENTRY_COUNT)
            {
                oamReferenced[src->oamIndex] = TRUE;
                if (CommandMatchesOam(src, &obj->finalOam[src->oamIndex]))
                {
                    obj->commands[obj->commandCount].oamValidated = TRUE;
                    obj->validatedCommandCount++;
                }
                else
                {
                    obj->commands[obj->commandCount].oamValidated = FALSE;
                    obj->provenanceMismatchCount++;
                }
            }
            obj->commandCount++;
        }

        // Visible/unmapped final entries become RAW_OAM commands.
        for (oamIndex = 0; oamIndex < OAM_ENTRY_COUNT; oamIndex++)
        {
            if (oamReferenced[oamIndex])
                continue;
            if (IsPresentedFinalOamEntry(&obj->finalOam[oamIndex]))
            {
                if (obj->commandCount >= NATIVE_SPRITE_COMMAND_MAX)
                {
                    obj->commandOverflow = TRUE;
                    break;
                }
                RawOamCommand(obj->finalOam, oamIndex, &obj->finalOam[oamIndex],
                              &obj->commands[obj->commandCount], obj->commandCount);
                obj->commandCount++;
                obj->rawOamCount++;
            }
            else
            {
                obj->unmappedNonPresentedCount++;
            }
        }

        for (i = 0; i < sPublished.unpresentedCount; i++)
        {
            if (obj->unpresentedCount >= NATIVE_SPRITE_UNPRESENTED_MAX)
            {
                obj->commandOverflow = TRUE;
                break;
            }
            obj->unpresented[obj->unpresentedCount++] = sPublished.unpresented[i];
        }
    }
    else
    {
        // No valid published frame (e.g. a save was loaded since the last
        // LoadOam commit): every presented final OAM entry is raw-OAM-only
        // until the next successful commit.
        for (oamIndex = 0; oamIndex < OAM_ENTRY_COUNT; oamIndex++)
        {
            if (IsPresentedFinalOamEntry(&obj->finalOam[oamIndex]))
            {
                if (obj->commandCount >= NATIVE_SPRITE_COMMAND_MAX)
                {
                    obj->commandOverflow = TRUE;
                    break;
                }
                RawOamCommand(obj->finalOam, oamIndex, &obj->finalOam[oamIndex],
                              &obj->commands[obj->commandCount], obj->commandCount);
                obj->commandCount++;
                obj->rawOamCount++;
            }
            else
            {
                obj->unmappedNonPresentedCount++;
            }
        }
    }

    obj->commandOverflow = obj->commandOverflow || sPublished.commandOverflow;
    return TRUE;
}

/* -------------------------------------------------------------------------
 * Versioned deterministic serialization.
 * ---------------------------------------------------------------------- */

static void WriteLE32(u8 *p, u32 v)
{
    p[0] = (u8)(v);
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 ReadLE32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

bool32 NativeObjSnapshot_Serialize(const struct NativeObjSnapshot *obj,
                                   u8 *out, size_t capacity, size_t *written)
{
    size_t bodySize;
    size_t total;

    if (obj == NULL || out == NULL || written == NULL)
        return FALSE;
    bodySize = sizeof(*obj);
    total = NATIVE_OBJ_SERIALIZE_HEADER + bodySize;
    if (capacity < total)
        return FALSE;

    WriteLE32(out + 0, NATIVE_OBJ_SERIALIZE_MAGIC);
    WriteLE32(out + 4, NATIVE_OBJ_SNAPSHOT_SCHEMA_VERSION);
    WriteLE32(out + 8, NATIVE_OBJ_SERIALIZE_MARKER);
    WriteLE32(out + 12, (u32)bodySize);
    memcpy(out + NATIVE_OBJ_SERIALIZE_HEADER, obj, bodySize);
    *written = total;
    return TRUE;
}

bool32 NativeObjSnapshot_Deserialize(const u8 *in, size_t size,
                                     struct NativeObjSnapshot *obj)
{
    size_t bodySize;

    if (in == NULL || obj == NULL)
        return FALSE;
    if (size < NATIVE_OBJ_SERIALIZE_HEADER)
        return FALSE;
    if (ReadLE32(in + 0) != NATIVE_OBJ_SERIALIZE_MAGIC)
        return FALSE;
    if (ReadLE32(in + 4) != NATIVE_OBJ_SNAPSHOT_SCHEMA_VERSION)
        return FALSE;
    if (ReadLE32(in + 8) != NATIVE_OBJ_SERIALIZE_MARKER)
        return FALSE;
    bodySize = ReadLE32(in + 12);
    if (bodySize != sizeof(*obj))
        return FALSE;
    if (size != NATIVE_OBJ_SERIALIZE_HEADER + bodySize)
        return FALSE;
    memcpy(obj, in + NATIVE_OBJ_SERIALIZE_HEADER, bodySize);
    return TRUE;
}

#endif // PLATFORM_SDL2 && LINUX64

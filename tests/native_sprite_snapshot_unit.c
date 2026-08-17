#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/platform/native_sprite_snapshot.c"

/*
 * Stage 3A OBJ snapshot unit tests: the command sink, the pre-wrap signed
 * coordinate derivation, per-command OAM validation, raw-OAM classification,
 * presentation-sequence/commit semantics, and versioned serialization.
 *
 * The sink is driven directly (Begin/Append/End/Commit) exactly as sprite.c's
 * OAM construction calls it under LINUX64; the test simulates UpdateOamCoords +
 * AddSpriteToOamBuffer/AddSubspritesToOamBuffer by writing the packed OAM
 * entries the way the real code does, so the "agree by construction" invariant
 * (sink command -> final OAM entry) is checked numerically.
 *
 * Compile with the Makefile_pc test flags
 * (-DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 -DPLATFORM_SDL2
 *  -DNATIVE_LINUX -DLINUX64=1). Requires no game launch and no fixtures.
 */

/* ---------------- stub runtime environment ---------------- */

unsigned char PLTT[PLTT_SIZE] __attribute__((aligned(4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));
unsigned char OAM[OAM_SIZE] __attribute__((aligned(4)));
unsigned char REG_BASE[0x400] __attribute__((aligned(4)));

static struct SaveBlock1 sTestSaveBlock1;
struct SaveBlock1 *gSaveBlock1Ptr = &sTestSaveBlock1;
struct ObjectEvent gObjectEvents[OBJECT_EVENTS_COUNT];
struct Sprite gSprites[MAX_SPRITES + 1];
s16 gSpriteCoordOffsetX;
s16 gSpriteCoordOffsetY;
struct OamMatrix gOamMatrices[OAM_MATRIX_COUNT];

static s16 sTestCameraX;
static s16 sTestCameraY;

void GetCameraOffsetWithPan(s16 *x, s16 *y)
{
    *x = sTestCameraX;
    *y = sTestCameraY;
}

/* ---------------- helpers ---------------- */

static void WriteOamEntry(u8 index, const struct OamData *oam)
{
    memcpy(&OAM[index * sizeof(struct OamData)], oam, sizeof(struct OamData));
}

/* Fill all 128 OAM entries with the gDummyOamData filler pattern (y=160,
 * x=304, priority 3, 8x8): the oracle never treats these as presented. */
static void SetDummyOamAll(void)
{
    struct OamData dummy;
    u8 i;

    memset(&dummy, 0, sizeof(dummy));
    dummy.y = DISPLAY_HEIGHT;
    dummy.x = DISPLAY_WIDTH + 64;
    dummy.priority = 3;
    dummy.shape = ST_OAM_SQUARE;
    dummy.size = ST_OAM_SIZE_0;
    for (i = 0; i < OAM_ENTRY_COUNT; i++)
        WriteOamEntry(i, &dummy);
}

static void ResetStubs(void)
{
    memset(gSprites, 0, sizeof(gSprites));
    memset(gObjectEvents, 0, sizeof(gObjectEvents));
    memset(gOamMatrices, 0, sizeof(gOamMatrices));
    memset(PLTT, 0, sizeof(PLTT));
    memset(VRAM_, 0, sizeof(VRAM_));
    memset(REG_BASE, 0, sizeof(REG_BASE));
    memset(&sTestSaveBlock1, 0, sizeof(sTestSaveBlock1));
    gSpriteCoordOffsetX = 0;
    gSpriteCoordOffsetY = 0;
    sTestCameraX = 0;
    sTestCameraY = 0;
    SetDummyOamAll();
    NativeSpriteCommandSink_Invalidate();
}

/* The exact pre-wrap signed top-left formulas (the factored forms of
 * UpdateOamCoords' inline math under LINUX64). */
static s32 SignedXOf(const struct Sprite *s)
{
    return s->x + s->x2 + s->centerToCornerVecX
         + (s->coordOffsetEnabled ? gSpriteCoordOffsetX : 0);
}
static s32 SignedYOf(const struct Sprite *s)
{
    return s->y + s->y2 + s->centerToCornerVecY
         + (s->coordOffsetEnabled ? gSpriteCoordOffsetY : 0);
}

/* Simulate UpdateOamCoords wrapping the pre-wrap top-left into the 9-bit/8-bit
 * OAM bitfields. */
static void PackSpriteOam(struct Sprite *s)
{
    s->oam.x = (u32)(SignedXOf(s) & 0x1FF);
    s->oam.y = (u32)(SignedYOf(s) & 0xFF);
}

/* ---------------- pure coordinate helpers ---------------- */

static void TestSignedPacking(void)
{
    assert(NativeSprite_PackSignedX(-20) == 492);
    assert(NativeSprite_PackSignedX(280) == 280);
    assert(NativeSprite_PackSignedX(511) == 511);
    assert(NativeSprite_PackSignedX(-512) == 0);
    assert(NativeSprite_PackSignedY(-20) == 236);
    assert(NativeSprite_PackSignedY(200) == 200);
    assert(NativeSprite_PackSignedY(-256) == 0);

    assert(NativeSprite_ComputeSubspriteOffset(4, 8, FALSE) == 4);
    assert(NativeSprite_ComputeSubspriteOffset(4, 8, TRUE) == -12);
    assert(NativeSprite_ComputeSubspriteOffset(-8, 8, TRUE) == 0);
    assert(NativeSprite_ComputeSubspriteOffset(0, 16, TRUE) == -16);
    assert(NativeSprite_ComputeSubspriteOffset(-16, 8, TRUE) == 8);
}

static void TestSignedTopLeftMath(void)
{
    struct Sprite s;

    memset(&s, 0, sizeof(s));
    s.x = 10;
    s.x2 = 5;
    s.centerToCornerVecX = -8;
    s.coordOffsetEnabled = FALSE;
    assert(NativeSprite_GetSignedTopLeftX(&s) == 7); // 10 + 5 - 8
    gSpriteCoordOffsetX = 13;
    s.coordOffsetEnabled = TRUE;
    assert(NativeSprite_GetSignedTopLeftX(&s) == 20); // 10 + 5 - 8 + 13
    gSpriteCoordOffsetX = 0;
    s.coordOffsetEnabled = FALSE;

    s.y = 3;
    s.y2 = 1;
    s.centerToCornerVecY = -4;
    assert(NativeSprite_GetSignedTopLeftY(&s) == 0);  // 3 + 1 - 4
    s.centerToCornerVecY = 4;
    assert(NativeSprite_GetSignedTopLeftY(&s) == 8);  // 3 + 1 + 4

    // Negative pre-wrap values must pack to the hardware wrap form.
    s.x = -20;
    s.x2 = 0;
    s.centerToCornerVecX = 0;
    s.y = 200;
    s.y2 = 0;
    s.centerToCornerVecY = 0;
    assert(NativeSprite_PackSignedX(NativeSprite_GetSignedTopLeftX(&s)) == 492);
    assert(NativeSprite_PackSignedY(NativeSprite_GetSignedTopLeftY(&s)) == 200);
}

/* ---------------- emission, ordering, placement, validation ---------------- */

static void TestMultiEmissionOrderAndValidation(void)
{
    struct Sprite *s0 = &gSprites[0];
    struct Sprite *s1 = &gSprites[1];
    struct Sprite *s2 = &gSprites[2];
    struct NativeObjSnapshot obj;
    s32 cameraOriginX;
    s32 cameraOriginY;

    ResetStubs();
    // Camera origin: player at metatile (100,200), no pan.
    sTestSaveBlock1.pos.x = 100;
    sTestSaveBlock1.pos.y = 200;
    sTestCameraX = 0;
    sTestCameraY = 0;
    cameraOriginX = 100 * 16 + 0;
    cameraOriginY = 200 * 16 + 0;

    // Object-event owned sprite at logical top-left (20,30), 16x16.
    memset(s0, 0, sizeof(*s0));
    s0->inUse = TRUE;
    s0->x = 20;
    s0->y = 30;
    s0->oam.shape = ST_OAM_SQUARE;
    s0->oam.size = ST_OAM_SIZE_1;   // 16x16
    s0->oam.tileNum = 100;
    s0->oam.priority = 1;
    s0->oam.paletteNum = 5;
    gObjectEvents[3].active = TRUE;
    gObjectEvents[3].spriteId = 0;
    gObjectEvents[3].currentCoords.x = 10;
    gObjectEvents[3].currentCoords.y = 20;

    // Camera-relative effect (coordOffsetEnabled), 16x16 at logical center (40,50).
    memset(s1, 0, sizeof(*s1));
    s1->inUse = TRUE;
    s1->x = 40;
    s1->y = 50;
    s1->centerToCornerVecX = -8;
    s1->centerToCornerVecY = -8;
    s1->coordOffsetEnabled = TRUE;
    s1->oam.shape = ST_OAM_SQUARE;
    s1->oam.size = ST_OAM_SIZE_1;
    s1->oam.tileNum = 10;
    s1->oam.priority = 2;
    gSpriteCoordOffsetX = 5;
    gSpriteCoordOffsetY = 7;

    // Plain sprite (no object event, no coord offset), 8x8 at (200,90).
    memset(s2, 0, sizeof(*s2));
    s2->inUse = TRUE;
    s2->x = 200;
    s2->y = 90;
    s2->oam.shape = ST_OAM_SQUARE;
    s2->oam.size = ST_OAM_SIZE_0;
    s2->oam.tileNum = 7;
    s2->oam.priority = 0;

    // Simulate the OAM build: pack + write each sprite to OAM, then Append.
    PackSpriteOam(s0);
    WriteOamEntry(0, &s0->oam);
    PackSpriteOam(s1);
    WriteOamEntry(1, &s1->oam);
    PackSpriteOam(s2);
    WriteOamEntry(2, &s2->oam);

    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(s0, 0, 0, s0->oam.shape, s0->oam.size,
                                   s0->oam.tileNum, s0->oam.priority);
    NativeSpriteCommandSink_Append(s1, 1, 0, s1->oam.shape, s1->oam.size,
                                   s1->oam.tileNum, s1->oam.priority);
    NativeSpriteCommandSink_Append(s2, 2, 0, s2->oam.shape, s2->oam.size,
                                   s2->oam.tileNum, s2->oam.priority);
    NativeSpriteCommandSink_End();
    NativeSpriteCommandSink_Commit();

    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.valid == TRUE);
    assert(obj.presentationSequence == 1);
    assert(obj.oamAuthorityValid == TRUE);
    assert(obj.commandCount == 3);
    assert(obj.gSpriteCommandCount == 3);
    assert(obj.validatedCommandCount == 3);
    assert(obj.provenanceMismatchCount == 0);
    assert(obj.rawOamCount == 0);
    assert(obj.unmappedNonPresentedCount == 125); // remaining dummy entries
    assert(obj.commandOverflow == FALSE);
    assert(obj.commandCameraOriginX == cameraOriginX);
    assert(obj.commandCameraOriginY == cameraOriginY);

    // Deterministic identity + order invariants.
    assert(obj.commands[0].commandId == 0);
    assert(obj.commands[0].emissionOrdinal == 0);
    assert(obj.commands[0].oamIndex == 0);
    assert(obj.commands[1].commandId == 1);
    assert(obj.commands[1].emissionOrdinal == 1);
    assert(obj.commands[1].oamIndex == 1);
    assert(obj.commands[2].commandId == 2);
    assert(obj.commands[2].emissionOrdinal == 2);
    assert(obj.commands[2].oamIndex == 2);

    // World-placement classifications + expanded eligibility.
    assert(obj.commands[0].placement == NATIVE_SPRITE_PLACEMENT_WORLD);
    assert(obj.commands[0].objectEventId == 3);
    assert(obj.commands[0].expandedEligible == TRUE);
    assert(obj.commands[1].placement == NATIVE_SPRITE_PLACEMENT_CAMERA_RELATIVE);
    assert(obj.commands[1].objectEventId == NATIVE_OBJ_NO_ID);
    assert(obj.commands[1].expandedEligible == TRUE);
    assert(obj.commands[2].placement == NATIVE_SPRITE_PLACEMENT_UNKNOWN);
    assert(obj.commands[2].objectEventId == NATIVE_OBJ_NO_ID);
    assert(obj.commands[2].expandedEligible == FALSE);

    // Pre-wrap signed positions.
    assert(obj.commands[0].signedX == 20);
    assert(obj.commands[0].signedY == 30);
    // s1: signedX = 40 + 5 - 8 = 37, signedY = 50 + 7 - 8 = 49.
    assert(obj.commands[1].signedX == 37);
    assert(obj.commands[1].signedY == 49);
    assert(obj.commands[2].signedX == 200);
    assert(obj.commands[2].signedY == 90);

    // World anchors.
    // s0 (WORLD): anchor=(10*16+8, 20*16+16), expected screen = anchor - origin,
    // actual logical center = signed + centerToCornerVec.
    assert(obj.commands[0].worldX == (10 * 16 + 8) + (20 - ((10 * 16 + 8) - cameraOriginX)));
    assert(obj.commands[0].worldY == (20 * 16 + 16) + (30 - ((20 * 16 + 16) - cameraOriginY)));
    // s1 (CAMERA_RELATIVE): signed top-left + latched origin.
    assert(obj.commands[1].worldX == 37 + cameraOriginX);
    assert(obj.commands[1].worldY == 49 + cameraOriginY);
    // s2 (UNKNOWN): no trustworthy anchor.
    assert(obj.commands[2].worldX == 0);
    assert(obj.commands[2].worldY == 0);

    // Per-command OAM validation against the packed final OAM entries.
    assert(obj.commands[0].oamValidated == TRUE);
    assert(obj.commands[1].oamValidated == TRUE);
    assert(obj.commands[2].oamValidated == TRUE);
    assert(obj.finalOam[0].x == NativeSprite_PackSignedX(20));
    assert(obj.finalOam[0].y == NativeSprite_PackSignedY(30));
    assert(obj.finalOam[1].x == NativeSprite_PackSignedX(37));
    assert(obj.finalOam[2].x == NativeSprite_PackSignedX(200));

    // Attribute copies.
    assert(obj.commands[0].tileNum == 100);
    assert(obj.commands[0].shape == ST_OAM_SQUARE);
    assert(obj.commands[0].size == ST_OAM_SIZE_1);
    assert(obj.commands[0].priority == 1);
    assert(obj.commands[0].paletteNum == 5);
    assert(obj.commands[0].affineMode == ST_OAM_AFFINE_OFF);
    assert(obj.commands[0].flipX == FALSE);
    assert(obj.commands[0].flipY == FALSE);

    printf("ok: multi-emission order + placement + OAM validation\n");
}

static void TestRegistersAndMemCopy(void)
{
    struct NativeObjSnapshot obj;

    ResetStubs();
    REG_DISPCNT = 0x1140;
    REG_MOSAIC = 0x00FF;
    REG_BLDCNT = 0x3F42;
    REG_BLDALPHA = 0x0510;
    REG_BLDY = 0x10;
    REG_WIN0H = 0x1213;
    REG_WIN0V = 0x2122;
    REG_WIN1H = 0x3132;
    REG_WIN1V = 0x4142;
    REG_WININ = 0x3355;
    REG_WINOUT = 0x3344;
    *(u16 *)&PLTT[BG_PLTT_SIZE] = 0x7FFF;      // OBJ palette entry 0
    VRAM_[0x10000] = 0xAB;                     // OBJ VRAM tile byte 0
    VRAM_[0x17FFF] = 0xCD;                     // last OBJ VRAM byte

    NativeObjSnapshot_Capture(&obj);
    assert(obj.dispCnt == 0x1140);
    assert(obj.mosaic == 0x00FF);
    assert(obj.bldCnt == 0x3F42);
    assert(obj.bldAlpha == 0x0510);
    assert(obj.bldY == 0x10);
    assert(obj.win0H == 0x1213);
    assert(obj.win0V == 0x2122);
    assert(obj.win1H == 0x3132);
    assert(obj.win1V == 0x4142);
    assert(obj.winIn == 0x3355);
    assert(obj.winOut == 0x3344);
    assert(obj.objPalette[0] == 0x7FFF);
    assert(obj.objVram[0] == 0xAB);
    assert(obj.objVram[OBJ_VRAM0_SIZE - 1] == 0xCD);
    assert(obj.logicalViewportWidth == DISPLAY_WIDTH);
    assert(obj.logicalViewportHeight == DISPLAY_HEIGHT);

    printf("ok: register + OBJ VRAM + OBJ palette capture\n");
}

/* ---------------- subsprite emission ---------------- */

static void TestSubspriteEmission(void)
{
    struct Sprite *sprite = &gSprites[0];
    struct Subsprite parts[2];
    struct SubspriteTable table;
    struct NativeObjSnapshot obj;
    s32 centerX;
    s32 centerY;
    s16 cameraOriginX;
    s16 cameraOriginY;

    ResetStubs();
    sTestSaveBlock1.pos.x = 50;
    sTestSaveBlock1.pos.y = 60;
    sTestCameraX = 2;
    sTestCameraY = 3;
    cameraOriginX = 50 * 16 + 2;
    cameraOriginY = 60 * 16 + 3;

    // 32x32 base sprite, logical center at (100,50): centerToCornerVec = -16.
    memset(sprite, 0, sizeof(*sprite));
    sprite->inUse = TRUE;
    sprite->x = 100;
    sprite->y = 50;
    sprite->centerToCornerVecX = -16;
    sprite->centerToCornerVecY = -16;
    sprite->oam.tileNum = 32;
    sprite->oam.priority = 3;
    sprite->oam.shape = ST_OAM_SQUARE;
    sprite->oam.size = ST_OAM_SIZE_2;   // 32x32
    sprite->subspriteTables = &table;
    sprite->subspriteTableNum = 0;
    sprite->subspriteMode = SUBSPRITES_ON;
    // Non-affine flips (matrixNum bits 3/4).
    sprite->oam.matrixNum = ST_OAM_HFLIP | ST_OAM_VFLIP;

    // part 0: offset (4,-8), 16x16, tileOffset 0, priority 1 (override).
    parts[0].x = 4;
    parts[0].y = -8;
    parts[0].shape = ST_OAM_SQUARE;
    parts[0].size = ST_OAM_SIZE_1;
    parts[0].tileOffset = 0;
    parts[0].priority = 1;
    // part 1: offset (-16,4), 8x8, tileOffset 8, priority 2 (override).
    parts[1].x = -16;
    parts[1].y = 4;
    parts[1].shape = ST_OAM_SQUARE;
    parts[1].size = ST_OAM_SIZE_0;
    parts[1].tileOffset = 8;
    parts[1].priority = 2;
    table.subspriteCount = 2;
    table.subsprites = parts;

    // Emulate UpdateOamCoords for the base sprite, then AddSubspritesToOamBuffer:
    // baseX/baseY = wrapped top-left - centerToCornerVec = logical center.
    PackSpriteOam(sprite);
    centerX = 100;
    centerY = 50;

    // Base sprite is hFlip|vFlip (matrixNum 0x18), so EVERY part is flipped and
    // the offset is negated against the part's OWN dimension:
    //   hFlip: x = ~(sub.x + partWidth) + 1   vFlip: y = ~(sub.y + partHeight) + 1
    // part0 (16x16, offset (4,-8)): x = -(4+16) = -20, y = -(-8+16) = -8.
    {
        struct OamData o;
        memset(&o, 0, sizeof(o));
        o = sprite->oam;
        o.shape = parts[0].shape;
        o.size = parts[0].size;
        o.x = (u32)((s16)centerX + (s16)(-(4 + 16))) & 0x1FF;
        o.y = (u32)(centerY + (-(-8 + 16))) & 0xFF;
        o.tileNum = 32 + parts[0].tileOffset;
        o.priority = parts[0].priority;
        WriteOamEntry(0, &o);
    }
    // part1 (8x8, offset (-16,4)): x = -(-16+8) = 8, y = -(4+8) = -12.
    {
        struct OamData o;
        memset(&o, 0, sizeof(o));
        o = sprite->oam;
        o.shape = parts[1].shape;
        o.size = parts[1].size;
        o.x = (u32)((s16)centerX + (s16)8) & 0x1FF;
        o.y = (u32)(centerY + (-12)) & 0xFF;
        o.tileNum = 32 + parts[1].tileOffset;
        o.priority = parts[1].priority;
        WriteOamEntry(1, &o);
    }

    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(sprite, 0, 0, parts[0].shape, parts[0].size,
                                   32 + parts[0].tileOffset, parts[0].priority);
    NativeSpriteCommandSink_Append(sprite, 1, 1, parts[1].shape, parts[1].size,
                                   32 + parts[1].tileOffset, parts[1].priority);
    NativeSpriteCommandSink_End();
    NativeSpriteCommandSink_Commit();

    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.commandCount == 2);
    assert(obj.validatedCommandCount == 2);
    assert(obj.commands[0].partIndex == 0);
    assert(obj.commands[1].partIndex == 1);
    assert(obj.commands[0].spriteId == 0);
    assert(obj.commands[1].spriteId == 0);

    // Signed pre-wrap top-lefts (flip-adjusted subsprite offsets).
    assert(obj.commands[0].signedX == centerX - 20);  // hFlip: -(4+16)
    assert(obj.commands[0].signedY == centerY - 8);   // vFlip: -(-8+16)
    assert(obj.commands[1].signedX == centerX + 8);   // hFlip: -(-16+8)
    assert(obj.commands[1].signedY == centerY - 12);  // vFlip: -(4+8)

    // Packed values must equal the OAM entries AddSubspritesToOamBuffer wrote.
    assert(obj.finalOam[0].x == NativeSprite_PackSignedX(centerX - 20));
    assert(obj.finalOam[0].y == NativeSprite_PackSignedY(centerY - 8));
    assert(obj.finalOam[1].x == NativeSprite_PackSignedX(centerX + 8));
    assert(obj.finalOam[1].y == NativeSprite_PackSignedY(centerY - 12));

    // Effective per-part attributes: tile offsets + priority overrides.
    assert(obj.commands[0].tileNum == 32);
    assert(obj.commands[0].priority == 1);
    assert(obj.commands[0].shape == ST_OAM_SQUARE);
    assert(obj.commands[0].size == ST_OAM_SIZE_1);
    assert(obj.commands[1].tileNum == 40);
    assert(obj.commands[1].priority == 2);
    assert(obj.commands[1].shape == ST_OAM_SQUARE);
    assert(obj.commands[1].size == ST_OAM_SIZE_0);

    // Flip bits from the non-affine matrixNum.
    assert(obj.commands[0].flipX == TRUE);
    assert(obj.commands[0].flipY == TRUE);
    assert(obj.commands[1].flipX == TRUE);
    assert(obj.commands[1].flipY == TRUE);

    // World anchor (CAMERA_RELATIVE? no -- this sprite has no object event and
    // coordOffsetEnabled is FALSE, so UNKNOWN; world is 0).
    assert(obj.commands[0].placement == NATIVE_SPRITE_PLACEMENT_UNKNOWN);
    assert(obj.commands[0].worldX == 0);
    assert(obj.commands[1].worldX == 0);

    printf("ok: subsprite emission (flips, tile offsets, per-part priority)\n");
}

/* ---------------- affine matrix copy + reuse ---------------- */

static void TestAffineMatrixCopy(void)
{
    struct Sprite *s0 = &gSprites[0];
    struct Sprite *s1 = &gSprites[1];
    struct NativeObjSnapshot obj;

    ResetStubs();
    memset(s0, 0, sizeof(*s0));
    s0->inUse = TRUE;
    s0->x = 100;
    s0->y = 100;
    s0->oam.shape = ST_OAM_SQUARE;
    s0->oam.size = ST_OAM_SIZE_1;
    s0->oam.tileNum = 4;
    s0->oam.affineMode = ST_OAM_AFFINE_NORMAL;
    s0->oam.matrixNum = 3;
    gOamMatrices[3].a = 0x0100;
    gOamMatrices[3].b = 1;
    gOamMatrices[3].c = -2;
    gOamMatrices[3].d = 0x01FF;

    memset(s1, 0, sizeof(*s1));
    s1->inUse = TRUE;
    s1->x = 200;
    s1->y = 120;
    s1->oam.shape = ST_OAM_SQUARE;
    s1->oam.size = ST_OAM_SIZE_0;
    s1->oam.tileNum = 8;
    s1->oam.affineMode = ST_OAM_AFFINE_NORMAL;
    s1->oam.matrixNum = 3; // shares matrix slot 3

    PackSpriteOam(s0);
    WriteOamEntry(0, &s0->oam);
    PackSpriteOam(s1);
    WriteOamEntry(1, &s1->oam);

    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(s0, 0, 0, s0->oam.shape, s0->oam.size,
                                   s0->oam.tileNum, s0->oam.priority);
    NativeSpriteCommandSink_Append(s1, 1, 0, s1->oam.shape, s1->oam.size,
                                   s1->oam.tileNum, s1->oam.priority);
    NativeSpriteCommandSink_End();
    NativeSpriteCommandSink_Commit();

    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.commands[0].affineMode == ST_OAM_AFFINE_NORMAL);
    assert(obj.commands[0].pa == 0x0100);
    assert(obj.commands[0].pb == 1);
    assert(obj.commands[0].pc == -2);
    assert(obj.commands[0].pd == 0x01FF);
    // Matrix reuse: both commands carry the same copied values.
    assert(obj.commands[1].pa == obj.commands[0].pa);
    assert(obj.commands[1].pb == obj.commands[0].pb);
    assert(obj.commands[1].pc == obj.commands[0].pc);
    assert(obj.commands[1].pd == obj.commands[0].pd);
    assert(obj.commands[1].flipX == FALSE);  // matrixNum 3: bits 3/4 clear
    assert(obj.commands[1].flipY == FALSE);
    // Validation is against the entry's own fields, not the matrix values.
    assert(obj.commands[0].oamValidated == TRUE);
    assert(obj.commands[1].oamValidated == TRUE);

    printf("ok: affine matrix copy + matrix reuse\n");
}

/* ---------------- presentation semantics ---------------- */

static void TestPendingNotPublishedWithoutCommit(void)
{
    struct Sprite *sprite = &gSprites[0];
    struct NativeObjSnapshot obj;

    ResetStubs();
    memset(sprite, 0, sizeof(*sprite));
    sprite->inUse = TRUE;
    sprite->x = 50;
    sprite->y = 50;
    sprite->oam.shape = ST_OAM_SQUARE;
    sprite->oam.size = ST_OAM_SIZE_0;
    PackSpriteOam(sprite);
    WriteOamEntry(0, &sprite->oam);

    // Build a pending frame but never commit (the LoadOam copy was blocked).
    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(sprite, 0, 0, sprite->oam.shape,
                                   sprite->oam.size, sprite->oam.tileNum,
                                   sprite->oam.priority);
    NativeSpriteCommandSink_End();

    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.valid == FALSE);
    assert(obj.presentationSequence == 0);
    assert(obj.gSpriteCommandCount == 0);      // no GSPRITE provenance leaked
    assert(obj.rawOamCount == 1);              // OAM[0] is raw-OAM-only fallback
    assert(obj.commandCount == 1);
    assert(obj.commands[0].sourceKind == NATIVE_SPRITE_SOURCE_RAW_OAM);
    assert(obj.commands[0].oamIndex == 0);

    printf("ok: pending command stream not published before LoadOam commit\n");
}

static void TestOamLoadDisabledLeavesPublishedUnchanged(void)
{
    struct Sprite *sprite = &gSprites[0];
    struct Sprite *other = &gSprites[1];
    struct NativeObjSnapshot obj;

    ResetStubs();

    // Frame A: commit sprite 0 at OAM[0].
    memset(sprite, 0, sizeof(*sprite));
    sprite->inUse = TRUE;
    sprite->x = 50;
    sprite->y = 50;
    sprite->oam.shape = ST_OAM_SQUARE;
    sprite->oam.size = ST_OAM_SIZE_0;
    sprite->oam.tileNum = 3;
    PackSpriteOam(sprite);
    WriteOamEntry(0, &sprite->oam);
    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(sprite, 0, 0, sprite->oam.shape,
                                   sprite->oam.size, sprite->oam.tileNum,
                                   sprite->oam.priority);
    NativeSpriteCommandSink_End();
    NativeSpriteCommandSink_Commit();

    // Frame B: oamLoadDisabled prevents the OAM copy, so LoadOam does not run.
    // Build a DIFFERENT pending frame (other sprite at OAM[5]) but no commit.
    // OAM still holds frame A's content.
    memset(other, 0, sizeof(*other));
    other->inUse = TRUE;
    other->x = 120;
    other->y = 80;
    other->oam.shape = ST_OAM_SQUARE;
    other->oam.size = ST_OAM_SIZE_1;
    other->oam.tileNum = 9;
    PackSpriteOam(other);
    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(other, 5, 0, other->oam.shape, other->oam.size,
                                   other->oam.tileNum, other->oam.priority);
    NativeSpriteCommandSink_End();

    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.valid == TRUE);
    assert(obj.presentationSequence == 1); // unchanged: frame B was not committed
    assert(obj.commandCount == 1);
    assert(obj.gSpriteCommandCount == 1);
    assert(obj.validatedCommandCount == 1);
    assert(obj.commands[0].oamIndex == 0); // still frame A's command
    assert(obj.commands[0].spriteId == 0);
    assert(obj.commands[0].tileNum == 3);

    printf("ok: oamLoadDisabled leaves the presented command frame unchanged\n");
}

static void TestCommitIncrementsSequence(void)
{
    struct Sprite *sprite = &gSprites[0];
    struct NativeObjSnapshot obj;

    ResetStubs();
    memset(sprite, 0, sizeof(*sprite));
    sprite->inUse = TRUE;
    sprite->x = 10;
    sprite->y = 10;
    sprite->oam.shape = ST_OAM_SQUARE;
    sprite->oam.size = ST_OAM_SIZE_0;
    PackSpriteOam(sprite);
    WriteOamEntry(0, &sprite->oam);

    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(sprite, 0, 0, sprite->oam.shape, sprite->oam.size,
                                   sprite->oam.tileNum, sprite->oam.priority);
    NativeSpriteCommandSink_End();
    NativeSpriteCommandSink_Commit();

    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.presentationSequence == 1);

    // Second commit: same pending re-emitted, sequence advances.
    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(sprite, 0, 0, sprite->oam.shape, sprite->oam.size,
                                   sprite->oam.tileNum, sprite->oam.priority);
    NativeSpriteCommandSink_End();
    NativeSpriteCommandSink_Commit();

    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.presentationSequence == 2);

    printf("ok: presentation sequence increments per LoadOam commit\n");
}

/* ---------------- visibility classification (End) ---------------- */

static void TestVisibilityClassification(void)
{
    struct Sprite *s0 = &gSprites[0]; // emitted object-event sprite
    struct Sprite *s1 = &gSprites[1]; // intentionally hidden (objectEvent.invisible)
    struct Sprite *s2 = &gSprites[2]; // viewport-culled (objectEvent.offScreen)
    struct Sprite *s3 = &gSprites[3]; // hidden, reason unprovable
    struct Sprite *s4 = &gSprites[4]; // visible but not emitted (OAM budget)
    struct NativeObjSnapshot obj;

    ResetStubs();

    memset(s0, 0, sizeof(*s0));
    s0->inUse = TRUE;
    s0->x = 40;
    s0->y = 40;
    s0->oam.shape = ST_OAM_SQUARE;
    s0->oam.size = ST_OAM_SIZE_1;
    s0->oam.tileNum = 1;
    gObjectEvents[0].active = TRUE;
    gObjectEvents[0].spriteId = 0;

    memset(s1, 0, sizeof(*s1));
    s1->inUse = TRUE;
    s1->invisible = TRUE;
    s1->x = 10;
    s1->y = 10;
    gObjectEvents[1].active = TRUE;
    gObjectEvents[1].spriteId = 1;
    gObjectEvents[1].invisible = TRUE;

    memset(s2, 0, sizeof(*s2));
    s2->inUse = TRUE;
    s2->invisible = TRUE;
    s2->x = 10;
    s2->y = 10;
    gObjectEvents[2].active = TRUE;
    gObjectEvents[2].spriteId = 2;
    gObjectEvents[2].offScreen = TRUE;

    memset(s3, 0, sizeof(*s3));
    s3->inUse = TRUE;
    s3->invisible = TRUE;
    s3->x = 10;
    s3->y = 10;

    memset(s4, 0, sizeof(*s4));
    s4->inUse = TRUE;
    s4->x = 220;
    s4->y = 150;
    s4->oam.shape = ST_OAM_SQUARE;
    s4->oam.size = ST_OAM_SIZE_0;

    PackSpriteOam(s0);
    WriteOamEntry(0, &s0->oam);
    PackSpriteOam(s4);
    WriteOamEntry(1, &s4->oam);

    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(s0, 0, 0, s0->oam.shape, s0->oam.size,
                                   s0->oam.tileNum, s0->oam.priority);
    // s4 is NOT emitted (simulating an OAM-budget drop).
    NativeSpriteCommandSink_End();
    NativeSpriteCommandSink_Commit();

    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.unpresentedCount == 4);
    assert(obj.commands[0].visibility == NATIVE_SPRITE_GBA_EMITTED);
    assert(obj.commands[0].placement == NATIVE_SPRITE_PLACEMENT_WORLD);
    assert(obj.commands[0].expandedEligible == TRUE);

    assert(obj.unpresented[0].visibility == NATIVE_SPRITE_INTENTIONALLY_HIDDEN);
    assert(obj.unpresented[0].objectEventId == 1);
    assert(obj.unpresented[0].placement == NATIVE_SPRITE_PLACEMENT_WORLD);
    assert(obj.unpresented[0].expandedEligible == FALSE);

    assert(obj.unpresented[1].visibility == NATIVE_SPRITE_VANILLA_VIEWPORT_CULLED);
    assert(obj.unpresented[1].objectEventId == 2);
    assert(obj.unpresented[1].placement == NATIVE_SPRITE_PLACEMENT_WORLD);
    assert(obj.unpresented[1].expandedEligible == TRUE);

    assert(obj.unpresented[2].visibility == NATIVE_SPRITE_HIDDEN_UNKNOWN);
    assert(obj.unpresented[2].objectEventId == NATIVE_OBJ_NO_ID);
    assert(obj.unpresented[2].expandedEligible == FALSE);

    assert(obj.unpresented[3].visibility == NATIVE_SPRITE_OAM_LIMIT_DROPPED);
    assert(obj.unpresented[3].objectEventId == NATIVE_OBJ_NO_ID);
    assert(obj.unpresented[3].expandedEligible == TRUE);

    printf("ok: visibility classification (hidden / culled / unknown / budget)\n");
}

/* ---------------- raw final-OAM detection ---------------- */

static void TestRawOamDetection(void)
{
    struct OamData visible;
    struct NativeObjSnapshot obj;

    ResetStubs();

    // A visible 16x16 OBJ at (100,100) with no command behind it.
    memset(&visible, 0, sizeof(visible));
    visible.y = 100;
    visible.x = 100;
    visible.shape = ST_OAM_SQUARE;
    visible.size = ST_OAM_SIZE_1;
    visible.tileNum = 50;
    visible.priority = 2;
    WriteOamEntry(0, &visible);

    NativeObjSnapshot_Capture(&obj);
    assert(obj.valid == FALSE); // no published command frame
    assert(obj.commandCount == 1);
    assert(obj.rawOamCount == 1);
    assert(obj.commands[0].sourceKind == NATIVE_SPRITE_SOURCE_RAW_OAM);
    assert(obj.commands[0].oamIndex == 0);
    assert(obj.commands[0].signedX == 100);
    assert(obj.commands[0].signedY == 100);
    assert(obj.commands[0].placement == NATIVE_SPRITE_PLACEMENT_UNKNOWN);
    assert(obj.commands[0].expandedEligible == FALSE);
    assert(obj.commands[0].tileNum == 50);
    assert(obj.commands[0].priority == 2);
    assert(obj.unmappedNonPresentedCount == 127); // the 127 dummy entries

    printf("ok: visible/unmapped final OAM entry classified as raw-OAM\n");
}

static void TestRawOamWrapAndOffscreen(void)
{
    struct OamData wrapped;
    struct NativeObjSnapshot obj;
    u8 i;

    ResetStubs();

    // A 32x32 OBJ whose packed x=488 means signed x=-24: still visible
    // (-24+32 = 8 > 0), so it is a presented raw entry and the signed top-left
    // must be canonicalized to -24, not 488.
    memset(&wrapped, 0, sizeof(wrapped));
    wrapped.y = 100;
    wrapped.x = 488; // == -24
    wrapped.shape = ST_OAM_SQUARE;
    wrapped.size = ST_OAM_SIZE_2;
    WriteOamEntry(0, &wrapped);

    NativeObjSnapshot_Capture(&obj);
    assert(obj.rawOamCount == 1);
    assert(obj.commands[0].signedX == -24);

    // An 8x8 OBJ packed at x=240 (== -272 canonicalized) is fully offscreen and
    // must NOT be classified as presented (accounted as non-presented).
    memset(&wrapped, 0, sizeof(wrapped));
    wrapped.y = 100;
    wrapped.x = 240;
    wrapped.shape = ST_OAM_SQUARE;
    wrapped.size = ST_OAM_SIZE_0;
    WriteOamEntry(0, &wrapped);
    NativeObjSnapshot_Capture(&obj);
    assert(obj.rawOamCount == 0);
    assert(obj.unmappedNonPresentedCount == 128);

    printf("ok: raw-OAM wrap canonicalization + offscreen accounting\n");
}

/* ---------------- provenance mismatch ---------------- */

static void TestProvenanceMismatch(void)
{
    struct Sprite *sprite = &gSprites[0];
    struct NativeObjSnapshot obj;

    ResetStubs();
    memset(sprite, 0, sizeof(*sprite));
    sprite->inUse = TRUE;
    sprite->x = 20;
    sprite->y = 20;
    sprite->oam.shape = ST_OAM_SQUARE;
    sprite->oam.size = ST_OAM_SIZE_0;
    sprite->oam.tileNum = 7;
    PackSpriteOam(sprite);

    // Drift: the final OAM entry does not match what the command claims.
    {
        struct OamData drifted;
        memset(&drifted, 0, sizeof(drifted));
        drifted = sprite->oam;
        drifted.x = 21; // command says 20
        WriteOamEntry(0, &drifted);
    }

    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(sprite, 0, 0, sprite->oam.shape, sprite->oam.size,
                                   sprite->oam.tileNum, sprite->oam.priority);
    NativeSpriteCommandSink_End();
    NativeSpriteCommandSink_Commit();

    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.validatedCommandCount == 0);
    assert(obj.provenanceMismatchCount == 1);
    assert(obj.commands[0].oamValidated == FALSE);
    assert(obj.commands[0].sourceKind == NATIVE_SPRITE_SOURCE_GSPRITE);

    printf("ok: command/OAM provenance mismatch counted, not silently omitted\n");
}

/* ---------------- stale-state invalidation ---------------- */

static void TestInvalidateForcesRawOamFallback(void)
{
    struct Sprite *sprite = &gSprites[0];
    struct NativeObjSnapshot obj;

    ResetStubs();
    memset(sprite, 0, sizeof(*sprite));
    sprite->inUse = TRUE;
    sprite->x = 30;
    sprite->y = 30;
    sprite->oam.shape = ST_OAM_SQUARE;
    sprite->oam.size = ST_OAM_SIZE_0;
    PackSpriteOam(sprite);
    WriteOamEntry(0, &sprite->oam);

    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(sprite, 0, 0, sprite->oam.shape, sprite->oam.size,
                                   sprite->oam.tileNum, sprite->oam.priority);
    NativeSpriteCommandSink_End();
    NativeSpriteCommandSink_Commit();

    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.valid == TRUE);
    assert(obj.presentationSequence == 1);
    assert(obj.gSpriteCommandCount == 1);

    // State load: OAM was restored directly; the published frame is stale.
    NativeSpriteCommandSink_Invalidate();
    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.valid == FALSE);
    assert(obj.presentationSequence == 0);
    assert(obj.gSpriteCommandCount == 0);
    assert(obj.rawOamCount == 1); // the OAM[0] content is now raw-OAM only
    assert(obj.commands[0].sourceKind == NATIVE_SPRITE_SOURCE_RAW_OAM);
    assert(obj.commands[0].oamIndex == 0);

    printf("ok: state load invalidates published provenance (raw-OAM fallback)\n");
}

/* ---------------- command overflow ---------------- */

static void TestCommandOverflowBounded(void)
{
    struct Sprite *sprite = &gSprites[0];
    struct NativeObjSnapshot obj;
    u8 i;

    ResetStubs();
    memset(sprite, 0, sizeof(*sprite));
    sprite->inUse = TRUE;
    sprite->x = 10;
    sprite->y = 10;
    sprite->oam.shape = ST_OAM_SQUARE;
    sprite->oam.size = ST_OAM_SIZE_0;
    PackSpriteOam(sprite);

    NativeSpriteCommandSink_Begin();
    for (i = 0; i < NATIVE_SPRITE_COMMAND_MAX + 2; i++)
    {
        NativeSpriteCommandSink_Append(sprite, i, 0, sprite->oam.shape,
                                       sprite->oam.size, sprite->oam.tileNum,
                                       sprite->oam.priority);
    }
    NativeSpriteCommandSink_End();
    NativeSpriteCommandSink_Commit();

    assert(NativeObjSnapshot_Capture(&obj));
    assert(obj.commandCount == NATIVE_SPRITE_COMMAND_MAX);
    assert(obj.commandOverflow == TRUE);

    printf("ok: command stream bounded with explicit overflow flag\n");
}

/* ---------------- serialization ---------------- */

static void TestSerializeRoundTrip(void)
{
    struct Sprite *sprite = &gSprites[0];
    struct NativeObjSnapshot obj;
    struct NativeObjSnapshot obj2;
    static u8 buf[NATIVE_OBJ_SERIALIZE_HEADER + sizeof(struct NativeObjSnapshot)];
    size_t written = 0;
    size_t need = NATIVE_OBJ_SERIALIZE_HEADER + sizeof(obj);

    ResetStubs();
    memset(sprite, 0, sizeof(*sprite));
    sprite->inUse = TRUE;
    sprite->x = 60;
    sprite->y = 70;
    sprite->oam.shape = ST_OAM_SQUARE;
    sprite->oam.size = ST_OAM_SIZE_1;
    sprite->oam.tileNum = 22;
    sprite->oam.paletteNum = 9;
    PackSpriteOam(sprite);
    WriteOamEntry(0, &sprite->oam);

    NativeSpriteCommandSink_Begin();
    NativeSpriteCommandSink_Append(sprite, 0, 0, sprite->oam.shape, sprite->oam.size,
                                   sprite->oam.tileNum, sprite->oam.priority);
    NativeSpriteCommandSink_End();
    NativeSpriteCommandSink_Commit();
    NativeObjSnapshot_Capture(&obj);

    assert(NativeObjSnapshot_Serialize(&obj, buf, sizeof(buf), &written) == TRUE);
    assert(written == need);
    // Deterministic header.
    assert(buf[0] == (u8)NATIVE_OBJ_SERIALIZE_MAGIC);
    assert(buf[4] == NATIVE_OBJ_SNAPSHOT_SCHEMA_VERSION);

    assert(NativeObjSnapshot_Deserialize(buf, written, &obj2) == TRUE);
    assert(memcmp(&obj, &obj2, sizeof(obj)) == 0);

    // Capacity too small is rejected without writing.
    written = 0xDEAD;
    assert(NativeObjSnapshot_Serialize(&obj, buf, need - 1, &written) == FALSE);
    assert(written == 0xDEAD);

    // Size mismatch rejected.
    assert(NativeObjSnapshot_Deserialize(buf, need - 1, &obj2) == FALSE);
    // Corrupt magic rejected.
    buf[0] ^= 0xFF;
    assert(NativeObjSnapshot_Deserialize(buf, need, &obj2) == FALSE);
    buf[0] ^= 0xFF;
    // Wrong schema version rejected.
    buf[4] = NATIVE_OBJ_SNAPSHOT_SCHEMA_VERSION + 1;
    assert(NativeObjSnapshot_Deserialize(buf, need, &obj2) == FALSE);
    buf[4] = NATIVE_OBJ_SNAPSHOT_SCHEMA_VERSION;
    // Corrupt endian marker rejected.
    buf[8] ^= 0x80;
    assert(NativeObjSnapshot_Deserialize(buf, need, &obj2) == FALSE);

    printf("ok: deterministic serialization + round-trip + rejection cases\n");
}

/* ---------------- main ---------------- */

int main(void)
{
    TestSignedPacking();
    TestSignedTopLeftMath();
    TestMultiEmissionOrderAndValidation();
    TestRegistersAndMemCopy();
    TestSubspriteEmission();
    TestAffineMatrixCopy();
    TestPendingNotPublishedWithoutCommit();
    TestOamLoadDisabledLeavesPublishedUnchanged();
    TestCommitIncrementsSequence();
    TestVisibilityClassification();
    TestRawOamDetection();
    TestRawOamWrapAndOffscreen();
    TestProvenanceMismatch();
    TestInvalidateForcesRawOamFallback();
    TestCommandOverflowBounded();
    TestSerializeRoundTrip();

    printf("native sprite snapshot unit test passed\n");
    return 0;
}

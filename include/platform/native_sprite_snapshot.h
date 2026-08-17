#ifndef GUARD_PLATFORM_NATIVE_SPRITE_SNAPSHOT_H
#define GUARD_PLATFORM_NATIVE_SPRITE_SNAPSHOT_H

#include "global.h"
#include "sprite.h"

/*
 * Stage 3A: presentation-coherent native sprite/OBJ snapshot, pre-wrap command
 * capture, and diagnostics.
 *
 * This is the OBJ half of the native overworld snapshot contract. It is a
 * SEPARATE, versioned, self-contained struct (NativeObjSnapshot) rather than a
 * field appended to NativeOverworldSnapshot: the existing 93312-byte map/BG
 * snapshot fixtures must remain byte-identical, and OBJ captures are a distinct
 * presentation object. The two snapshots are captured back-to-back from the
 * same settled frame state (worker blocked at VBlankIntrWait) and are coherent
 * by construction.
 *
 * PRESENTATION TIMING (the critical invariant):
 *
 *   Platform_VideoDrawFrame reads the final hardware OAM/OBJ memory for the
 *   currently presented frame. By then gSprites / gOamMatrices / gMain.oamBuffer
 *   have already advanced toward the NEXT VBlank. So the command stream must be
 *   built as a PENDING frame during the actual BuildOamBuffer emission (the same
 *   sorted path that writes gMain.oamBuffer), and only PUBLISHED when LoadOam
 *   successfully copies gMain.oamBuffer to OAM. If oamLoadDisabled prevents the
 *   copy, the published frame is NOT advanced. Capture must therefore copy the
 *   PUBLISHED command frame together with final OAM, current OBJ VRAM, OBJ
 *   palette RAM, and the relevant registers.
 *
 * The camera origin used to derive world positions is latched WITH the pending
 * command frame (NativeSpriteCommandSink_Begin). It is never re-read at capture
 * time -- reading the live camera there risks another one-frame mismatch.
 *
 * Every presented command carries the signed PRE-WRAP destination top-left
 * (not just the packed OAM coordinate) so an expanded viewport can reconstruct
 * world placement. The packed final OAM entry remains the 240x160 authority:
 * at capture every GSPRITE command is validated against its final OAM entry,
 * and visible/unmapped final entries are classified explicitly as raw-OAM-only
 * or provenance mismatches -- never silently omitted.
 *
 * This module performs NO pixel rendering and NO final OBJ compositing (Stage
 * 3B). It only captures, classifies, validates, and serializes.
 */

#define NATIVE_OBJ_SNAPSHOT_SCHEMA_VERSION 1

#define OAM_ENTRY_COUNT          128
#define OBJ_PALETTE_ENTRY_COUNT  256
#define NATIVE_OBJ_NO_ID         0xFF

// Bounded command stream. Emitted GSPRITE primitives occupy at most gOamLimit
// (<= 128) distinct OAM indexes, and raw-OAM entries are unmapped, so the two
// sets are disjoint: total commands can never exceed 128.
#define NATIVE_SPRITE_COMMAND_MAX      128
// Bounded unpresented (hidden/culled/not-emitted) sprite records: at most
// MAX_SPRITES (64).
#define NATIVE_SPRITE_UNPRESENTED_MAX  64

// Serialized form: [u32 magic "NOBJ"][u32 schemaVersion][u32 endian marker]
//                      [u32 bodySize][raw struct body].
#define NATIVE_OBJ_SERIALIZE_MAGIC     0x4A424F4Eu
#define NATIVE_OBJ_SERIALIZE_MARKER    0x12345678u
#define NATIVE_OBJ_SERIALIZE_HEADER    16u // four u32 fields

// Provenance of a captured command.
enum NativeSpriteSourceKind
{
    NATIVE_SPRITE_SOURCE_GSPRITE = 0,   // emitted from the gSprites OAM build
    NATIVE_SPRITE_SOURCE_RAW_OAM = 1,   // visible final OAM entry with no command
    NATIVE_SPRITE_SOURCE_FUTURE_PROVIDER = 2,
};

// World-placement classification, used for future expanded-viewport handling.
// At 240x160 it has no effect on rendering (the packed OAM is authoritative).
enum NativeSpritePlacement
{
    NATIVE_SPRITE_PLACEMENT_WORLD = 0,          // object-event owned (world anchor)
    NATIVE_SPRITE_PLACEMENT_CAMERA_RELATIVE = 1,// coordOffsetEnabled effects
    NATIVE_SPRITE_PLACEMENT_SCREEN_FIXED = 2,   // reserved: no Stage 3A detector
    NATIVE_SPRITE_PLACEMENT_UNKNOWN = 3,
};

// Visibility classification. Only VANILLA_VIEWPORT_CULLED with
// expandedEligible=true may be revealed in an expanded viewport; hidden-unknown
// and intentionally-hidden sprites must stay hidden, and not-emitted sprites
// (OAM budget) are recorded but were never presented at 240x160.
enum NativeSpriteVisibility
{
    NATIVE_SPRITE_GBA_EMITTED = 0,              // presented at 240x160
    NATIVE_SPRITE_VANILLA_VIEWPORT_CULLED = 1,  // hidden by off-screen culling
    NATIVE_SPRITE_INTENTIONALLY_HIDDEN = 2,     // objectEvent.invisible
    NATIVE_SPRITE_HIDDEN_UNKNOWN = 3,           // invisible, reason unprovable
    NATIVE_SPRITE_OAM_LIMIT_DROPPED = 4,        // visible but sort hit gOamLimit
};

struct NativeSpriteDrawCommand
{
    u32 commandId;          // deterministic per-frame identity == emissionOrdinal
    u32 emissionOrdinal;    // 0-based emission order within the published frame
    u8 sourceKind;          // enum NativeSpriteSourceKind
    u8 spriteId;            // source gSprites index (NATIVE_OBJ_NO_ID for raw)
    u8 objectEventId;       // owning ObjectEvent index, NATIVE_OBJ_NO_ID if none
    u8 partIndex;           // subsprite part index (0 for non-subsprite)
    u8 oamIndex;            // exact destination OAM entry (NATIVE_OBJ_NO_ID for
                            // unpresented records)
    u8 placement;           // enum NativeSpritePlacement
    u8 visibility;          // enum NativeSpriteVisibility
    u8 expandedEligible;    // bool: may be revealed/moved by an expanded viewport
    u8 oamValidated;        // bool: capture-time validation against final OAM
    u8 reserved;            // always 0
    s32 signedX;            // pre-wrap destination top-left X
    s32 signedY;            // pre-wrap destination top-left Y
    s32 worldX;             // derived world anchor (WORLD/CAMERA_RELATIVE)
    s32 worldY;
    u16 tileNum;            // effective (subsprite-adjusted) tile number
    u8 shape;
    u8 size;
    u8 priority;            // effective OAM priority (subsprite override applied)
    u8 subpriority;         // diagnostic provenance only
    u8 paletteNum;
    u8 bpp;
    u8 objMode;
    u8 affineMode;
    u8 mosaic;
    u8 flipX;               // bool (matrixNum bit 3, non-affine)
    u8 flipY;               // bool (matrixNum bit 4, non-affine)
    s16 pa;                 // copied affine matrix values (0 for non-affine)
    s16 pb;
    s16 pc;
    s16 pd;
};

/*
 * Self-contained OBJ presentation snapshot captured at host draw time from the
 * PUBLISHED command frame plus the final hardware state. `valid` is FALSE when
 * the published command frame is stale (state loaded since the last LoadOam
 * commit); capture then classifies every presented final OAM entry as raw-OAM
 * only until the next successful commit.
 */
struct NativeObjSnapshot
{
    u32 schemaVersion;          // NATIVE_OBJ_SNAPSHOT_SCHEMA_VERSION
    u64 presentationSequence;   // increments per successful LoadOam commit
    bool32 valid;               // published command frame present and valid
    bool32 oamAuthorityValid;   // final OAM copy is authoritative (always TRUE
                                // at capture)
    u16 dispCnt;
    u16 mosaic;
    u16 bldCnt;
    u16 bldAlpha;
    u16 bldY;
    u16 win0H;
    u16 win0V;
    u16 win1H;
    u16 win1V;
    u16 winIn;
    u16 winOut;
    u16 logicalViewportWidth;   // DISPLAY_WIDTH at capture
    u16 logicalViewportHeight;  // DISPLAY_HEIGHT at capture
    s32 commandCameraOriginX;   // latched with the pending command frame
    s32 commandCameraOriginY;

    // commands[] = [validated GSPRITE][mismatched GSPRITE][RAW_OAM], in
    // emission order for the GSPRITE prefix (emissionOrdinal == array index).
    u16 gSpriteCommandCount;        // leading commands with GSPRITE provenance
    u16 validatedCommandCount;      // GSPRITE commands that matched final OAM
    u16 provenanceMismatchCount;    // GSPRITE commands that disagreed with OAM
    u16 rawOamCount;                // appended RAW_OAM commands (visible/unmapped)
    u16 unpresentedCount;           // hidden/culled/not-emitted records
    u16 unmappedNonPresentedCount;  // offscreen/dummy unmapped entries (accounted)
    u16 commandCount;               // total entries in commands[]
    bool32 commandOverflow;         // a bounded array was exceeded (never expected)

    struct OamData finalOam[OAM_ENTRY_COUNT];       // final hardware OAM
    u8 objVram[OBJ_VRAM0_SIZE];                     // current OBJ tile VRAM
    u16 objPalette[OBJ_PALETTE_ENTRY_COUNT];        // current OBJ palette RAM

    struct NativeSpriteDrawCommand commands[NATIVE_SPRITE_COMMAND_MAX];
    struct NativeSpriteDrawCommand unpresented[NATIVE_SPRITE_UNPRESENTED_MAX];
};

/*
 * Pure coordinate helpers. NativeSprite_GetSignedTopLeftX/Y are the factored
 * forms of UpdateOamCoords' inline math, so the command sink and the OAM build
 * agree by construction. The subsprite offset mirrors AddSubspritesToOamBuffer's
 * flip adjustment exactly.
 */
s32 NativeSprite_GetSignedTopLeftX(const struct Sprite *sprite);
s32 NativeSprite_GetSignedTopLeftY(const struct Sprite *sprite);
s32 NativeSprite_PackSignedX(s32 signedX);  // x -20 -> 492, x 280 -> 280
s32 NativeSprite_PackSignedY(s32 signedY);  // y -20 -> 236, y 200 -> 200
s32 NativeSprite_ComputeSubspriteOffset(s8 offset, s8 dimension, bool32 flip);

/*
 * Command emission sink, called from the OAM construction path in sprite.c
 * (LINUX64 builds only). The sink builds a PENDING frame that is only PUBLISHED
 * when LoadOam actually copies gMain.oamBuffer to OAM. Call order per frame:
 *
 *   BuildOamBuffer:  Begin() ... AddSpriteToOamBuffer/AddSubspritesToOamBuffer
 *                    call Append() per emitted primitive ... End()
 *   LoadOam:         after the successful CpuCopy32: Commit()
 *   native_state.c:  after restoring OAM from a save: Invalidate()
 */
void NativeSpriteCommandSink_Begin(void);
void NativeSpriteCommandSink_Append(const struct Sprite *sprite, u8 oamIndex,
                                    u8 partIndex, u8 partShape, u8 partSize,
                                    u16 partTileNum, u8 partPriority);
void NativeSpriteCommandSink_End(void);
void NativeSpriteCommandSink_Commit(void);
void NativeSpriteCommandSink_Invalidate(void);

/*
 * Capture the current presentation-coherent OBJ snapshot: final OAM, OBJ VRAM,
 * OBJ palette, relevant registers, and the PUBLISHED command frame (with
 * per-command OAM validation and raw-OAM classification). Returns FALSE only on
 * a NULL output pointer.
 */
bool32 NativeObjSnapshot_Capture(struct NativeObjSnapshot *obj);

// Versioned, deterministic serialization with a self-describing header.
bool32 NativeObjSnapshot_Serialize(const struct NativeObjSnapshot *obj,
                                   u8 *out, size_t capacity, size_t *written);
bool32 NativeObjSnapshot_Deserialize(const u8 *in, size_t size,
                                     struct NativeObjSnapshot *obj);

#endif // GUARD_PLATFORM_NATIVE_SPRITE_SNAPSHOT_H

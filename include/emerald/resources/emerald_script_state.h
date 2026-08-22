#ifndef EMERALD_RESOURCES_EMERALD_SCRIPT_STATE_H
#define EMERALD_RESOURCES_EMERALD_SCRIPT_STATE_H

/* R13-G4: State-v5 field-script execution adapter.
 *
 * This is deliberately a state-family adapter, not a second state format.
 * Static staged-script pointers become the existing v5 64-byte resource
 * record (key + type/schema/role + payload offset).  The boundary role is
 * determined by the exact destination field, so no serialized field is
 * added.  Mutable script buffers retain the generic in-band persistent
 * address representation and are validated against an explicitly captured
 * dynamic-buffer registration.
 *
 * The adapter never publishes the shadow arena, changes an opcode handler,
 * or registers a production resource range. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_id.h"

struct ScriptContext;

#define EMERALD_SCRIPT_STATE_STACK_CAP 20u
#define EMERALD_SCRIPT_STATE_DYNAMIC_BUFFER_CAP 8u
#define EMERALD_SCRIPT_STATE_OWNER_ID_CAP 48u

enum EmeraldScriptStateStatus
{
    EMERALD_SCRIPT_STATE_OK = 0,
    EMERALD_SCRIPT_STATE_NOT_SCRIPT,
    EMERALD_SCRIPT_STATE_SCRUB,
    EMERALD_SCRIPT_STATE_DYNAMIC,
    EMERALD_SCRIPT_STATE_ERR_UNAVAILABLE,
    EMERALD_SCRIPT_STATE_ERR_CONTEXT1_DEPTH,
    EMERALD_SCRIPT_STATE_ERR_CONTEXT1_NULL_IP,
    EMERALD_SCRIPT_STATE_ERR_CONTEXT2_ACTIVE,
    EMERALD_SCRIPT_STATE_ERR_UNKNOWN_POINTER,
    EMERALD_SCRIPT_STATE_ERR_STALE_GENERATION,
    EMERALD_SCRIPT_STATE_ERR_WRONG_SEGMENT,
    EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY,
    EMERALD_SCRIPT_STATE_ERR_DYNAMIC_UNKNOWN,
    EMERALD_SCRIPT_STATE_ERR_DYNAMIC_BOUNDS,
    EMERALD_SCRIPT_STATE_ERR_DYNAMIC_GENERATION,
    /* R13-G6 (plan sec 9): EMERALD_SCRIPT_STATE_ERR_VADDRESS_HOST_DELTA
     * removed - the legacy creator-process host delta and its refusal
     * are gone (dead storage deleted; anchors are the only model). */
    EMERALD_SCRIPT_STATE_ERR_MISSING_KEY,
    EMERALD_SCRIPT_STATE_ERR_SCHEMA,
};

enum EmeraldScriptStateDynamicKind
{
    EMERALD_SCRIPT_DYNAMIC_SAVE_RAM_SCRIPT = 1,
    EMERALD_SCRIPT_DYNAMIC_MYSTERY_EVENT_BUFFER = 2,
    EMERALD_SCRIPT_DYNAMIC_CAPTURED_BUFFER = 3,
    /* R13-G5: the live static G arena registers as one buffer so the
     * stable virtual anchor resolves static-script vaddress targets
     * through the same machinery as the dynamic buffers. */
    EMERALD_SCRIPT_DYNAMIC_STATIC_G_ARENA = 4,
};

struct EmeraldScriptStateLayout
{
    struct ScriptContext *context1;
    const uint8_t *context1Status;
    const uint64_t *generationStamp;
    struct ScriptContext *context2;
    const uint8_t **ramScriptRetAddr;
    const uint8_t **approachingTrainerScript[2];
    const uint8_t **trainerBattleEndScript;
    const uint8_t **trainerAReturnScript;
    const uint8_t **trainerBReturnScript;
    struct ScriptContext *mysteryEventContext;
    uint8_t **mysteryEventNativeBase;
};

struct EmeraldScriptStateResourceIdentity
{
    Gen3ResourceKey key;
    uint32_t resourceType;
    uint32_t schema;
    uint32_t representationRole;
    uint32_t payloadOffset;
    uint32_t boundaryRole;
    uint64_t generationId; /* validation-only; never serialized */
    char moduleKey[96];
};

struct EmeraldScriptDynamicBuffer
{
    uint32_t kind;
    uint32_t ownerStorageId;
    uint64_t generation;
    uint8_t *base;
    size_t size;
    /* Optional byte bitmap, one byte per buffer byte. Nonzero marks an exact
     * instruction boundary. Required for dynamic IP/return validation. */
    const uint8_t *instructionStarts;
    char ownerId[EMERALD_SCRIPT_STATE_OWNER_ID_CAP];
};

/* Stable replacement-state model for sAddressOffset.  It contains no host
 * pointer or creator-process delta and can live in an existing captured
 * slice without a State-v5 format change. */
struct EmeraldScriptVirtualAnchor
{
    uint32_t encodedVirtualBase;
    uint32_t bufferKind;
    uint32_t ownerStorageId;
    uint32_t liveBaseOffset;
    uint64_t bufferGeneration;
    uint32_t valid;
    uint32_t reserved;
};

bool EmeraldScriptState_BindLiveLayout(void);
void EmeraldScriptState_SetLayout(const struct EmeraldScriptStateLayout *layout);
void EmeraldScriptState_ClearLayout(void);
void EmeraldScriptState_ClearDynamicBuffers(void);
bool EmeraldScriptState_RegisterDynamicBuffer(
    const struct EmeraldScriptDynamicBuffer *buffer);

/* R13-G6 (plan sec 9): build the 17-op MEVENT grammar's instruction
 * boundary bitmap - one byte per buffer byte, nonzero at an exact
 * instruction start. The walk decodes sequentially from offset 0 and
 * breaks on an unknown opcode or after `end` (0x02), leaving trailing
 * bytes (main-dialect sub-scripts, embedded data) opaque/unmarked.
 * `bitmap` must hold `size` bytes and is fully zeroed first. */
void EmeraldScriptState_BuildMysteryEventBoundaryBitmap(
    const uint8_t *script, size_t size, uint8_t *bitmap);

/* Whole-capture preflight.  It enforces the Context1 depth/IP rules and the
 * hard Context2 stopped/null rule before the generic walker can emit any
 * sidecar record, preserving all-or-nothing capture. */
enum EmeraldScriptStateStatus EmeraldScriptState_PrepareCapture(void);

/* Classify one exact pointer field.  OK means emit the returned ordinary v5
 * resource record; SCRUB means zero inactive scratch; DYNAMIC means validate
 * then let the generic persistent-address path encode it; NOT_SCRIPT means
 * continue with the existing generic walker. */
enum EmeraldScriptStateStatus EmeraldScriptState_CaptureField(
    uintptr_t fieldAddress, uintptr_t pointer,
    struct EmeraldScriptStateResourceIdentity *outIdentity);

/* Resolve one existing sidecar record for an exact destination field.  The
 * adapter validates every record into caller-owned staging before any slice
 * is committed. */
enum EmeraldScriptStateStatus EmeraldScriptState_ResolveField(
    uintptr_t fieldAddress, const Gen3ResourceKey *key,
    uint32_t resourceType, uint32_t schema, uint32_t representationRole,
    uint32_t payloadOffset, const void *serializedSlice,
    uintptr_t liveSliceBase, size_t sliceSize, uintptr_t *outPointer);

bool EmeraldScriptState_IsStaticRecord(uint32_t resourceType,
                                       uint32_t schema,
                                       uint32_t representationRole);
enum EmeraldScriptStateStatus EmeraldScriptState_ValidateDynamicField(
    uintptr_t fieldAddress, uintptr_t pointer);

/* R13-G5 (plan sec 7): build the live anchor for the buffer containing
 * `liveBase` (the byte after the setvaddress operand) - a registered
 * dynamic buffer or the static G arena registered at publication. */
enum EmeraldScriptStateStatus EmeraldScriptState_BuildVirtualAnchorFromBase(
    uintptr_t liveBase, uint32_t encodedVirtualBase,
    struct EmeraldScriptVirtualAnchor *outAnchor);

enum EmeraldScriptStateStatus EmeraldScriptState_BuildVirtualAnchor(
    const struct EmeraldScriptDynamicBuffer *buffer,
    uint32_t encodedVirtualBase, uint32_t liveBaseOffset,
    struct EmeraldScriptVirtualAnchor *outAnchor);
enum EmeraldScriptStateStatus EmeraldScriptState_ResolveVirtualTarget(
    const struct EmeraldScriptVirtualAnchor *anchor, uint32_t encodedTarget,
    size_t width, uintptr_t *outAddress);

const char *EmeraldScriptState_GetLastSurface(void);
const char *EmeraldScriptStateStatus_Describe(enum EmeraldScriptStateStatus status);

#endif /* EMERALD_RESOURCES_EMERALD_SCRIPT_STATE_H */

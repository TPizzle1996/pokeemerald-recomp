/* R13-G4 State-v5 field-script execution readiness.
 *
 * Static G pointers use the existing v5 resource sidecar.  This adapter owns
 * only family-specific surface and boundary policy; the generic container
 * still owns record layout, ordering, CRCs, capacity, and transactional
 * commit.  Dynamic buffers remain ordinary captured mutable storage and use
 * the existing in-band persistent address. */

#include "emerald/resources/emerald_script_state.h"

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "script.h"
#include "trainer_see.h"
#include "gen3/resources/resource_types.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_script_compat.h"
#include "emerald/resources/script_native.generated.h"

/* Production G4 deliberately does not link the G3 shadow seam.  The adapter
 * remains present and falls through to the existing image-relative State-v5
 * path until G5 links/publishes the seam atomically.  Focused G4 harnesses
 * link strong definitions and exercise every path below. */
#pragma weak EmeraldScriptCompat_GetGenerationId
#pragma weak EmeraldScriptCompat_ReverseResolve
#pragma weak EmeraldScriptCompat_ValidateBoundary
#pragma weak EmeraldScriptCompat_GetStateIdentity
#pragma weak EmeraldScriptCompat_ResolveStateIdentity

/* Narrow, read-only surface accessors.  Weak declarations keep focused
 * offline harnesses able to install an exact cloned layout without linking
 * the live interpreter TUs. */
extern void Script_GetStateContexts(struct ScriptContext **context1,
                                    const uint8_t **context1Status,
                                    struct ScriptContext **context2)
    __attribute__((weak));
extern void ScrCmd_GetStatePointers(const uint8_t ***ramScriptRetAddr,
                                    intptr_t **addressOffset)
    __attribute__((weak));
extern void BattleSetup_GetScriptStatePointers(
    const uint8_t ***battleEnd, const uint8_t ***trainerAReturn,
    const uint8_t ***trainerBReturn) __attribute__((weak));
extern void MysteryEvent_GetScriptStatePointers(
    struct ScriptContext **context, uint8_t ***nativeBase)
    __attribute__((weak));

enum SurfaceClass
{
    SURFACE_NONE = 0,
    SURFACE_CONTEXT1_IP,
    SURFACE_CONTEXT1_RETURN,
    SURFACE_CONTEXT1_INACTIVE,
    SURFACE_CONTEXT2_TRANSIENT,
    SURFACE_RAM_RETURN,
    SURFACE_APPROACHING,
    SURFACE_TRAINER_END,
    SURFACE_TRAINER_RETURN,
    SURFACE_MYSTERY_IP,
    SURFACE_MYSTERY_RETURN,
    SURFACE_MYSTERY_INACTIVE,
    SURFACE_MYSTERY_BASE,
};

struct SurfaceInfo
{
    enum SurfaceClass surfaceClass;
    uint32_t boundaryRole;
    const char *name;
    bool dynamicAllowed;
};

static struct EmeraldScriptStateLayout sLayout;
static bool sLayoutBound;
static struct EmeraldScriptDynamicBuffer
    sDynamicBuffers[EMERALD_SCRIPT_STATE_DYNAMIC_BUFFER_CAP];
static size_t sDynamicBufferCount;
static char sLastSurface[96];

static void NoteSurface(const char *name)
{
    if (name == NULL)
        name = "field-script-state";
    snprintf(sLastSurface, sizeof(sLastSurface), "%s", name);
}

static bool PointerSlot(uintptr_t fieldAddress, const uint8_t *const *slot)
{
    return slot != NULL && fieldAddress == (uintptr_t)(const void *)slot;
}

static struct SurfaceInfo FindSurface(uintptr_t fieldAddress)
{
    struct SurfaceInfo out;
    size_t i;

    memset(&out, 0, sizeof(out));
    if (!sLayoutBound)
        return out;
    if (sLayout.context1 != NULL)
    {
        if (PointerSlot(fieldAddress, &sLayout.context1->scriptPtr))
        {
            out.surfaceClass = sLayout.context1->mode == 0
                             ? SURFACE_CONTEXT1_INACTIVE
                             : SURFACE_CONTEXT1_IP;
            out.boundaryRole = EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START;
            out.name = "Context1.scriptPtr";
            out.dynamicAllowed = true;
            return out;
        }
        for (i = 0u; i < EMERALD_SCRIPT_STATE_STACK_CAP; i++)
        {
            if (!PointerSlot(fieldAddress, &sLayout.context1->stack[i]))
                continue;
            out.surfaceClass = (sLayout.context1->mode == 0
                             || i >= sLayout.context1->stackDepth)
                             ? SURFACE_CONTEXT1_INACTIVE
                             : SURFACE_CONTEXT1_RETURN;
            out.boundaryRole = EMERALD_SCRIPT_BOUNDARY_NEXT_INSTRUCTION;
            out.name = "Context1.stack";
            out.dynamicAllowed = true;
            return out;
        }
    }
    if (sLayout.context2 != NULL)
    {
        if (PointerSlot(fieldAddress, &sLayout.context2->scriptPtr))
        {
            out.surfaceClass = SURFACE_CONTEXT2_TRANSIENT;
            out.name = "Context2.scriptPtr";
            return out;
        }
        for (i = 0u; i < EMERALD_SCRIPT_STATE_STACK_CAP; i++)
        {
            if (PointerSlot(fieldAddress, &sLayout.context2->stack[i]))
            {
                out.surfaceClass = SURFACE_CONTEXT2_TRANSIENT;
                out.name = "Context2.stack";
                return out;
            }
        }
    }
    if (PointerSlot(fieldAddress, sLayout.ramScriptRetAddr))
    {
        out.surfaceClass = SURFACE_RAM_RETURN;
        out.boundaryRole = EMERALD_SCRIPT_BOUNDARY_NEXT_INSTRUCTION;
        out.name = "gRamScriptRetAddr";
        return out;
    }
    for (i = 0u; i < 2u; i++)
    {
        if (PointerSlot(fieldAddress, sLayout.approachingTrainerScript[i]))
        {
            out.surfaceClass = SURFACE_APPROACHING;
            out.boundaryRole = EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START;
            out.name = i == 0u ? "gApproachingTrainers[0].trainerScriptPtr"
                               : "gApproachingTrainers[1].trainerScriptPtr";
            return out;
        }
    }
    if (PointerSlot(fieldAddress, sLayout.trainerBattleEndScript))
    {
        out.surfaceClass = SURFACE_TRAINER_END;
        out.boundaryRole = EMERALD_SCRIPT_BOUNDARY_NEXT_INSTRUCTION;
        out.name = "sTrainerBattleEndScript";
        return out;
    }
    if (PointerSlot(fieldAddress, sLayout.trainerAReturnScript))
    {
        out.surfaceClass = SURFACE_TRAINER_RETURN;
        out.boundaryRole = EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT;
        out.name = "sTrainerABattleScriptRetAddr";
        return out;
    }
    if (PointerSlot(fieldAddress, sLayout.trainerBReturnScript))
    {
        out.surfaceClass = SURFACE_TRAINER_RETURN;
        out.boundaryRole = EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT;
        out.name = "sTrainerBBattleScriptRetAddr";
        return out;
    }
    if (sLayout.mysteryEventContext != NULL)
    {
        if (PointerSlot(fieldAddress,
                        &sLayout.mysteryEventContext->scriptPtr))
        {
            out.surfaceClass = sLayout.mysteryEventContext->mode == 0
                             ? SURFACE_MYSTERY_INACTIVE : SURFACE_MYSTERY_IP;
            out.name = "MysteryEvent.scriptPtr";
            out.dynamicAllowed = true;
            return out;
        }
        for (i = 0u; i < EMERALD_SCRIPT_STATE_STACK_CAP; i++)
        {
            if (!PointerSlot(fieldAddress,
                             &sLayout.mysteryEventContext->stack[i]))
                continue;
            out.surfaceClass = (sLayout.mysteryEventContext->mode == 0
                             || i >= sLayout.mysteryEventContext->stackDepth)
                             ? SURFACE_MYSTERY_INACTIVE
                             : SURFACE_MYSTERY_RETURN;
            out.name = "MysteryEvent.stack";
            out.dynamicAllowed = true;
            return out;
        }
    }
    if (sLayout.mysteryEventNativeBase != NULL
     && fieldAddress == (uintptr_t)(void *)sLayout.mysteryEventNativeBase)
    {
        out.surfaceClass = SURFACE_MYSTERY_BASE;
        out.name = "sMysteryEventScriptNativeBase";
        out.dynamicAllowed = true;
    }
    return out;
}

static enum EmeraldScriptStateStatus FindDynamic(
    uintptr_t pointer, bool requireInstruction,
    const struct EmeraldScriptDynamicBuffer **outBuffer, size_t *outOffset)
{
    const struct EmeraldScriptDynamicBuffer *hit = NULL;
    size_t hitOffset = 0u;
    size_t i;

    for (i = 0u; i < sDynamicBufferCount; i++)
    {
        const struct EmeraldScriptDynamicBuffer *b = &sDynamicBuffers[i];
        if (pointer < (uintptr_t)b->base
         || pointer - (uintptr_t)b->base >= b->size)
            continue;
        if (hit != NULL)
            return EMERALD_SCRIPT_STATE_ERR_DYNAMIC_UNKNOWN;
        hit = b;
        hitOffset = pointer - (uintptr_t)b->base;
    }
    if (hit == NULL)
        return EMERALD_SCRIPT_STATE_ERR_DYNAMIC_UNKNOWN;
    if (requireInstruction
     && (hit->instructionStarts == NULL
      || hit->instructionStarts[hitOffset] == 0u))
        return EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY;
    if (outBuffer != NULL)
        *outBuffer = hit;
    if (outOffset != NULL)
        *outOffset = hitOffset;
    return EMERALD_SCRIPT_STATE_OK;
}

bool EmeraldScriptState_BindLiveLayout(void)
{
    struct EmeraldScriptStateLayout layout;
    const uint8_t **ram = NULL;
    intptr_t *addressOffset = NULL;
    const uint8_t **battleEnd = NULL;
    const uint8_t **trainerA = NULL;
    const uint8_t **trainerB = NULL;
    uint8_t **mysteryBase = NULL;

    memset(&layout, 0, sizeof(layout));
    if (Script_GetStateContexts == NULL || ScrCmd_GetStatePointers == NULL
     || BattleSetup_GetScriptStatePointers == NULL
     || MysteryEvent_GetScriptStatePointers == NULL)
        return false;
    Script_GetStateContexts(&layout.context1, &layout.context1Status,
                            &layout.context2);
    ScrCmd_GetStatePointers(&ram, &addressOffset);
    BattleSetup_GetScriptStatePointers(&battleEnd, &trainerA, &trainerB);
    MysteryEvent_GetScriptStatePointers(&layout.mysteryEventContext,
                                        &mysteryBase);
    layout.ramScriptRetAddr = ram;
    layout.approachingTrainerScript[0] =
        &gApproachingTrainers[0].trainerScriptPtr;
    layout.approachingTrainerScript[1] =
        &gApproachingTrainers[1].trainerScriptPtr;
    layout.trainerBattleEndScript = battleEnd;
    layout.trainerAReturnScript = trainerA;
    layout.trainerBReturnScript = trainerB;
    layout.addressOffset = addressOffset;
    layout.mysteryEventNativeBase = mysteryBase;
    EmeraldScriptState_SetLayout(&layout);
    return true;
}

void EmeraldScriptState_SetLayout(const struct EmeraldScriptStateLayout *layout)
{
    if (layout == NULL)
    {
        EmeraldScriptState_ClearLayout();
        return;
    }
    sLayout = *layout;
    sLayoutBound = true;
    sLastSurface[0] = '\0';
}

void EmeraldScriptState_ClearLayout(void)
{
    memset(&sLayout, 0, sizeof(sLayout));
    sLayoutBound = false;
    sLastSurface[0] = '\0';
}

void EmeraldScriptState_ClearDynamicBuffers(void)
{
    memset(sDynamicBuffers, 0, sizeof(sDynamicBuffers));
    sDynamicBufferCount = 0u;
}

bool EmeraldScriptState_RegisterDynamicBuffer(
    const struct EmeraldScriptDynamicBuffer *buffer)
{
    size_t i;
    uintptr_t start;
    uintptr_t end;

    if (buffer == NULL || buffer->base == NULL || buffer->size == 0u
     || buffer->kind == 0u || buffer->ownerId[0] == '\0'
     || buffer->generation == 0u || sDynamicBufferCount >= ARRAY_COUNT(sDynamicBuffers))
        return false;
    start = (uintptr_t)buffer->base;
    if (buffer->size > UINTPTR_MAX - start)
        return false;
    end = start + buffer->size;
    for (i = 0u; i < sDynamicBufferCount; i++)
    {
        uintptr_t otherStart = (uintptr_t)sDynamicBuffers[i].base;
        uintptr_t otherEnd = otherStart + sDynamicBuffers[i].size;
        if (start < otherEnd && otherStart < end)
            return false;
        if (buffer->kind == sDynamicBuffers[i].kind
         && buffer->ownerStorageId == sDynamicBuffers[i].ownerStorageId)
        {
            /* R13-G5: a re-registration of the same (kind, owner) is a
             * generation replacement - the new buffer supersedes the
             * old (the generation stamp validates every anchor). */
            sDynamicBuffers[i] = *buffer;
            return true;
        }
    }
    sDynamicBuffers[sDynamicBufferCount++] = *buffer;
    return true;
}

enum EmeraldScriptStateStatus EmeraldScriptState_PrepareCapture(void)
{
    size_t i;

    if (!sLayoutBound && !EmeraldScriptState_BindLiveLayout())
        return EMERALD_SCRIPT_STATE_ERR_UNAVAILABLE;
    if (sLayout.context1 != NULL)
    {
        if (sLayout.context1->stackDepth > EMERALD_SCRIPT_STATE_STACK_CAP)
        {
            NoteSurface("Context1.stackDepth");
            return EMERALD_SCRIPT_STATE_ERR_CONTEXT1_DEPTH;
        }
        if (sLayout.context1->mode != 0 && sLayout.context1->scriptPtr == NULL)
        {
            NoteSurface("Context1.scriptPtr");
            return EMERALD_SCRIPT_STATE_ERR_CONTEXT1_NULL_IP;
        }
    }
    if (sLayout.context2 != NULL)
    {
        bool active = sLayout.context2->mode != 0
                   || sLayout.context2->scriptPtr != NULL
                   || sLayout.context2->stackDepth != 0;
        for (i = 0u; i < EMERALD_SCRIPT_STATE_STACK_CAP; i++)
            active |= sLayout.context2->stack[i] != NULL;
        if (active)
        {
            NoteSurface("Context2(active/transient)");
            return EMERALD_SCRIPT_STATE_ERR_CONTEXT2_ACTIVE;
        }
    }
    /* Never serialize the legacy creator-process host delta. Successful G4
     * staged vaddress fixtures persist EmeraldScriptVirtualAnchor instead.
     * Converting a live nonzero delta requires G5's atomic handler/buffer
     * registration switch, so refuse before any state bytes are emitted. */
    if (sLayout.addressOffset != NULL && *sLayout.addressOffset != 0)
    {
        NoteSurface("sAddressOffset");
        return EMERALD_SCRIPT_STATE_ERR_VADDRESS_HOST_DELTA;
    }
    if (sLayout.mysteryEventContext != NULL
     && sLayout.mysteryEventContext->stackDepth > EMERALD_SCRIPT_STATE_STACK_CAP)
    {
        NoteSurface("MysteryEvent.stackDepth");
        return EMERALD_SCRIPT_STATE_ERR_CONTEXT1_DEPTH;
    }
    return EMERALD_SCRIPT_STATE_OK;
}

enum EmeraldScriptStateStatus EmeraldScriptState_CaptureField(
    uintptr_t fieldAddress, uintptr_t pointer,
    struct EmeraldScriptStateResourceIdentity *outIdentity)
{
    struct SurfaceInfo surface = FindSurface(fieldAddress);
    char moduleKey[96];
    uint32_t offset;
    uint32_t segmentKind;
    uint32_t schema;
    uint32_t payloadSize;
    enum EmeraldScriptCompatStatus scriptStatus;

    if (surface.surfaceClass == SURFACE_NONE)
        return EMERALD_SCRIPT_STATE_NOT_SCRIPT;
    NoteSurface(surface.name);
    if (surface.surfaceClass == SURFACE_CONTEXT1_INACTIVE
     || surface.surfaceClass == SURFACE_CONTEXT2_TRANSIENT
     || surface.surfaceClass == SURFACE_MYSTERY_INACTIVE)
        return EMERALD_SCRIPT_STATE_SCRUB;
    if (pointer == 0u)
        return EMERALD_SCRIPT_STATE_NOT_SCRIPT;
    if (surface.surfaceClass == SURFACE_MYSTERY_IP
     || surface.surfaceClass == SURFACE_MYSTERY_RETURN
     || surface.surfaceClass == SURFACE_MYSTERY_BASE)
    {
        enum EmeraldScriptStateStatus dyn = FindDynamic(
            pointer, surface.surfaceClass != SURFACE_MYSTERY_BASE, NULL, NULL);
        return dyn == EMERALD_SCRIPT_STATE_OK
             ? EMERALD_SCRIPT_STATE_DYNAMIC : dyn;
    }
    /* G4 production still executes compiled scripts and does not initialize
     * the shadow seam.  Those executable-image pointers must retain the
     * pre-G4 generic image-relative path.  Once a shadow generation exists
     * (the staged harness today, live G5 later), a pointer on an exact G
     * surface is required to belong to that generation or be refused. */
    if (EmeraldScriptCompat_GetGenerationId == NULL
     || EmeraldScriptCompat_ReverseResolve == NULL
     || EmeraldScriptCompat_ValidateBoundary == NULL
     || EmeraldScriptCompat_GetStateIdentity == NULL
     || EmeraldScriptCompat_GetGenerationId() == 0u)
        return EMERALD_SCRIPT_STATE_NOT_SCRIPT;
    if (sLayout.generationStamp != NULL
     && *sLayout.generationStamp != EmeraldScriptCompat_GetGenerationId())
        return EMERALD_SCRIPT_STATE_ERR_STALE_GENERATION;
    scriptStatus = EmeraldScriptCompat_ReverseResolve(
        pointer, moduleKey, sizeof(moduleKey), &offset, &segmentKind);
    if (scriptStatus != EMERALD_SCRIPT_OK)
    {
        if (surface.dynamicAllowed
         && FindDynamic(pointer, true, NULL, NULL) == EMERALD_SCRIPT_STATE_OK)
            return EMERALD_SCRIPT_STATE_DYNAMIC;
        return EMERALD_SCRIPT_STATE_ERR_UNKNOWN_POINTER;
    }
    if (segmentKind != EMERALD_SCRIPT_NATIVE_SEGMENT_BYTECODE)
        return EMERALD_SCRIPT_STATE_ERR_WRONG_SEGMENT;
    if (EmeraldScriptCompat_ValidateBoundary(moduleKey, offset,
                                             surface.boundaryRole)
            != EMERALD_SCRIPT_OK)
        return EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY;
    if (outIdentity == NULL)
        return EMERALD_SCRIPT_STATE_ERR_UNAVAILABLE;
    memset(outIdentity, 0, sizeof(*outIdentity));
    if (!EmeraldScriptCompat_GetStateIdentity(
            moduleKey, &outIdentity->key, &schema, &payloadSize))
        return EMERALD_SCRIPT_STATE_ERR_MISSING_KEY;
    outIdentity->resourceType = GEN3_RESOURCE_TYPE_STRUCTURED_DATA;
    outIdentity->schema = schema;
    outIdentity->representationRole = EMERALD_RESOURCE_ROLE_CANONICAL;
    outIdentity->payloadOffset = offset;
    outIdentity->boundaryRole = surface.boundaryRole;
    outIdentity->generationId = EmeraldScriptCompat_GetGenerationId();
    snprintf(outIdentity->moduleKey, sizeof(outIdentity->moduleKey), "%s",
             moduleKey);
    (void)payloadSize;
    return EMERALD_SCRIPT_STATE_OK;
}

enum EmeraldScriptStateStatus EmeraldScriptState_ResolveField(
    uintptr_t fieldAddress, const Gen3ResourceKey *key,
    uint32_t resourceType, uint32_t schema, uint32_t representationRole,
    uint32_t payloadOffset, const void *serializedSlice,
    uintptr_t liveSliceBase, size_t sliceSize, uintptr_t *outPointer)
{
    struct SurfaceInfo surface = FindSurface(fieldAddress);
    char moduleKey[96];
    enum EmeraldScriptCompatStatus status;

    if (surface.surfaceClass == SURFACE_NONE)
        return EMERALD_SCRIPT_STATE_NOT_SCRIPT;
    NoteSurface(surface.name);
    /* FindSurface uses live activity to decide capture scrubbing.  Restore
     * derives activity from the staged slice instead: the current process's
     * context may be stopped while the state being loaded is active. */
    if (sLayout.context1 != NULL
     && fieldAddress >= (uintptr_t)(void *)sLayout.context1
     && fieldAddress < (uintptr_t)(void *)(sLayout.context1 + 1))
    {
        uintptr_t contextAddress = (uintptr_t)(void *)sLayout.context1;
        uintptr_t contextOffset;
        const struct ScriptContext *saved;
        size_t i;
        if (contextAddress < liveSliceBase)
            return EMERALD_SCRIPT_STATE_ERR_UNAVAILABLE;
        contextOffset = contextAddress - liveSliceBase;
        if (serializedSlice == NULL
         || contextOffset > sliceSize
         || sizeof(*saved) > sliceSize - contextOffset)
            return EMERALD_SCRIPT_STATE_ERR_UNAVAILABLE;
        saved = (const struct ScriptContext *)((const uint8_t *)serializedSlice
                                               + contextOffset);
        if (saved->stackDepth > EMERALD_SCRIPT_STATE_STACK_CAP)
            return EMERALD_SCRIPT_STATE_ERR_CONTEXT1_DEPTH;
        if (saved->mode == 0)
            return EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY;
        if (fieldAddress == (uintptr_t)(void *)&sLayout.context1->scriptPtr)
            surface.boundaryRole = EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START;
        else
        {
            for (i = 0u; i < EMERALD_SCRIPT_STATE_STACK_CAP; i++)
                if (fieldAddress == (uintptr_t)(void *)&sLayout.context1->stack[i])
                    break;
            if (i >= saved->stackDepth)
                return EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY;
            surface.boundaryRole = EMERALD_SCRIPT_BOUNDARY_NEXT_INSTRUCTION;
        }
        surface.surfaceClass = SURFACE_CONTEXT1_RETURN;
    }
    if (surface.surfaceClass == SURFACE_CONTEXT2_TRANSIENT
     || surface.surfaceClass == SURFACE_MYSTERY_IP
     || surface.surfaceClass == SURFACE_MYSTERY_RETURN
     || surface.surfaceClass == SURFACE_MYSTERY_INACTIVE
     || surface.surfaceClass == SURFACE_MYSTERY_BASE)
        return EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY;
    if (EmeraldScriptCompat_ResolveStateIdentity == NULL)
        return EMERALD_SCRIPT_STATE_ERR_UNAVAILABLE;
    status = EmeraldScriptCompat_ResolveStateIdentity(
        key, resourceType, schema, representationRole, payloadOffset,
        surface.boundaryRole, outPointer, moduleKey, sizeof(moduleKey));
    if (status == EMERALD_SCRIPT_ERR_UNEXPECTED_SCHEMA)
        return EMERALD_SCRIPT_STATE_ERR_SCHEMA;
    if (status == EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED)
        return EMERALD_SCRIPT_STATE_ERR_MISSING_KEY;
    if (status == EMERALD_SCRIPT_ERR_BOUNDARY_INVALID)
        return EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY;
    if (status != EMERALD_SCRIPT_OK)
        return EMERALD_SCRIPT_STATE_ERR_UNAVAILABLE;
    return EMERALD_SCRIPT_STATE_OK;
}

bool EmeraldScriptState_IsStaticRecord(uint32_t resourceType,
                                       uint32_t schema,
                                       uint32_t representationRole)
{
    return resourceType == GEN3_RESOURCE_TYPE_STRUCTURED_DATA
        && (schema == 45u || schema == 46u)
        && representationRole == EMERALD_RESOURCE_ROLE_CANONICAL;
}

enum EmeraldScriptStateStatus EmeraldScriptState_ValidateDynamicField(
    uintptr_t fieldAddress, uintptr_t pointer)
{
    struct SurfaceInfo surface = FindSurface(fieldAddress);

    if (surface.surfaceClass == SURFACE_NONE)
        return EMERALD_SCRIPT_STATE_NOT_SCRIPT;
    NoteSurface(surface.name);
    if (pointer == 0u)
        return EMERALD_SCRIPT_STATE_OK;
    if (!surface.dynamicAllowed)
        return EMERALD_SCRIPT_STATE_ERR_UNKNOWN_POINTER;
    return FindDynamic(pointer,
                       surface.surfaceClass != SURFACE_MYSTERY_BASE,
                       NULL, NULL);
}

enum EmeraldScriptStateStatus EmeraldScriptState_BuildVirtualAnchorFromBase(
    uintptr_t liveBase, uint32_t encodedVirtualBase,
    struct EmeraldScriptVirtualAnchor *outAnchor)
{
    const struct EmeraldScriptDynamicBuffer *buffer = NULL;
    size_t offset = 0u;
    enum EmeraldScriptStateStatus status;

    status = FindDynamic(liveBase, false, &buffer, &offset);
    if (status != EMERALD_SCRIPT_STATE_OK || buffer == NULL)
        return status;
    return EmeraldScriptState_BuildVirtualAnchor(buffer, encodedVirtualBase,
                                                    (uint32_t)offset, outAnchor);
}

enum EmeraldScriptStateStatus EmeraldScriptState_BuildVirtualAnchor(
    const struct EmeraldScriptDynamicBuffer *buffer,
    uint32_t encodedVirtualBase, uint32_t liveBaseOffset,
    struct EmeraldScriptVirtualAnchor *outAnchor)
{
    size_t i;

    if (buffer == NULL || outAnchor == NULL
     || liveBaseOffset >= buffer->size)
        return EMERALD_SCRIPT_STATE_ERR_DYNAMIC_BOUNDS;
    for (i = 0u; i < sDynamicBufferCount; i++)
    {
        if (sDynamicBuffers[i].kind == buffer->kind
         && sDynamicBuffers[i].ownerStorageId == buffer->ownerStorageId
         && sDynamicBuffers[i].generation == buffer->generation
         && sDynamicBuffers[i].base == buffer->base
         && sDynamicBuffers[i].size == buffer->size)
        {
            memset(outAnchor, 0, sizeof(*outAnchor));
            outAnchor->encodedVirtualBase = encodedVirtualBase;
            outAnchor->bufferKind = buffer->kind;
            outAnchor->ownerStorageId = buffer->ownerStorageId;
            outAnchor->liveBaseOffset = liveBaseOffset;
            outAnchor->bufferGeneration = buffer->generation;
            outAnchor->valid = 1u;
            return EMERALD_SCRIPT_STATE_OK;
        }
    }
    return EMERALD_SCRIPT_STATE_ERR_DYNAMIC_UNKNOWN;
}

enum EmeraldScriptStateStatus EmeraldScriptState_ResolveVirtualTarget(
    const struct EmeraldScriptVirtualAnchor *anchor, uint32_t encodedTarget,
    size_t width, uintptr_t *outAddress)
{
    size_t i;
    uint64_t relative;
    uint64_t offset;

    if (anchor == NULL || outAddress == NULL || anchor->valid != 1u
     || anchor->reserved != 0u || width == 0u)
        return EMERALD_SCRIPT_STATE_ERR_DYNAMIC_UNKNOWN;
    if (encodedTarget < anchor->encodedVirtualBase)
        return EMERALD_SCRIPT_STATE_ERR_DYNAMIC_BOUNDS;
    relative = (uint64_t)encodedTarget - anchor->encodedVirtualBase;
    offset = (uint64_t)anchor->liveBaseOffset + relative;
    for (i = 0u; i < sDynamicBufferCount; i++)
    {
        const struct EmeraldScriptDynamicBuffer *b = &sDynamicBuffers[i];
        if (b->kind != anchor->bufferKind
         || b->ownerStorageId != anchor->ownerStorageId)
            continue;
        if (b->generation != anchor->bufferGeneration)
            return EMERALD_SCRIPT_STATE_ERR_DYNAMIC_GENERATION;
        if (offset > b->size || width > b->size - (size_t)offset)
            return EMERALD_SCRIPT_STATE_ERR_DYNAMIC_BOUNDS;
        *outAddress = (uintptr_t)b->base + (uintptr_t)offset;
        return EMERALD_SCRIPT_STATE_OK;
    }
    return EMERALD_SCRIPT_STATE_ERR_DYNAMIC_UNKNOWN;
}

const char *EmeraldScriptState_GetLastSurface(void)
{
    return sLastSurface;
}

const char *EmeraldScriptStateStatus_Describe(enum EmeraldScriptStateStatus status)
{
    switch (status)
    {
    case EMERALD_SCRIPT_STATE_OK: return "ok";
    case EMERALD_SCRIPT_STATE_NOT_SCRIPT: return "not a script-state field";
    case EMERALD_SCRIPT_STATE_SCRUB: return "inactive script scratch";
    case EMERALD_SCRIPT_STATE_DYNAMIC: return "captured dynamic script buffer";
    case EMERALD_SCRIPT_STATE_ERR_UNAVAILABLE: return "script state adapter unavailable";
    case EMERALD_SCRIPT_STATE_ERR_CONTEXT1_DEPTH: return "script stackDepth exceeds 20";
    case EMERALD_SCRIPT_STATE_ERR_CONTEXT1_NULL_IP: return "active Context1 has null IP";
    case EMERALD_SCRIPT_STATE_ERR_CONTEXT2_ACTIVE: return "Context2 is active or contains transient execution state";
    case EMERALD_SCRIPT_STATE_ERR_UNKNOWN_POINTER: return "pointer is outside the current staged generation and registered dynamic buffers";
    case EMERALD_SCRIPT_STATE_ERR_STALE_GENERATION: return "pointer state is stamped for a stale script generation";
    case EMERALD_SCRIPT_STATE_ERR_WRONG_SEGMENT: return "pointer targets script data/text/movement hole";
    case EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY: return "pointer has the wrong instruction/return/entrypoint boundary";
    case EMERALD_SCRIPT_STATE_ERR_DYNAMIC_UNKNOWN: return "dynamic script buffer identity is unknown or ambiguous";
    case EMERALD_SCRIPT_STATE_ERR_DYNAMIC_BOUNDS: return "dynamic script offset is out of bounds";
    case EMERALD_SCRIPT_STATE_ERR_DYNAMIC_GENERATION: return "dynamic script buffer generation is stale";
    case EMERALD_SCRIPT_STATE_ERR_VADDRESS_HOST_DELTA: return "legacy creator-process vaddress host delta has no stable anchor";
    case EMERALD_SCRIPT_STATE_ERR_MISSING_KEY: return "script resource key is missing from the staged generation";
    case EMERALD_SCRIPT_STATE_ERR_SCHEMA: return "script resource type/schema/role does not match";
    default: return "unknown script state error";
    }
}

/* R13-H3 State-v5 battle-family execution adapter (shadow/test-only).
 *
 * Staged H pointers use the existing v5 resource sidecar; this adapter
 * owns only family-specific surface and boundary policy. The generic
 * container still owns record layout, ordering, CRCs, capacity, and
 * transactional commit. With no shadow generation present (production
 * through H3) every field returns NOT_BATTLE and the existing
 * image-relative compiled-script path is unchanged. */

#include "emerald/resources/emerald_battle_state.h"

#include <stdio.h>
#include <string.h>

#include "gen3/resources/resource_types.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_battle_compat.h"

/* Production H3 does not link the shadow seam; the adapter falls
 * through to the existing image-relative State-v5 path until H4-H6 link
 * and publish the seam. Focused H3 harnesses link strong definitions
 * and exercise every path below. */
#pragma weak EmeraldBattleCompat_GetGenerationId
#pragma weak EmeraldBattleCompat_ReverseResolve
#pragma weak EmeraldBattleCompat_ValidateBoundary
#pragma weak EmeraldBattleCompat_GetStateIdentity
#pragma weak EmeraldBattleCompat_ResolveStateIdentity
#pragma weak EmeraldBattleCompat_GetArena

enum SurfaceClass
{
    SURFACE_NONE = 0,
    SURFACE_BATTLE_IP,
    SURFACE_BATTLE_SELECTION,
    SURFACE_BATTLE_PALACE,
    SURFACE_BATTLE_STACK,
    SURFACE_BATTLE_STACK_INACTIVE,
    SURFACE_BATTLE_CALLBACK,
    SURFACE_AI_IP,
    SURFACE_AI_STACK,
    SURFACE_AI_STACK_INACTIVE,
    SURFACE_ANIM_IP,
    SURFACE_ANIM_RETURN,
    SURFACE_ANIM_CALLBACK,
    SURFACE_CONTEST_STACK,
    SURFACE_CONTEST_STACK_INACTIVE,
};

struct SurfaceInfo
{
    enum SurfaceClass surfaceClass;
    uint32_t boundaryRole;
    uint32_t family; /* expected family; UINT32_MAX = engine slot */
    const char *name;
};

static struct EmeraldBattleStateLayout sLayout;
static bool sLayoutBound;
static char sLastSurface[96];

static void NoteSurface(const char *name)
{
    if (name == NULL)
        name = "battle-state";
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
    out.family = UINT32_MAX;
    if (!sLayoutBound)
        return out;
    if (PointerSlot(fieldAddress, sLayout.battlescriptCurrInstr))
    {
        out.surfaceClass = SURFACE_BATTLE_IP;
        out.boundaryRole = EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START;
        out.family = EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT;
        out.name = "gBattlescriptCurrInstr";
        return out;
    }
    for (i = 0u; i < 4u; i++)
    {
        if (PointerSlot(fieldAddress, sLayout.selectionScripts[i]))
        {
            out.surfaceClass = SURFACE_BATTLE_SELECTION;
            out.boundaryRole = EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START;
            out.family = EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT;
            out.name = "gSelectionBattleScripts[]";
            return out;
        }
        if (PointerSlot(fieldAddress, sLayout.palaceSelectionScripts[i]))
        {
            out.surfaceClass = SURFACE_BATTLE_PALACE;
            out.boundaryRole = EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START;
            out.family = EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT;
            out.name = "gPalaceSelectionBattleScripts[]";
            return out;
        }
    }
    for (i = 0u; i < EMERALD_BATTLE_STATE_STACK_CAP; i++)
    {
        if (PointerSlot(fieldAddress, sLayout.battleStackPtrs[i]))
        {
            out.surfaceClass = (sLayout.battleStackSize != NULL
                             && i >= *sLayout.battleStackSize)
                             ? SURFACE_BATTLE_STACK_INACTIVE
                             : SURFACE_BATTLE_STACK;
            out.boundaryRole = EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION;
            out.family = EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT;
            out.name = "battleScriptsStack.ptr[]";
            return out;
        }
        if (sLayout.battleCallbacks[i] != NULL
         && fieldAddress == (uintptr_t)(void *)sLayout.battleCallbacks[i])
        {
            out.surfaceClass = SURFACE_BATTLE_CALLBACK;
            out.name = "battleCallbackStack.function[]";
            return out;
        }
    }
    if (PointerSlot(fieldAddress, sLayout.aiScriptPtr))
    {
        out.surfaceClass = SURFACE_AI_IP;
        out.boundaryRole = EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START;
        /* The gAIScriptPtr slot is SHARED between the battle-AI and
         * contest-AI interpreters (the family count marks "either AI"). */
        out.family = EMERALD_BATTLE_FAMILY_COUNT;
        out.name = "gAIScriptPtr";
        return out;
    }
    for (i = 0u; i < EMERALD_BATTLE_STATE_STACK_CAP; i++)
    {
        if (PointerSlot(fieldAddress, sLayout.aiStackPtrs[i]))
        {
            out.surfaceClass = (sLayout.aiStackSize != NULL
                             && i >= *sLayout.aiStackSize)
                             ? SURFACE_AI_STACK_INACTIVE
                             : SURFACE_AI_STACK;
            out.boundaryRole = EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION;
            out.family = EMERALD_BATTLE_FAMILY_BATTLE_AI;
            out.name = "AI_ScriptsStack.ptr[]";
            return out;
        }
    }
    if (PointerSlot(fieldAddress, sLayout.animScriptPtr))
    {
        out.surfaceClass = SURFACE_ANIM_IP;
        out.boundaryRole = EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START;
        out.family = EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT;
        out.name = "sBattleAnimScriptPtr";
        return out;
    }
    if (PointerSlot(fieldAddress, sLayout.animScriptRetAddr))
    {
        out.surfaceClass = SURFACE_ANIM_RETURN;
        out.boundaryRole = EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION;
        out.family = EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT;
        out.name = "sBattleAnimScriptRetAddr";
        return out;
    }
    if (sLayout.animScriptCallback != NULL
     && fieldAddress == (uintptr_t)(void *)sLayout.animScriptCallback)
    {
        out.surfaceClass = SURFACE_ANIM_CALLBACK;
        out.name = "gAnimScriptCallback";
        return out;
    }
    for (i = 0u; i < EMERALD_BATTLE_STATE_STACK_CAP; i++)
    {
        if (PointerSlot(fieldAddress, sLayout.contestStackPtrs[i]))
        {
            out.surfaceClass = (sLayout.contestStackSize != NULL
                             && i >= *sLayout.contestStackSize)
                             ? SURFACE_CONTEST_STACK_INACTIVE
                             : SURFACE_CONTEST_STACK;
            out.boundaryRole = EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION;
            out.family = EMERALD_BATTLE_FAMILY_CONTEST_AI;
            out.name = "ContestAIInfo.stack[]";
            return out;
        }
    }
    return out;
}

void EmeraldBattleState_SetLayout(const struct EmeraldBattleStateLayout *layout)
{
    if (layout == NULL)
    {
        EmeraldBattleState_ClearLayout();
        return;
    }
    sLayout = *layout;
    sLayoutBound = true;
    sLastSurface[0] = '\0';
}

void EmeraldBattleState_ClearLayout(void)
{
    memset(&sLayout, 0, sizeof(sLayout));
    sLayoutBound = false;
    sLastSurface[0] = '\0';
}

static bool ShadowGenerationExists(void)
{
    return EmeraldBattleCompat_GetGenerationId != NULL
        && EmeraldBattleCompat_ReverseResolve != NULL
        && EmeraldBattleCompat_ValidateBoundary != NULL
        && EmeraldBattleCompat_GetStateIdentity != NULL
        && EmeraldBattleCompat_ResolveStateIdentity != NULL
        && EmeraldBattleCompat_GetGenerationId() != 0u;
}

enum EmeraldBattleStateStatus EmeraldBattleState_PrepareCapture(void)
{
    if (!ShadowGenerationExists())
        return EMERALD_BATTLE_STATE_ERR_UNAVAILABLE;
    if (!sLayoutBound)
        return EMERALD_BATTLE_STATE_ERR_UNAVAILABLE;
    if (sLayout.battleStackSize != NULL
     && *sLayout.battleStackSize > EMERALD_BATTLE_STATE_STACK_CAP)
    {
        NoteSurface("battleScriptsStack.size");
        return EMERALD_BATTLE_STATE_ERR_STACK_DEPTH;
    }
    if (sLayout.aiStackSize != NULL
     && *sLayout.aiStackSize > EMERALD_BATTLE_STATE_STACK_CAP)
    {
        NoteSurface("AI_ScriptsStack.size");
        return EMERALD_BATTLE_STATE_ERR_STACK_DEPTH;
    }
    if (sLayout.contestStackSize != NULL
     && *sLayout.contestStackSize > EMERALD_BATTLE_STATE_STACK_CAP)
    {
        NoteSurface("ContestAIInfo.stackSize");
        return EMERALD_BATTLE_STATE_ERR_STACK_DEPTH;
    }
    return EMERALD_BATTLE_STATE_OK;
}

/* Is the pointer inside any current family arena hull (regardless of
 * module-span containment)? Used to turn "unresolvable H pointer" into a
 * precise refusal instead of a generic unmanaged-pointer error. */
static bool InArenaHull(uintptr_t pointer, uint32_t *outFamily)
{
    uint32_t family;

    for (family = 0u; family < EMERALD_BATTLE_FAMILY_COUNT; family++)
    {
        const uint8_t *base = NULL;
        size_t size = 0u;

        if (EmeraldBattleCompat_GetArena == NULL
         || !EmeraldBattleCompat_GetArena(family, &base, &size)
         || base == NULL)
            continue;
        if (pointer >= (uintptr_t)base
         && pointer - (uintptr_t)base < size)
        {
            if (outFamily != NULL)
                *outFamily = family;
            return true;
        }
    }
    return false;
}

enum EmeraldBattleStateStatus EmeraldBattleState_CaptureField(
    uintptr_t fieldAddress, uintptr_t pointer,
    struct EmeraldBattleStateResourceIdentity *outIdentity)
{
    struct SurfaceInfo surface = FindSurface(fieldAddress);
    char moduleKey[96];
    uint32_t offset;
    uint32_t family;
    uint32_t schema;
    uint32_t payloadSize;
    enum EmeraldBattleCompatStatus battleStatus;
    uint32_t arenaFamily;

    if (!ShadowGenerationExists())
        return EMERALD_BATTLE_STATE_NOT_BATTLE;
    if (sLayout.generationStamp != NULL
     && *sLayout.generationStamp != EmeraldBattleCompat_GetGenerationId())
    {
        NoteSurface("battle generation stamp");
        return EMERALD_BATTLE_STATE_ERR_STALE_GENERATION;
    }
    /* Field-effect policy: no persistent FE instruction pointer exists,
     * so an FE-arena pointer in ANY serialized field is prohibited
     * transient state (brief sec 13). */
    if (pointer != 0u && InArenaHull(pointer, &arenaFamily))
    {
        if (arenaFamily == EMERALD_BATTLE_FAMILY_FIELD_EFFECT_SCRIPT)
        {
            NoteSurface(surface.surfaceClass != SURFACE_NONE
                            ? surface.name : "field-effect bytecode");
            return EMERALD_BATTLE_STATE_ERR_TRANSIENT;
        }
    }
    if (surface.surfaceClass == SURFACE_NONE)
    {
        /* An H pointer on a non-H field is a capture error with a
         * precise diagnostic - never a silent generic fallthrough. */
        if (pointer != 0u && InArenaHull(pointer, NULL))
        {
            NoteSurface("non-battle field holding H bytecode");
            return EMERALD_BATTLE_STATE_ERR_WRONG_CLASS;
        }
        return EMERALD_BATTLE_STATE_NOT_BATTLE;
    }
    NoteSurface(surface.name);
    if (surface.surfaceClass == SURFACE_BATTLE_CALLBACK
     || surface.surfaceClass == SURFACE_ANIM_CALLBACK)
    {
        /* Engine function slots: an H bytecode pointer here is a
         * misclassification (brief sec 16) - refuse precisely. */
        if (pointer != 0u && InArenaHull(pointer, NULL))
            return EMERALD_BATTLE_STATE_ERR_WRONG_CLASS;
        return EMERALD_BATTLE_STATE_NOT_BATTLE;
    }
    if (surface.surfaceClass == SURFACE_BATTLE_STACK_INACTIVE
     || surface.surfaceClass == SURFACE_AI_STACK_INACTIVE
     || surface.surfaceClass == SURFACE_CONTEST_STACK_INACTIVE)
    {
        if (pointer != 0u && InArenaHull(pointer, NULL))
            return EMERALD_BATTLE_STATE_ERR_UNKNOWN_POINTER;
        return EMERALD_BATTLE_STATE_SCRUB;
    }
    if (pointer == 0u)
        return EMERALD_BATTLE_STATE_NOT_BATTLE;
    battleStatus = EmeraldBattleCompat_ReverseResolve(
        pointer, moduleKey, sizeof(moduleKey), &offset, &family);
    if (battleStatus != EMERALD_BATTLE_OK)
    {
        if (InArenaHull(pointer, NULL))
            return EMERALD_BATTLE_STATE_ERR_UNKNOWN_POINTER; /* hole/gap */
        return EMERALD_BATTLE_STATE_ERR_UNKNOWN_POINTER;
    }
    if (surface.family == EMERALD_BATTLE_FAMILY_COUNT)
    {
        if (family != EMERALD_BATTLE_FAMILY_BATTLE_AI
         && family != EMERALD_BATTLE_FAMILY_CONTEST_AI)
            return EMERALD_BATTLE_STATE_ERR_WRONG_FAMILY;
    }
    else if (family != surface.family)
        return EMERALD_BATTLE_STATE_ERR_WRONG_FAMILY;
    if (EmeraldBattleCompat_ValidateBoundary(moduleKey, offset,
                                             surface.boundaryRole)
            != EMERALD_BATTLE_OK)
        return EMERALD_BATTLE_STATE_ERR_WRONG_BOUNDARY;
    if (outIdentity == NULL)
        return EMERALD_BATTLE_STATE_ERR_UNAVAILABLE;
    memset(outIdentity, 0, sizeof(*outIdentity));
    if (!EmeraldBattleCompat_GetStateIdentity(
            moduleKey, &outIdentity->key, &schema, &payloadSize))
        return EMERALD_BATTLE_STATE_ERR_MISSING_KEY;
    outIdentity->resourceType = GEN3_RESOURCE_TYPE_STRUCTURED_DATA;
    outIdentity->schema = schema;
    outIdentity->representationRole = EMERALD_RESOURCE_ROLE_CANONICAL;
    outIdentity->payloadOffset = offset;
    outIdentity->boundaryRole = surface.boundaryRole;
    outIdentity->generationId = EmeraldBattleCompat_GetGenerationId();
    snprintf(outIdentity->moduleKey, sizeof(outIdentity->moduleKey), "%s",
             moduleKey);
    (void)payloadSize;
    return EMERALD_BATTLE_STATE_OK;
}

/* Resolve-side activity derivation for the stack surfaces: the live
 * process may be quiescent while the loaded state is mid-battle, so the
 * saved size byte decides active/inactive, exactly like G4's Context1
 * handling. */
static enum EmeraldBattleStateStatus DeriveStackActivity(
    uintptr_t fieldAddress, const void *serializedSlice,
    uintptr_t liveSliceBase, size_t sliceSize,
    const uint8_t ***slots, const uint8_t *sizeByte)
{
    size_t i;

    /* `slots` are the LIVE addresses; their serialized counterparts sit
     * at the same offset within the slice. Find the saved size first. */
    for (i = 0u; i < EMERALD_BATTLE_STATE_STACK_CAP; i++)
    {
        if (PointerSlot(fieldAddress, slots[i]))
            break;
    }
    if (i >= EMERALD_BATTLE_STATE_STACK_CAP)
        return EMERALD_BATTLE_STATE_ERR_WRONG_BOUNDARY;
    if (sizeByte == NULL || serializedSlice == NULL)
        return EMERALD_BATTLE_STATE_ERR_UNAVAILABLE;
    if ((uintptr_t)sizeByte < liveSliceBase)
        return EMERALD_BATTLE_STATE_ERR_UNAVAILABLE;
    {
        uintptr_t sizeOffset = (uintptr_t)sizeByte - liveSliceBase;
        if (sizeOffset > sliceSize || 1u > sliceSize - sizeOffset)
            return EMERALD_BATTLE_STATE_ERR_UNAVAILABLE;
        if (*((const uint8_t *)serializedSlice + sizeOffset)
                > EMERALD_BATTLE_STATE_STACK_CAP)
            return EMERALD_BATTLE_STATE_ERR_STACK_DEPTH;
        if (i >= *((const uint8_t *)serializedSlice + sizeOffset))
            return EMERALD_BATTLE_STATE_ERR_WRONG_BOUNDARY;
    }
    return EMERALD_BATTLE_STATE_OK;
}

enum EmeraldBattleStateStatus EmeraldBattleState_ResolveField(
    uintptr_t fieldAddress, const Gen3ResourceKey *key,
    uint32_t resourceType, uint32_t schema, uint32_t representationRole,
    uint32_t payloadOffset, const void *serializedSlice,
    uintptr_t liveSliceBase, size_t sliceSize, uintptr_t *outPointer)
{
    struct SurfaceInfo surface = FindSurface(fieldAddress);
    char moduleKey[96];
    enum EmeraldBattleCompatStatus status;

    if (!ShadowGenerationExists())
        return EMERALD_BATTLE_STATE_NOT_BATTLE;
    if (surface.surfaceClass == SURFACE_NONE)
        return EMERALD_BATTLE_STATE_NOT_BATTLE;
    NoteSurface(surface.name);
    if (surface.surfaceClass == SURFACE_BATTLE_CALLBACK
     || surface.surfaceClass == SURFACE_ANIM_CALLBACK)
        return EMERALD_BATTLE_STATE_ERR_WRONG_CLASS;
    /* Stack surfaces: derive activity from the SERIALIZED slice. */
    if (surface.surfaceClass == SURFACE_BATTLE_STACK
     || surface.surfaceClass == SURFACE_BATTLE_STACK_INACTIVE)
    {
        enum EmeraldBattleStateStatus derived = DeriveStackActivity(
            fieldAddress, serializedSlice, liveSliceBase, sliceSize,
            sLayout.battleStackPtrs, sLayout.battleStackSize);
        if (derived != EMERALD_BATTLE_STATE_OK)
            return derived;
    }
    else if (surface.surfaceClass == SURFACE_AI_STACK
          || surface.surfaceClass == SURFACE_AI_STACK_INACTIVE)
    {
        enum EmeraldBattleStateStatus derived = DeriveStackActivity(
            fieldAddress, serializedSlice, liveSliceBase, sliceSize,
            sLayout.aiStackPtrs, sLayout.aiStackSize);
        if (derived != EMERALD_BATTLE_STATE_OK)
            return derived;
    }
    else if (surface.surfaceClass == SURFACE_CONTEST_STACK
          || surface.surfaceClass == SURFACE_CONTEST_STACK_INACTIVE)
    {
        enum EmeraldBattleStateStatus derived = DeriveStackActivity(
            fieldAddress, serializedSlice, liveSliceBase, sliceSize,
            sLayout.contestStackPtrs, sLayout.contestStackSize);
        if (derived != EMERALD_BATTLE_STATE_OK)
            return derived;
    }
    status = EmeraldBattleCompat_ResolveStateIdentity(
        key, resourceType, schema, representationRole, payloadOffset,
        surface.boundaryRole, outPointer, moduleKey, sizeof(moduleKey));
    if (status == EMERALD_BATTLE_ERR_UNEXPECTED_SCHEMA
     || status == EMERALD_BATTLE_ERR_TABLE_MISMATCH)
        return EMERALD_BATTLE_STATE_ERR_SCHEMA;
    if (status == EMERALD_BATTLE_ERR_TARGET_UNRESOLVED)
        return EMERALD_BATTLE_STATE_ERR_MISSING_KEY;
    if (status == EMERALD_BATTLE_ERR_BOUNDARY_INVALID)
        return EMERALD_BATTLE_STATE_ERR_WRONG_BOUNDARY;
    if (status == EMERALD_BATTLE_ERR_ALIAS_IDENTITY)
        return EMERALD_BATTLE_STATE_ERR_MISSING_KEY;
    if (status != EMERALD_BATTLE_OK)
        return EMERALD_BATTLE_STATE_ERR_UNAVAILABLE;
    /* The resolved module's family must match the surface's expected
     * family (schema check alone is per-family but this is the explicit
     * wrong-family gate). */
    {
        char resolvedKey[96];
        uint32_t family = UINT32_MAX;
        if (EmeraldBattleCompat_ReverseResolve(
                *outPointer, resolvedKey, sizeof(resolvedKey), NULL,
                &family) == EMERALD_BATTLE_OK
         && family != surface.family
         && !(surface.family == EMERALD_BATTLE_FAMILY_COUNT
              && (family == EMERALD_BATTLE_FAMILY_BATTLE_AI
               || family == EMERALD_BATTLE_FAMILY_CONTEST_AI)))
            return EMERALD_BATTLE_STATE_ERR_WRONG_FAMILY;
    }
    return EMERALD_BATTLE_STATE_OK;
}

bool EmeraldBattleState_IsStaticRecord(uint32_t resourceType,
                                       uint32_t schema,
                                       uint32_t representationRole)
{
    return resourceType == GEN3_RESOURCE_TYPE_STRUCTURED_DATA
        && schema >= 47u && schema <= 51u
        && representationRole == EMERALD_RESOURCE_ROLE_CANONICAL;
}

const char *EmeraldBattleState_GetLastSurface(void)
{
    return sLastSurface;
}

const char *EmeraldBattleStateStatus_Describe(enum EmeraldBattleStateStatus status)
{
    switch (status)
    {
    case EMERALD_BATTLE_STATE_OK: return "ok";
    case EMERALD_BATTLE_STATE_NOT_BATTLE: return "not a battle-state field";
    case EMERALD_BATTLE_STATE_SCRUB: return "inactive battle stack scratch";
    case EMERALD_BATTLE_STATE_ERR_UNAVAILABLE: return "battle state adapter unavailable";
    case EMERALD_BATTLE_STATE_ERR_STACK_DEPTH: return "battle/AI/contest stack size exceeds 8";
    case EMERALD_BATTLE_STATE_ERR_STALE_GENERATION: return "pointer state is stamped for a stale battle generation";
    case EMERALD_BATTLE_STATE_ERR_UNKNOWN_POINTER: return "pointer is outside every staged battle module span";
    case EMERALD_BATTLE_STATE_ERR_WRONG_FAMILY: return "pointer family does not match the destination surface";
    case EMERALD_BATTLE_STATE_ERR_WRONG_CLASS: return "H bytecode found on an engine/non-battle surface";
    case EMERALD_BATTLE_STATE_ERR_WRONG_BOUNDARY: return "pointer has the wrong instruction/return/entrypoint boundary";
    case EMERALD_BATTLE_STATE_ERR_TRANSIENT: return "prohibited transient execution state (field-effect bytecode has no persistent IP)";
    case EMERALD_BATTLE_STATE_ERR_MISSING_KEY: return "battle resource key is missing from the staged generation";
    case EMERALD_BATTLE_STATE_ERR_SCHEMA: return "battle resource type/schema/role does not match";
    default: return "unknown battle state error";
    }
}

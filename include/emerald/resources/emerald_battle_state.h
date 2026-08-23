#ifndef EMERALD_RESOURCES_EMERALD_BATTLE_STATE_H
#define EMERALD_RESOURCES_EMERALD_BATTLE_STATE_H

/* R13-H3: State-v5 battle-family execution adapter (shadow/test-only).
 *
 * This is a state-family adapter, not a second state format. Staged H
 * execution pointers become the existing v5 64-byte resource record
 * (module key + type/schema/role + payload offset); the boundary role is
 * determined by the exact destination field, so no serialized field is
 * added and no format changes (brief sec 1).
 *
 * Surfaces (exact production structures, brief sec 5):
 *
 *   gBattlescriptCurrInstr                 INSTRUCTION_START
 *   gSelectionBattleScripts[4]             INSTRUCTION_START
 *   gPalaceSelectionBattleScripts[4]       INSTRUCTION_START
 *   gBattleResources->battleScriptsStack   NEXT_INSTRUCTION (size-gated)
 *   gBattleResources->battleCallbackStack  ENGINE (never H - function
 *                                          image, generic path)
 *   gAIScriptPtr                           INSTRUCTION_START (quiescent
 *                                          stale IP; relocate, never
 *                                          execute - next turn re-derives)
 *   gBattleResources->AI_ScriptsStack      NEXT_INSTRUCTION (size-gated;
 *                                          size 0 at every VBlank by
 *                                          construction; relocation-safe
 *                                          for test/debug capture)
 *   sBattleAnimScriptPtr                   INSTRUCTION_START
 *   sBattleAnimScriptRetAddr               NEXT_INSTRUCTION
 *   gAnimScriptCallback                    ENGINE (function image)
 *   gContestResources->aiData stack[8]     NEXT_INSTRUCTION
 *                                          (contestSize-gated; contest
 *                                          resources never exist during
 *                                          a battle capture)
 *
 * Field-effect bytecode has NO persistent instruction pointer (the
 * interpreter cursor is function-local, field_effect.c:705): capture
 * cannot legally observe active field-effect execution, so a
 * field-effect arena pointer in any serialized slice is an explicit
 * policy refusal (brief sec 13).
 *
 * Production behavior through H3: production links only this weak
 * adapter; the shadow seam and its generated table are harness-linked.
 * With no shadow generation present the adapter returns NOT_BATTLE for
 * every field, so the existing image-relative compiled-script path is
 * byte-for-byte unchanged (brief sec 28/29). Once a shadow generation
 * exists (the H3 harness today, H4-H6 later), a pointer on an exact H
 * surface must belong to the current generation or be refused.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_id.h"

#define EMERALD_BATTLE_STATE_STACK_CAP 8u

enum EmeraldBattleStateStatus
{
    EMERALD_BATTLE_STATE_OK = 0,
    EMERALD_BATTLE_STATE_NOT_BATTLE,
    EMERALD_BATTLE_STATE_SCRUB,
    EMERALD_BATTLE_STATE_ERR_UNAVAILABLE,
    EMERALD_BATTLE_STATE_ERR_STACK_DEPTH,
    EMERALD_BATTLE_STATE_ERR_STALE_GENERATION,
    EMERALD_BATTLE_STATE_ERR_UNKNOWN_POINTER, /* H pointer outside every span */
    EMERALD_BATTLE_STATE_ERR_WRONG_FAMILY,
    EMERALD_BATTLE_STATE_ERR_WRONG_CLASS,     /* H bytecode on an engine surface */
    EMERALD_BATTLE_STATE_ERR_WRONG_BOUNDARY,
    EMERALD_BATTLE_STATE_ERR_TRANSIENT,       /* prohibited active state (FE) */
    EMERALD_BATTLE_STATE_ERR_MISSING_KEY,
    EMERALD_BATTLE_STATE_ERR_SCHEMA,
};

/* Exact destination-field addresses for every H pointer surface. The
 * stack surfaces are the address of the ptr[] member slots and the size
 * byte - the harness binds its exact production-shaped fixtures; the
 * future H4-H6 production cutovers bind the real EWRAM/heap objects. */
struct EmeraldBattleStateLayout
{
    const uint8_t **battlescriptCurrInstr;
    const uint8_t **selectionScripts[EMERALD_BATTLE_STATE_STACK_CAP / 2u];
    const uint8_t **palaceSelectionScripts[EMERALD_BATTLE_STATE_STACK_CAP / 2u];
    const uint8_t **battleStackPtrs[EMERALD_BATTLE_STATE_STACK_CAP];
    uint8_t *battleStackSize;
    void (**battleCallbacks[EMERALD_BATTLE_STATE_STACK_CAP])(void);
    const uint8_t **aiScriptPtr;
    const uint8_t **aiStackPtrs[EMERALD_BATTLE_STATE_STACK_CAP];
    uint8_t *aiStackSize;
    const uint8_t **animScriptPtr;
    const uint8_t **animScriptRetAddr;
    void (**animScriptCallback)(void);
    const uint8_t **contestStackPtrs[EMERALD_BATTLE_STATE_STACK_CAP];
    uint8_t *contestStackSize;
    const uint64_t *generationStamp; /* harness fixture; validation-only */
};

struct EmeraldBattleStateResourceIdentity
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

void EmeraldBattleState_SetLayout(const struct EmeraldBattleStateLayout *layout);
void EmeraldBattleState_ClearLayout(void);

/* Whole-capture preflight: structural depth checks (stack sizes within
 * the 8-slot caps). Runs before the generic walker can emit any sidecar
 * record, preserving all-or-nothing capture. */
enum EmeraldBattleStateStatus EmeraldBattleState_PrepareCapture(void);

/* Classify one exact pointer field. OK means emit the returned ordinary
 * v5 resource record; SCRUB means zero inactive stack scratch; NOT_BATTLE
 * means continue with the existing generic walker. H-arena pointers on
 * non-H fields, engine slots holding H bytecode, wrong-family pointers,
 * holes/gaps, stale generations and transient prohibited state all
 * refuse with a precise status. */
enum EmeraldBattleStateStatus EmeraldBattleState_CaptureField(
    uintptr_t fieldAddress, uintptr_t pointer,
    struct EmeraldBattleStateResourceIdentity *outIdentity);

/* Resolve one existing sidecar record for an exact destination field.
 * Activity for stack slots derives from the serialized slice (the live
 * process may be quiescent while the loaded state is mid-battle). */
enum EmeraldBattleStateStatus EmeraldBattleState_ResolveField(
    uintptr_t fieldAddress, const Gen3ResourceKey *key,
    uint32_t resourceType, uint32_t schema, uint32_t representationRole,
    uint32_t payloadOffset, const void *serializedSlice,
    uintptr_t liveSliceBase, size_t sliceSize, uintptr_t *outPointer);

bool EmeraldBattleState_IsStaticRecord(uint32_t resourceType,
                                       uint32_t schema,
                                       uint32_t representationRole);

const char *EmeraldBattleState_GetLastSurface(void);
const char *EmeraldBattleStateStatus_Describe(enum EmeraldBattleStateStatus status);

#endif /* EMERALD_RESOURCES_EMERALD_BATTLE_STATE_H */

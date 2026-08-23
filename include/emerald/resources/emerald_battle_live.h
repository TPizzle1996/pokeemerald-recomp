#ifndef EMERALD_RESOURCES_EMERALD_BATTLE_LIVE_H
#define EMERALD_RESOURCES_EMERALD_BATTLE_LIVE_H

/* R13-H4: production live seam for the battle-anim + field-effect arenas.
 *
 * This seam is the FIRST live H-family cutover (plan sec 17). It stages
 * the anim (63,811 B) and field-effect (817 B) arena payloads from the
 * pack into one host buffer, registers exactly two live ranges (6,377 ->
 * 6,379), publishes live execution, and then resolves every animation /
 * field-effect script pointer the interpreters read through a typed,
 * metadata-driven path - NEVER through HostResolveGbaAddr identity
 * arithmetic and NEVER back to compiled payloads (brief sec 8/9/23).
 *
 * Transaction order (brief sec 4): validate anim surface -> validate FE
 * surface -> stage generation -> register the 2 ranges -> publish. No
 * partial cutover: every failure before publish leaves the compiled
 * runtime untouched; every failure after ranges registered rolls the
 * ranges back. The loader drives this and refuses the session on any
 * failure (see emerald_runtime_loader.c).
 *
 * The weak state-adapter bridge (emerald_battle_state.c) resolves
 * through the six EmeraldBattleCompat_* functions this seam provides
 * (same symbols as the harness-only H3 shadow seam; the two never link
 * into the same binary). The bridge covers anim + FE only: battle/AI/
 * contest surfaces gate NOT_BATTLE (family-live policy).
 *
 * Refuse-only bindings: four engine bindings whose native symbols do not
 * exist in this fork (upstream battle_anim_mist.c/battle_anim_terrain.c
 * are absent) resolve to address 0 and the resolver hard-refuses them
 * (brief sec 10/24) - the affected moves' animations end cleanly and the
 * battle continues.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emerald/resources/emerald_battle_compat.h" /* statuses + bridge */

#define EMERALD_BATTLE_LIVE_RANGE_COUNT 2u

enum EmeraldBattleLiveStatus
{
    EMERALD_BATTLE_LIVE_OK = 0,
    EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT,
    EMERALD_BATTLE_LIVE_ERR_OUT_OF_MEMORY,
    EMERALD_BATTLE_LIVE_ERR_RESOLVE_FAILED,
    EMERALD_BATTLE_LIVE_ERR_UNEXPECTED_OWNERSHIP,
    EMERALD_BATTLE_LIVE_ERR_PAYLOAD_SIZE_MISMATCH,
    EMERALD_BATTLE_LIVE_ERR_UNEXPECTED_COUNT,
    EMERALD_BATTLE_LIVE_ERR_UNEXPECTED_SCHEMA,
    EMERALD_BATTLE_LIVE_ERR_TABLE_MISMATCH,
    EMERALD_BATTLE_LIVE_ERR_ALIAS_IDENTITY,
    EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID,
    EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED,
    EMERALD_BATTLE_LIVE_ERR_STAGING_FAILED,
    EMERALD_BATTLE_LIVE_ERR_UNAVAILABLE,
    EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED,
    EMERALD_BATTLE_LIVE_ERR_REFUSED,
    EMERALD_BATTLE_LIVE_ERR_BUSY,
    EMERALD_BATTLE_LIVE_ERR_RANGE_REGISTRATION,
    EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY,
};

/* Loader transaction (once per session; any failure = session refused).
 * `layout` is the physical arena layout: 0 = GBA-preserving (production),
 * 1 = tight-packed reversed (semantic-identity perturbation proofs). */
enum EmeraldBattleLiveStatus EmeraldBattleLive_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack, uint32_t layout,
    struct EmeraldBattleCompatDiagnostics *diagnostics);
enum EmeraldBattleLiveStatus EmeraldBattleLive_RegisterRanges(void);
enum EmeraldBattleLiveStatus EmeraldBattleLive_Publish(void);
void EmeraldBattleLive_ClearMigratedEntries(void);
void EmeraldBattleLive_UnregisterRanges(void);
void EmeraldBattleLive_UnregisterRange(const char *canonicalName);
size_t EmeraldBattleLive_GetRangeCount(void);
bool EmeraldBattleLive_IsPublished(void);
uint64_t EmeraldBattleLive_GetGenerationId(void);

/* Execution resolution (only after Publish; ERR_NOT_PUBLISHED before). */
enum EmeraldBattleLiveStatus EmeraldBattleLive_ResolveScriptTarget(
    uint32_t family, uint32_t word, uintptr_t *outPointer);
enum EmeraldBattleLiveStatus EmeraldBattleLive_ResolveOperand(
    uintptr_t operandHostAddress, uintptr_t *outPointer);
/* Launch-target resolution: a trusted launch-table word (a
 * gBattleAnims_* / gFieldEffectScriptFuncs entry) denoting a module
 * root export of the requested family. Unlike ResolveScriptTarget, no
 * SCRIPT_TARGET word-set gate applies - launch tables legitimately
 * reference modules no script operand jumps to. The word must still
 * contain to a live payload module of the requested family at its root
 * export (offset 0). */
enum EmeraldBattleLiveStatus EmeraldBattleLive_ResolveLaunchTarget(
    uint32_t family, uint32_t word, uintptr_t *outPointer);

const char *EmeraldBattleLiveStatus_Describe(
    enum EmeraldBattleLiveStatus status);

#endif /* EMERALD_RESOURCES_EMERALD_BATTLE_LIVE_H */

#ifndef EMERALD_RESOURCES_EMERALD_BATTLE_LIVE_H
#define EMERALD_RESOURCES_EMERALD_BATTLE_LIVE_H

/* R13-H4/H5/H6: production live seam for the battle + battle-anim +
 * battle-AI + contest-AI + field-effect arenas.
 *
 * H4 made the FIRST live H-family cutover (anim + field-effect, 2
 * ranges, 6,377 -> 6,379). H5 adds the battle-script family (3 ranges,
 * 6,380). H6 adds the battle-AI and contest-AI families (5 ranges,
 * 6,382): the seam stages all five arenas (battle 14,413 B hull, 640
 * payload modules; battle_anim 63,811 B; battle_ai 9,303 B, 553
 * modules; contest_ai 2,524 B, 165 modules; field_effect 817 B) from
 * the pack into one host buffer, registers exactly five live ranges,
 * publishes live execution, and resolves every battle/anim/AI/contest/
 * field-effect script pointer the interpreters read through a typed,
 * metadata-driven path - NEVER through HostResolveGbaAddr identity
 * arithmetic and NEVER back to compiled payloads (brief sec 8/9/23).
 *
 * Battle/AI-specific resolution (H5/H6):
 *  - the central pointer-operand reader (EmeraldBattleLive_ReadPointerOperand)
 *    sits inside T1_READ_PTR/T2_READ_PTR: operand words read from any
 *    live arena span resolve through the relocation source index (typed
 *    semantic target) - including every AI control-flow operand
 *    (branch/jump/call/tail-call; brief sec 7); words read anywhere
 *    else keep the legacy path;
 *  - EWRAM operands resolve as semantic base + validated addend
 *    (native address from the battle_live_native.generated.c TU);
 *  - the 7 routing tables (5 battle + gBattleAI_ScriptsTable 32 rows +
 *    gContestAI_ScriptsTable 32 rows) are arena-owned: rows are read
 *    from the canonical arena bytes and each row word resolves as a
 *    SCRIPT_TARGET root of the table's own family (the compiled tables
 *    are dead at runtime; the AI entry tables are the only entry
 *    surfaces for the AI VMs, brief sec 11);
 *  - direct C label references (BattleScript_Get) resolve through the
 *    compiled-label map (native symbol -> canonical GBA word -> arena
 *    root export);
 *  - data-target relocs (38 battle-AI if_in_* byte/hword list tables)
 *    resolve at offset 0 by map kind - no instruction boundaries
 *    (brief sec 6).
 *
 * Transaction order (brief sec 3/4): validate pack surfaces -> stage
 * generation -> register the 5 ranges -> publish. No partial cutover:
 * every failure before publish leaves the compiled runtime untouched;
 * every failure after ranges registered rolls the ranges back. The
 * loader drives this and refuses the session on any failure (see
 * emerald_runtime_loader.c).
 *
 * The weak state-adapter bridge (emerald_battle_state.c) resolves
 * through the six EmeraldBattleCompat_* functions this seam provides
 * (same symbols as the harness-only H3 shadow seam; the two never link
 * into the same binary). The bridge covers all five live families: the
 * shared gAIScriptPtr surface accepts either AI family (FAMILY_COUNT
 * sentinel) and flips live automatically via GetArena once the AI
 * arenas publish (R13-H3 sec 15/16 policy unchanged).
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
#include "emerald/resources/battle_live.generated.h" /* table + routing enum */

#define EMERALD_BATTLE_LIVE_RANGE_COUNT 5u

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

/* H5/H6 routing: `tableWord` is a routing-table GBA word
 * (EMERALD_BATTLE_ROUTING_*, whose values ARE the canonical words); the
 * canonical row bytes are read from the ARENA (the compiled tables are
 * dead at runtime) and the row word resolves as a SCRIPT_TARGET root of
 * the table's own family (battle move-effects/ball-throw/using-item/
 * running-by-item/safari-actions + battle-AI + contest-AI entry
 * tables). */
enum EmeraldBattleLiveStatus EmeraldBattleLive_ResolveRoutingTarget(
    uint32_t tableWord, uint32_t rowIndex, uintptr_t *outPointer);

/* H5 battle direct entry: `word` is a canonical battle root GBA address
 * (from the compiled-label map - a generator-validated constant, never
 * a runtime byte). Resolves to the module's export at the word. */
enum EmeraldBattleLiveStatus EmeraldBattleLive_GetBattleScript(
    uint32_t word, uintptr_t *outPointer);

/* H5 compiled-label translation: the native address of a compiled
 * battle script label symbol (BattleScript_* as linked in the host
 * binary) -> its canonical GBA word, then arena resolution. Refuses
 * (never falls back to the compiled bytes) when the label is not in
 * the generated map or the battle arena is not published. */
enum EmeraldBattleLiveStatus EmeraldBattleLive_ResolveCompiledLabel(
    const void *nativeSymbolAddress, uintptr_t *outPointer);

/* H5/H6 central pointer-operand reader (the T1_READ_PTR/T2_READ_PTR
 * body on linux64): operand words read from any live arena span (the
 * five battle/AI families) resolve through the typed relocation source
 * index (word-checked, class-dispatched; legal NULL literals return
 * NULL; everything else is a hard fail-closed refusal). Every AI
 * control-flow operand - branch/jump/call/tail-call - funnels through
 * this reader (R13-H6 sec 7: control-flow resolution at consumption,
 * no scattered raw address resolution). Words read anywhere else keep
 * the legacy HostResolveGbaAddr path (host-pointer identity). */
void *EmeraldBattleLive_ReadPointerOperand(const uint8_t *operandAddress);

/* H5 fail-closed C-site accessors: a compiled battle label symbol ->
 * its live arena root, and a routing-table row -> its live arena
 * script root. Both hard-refuse (never fall back to compiled bytes). */
const uint8_t *EmeraldBattleLive_BattleScriptPtr(
    const void *nativeSymbolAddress);
const uint8_t *EmeraldBattleLive_RoutingScriptPtr(uint32_t tableWord,
                                                  uint32_t rowIndex);

const char *EmeraldBattleLiveStatus_Describe(
    enum EmeraldBattleLiveStatus status);

#endif /* EMERALD_RESOURCES_EMERALD_BATTLE_LIVE_H */

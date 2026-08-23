#ifndef EMERALD_RESOURCES_EMERALD_BATTLE_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_BATTLE_COMPAT_H

/* R13-H3: battle-family shadow staging + State-v5 identity bridge.
 *
 * SHADOW MODE ONLY (plan sec 17, H3 wave): this seam builds a COMPLETE
 * candidate battle-family generation - five deterministic family arenas
 * holding the 2,081 payload modules' exact canonical bytes (the qualified
 * ROM slices, never rewritten), the per-module boundary + export indexes,
 * the eight zero-width alias identities canonicalized to their payload
 * owner, reverse containment, and the five-family projected-range
 * arithmetic - and publishes it ONLY to its own private shadow state.
 *
 * NOTHING in this seam mutates live execution state: the battle, anim,
 * AI, contest and field-effect VMs keep executing compiled content
 * through H3, no H range is registered with the production State-v5
 * range index (live count stays 6,377, zero H ranges), and no
 * interpreter is touched. H3 is a state-readiness dry run; H4-H6 own the
 * live cutovers.
 *
 * Physical layout is a per-generation choice: layout 0 reproduces the
 * h2_stage.py geometry (GBA-preserving within each arena, arenas in
 * sidecar order); layout 1 perturbs it (modules tight-packed in module-id
 * order, arenas in reversed order). Both must resolve the same semantic
 * identity - a persisted state stores family/module/offset, never an
 * aggregate offset (brief sec 21/22).
 *
 * Transactional: phase 1 validates the session pack against the
 * generated inventory (exactly 2,081 payload records across schemas
 * 47-51, digest/size/type equality, the 8 zero-width alias identities
 * absent, ROM_BASE ownership via the snapshot); phase 2 allocates the
 * arenas, copies the exact bytes and builds the per-generation indexes;
 * phase 3 swaps the shadow generation atomically. A failed restage
 * preserves the previous valid generation; each generation carries a
 * monotonic identity used by the state adapter's staleness checks.
 *
 * Platform-neutral (no global.h/engine/frontend objects) so the offline
 * test harness can compile and drive it directly. Production links only
 * the weak state adapter (emerald_battle_state.c); this seam and its
 * generated table are harness-linked through H3.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/battle_native.generated.h"

#define EMERALD_BATTLE_KEY_CAP 96u

enum EmeraldBattleCompatStatus
{
    EMERALD_BATTLE_OK = 0,
    EMERALD_BATTLE_ERR_INVALID_ARGUMENT,
    EMERALD_BATTLE_ERR_OUT_OF_MEMORY,
    EMERALD_BATTLE_ERR_RESOLVE_FAILED,   /* snapshot did not resolve a module */
    EMERALD_BATTLE_ERR_UNEXPECTED_OWNERSHIP, /* winner is not ROM_BASE */
    EMERALD_BATTLE_ERR_PAYLOAD_SIZE_MISMATCH,
    EMERALD_BATTLE_ERR_UNEXPECTED_COUNT, /* pack surface != generated pins */
    EMERALD_BATTLE_ERR_UNEXPECTED_SCHEMA,
    EMERALD_BATTLE_ERR_TABLE_MISMATCH,   /* pack record disagrees with table */
    EMERALD_BATTLE_ERR_ALIAS_IDENTITY,   /* zero-width alias must canonicalize */
    EMERALD_BATTLE_ERR_BOUNDARY_INVALID,
    EMERALD_BATTLE_ERR_TARGET_UNRESOLVED,
    EMERALD_BATTLE_ERR_STAGING_FAILED,
    EMERALD_BATTLE_ERR_UNAVAILABLE,      /* no shadow generation */
};

struct EmeraldBattleCompatDiagnostics
{
    char canonicalName[96];
    char stage[24];                /* "validate" / "stage" / "commit" */
    char expectedType[24];
    char actualType[24];
    uint32_t expectedSchema;
    uint32_t actualSchema;
    uint32_t expectedSize;
    uint32_t actualSize;
    char winningProviderId[64];
    char winningProviderVersion[32];
    uint32_t winningProviderPrecedence;
};

/* Boundary roles (brief sec 17/18). INSTRUCTION_START and
 * NEXT_INSTRUCTION both require an exact bytecode-map instruction start
 * (runtime return addresses are interior instruction boundaries by
 * construction - IP+5 etc.; H1's interior RELOCATION count 0 says
 * nothing about runtime returns). ENTRYPOINT requires a generated
 * export at the offset. Middle-of-operand, holes, padding, data/routing
 * spans, zero-width aliases, wrong-family keys and past-end offsets all
 * refuse. */
enum EmeraldBattleBoundaryKind
{
    EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START = 0,
    EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION = 1,
    EMERALD_BATTLE_BOUNDARY_ENTRYPOINT = 2,
};

struct EmeraldBattleCompatIndexCounts
{
    uint32_t modules;             /* 2,089 semantic (incl. 8 aliases) */
    uint32_t payloadModules;      /* 2,081 */
    uint32_t aliases;             /* 8 */
    uint32_t boundaries;          /* instruction-boundary rows */
    uint32_t exportRows;          /* 2,109 */
    uint32_t arenas;              /* 5 */
};

/* Validate the session pack against the generated battle inventory,
 * stage the candidate generation with the requested physical layout and
 * shadow-commit it. Transactional; a failed restage preserves the
 * previous shadow generation. */
enum EmeraldBattleCompatStatus
EmeraldBattleCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    uint32_t layout,
    struct EmeraldBattleCompatDiagnostics *diagnostics);

void EmeraldBattleCompat_ClearMigratedEntries(void);
void EmeraldBattleCompat_Shutdown(void);

uint64_t EmeraldBattleCompat_GetGenerationId(void);

/* Per-family arena span in the current generation (host address + size).
 * Returns false when no generation exists. */
bool EmeraldBattleCompat_GetArena(uint32_t family,
                                  const uint8_t **outBase, size_t *outSize);
bool EmeraldBattleCompat_GetModuleSpan(const char *moduleKey,
                                       const uint8_t **outBase,
                                       size_t *outSize);

/* Reverse containment: a live host pointer inside exactly one staged
 * payload-module span -> module key + payload offset + family. Refuses
 * hull holes, inter-module gaps, the battle hull's unmapped FE region,
 * zero-width alias identities (they own no bytes - a pointer at a shared
 * base belongs to the payload owner), and stale-generation pointers
 * (outside every current span). O(log n) over the 2,081 spans. */
enum EmeraldBattleCompatStatus
EmeraldBattleCompat_ReverseResolve(uintptr_t address,
                                   char *outModuleKey, size_t keyCap,
                                   uint32_t *outOffset, uint32_t *outFamily);

/* Boundary validation (brief sec 17): as described on the boundary-kind
 * enum. Zero-width alias keys refuse with ALIAS_IDENTITY so state
 * identity always canonicalizes to the payload owner first. */
enum EmeraldBattleCompatStatus
EmeraldBattleCompat_ValidateBoundary(const char *moduleKey, uint32_t offset,
                                     uint32_t boundaryKind);

/* State-v5 identity bridge. The serialized form is the ordinary 64-byte
 * sidecar record (STRUCTURED_DATA, the module's schema 47-51, role
 * CANONICAL, rangeOffset = module payload offset); the boundary role is
 * derived from the exact pointer surface, never stored in a new field.
 * Zero-width alias keys redirect to the canonical payload owner. */
bool EmeraldBattleCompat_GetStateIdentity(
    const char *moduleKey, Gen3ResourceKey *outKey, uint32_t *outSchema,
    uint32_t *outPayloadSize);
enum EmeraldBattleCompatStatus EmeraldBattleCompat_ResolveStateIdentity(
    const Gen3ResourceKey *key, uint32_t resourceType, uint32_t schema,
    uint32_t representationRole, uint32_t payloadOffset,
    uint32_t boundaryRole, uintptr_t *outAddress,
    char *outModuleKey, size_t keyCap);

/* The five-family projected range arithmetic (brief sec 2/20): the H4+
 * live world registers exactly one range per family arena. The dry-run
 * validates currentCount + 5 <= capacity, per-arena span containment in
 * the current generation, and pairwise non-overlap. */
enum EmeraldBattleCompatStatus EmeraldBattleCompat_ValidateProjectedRanges(
    size_t currentRangeCount, size_t rangeCapacity,
    size_t *outProjectedRangeCount);

/* The arena-range descriptors for the dry-run transaction: the exact
 * synthetic identity ("emerald:<family>/@arena" derived key, the
 * family schema, CANONICAL role) the future live registration will use. */
struct EmeraldBattleCompatArenaRange
{
    char canonicalName[EMERALD_BATTLE_KEY_CAP];
    Gen3ResourceKey key;
    uint32_t resourceType;
    uint32_t schema;
    uint32_t role;
    uintptr_t base;
    size_t size;
};

bool EmeraldBattleCompat_GetArenaRanges(
    struct EmeraldBattleCompatArenaRange outRanges[EMERALD_BATTLE_FAMILY_COUNT]);

bool EmeraldBattleCompat_GetIndexCounts(struct EmeraldBattleCompatIndexCounts *outCounts);

const char *EmeraldBattleCompatStatus_Describe(enum EmeraldBattleCompatStatus status);

#endif /* EMERALD_RESOURCES_EMERALD_BATTLE_COMPAT_H */

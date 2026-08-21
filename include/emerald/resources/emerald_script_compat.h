#ifndef EMERALD_RESOURCES_EMERALD_SCRIPT_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_SCRIPT_COMPAT_H

/* R13-G3: Emerald field-script shadow staging + typed resolver seam.
 *
 * SHADOW MODE ONLY (plan sec 17, G3 wave): this seam builds a COMPLETE
 * candidate script generation - deterministic arena with all 523 module
 * spans (exact G2 payload bytes, no operand patching), the 16,704-row
 * source relocation index, the typed target/binding index, export and
 * boundary indexes, reverse containment, the staged 11-entry
 * gStdScripts candidate table and the staged 3,501-row R13-F inbound
 * rebind plan - and publishes it ONLY to its own private shadow state.
 *
 * NOTHING in this seam mutates live execution state: the running VM
 * keeps executing compiled field scripts, gStdScripts stays compiled,
 * the R13-F map/event pointers are untouched, ScriptReadPointer and
 * sAddressOffset are untouched, and NO range is registered with the
 * State-v5 range index (the shadow arena is intentionally invisible to
 * the range walker; a session that captures pointers into it would be
 * corrupt by definition - G4/G5 wire the arena in through the normal
 * publication path before any save can see it). Live publication of
 * gStdScripts, the F rebind and range registration is G5's job.
 *
 * REFUSE-CLASS and transactional (plan sec 11): phase 1 validates the
 * generated inventory against the session's pack (exactly 467 embedded
 * module records: schema/type/digest/size equality, the 56
 * routing-only identities absent), the structural table gates (counts,
 * sortedness, packing, operand non-overlap), every typed target
 * resolution (8,208 script incl. 95 root / 8,113 interior, 6,207 text
 * into the R13-C published catalog, 2,009 movement into the R13-B
 * binding surface + 3 named compiled bridges, 262 static-data, 18 RAM
 * against the one allowlisted host symbol), the 16,704-row dynamic
 * parity oracle (every relocation's encoded operand resolves through
 * the encoded-GBA target index to the identical canonical identity),
 * the 11 gStdScripts records, the 3,501 F inbound bindings and the
 * instruction-boundary model; phase 2 allocates the arena, copies the
 * exact bytes, re-proves every operand's raw bytes against the
 * recorded encoded values, and builds the per-generation indexes;
 * phase 3 swaps the shadow generation atomically. A failed restage
 * preserves the previous valid generation (each generation carries an
 * explicit monotonic identity for G4/G5).
 *
 * The instruction-boundary model is the G1 census walk (proven roots,
 * break on unknown opcode/terminal) plus a supplementary walk seeded
 * from every script entry point; bytes neither walk decoded are
 * opaque zones (data or dynamically-reached code): instruction queries
 * refuse there, export identities still resolve. Static-data segments
 * (mart tables) are typed-data overlays that an instruction may
 * legitimately start inside (trainerbattle inline metadata).
 *
 * Sibling-seam contract: TryInitialize requires the R13-C text seam
 * AND the R13-B leaf seam to be published first (the loader's normal
 * ordering); a missing sibling publication is a refused shadow stage,
 * never a partial one.
 *
 * Platform-neutral (no global.h/engine/frontend objects) so the
 * offline test harness can compile and drive it directly. The compiled
 * movement-bridge pointers and the gStringVar4 RAM target come from
 * the generated table's extern references, not from this seam.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/script_native.generated.h"
#include "emerald/resources/text_bundle_index.generated.h"

#define EMERALD_SCRIPT_KEY_CAP   96u
#define EMERALD_SCRIPT_LABEL_CAP 96u

/* Compile-time agreement between the generated inventory and the G2
 * pins (plan sec 1/15): a regenerated table that disagrees is a build
 * error, not a runtime surprise. */
#if EMERALD_SCRIPT_MODULE_COUNT != 523u
#error "script module count disagrees with the R13-G2 pins"
#endif
#if EMERALD_SCRIPT_TOTAL_RELOC_COUNT != 16704u
#error "script relocation count disagrees with the R13-G2 pins"
#endif
#if EMERALD_SCRIPT_F_BINDING_COUNT != 3501u
#error "script F inbound count disagrees with the R13-G2 pins"
#endif

enum EmeraldScriptCompatStatus
{
    EMERALD_SCRIPT_OK = 0,
    EMERALD_SCRIPT_ERR_INVALID_ARGUMENT,  /* NULL snapshot/pack/diagnostics */
    EMERALD_SCRIPT_ERR_OUT_OF_MEMORY,
    EMERALD_SCRIPT_ERR_RESOLVE_FAILED,    /* M0/M1 snapshot did not resolve a module */
    EMERALD_SCRIPT_ERR_UNEXPECTED_OWNERSHIP, /* winner is not ROM_BASE */
    EMERALD_SCRIPT_ERR_PAYLOAD_SIZE_MISMATCH, /* pack/view size differs */
    EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT,  /* script composition != pins */
    EMERALD_SCRIPT_ERR_UNEXPECTED_SCHEMA, /* module schema != 45/46 */
    EMERALD_SCRIPT_ERR_TABLE_MISMATCH,    /* pack disagrees with the generated table */
    EMERALD_SCRIPT_ERR_SEGMENT_INVALID,   /* segment range/packing/kind fault */
    EMERALD_SCRIPT_ERR_EXPORT_INVALID,    /* export offset/boundary fault */
    EMERALD_SCRIPT_ERR_RELOC_INVALID,     /* source/width/raw-value/overlap fault */
    EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED, /* a typed target could not resolve */
    EMERALD_SCRIPT_ERR_BOUNDARY_INVALID,  /* boundary query refused */
    EMERALD_SCRIPT_ERR_STAGING_FAILED,    /* phase-2 allocation/index failure */
    EMERALD_SCRIPT_ERR_UNAVAILABLE,       /* no shadow generation / sibling seam unpublished */
};

/* Structured diagnostics (same shape as the other compat seams: stable
 * identifiers, sizes and dispositions - never pointers or paths). On
 * failure the FIRST failing entry is named. */
struct EmeraldScriptCompatDiagnostics
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

/* Where a resolved target's live pointer comes from. STAGED_ARENA
 * pointers are generation-scoped and never persist; SIBLING_SEAM
 * pointers belong to the published text/leaf arenas; COMPILED_BRIDGE
 * and HOST_RAM point at named compiled symbols; DEFERRED targets have
 * no live pointer in G3 (routing dispatch tables stay ROM-resident
 * and the braille labels await their C handoff). */
enum EmeraldScriptTargetDisposition
{
    EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA = 0,
    EMERALD_SCRIPT_DISPOSITION_SIBLING_SEAM = 1,
    EMERALD_SCRIPT_DISPOSITION_COMPILED_BRIDGE = 2,
    EMERALD_SCRIPT_DISPOSITION_HOST_RAM = 3,
    EMERALD_SCRIPT_DISPOSITION_DEFERRED = 4,
};

/* The typed resolution result: canonical identity (family/class, kind,
 * resource key, label, offsets, boundary) plus the live address where
 * one exists in the current generation. Identities are generation-
 * independent; liveAddress is generation-scoped. */
struct EmeraldScriptCompatResolvedTarget
{
    uint32_t targetClass;             /* EmeraldScriptNativeTargetClass */
    uint32_t targetKind;              /* EmeraldScriptNativeTargetKind */
    uint32_t disposition;             /* EmeraldScriptTargetDisposition */
    uint32_t boundaryKind;            /* EmeraldScriptNativeBoundary */
    char resourceKey[EMERALD_SCRIPT_KEY_CAP];
    char label[EMERALD_SCRIPT_LABEL_CAP];
    uint32_t targetOffset;            /* region-relative canonical; OFFSET_NONE when n/a */
    uint32_t targetPayloadOffset;     /* module payload offset; OFFSET_NONE when n/a */
    uintptr_t liveAddress;            /* 0 when not staged in this generation */
    uint32_t liveSize;                /* known byte extent where meaningful */
};

/* The staged F rebind plan row (plan sec 13): validation only in G3,
 * the live R13-F structures are never mutated. */
struct EmeraldScriptCompatStagedFBinding
{
    uint32_t kind;                    /* EmeraldScriptNativeFKind */
    uint32_t boundaryKind;
    uint32_t sameMap;
    char mapSymbol[EMERALD_SCRIPT_KEY_CAP];
    char mapKey[EMERALD_SCRIPT_KEY_CAP];
    char moduleKey[EMERALD_SCRIPT_KEY_CAP];
    char exportName[EMERALD_SCRIPT_LABEL_CAP];
    uint32_t gbaTarget;               /* original GBA provenance */
    uint32_t moduleOffset;            /* region-relative canonical offset */
    uint32_t payloadOffset;           /* OFFSET_NONE for routing rows */
    uint32_t disposition;
    uintptr_t stagedAddress;          /* 0 for routing rows (unstaged bytes) */
};

/* The staged gStdScripts candidate row (plan sec 12). */
struct EmeraldScriptCompatStagedStdScript
{
    uint32_t slot;
    uint32_t moduleIndex;
    uint32_t payloadOffset;
    uint32_t encodedGba;
    uint32_t boundaryKind;
    char moduleKey[EMERALD_SCRIPT_KEY_CAP];
    char exportName[EMERALD_SCRIPT_LABEL_CAP];
    uintptr_t stagedAddress;          /* arena pointer; never a compiled GBA payload */
};

struct EmeraldScriptCompatIndexCounts
{
    uint32_t modules;
    uint32_t segments;
    uint32_t exports;
    uint32_t relocs;                  /* module-side source rows */
    uint32_t routingRelocs;           /* routing-side source rows */
    uint32_t boundaries;
    uint32_t dynamicTargets;
    uint32_t stdScripts;
    uint32_t fBindings;
    uint32_t marts;
    uint32_t ramTargets;
    uint32_t ramAllowlist;
    uint32_t bridges;
    uint32_t opaqueBytes;             /* payload bytes no walk decoded */
};

/* Validate the session's pack against the generated script inventory,
 * resolve every typed dependency, stage the candidate generation and
 * shadow-commit it. Transactional; idempotent for a new session (the
 * previous shadow generation stays queryable until the new one fully
 * validates). Requires the text and leaf seams published. `pack` must
 * be the pack the session was built from. */
enum EmeraldScriptCompatStatus
EmeraldScriptCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldScriptCompatDiagnostics *diagnostics);

/* Drop the shadow generation (idempotent; used on session rollback /
 * shutdown). Live execution state is never touched. */
void EmeraldScriptCompat_ClearMigratedEntries(void);
void EmeraldScriptCompat_Shutdown(void);

/* Generation + arena queries. GetGenerationId is monotonic across
 * restages and cleared-state transitions (G4 stamps captures with it). */
uint64_t EmeraldScriptCompat_GetGenerationId(void);
bool EmeraldScriptCompat_GetArena(const uint8_t **outBase, size_t *outSize);
bool EmeraldScriptCompat_GetModuleSpan(const char *moduleKey,
                                       const uint8_t **outBase,
                                       size_t *outSize);

/* Source-operand resolution (plan sec 4/5): the exact relocation row
 * for a live operand address, with its typed target resolved for the
 * current generation. O(log n) in the source index. */
enum EmeraldScriptCompatStatus
EmeraldScriptCompat_ResolveOperand(uintptr_t operandAddress,
                                   struct EmeraldScriptCompatResolvedTarget *outTarget);

/* Binding resolution by canonical identity (plan sec 6-9). */
enum EmeraldScriptCompatStatus
EmeraldScriptCompat_ResolveBinding(uint32_t targetClass,
                                   const char *resourceKey,
                                   const char *label,
                                   uint32_t targetOffset,
                                   struct EmeraldScriptCompatResolvedTarget *outTarget);

/* Encoded-target resolution (plan sec 20): the dynamic/vaddress path
 * resolves a raw encoded GBA target against the canonical target index
 * - the resolver infrastructure G5's dynamic buffers need. O(log n)
 * in the dynamic index. */
enum EmeraldScriptCompatStatus
EmeraldScriptCompat_ResolveEncodedTarget(
    uint32_t encodedGba, uint32_t expectedClass,
    struct EmeraldScriptCompatResolvedTarget *outTarget);

/* Reverse containment (plan sec 11): a live pointer inside exactly one
 * staged module span -> module key + payload offset + segment kind.
 * Refuses arena-hull pointers outside any span, end pointers, and
 * stale-generation pointers. O(log n) over the 523 spans. */
enum EmeraldScriptCompatStatus
EmeraldScriptCompat_ReverseResolve(uintptr_t address,
                                   char *outModuleKey, size_t keyCap,
                                   uint32_t *outOffset,
                                   uint32_t *outSegmentKind);

/* Boundary validation (plan sec 10): the G4-facing queries.
 * INSTRUCTION_START: a decoded instruction start (G1 walk).
 * NEXT_INSTRUCTION: an instruction start (returns target the
 * instruction after the consumed one - always a start).
 * ENTRYPOINT: an export boundary (offset-zero / interior instruction /
 * typed-data / opaque positions all resolve as identities; the kind
 * distinguishes them for G4's capture proof).
 * SCRIPT_INTERIOR: an interior export at an instruction start.
 * TYPED_DATA_START: a static-data segment start (mart tables).
 * ROUTING: never arena-resident in G3 - always refused here.
 * Opaque-zone offsets refuse INSTRUCTION_START / NEXT_INSTRUCTION. */
enum EmeraldScriptBoundaryKind
{
    EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START = 0,
    EMERALD_SCRIPT_BOUNDARY_NEXT_INSTRUCTION = 1,
    EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT = 2,
    EMERALD_SCRIPT_BOUNDARY_SCRIPT_INTERIOR = 3,
    EMERALD_SCRIPT_BOUNDARY_TYPED_DATA_START = 4,
    EMERALD_SCRIPT_BOUNDARY_ROUTING = 5,
};

enum EmeraldScriptCompatStatus
EmeraldScriptCompat_ValidateBoundary(const char *moduleKey, uint32_t offset,
                                     uint32_t boundaryKind);

/* Staged shadow surfaces (plan sec 12/13). */
size_t EmeraldScriptCompat_GetStagedStdScriptCount(void);
bool EmeraldScriptCompat_GetStagedStdScript(uint32_t slot,
                                            struct EmeraldScriptCompatStagedStdScript *outRow);
size_t EmeraldScriptCompat_GetStagedFBindingCount(void);
bool EmeraldScriptCompat_GetStagedFBinding(
    size_t index, struct EmeraldScriptCompatStagedFBinding *outRow);

/* Counts + parity + status text. GetParityCounts reports the dynamic
 * oracle run in phase 1: checked == 16,704, mismatches == 0. */
bool EmeraldScriptCompat_GetIndexCounts(struct EmeraldScriptCompatIndexCounts *outCounts);
bool EmeraldScriptCompat_GetParityCounts(uint32_t *outChecked,
                                         uint32_t *outMismatches);
const char *EmeraldScriptCompatStatus_Describe(enum EmeraldScriptCompatStatus status);

#ifdef EMERALD_SCRIPT_COMPAT_TEST_HOOKS
/* Test-only: fail the Nth stage allocation of the next
 * TryInitialize (0-based among arena/span/source/F-plan calls), so a
 * partial allocation failure is injectable deterministically (plan
 * sec 17). Not present in production builds. */
void EmeraldScriptCompat_TestSetAllocFail(size_t nth);
#endif

#endif /* EMERALD_RESOURCES_EMERALD_SCRIPT_COMPAT_H */

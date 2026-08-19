#ifndef EMERALD_RESOURCES_EMERALD_LEAF_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_LEAF_COMPAT_H

/* R13-B: Emerald leaf payload ownership migration - additive publication
 * seam.
 *
 * The MOVEMENT family (1,055 resources: 1,047 script_data label scripts,
 * 7,404 B + 8 sMovement_* .rodata objects, 24 B = 7,428 B) and the
 * MULTIBOOT family (2 programs: ereader 12,512 B + pokemon-colosseum
 * 163,840 B) are extracted from the retail-qualified ROM into the
 * production pack (resources/extraction/emerald/bpee01/{movement,
 * multiboot}/) as canonical GBA-form byte payloads (type binary,
 * schema 1 = movement, schema 2 = multiboot, canonical representation
 * gba-bytes). This seam publishes those bytes into ONE process-lifetime
 * arena: a packed payload zone plus a record table (canonical id, legacy
 * compiled symbol, ROM offset, size, schema).
 *
 * ADDITIVE CONTRACT (R13-B brief §6): no consumer is redirected. The
 * movement consumers are all deferred (script bytecode operands +
 * C immediates - the redirect needs the map/script pointer graph, R13-G);
 * the ereader multiboot pointer is LIVE but not trivially redirectable
 * (EReaderHandleTransfer may write the buffer); the colosseum program is
 * dead data (compiled out of the native build). Because nothing reads the
 * arena, a failed publication is a DEGRADE, never a session refusal: the
 * diagnostics name the first failing resource, the arena stays absent,
 * and the game behaves exactly as pre-R13-B. No hot reload (the arena is
 * immutable and there is no republish path), no R10 range registration
 * (nothing the arena holds is serialized by State-v5), no logical-address
 * publication (no consumer resolves leaf addresses), and no host pointer
 * is stored anywhere in the arena (the payloads are pure bytes; the
 * record table holds names/offsets/sizes only) - so nothing in the arena
 * can ever be persisted as a raw native pointer.
 *
 * The EXPECTED INVENTORY is the generated slot table
 * (src/emerald/resources/leaf_native_table.generated.c, declared in
 * include/emerald/resources/leaf_native.generated.h): the seam validates
 * the session's pack against it - name/size/schema set equality, any
 * drift is a failed publication, never a partial one.
 *
 * Platform-neutral (no global.h/engine/frontend objects) so the offline
 * test harness can compile and drive it directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/leaf_native.generated.h"

/* The R13-B leaf families: exact counts pinned from the R13-B inventory
 * re-verification (R13 audit claimed 7,576 B; the corrected figure is
 * 7,428 B = 1,047 pret labels at 7,404 B + 8 objects at 24 B; the 3
 * recomp-local labels, 12 B, stay compiled and are not resources). The
 * multiboot figure (176,352 B = 12,512 ereader + 163,840 colosseum)
 * matches the audit exactly. The seam refuses a session whose leaf
 * composition differs (pack drift is a hard failure, not a partial
 * publication). */
#define EMERALD_LEAF_MOVEMENT_COUNT   1055u
#define EMERALD_LEAF_MULTIBOOT_COUNT    2u
#define EMERALD_LEAF_MOVEMENT_BYTES    7428u
#define EMERALD_LEAF_MULTIBOOT_BYTES  176352u
#define EMERALD_LEAF_TOTAL_BYTES \
    (EMERALD_LEAF_MOVEMENT_BYTES + EMERALD_LEAF_MULTIBOOT_BYTES)
#define EMERALD_LEAF_RESOURCE_COUNT \
    (EMERALD_LEAF_MOVEMENT_COUNT + EMERALD_LEAF_MULTIBOOT_COUNT)

/* The generated table is the expected inventory: the seam cross-checks
 * the session's pack against it, so its count and this header's pinned
 * count must agree at compile time - a regenerated table that disagrees
 * with the pins is a build error, not a runtime surprise. */
#if EMERALD_LEAF_MOVEMENT_COUNT != LEAF_NATIVE_MOVEMENT_COUNT
#error "leaf movement count disagrees with the generated slot table"
#endif
#if EMERALD_LEAF_MULTIBOOT_COUNT != LEAF_NATIVE_MULTIBOOT_COUNT
#error "leaf multiboot count disagrees with the generated slot table"
#endif
#if EMERALD_LEAF_RESOURCE_COUNT != LEAF_NATIVE_RESOURCE_COUNT
#error "leaf resource count disagrees with the generated slot table"
#endif

enum EmeraldLeafCompatStatus
{
    EMERALD_LEAF_OK = 0,
    EMERALD_LEAF_ERR_INVALID_ARGUMENT,  /* NULL snapshot/pack pointer */
    EMERALD_LEAF_ERR_OUT_OF_MEMORY,
    EMERALD_LEAF_ERR_RESOLVE_FAILED,    /* M0/M1 snapshot did not resolve a leaf */
    EMERALD_LEAF_ERR_PAYLOAD_SIZE_MISMATCH, /* pack/view size or bytes differ */
    EMERALD_LEAF_ERR_UNEXPECTED_COUNT,  /* leaf composition != 1055 + 2 */
    EMERALD_LEAF_ERR_UNEXPECTED_OWNERSHIP, /* winner is not the ROM_BASE provider */
    EMERALD_LEAF_ERR_TABLE_MISMATCH,    /* pack disagrees with the slot table */
    EMERALD_LEAF_ERR_OVERLAPPING_SLICE, /* two claimed ROM slices overlap */
};

/* Structured diagnostics for the arena build (same shape as the other
 * compat seams: stable identifiers, sizes and dispositions - never
 * pointers or paths). On failure the FIRST failing resource is named. */
struct EmeraldLeafCompatDiagnostics
{
    char canonicalName[96];
    char stage[24];                /* "build" / "publish" */
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

struct EmeraldLeafArena;

/* Resolve every leaf resource of the session through the NORMAL M0/M1
 * snapshot (type binary, schema per family, winner must be the ROM_BASE
 * provider), cross-check the resolver view against the pack entry (size
 * and byte equality), validate the composition (exactly 1,055 movement +
 * 2 multiboot) and the generated slot table (name/size/schema set
 * equality), then copy the canonical bytes into the single packed arena.
 * Transactional: every validation runs before the arena is allocated; on
 * any failure the arena stays absent and the diagnostics name the first
 * failing resource. Idempotent for a new session: an already-published
 * arena is replaced atomically only after the new session validates.
 * `pack` must be the pack the session was built from (its sourceRomOffset
 * + payload carry the build-time ROM provenance). */
enum EmeraldLeafCompatStatus
EmeraldLeafCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldLeafCompatDiagnostics *diagnostics);

/* Fail-closed clear: release the arena. Used when a session is rolled
 * back or shut down (idempotent). */
void EmeraldLeafCompat_ClearMigratedEntries(void);
void EmeraldLeafCompat_Shutdown(void);

/* Query helpers (tests + future R13 consumers). GetArena returns the
 * packed payload zone (base + total payload bytes); GetResourceSpan /
 * GetResourceBytes look a leaf up by canonical id (false when
 * unpublished or unknown). */
bool EmeraldLeafCompat_GetArena(const uint8_t **outBase, size_t *outSize);
size_t EmeraldLeafCompat_GetPublishedCount(void);
bool EmeraldLeafCompat_GetResourceSpan(const char *canonicalName,
                                       size_t *outRomOffset, size_t *outSize,
                                       uint8_t *outSchema);
bool EmeraldLeafCompat_GetResourceBytes(const char *canonicalName,
                                        const uint8_t **outBytes,
                                        size_t *outSize);

/* Arena-residency canary: bounds check against the published payload
 * zone. Nothing in the game may ever hold a leaf-arena pointer in
 * R13-B (no consumer is redirected); the canary exists for the test
 * battery and the future redirect stages. */
bool EmeraldLeafCompat_ContainsPointer(uintptr_t address);

#endif

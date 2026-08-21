#ifndef EMERALD_RESOURCES_EMERALD_TEXT_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_TEXT_COMPAT_H

/* R13-C: Emerald text ownership migration - 16 per-family arenas +
 * live slot/skeleton cutover.
 *
 * The text family (12,777 labels: 4,824 C-side per-label
 * resources + 7,953 bundle-local labels in 363 script/data blobs;
 * 903,151 canonical bytes) is extracted from the retail-qualified ROM
 * into the production pack (resources/extraction/emerald/bpee01/text/,
 * type text, schema 1). This seam validates the session's pack against
 * the generated inventory (kTextNativeResources: name/size/isBundle/
 * arena set equality) and publishes the canonical bytes into SIXTEEN
 * per-family arenas (plan §7/§9): one contiguous arena per family
 * group, labels packed in canonical-name order, 16-aligned payload
 * zone, byte-exact copies including 0xFF terminators. Deterministic:
 * same inputs -> same layout, byte-identical across runs.
 *
 * LIVE CUTOVER (§11): the arenas are then the reference for the
 * generated pointer slots (kTextSlotBindings, 3,119) and skeleton
 * tables (kTextSkeletonTables, 2,972 fills of kinds 0/1/2) - every
 * written pointer is an arena pointer (the only reference form), so
 * the State-v5 currentChar walker's 16 family-arena ranges cover all
 * live text (plan §8/§9; registration is the R13-C §13-15 machinery,
 * not this seam).
 *
 * REFUSE CONTRACT (brief §17): the cut-over families (battle/move/
 * ability/nature/shared/system/match-call/ribbon) have NO compiled
 * fallback after the C-side cutover guards land, so a session whose
 * text cannot be published is a REFUSED session - the loader rolls
 * the whole registration back. The seam itself is transactional:
 * phase 1 validates every resource (composition pins, M0/M1
 * resolution, ownership, type/schema, size + byte equality against
 * the pack, escape-grammar revalidation, C-label ROM-slice
 * disjointness, per-bundle blob tiling, per-arena summaries) before
 * any allocation; phase 2 builds the arenas and resolves every slot/
 * skeleton fill; phase 3 publishes atomically (one allocation, then
 * the infallible pointer stores). On any phase-1/2 failure the arena
 * stays absent, nothing is written, and the diagnostics name the
 * first failing entry.
 *
 * Platform-neutral (no global.h/engine/frontend objects) so the
 * offline test harness can compile and drive it directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/text_arenas.generated.h"
#include "emerald/resources/text_native.generated.h"
#include "emerald/resources/text_bundle_index.generated.h"
#include "emerald/resources/text_slot_bindings.generated.h"
#include "emerald/resources/text_skeletons.generated.h"

/* The generated inventory is the expected inventory; its counts and
 * the per-arena summaries must agree with this header's pins at
 * compile time - a regenerated table that disagrees with the pins is
 * a build error, not a runtime surprise. */
#if TEXT_ARENA_COUNT != 16u
#error "text arena count disagrees with the generated arenas table"
#endif
#if TEXT_NATIVE_RESOURCE_COUNT != (TEXT_NATIVE_LABEL_COUNT + TEXT_NATIVE_BUNDLE_COUNT)
#error "text native inventory counts disagree"
#endif
#if TEXT_NATIVE_LABEL_COUNT != 4844u
#error "text native label count disagrees with the R13-C pins"
#endif
/* 4,844 = 4,824 C-side + 20 R13-G2 §7.3 mystery-gift handoff labels
 * (emerald:text/mystery-gift/*, 2,682 B; passive inventory records,
 * no range registration). */
#if TEXT_NATIVE_BUNDLE_COUNT != 363u
#error "text native bundle count disagrees with the R13-C pins"
#endif
#if TEXT_BUNDLE_ENTRY_COUNT != 7953u
#error "text bundle entry count disagrees with the R13-C pins"
#endif
#if TEXT_BUNDLE_COUNT != 363u
#error "text bundle count disagrees with the R13-C pins"
#endif
#if TEXT_SLOT_BINDING_COUNT != 3118u
#error "text slot binding count disagrees with the R13-C pins"
#endif
#if TEXT_SKELETON_TABLE_COUNT != 283u
#error "text skeleton table count disagrees with the R13-C pins"
#endif
#if TEXT_SKELETON_FILL_COUNT != 3335u
#error "text skeleton fill count disagrees with the R13-C pins"
#endif

/* Total label count across the 16 arenas: 12,797 records = 4,844
 * per-label rows (4,824 C-side + 20 R13-G2 §7.3 mystery-gift handoff)
 * + 7,953 bundle-local entries; the gift labels additionally publish
 * per-label while staying bundle members, so the seam's inventory
 * model counts them in both representations (the 13 out-of-contract
 * symbols stay compiled and are not resources). 905,839 B =
 * 903,157 + 2,682 (the +20 gift per-label rows; R13-C #112:
 * gText_123Dot is a single 9-byte record instead of a 3-byte run). */
#define EMERALD_TEXT_LABEL_COUNT 12797u
#define EMERALD_TEXT_TOTAL_BYTES 905839u

/* The longest canonical label name (the dash-form key) and resource id
 * fit these buffers; a truncating copy made a leaf-seam comparison
 * fail, so both are checked at build. */
#define EMERALD_TEXT_NAME_CAP 128u

enum EmeraldTextCompatStatus
{
    EMERALD_TEXT_OK = 0,
    EMERALD_TEXT_ERR_INVALID_ARGUMENT,   /* NULL snapshot/pack pointer */
    EMERALD_TEXT_ERR_OUT_OF_MEMORY,
    EMERALD_TEXT_ERR_RESOLVE_FAILED,     /* M0/M1 snapshot did not resolve */
    EMERALD_TEXT_ERR_PAYLOAD_SIZE_MISMATCH, /* pack/view size or bytes differ */
    EMERALD_TEXT_ERR_UNEXPECTED_COUNT,   /* label composition != pins */
    EMERALD_TEXT_ERR_UNEXPECTED_OWNERSHIP, /* winner is not the ROM_BASE provider */
    EMERALD_TEXT_ERR_TABLE_MISMATCH,     /* pack disagrees with the inventory */
    EMERALD_TEXT_ERR_OVERLAPPING_SLICE,  /* C-label ROM slices overlap */
    EMERALD_TEXT_ERR_BUNDLE_LAYOUT,      /* bundle index disagrees with blob */
    EMERALD_TEXT_ERR_ESCAPE_GRAMMAR,     /* canonical payload not a valid string */
    EMERALD_TEXT_ERR_ARENA_MISMATCH,     /* arena summaries differ */
    EMERALD_TEXT_ERR_FILL_RESOLVE,       /* slot/skeleton fill unresolved */
    EMERALD_TEXT_ERR_RANGE_REGISTRATION, /* arena spans could not register */
    EMERALD_TEXT_ERR_UNAVAILABLE,        /* no published arena to republish */
};

/* Structured diagnostics (same shape as the other compat seams:
 * stable identifiers, sizes and dispositions - never pointers or
 * paths). On failure the FIRST failing entry is named. */
struct EmeraldTextCompatDiagnostics
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

struct EmeraldTextArena;

/* Resolve every text resource of the session through the NORMAL M0/M1
 * snapshot (type text, schema 1, winner must be the ROM_BASE
 * provider), cross-check the resolver view against the pack entry
 * (size and byte equality), validate the composition against the
 * generated inventory (name/size/isBundle/arena set equality),
 * revalidate the escape grammar of every canonical payload, prove the
 * C-label ROM slices pairwise disjoint and every bundle blob tiled by
 * its index entries, then build the 16 family arenas (sorted-name
 * packing, 16-aligned zones) and apply the generated slot + skeleton
 * fills. Transactional: every validation runs and every fill
 * resolves before any allocation; the pointer stores in phase 3 are
 * infallible. On any failure the prior state is untouched (a
 * previously published arena stays live until the new session fully
 * validates) and the diagnostics name the first failing entry.
 * Idempotent for a new session: an already-published arena is
 * replaced atomically only after the new session validates. `pack`
 * must be the pack the session was built from. */
enum EmeraldTextCompatStatus
EmeraldTextCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldTextCompatDiagnostics *diagnostics);

/* Fail-closed clear: NULL every applied slot/skeleton pointer (so no
 * host slot dangles into a freed arena), then release the arena. Used
 * when a session is rolled back or shut down (idempotent). */
void EmeraldTextCompat_ClearMigratedEntries(void);
void EmeraldTextCompat_Shutdown(void);

/* Query helpers (tests + the R13-C §13-15 currentChar walker).
 * GetArenaByKey returns one family arena's payload zone (base +
 * byteTotal, before 16-alignment tail padding). */
bool EmeraldTextCompat_GetArenaByKey(const char *key,
                                     const uint8_t **outBase,
                                     size_t *outSize);
size_t EmeraldTextCompat_GetArenaCount(void);
size_t EmeraldTextCompat_GetPublishedCount(void);
bool EmeraldTextCompat_GetResourceBytes(const char *resourceId,
                                        const uint8_t **outBytes,
                                        size_t *outSize);
bool EmeraldTextCompat_GetLabelBytes(const char *canonicalName,
                                     const uint8_t **outBytes,
                                     size_t *outSize);

/* Arena-residency canaries (the R12-E precedent): bounds checks
 * against the published arenas. GetArenaForPointer identifies the
 * enclosing family arena (arena index + byte offset) - the walker
 * input for the State-v5 currentChar routing. */
bool EmeraldTextCompat_ContainsPointer(uintptr_t address);
bool EmeraldTextCompat_GetArenaForPointer(uintptr_t address,
                                          size_t *outArenaIndex,
                                          size_t *outOffset);

/* Offset-in-range resolution (plan §9): the label whose bytes enclose
 * `offset` in arena `arenaIndex`, per the seam's deterministic sorted
 * layout. Used by the currentChar walker to validate a mid-string
 * pointer against a known label span. */
bool EmeraldTextCompat_GetLabelAtOffset(uint32_t arenaIndex, size_t offset,
                                        const char **outName,
                                        size_t *outStart, size_t *outSize);

#endif

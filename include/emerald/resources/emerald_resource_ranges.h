#ifndef EMERALD_RESOURCES_EMERALD_RESOURCE_RANGES_H
#define EMERALD_RESOURCES_EMERALD_RESOURCE_RANGES_H

/* R10-C: reverse resource-range index.
 *
 * The state system needs to recognize pointers that reference immutable
 * runtime resource data, and to reconstruct such pointers from stable
 * identity + offset instead of ever serializing the pointer itself. This
 * module is the reverse index for that: for every immutable runtime resource
 * range that game state may reference it maps a host address to
 *
 *   - the owning resource key (Gen3ResourceKey, derived from the canonical
 *     name),
 *   - the resource type and schema,
 *   - the representation role (see enum below),
 *   - and the offset of the address within the range.
 *
 * Ranges are registered by the compatibility seams for each entry of a
 * built EmeraldResourceCompatibilityImage: the entry's stream region (the
 * literal/legacy-LZ bytes the runtime tables publish). Canonical provider
 * payload bytes and compatibility object/table structures are distinct roles
 * that the enum models even though no runtime range uses them today (the
 * canonical payloads are build-time-only, and the compat objects live in
 * the executable's host .data, not in the arena).
 *
 * Determinism and safety rules:
 *   - identity is the derived resource key, never a handle or pointer;
 *   - ranges are kept sorted by base and MUST NOT overlap - an overlapping
 *     registration is rejected (no ambiguous overlap exists in the system);
 *   - all address arithmetic is uintptr_t with explicit overflow checks;
 *   - the index owns no memory and holds no persistent identity beyond keys:
 *     it dies with the session that owns it, and lookups are only performed
 *     while that session is alive (capture/load), so no use-after-destruction
 *     is possible;
 *   - an optional "hull" list records resource-owned spans that game state
 *     may never legally reference (the EXPOSED stream region of each arena
 *     allocation - the bytes the runtime tables publish): a pointer inside
 *     a hull but outside every registered range is a capture error, never
 *     a silently persisted value. The unexposed [entry table][names]
 *     [canonical payloads] prefix of each arena is build-time-only and is
 *     NOT hulled: an 8-byte window that happens to be numerically inside
 *     it is ordinary data, never a resource reference (hulling it would
 *     fail captures on coincidental values such as packed sprite pixels or
 *     M4A channel byte fields).
 *
 * Platform-neutral: no global.h, no sprite.h, no frontend objects.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emerald/resources/emerald_resource_compat.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_types.h"

/* Hard bound on registered ranges: the production session registers ~2040
 * (one per compat-image entry: 196 trainer + 1804 pokemon), R11 adds the
 * object-event (288) and tileset (1544) families, and R11-D the layout
 * family (882): the merged view holds 4518 ranges. 8192 leaves headroom
 * while keeping capture-side bookkeeping bounded. */
#define EMERALD_RESOURCE_RANGE_INDEX_MAX_RANGES 8192u
/* Hulls: one per live resource arena allocation (trainer image, pokemon
 * image) plus headroom. */
#define EMERALD_RESOURCE_RANGE_INDEX_MAX_HULLS 8u

/* Representation role of the bytes a pointer references (§B). */
enum EmeraldResourceRangeRole
{
    EMERALD_RESOURCE_ROLE_CANONICAL = 0,     /* canonical provider payload bytes */
    EMERALD_RESOURCE_ROLE_LEGACY_LZ = 1,     /* legacy/literal-LZ payload stream */
    EMERALD_RESOURCE_ROLE_COMPAT_OBJECT = 2, /* compatibility object/table */
};

struct EmeraldResourceRange
{
    uintptr_t base;
    size_t length;
    Gen3ResourceKey key;
    uint32_t type;
    uint32_t schema;
    uint32_t role;
};

struct EmeraldResourceRangeHit
{
    Gen3ResourceKey key;
    uint32_t type;
    uint32_t schema;
    uint32_t role;
    size_t rangeOffset; /* hit address minus range base */
};

struct EmeraldResourceRangeHull
{
    uintptr_t base;
    size_t length;
};

struct EmeraldResourceRangeIndex
{
    struct EmeraldResourceRange ranges[EMERALD_RESOURCE_RANGE_INDEX_MAX_RANGES];
    size_t rangeCount; /* sorted ascending by base, non-overlapping */
    struct EmeraldResourceRangeHull hulls[EMERALD_RESOURCE_RANGE_INDEX_MAX_HULLS];
    size_t hullCount;
    bool hullOverlap; /* hulls are allowed to touch/overlap; ranges never */
};

void EmeraldResourceRangeIndex_Init(struct EmeraldResourceRangeIndex *index);
void EmeraldResourceRangeIndex_Reset(struct EmeraldResourceRangeIndex *index);

size_t EmeraldResourceRangeIndex_GetRangeCount(
    const struct EmeraldResourceRangeIndex *index);

/* Record a resource-owned span that no game field may legally reference
 * except through a registered range (the exposed stream region of an arena
 * allocation - NOT the whole arena: the build-time-only prefix must never
 * be hulled, see the module comment). Fails on NULL index, zero length,
 * base+length overflow, or a full hull table. */
bool EmeraldResourceRangeIndex_AddHull(
    struct EmeraldResourceRangeIndex *index, uintptr_t base, size_t length);

/* Register one compat-image entry's stream region. The key is derived from
 * the entry's canonical name, type from the entry, schema from the caller
 * (the seam knows it; the image does not store it). Fails on NULL image,
 * invalid entry index, zero-length stream, base+length overflow, a full
 * range table, or overlap with an already-registered range. */
bool EmeraldResourceRangeIndex_RegisterStream(
    struct EmeraldResourceRangeIndex *index,
    const struct EmeraldResourceCompatibilityImage *image, size_t entryIndex,
    uint32_t schema, enum EmeraldResourceRangeRole role);

/* Look up a host address. Returns true and fills *outHit for an exactly-one
 * range hit; returns false for no hit. Ambiguity is impossible after
 * registration validation but is still checked and reported as false (with
 * the hit left untouched) so a caller can fail closed. */
bool EmeraldResourceRangeIndex_Lookup(
    const struct EmeraldResourceRangeIndex *index, uintptr_t address,
    struct EmeraldResourceRangeHit *outHit);

/* true when the address lies inside any registered hull span (resource-owned
 * memory with no registered identity). */
bool EmeraldResourceRangeIndex_InHull(
    const struct EmeraldResourceRangeIndex *index, uintptr_t address);

/* Load-direction resolution (R10 §E): find the range whose key, type,
 * schema and role match the record, and materialize the pointer for an
 * offset within it. Returns true and fills *outPointer on success; false on
 * an unknown key, any identity mismatch, an out-of-range offset, or
 * address arithmetic overflow. This is the ONLY function the state loader
 * uses to turn a sidecar record back into a pointer. */
bool EmeraldResourceRangeIndex_ResolveByKey(
    const struct EmeraldResourceRangeIndex *index, const Gen3ResourceKey *key,
    uint32_t type, uint32_t schema, uint32_t role, size_t rangeOffset,
    uintptr_t *outPointer);

#endif

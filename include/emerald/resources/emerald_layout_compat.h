#ifndef EMERALD_RESOURCES_EMERALD_LAYOUT_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_LAYOUT_COMPAT_H

/* R11-D: raw map blockdata native compatibility publication.
 *
 * The layout family (882 resources: 441 blockdata + 441 border leaves)
 * migrates to ROM_BASE. The 441 `struct MapLayout` records remain compiled
 * STRUCTURAL metadata (gen_layout_family emits them writable, NULL-sentineled
 * .map/.border pointers, into layout_native.generated.h); only the payload
 * leaves leave the native link. This seam publishes the session's canonical
 * streams into the two pointer members of each record:
 *
 *   - .map    -> the blockdata stream (the GBA build's map.bin INCBIN,
 *                w*h*2 bytes or the 20 grid-padded files' real sizes);
 *   - .border -> the border stream (the GBA build's border.bin INCBIN,
 *                8 bytes, 2x2 words).
 *
 * Encodings: both leaves are raw (uncompressed) canonical bytes on both
 * targets - every entry registers under ROLE_CANONICAL, never LEGACY_LZ.
 *
 * Contract (mirrors the object-event/tileset seams):
 *   - init is transactional: resolve + verify ALL 882 resources (type
 *     tilemap, schema 1, winner == ROM_BASE, exact expected size) and build
 *     the image BEFORE any record pointer is written; any failure leaves the
 *     records at their NULL sentinels;
 *   - publication changes ONLY the .map/.border pointer fields; every other
 *     field (width, height, tileset references) stays identical;
 *   - the image is session-lifetime, immutable after construction;
 *     Republish is allocation-free and idempotent (post-state-load
 *     re-derivation); ClearMigratedEntries NULLs both pointer fields
 *     (fail-closed sentinels);
 *   - the module is compiled only on the native target; the GBA build
 *     keeps the const compile-time payloads and never calls these
 *     functions.
 *
 * Lifecycle entry points are driven by the trainer seam's
 * InitializeFromSnapshot/Republish/ClearMigratedEntries/Shutdown.
 * Additive-degradation policy: on the unit-harness additive path a
 * snapshot that cannot serve the layout family leaves the records at their
 * sentinels with the diagnostics cleared (the other families' publication
 * stands); the strict game path refuses the session - with the compiled
 * leaves gone from the native link, NULL blockdata cannot render any map.
 */

#include "emerald/resources/emerald_resource_compat.h"
#include "gen3/resources/resource_resolver.h"

/* Resolve the whole layout family from `snapshot`, build the
 * compatibility image and publish every record pointer. Returns
 * EMERALD_COMPAT_OK on success; on any failure nothing is mutated and
 * *diagnostics (may be NULL) carries the first failing resource. */
enum EmeraldResourceCompatStatus
EmeraldLayoutCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

/* Re-derive every pointer from the retained session image.
 * Allocation-free, idempotent. Fails closed
 * (EMERALD_COMPAT_ERR_UNAVAILABLE) when no image is installed. */
enum EmeraldResourceCompatStatus
EmeraldLayoutCompat_Republish(
    struct EmeraldResourceCompatDiagnostics *diagnostics);

/* NULL the migrated record .map/.border pointers. */
void EmeraldLayoutCompat_ClearMigratedEntries(void);

/* Destroy the session image and release the publication state. */
void EmeraldLayoutCompat_Shutdown(void);

/* R10-C: the retained session image and its entry count (for range
 * registration). NULL / 0 on no session. */
const struct EmeraldResourceCompatibilityImage *EmeraldLayoutCompat_GetImage(void);
size_t EmeraldLayoutCompat_GetEntryCount(void);

/* R10-C: the registered range-index schema and role for image entry `i`
 * (the trainer seam's range rebuild). Every layout entry is schema 1 and
 * role CANONICAL (raw bytes). */
uint32_t EmeraldLayoutCompat_GetEntrySchema(size_t i);
uint32_t EmeraldLayoutCompat_GetEntryRole(size_t i);

#endif

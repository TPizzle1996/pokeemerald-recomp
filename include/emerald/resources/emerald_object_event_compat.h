#ifndef EMERALD_RESOURCES_EMERALD_OBJECT_EVENT_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_OBJECT_EVENT_COMPAT_H

/* R11-B: object-event graphics native compatibility publication.
 *
 * The overworld object-event family (253 raw 4bpp pic sheets + 35 gbapal
 * palettes, 288 resources) migrates to ROM_BASE. The compiled
 * ObjectEventGraphicsInfo structs, SpriteFrameImage arrays, animation
 * command tables, OAM/subsprite tables and berry/Mauville identity tables
 * remain compiled STRUCTURAL metadata on the native link; only the payload
 * leaves leave it. On native the frame arrays and the palette arrays are
 * non-const NULL-sentinel tables (emitted by gen_object_event_family into
 * object_event_pic_tables.native.generated.h) and this seam publishes the
 * session's canonical streams into them:
 *
 *   frame.data = canonical sheet stream + width*height*frame*32
 *   palette bytes = canonical 32-byte BGR555 payload, copied in place
 *
 * The payloads are RAW: the pack stores the canonical bytes verbatim and
 * the compat image serves them with EMERALD_COMPAT_ENTRY_RAW — no LZ
 * representation exists anywhere in this family. The streams register
 * with the R10 reverse resource-range index under ROLE_CANONICAL (the
 * bytes ARE the canonical payload representation).
 *
 * Contract (mirrors the pokémon seam):
 *   - init is transactional: resolve + verify ALL 288 resources (type,
 *     schema, winner == ROM_BASE, exact expected size) and build the image
 *     BEFORE any table is written; any failure leaves the tables in their
 *     NULL sentinels;
 *   - publication changes ONLY the migrated frame `.data`/`.size` fields
 *     and the palette array bytes; every other field stays identical;
 *   - the image is session-lifetime, immutable after construction;
 *     Republish is allocation-free and idempotent (post-state-load
 *     re-derivation); ClearMigratedEntries NULLs the frame data and zeros
 *     the palette bytes (fail-closed sentinels);
 *   - the module is compiled only on the native target; the GBA build
 *     keeps the const compile-time payloads and never calls these
 *     functions.
 *
 * Lifecycle entry points are driven by the trainer seam's
 * InitializeFromSnapshot/Republish/ClearMigratedEntries/Shutdown.
 * Additive-degradation policy: on the unit-harness additive path a
 * snapshot that cannot serve the object-event family leaves the tables at
 * their NULL sentinels with the diagnostics cleared (the trainer/pokémon
 * families' publication stands); the strict game path refuses the session.
 */

#include "emerald/resources/emerald_resource_compat.h"
#include "gen3/resources/resource_resolver.h"

/* Resolve the whole object-event family from `snapshot`, build the
 * compatibility image and publish every frame/palette. Returns
 * EMERALD_COMPAT_OK on success; on any failure nothing is mutated and
 * *diagnostics (may be NULL) carries the first failing resource. */
enum EmeraldResourceCompatStatus
EmeraldObjectEventCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

/* Re-derive every frame/palette from the retained session image.
 * Allocation-free, idempotent. Fails closed
 * (EMERALD_COMPAT_ERR_UNAVAILABLE) when no image is installed. */
enum EmeraldResourceCompatStatus
EmeraldObjectEventCompat_Republish(
    struct EmeraldResourceCompatDiagnostics *diagnostics);

/* NULL the migrated frame `.data` fields and zero the palette bytes. */
void EmeraldObjectEventCompat_ClearMigratedEntries(void);

/* Destroy the session image and release the publication state. */
void EmeraldObjectEventCompat_Shutdown(void);

/* R10-C: the retained session image and its entry count (for range
 * registration). NULL / 0 on no session. */
const struct EmeraldResourceCompatibilityImage *EmeraldObjectEventCompat_GetImage(void);
size_t EmeraldObjectEventCompat_GetEntryCount(void);

#endif

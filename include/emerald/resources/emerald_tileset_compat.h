#ifndef EMERALD_RESOURCES_EMERALD_TILESET_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_TILESET_COMPAT_H

/* R11-C: tileset graphics native compatibility publication.
 *
 * The tileset family (1544 resources: 75 tile leaves, 1,200 palette rows,
 * 70 metatiles, 70 metatile-attributes, 125 anim frames, 4 floor-light
 * palettes) migrates to ROM_BASE. The 75 `struct Tileset` objects, the anim
 * frame tables and all compiled callbacks remain compiled STRUCTURAL
 * metadata; only the payload leaves leave the native link. The native
 * objects are non-const NULL-sentinel slots (emitted by gen_tileset_family
 * into tileset_native.generated.h); this seam publishes the session's
 * canonical streams into them:
 *
 *   - the 4 payload POINTER members of each struct (tiles / palettes /
 *     metatiles / metatileAttributes) point into the session compat arena:
 *     compressed tilesets serve a synthesized literal-only GBA LZ77 stream
 *     (the GBA build's INCBIN'd retail stream shape; the runtime's
 *     DecompressAndCopyTileDataToVram LZ77-decompresses exactly as on GBA),
 *     everything else serves the canonical bytes verbatim;
 *   - the palette-row arrays (gTilesetPalettes_*) and the anim-frame /
 *     floor-light leaf arrays (gTilesetAnims_*) get the raw bytes copied in
 *     place (the R11-B palette pattern), because compiled consumers
 *     (fldeff_misc.c's SandPillar palette, the anim frame tables) reference
 *     the array symbols directly.
 *
 * Encodings: compressed tile streams register with the R10 reverse
 * resource-range index under ROLE_LEGACY_LZ (the stream IS the GBA LZ
 * representation); raw tiles, metatiles, attributes, palette rows, anim
 * frames and floor-light palettes register under ROLE_CANONICAL (the bytes
 * ARE the canonical payload representation).
 *
 * Contract (mirrors the object-event seam):
 *   - init is transactional: resolve + verify ALL 1,544 resources (type,
 *     schema, winner == ROM_BASE, exact expected size) and build the image
 *     BEFORE any slot is written; any failure leaves the structs at their
 *     NULL sentinels and the arrays zeroed;
 *   - publication changes ONLY the four payload pointer fields and the
 *     palette/anim/floor array bytes; every other field stays identical;
 *   - the image is session-lifetime, immutable after construction;
 *     Republish is allocation-free and idempotent (post-state-load
 *     re-derivation); ClearMigratedEntries NULLs the pointer fields and
 *     zeroes the arrays (fail-closed sentinels);
 *   - the module is compiled only on the native target; the GBA build
 *     keeps the const compile-time payloads and never calls these
 *     functions.
 *
 * Lifecycle entry points are driven by the trainer seam's
 * InitializeFromSnapshot/Republish/ClearMigratedEntries/Shutdown.
 * Additive-degradation policy: on the unit-harness additive path a
 * snapshot that cannot serve the tileset family leaves the slots at their
 * sentinels with the diagnostics cleared (the trainer/pokémon/object-event
 * families' publication stands); the strict game path refuses the session.
 */

#include "emerald/resources/emerald_resource_compat.h"
#include "gen3/resources/resource_resolver.h"

/* Resolve the whole tileset family from `snapshot`, build the
 * compatibility image and publish every struct pointer / array byte.
 * Returns EMERALD_COMPAT_OK on success; on any failure nothing is mutated
 * and *diagnostics (may be NULL) carries the first failing resource. */
enum EmeraldResourceCompatStatus
EmeraldTilesetCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

/* Re-derive every pointer / array from the retained session image.
 * Allocation-free, idempotent. Fails closed
 * (EMERALD_COMPAT_ERR_UNAVAILABLE) when no image is installed. */
enum EmeraldResourceCompatStatus
EmeraldTilesetCompat_Republish(
    struct EmeraldResourceCompatDiagnostics *diagnostics);

/* NULL the migrated struct payload pointers and zero the palette / anim /
 * floor-light arrays. */
void EmeraldTilesetCompat_ClearMigratedEntries(void);

/* Destroy the session image and release the publication state. */
void EmeraldTilesetCompat_Shutdown(void);

/* R10-C: the retained session image and its entry count (for range
 * registration). NULL / 0 on no session. */
const struct EmeraldResourceCompatibilityImage *EmeraldTilesetCompat_GetImage(void);
size_t EmeraldTilesetCompat_GetEntryCount(void);

/* R10-C: the registered range-index schema and role for image entry `i`
 * (the trainer seam's range rebuild). Schema is the catalog schema of the
 * entry's resource; role is LEGACY_LZ for compressed tile streams and
 * CANONICAL for every raw payload. */
uint32_t EmeraldTilesetCompat_GetEntrySchema(size_t i);
uint32_t EmeraldTilesetCompat_GetEntryRole(size_t i);

#endif

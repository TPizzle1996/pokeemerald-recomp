#ifndef EMERALD_RESOURCES_EMERALD_POKEMON_NATIVE_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_POKEMON_NATIVE_COMPAT_H

/* R9 §5: Pokémon battle graphics native compatibility publication.
 *
 * The four Pokémon battle tables (gMonFrontPicTable, gMonBackPicTable,
 * gMonPaletteTable, gMonShinyPaletteTable) are the GBA build's compile-time
 * assets: per-species LZ77-compressed 4bpp sheets and BGR555 palettes, all
 * 440 slots each, slot index == species id. On native they are mutable and
 * published by this module from the ROM_BASE-resolved session snapshot -
 * exactly the R7B/R8 trainer seam pattern, but fully table-driven: the slot
 * map and the 1608-resource size/type table come from the GENERATED header
 * emerald/resources/pokemon_battle_slots.generated.h (emitted by
 * gen_pokemon_family, verified byte-for-byte by --check), never from
 * handwritten per-species logic.
 *
 * Contract (mirrors the trainer seam):
 *   - the pack stores the DECODED representation of every retail stream
 *     (source_encoding "gba-lz77", three-way-equal to the retail ROM's
 *     decode, Stage 3); the seam re-encodes each decoded payload into a
 *     byte-deterministic literal-only GBA LZ77 stream
 *     (Gen3LzLiteral_Encode, Stage R5) and serves that stream via
 *     EMERALD_COMPAT_ENTRY_GBA_LZ, so the existing decompressors
 *     (DecompressPicFromTable_2, LoadSpecialPokePic_2,
 *     LoadCompressedSpritePalette, ...) decode exactly the bytes the GBA
 *     build decodes - the retail stream itself is not in the pack and is
 *     not needed;
 *   - init is transactional: resolve + verify ALL 1608 resources (type,
 *     schema, winner == ROM_BASE, LZ77-header decoded size == mapping) and
 *     build the image BEFORE any table pointer is written; any failure leaves
 *     the live tables untouched;
 *   - publication changes ONLY the migrated .data pointers; indices, tags
 *     and sizes are retained exactly. The one external slot (the back-EGG
 *     row, gMonStillFrontPic_Egg) has no canonical and stays compiled;
 *   - the image is session-lifetime, immutable after construction;
 *     Republish is allocation-free and idempotent (state-load re-derivation);
 *     ClearMigratedEntries NULLs exactly the published slots (fail-closed
 *     sentinel);
 *   - the module is compiled only on the native target; GBA/Windows keep the
 *     const compile-time tables and never call these functions.
 *
 * The lifecycle entry points are driven by the trainer seam's own
 * InitializeFromSnapshot/Republish/ClearMigratedEntries/Shutdown, so the
 * existing call sites (desktop_game_content.c, sdl2.c, native_state.c,
 * emerald_runtime_loader.c) are unchanged.
 *
 * Additive-degradation policy (R9 §5): the Pokémon family is ADDITIVE to the
 * trainer seam. A snapshot that does not carry it (trainer-only harness
 * snapshots) - or whose Pokémon resources fail validation - leaves the
 * Pokémon tables at their compiled payloads (until R9 §7): nothing mutated,
 * no stale pointer ever installed, and the trainer family's own
 * publication stands (the seam's trainer contract predates R9). The trainer
 * seam clears the diagnostics on that degraded path so the pinned
 * trainer-contract assertions hold; the production proof catches a real
 * pack regression anyway, because its post-init Pokémon verification fails
 * loudly on any slot that did not publish. R9 §8 tightens this to hard-fail
 * once the compiled payloads are gone on native.
 */

#include "emerald/resources/emerald_resource_compat.h"
#include "gen3/resources/resource_resolver.h"

/* Resolve the whole Pokémon battle family from `snapshot`, build the
 * compatibility image and publish every table slot. Returns EMERALD_COMPAT_OK
 * on success; on any failure nothing is mutated and *diagnostics (may be
 * NULL) carries the first failing resource. */
enum EmeraldResourceCompatStatus
EmeraldPokemonCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

/* Re-derive every table pointer from the retained session image. Allocation
 * free, idempotent. Fails closed (EMERALD_COMPAT_ERR_UNAVAILABLE) when no
 * image is installed. */
enum EmeraldResourceCompatStatus
EmeraldPokemonCompat_Republish(struct EmeraldResourceCompatDiagnostics *diagnostics);

/* NULL the migrated .data pointers of every published slot (the external
 * back-EGG slot is untouched). */
void EmeraldPokemonCompat_ClearMigratedEntries(void);

/* Destroy the session image and release the publication state. */
void EmeraldPokemonCompat_Shutdown(void);

/* R10-C: expose the retained session image and the per-entry schema (the
 * image itself does not store schemas; the generated resource table does).
 * NULL / 0 on no session or an out-of-range entry index. */
const struct EmeraldResourceCompatibilityImage *EmeraldPokemonCompat_GetImage(void);
uint32_t EmeraldPokemonCompat_GetEntrySchema(size_t entryIndex);

#endif

#ifndef EMERALD_RESOURCES_EMERALD_TRAINER_NATIVE_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_TRAINER_NATIVE_COMPAT_H

/* Native-target compatibility publication (Stage R5, generalized in R7B,
 * extended to the trainer-BACK family in R8).
 *
 * The GBA build declares gTrainerFrontPicTable/gTrainerFrontPicPaletteTable as
 * const compile-time assets. On the native target those two tables are declared
 * non-const (see include/data.h + src/data/trainer_graphics/front_pic_tables.h)
 * so that the compatibility seam can publish the ROM_BASE-resolved payload
 * streams into them at session init - the existing Emerald
 * decompression/load consumers then read every trainer exactly as they always
 * have.
 *
 * R5 published exactly three migrated slots (Brendan's front sheet, front
 * palette and back-pic palette). R7B generalizes the seam to the FULL
 * trainer-front family: all 93 front sheet slots and all 93 front palette
 * slots of the live native tables can receive ROM_BASE-backed streams, plus
 * the 6 gTrainerBackPicPaletteTable slots that consume front normal-palette
 * resources (brendan, may, rs-brendan, rs-may, wally, steven - the shared
 * back-pic palette consumers, R7A §5). The resource-id -> table-slot mapping
 * mirrors the R7A family descriptor
 * (resources/extraction/emerald/bpee01/trainer_front_family.toml); the
 * generalized test harness verifies the seam against the descriptor so the
 * mapping cannot drift.
 *
 * R8 extends the seam to the trainer-BACK family: the compatibility image
 * carries 196 entries (the front 186 unchanged, then the back family: 8 raw
 * back sheets in TRAINER_BACK_PIC_* order, then the 2 back-only palettes
 * red/leaf). Publication additionally serves
 *   - the 8 gTrainerBackPicTable sheet slots (live READ: the same
 *     DecompressTrainerBackPic dereferences the data pointer every battle,
 *     though its decompression output is unused and overwritten later), and
 *   - the 34 gTrainerBackPicTable_<X> SpriteFrameImage slots, the LIVE pixel
 *     surface (sTrainerBackSpriteTemplates -> SetMultiuseSpriteTemplateTo-
 *     TrainerBack -> CreateSprite -> RequestSpriteFrameImageCopy), and
 *   - the 2 Red/Leaf gTrainerBackPicPaletteTable slots (live LoadCompressed-
 *     Palette in the same DecompressTrainerBackPic).
 * The mapping mirrors the R8 family descriptor
 * (resources/extraction/emerald/bpee01/trainer_back_family.toml); the test
 * harness verifies the seam against both descriptors so they cannot drift.
 * Nothing is published into non-live surfaces: there are none in the back
 * family - every back table slot is read by live gameplay code (the sheet
 * table by the decompressor, the frame tables by the sprite pipeline, the
 * palette table by LoadCompressedPalette).
 *
 * This module is the ONE explicit init point (§14): it resolves the whole
 * family through the normal active M0/M1 snapshot, verifies
 * type/schema/size/winner for every resource, builds the
 * EmeraldResourceCompatibilityImage transactionally, and publishes its
 * literal-only LZ77 streams into the live native trainer tables. It changes
 * ONLY the migrated .data slots (§7) and never touches the renderer (§18).
 *
 * Emerald-specific (trainer-table assumptions are fine here, per R5 §26);
 * platform-neutral in its interface so tests can compile it, but the
 * implementation is guarded to the native SDL2 target.
 */

#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_ranges.h"

/* Family size: 93 front table entries (TRAINER_PIC_HIKER .. TRAINER_PIC_RS_MAY),
 * one sheet + one normal palette resource each (R7A §2). */
#define EMERALD_TRAINER_FRONT_COUNT 93u

/* R8 back family (trainer_back_family.toml): 8 back sheets (raw 4bpp,
 * TRAINER_BACK_PIC_BRENDAN .. TRAINER_BACK_PIC_STEVEN) + 2 back-only
 * palettes (red, leaf; gba-lz77). The 6 other back-palette slots are R7B
 * aliases to front normal-palette resources and add no image entries. */
#define EMERALD_TRAINER_BACK_SHEET_COUNT   8u
#define EMERALD_TRAINER_BACK_PALETTE_COUNT 2u
#define EMERALD_TRAINER_BACK_ENTRY_COUNT \
    (EMERALD_TRAINER_BACK_SHEET_COUNT + EMERALD_TRAINER_BACK_PALETTE_COUNT)

/* Compatibility image layout: the front 186 entries unchanged (sheet at 2i,
 * palette at 2i+1 for table index i), then the back family appended - the 8
 * sheets in TRAINER_BACK_PIC_* table order at FRONT_ENTRY_COUNT..+7, then the
 * 2 palettes (red, leaf) at FRONT_ENTRY_COUNT+8..+9. */
#define EMERALD_TRAINER_FRONT_ENTRY_COUNT (EMERALD_TRAINER_FRONT_COUNT * 2u)
#define EMERALD_TRAINER_FAMILY_ENTRY_COUNT \
    (EMERALD_TRAINER_FRONT_ENTRY_COUNT + EMERALD_TRAINER_BACK_ENTRY_COUNT)

/* Publish the compatibility image into the live native trainer tables.
 * Validates the image up front (entry count 196 = 2*93 + 10, per-entry
 * type/decoded size, name matches the family mappings) and only then mutates
 * the migrated data slots: every [i].data in the front sheet and front
 * palette tables, the 6 shared back-pic palette slots, and in R8 the back
 * family - the 8 gTrainerBackPicTable sheet slots, the 34
 * gTrainerBackPicTable_<X> SpriteFrameImage slots, and the 2 Red/Leaf
 * back-palette slots. On failure no table entry is modified (§13
 * transactional publication). Indices, tags, decoded sizes and every
 * non-migrated entry stay identical. */
enum EmeraldResourceCompatStatus
EmeraldResourceCompat_PublishTrainerTables(
    const struct EmeraldResourceCompatibilityImage *image,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

/* R6 post-state-load republish. Requires a valid initialized session image; it
 * is a narrow, idempotent, allocation-free re-publication of the already-valid
 * current-session compatibility pointers into the same migrated slots - no
 * re-resolution from ROM, no pack reread, no resource rebuild, no pointer
 * serialization (§2/§3). Returns EMERALD_COMPAT_ERR_UNAVAILABLE (fail closed)
 * when no session image exists; on failure the caller is expected to clear the
 * migrated entries so stale pointers cannot survive. */
enum EmeraldResourceCompatStatus
EmeraldResourceCompat_Republish(struct EmeraldResourceCompatDiagnostics *diagnostics);

/* R6 fail-closed clear: set every migrated native table slot's data to NULL -
 * in R8 that is the whole trainer family: all 93 front sheet + 93 front
 * palette slots, the 6 shared back-palette slots, the 8 back sheet slots, the
 * 34 back SpriteFrameImage slots and the 2 Red/Leaf back-palette slots. Used
 * when a republish is not possible, so no consumer can dereference a stale
 * or unavailable pointer. */
void EmeraldResourceCompat_ClearMigratedEntries(void);

/* The one explicit R5 init point (§14): resolve the whole trainer family
 * (front 186 + back 10, R8) from the active M0/M1 snapshot through the NORMAL
 * resolver (§11), verify type/schema/payload-size/winner==ROM_BASE for every
 * resource, build the compatibility image transactionally, then publish it
 * into the live native trainer tables. Must run AFTER the ROM_BASE snapshot
 * is valid and BEFORE any trainer-graphics gameplay consumer. On any failure
 * the live tables are left untouched and the session image is unchanged; the
 * image is retained for the process session until
 * EmeraldResourceCompat_Shutdown.
 *
 * R9 §5/§8: the Pokémon battle family publishes from the same snapshot with
 * the same lifecycle. STRICT by default: a snapshot that cannot publish the
 * Pokémon family (the compiled payloads are gone from the native link since
 * R9 §7) is an init error - the trainer publication stands (per-family
 * transactionality), the diagnostics carry the first failing Pokémon
 * resource, and the caller is expected to refuse the session (the R6 runtime
 * loader rolls the tables back and returns the failure). The additive
 * opt-in below is for offline/synthetic snapshots only (the unit harness). */
enum EmeraldResourceCompatStatus
EmeraldResourceCompat_InitializeFromSnapshot(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

/* R9 §8 additive opt-in (unit harness only): the Pokémon battle family is
 * optional - its failure degrades to NULL sentinels with the diagnostics
 * cleared and the trainer family's success stands. The game path never uses
 * this; it is how the offline trainer-only snapshots keep their pinned
 * fail-closed contract. */
enum EmeraldResourceCompatStatus
EmeraldResourceCompat_InitializeFromSnapshotAllowPokemonDegradation(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

/* Runtime hook (R6 loader feeds a snapshot here; the content-hydration path
 * calls TryInitialize once). Idempotent: initialization runs at most once per
 * registered snapshot. Until a snapshot is registered these are no-ops. */
void EmeraldResourceCompat_SetSnapshot(const struct Gen3ResourceSnapshot *snapshot);
void EmeraldResourceCompat_ClearSnapshot(void);
void EmeraldResourceCompat_TryInitialize(void);

/* R6 runtime loader (emerald_runtime_loader.c): build the production ROM_BASE
 * snapshot from the production pack and register it with the seam. The pack is
 * read from disk at the given path (the working-tree production output
 * games/emerald/base/emerald-bpee01-v1.rpack) exactly as the production harness
 * does - it is deliberately NOT embedded in the binary, because R7A requires
 * every ROM_BASE_ONLY payload's encoded/decoded bytes to be absent from the
 * native link. Idempotent (at most one registration per process session);
 * fail-closed: a NULL/empty path or an absent or invalid pack leaves the
 * snapshot unregistered and the migrated slots at their NULL sentinel.
 *
 * R9 §8: registration PUBLISHES. The loader runs the strict
 * EmeraldResourceCompat_InitializeFromSnapshot at registration; a pack that
 * cannot serve the Pokémon battle family (the compiled payloads are gone from
 * the native link since R9 §7) is a refused session - the loader rolls the
 * tables back, drops the snapshot, and returns the failure, so the
 * content-hydration path's TryInitialize stays a no-op and the migrated slots
 * stay at their NULL sentinel. Call once at native startup before the
 * content-hydration path's TryInitialize. */
enum EmeraldResourceCompatStatus
EmeraldResourceCompat_RegisterRuntimeSnapshot(const char *packPath);

/* R12-E §12.2/§12.3: whether a runtime session is registered (set only by a
 * fully successful RegisterRuntimeSnapshot; a refused session leaves it
 * false). Session-ful links require the post-load audio republish to
 * succeed; session-less links skip it. */
bool EmeraldResourceCompat_IsSessionRegistered(void);

/* Release the session image. Leaves the tables as last published (the streams
 * die with the image, so only call when no consumer can read them). */
void EmeraldResourceCompat_Shutdown(void);

/* R10-C: the reverse resource-range index over the published session
 * images' streams, rebuilt whenever a new session image is adopted. NULL
 * while no valid index exists. Non-const because seams register their own
 * spans into the shared index at publish: the trainer rebuild resets it and
 * re-registers the image streams (RebuildRangeIndex), and the audio seam
 * (R12-C) appends its arena spans + hull after publication - the state
 * system (native_state.c) only ever reads it. */
struct EmeraldResourceRangeIndex *EmeraldResourceCompat_GetRangeIndex(void);

/* R10-F: the session content fingerprint, computed by the R6 loader from
 * the pack-derived session info at registration. Until set (or after
 * Shutdown), save/load uses the legacy constant fingerprint. */
void EmeraldResourceCompat_SetSessionContentFingerprint(
    const uint8_t digest[GEN3_PACK_SHA256_SIZE]);
bool EmeraldResourceCompat_GetSessionContentFingerprint(
    uint8_t outDigest[GEN3_PACK_SHA256_SIZE]);

#endif

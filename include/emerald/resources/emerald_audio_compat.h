#ifndef EMERALD_RESOURCES_EMERALD_AUDIO_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_AUDIO_COMPAT_H

/* R12-B: Emerald audio leaf ownership migration - verbatim-zone arena.
 *
 * The 569 audio LEAF resources (105 root + 51 phoneme + 388 cry samples, 25
 * programmable waves) are extracted from the retail-qualified ROM into the
 * production pack (R12-B extractor: resources/extraction/emerald/bpee01/
 * audio/{catalog,bindings,manifest.production}.toml) and are part of the
 * runtime ROM_BASE session (the pack-derived catalog covers every pack
 * entry). This seam publishes the canonical bytes into ONE consolidated
 * process-lifetime arena, a VERBATIM ZONE mapped at ROM-relative offsets:
 *
 *     arenaOffset = romAddr - EMERALD_AUDIO_ROM_START
 *
 * so every leaf sits at the exact GBA-address-relative offset it will occupy
 * once the compiled payloads are removed from the native link (R12-G). The
 * zone spans [0x0867709C, 0x089A3DB4] (3,329,304 B); the 569 payloads
 * (2,274 KiB) cover 71.8% of it, and the unbacked holes (0x0867709C.. first
 * payload and gaps) are zeroed - a single bounded allocation, no per-resource
 * heap allocations.
 *
 * R12-C extends the arena with the 202 STRUCTURAL resources (195 voicegroups,
 * 2 cry tables, 5 keysplit runs): their GBA-form 12-byte rows are
 * transformed to the native 24-byte width (per-row pointers resolved at
 * publish against the leaf table and the structural labels) into a
 * TRANSFORMED ZONE appended after the verbatim zone; the keysplit runs are
 * copied into the verbatim zone at their ROM-relative offsets. See docs/
 * R12C_STRUCTURAL_AUDIO_IMPLEMENTATION_PLAN.md §1-2.
 *
 * R12-B/R12-C ADDITIVE CONTRACT: no consumer reads the arena until the
 * R12-C consumer redirect lands (cry accessor + logical-address
 * registration; sound.c / host_memory.c changes are separate files). A
 * failed publication is a degrade, not a session refusal: the diagnostics
 * name the first failing resource, the arena stays absent, and the game
 * sounds exactly as pre-R12-B because no consumer reads the arena either
 * way. No compiled audio object is removed from the native executable (the
 * compiled payloads remain the live audio source until R12-G).
 *
 * Platform-neutral (no global.h/MP2K/frontend objects) so the offline test
 * harness can compile and drive it directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"

/* The audio section's ROM span (R12 audit): the section starts with the
 * structural tables - voicegroup_dummy (voicegroup000) at 0x08675D04, the
 * first 9 voicegroup labels below the leaf zone - and the LEAF zone starts
 * at 0x0867709C (the first sample); both end at the last leaf (0x089A3DB4).
 * Offsets inside the verbatim zone are ROM-relative to the leaf-zone start,
 * exactly as the R12-G consumers will index them. Structural labels may sit
 * anywhere in the section (dummy and the early tables precede the leaf
 * zone); only the leaf zone is ROM-relative. */
#define EMERALD_AUDIO_SECTION_START 0x08675D04u
#define EMERALD_AUDIO_ROM_START 0x0867709Cu
#define EMERALD_AUDIO_SPAN_END  0x089A3DB4u
#define EMERALD_AUDIO_SPAN_SIZE (EMERALD_AUDIO_SPAN_END - EMERALD_AUDIO_ROM_START)

/* The R12-B leaf family: exact counts pinned from the R12-A inventory. The
 * seam refuses a session whose audio-sample composition differs (pack drift
 * is a hard failure, not a partial publication). */
#define EMERALD_AUDIO_ROOT_COUNT    105u
#define EMERALD_AUDIO_PHONEME_COUNT  51u
#define EMERALD_AUDIO_CRY_COUNT     388u
#define EMERALD_AUDIO_WAVE_COUNT     25u
#define EMERALD_AUDIO_LEAF_COUNT \
    (EMERALD_AUDIO_ROOT_COUNT + EMERALD_AUDIO_PHONEME_COUNT \
     + EMERALD_AUDIO_CRY_COUNT + EMERALD_AUDIO_WAVE_COUNT)

/* R12-C structural family: 195 voicegroups (20,594 GBA-form 12-byte rows),
 * 2 cry tables (388 rows each), 5 keysplit runs (372 B total). All 21,370
 * rows are transformed to the native 24-byte width (docs/
 * R12C_STRUCTURAL_AUDIO_IMPLEMENTATION_PLAN.md §1.2) in a TRANSFORMED ZONE
 * appended to the same single arena allocation. */
#define EMERALD_AUDIO_VOICEGROUP_COUNT 195u
#define EMERALD_AUDIO_CRY_TABLE_COUNT   2u
#define EMERALD_AUDIO_KEYSPLIT_COUNT    5u
#define EMERALD_AUDIO_STRUCTURAL_COUNT \
    (EMERALD_AUDIO_VOICEGROUP_COUNT + EMERALD_AUDIO_CRY_TABLE_COUNT \
     + EMERALD_AUDIO_KEYSPLIT_COUNT)
#define EMERALD_AUDIO_VOICEGROUP_ROWS 20594u
#define EMERALD_AUDIO_CRY_ROWS         776u
#define EMERALD_AUDIO_STREAM_ROWS \
    (EMERALD_AUDIO_VOICEGROUP_ROWS + EMERALD_AUDIO_CRY_ROWS)

/* Transformed-zone bytes: 21,370 rows x 24 B + the 10 drumset back-shift
 * pads (364 rows x 24 B; 9 x 36 + 1 x 40, pinned in the seam) = 521,616 B.
 * The exact figure is a seam composition gate (docs/R12C §2.1). */
#define EMERALD_AUDIO_TRANSFORM_ROWS_BYTES (EMERALD_AUDIO_STREAM_ROWS * 24u)
#define EMERALD_AUDIO_DRUMSET_PAD_ROWS      364u
#define EMERALD_AUDIO_DRUMSET_PAD_BYTES \
    (EMERALD_AUDIO_DRUMSET_PAD_ROWS * 24u)
#define EMERALD_AUDIO_TRANSFORM_SIZE \
    (EMERALD_AUDIO_TRANSFORM_ROWS_BYTES + EMERALD_AUDIO_DRUMSET_PAD_BYTES)

/* The native-width audio row (24 bytes): the exact layout the LINUX64
 * assembler emits for a ToneData (music_voice.inc LINUX64 branches: four
 * scalar bytes, .space 4, .quad union, ADSR bytes, .space 4). Opaque in the
 * seam (platform-neutral); the MP2K consumer casts to struct ToneData - the
 * R12-C parity gate proves byte equality with the assembler output for all
 * 21,370 rows. */
struct EmeraldAudioToneRow
{
    uint8_t bytes[24];
};

enum EmeraldAudioCompatStatus
{
    EMERALD_AUDIO_OK = 0,
    EMERALD_AUDIO_ERR_INVALID_ARGUMENT, /* NULL snapshot/pack pointer */
    EMERALD_AUDIO_ERR_OUT_OF_MEMORY,
    EMERALD_AUDIO_ERR_RESOLVE_FAILED,   /* M0/M1 snapshot did not resolve a leaf */
    EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH, /* pack/view size or bytes differ */
    EMERALD_AUDIO_ERR_UNEXPECTED_COUNT, /* audio-sample composition != 569 leaves */
    EMERALD_AUDIO_ERR_UNEXPECTED_OWNERSHIP, /* winner is not the ROM_BASE provider */
    EMERALD_AUDIO_ERR_UNAVAILABLE,      /* no published arena to republish */
    EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION, /* structural composition != 202/21,370 */
    EMERALD_AUDIO_ERR_UNRESOLVED_POINTER, /* a row pointer failed to resolve */
    EMERALD_AUDIO_ERR_RANGE_REGISTRATION, /* R10 range index registration failed (§8) */
};

/* Structured diagnostics for the arena build (same shape as the other compat
 * seams: stable identifiers, sizes and dispositions - never pointers or
 * paths). On failure the FIRST failing leaf is named. */
struct EmeraldAudioCompatDiagnostics
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

struct EmeraldAudioArena;

/* Resolve every audio-sample leaf of the session through the NORMAL M0/M1
 * snapshot (type audio-sample, schema 1, winner must be the ROM_BASE
 * provider), cross-check the resolver view against the pack entry (size and
 * byte equality), validate the composition (exactly 105/51/388/25), then copy
 * the canonical bytes into the single verbatim-zone arena at
 * romAddr - EMERALD_AUDIO_ROM_START. Transactional: every validation runs
 * before the arena is allocated; on any failure the arena stays absent and
 * the diagnostics name the first failing leaf. Idempotent for a new session:
 * an already-published arena is replaced atomically only after the new
 * session validates. `pack` must be the pack the session was built from (its
 * sourceRomOffset + payload carry the build-time ROM provenance). */
enum EmeraldAudioCompatStatus
EmeraldAudioCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldAudioCompatDiagnostics *diagnostics);

/* R12-B republish: the arena is immutable and nothing publishes consumer
 * pointers into live tables, so republish re-validates that a published
 * arena exists (EMERALD_AUDIO_OK) or fails closed (EMERALD_AUDIO_ERR_
 * UNAVAILABLE). R12-F replaces this with pointer re-derivation. */
enum EmeraldAudioCompatStatus
EmeraldAudioCompat_Republish(struct EmeraldAudioCompatDiagnostics *diagnostics);

/* Fail-closed clear: release the arena (nothing else is published in
 * R12-B). Used when a session is rolled back or shut down. */
void EmeraldAudioCompat_ClearMigratedEntries(void);
void EmeraldAudioCompat_Shutdown(void);

/* Query helpers (tests + future R12 consumers). GetArena's size now spans
 * the verbatim zone PLUS the R12-C transformed zone (the two are contiguous
 * from the zone base); the R12-B zone offsets are unchanged. */
bool EmeraldAudioCompat_GetArena(const uint8_t **outBase, size_t *outSize);
/* Arena layout query: all offsets are relative to the arena struct base
 * (sArena->bytes[0]); the verbatim zone occupies [zoneOffset, +spanSize) and
 * the transformed zone [transformOffset, +transformSize). The 8-byte-aligned
 * transformOffset may leave a small gap between the two zones; GetArena's
 * size spans the verbatim zone only, so callers that walk the transform zone
 * (tests, the offline parity gate) use this accessor instead. */
bool EmeraldAudioCompat_GetArenaLayout(size_t *outZoneOffset,
                                       size_t *outTransformOffset,
                                       size_t *outSpanSize,
                                       size_t *outTransformSize);
size_t EmeraldAudioCompat_GetPublishedCount(void);
bool EmeraldAudioCompat_GetLeafSpan(const char *canonicalName,
                                    size_t *outArenaOffset, size_t *outSize);
bool EmeraldAudioCompat_GetLeafBytes(const char *canonicalName,
                                     const uint8_t **outBytes, size_t *outSize);

/* R12-C structural queries. */
size_t EmeraldAudioCompat_GetStructuralCount(void);
bool EmeraldAudioCompat_GetTransformedRows(const uint8_t **outBase,
                                           size_t *outSize);
bool EmeraldAudioCompat_GetVoicegroupSpan(const char *canonicalName,
                                          size_t *outTransformOffset,
                                          size_t *outRowCount);
bool EmeraldAudioCompat_GetCryTableSpan(bool reversed,
                                        size_t *outTransformOffset,
                                        size_t *outRowCount);
bool EmeraldAudioCompat_GetKeysplitSpan(const char *canonicalName,
                                        size_t *outVerbatimOffset,
                                        size_t *outRunBytes);

/* The R12-C cry row accessor (sound.c GET_CRY consumer): row `index` of the
 * 128-row bank `table` (0..3) in the forward or reverse transformed cry
 * block. `128*table + index` must be < 388 (the compiled bound; SpeciesToCryId
 * caps ids at <= 387); out-of-range or unpublished -> NULL. */
const struct EmeraldAudioToneRow *
EmeraldAudioCryTableRow(uint8_t table, bool reversed, uint8_t index);

/* Logical-label enumeration for the host_memory exact-start table (197
 * entries: 195 voicegroup labels + 2 cry table labels, host addresses
 * resolved at publish). Platform-neutral: the platform layer registers the
 * labels with HostMemoryRegisterLogicalAddress via this callback. */
typedef void (*EmeraldAudioLogicalLabelCallback)(uint32_t gbaAddr,
                                                 const void *hostBase,
                                                 void *user);
void EmeraldAudioCompat_ForEachLogicalLabel(
    EmeraldAudioLogicalLabelCallback callback, void *user);

#endif

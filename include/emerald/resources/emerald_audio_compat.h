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
 * R12-B ADDITIVE CONTRACT: nothing consumes the arena yet. There are no
 * logical-range registrations (R12-F), no HostResolveGbaAddr ranges, no
 * State v5 ranges, no MP2K redirect, and no compiled audio object is removed
 * from the native executable (the compiled payloads remain the live audio
 * source until R12-G). A failed publication is a degrade, not a session
 * refusal: the diagnostics name the first failing resource, the arena stays
 * absent, and the game sounds exactly as pre-R12-B because no consumer reads
 * the arena either way.
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

/* The audio section's ROM span (R12 audit): the voicegroup_dummy anchor
 * starts the audio section at 0x0867709C; the span ends at the last leaf
 * (0x089A3DB4). Offsets inside the verbatim zone are ROM-relative to the
 * start, exactly as the R12-G consumers will index them. */
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

/* Query helpers (tests + future R12 consumers). */
bool EmeraldAudioCompat_GetArena(const uint8_t **outBase, size_t *outSize);
size_t EmeraldAudioCompat_GetPublishedCount(void);
bool EmeraldAudioCompat_GetLeafSpan(const char *canonicalName,
                                    size_t *outArenaOffset, size_t *outSize);
bool EmeraldAudioCompat_GetLeafBytes(const char *canonicalName,
                                     const uint8_t **outBytes, size_t *outSize);

#endif

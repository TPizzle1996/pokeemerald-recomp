#ifndef EMERALD_RESOURCES_EMERALD_RESOURCE_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_RESOURCE_COMPAT_H

/* Emerald runtime compatibility seam (Stage R5, generalized in R7B).
 *
 * The GBA Emerald runtime only consumes graphics through compressed
 * sprite-sheet/palette tables (CompressedSpriteSheet/CompressedSpritePalette)
 * and the GBA LZ77 codec. The M0/M1 resolver hands the runtime canonical,
 * already-validated ROM_BASE payloads for the trainer-front family (93 sheets,
 * each 2048 bytes / type TILE_GRAPHICS, and 93 normal palettes, each 32 bytes /
 * type PALETTE). This module is the permanent compatibility representation
 * between those two worlds:
 *
 *   ROM_BASE resolved canonical payload
 *       -> EmeraldResourceCompatibilityImage (literal-only GBA LZ77 streams)
 *       -> existing trainer front/back palette table entries
 *       -> existing Emerald decompression/load consumers
 *
 * R5 built the image from exactly two resources (Brendan's sheet + palette).
 * R7B generalizes it to a full family: the image is created from an ordered
 * list of source entries (canonical name, type, schema, payload), validated
 * against the family invariants (sheets decode to 2048 bytes, palettes to 32),
 * and published to the live native trainer tables by the compatibility seam
 * (emerald_trainer_native_compat.c), which owns the resource-id -> table-slot
 * mapping.
 *
 * R8 adds the trainer-BACK family: back sheets (8192/10240 bytes) enter with a
 * per-entry expectedSize override and EMERALD_COMPAT_ENTRY_RAW encoding - the
 * payload bytes ARE the stream, because the back sheets' live consumers are
 * not decompressors (the sprite pipeline copies frame data straight into OBJ
 * VRAM, and DecompressTrainerBackPic must see the same raw bytes the GBA build
 * links). Palettes stay LZ-encoded as before.
 *
 * The image is built transactionally (all-or-nothing), immutable after
 * construction, owns a single permanent session-lifetime allocation, and is
 * completely independent of the temporary pack/provider/candidate objects it
 * was resolved from: payloads AND canonical names are copied into the image
 * arena.
 *
 * This module is Emerald-specific (trainer-family invariants are fine here,
 * per R5 §26) but platform-neutral: it does not include global.h, sprite.h,
 * or any frontend object. The literal-only LZ77 codec it uses lives in the
 * reusable gen3 module resource_lz.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_lz.h"
#include "gen3/resources/resource_types.h"

/* Stream encoding for a compatibility-image entry (see the source-entry
 * struct below). */
enum EmeraldResourceCompatEntryEncoding
{
    EMERALD_COMPAT_ENTRY_LZ = 0,  /* literal-only LZ77 stream (R5/R7B) */
    EMERALD_COMPAT_ENTRY_RAW = 1, /* payload bytes verbatim (R8 back sheets) */
    /* R9 §5 (Pokémon battle family): the payload bytes ARE a GBA LZ77 stream,
     * so the stream is the payload verbatim and the existing GBA decompressors
     * LZ77UnCompWram it exactly as they do on the GBA build. For the Pokémon
     * family the pack stores the DECODED representation of each retail
     * stream, so the seam re-encodes it into a byte-deterministic
     * literal-only stream (Gen3LzLiteral_Encode) before building the image -
     * the image never sees the pack's decoded bytes, only the synthesized
     * stream. expectedSize is the DECODED size the stream declares
     * (4096/2048/32/8192/128 per resource, from the generated bindings) -
     * the encoded length carries no invariant. */
    EMERALD_COMPAT_ENTRY_GBA_LZ = 2,
};

/* Family-wide canonical invariants (R7A §3): every trainer-front sheet decodes
 * to exactly 2048 bytes and every trainer-front normal palette to exactly 32
 * bytes, independent of the table `.size` consumer-allocation multiplier. */
#define EMERALD_TRAINER_SHEET_SIZE  2048u /* TRAINER_PIC_SIZE: 64*64/2 */
#define EMERALD_TRAINER_PALETTE_SIZE 32u  /* 16 colors * 2 bytes */
#define EMERALD_TRAINER_SCHEMA       1u

enum EmeraldResourceCompatStatus
{
    EMERALD_COMPAT_OK = 0,
    EMERALD_COMPAT_ERR_INVALID_ARGUMENT,   /* NULL image/entry/payload pointer */
    EMERALD_COMPAT_ERR_OUT_OF_MEMORY,
    EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH, /* entry size != 2048 (sheet) / 32 (palette) */
    EMERALD_COMPAT_ERR_ENCODE_FAILED,      /* literal LZ encode rejected input */
    EMERALD_COMPAT_ERR_RESOLVE_FAILED,     /* M0/M1 snapshot did not resolve */
    EMERALD_COMPAT_ERR_PUBLISH_FAILED,     /* native table publication refused */
    EMERALD_COMPAT_ERR_UNAVAILABLE,        /* no valid session image to republish */
};

/* Structured diagnostics for the compat construction pipeline (R5 §27).
 * Only stable identifiers, sizes and dispositions - never pointers or paths.
 * For family-wide operations the diagnostics carry the FIRST failing entry. */
struct EmeraldResourceCompatDiagnostics
{
    char canonicalName[96];
    char stage[24];                /* "build" / "resolve" / "publish" */
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

struct EmeraldResourceCompatibilityImage;

/* One source entry for the family image: an already-resolved canonical payload
 * plus its stable identity. The image copies both the payload and the name, so
 * the pointers need only remain valid for the duration of the create call. */
struct EmeraldResourceCompatSourceEntry
{
    const char *canonicalName;   /* stable M0/M1 key, e.g. "emerald:trainer/hiker/battle/front/sheet" */
    enum Gen3ResourceType type;  /* TILE_GRAPHICS (sheet) or PALETTE (normal palette) */
    uint32_t schema;
    const uint8_t *payload;
    uint32_t payloadSize;
    /* R8: per-entry expected size override. 0 (the R7B default) means
     * "derive from type": 2048 for TILE_GRAPHICS, 32 for PALETTE. The back
     * sheets (8192/10240) pass frameCount*2048 explicitly. R9: for
     * EMERALD_COMPAT_ENTRY_GBA_LZ entries this is the stream's DECODED size
     * (what its LZ77 header declares), never the encoded payload length. */
    uint32_t expectedSize;
    /* R8/R9: stream encoding. EMERALD_COMPAT_ENTRY_LZ (the R7B default) serves
     * literal-only LZ77 streams to the existing decompressor consumers
     * (trainer front sheets, all trainer palettes). EMERALD_COMPAT_ENTRY_RAW
     * serves the payload bytes verbatim as the stream - required for the
     * trainer back sheets, whose live consumers are NOT decompressors: the
     * sprite pipeline copies images[frame].data straight into OBJ VRAM, and
     * DecompressTrainerBackPic must LZ77-decode the same raw bytes it decodes
     * on the GBA build. EMERALD_COMPAT_ENTRY_GBA_LZ (R9) also serves the
     * payload bytes verbatim, but the payload is a GBA LZ77 stream: the
     * Pokémon battle tables' payloads ARE the retail ROM's compressed bytes
     * (source_encoding "gba-lz77"), and the existing decompressors
     * (DecompressPicFromTable_2, LoadSpecialPokePic_2, LoadCompressedSprite
     * Palette, ...) LZ77UnCompWram them exactly as on the GBA build. */
    enum EmeraldResourceCompatEntryEncoding encoding;
};

const char *EmeraldResourceCompatStatus_Describe(
    enum EmeraldResourceCompatStatus status);

/* Build the compatibility image from `entryCount` source entries.
 *
 * Every entry is validated up front (fail closed unless the type is
 * TILE_GRAPHICS or PALETTE and payloadSize matches the entry's expected size -
 * the R7B family invariant 2048/32 derived from type, or the R8 per-entry
 * expectedSize override; R9 GBA_LZ entries instead must be a GBA LZ77 stream
 * whose header-declared decoded size equals expectedSize). The image is a
 * single permanent allocation holding
 * the entry table, the canonical names, the canonical payload copies and the
 * streams (literal-only LZ77 for LZ entries, the payload bytes verbatim for
 * RAW entries), in the same order as `entries`. On success *outImage is owned
 * by the caller and immutable; on failure nothing is allocated and *outImage
 * is left NULL. `diagnostics` (may be NULL) is filled with the construction
 * stage and the first mismatch detail. */
enum EmeraldResourceCompatStatus
EmeraldResourceCompatImage_CreateFamily(
    const struct EmeraldResourceCompatSourceEntry *entries,
    size_t entryCount,
    struct EmeraldResourceCompatibilityImage **outImage,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

void EmeraldResourceCompatImage_Destroy(struct EmeraldResourceCompatibilityImage *image);

size_t EmeraldResourceCompatImage_GetEntryCount(
    const struct EmeraldResourceCompatibilityImage *image);
const char *EmeraldResourceCompatImage_GetEntryName(
    const struct EmeraldResourceCompatibilityImage *image, size_t index);
enum Gen3ResourceType EmeraldResourceCompatImage_GetEntryType(
    const struct EmeraldResourceCompatibilityImage *image, size_t index);

/* Per-entry accessors: the literal-only LZ77 stream (what the runtime tables
 * publish and the Emerald decompressors consume), its encoded size, the
 * canonical decoded payload copy and its decoded size. */
const uint8_t *EmeraldResourceCompatImage_GetStream(
    const struct EmeraldResourceCompatibilityImage *image, size_t index);
size_t EmeraldResourceCompatImage_GetStreamSize(
    const struct EmeraldResourceCompatibilityImage *image, size_t index);
uint32_t EmeraldResourceCompatImage_GetDecodedSize(
    const struct EmeraldResourceCompatibilityImage *image, size_t index);
const uint8_t *EmeraldResourceCompatImage_GetCanonical(
    const struct EmeraldResourceCompatibilityImage *image, size_t index);
uint32_t EmeraldResourceCompatImage_GetCanonicalSize(
    const struct EmeraldResourceCompatibilityImage *image, size_t index);

/* R10-C: the whole arena allocation span (the bytes[] region). Returns
 * false on NULL image. */
bool EmeraldResourceCompatImage_GetArenaSpan(
    const struct EmeraldResourceCompatibilityImage *image,
    const uint8_t **outBase, size_t *outSize);

/* The EXPOSED span of the arena: the stream block (first stream offset ..
 * arena end). Only the streams are published to runtime tables; the
 * [entry table][names][canonical payloads] prefix is build-time-only and
 * game state never references it. Used by the reverse range index as the
 * resource-owned "hull": a pointer inside this span that matches no
 * registered entry range is a capture error, never a silently persisted
 * value. A value in the unexposed prefix is ordinary data. Returns false
 * on NULL image or an empty/invalid stream block. */
bool EmeraldResourceCompatImage_GetExposedSpan(
    const struct EmeraldResourceCompatibilityImage *image,
    const uint8_t **outBase, size_t *outSize);

#endif

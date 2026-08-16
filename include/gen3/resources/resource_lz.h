#ifndef GEN3_RESOURCES_RESOURCE_LZ_H
#define GEN3_RESOURCES_RESOURCE_LZ_H

/* Deterministic literal-only GBA LZ77 encoder (Stage R5).
 *
 * The M0/M1 snapshots hand the runtime canonical payload bytes for ROM-derived
 * resources. The existing Emerald runtime only consumes graphics through the
 * GBA LZ77 codec (LZ77UnCompWram / LZDecompressVram and the compressed
 * sprite-sheet/palette loaders). This module is the compatibility seam that
 * turns a canonical payload into a valid, deterministic GBA LZ77 stream WITHOUT
 * being a general compressor: every group is emitted as a literal run (flag
 * byte 0x00 followed by up to eight literal bytes).
 *
 * The stream is byte-deterministic for a given payload and decodes to exactly
 * the input payload through the real GBA decompressor (the 24-bit decoded size
 * in the header terminates the output; trailing zero padding in the final
 * partial group is read by the decompressor but never written). It is not a
 * replacement for the Emerald codec and never back-references.
 *
 * The module is platform-neutral and reusable: it knows nothing about Emerald,
 * BPEE01, Brendan, FireRed, or any game symbol.
 */

#include <stddef.h>
#include <stdint.h>

enum Gen3LzResult
{
    GEN3_LZ_OK = 0,
    GEN3_LZ_ERR_INVALID_ARGUMENT,  /* NULL payload/out, or payloadSize == 0 */
    GEN3_LZ_ERR_BUFFER_TOO_SMALL,  /* outCapacity below the encoded size */
    GEN3_LZ_ERR_SIZE_OVERFLOW,     /* encoded size cannot be represented */
};

/* Exact encoded size of a literal-only LZ77 stream for a payload of
 * `payloadSize` bytes (GBA decoded-size field is 24-bit, so payloadSize must
 * be <= 0xFFFFFF for a valid stream; returns SIZE_MAX when payloadSize is
 * zero or exceeds the field). Does not require a destination buffer. */
size_t Gen3LzLiteral_EncodedSize(uint32_t payloadSize);

/* Encode `payload[0..payloadSize)` as a literal-only GBA LZ77 stream into
 * `out` (capacity `outCapacity`). On success writes the stream, sets
 * *outEncodedSize (may be NULL) and returns GEN3_LZ_OK. Fails closed on NULL
 * arguments, zero payloadSize, size overflow, or a too-small destination;
 * `out` is left untouched on failure. */
enum Gen3LzResult Gen3LzLiteral_Encode(const uint8_t *payload,
                                       uint32_t payloadSize,
                                       uint8_t *out,
                                       size_t outCapacity,
                                       size_t *outEncodedSize);

#endif

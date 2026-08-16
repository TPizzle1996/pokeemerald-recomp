#ifndef GEN3_RESOURCES_LZ77_H
#define GEN3_RESOURCES_LZ77_H

#include <stddef.h>
#include <stdint.h>

/* Strict GBA LZ77 decoder for the pret `.lz` container format.
 *
 * File layout (confirmed against tools/gbagfx/lz.c and the real Brendan
 * artifacts):
 *   [0]    0x10 (LZ77 type byte)
 *   [1..3] 24-bit little-endian decoded size
 *   [4..]  control/block stream
 *   [..]   zero-padded to a 4-byte multiple
 *
 * Stream: one control byte per group of up to 8 items, bits consumed MSB-first.
 *   bit = 1: 2-byte block:
 *       blockSize    = (b0 >> 4) + 3          (3..18 bytes)
 *       blockDistance= ((b0 & 0xF) << 8 | b1) + 1   (1..4096)
 *       copy `blockSize` bytes from destPos - blockDistance (byte-by-byte,
 *       so overlapping/RLE copies behave exactly like the GBA hardware).
 *   bit = 0: copy 1 literal byte.
 *
 * Decoding stops exactly at the declared size; every remaining input byte
 * must be zero (the 4-byte padding).
 */
enum Gen3Lz77Result
{
    GEN3_LZ77_OK = 0,
    GEN3_LZ77_BAD_HEADER,      /* fewer than 4 bytes, or type byte != 0x10 */
    GEN3_LZ77_TRUNCATED,       /* stream ends before the declared size is reached */
    GEN3_LZ77_INVALID_BACKREF, /* blockDistance exceeds bytes decoded so far */
    GEN3_LZ77_OVERFLOW,        /* decode would exceed the declared size */
    GEN3_LZ77_OVERREAD,        /* literal/block reads past the end of the input */
    GEN3_LZ77_TRAILING_DATA,   /* non-zero bytes after the declared size */
};

/* Decodes `encoded` (the full `.lz` file, header included) into `decoded`,
 * which must hold at least the declared size (see GEN3_LZ77_OVERFLOW).
 * On success *outDecodedSize is the exact decoded length. */
enum Gen3Lz77Result Gen3Lz77_Decode(const uint8_t *encoded, size_t encodedSize,
                                    uint8_t *decoded, size_t decodedCapacity,
                                    size_t *outDecodedSize);

#endif

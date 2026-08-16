#include "gen3/resources/lz77.h"

#include <stdbool.h>

enum Gen3Lz77Result Gen3Lz77_Decode(const uint8_t *encoded, size_t encodedSize,
                                    uint8_t *decoded, size_t decodedCapacity,
                                    size_t *outDecodedSize)
{
    size_t destSize;
    size_t srcPos;
    size_t destPos;

    if (outDecodedSize != NULL)
        *outDecodedSize = 0;
    if (encoded == NULL || (encodedSize != 0 && decoded == NULL))
        return GEN3_LZ77_BAD_HEADER;
    if (encodedSize < 4)
        return GEN3_LZ77_BAD_HEADER;
    if (encoded[0] != 0x10)
        return GEN3_LZ77_BAD_HEADER;

    destSize = (size_t)encoded[1]
             | ((size_t)encoded[2] << 8)
             | ((size_t)encoded[3] << 16);
    if (destSize > decodedCapacity)
        return GEN3_LZ77_OVERFLOW;

    srcPos = 4;
    destPos = 0;
    while (destPos < destSize)
    {
        uint8_t flags;
        unsigned bit;
        if (srcPos >= encodedSize)
            return GEN3_LZ77_TRUNCATED;
        flags = encoded[srcPos++];
        for (bit = 0; bit < 8 && destPos < destSize; bit++)
        {
            if ((flags & (0x80u >> bit)) != 0)
            {
                uint8_t b0;
                uint8_t b1;
                size_t blockSize;
                size_t blockDistance;
                size_t j;
                if (encodedSize - srcPos < 2)
                    return GEN3_LZ77_TRUNCATED;
                b0 = encoded[srcPos];
                b1 = encoded[srcPos + 1u];
                srcPos += 2;
                blockSize = (size_t)((b0 >> 4) + 3u);
                blockDistance = (size_t)((((b0 & 0xFu) << 8) | b1) + 1u);
                if (blockDistance > destPos)
                    return GEN3_LZ77_INVALID_BACKREF;
                for (j = 0; j < blockSize; j++)
                {
                    if (destPos >= destSize)
                        return GEN3_LZ77_OVERFLOW;
                    decoded[destPos] = decoded[destPos - blockDistance];
                    destPos++;
                }
            }
            else
            {
                if (srcPos >= encodedSize)
                    return GEN3_LZ77_TRUNCATED;
                if (destPos >= destSize)
                    return GEN3_LZ77_OVERFLOW;
                decoded[destPos] = encoded[srcPos];
                srcPos++;
                destPos++;
            }
        }
    }

    /* After the declared size the container is zero-padded to a 4-byte
     * multiple. Anything else is corruption. */
    for (; srcPos < encodedSize; srcPos++)
    {
        if (encoded[srcPos] != 0x00)
            return GEN3_LZ77_TRAILING_DATA;
    }

    if (outDecodedSize != NULL)
        *outDecodedSize = destSize;
    return GEN3_LZ77_OK;
}

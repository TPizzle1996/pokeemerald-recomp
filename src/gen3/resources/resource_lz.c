/* Deterministic literal-only GBA LZ77 encoder (Stage R5).
 * See resource_lz.h for the contract. Platform-neutral; no frontend includes. */

#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_lz.h"

#define GEN3_LZ_FLAG_LITERAL 0x00u
#define GEN3_LZ_GROUP_SIZE 8u
#define GEN3_LZ_TYPE_BYTE 0x10u
#define GEN3_LZ_HEADER_SIZE 4u
#define GEN3_LZ_MAX_DECODED_SIZE 0xFFFFFFu /* 24-bit size field */

static size_t GroupsForSize(uint32_t payloadSize)
{
    return ((size_t)payloadSize + GEN3_LZ_GROUP_SIZE - 1u) / GEN3_LZ_GROUP_SIZE;
}

size_t Gen3LzLiteral_EncodedSize(uint32_t payloadSize)
{
    size_t groups;

    if (payloadSize == 0u || payloadSize > GEN3_LZ_MAX_DECODED_SIZE)
        return SIZE_MAX;
    groups = GroupsForSize(payloadSize);
    /* 4 header bytes + one flag byte per group + eight literal bytes per group.
     * For the maximum 24-bit payload this is well below SIZE_MAX; the product
     * is still guarded so the arithmetic is overflow-checked on every target. */
    if (groups > (SIZE_MAX - GEN3_LZ_HEADER_SIZE) / (GEN3_LZ_GROUP_SIZE + 1u))
        return SIZE_MAX;
    return GEN3_LZ_HEADER_SIZE + groups * (GEN3_LZ_GROUP_SIZE + 1u);
}

enum Gen3LzResult Gen3LzLiteral_Encode(const uint8_t *payload,
                                       uint32_t payloadSize,
                                       uint8_t *out,
                                       size_t outCapacity,
                                       size_t *outEncodedSize)
{
    size_t encodedSize;
    size_t offset = 0u;
    uint32_t remaining;

    if (payload == NULL || out == NULL || payloadSize == 0u
     || payloadSize > GEN3_LZ_MAX_DECODED_SIZE)
        return GEN3_LZ_ERR_INVALID_ARGUMENT;

    encodedSize = Gen3LzLiteral_EncodedSize(payloadSize);
    if (encodedSize == SIZE_MAX)
        return GEN3_LZ_ERR_SIZE_OVERFLOW;
    if (outCapacity < encodedSize)
        return GEN3_LZ_ERR_BUFFER_TOO_SMALL;

    out[0] = GEN3_LZ_TYPE_BYTE;
    out[1] = (uint8_t)(payloadSize & 0xFFu);
    out[2] = (uint8_t)((payloadSize >> 8) & 0xFFu);
    out[3] = (uint8_t)((payloadSize >> 16) & 0xFFu);
    offset += GEN3_LZ_HEADER_SIZE;

    remaining = payloadSize;
    while (remaining > 0u)
    {
        size_t literals = remaining < GEN3_LZ_GROUP_SIZE
                        ? (size_t)remaining : (size_t)GEN3_LZ_GROUP_SIZE;
        size_t i;

        out[offset++] = GEN3_LZ_FLAG_LITERAL;
        for (i = 0u; i < GEN3_LZ_GROUP_SIZE; i++)
        {
            /* A zero flag group reads exactly eight source bytes even in the
             * final partial group; the decompressor stops on its decoded-size
             * counter, so the padding bytes are consumed but never written.
             * Padding is zero for a deterministic stream. */
            if (i < literals)
                out[offset++] = payload[payloadSize - remaining + i];
            else
                out[offset++] = 0u;
        }
        remaining -= (uint32_t)literals;
    }

    if (outEncodedSize != NULL)
        *outEncodedSize = encodedSize;
    return GEN3_LZ_OK;
}

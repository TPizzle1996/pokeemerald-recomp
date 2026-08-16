#include "gen3/resources/sha1.h"

#include <string.h>

static uint32_t RotateLeft(uint32_t value, unsigned count)
{
    return (value << count) | (value >> (32u - count));
}

static uint32_t ReadBigEndian32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24)
         | ((uint32_t)bytes[1] << 16)
         | ((uint32_t)bytes[2] << 8)
         | bytes[3];
}

static void Transform(struct Gen3Sha1Context *context, const uint8_t block[64])
{
    uint32_t words[80];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    size_t i;

    for (i = 0; i < 16; i++)
        words[i] = ReadBigEndian32(block + i * 4u);
    for (; i < 80; i++)
    {
        words[i] = RotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];

    for (i = 0; i < 80; i++)
    {
        uint32_t f;
        uint32_t k;
        uint32_t temp;
        if (i < 20)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999u;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        temp = RotateLeft(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = RotateLeft(b, 30);
        b = a;
        a = temp;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
}

void Gen3Sha1_Init(struct Gen3Sha1Context *context)
{
    static const uint32_t initial[5] =
    {
        0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u,
    };
    memcpy(context->state, initial, sizeof(initial));
    context->bitCount = 0;
    context->blockSize = 0;
}

void Gen3Sha1_Update(struct Gen3Sha1Context *context, const void *bytes, size_t size)
{
    const uint8_t *input = bytes;
    context->bitCount += (uint64_t)size * 8u;
    while (size != 0)
    {
        size_t available = sizeof(context->block) - context->blockSize;
        size_t count = size < available ? size : available;
        memcpy(context->block + context->blockSize, input, count);
        context->blockSize += count;
        input += count;
        size -= count;
        if (context->blockSize == sizeof(context->block))
        {
            Transform(context, context->block);
            context->blockSize = 0;
        }
    }
}

void Gen3Sha1_Final(struct Gen3Sha1Context *context, uint8_t digest[GEN3_SHA1_DIGEST_SIZE])
{
    uint64_t originalBitCount = context->bitCount;
    uint8_t marker = 0x80;
    uint8_t zero = 0;
    uint8_t length[8];
    size_t i;

    Gen3Sha1_Update(context, &marker, 1);
    while (context->blockSize != 56)
        Gen3Sha1_Update(context, &zero, 1);
    for (i = 0; i < sizeof(length); i++)
        length[sizeof(length) - 1u - i] = (uint8_t)(originalBitCount >> (i * 8u));
    Gen3Sha1_Update(context, length, sizeof(length));

    for (i = 0; i < GEN3_SHA1_DIGEST_SIZE / 4u; i++)
    {
        digest[i * 4u] = (uint8_t)(context->state[i] >> 24);
        digest[i * 4u + 1u] = (uint8_t)(context->state[i] >> 16);
        digest[i * 4u + 2u] = (uint8_t)(context->state[i] >> 8);
        digest[i * 4u + 3u] = (uint8_t)context->state[i];
    }
}

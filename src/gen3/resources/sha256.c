#include "gen3/resources/sha256.h"

#include <string.h>

static const uint32_t sRoundConstants[64] =
{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t RotateRight(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32u - count));
}

static uint32_t ReadBigEndian32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24)
         | ((uint32_t)bytes[1] << 16)
         | ((uint32_t)bytes[2] << 8)
         | bytes[3];
}

static void Transform(struct Gen3Sha256Context *context, const uint8_t block[64])
{
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t i;

    for (i = 0; i < 16; i++)
        words[i] = ReadBigEndian32(block + i * 4u);
    for (; i < 64; i++)
    {
        uint32_t s0 = RotateRight(words[i - 15], 7)
                    ^ RotateRight(words[i - 15], 18)
                    ^ (words[i - 15] >> 3);
        uint32_t s1 = RotateRight(words[i - 2], 17)
                    ^ RotateRight(words[i - 2], 19)
                    ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];

    for (i = 0; i < 64; i++)
    {
        uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + sum1 + choose + sRoundConstants[i] + words[i];
        uint32_t sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void Gen3Sha256_Init(struct Gen3Sha256Context *context)
{
    static const uint32_t initial[8] =
    {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    memcpy(context->state, initial, sizeof(initial));
    context->bitCount = 0;
    context->blockSize = 0;
}

void Gen3Sha256_Update(struct Gen3Sha256Context *context, const void *bytes, size_t size)
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

void Gen3Sha256_Final(struct Gen3Sha256Context *context, uint8_t digest[32])
{
    uint64_t originalBitCount = context->bitCount;
    uint8_t marker = 0x80;
    uint8_t zero = 0;
    uint8_t length[8];
    size_t i;

    Gen3Sha256_Update(context, &marker, 1);
    while (context->blockSize != 56)
        Gen3Sha256_Update(context, &zero, 1);
    for (i = 0; i < sizeof(length); i++)
        length[sizeof(length) - 1u - i] = (uint8_t)(originalBitCount >> (i * 8u));
    Gen3Sha256_Update(context, length, sizeof(length));

    for (i = 0; i < 8; i++)
    {
        digest[i * 4u] = (uint8_t)(context->state[i] >> 24);
        digest[i * 4u + 1u] = (uint8_t)(context->state[i] >> 16);
        digest[i * 4u + 2u] = (uint8_t)(context->state[i] >> 8);
        digest[i * 4u + 3u] = (uint8_t)context->state[i];
    }
}

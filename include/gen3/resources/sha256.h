#ifndef GEN3_RESOURCES_SHA256_H
#define GEN3_RESOURCES_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct Gen3Sha256Context
{
    uint32_t state[8];
    uint64_t bitCount;
    uint8_t block[64];
    size_t blockSize;
};

void Gen3Sha256_Init(struct Gen3Sha256Context *context);
void Gen3Sha256_Update(struct Gen3Sha256Context *context, const void *bytes, size_t size);
void Gen3Sha256_Final(struct Gen3Sha256Context *context, uint8_t digest[32]);

#endif

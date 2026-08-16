#ifndef GEN3_RESOURCES_SHA1_H
#define GEN3_RESOURCES_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define GEN3_SHA1_DIGEST_SIZE 20u
#define GEN3_SHA1_HEX_SIZE (GEN3_SHA1_DIGEST_SIZE * 2u + 1u)

/* FIPS 180-4 SHA-1, used to validate the GBA ROM image. Same streaming shape
 * as the shared Gen3 SHA-256 core so the two digesters stay interchangeable. */
struct Gen3Sha1Context
{
    uint32_t state[5];
    uint64_t bitCount;
    uint8_t block[64];
    size_t blockSize;
};

void Gen3Sha1_Init(struct Gen3Sha1Context *context);
void Gen3Sha1_Update(struct Gen3Sha1Context *context, const void *bytes, size_t size);
void Gen3Sha1_Final(struct Gen3Sha1Context *context, uint8_t digest[GEN3_SHA1_DIGEST_SIZE]);

#endif

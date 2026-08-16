#ifndef GEN3_RESOURCES_RESOURCE_PACK_INTERNAL_H
#define GEN3_RESOURCES_RESOURCE_PACK_INTERNAL_H

/* Private shared helpers for the pack writer/reader. Not part of the public
 * API. All integer fields are fixed-width little endian; all offset/size
 * arithmetic is overflow-checked. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void Gen3PackPutU16(uint8_t *out, uint16_t value);
void Gen3PackPutU32(uint8_t *out, uint32_t value);
void Gen3PackPutU64(uint8_t *out, uint64_t value);
uint16_t Gen3PackGetU16(const uint8_t *in);
uint32_t Gen3PackGetU32(const uint8_t *in);
uint64_t Gen3PackGetU64(const uint8_t *in);

/* Overflow-checked u64 add / multiply. Return false on overflow. */
bool Gen3PackAddU64(uint64_t left, uint64_t right, uint64_t *out);
bool Gen3PackMulU64(uint64_t left, uint64_t right, uint64_t *out);

/* align16 with overflow checking: out = (value + 15) & ~15. */
bool Gen3PackAlign16U64(uint64_t value, uint64_t *out);

/* Bytewise canonical-name comparison over length-delimited bytes. Shorter name
 * that is a prefix sorts first. Returns <0, 0, >0 like strcmp. */
int Gen3PackCompareNameBytes(const uint8_t *left, size_t leftLength,
                             const uint8_t *right, size_t rightLength);

/* True when every byte in [bytes, bytes+size) is zero. */
bool Gen3PackBytesAllZero(const uint8_t *bytes, size_t size);

/* True when every byte in [bytes, bytes+size) is in [0x20, 0x7E]. */
bool Gen3PackIsPrintableAscii(const uint8_t *bytes, size_t size);

/* True when gameId is a printable, non-empty, NUL-terminated ASCII string with
 * all zero bytes after the first NUL. */
bool Gen3PackGameIdValid(const uint8_t gameId[GEN3_PACK_GAME_ID_SIZE]);

#endif

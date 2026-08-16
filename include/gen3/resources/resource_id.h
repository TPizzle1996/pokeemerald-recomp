#ifndef GEN3_RESOURCES_RESOURCE_ID_H
#define GEN3_RESOURCES_RESOURCE_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GEN3_RESOURCE_NAME_MAX 255u
#define GEN3_RESOURCE_KEY_SIZE 32u
#define GEN3_RESOURCE_KEY_HEX_SIZE (GEN3_RESOURCE_KEY_SIZE * 2u + 1u)

typedef struct Gen3ResourceKey
{
    uint8_t bytes[GEN3_RESOURCE_KEY_SIZE];
} Gen3ResourceKey;

typedef uint32_t Gen3ResourceHandle;

enum Gen3ResourceNameStatus
{
    GEN3_RESOURCE_NAME_VALID = 0,
    GEN3_RESOURCE_NAME_NULL,
    GEN3_RESOURCE_NAME_EMPTY,
    GEN3_RESOURCE_NAME_TOO_LONG,
    GEN3_RESOURCE_NAME_INVALID_CHARACTER,
    GEN3_RESOURCE_NAME_INVALID_NAMESPACE,
    GEN3_RESOURCE_NAME_EMPTY_SEGMENT,
    GEN3_RESOURCE_NAME_DOT_SEGMENT,
};

enum Gen3ResourceNameStatus Gen3ResourceId_ValidateCanonicalName(const char *name);
void Gen3ResourceId_DeriveKey(const char *canonicalName, Gen3ResourceKey *outKey);
bool Gen3ResourceId_KeyEqual(const Gen3ResourceKey *left, const Gen3ResourceKey *right);
void Gen3ResourceId_FormatKeyHex(const Gen3ResourceKey *key,
                                 char outHex[GEN3_RESOURCE_KEY_HEX_SIZE]);

#endif

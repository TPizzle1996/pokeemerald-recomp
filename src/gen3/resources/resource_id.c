#include "gen3/resources/resource_id.h"

#include <string.h>

#include "gen3/resources/sha256.h"

static bool IsSegmentCharacter(char character)
{
    return (character >= 'a' && character <= 'z')
        || (character >= '0' && character <= '9')
        || character == '.' || character == '_' || character == '-';
}

static bool IsDotSegment(const char *start, size_t length)
{
    return (length == 1 && start[0] == '.')
        || (length == 2 && start[0] == '.' && start[1] == '.');
}

static bool IsReservedNamespace(const char *name, size_t length)
{
    static const char *const namespaces[] =
    {
        "gen3", "emerald", "firered", "engine", "mod",
    };
    size_t i;
    for (i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); i++)
    {
        if (strlen(namespaces[i]) == length
         && memcmp(name, namespaces[i], length) == 0)
            return true;
    }
    return false;
}

enum Gen3ResourceNameStatus Gen3ResourceId_ValidateCanonicalName(const char *name)
{
    const char *colon;
    const char *segment;
    const char *cursor;
    size_t length;

    if (name == NULL)
        return GEN3_RESOURCE_NAME_NULL;
    length = strlen(name);
    if (length == 0)
        return GEN3_RESOURCE_NAME_EMPTY;
    if (length > GEN3_RESOURCE_NAME_MAX)
        return GEN3_RESOURCE_NAME_TOO_LONG;
    colon = strchr(name, ':');
    if (colon == NULL || colon == name || colon[1] == '\0' || strchr(colon + 1, ':') != NULL)
        return GEN3_RESOURCE_NAME_INVALID_NAMESPACE;
    if (!IsReservedNamespace(name, (size_t)(colon - name)))
        return GEN3_RESOURCE_NAME_INVALID_NAMESPACE;

    for (cursor = name; cursor < colon; cursor++)
    {
        if (!IsSegmentCharacter(*cursor))
            return GEN3_RESOURCE_NAME_INVALID_CHARACTER;
    }

    segment = colon + 1;
    for (cursor = segment; ; cursor++)
    {
        if (*cursor == '/' || *cursor == '\0')
        {
            size_t segmentLength = (size_t)(cursor - segment);
            if (segmentLength == 0)
                return GEN3_RESOURCE_NAME_EMPTY_SEGMENT;
            if (IsDotSegment(segment, segmentLength))
                return GEN3_RESOURCE_NAME_DOT_SEGMENT;
            if (*cursor == '\0')
                break;
            segment = cursor + 1;
        }
        else if (!IsSegmentCharacter(*cursor))
        {
            return GEN3_RESOURCE_NAME_INVALID_CHARACTER;
        }
    }
    return GEN3_RESOURCE_NAME_VALID;
}

void Gen3ResourceId_DeriveKey(const char *canonicalName, Gen3ResourceKey *outKey)
{
    static const char domain[] = "gen3-resource-id-v1";
    struct Gen3Sha256Context context;

    if (outKey == NULL)
        return;
    memset(outKey, 0, sizeof(*outKey));
    if (canonicalName == NULL)
        return;
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, domain, sizeof(domain));
    Gen3Sha256_Update(&context, canonicalName, strlen(canonicalName));
    Gen3Sha256_Final(&context, outKey->bytes);
}

bool Gen3ResourceId_KeyEqual(const Gen3ResourceKey *left, const Gen3ResourceKey *right)
{
    return left != NULL && right != NULL
        && memcmp(left->bytes, right->bytes, GEN3_RESOURCE_KEY_SIZE) == 0;
}

void Gen3ResourceId_FormatKeyHex(const Gen3ResourceKey *key,
                                 char outHex[GEN3_RESOURCE_KEY_HEX_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    if (outHex == NULL)
        return;
    if (key == NULL)
    {
        outHex[0] = '\0';
        return;
    }
    for (i = 0; i < GEN3_RESOURCE_KEY_SIZE; i++)
    {
        outHex[i * 2u] = digits[key->bytes[i] >> 4];
        outHex[i * 2u + 1u] = digits[key->bytes[i] & 15u];
    }
    outHex[GEN3_RESOURCE_KEY_SIZE * 2u] = '\0';
}

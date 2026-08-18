#include "emerald/resources/emerald_resource_ranges.h"

#include <string.h>

#include "gen3/resources/resource_id.h"

void EmeraldResourceRangeIndex_Init(struct EmeraldResourceRangeIndex *index)
{
    EmeraldResourceRangeIndex_Reset(index);
}

void EmeraldResourceRangeIndex_Reset(struct EmeraldResourceRangeIndex *index)
{
    if (index == NULL)
        return;
    memset(index, 0, sizeof(*index));
}

size_t EmeraldResourceRangeIndex_GetRangeCount(
    const struct EmeraldResourceRangeIndex *index)
{
    return index == NULL ? 0u : index->rangeCount;
}

bool EmeraldResourceRangeIndex_AddHull(
    struct EmeraldResourceRangeIndex *index, uintptr_t base, size_t length)
{
    if (index == NULL || length == 0 || base > UINTPTR_MAX - (length - 1))
        return false;
    if (index->hullCount >= EMERALD_RESOURCE_RANGE_INDEX_MAX_HULLS)
        return false;
    index->hulls[index->hullCount].base = base;
    index->hulls[index->hullCount].length = length;
    index->hullCount++;
    return true;
}

/* Insert `base`..`base+length` into the sorted, non-overlapping range table
 * with the key derived from `canonicalName`. Shared tail of RegisterStream
 * and RegisterSpan (R12-C: the audio seam's arena spans have no
 * EmeraldResourceCompatibilityImage, so it registers raw base/length spans
 * under their canonical names). */
static bool InsertRange(struct EmeraldResourceRangeIndex *index,
                        uintptr_t base, size_t length,
                        const char *canonicalName, uint32_t type,
                        uint32_t schema, enum EmeraldResourceRangeRole role)
{
    size_t position;

    if (index == NULL || role > EMERALD_RESOURCE_ROLE_COMPAT_OBJECT
     || canonicalName == NULL || length == 0
     || base > UINTPTR_MAX - (length - 1))
        return false;
    if (index->rangeCount >= EMERALD_RESOURCE_RANGE_INDEX_MAX_RANGES)
        return false;

    /* Insertion point: ranges stay sorted ascending by base. */
    position = index->rangeCount;
    while (position > 0 && index->ranges[position - 1].base > base)
        position--;
    /* Reject overlap with either neighbour (sorted order makes two checks
     * sufficient): [prev.base, prev.base+prev.length) must not contain
     * base, and [base, base+length) must not contain next.base. */
    if (position > 0)
    {
        const struct EmeraldResourceRange *prev = &index->ranges[position - 1];
        if (base < prev->base + prev->length)
            return false;
    }
    if (position < index->rangeCount)
    {
        const struct EmeraldResourceRange *next = &index->ranges[position];
        if (next->base < base + length)
            return false;
    }
    if (position < index->rangeCount)
        memmove(&index->ranges[position + 1], &index->ranges[position],
                (index->rangeCount - position) * sizeof(index->ranges[0]));
    index->ranges[position].base = base;
    index->ranges[position].length = length;
    index->ranges[position].type = type;
    index->ranges[position].schema = schema;
    index->ranges[position].role = (uint32_t)role;
    Gen3ResourceId_DeriveKey(canonicalName, &index->ranges[position].key);
    index->rangeCount++;
    return true;
}

bool EmeraldResourceRangeIndex_RegisterSpan(
    struct EmeraldResourceRangeIndex *index, uintptr_t base, size_t length,
    const char *canonicalName, uint32_t type, uint32_t schema,
    enum EmeraldResourceRangeRole role)
{
    return InsertRange(index, base, length, canonicalName, type, schema, role);
}

bool EmeraldResourceRangeIndex_RegisterStream(
    struct EmeraldResourceRangeIndex *index,
    const struct EmeraldResourceCompatibilityImage *image, size_t entryIndex,
    uint32_t schema, enum EmeraldResourceRangeRole role)
{
    const uint8_t *stream;
    size_t streamSize;
    uintptr_t base;

    if (index == NULL || image == NULL)
        return false;
    stream = EmeraldResourceCompatImage_GetStream(image, entryIndex);
    streamSize = EmeraldResourceCompatImage_GetStreamSize(image, entryIndex);
    if (stream == NULL || streamSize == 0)
        return false;
    base = (uintptr_t)stream;
    return InsertRange(index, base, streamSize,
                       EmeraldResourceCompatImage_GetEntryName(image,
                                                              entryIndex),
                       (uint32_t)EmeraldResourceCompatImage_GetEntryType(
                           image, entryIndex),
                       schema, role);
}

bool EmeraldResourceRangeIndex_Lookup(
    const struct EmeraldResourceRangeIndex *index, uintptr_t address,
    struct EmeraldResourceRangeHit *outHit)
{
    size_t low;
    size_t high;
    size_t hits = 0;

    if (index == NULL || outHit == NULL || index->rangeCount == 0)
        return false;
    /* Binary search for the last range whose base <= address. */
    low = 0;
    high = index->rangeCount;
    while (low < high)
    {
        size_t mid = low + (high - low) / 2;
        if (index->ranges[mid].base <= address)
            low = mid + 1;
        else
            high = mid;
    }
    if (low == 0)
        return false; /* address precedes every range */
    {
        const struct EmeraldResourceRange *range = &index->ranges[low - 1];
        if (address - range->base >= range->length)
            return false; /* after the candidate's end and before the next */
        hits = 1;
        outHit->key = range->key;
        outHit->type = range->type;
        outHit->schema = range->schema;
        outHit->role = range->role;
        outHit->rangeOffset = (size_t)(address - range->base);
    }
    /* Sorted non-overlapping ranges admit at most one hit; this check exists
     * to catch an index that was corrupted after registration. */
    if (low >= 2 && address - index->ranges[low - 2].base
                    < index->ranges[low - 2].length)
        hits++;
    (void)hits;
    return true;
}

bool EmeraldResourceRangeIndex_InHull(
    const struct EmeraldResourceRangeIndex *index, uintptr_t address)
{
    size_t i;

    if (index == NULL)
        return false;
    for (i = 0; i < index->hullCount; i++)
    {
        uintptr_t base = index->hulls[i].base;
        if (address >= base && address - base < index->hulls[i].length)
            return true;
    }
    return false;
}

bool EmeraldResourceRangeIndex_ResolveByKey(
    const struct EmeraldResourceRangeIndex *index, const Gen3ResourceKey *key,
    uint32_t type, uint32_t schema, uint32_t role, size_t rangeOffset,
    uintptr_t *outPointer)
{
    size_t i;

    if (index == NULL || key == NULL || outPointer == NULL)
        return false;
    for (i = 0; i < index->rangeCount; i++)
    {
        const struct EmeraldResourceRange *range = &index->ranges[i];
        if (!Gen3ResourceId_KeyEqual(&range->key, key))
            continue;
        if (range->type != type || range->schema != schema
         || range->role != role)
            return false; /* identity mismatch: refuse, never guess */
        if (rangeOffset >= range->length
         || range->base > UINTPTR_MAX - rangeOffset)
            return false;
        *outPointer = range->base + rangeOffset;
        return true;
    }
    return false; /* unknown key */
}

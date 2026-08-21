/* R13-F: Emerald map-metadata publication seam. See
 * include/emerald/resources/emerald_map_compat.h for the contract.
 *
 * Validates the four R13-F map families (header 41 / layout-meta 42 / event
 * bundles 43 / connections 44) through the NORMAL M0/M1 snapshot, then
 * publishes the native `gMapHeaders[518]` (48-byte rows, layout/events/
 * connections pointers rebuilt) plus seam-owned native event and connection
 * arenas, and registers the COMPAT_OBJECT ranges for the State-v5 walker.
 *
 * Script boundary (R13-G contract): the native header mapScripts, coord/bg
 * script pointers are resolved via HostResolveGbaAddr; object scripts are
 * preserved byte-for-byte as the native GbaAddr field.
 *
 * Transactional: phase 1 validates everything BEFORE any write; phase 2
 * precomputes the native rows/arenas (infallible once phase 1 passed); phase 3
 * publishes atomically (pure stores) + registers the ranges.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* System headers must precede the seam header, whose map_data_native.h pulls
 * global.h (which macro-redefines abs/stdlib symbols). */
#include "emerald/resources/emerald_map_compat.h"
#include "emerald/resources/map_data_native.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_script_compat.h"
#include "platform/host_memory.h"

/* Weak forward decl (strong definition in emerald_trainer_native_compat.c);
 * offline links bind to a stub, exactly as the other range-registering seams
 * (emerald_encounter_compat.c etc.). */
struct EmeraldResourceRangeIndex *EmeraldResourceCompat_GetRangeIndex(void);

#define EMERALD_MAP_GBA_ROM_BASE ((uint32_t)0x08000000u)

#define MAP_OBJ_WIRE 24u
#define MAP_WARP_WIRE 8u
#define MAP_COORD_WIRE 16u
#define MAP_BG_WIRE 12u
#define MAP_EVENTS_WIRE 20u
#define MAP_CONN_WIRE 12u
#define MAP_CONNS_WIRE 8u

#define MAP_MAX_REG_RANGES 8u

#if defined(PORTABLE)
extern const GbaAddr gMapLayouts[];
#endif

static uint16_t ReadLe16(const uint8_t *d) { return (uint16_t)(d[0] | ((uint16_t)d[1] << 8)); }
static int32_t ReadLe32(const uint8_t *d)
{
    return (int32_t)((uint32_t)d[0] | ((uint32_t)d[1] << 8)
        | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24));
}

/* ---- session/publish state ---- */
static HOST_DATA struct MapHeader sPublishedHeaders[EMERALD_MAP_HEADER_COUNT];
static uint8_t *sEventArena;
static size_t sEventArenaBytes;

/* R13-G5 (plan sec 10/11): per-map event-bundle layout bookkeeping so
 * the script rebind can walk the published arena rows exactly as the
 * publish phase laid them out. */
struct EmeraldMapEventBundle
{
    uint32_t arenaOffset;
    uint16_t objectCount;
    uint16_t coordCount;
    uint16_t bgCount;
};
static struct EmeraldMapEventBundle sEventBundles[EMERALD_MAP_EVENT_COUNT];
/* R13-G5: per-header mapScripts GBA provenance (retained for the rebind). */
static uint32_t sHeaderScriptGba[EMERALD_MAP_HEADER_COUNT];
static uint8_t *sConnArena;
static size_t sConnArenaBytes;

/* Native MapConnections contains a host pointer and therefore each packed
 * arena block must begin at pointer alignment.  The wire rows remain exact
 * 12-byte records; only zero padding between native blocks is added. */
static size_t NativeConnectionBlockSize(uint32_t count)
{
    size_t size = 16u + (size_t)count * MAP_CONN_WIRE;
    size_t alignment = sizeof(uintptr_t);
    return (size + alignment - 1u) & ~(alignment - 1u);
}
static struct { uintptr_t base; size_t length; } sRegisteredRanges[
    MAP_MAX_REG_RANGES];
static size_t sRegisteredRangeCount;
static size_t sPublishedCount;

static void ClearDiagnostics(struct EmeraldMapCompatDiagnostics *d)
{
    if (d != NULL)
        memset(d, 0, sizeof(*d));
}

static void NoteFailure(struct EmeraldMapCompatDiagnostics *d,
                        const char *stage, const char *canonicalName,
                        enum Gen3ResourceType expectedType,
                        enum Gen3ResourceType actualType,
                        uint32_t expectedSchema, uint32_t actualSchema,
                        uint32_t expectedSize, uint32_t actualSize,
                        const struct Gen3ResourceView *view)
{
    if (d == NULL)
        return;
    ClearDiagnostics(d);
    if (stage != NULL)
        snprintf(d->stage, sizeof(d->stage), "%s", stage);
    if (canonicalName != NULL)
        snprintf(d->canonicalName, sizeof(d->canonicalName), "%s",
                 canonicalName);
    snprintf(d->expectedType, sizeof(d->expectedType), "%s",
             Gen3ResourceType_Name(expectedType));
    if (actualType != GEN3_RESOURCE_TYPE_INVALID)
        snprintf(d->actualType, sizeof(d->actualType), "%s",
                 Gen3ResourceType_Name(actualType));
    d->expectedSchema = expectedSchema;
    d->actualSchema = actualSchema;
    d->expectedSize = expectedSize;
    d->actualSize = actualSize;
    if (view != NULL && view->winningProviderId != NULL)
    {
        snprintf(d->winningProviderId, sizeof(d->winningProviderId), "%s",
                 view->winningProviderId);
        if (view->winningProviderVersion != NULL)
            snprintf(d->winningProviderVersion,
                     sizeof(d->winningProviderVersion), "%s",
                     view->winningProviderVersion);
        d->winningProviderPrecedence = view->winningProviderPrecedence;
    }
}

static bool ResolveSessionView(const struct Gen3ResourceSnapshot *snapshot,
                               const char *id, uint32_t schema,
                               struct Gen3ResourceView *view)
{
    Gen3ResourceHandle handle;
    if (Gen3ResourceSnapshot_FindHandle(snapshot, id, &handle) != GEN3_RESOURCE_OK)
        return false;
    if (Gen3ResourceSnapshot_Resolve(snapshot, handle,
                                     GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                     schema, view) != GEN3_RESOURCE_OK
        || view->payload == NULL)
        return false;
    return true;
}

/* Layout blockdata/border bindings name individual R11 resources. Tileset
 * bindings name the R11 tileset family root; there is deliberately no
 * aggregate resource at that root (the family consists of tiles, palettes,
 * metatiles, and attributes). The strict R11 tileset publisher has already
 * resolved the complete family before R13-F runs, so the canonical /tiles
 * member is the pack identity anchor for the family root. */
static bool PackHasLayoutBinding(const struct Gen3ResourcePack *pack,
                                 const char *binding, bool tilesetRoot)
{
    char resourceId[GEN3_RESOURCE_NAME_MAX + 1u];
    int length;

    if (!tilesetRoot)
        return Gen3ResourcePack_FindByCanonicalName(pack, binding) != NULL;
    length = snprintf(resourceId, sizeof(resourceId), "%s/tiles", binding);
    return length >= 0 && (size_t)length < sizeof(resourceId)
        && Gen3ResourcePack_FindByCanonicalName(pack, resourceId) != NULL;
}

/* Validate a single structured resource: resolve schema/type, ROM_BASE
 * winner, exact pack-side size + byte parity. Returns the pack payload. */
static const uint8_t *ResolveMapResource(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    const char *id, uint32_t schema, size_t expectedSize,
    struct EmeraldMapCompatDiagnostics *d,
    enum EmeraldMapCompatStatus *resultOut)
{
    struct Gen3ResourceView view;
    const struct Gen3ResourcePackEntry *entry;

    if (!ResolveSessionView(snapshot, id, schema, &view))
    {
        NoteFailure(d, "resolve", id, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, schema, 0u, 0u, 0u, NULL);
        *resultOut = EMERALD_MAP_ERR_RESOLVE_FAILED;
        return NULL;
    }
    if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
    {
        NoteFailure(d, "resolve", id, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, schema, schema,
                    (uint32_t)view.payloadSize, (uint32_t)view.payloadSize,
                    &view);
        *resultOut = EMERALD_MAP_ERR_UNEXPECTED_OWNERSHIP;
        return NULL;
    }
    entry = Gen3ResourcePack_FindByCanonicalName(pack, id);
    if (entry == NULL || entry->payload == NULL
     || entry->payloadSize != expectedSize
     || view.payloadSize != entry->payloadSize
     || memcmp(view.payload, entry->payload, entry->payloadSize) != 0)
    {
        NoteFailure(d, "build", id, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, schema, schema,
                    (uint32_t)expectedSize,
                    (uint32_t)(entry ? entry->payloadSize : 0u), NULL);
        *resultOut = EMERALD_MAP_ERR_PAYLOAD_SIZE_MISMATCH;
        return NULL;
    }
    return entry->payload;
}

/* ---- State-v5 range registration (headers / events arena / conns arena) */
static bool RegisterMapRanges(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();
    bool allRanges = true;
    sRegisteredRangeCount = 0u;
    if (index == NULL)
        return true;
    /* gMapHeaders span (each serialized EWRAM gMapHeader pointer edge into
     * this array must reconcile for the State-v5 walker). */
    if (!EmeraldResourceRangeIndex_RegisterSpan(
            index, (uintptr_t)sPublishedHeaders, sizeof(sPublishedHeaders),
            "emerald:data/map/headers", GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            EMERALD_MAP_SCHEMA_HEADER, EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
        allRanges = false;
    else
    {
        sRegisteredRanges[sRegisteredRangeCount].base = (uintptr_t)sPublishedHeaders;
        sRegisteredRanges[sRegisteredRangeCount].length = sizeof(sPublishedHeaders);
        sRegisteredRangeCount++;
    }
    if (sEventArena != NULL && !EmeraldResourceRangeIndex_RegisterSpan(
            index, (uintptr_t)sEventArena, sEventArenaBytes,
            "emerald:data/arena/map-events", GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            EMERALD_MAP_SCHEMA_EVENTS, EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
        allRanges = false;
    else if (sEventArena != NULL)
    {
        sRegisteredRanges[sRegisteredRangeCount].base = (uintptr_t)sEventArena;
        sRegisteredRanges[sRegisteredRangeCount].length = sEventArenaBytes;
        sRegisteredRangeCount++;
    }
    if (sConnArena != NULL && !EmeraldResourceRangeIndex_RegisterSpan(
            index, (uintptr_t)sConnArena, sConnArenaBytes,
            "emerald:data/arena/map-connections", GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            EMERALD_MAP_SCHEMA_CONNECTIONS, EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
        allRanges = false;
    else if (sConnArena != NULL)
    {
        sRegisteredRanges[sRegisteredRangeCount].base = (uintptr_t)sConnArena;
        sRegisteredRanges[sRegisteredRangeCount].length = sConnArenaBytes;
        sRegisteredRangeCount++;
    }
    return allRanges;
}

static void UnregisterMapRanges(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();
    size_t r;
    if (index == NULL)
        return;
    for (r = 0u; r < sRegisteredRangeCount; r++)
    {
        uintptr_t base = sRegisteredRanges[r].base;
        size_t j;
        for (j = 0u; j < index->rangeCount; j++)
        {
            if (index->ranges[j].base == base)
            {
                memmove(&index->ranges[j], &index->ranges[j + 1u],
                        (index->rangeCount - j - 1u) * sizeof(index->ranges[0]));
                index->rangeCount--;
                break;
            }
        }
    }
    sRegisteredRangeCount = 0u;
}

enum EmeraldMapCompatStatus
EmeraldMapCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldMapCompatDiagnostics *diagnostics)
{
    enum EmeraldMapCompatStatus result = EMERALD_MAP_OK;
    /* phase-1 cached resource payloads (index order). */
    const uint8_t *hdrWire[EMERALD_MAP_HEADER_COUNT];
    const uint8_t *layWire[EMERALD_MAP_LAYOUT_COUNT];
    const uint8_t *evWire[EMERALD_MAP_EVENT_COUNT];
    const uint8_t *conWire[EMERALD_MAP_CONNECTION_COUNT];
    /* precomputed native pointers, per header routing index. */
    const struct MapLayout *layoutByHeader[EMERALD_MAP_HEADER_COUNT];
    const struct MapEvents *eventsByHeader[EMERALD_MAP_HEADER_COUNT];
    const struct MapConnections *connsByHeader[EMERALD_MAP_HEADER_COUNT];
    /* arena-write cursors. */
    uint8_t *evCur;
    uint8_t *conCur;
    size_t evBytes = 0u;
    size_t conBytes = 0u;
    size_t packCount;
    size_t i;
    uint32_t s41 = 0u, s42 = 0u, s43 = 0u, s44 = 0u;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || pack == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_MAP_SCHEMA_HEADER,
                    0u, 0u, 0u, NULL);
        return EMERALD_MAP_ERR_INVALID_ARGUMENT;
    }
    memset(hdrWire, 0, sizeof(hdrWire));
    memset(layWire, 0, sizeof(layWire));
    memset(evWire, 0, sizeof(evWire));
    memset(conWire, 0, sizeof(conWire));
    memset(layoutByHeader, 0, sizeof(layoutByHeader));
    memset(eventsByHeader, 0, sizeof(eventsByHeader));
    memset(connsByHeader, 0, sizeof(connsByHeader));
    packCount = Gen3ResourcePack_GetEntryCount(pack);

    /* ---- Phase 1a: headers. ---- */
    for (i = 0u; i < EMERALD_MAP_HEADER_COUNT; i++)
    {
        const char *name = kMapHeaderKeys[i];
        const uint8_t *wire = ResolveMapResource(
            snapshot, pack, name, EMERALD_MAP_SCHEMA_HEADER,
            28u, diagnostics, &result);
        if (wire == NULL)
            goto done;
        hdrWire[i] = wire;
        /* The header's layout / events / connections keys must exist. */
        if (kMapLayoutKeyByHeader[i][0] == '\0')
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_MAP_SCHEMA_HEADER, EMERALD_MAP_SCHEMA_HEADER,
                        0u, 0u, NULL);
            result = EMERALD_MAP_ERR_LAYOUT_BINDING_MISSING;
            goto done;
        }
        if (Gen3ResourcePack_FindByCanonicalName(pack,
                                                 kMapLayoutKeyByHeader[i]) == NULL)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_MAP_SCHEMA_HEADER, EMERALD_MAP_SCHEMA_LAYOUT,
                        0u, 0u, NULL);
            result = EMERALD_MAP_ERR_LAYOUT_BINDING_MISSING;
            goto done;
        }
        /* Each event/connection key declared must exist (or be empty). */
        if (kMapEventsKeyByHeader[i][0] != '\0'
                && Gen3ResourcePack_FindByCanonicalName(pack,
                                                        kMapEventsKeyByHeader[i]) == NULL)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_MAP_SCHEMA_HEADER, EMERALD_MAP_SCHEMA_EVENTS,
                        0u, 0u, NULL);
            result = EMERALD_MAP_ERR_MISSING_RESOURCE;
            goto done;
        }
        /* Script address must be a valid GBA logical address (R13-G bridge). */
        if ((uint32_t)ReadLe32(wire + 8u) < EMERALD_MAP_GBA_ROM_BASE)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_MAP_SCHEMA_HEADER, EMERALD_MAP_SCHEMA_HEADER,
                        0u, 0u, NULL);
            result = EMERALD_MAP_ERR_SCRIPT_ADDR_INVALID;
            goto done;
        }
    }

    /* ---- Phase 1b: map key bijection (group,num) uniqueness. ---- */
    for (i = 0u; i < EMERALD_MAP_HEADER_COUNT; i++)
    {
        size_t j;
        for (j = 0u; j < i; j++)
        {
            if (kMapHeaderGroups[j] == kMapHeaderGroups[i]
             && kMapHeaderNums[j] == kMapHeaderNums[i])
            {
                NoteFailure(diagnostics, "build", kMapHeaderKeys[i],
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_MAP_SCHEMA_HEADER, EMERALD_MAP_SCHEMA_HEADER,
                            0u, 0u, NULL);
                result = EMERALD_MAP_ERR_MAP_KEY_BAD_BIJECTION;
                goto done;
            }
        }
    }

    /* ---- Phase 1c: layout-meta rows + R11 bindings. ---- */
    for (i = 0u; i < EMERALD_MAP_LAYOUT_COUNT; i++)
    {
        const char *name = kMapLayoutKeys[i];
        const char *const *keys[4];
        size_t b;
        const uint8_t *wire = ResolveMapResource(
            snapshot, pack, name, EMERALD_MAP_SCHEMA_LAYOUT,
            24u, diagnostics, &result);
        if (wire == NULL)
            goto done;
        layWire[i] = wire;
        (void)wire;
        /* R11 pointer-target bindings must resolve to existing resources. */
        keys[0] = &kMapLayoutBlockdataKey[i];
        keys[1] = &kMapLayoutBorderKey[i];
        keys[2] = &kMapLayoutPrimaryKey[i];
        keys[3] = &kMapLayoutSecondaryKey[i];
        for (b = 0u; b < 4u; b++)
        {
            const char *k = *(keys[b]);
            if (k[0] == '\0')
                continue;
            if (!PackHasLayoutBinding(pack, k, b >= 2u))
            {
                NoteFailure(diagnostics, "build", name,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_MAP_SCHEMA_LAYOUT,
                            EMERALD_MAP_SCHEMA_LAYOUT, 0u, 0u, NULL);
                result = EMERALD_MAP_ERR_LAYOUT_BINDING_MISSING;
                goto done;
            }
        }
    }

    /* ---- Phase 1d: event bundles (exact size + script provenance). ---- */
    for (i = 0u; i < EMERALD_MAP_EVENT_COUNT; i++)
    {
        const struct EmeraldMapEventRecord *r = &kMapEvents[i];
        size_t objB = (size_t)r->objectCount * MAP_OBJ_WIRE;
        size_t warpB = (size_t)r->warpCount * MAP_WARP_WIRE;
        size_t coordB = (size_t)r->coordCount * MAP_COORD_WIRE;
        size_t bgB = (size_t)r->bgCount * MAP_BG_WIRE;
        size_t exp = MAP_EVENTS_WIRE + objB + warpB + coordB + bgB;
        size_t o;
        const uint8_t *wire = ResolveMapResource(
            snapshot, pack, r->name, EMERALD_MAP_SCHEMA_EVENTS, exp,
            diagnostics, &result);
        if (wire == NULL)
        {
            result = EMERALD_MAP_ERR_BUNDLE_SIZE_MISMATCH;
            goto done;
        }
        evWire[i] = wire;
        evBytes += (size_t)40u + (size_t)r->objectCount * 24u
                 + (size_t)r->warpCount * 8u + (size_t)r->coordCount * 24u
                 + (size_t)r->bgCount * 16u;
        /* object script provenance. */
        for (o = 0u; o < (size_t)r->objectCount; o++)
        {
            uint32_t a = ReadLe32(wire + o * MAP_OBJ_WIRE + 0x10u);
            uint32_t ex = kMapEventObjectScriptAddrs[
                kMapEventObjectScriptStart[i] + o];
            if (a != ex)
            {
                NoteFailure(diagnostics, "build", r->name,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_MAP_SCHEMA_EVENTS, EMERALD_MAP_SCHEMA_EVENTS,
                            0u, 0u, NULL);
                result = EMERALD_MAP_ERR_PARITY_MISMATCH;
                goto done;
            }
        }
        /* coord script provenance. */
        for (o = 0u; o < (size_t)r->coordCount; o++)
        {
            uint32_t a = ReadLe32(wire + objB + warpB + o * MAP_COORD_WIRE + 12u);
            uint32_t ex = kMapEventCoordScriptAddrs[
                kMapEventCoordScriptStart[i] + o];
            if (a != ex)
            {
                NoteFailure(diagnostics, "build", r->name,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_MAP_SCHEMA_EVENTS, EMERALD_MAP_SCHEMA_EVENTS,
                            0u, 0u, NULL);
                result = EMERALD_MAP_ERR_PARITY_MISMATCH;
                goto done;
            }
        }
        /* bg signal-script provenance. */
        for (o = 0u; o < (size_t)r->bgCount; o++)
        {
            uint8_t kind = wire[objB + warpB + coordB + o * MAP_BG_WIRE + 5u];
            uint32_t ex = kMapEventBgScriptAddrs[
                kMapEventBgScriptStart[i] + o];
            if (kind <= 4u)
            {
                uint32_t a = ReadLe32(wire + objB + warpB + coordB
                                      + o * MAP_BG_WIRE + 8u);
                if (a != ex)
                {
                    NoteFailure(diagnostics, "build", r->name,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_MAP_SCHEMA_EVENTS,
                                EMERALD_MAP_SCHEMA_EVENTS, 0u, 0u, NULL);
                    result = EMERALD_MAP_ERR_PARITY_MISMATCH;
                    goto done;
                }
            }
            else if (ex != 0u)
            {
                NoteFailure(diagnostics, "build", r->name,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_MAP_SCHEMA_EVENTS, EMERALD_MAP_SCHEMA_EVENTS,
                            0u, 0u, NULL);
                result = EMERALD_MAP_ERR_PARITY_MISMATCH;
                goto done;
            }
        }
    }

    /* ---- Phase 1e: connections (size + target maps exist). ---- */
    for (i = 0u; i < EMERALD_MAP_CONNECTION_COUNT; i++)
    {
        const struct EmeraldMapConnectionRecord *r = &kMapConnections[i];
        size_t exp = MAP_CONNS_WIRE + (size_t)r->count * MAP_CONN_WIRE;
        int32_t j;
        const uint8_t *wire = ResolveMapResource(
            snapshot, pack, r->name, EMERALD_MAP_SCHEMA_CONNECTIONS, exp,
            diagnostics, &result);
        if (wire == NULL)
        {
            result = EMERALD_MAP_ERR_BUNDLE_SIZE_MISMATCH;
            goto done;
        }
        conWire[i] = wire;
        conBytes += NativeConnectionBlockSize(r->count);
        /* the block's own count must match the metadata. */
        if ((int32_t)ReadLe32(wire + (size_t)r->count * MAP_CONN_WIRE)
                != r->count)
        {
            NoteFailure(diagnostics, "build", r->name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_MAP_SCHEMA_CONNECTIONS,
                        EMERALD_MAP_SCHEMA_CONNECTIONS, 0u, 0u, NULL);
            result = EMERALD_MAP_ERR_BUNDLE_SIZE_MISMATCH;
            goto done;
        }
        /* every connection target (mapGroup,mapNum) must be a real map. */
        for (j = 0; j < r->count; j++)
        {
            uint8_t g = wire[(size_t)j * MAP_CONN_WIRE + 8u];
            uint8_t m = wire[(size_t)j * MAP_CONN_WIRE + 9u];
            size_t t;
            bool found = false;
            for (t = 0u; t < EMERALD_MAP_HEADER_COUNT; t++)
            {
                if (kMapHeaderGroups[t] == g && kMapHeaderNums[t] == m)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                NoteFailure(diagnostics, "build", r->name,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_MAP_SCHEMA_CONNECTIONS,
                            EMERALD_MAP_SCHEMA_CONNECTIONS, 0u, 0u, NULL);
                result = EMERALD_MAP_ERR_TARGET_MAP_MISSING;
                goto done;
            }
        }
    }

    /* ---- Phase 1f: pack-level schema set equality. ---- */
    for (i = 0u; i < packCount; i++)
    {
        const struct Gen3ResourcePackEntry *pe = Gen3ResourcePack_GetEntry(pack, i);
        if (pe == NULL || pe->type != GEN3_RESOURCE_TYPE_STRUCTURED_DATA)
            continue;
        if (pe->schema == EMERALD_MAP_SCHEMA_HEADER) s41++;
        else if (pe->schema == EMERALD_MAP_SCHEMA_LAYOUT) s42++;
        else if (pe->schema == EMERALD_MAP_SCHEMA_EVENTS) s43++;
        else if (pe->schema == EMERALD_MAP_SCHEMA_CONNECTIONS) s44++;
    }
    if (s41 != EMERALD_MAP_HEADER_COUNT || s42 != EMERALD_MAP_LAYOUT_COUNT
     || s43 != EMERALD_MAP_EVENT_COUNT || s44 != EMERALD_MAP_CONNECTION_COUNT)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_MAP_SCHEMA_HEADER,
                    EMERALD_MAP_SCHEMA_HEADER, 0u, s41, NULL);
        result = EMERALD_MAP_ERR_UNEXPECTED_COUNT;
        goto done;
    }

    /* ---- Phase 2a: allocate arenas (infallible once phase 1 passed). ---- */
    evBytes = evBytes ? evBytes : 1u;
    conBytes = conBytes ? conBytes : 1u;
    sEventArena = (uint8_t *)calloc(1, evBytes);
    sConnArena = (uint8_t *)calloc(1, conBytes);
    if (sEventArena == NULL || sConnArena == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_MAP_SCHEMA_EVENTS,
                    0u, 0u, 0u, NULL);
        result = EMERALD_MAP_ERR_OUT_OF_MEMORY;
        goto done;
    }

    /* ---- Phase 2b: build native event bundles + arena. ---- */
    evCur = sEventArena;
    for (i = 0u; i < EMERALD_MAP_EVENT_COUNT; i++)
    {
        const struct EmeraldMapEventRecord *r = &kMapEvents[i];
        const uint8_t *w = evWire[i];
        sEventBundles[i].arenaOffset = (uint32_t)(evCur - sEventArena);
        sEventBundles[i].objectCount = (uint16_t)r->objectCount;
        sEventBundles[i].coordCount = (uint16_t)r->coordCount;
        sEventBundles[i].bgCount = (uint16_t)r->bgCount;
        size_t objB = (size_t)r->objectCount * MAP_OBJ_WIRE;
        size_t warpB = (size_t)r->warpCount * MAP_WARP_WIRE;
        size_t coordB = (size_t)r->coordCount * MAP_COORD_WIRE;
        /* native row layout in-arena: [then object][warp][coord][bg][MapEvents]
         * All offsets must be 8-aligned for the 64-bit pointers. */
        uint8_t *pObjects = evCur;
        uint8_t *pWarps = pObjects + (size_t)r->objectCount * 24u;
        uint8_t *pCoords = pWarps + (size_t)r->warpCount * 8u;
        uint8_t *pBgs = pCoords + (size_t)r->coordCount * 24u;
        struct MapEvents *pMe = (struct MapEvents *)(pBgs
            + (size_t)r->bgCount * 16u);
        size_t o;
        /* Every per-type row byte-size (24/8/16) and the MapEvents native
         * size (40) are multiples of 8, so with the arena 8-aligned every
         * bundle + every 64-bit-pointer-bearing structure stays 8-aligned:
         * no explicit padding is required between rows. */
        /* object rows: byte-identical 24 B (script is the native GbaAddr u32). */
        if (r->objectCount)
            memcpy(pObjects, w, objB);
        /* warp rows: byte-identical 8 B. */
        if (r->warpCount)
            memcpy(pWarps, w + objB, warpB);
        /* coord rows: 16 -> 24 native with script via HostResolveGbaAddr. */
        for (o = 0u; o < (size_t)r->coordCount; o++)
        {
            const uint8_t *src = w + objB + warpB + o * MAP_COORD_WIRE;
            struct CoordEvent *dst = (struct CoordEvent *)(pCoords + o * 24u);
            dst->x = (s16)ReadLe16(src + 0u);
            dst->y = (s16)ReadLe16(src + 2u);
            dst->elevation = src[4u];
            dst->trigger = ReadLe16(src + 6u);
            dst->index = ReadLe16(src + 8u);
            dst->script = (const u8 *)HostResolveGbaAddr(
                (uint32_t)ReadLe32(src + 12u));
        }
        /* bg rows: 12 -> 16 native; script for sign/player-facing kinds. */
        for (o = 0u; o < (size_t)r->bgCount; o++)
        {
            const uint8_t *src = w + objB + warpB + coordB + o * MAP_BG_WIRE;
            struct BgEvent *dst = (struct BgEvent *)(pBgs + o * 16u);
            dst->x = ReadLe16(src + 0u);
            dst->y = ReadLe16(src + 2u);
            dst->elevation = src[4u];
            dst->kind = src[5u];
            if (dst->kind <= 4u)
                dst->bgUnion.script = (const u8 *)HostResolveGbaAddr(
                    (uint32_t)ReadLe32(src + 8u));
            else
                dst->bgUnion.secretBaseId = (uint32_t)ReadLe32(src + 8u);
        }
        /* native MapEvents. */
        pMe->objectEventCount = r->objectCount;
        pMe->warpCount = r->warpCount;
        pMe->coordEventCount = r->coordCount;
        pMe->bgEventCount = r->bgCount;
        pMe->objectEvents = (const struct ObjectEventTemplate *)pObjects;
        pMe->warps = (const struct WarpEvent *)pWarps;
        pMe->coordEvents = (const struct CoordEvent *)pCoords;
        pMe->bgEvents = (const struct BgEvent *)pBgs;
        evCur = (uint8_t *)(pMe + 1);
    }
    evBytes = (size_t)(evCur - sEventArena);
    sEventArenaBytes = evBytes;

    /* ---- Phase 2c: build native connection arenas. ---- */
    conCur = sConnArena;
    for (i = 0u; i < EMERALD_MAP_CONNECTION_COUNT; i++)
    {
        const struct EmeraldMapConnectionRecord *r = &kMapConnections[i];
        const uint8_t *w = conWire[i];
        struct MapConnections *pm = (struct MapConnections *)conCur;
        struct MapConnection *rows =
            (struct MapConnection *)(conCur + 16u);
        if (r->count)
            memcpy(rows, w, (size_t)r->count * MAP_CONN_WIRE);
        pm->count = r->count;
        pm->connections = (const struct MapConnection *)rows;
        conCur += NativeConnectionBlockSize(r->count);
    }
    sConnArenaBytes = (size_t)(conCur - sConnArena);

    /* ---- Phase 2d: precompute each header's native layout pointer. ---- */
    for (i = 0u; i < EMERALD_MAP_HEADER_COUNT; i++)
    {
        uint32_t layoutId = ReadLe16(hdrWire[i] + 18u);
        const void *lay = layoutId ? (const void *)HostResolveGbaTableEntry(
            gMapLayouts, layoutId - 1u) : NULL;
        layoutByHeader[i] = (const struct MapLayout *)lay;
    }

    /* ---- Phase 3: FINAL header pointer assembly (staged from arenas). ---- */
    /* eventsByHeader / connsByHeader are filled from the arena by walking the
     * published native bundles in the same order we built them. */
    {
        uint8_t *ec = sEventArena;
        size_t e;
        for (e = 0u; e < EMERALD_MAP_EVENT_COUNT; e++)
        {
            const struct EmeraldMapEventRecord *r = &kMapEvents[e];
            size_t sz = 40u + (size_t)r->objectCount * 24u
                      + (size_t)r->warpCount * 8u
                      + (size_t)r->coordCount * 24u
                      + (size_t)r->bgCount * 16u;
            struct MapEvents *pm = (struct MapEvents *)(ec + sz - 40u);
            size_t h;
            for (h = 0u; h < EMERALD_MAP_HEADER_COUNT; h++)
                if (kMapEventsKeyByHeader[h][0] != '\0'
                 && strcmp(kMapEventsKeyByHeader[h], r->name) == 0)
                    eventsByHeader[h] = pm;
            ec += sz;
        }
    }
    {
        uint8_t *cc = sConnArena;
        size_t c;
        for (c = 0u; c < EMERALD_MAP_CONNECTION_COUNT; c++)
        {
            const struct EmeraldMapConnectionRecord *r = &kMapConnections[c];
            struct MapConnections *pm = (struct MapConnections *)cc;
            size_t h;
            for (h = 0u; h < EMERALD_MAP_HEADER_COUNT; h++)
                if (kMapConnectionsKeyByHeader[h][0] != '\0'
                 && strcmp(kMapConnectionsKeyByHeader[h], r->name) == 0)
                    connsByHeader[h] = pm;
            cc += NativeConnectionBlockSize(r->count);
        }
    }

    /* ---- Phase 3b: assemble + publish gMapHeaders atomically. ---- */
    for (i = 0u; i < EMERALD_MAP_HEADER_COUNT; i++)
    {
        const uint8_t *w = hdrWire[i];
        struct MapHeader *d = &sPublishedHeaders[i];
        memset(d, 0, sizeof(*d));
        d->mapLayout = layoutByHeader[i];
        d->events = eventsByHeader[i];
        sHeaderScriptGba[i] = (uint32_t)ReadLe32(w + 8u);
        d->mapScripts = (const u8 *)HostResolveGbaAddr(
            sHeaderScriptGba[i]);
        d->connections = connsByHeader[i];
        d->music = ReadLe16(w + 16u);
        d->mapLayoutId = ReadLe16(w + 18u);
        d->regionMapSectionId = w[20u];
        d->cave = w[21u];
        d->weather = w[22u];
        d->mapType = w[23u];
        d->battleType = w[27u];
        d->allowCycling = (w[26u] >> 0) & 1u;
        d->allowEscaping = (w[26u] >> 1) & 1u;
        d->allowRunning = (w[26u] >> 2) & 1u;
        d->showMapName = (w[26u] >> 3) & 0x1f;
    }
    memcpy(gMapHeaders, sPublishedHeaders, sizeof(sPublishedHeaders));
    if (!RegisterMapRanges())
    {
        NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
        result = EMERALD_MAP_ERR_RANGE_REGISTRATION;
        goto done;
    }
    sPublishedCount = EMERALD_MAP_HEADER_COUNT + EMERALD_MAP_LAYOUT_COUNT
                    + EMERALD_MAP_EVENT_COUNT + EMERALD_MAP_CONNECTION_COUNT;
    NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
    result = EMERALD_MAP_OK;

done:
    if (result != EMERALD_MAP_OK)
    {
        EmeraldMapCompat_ClearMigratedEntries();
    }
    else
    {
        /* keep the arenas live for the session. */
    }
    return result;
}

void EmeraldMapCompat_ClearMigratedEntries(void)
{
    size_t i;
    UnregisterMapRanges();
    for (i = 0u; i < EMERALD_MAP_HEADER_COUNT; i++)
    {
        memset(&gMapHeaders[i], 0, sizeof(struct MapHeader));
        memset(&sPublishedHeaders[i], 0, sizeof(struct MapHeader));
    }
    free(sEventArena);
    sEventArena = NULL;
    sEventArenaBytes = 0u;
    free(sConnArena);
    sConnArena = NULL;
    sConnArenaBytes = 0u;
    sPublishedCount = 0u;
}

void EmeraldMapCompat_Shutdown(void)
{
    EmeraldMapCompat_ClearMigratedEntries();
}

size_t EmeraldMapCompat_GetPublishedCount(void)
{
    return sPublishedCount;
}

size_t EmeraldMapCompat_GetEventArenaBytes(void)
{
    return sEventArenaBytes;
}

size_t EmeraldMapCompat_GetConnArenaBytes(void)
{
    return sConnArenaBytes;
}

/* R13-G5 (plan sec 10/11): the live R13-F script rebind. Walks the
 * published event arena exactly as the publish phase laid it out and
 * replaces every coord/bg script pointer (originally resolved through
 * HostResolveGbaAddr) and every header mapScripts pointer with the
 * live G generation pointer resolved from the validated GBA
 * provenance. Object rows keep their GbaAddr field (the accessors
 * resolve). Every row must resolve - a missing target refuses the
 * whole transaction (all-or-nothing). */
bool EmeraldMapCompat_RebindScripts(void)
{
    size_t i;

    if (sEventArena == NULL || sPublishedCount == 0u)
        return false;
    if (!EmeraldScriptCompat_IsPublished())
        return false;
    for (i = 0u; i < EMERALD_MAP_HEADER_COUNT; i++)
    {
        uintptr_t tableBase;
        size_t tableSize;
        if (!EmeraldScriptCompat_ResolveRoutingTable(
                sHeaderScriptGba[i], &tableBase, &tableSize))
        {
            return false;
        }
        sPublishedHeaders[i].mapScripts = (const u8 *)tableBase;
    }
    for (i = 0u; i < EMERALD_MAP_EVENT_COUNT; i++)
    {
        const struct EmeraldMapEventBundle *bundle = &sEventBundles[i];
        uint8_t *base = sEventArena + bundle->arenaOffset;
        uint8_t *pObjects = base;
        uint8_t *pWarps = pObjects + (size_t)bundle->objectCount * 24u;
        uint8_t *pCoords = pWarps + (size_t)kMapEvents[i].warpCount * 8u;
        uint8_t *pBgs = pCoords + (size_t)bundle->coordCount * 24u;
        size_t o;

        for (o = 0u; o < (size_t)bundle->coordCount; o++)
        {
            struct CoordEvent *dst = (struct CoordEvent *)(pCoords + o * 24u);
            uint32_t gba = kMapEventCoordScriptAddrs[
                kMapEventCoordScriptStart[i] + o];

            /* Weather-only coord rows carry no script (the engine's
             * TryRunCoordEventScript handles the null case); they stay
             * null through the rebind. */
            if (gba == 0u)
                continue;
            struct EmeraldScriptCompatResolvedTarget target;

            {
                uintptr_t address;
                if (!EmeraldScriptCompat_ResolveFEntrypoint(
                        EMERALD_SCRIPT_NATIVE_F_COORD_EVENT, gba, &address))
                {
                    return false;
                }
                dst->script = (const u8 *)address;
            }
        }
        for (o = 0u; o < (size_t)bundle->bgCount; o++)
        {
            struct BgEvent *dst = (struct BgEvent *)(pBgs + o * 16u);
            uint32_t gba;

            if (dst->kind > 4u)
                continue;
            gba = kMapEventBgScriptAddrs[kMapEventBgScriptStart[i] + o];
            if (gba == 0u)
                continue;
            {
                uintptr_t address;
                if (!EmeraldScriptCompat_ResolveFEntrypoint(
                        EMERALD_SCRIPT_NATIVE_F_BG_EVENT, gba, &address))
                {
                    return false;
                }
                dst->bgUnion.script = (const u8 *)address;
            }
        }
    }
    return true;
}

const char *EmeraldMapCompatStatus_Describe(
    enum EmeraldMapCompatStatus status)
{
    switch (status)
    {
    case EMERALD_MAP_OK: return "ok";
    case EMERALD_MAP_ERR_INVALID_ARGUMENT: return "invalid argument";
    case EMERALD_MAP_ERR_OUT_OF_MEMORY: return "out of memory";
    case EMERALD_MAP_ERR_RESOLVE_FAILED: return "resolve failed";
    case EMERALD_MAP_ERR_UNEXPECTED_OWNERSHIP: return "unexpected ownership";
    case EMERALD_MAP_ERR_PAYLOAD_SIZE_MISMATCH: return "payload size mismatch";
    case EMERALD_MAP_ERR_UNEXPECTED_COUNT: return "unexpected count";
    case EMERALD_MAP_ERR_MISSING_RESOURCE: return "missing resource";
    case EMERALD_MAP_ERR_UNEXPECTED_TYPE: return "unexpected type";
    case EMERALD_MAP_ERR_UNEXPECTED_SCHEMA: return "unexpected schema";
    case EMERALD_MAP_ERR_MAP_KEY_BAD_BIJECTION: return "bad map key bijection";
    case EMERALD_MAP_ERR_SCRIPT_ADDR_INVALID: return "invalid script address";
    case EMERALD_MAP_ERR_LAYOUT_BINDING_MISSING: return "missing layout binding";
    case EMERALD_MAP_ERR_LAYOUT_BINDING_MISMATCH: return "layout binding mismatch";
    case EMERALD_MAP_ERR_TARGET_MAP_MISSING: return "connection target missing";
    case EMERALD_MAP_ERR_BUNDLE_SIZE_MISMATCH: return "bundle size mismatch";
    case EMERALD_MAP_ERR_PARITY_MISMATCH: return "parity mismatch";
    case EMERALD_MAP_ERR_RANGE_REGISTRATION: return "range registration";
    case EMERALD_MAP_ERR_UNAVAILABLE: return "unavailable";
    }
    return "unknown";
}

#ifndef EMERALD_RESOURCES_EMERALD_MAP_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_MAP_COMPAT_H

/* R13-F: Emerald map-metadata publication seam.
 *
 * Publishes the R13-F map families (schema 41 header, 42 layout-meta, 43
 * event bundles, 44 connections) from the production pack into the native
 * HOST_DATA fill target (`struct MapHeader gMapHeaders[518]`) plus seam-owned
 * native event/connection arenas, through the NORMAL M0/M1 snapshot.
 *
 * REFUSE-CLASS: the compiled mapjson-generated map data is
 * NATIVE_LINUX-gated out of the link (headers/events/connections inc under
 * data/maps.s / data/map_events.s), so there is no compiled fallback - a
 * session whose map metadata cannot publish is refused and the loader rolls
 * the whole registration back. gMapGroups + the gMapLayouts index tables stay
 * compiled (INDEX_ROUTING) and resolve (HostResolveGbaAddr) to the published
 * tables; every map header's mapLayout is rebuilt to the R11-published native
 * layout record via the gMapLayouts index.
 *
 * Script boundary (R13-G contract): MapHeader.mapScripts,
 * ObjectEventTemplate.script, CoordEvent.script and BgEvent.script remain GBA
 * LOGICAL addresses. The native header's mapScripts and the native
 * coord/bg script pointers are resolved through the existing
 * HostResolveGbaAddr bridge; object scripts are preserved byte-for-byte as the
 * native GbaAddr field. movementType stays a u8 enum (no movement cutover).
 *
 * Transactional phases: 1 validates every header/layout-meta/event-bundle/
 * connection resource (schema + exact size + ROM_BASE winner + pack byte
 * parity + generated-set equality + the map key bijection + the script GBA
 * addresses via the provenance map + the R11 layout pointer-target bindings
 * (blockdata/border/tileset keys resolve to existing R11 resources) + every
 * connection target map exists) BEFORE any write; 2 precomputes the native
 * header/event/connection rows + arenas; 3 publishes atomically (pure stores)
 * and registers the COMPAT_OBJECT ranges. On any phase-1/2 failure nothing is
 * written and the diagnostics name the first failing resource.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/resource_types.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/map_native.generated.h"

#define EMERALD_MAP_SCHEMA_HEADER      41u
#define EMERALD_MAP_SCHEMA_LAYOUT      42u
#define EMERALD_MAP_SCHEMA_EVENTS      43u
#define EMERALD_MAP_SCHEMA_CONNECTIONS 44u

enum EmeraldMapCompatStatus
{
    EMERALD_MAP_OK = 0,
    EMERALD_MAP_ERR_INVALID_ARGUMENT,
    EMERALD_MAP_ERR_OUT_OF_MEMORY,
    EMERALD_MAP_ERR_RESOLVE_FAILED,
    EMERALD_MAP_ERR_UNEXPECTED_OWNERSHIP,
    EMERALD_MAP_ERR_PAYLOAD_SIZE_MISMATCH,
    EMERALD_MAP_ERR_UNEXPECTED_COUNT,
    EMERALD_MAP_ERR_MISSING_RESOURCE,
    EMERALD_MAP_ERR_UNEXPECTED_TYPE,
    EMERALD_MAP_ERR_UNEXPECTED_SCHEMA,
    EMERALD_MAP_ERR_MAP_KEY_BAD_BIJECTION,
    EMERALD_MAP_ERR_SCRIPT_ADDR_INVALID,
    EMERALD_MAP_ERR_LAYOUT_BINDING_MISSING,
    EMERALD_MAP_ERR_LAYOUT_BINDING_MISMATCH,
    EMERALD_MAP_ERR_TARGET_MAP_MISSING,
    EMERALD_MAP_ERR_BUNDLE_SIZE_MISMATCH,
    EMERALD_MAP_ERR_PARITY_MISMATCH,
    EMERALD_MAP_ERR_RANGE_REGISTRATION,
    EMERALD_MAP_ERR_UNAVAILABLE,
};

/* Structured diagnostics (same shape as the other compat seams). On failure
 * the FIRST failing entry is named. */
struct EmeraldMapCompatDiagnostics
{
    char canonicalName[96];
    char stage[24];              /* "build" / "publish" */
    char expectedType[24];
    char actualType[24];
    uint32_t expectedSchema;
    uint32_t actualSchema;
    uint32_t expectedSize;
    uint32_t actualSize;
    char winningProviderId[64];
    char winningProviderVersion[32];
    uint32_t winningProviderPrecedence;
};

/* Validate + publish the R13-F map families. Transactional; idempotent for a
 * new session. `pack` must be the pack the session was built from. */
enum EmeraldMapCompatStatus
EmeraldMapCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldMapCompatDiagnostics *diagnostics);

/* Fail-closed clear: NULL every published pointer in gMapHeaders, free the
 * event/connection arenas and unregister the ranges (idempotent). */
void EmeraldMapCompat_ClearMigratedEntries(void);
void EmeraldMapCompat_Shutdown(void);

/* Query helpers (tests + ranges walker). PublishedCount is the number of
 * published resources (518 + 441 + 507 + 64 = 1530 when OK). */
/* R13-G5 (plan sec 10): atomically rebind the published header/
 * coord/bg script pointers to the live G generation (all-or-nothing). */
bool EmeraldMapCompat_RebindScripts(void);

size_t EmeraldMapCompat_GetPublishedCount(void);
size_t EmeraldMapCompat_GetEventArenaBytes(void);
size_t EmeraldMapCompat_GetConnArenaBytes(void);
const char *EmeraldMapCompatStatus_Describe(
    enum EmeraldMapCompatStatus status);

#endif /* EMERALD_RESOURCES_EMERALD_MAP_COMPAT_H */

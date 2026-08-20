#ifndef EMERALD_RESOURCES_EMERALD_ENCOUNTER_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_ENCOUNTER_COMPAT_H

/* R13-E2: Emerald wild-encounter (gWildMonHeaders / map-based WildPokemonInfo /
 * slot tables) publication seam.
 *
 * Publishes the encounter families (structured-data schema 19 headers block +
 * schema 20 per-(map,type) slot tables) from the production pack into their
 * native HOST_DATA fill targets through the NORMAL M0/M1 snapshot:
 *   - `gWildMonHeaders[125]` (40-byte native rows) with the four info pointers
 *     rebuilt to the seam's 209 published native WildPokemonInfo objects;
 *   - `gWildEncounterInfos[209]` (16-byte native rows) with each slotPtr
 *     rebuilt into the seam's slot arena;
 *   - one slot arena (4-byte rows, byte-exact to the ROM slices).
 *
 * REFUSE-CLASS: the compiled const map-based definitions (src/data/
 * wild_encounters.h) are NATIVE_LINUX-guarded out of the link (the Pier/Pyramid
 * facilities stay compiled), so there is no compiled fallback: a session whose
 * wild-encounter data cannot publish is refused and the loader rolls the whole
 * registration back.
 *
 * Transactional phases mirror EmeraldTrainerCompat: 1 validates the headers
 * block (2,500 B, sentinel at row 124), every slot resource (schema 20, ROM_BASE
 * winner, M0/M1 resolution, pack byte equality, generated set equality), the
 * full header->info->slot pointer graph (every header info pointer matches its
 * generated info record; every info slot pointer resolves to the slot table's
 * ROM address), the Altering Cave 9-header ordering (rows 114-122 map (24,106)
 * with the altering-cave-1..9 keys) and the pack-level schema counts - BEFORE
 * any publication; 2 builds the slot arena + the 209 native info objects + the
 * 125 native header rows (all infallible once phase 1 passed); 3 publishes
 * atomically and registers ONE COMPAT_OBJECT range over the slot arena. On any
 * phase-1/2 failure nothing is written and the diagnostics name the first
 * failing resource.
 *
 * Platform-neutral: no engine globals beyond the fill-target headers are
 * touched; it drives the gen3 session core like the other compat seams.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/resource_types.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/encounter_native.generated.h"
#include "emerald/resources/encounter_data_native.h"

#define EMERALD_ENCOUNTER_SCHEMA_HEADERS 19u
#define EMERALD_ENCOUNTER_SCHEMA_SLOT    20u

#define EMERALD_ENCOUNTER_FIELD_LAND       0u
#define EMERALD_ENCOUNTER_FIELD_WATER      1u
#define EMERALD_ENCOUNTER_FIELD_ROCK_SMASH 2u
#define EMERALD_ENCOUNTER_FIELD_FISHING    3u
#define EMERALD_ENCOUNTER_FIELD_COUNT      4u

/* ROM geometry (GBA addresses +0x08000000); the sentinel header (MAP_UNDEFINED
 * = 0xFF,0xFF) terminates the consumer's linear scan. */
#define EMERALD_ENCOUNTER_HEADER_WIRE   20u
#define EMERALD_ENCOUNTER_SLOT_WIRE     4u
#define EMERALD_ENCOUNTER_SENTINEL_ROW  124u
#define EMERALD_ENCOUNTER_ALTERING_ROW0 114u
#define EMERALD_ENCOUNTER_ALTERING_MAP_GROUP 24u
#define EMERALD_ENCOUNTER_ALTERING_MAP_NUM   106u
#define EMERALD_GBA_ROM_BASE_COMPAT  ((uint32_t)0x08000000u)

enum EmeraldEncounterCompatStatus
{
    EMERALD_ENCOUNTER_OK = 0,
    EMERALD_ENCOUNTER_ERR_INVALID_ARGUMENT,     /* NULL snapshot/pack pointer */
    EMERALD_ENCOUNTER_ERR_OUT_OF_MEMORY,
    EMERALD_ENCOUNTER_ERR_RESOLVE_FAILED,       /* M0/M1 snapshot did not resolve */
    EMERALD_ENCOUNTER_ERR_UNEXPECTED_OWNERSHIP, /* winner is not ROM_BASE */
    EMERALD_ENCOUNTER_ERR_PAYLOAD_SIZE_MISMATCH, /* pack/view size differs */
    EMERALD_ENCOUNTER_ERR_UNEXPECTED_COUNT,     /* pack schema/slot counts != pins */
    EMERALD_ENCOUNTER_ERR_BAD_HEADERS_BLOCK,    /* headers size / sentinel bad */
    EMERALD_ENCOUNTER_ERR_HEADER_INFO_PTR,      /* header info ptr != generated record */
    EMERALD_ENCOUNTER_ERR_INFO_SLOT_PTR,        /* info slot ptr != slot ROM addr */
    EMERALD_ENCOUNTER_ERR_MISSING_SLOT,         /* a non-null header field has no slot resource */
    EMERALD_ENCOUNTER_ERR_NULL_FIELD_KEY,       /* a null header field unexpectedly has a key */
    EMERALD_ENCOUNTER_ERR_ALTERING_CAVE,        /* altering-cave rows (24,106) misordered/missing */
    EMERALD_ENCOUNTER_ERR_UNEXPECTED_TYPE,      /* entry type != STRUCTURED_DATA */
    EMERALD_ENCOUNTER_ERR_UNEXPECTED_SCHEMA,    /* entry schema != 19/20 */
    EMERALD_ENCOUNTER_ERR_RANGE_REGISTRATION,   /* slot arena span could not register */
    EMERALD_ENCOUNTER_ERR_UNAVAILABLE,          /* no published state to republish */
};

/* Structured diagnostics (same shape as the other compat seams). On
 * failure the FIRST failing entry is named. */
struct EmeraldEncounterCompatDiagnostics
{
    char canonicalName[96];
    char stage[24];                /* "build" / "publish" */
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

/* Validate + publish the encounter families. Transactional; idempotent for a
 * new session. `pack` must be the pack the session was built from. */
enum EmeraldEncounterCompatStatus
EmeraldEncounterCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldEncounterCompatDiagnostics *diagnostics);

/* Fail-closed clear: NULL every published header info pointer + info slot
 * pointer, zero the fixed tables, release the slot arena and unregister the
 * arena range. Used when a session is rolled back or shut down (idempotent). */
void EmeraldEncounterCompat_ClearMigratedEntries(void);
void EmeraldEncounterCompat_Shutdown(void);

/* Query helpers (tests + the R13-E2 ranges walker). PublishedCount is the
 * number of encounter resources published (210 until OK, else 0). */
size_t EmeraldEncounterCompat_GetPublishedCount(void);
size_t EmeraldEncounterCompat_GetSlotArenaBytes(void);
const char *EmeraldEncounterCompatStatus_Describe(
    enum EmeraldEncounterCompatStatus status);

#endif /* EMERALD_RESOURCES_EMERALD_ENCOUNTER_COMPAT_H */
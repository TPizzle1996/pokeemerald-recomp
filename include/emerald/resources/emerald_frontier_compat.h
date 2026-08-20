#ifndef EMERALD_RESOURCES_EMERALD_FRONTIER_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_FRONTIER_COMPAT_H

/* R13-E3a-1: Emerald Battle Frontier + Battle Tent trainer/mon data
 * publication seam. See emerald_trainer_compat.h for the sibling contract.
 *
 * Publishes the frontier families (structured-data schema 21 trainer metadata
 * / 22 mon-set index-stream leaves / 23 shared mons pool / 24 held-items / 25
 * banned-species) plus the three Battle Tent families (same trainer schema,
 * per-tent mons pools) from the production pack into their native HOST_DATA
 * fill targets (src/emerald/resources/frontier_data_native.c) through the
 * NORMAL M0/M1 snapshot.
 *
 * REFUSE-CLASS: the compiled const definitions in the battle_frontier data
 * headers (battle_frontier_trainers.h, battle_frontier_trainer_mons.h,
 * battle_frontier_mons.h, battle_tent.h), the gBattleFrontierHeldItems block
 * in src/battle_tower.c and gFrontierBannedSpecies in src/frontier_util.c are
 * NATIVE_LINUX-guarded out of the link, so there is no compiled fallback: a
 * session whose frontier data cannot publish is refused and the loader rolls
 * the whole registration back.
 *
 * Mon index-stream model confirmed from the ROM: each frontier/tent trainer's
 * GBA monSet pointer resolves to its own 0xFFFF-terminated u16 leaf (indices
 * 0..881 into the shared gBattleFrontierMons pool, or 0..pool-1 for a tent's
 * own pool). The seam rebuilds every native trainer row's monSet into its own
 * packed mon-set arena and publishes ONE COMPAT_OBJECT range over that arena.
 *
 * Transactional phases: 1 validates every trainer metadata row (52 B, ROM_BASE
 * winner, M0/M1 resolution, pack byte equality), every mon-set leaf (schema
 * 22; 0xFFFF-terminated, every index in range) and the row->leaf GBA-pointer
 * edge (monSet == leaf source ROM address + 0x08000000) for all 300 frontier +
 * 90 tent edges, the shared 882-row mons pool (16 B rows), held items, banned
 * species and the pack-level schema counts - BEFORE any allocation; 2 builds
 * the packed mon-set arena and the native mons/trainer/tent rows (all
 * infallible once phase 1 passed); 3 publishes atomically (pure stores) and
 * registers ONE COMPAT_OBJECT range over the mon-set arena. On any
 * phase-1/2 failure nothing is written and the diagnostics name the first
 * failing resource.
 *
 * Platform-neutral: no engine globals beyond the fill-target headers are
 * touched; it drives the gen3 session core like the other compat seams.
 * gFacilityTrainers/gFacilityTrainerMons are engine-side EWRAM aliases that
 * the engine re-points at SetFacilityPtrsGetLevel/SetTentPtrsGetLevel; after
 * publication they resolve into these fill targets automatically (no consumer
 * edit). The mon-set arena is the only malloc'd object, so it is the only
 * COMPAT_OBJECT range a State-v5 walk must relocate.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/resource_types.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/frontier_data_native.h"
#include "emerald/resources/frontier_native.generated.h"

#define EMERALD_FRONTIER_SCHEMA_TRAINER      21u
#define EMERALD_FRONTIER_SCHEMA_MON_SET      22u
#define EMERALD_FRONTIER_SCHEMA_MON          23u
#define EMERALD_FRONTIER_SCHEMA_HELD_ITEMS   24u
#define EMERALD_FRONTIER_SCHEMA_BANNED       25u
/* R13-E3a-2 facility AUX schema codes (26..37). */
#define EMERALD_FRONTIER_SCHEMA_FACTORY        26u
#define EMERALD_FRONTIER_SCHEMA_PALACE         27u
#define EMERALD_FRONTIER_SCHEMA_ARENA          28u
#define EMERALD_FRONTIER_SCHEMA_PIKE_NPC       29u
#define EMERALD_FRONTIER_SCHEMA_PIKE_SPEECH    30u
#define EMERALD_FRONTIER_SCHEMA_PYRAMID_FLOOR 31u
#define EMERALD_FRONTIER_SCHEMA_PYRAMID_ITEM  32u
#define EMERALD_FRONTIER_SCHEMA_PYRAMID_SLOTS 33u
#define EMERALD_FRONTIER_SCHEMA_BRAIN         34u
#define EMERALD_FRONTIER_SCHEMA_APPRENTICE    35u
#define EMERALD_FRONTIER_SCHEMA_WILD_HEADERS  36u
#define EMERALD_FRONTIER_SCHEMA_WILD          37u

/* ROM geometry (ROM-relative; GBA addresses +0x08000000). */
#define EMERALD_FRONTIER_TRAINER_WIRE            52u
#define EMERALD_FRONTIER_TRAINER_NATIVE          56u
#define EMERALD_FRONTIER_MON_WIRE                16u
#define EMERALD_FRONTIER_MON_NATIVE              14u

enum EmeraldFrontierCompatStatus
{
    EMERALD_FRONTIER_OK = 0,
    EMERALD_FRONTIER_ERR_INVALID_ARGUMENT,        /* NULL snapshot/pack pointer */
    EMERALD_FRONTIER_ERR_OUT_OF_MEMORY,
    EMERALD_FRONTIER_ERR_RESOLVE_FAILED,          /* M0/M1 snapshot did not resolve */
    EMERALD_FRONTIER_ERR_UNEXPECTED_OWNERSHIP,    /* winner is not ROM_BASE */
    EMERALD_FRONTIER_ERR_PAYLOAD_SIZE_MISMATCH,   /* pack/view size or bytes differ */
    EMERALD_FRONTIER_ERR_UNEXPECTED_COUNT,        /* frontier/tent composition != pins */
    EMERALD_FRONTIER_ERR_MONSET_LINK,             /* row monSet ptr != leaf ROM addr */
    EMERALD_FRONTIER_ERR_UNTERMINATED_MONSET,     /* leaf not 0xFFFF-terminated */
    EMERALD_FRONTIER_ERR_MON_INDEX,               /* leaf index out of pool range */
    EMERALD_FRONTIER_ERR_MISSING_RESOURCE,        /* a required whole-table resource is absent */
    EMERALD_FRONTIER_ERR_UNEXPECTED_TYPE,         /* entry type != STRUCTURED_DATA */
    EMERALD_FRONTIER_ERR_UNEXPECTED_SCHEMA,       /* entry schema != 21..25 */
    EMERALD_FRONTIER_ERR_RANGE_REGISTRATION,      /* mon-set arena span could not register */
    EMERALD_FRONTIER_ERR_UNAVAILABLE,             /* no published state to read */
/* ---- R13-E3a-2 facility AUX diagnostics (unused if E3a-2 not driven). ---- */
    EMERALD_FRONTIER_ERR_FACTORY_MOVES,           /* factory strategy move list bad */
    EMERALD_FRONTIER_ERR_PALACE_PRIZES,           /* Battle Palace prize table bad */
    EMERALD_FRONTIER_ERR_ARENA_PRIZES,            /* Battle Arena prize table bad */
    EMERALD_FRONTIER_ERR_PIKE_NPC,                /* Pike NPC table / row malformed */
    EMERALD_FRONTIER_ERR_PIKE_SPEECH,             /* Pike speeches/hints/heals bad */
    EMERALD_FRONTIER_ERR_PIKE_WILDMON,            /* Pike wild-mon table bad */
    EMERALD_FRONTIER_ERR_PYRAMID_FLOOR,           /* Pyramid floor template/options bad */
    EMERALD_FRONTIER_ERR_PYRAMID_ITEM,            /* Pyramid pickup item pool bad */
    EMERALD_FRONTIER_ERR_PYRAMID_SLOTS,           /* Pyramid pickup item slots bad */
    EMERALD_FRONTIER_ERR_BRAIN_IDS,               /* Frontier brain trainer-id table bad */
    EMERALD_FRONTIER_ERR_BRAIN_MONS,              /* Frontier brain mons table bad */
    EMERALD_FRONTIER_ERR_BRAIN_STREAK,            /* brain streak-appearances bad */
    EMERALD_FRONTIER_ERR_APPRENTICE,              /* apprentice 88->86 transform bad */
    EMERALD_FRONTIER_ERR_WILD_HEADER,             /* pike/pyramid wild header bad */
    EMERALD_FRONTIER_ERR_WILD_INFO,               /* header->info pointer edge bad */
    EMERALD_FRONTIER_ERR_WILD_SLOT,               /* info->slot pointer / size bad */
    EMERALD_FRONTIER_ERR_WILD_RATE,               /* wild encounter rate mismatch */
};

/* Structured diagnostics (same shape as the other compat seams). On failure
 * the FIRST failing entry is named. */
struct EmeraldFrontierCompatDiagnostics
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

/* Validate + publish the frontier/tent families. Transactional; idempotent
 * for a new session. `pack` must be the pack the session was built from. */
enum EmeraldFrontierCompatStatus
EmeraldFrontierCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldFrontierCompatDiagnostics *diagnostics);

/* Fail-closed clear: NULL every published monSet pointer (so no host pointer
 * dangles into a freed arena), zero the published tables, release the mon-set
 * arena and unregister the arena range. Used when a session is rolled back or
 * shut down (idempotent). */
void EmeraldFrontierCompat_ClearMigratedEntries(void);
void EmeraldFrontierCompat_Shutdown(void);

/* Query helpers (tests + the R13-E ranges walker). */
size_t EmeraldFrontierCompat_GetPublishedCount(void);
size_t EmeraldFrontierCompat_GetMonSetArenaBytes(void);
const char *EmeraldFrontierCompatStatus_Describe(
    enum EmeraldFrontierCompatStatus status);

#endif /* EMERALD_RESOURCES_EMERALD_FRONTIER_COMPAT_H */
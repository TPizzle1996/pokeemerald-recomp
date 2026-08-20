#ifndef EMERALD_RESOURCES_EMERALD_TRAINER_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_TRAINER_COMPAT_H

/* R13-E1: Emerald trainer-data (gTrainers / party leaves / trainer class
 * names) publication seam.
 *
 * Publishes the trainer families (structured-data schemas 16 metadata /
 * 17 party / 18 class-name) from the production pack into their native
 * HOST_DATA fill targets (struct Trainer gTrainers[855] - 48-byte native
 * rows with the party pointer rebuilt into the seam's packed party arena -
 * and gTrainerClassNames[66][13]) through the NORMAL M0/M1 snapshot.
 *
 * REFUSE-CLASS: the compiled const definitions (src/data/trainers.h,
 * src/data/text/trainer_class_names.h, src/data/trainer_parties.h) are
 * NATIVE_LINUX-guarded out of the link, so there is no compiled fallback:
 * a session whose trainer data cannot publish is refused and the loader
 * rolls the whole registration back.
 *
 * Transactional phases mirror EmeraldGameplayCompat: 1 validates every
 * trainer metadata row (schema 16, size 40, ROM_BASE winner, M0/M1
 * resolution, pack byte equality), every party leaf (schema 17; partySize x
 * ROM stride == leaf size; variant == partyFlags; the row's GBA party
 * pointer resolves to the leaf's ROM address), the generated-inventory
 * agreement, the 66 class-name rows and the contiguous 18,088-byte party
 * tiling - BEFORE any publication; 2 builds the packed party arena and the
 * native rows (all infallible once phase 1 passed); 3 publishes atomically
 * (pure stores) and registers ONE COMPAT_OBJECT range over the party arena.
 * On any phase-1/2 failure nothing is written and the diagnostics name the
 * first failing resource.
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
#include "emerald/resources/gameplay_native.generated.h"
#include "emerald/resources/trainer_native.generated.h"
#include "emerald/resources/trainer_data_native.h"

#if GAMEPLAY_NATIVE_RESOURCE_COUNT != 6458u
#error "R13-E gameplay+trainer+encounter resource count disagrees with the generated inventory"
#endif

#define EMERALD_TRAINER_SCHEMA_METADATA 16u
#define EMERALD_TRAINER_SCHEMA_PARTY    17u
#define EMERALD_TRAINER_SCHEMA_CLASS    18u

#define EMERALD_TRAINER_PARTY_COUNT     854u  /* non-NULL party leaves */

/* ROM geometry (ROM-relative offsets; GBA addresses +0x08000000). */
#define EMERALD_TRAINER_PARTY_BLOCK_START 0x30b62cu
#define EMERALD_TRAINER_PARTY_BLOCK_END   0x30fcd4u
#define EMERALD_TRAINER_PARTY_BLOCK_BYTES 18088u
#define EMERALD_TRAINER_ROW_WIRE          40u
#define EMERALD_TRAINER_CLASS_WIRE        13u

enum EmeraldTrainerCompatStatus
{
    EMERALD_TRAINER_OK = 0,
    EMERALD_TRAINER_ERR_INVALID_ARGUMENT,    /* NULL snapshot/pack pointer */
    EMERALD_TRAINER_ERR_OUT_OF_MEMORY,
    EMERALD_TRAINER_ERR_RESOLVE_FAILED,      /* M0/M1 snapshot did not resolve */
    EMERALD_TRAINER_ERR_UNEXPECTED_OWNERSHIP, /* winner is not ROM_BASE */
    EMERALD_TRAINER_ERR_PAYLOAD_SIZE_MISMATCH, /* pack/view size or bytes differ */
    EMERALD_TRAINER_ERR_UNEXPECTED_COUNT,    /* trainer/class composition != pins */
    EMERALD_TRAINER_ERR_TABLE_MISMATCH,      /* party leaf disagrees with the metadata */
    EMERALD_TRAINER_ERR_PARTY_LINK,          /* row party pointer != leaf ROM addr */
    EMERALD_TRAINER_ERR_VARIANT_MISMATCH,    /* partyFlags differs from the generated variant */
    EMERALD_TRAINER_ERR_BAD_PARTY_SIZE,      /* wire partySize != leaf entry count */
    EMERALD_TRAINER_ERR_TILING,              /* party leaves do not tile the block */
    EMERALD_TRAINER_ERR_UNEXPECTED_TYPE,     /* entry type != STRUCTURED_DATA */
    EMERALD_TRAINER_ERR_UNEXPECTED_SCHEMA,   /* entry schema != 16/17/18 */
    EMERALD_TRAINER_ERR_RANGE_REGISTRATION,  /* party arena span could not register */
    EMERALD_TRAINER_ERR_UNAVAILABLE,         /* no published state to republish */
};

/* Structured diagnostics (same shape as the other compat seams). On
 * failure the FIRST failing entry is named. */
struct EmeraldTrainerCompatDiagnostics
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

/* Validate + publish the trainer families. Transactional; idempotent for a
 * new session. `pack` must be the pack the session was built from. */
enum EmeraldTrainerCompatStatus
EmeraldTrainerCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldTrainerCompatDiagnostics *diagnostics);

/* Fail-closed clear: NULL every published party pointer (so no host pointer
 * dangles into a freed arena), release the party arena, unregister the
 * arena range and zero the fixed tables. Used when a session is rolled back
 * or shut down (idempotent). */
void EmeraldTrainerCompat_ClearMigratedEntries(void);
void EmeraldTrainerCompat_Shutdown(void);

/* Query helpers (tests + the R13-E ranges walker). PublishedCount is the
 * number of trainer resources published (0 until OK). PartyArenaBytes is
 * the packed party arena size in bytes. */
size_t EmeraldTrainerCompat_GetPublishedCount(void);
size_t EmeraldTrainerCompat_GetPartyArenaBytes(void);
const char *EmeraldTrainerCompatStatus_Describe(
    enum EmeraldTrainerCompatStatus status);

#endif /* EMERALD_RESOURCES_EMERALD_TRAINER_COMPAT_H */
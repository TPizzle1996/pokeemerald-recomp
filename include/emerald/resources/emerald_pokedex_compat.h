#ifndef EMERALD_RESOURCES_EMERALD_POKEDEX_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_POKEDEX_COMPAT_H

/* R13-E3b: Emerald Pokédex publication seam.
 *
 * Publishes the Pokédex structured-data families (schema 38 row + schema 39
 * order + schema 40 species-to-national) from the production pack into the
 * native HOST_DATA fill targets (src/emerald/resources/pokedex_data_native.c)
 * through the NORMAL M0/M1 snapshot, and completes the R13-C Pokédex
 * description-text cutover by re-pointing each row's description pointer into
 * the live R13-C text arena.
 *
 * REFUSE-CLASS: the compiled const data (gPokedexEntries in
 * src/data/pokemon/pokedex_entries.h, the gPokedexOrder_* arrays in
 * src/data/pokemon/pokedex_orders.h, sSpeciesToNationalPokedexNum in
 * src/pokemon.c, and the compiled g<Species>PokedexText payloads in
 * src/data/pokemon/pokedex_text.h) is NATIVE_LINUX-guarded out of the link, so
 * there is no compiled fallback: a session whose Pokédex data cannot publish
 * is refused and the loader rolls the whole registration back.
 *
 * Row model confirmed from the ROM: gPokedexEntries is 387 rows x 32 B GBA
 * wire (no 40 B rows), indexed by national-dex value. Row 0 is the UNKNOWN/
 * dummy entry (description = gDummyPokedexText). The description GBA pointer
 * @ wire offset 16 is the ONLY text pointer per row; it must resolve to the
 * reference g<Species>PokedexText symbol and hence an R13-C Pokédex text
 * label (emerald:text/pokedex/g<species>pokedextext). categoryName[12] is an
 * INLINE Gen-3 charmap string, copied byte-identically into the 40-byte native
 * row (no text binding). The native transform (32 B wire -> 40 B native) is
 * owned entirely by the seam, never the pack.
 *
 * Transactional phases: 1 validates every row resource (schema 38, exact 32 B,
 * ROM_BASE winner, M0/M1 resolution, pack byte equality, name/schema/size set
 * equality against the generated kPokedexRowKeys/kPokedexDescLabels, the
 * description GBA pointer binds to the exact R13-C text label via
 * EmeraldTextCompat, the ordering resources (schemas 39/40) and the schema
 * counts, and the full parity/oracle proof) BEFORE any write; 2 precomputes
 * every native row (category inline + scalar fields byte-identical, description
 * re-pointed into the R13-C arena) and the four native u16 arrays (all
 * infallible once phase 1 passed); 3 publishes atomically (pure stores). On any
 * phase-1/2 failure nothing is written and the diagnostics name the first
 * failing resource. No arena range is registered: gPokedexEntries.description
 * is a pure function-pointer-free u8 string pointer (a serialized pointer
 * surface into the R13-C text arena is already owned by EmeraldTextCompat for
 * the State-v5 walker); no Pokédex structured arena is allocated.
 *
 * Platform-neutral: no engine globals beyond the fill-target header are
 * touched.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/resource_types.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/pokedex_native.generated.h"
#include "emerald/resources/pokedex_data_native.h"
#include "emerald/resources/emerald_text_compat.h"

#define EMERALD_POKEDEX_SCHEMA_ROW    38u
#define EMERALD_POKEDEX_SCHEMA_ORDER  39u
#define EMERALD_POKEDEX_SCHEMA_S2N    40u

enum EmeraldPokedexCompatStatus
{
    EMERALD_POKEDEX_OK = 0,
    EMERALD_POKEDEX_ERR_INVALID_ARGUMENT,      /* NULL snapshot/pack pointer */
    EMERALD_POKEDEX_ERR_OUT_OF_MEMORY,
    EMERALD_POKEDEX_ERR_RESOLVE_FAILED,        /* M0/M1 snapshot did not resolve */
    EMERALD_POKEDEX_ERR_UNEXPECTED_OWNERSHIP,  /* winner is not ROM_BASE */
    EMERALD_POKEDEX_ERR_PAYLOAD_SIZE_MISMATCH, /* pack/view size or bytes differ */
    EMERALD_POKEDEX_ERR_UNEXPECTED_COUNT,      /* family composition != pins */
    EMERALD_POKEDEX_ERR_MISSING_RESOURCE,      /* a required whole-table resource is absent */
    EMERALD_POKEDEX_ERR_UNEXPECTED_TYPE,       /* entry type != STRUCTURED_DATA */
    EMERALD_POKEDEX_ERR_UNEXPECTED_SCHEMA,     /* entry schema != 38/39/40 */
    EMERALD_POKEDEX_ERR_DESC_PTR_INVALID,      /* description GBA addr not a ROM text addr */
    EMERALD_POKEDEX_ERR_TEXT_BINDING_MISSING,  /* desc addr resolves to no R13-C pokedex text label */
    EMERALD_POKEDEX_ERR_TEXT_LABEL_MISMATCH,   /* resolved label != generated kPokedexDescLabels[i] */
    EMERALD_POKEDEX_ERR_TEXT_NOT_PUBLISHED,    /* EmeraldTextCompat arena not live for the label */
    EMERALD_POKEDEX_ERR_MALFORMED_ORDER,       /* ordering/routing table malformed */
    EMERALD_POKEDEX_ERR_PARITY_MISMATCH,       /* native-oracle re-derivation != pack rows */
    EMERALD_POKEDEX_ERR_UNAVAILABLE,           /* no published state to republish */
};

/* Structured diagnostics (same shape as the other compat seams). On failure
 * the FIRST failing entry is named. */
struct EmeraldPokedexCompatDiagnostics
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

/* Validate + publish the Pokédex families. Transactional; idempotent for a
 * new session. `pack` must be the pack the session was built from. */
enum EmeraldPokedexCompatStatus
EmeraldPokedexCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldPokedexCompatDiagnostics *diagnostics);

/* Fail-closed clear: NULL every published description pointer (so no host
 * pointer dangles into a freed/failed arena), zero every HOST_DATA row/array.
 * Used when a session is rolled back or shut down (idempotent). */
void EmeraldPokedexCompat_ClearMigratedEntries(void);
void EmeraldPokedexCompat_Shutdown(void);

/* Query helpers (tests + the R13-E ranges walker). PublishedCount is the
 * number of published resources (387 rows + 4 tables = 391 when OK). */
size_t EmeraldPokedexCompat_GetPublishedCount(void);
const char *EmeraldPokedexCompatStatus_Describe(
    enum EmeraldPokedexCompatStatus status);

#endif /* EMERALD_RESOURCES_EMERALD_POKEDEX_COMPAT_H */
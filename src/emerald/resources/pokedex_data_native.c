/* R13-E3b: native Pokédex fill-target definitions.
 *
 * The single source of the runtime Pokédex rows + ordering/routing tables on
 * the native build. The arrays are HOST_DATA fill targets: the publication
 * seam (emerald_pokedex_compat.c) fills them from the production pack at boot
 * (REFUSE-class - the compiled const definitions in src/data/pokemon/
 * pokedex_entries.h + pokedex_orders.h and sSpeciesToNationalPokedexNum in
 * src/pokemon.c are NATIVE_LINUX-guarded out of the link, so there is no
 * fallback), and engine consumers read them through their existing extern
 * declarations (pokedex.c / src/international_string_util.c).
 *
 * Rows: 387 x 40-byte native struct PokedexEntry (categoryName[12] inline
 * Gen-3 charmap + height/weight/description + scale/offset). The description
 * field is a native text pointer the seam re-points into the R13-C Pokédex
 * text arena. The ordering/routing arrays are byte-identical u16 copies.
 * Native-only; compiled by Makefile_pc.
 */

#include "emerald/resources/pokedex_data_native.h"

HOST_DATA struct PokedexEntry gPokedexEntries[EMERALD_POKEDEX_ROW_COUNT];
HOST_DATA u16 sSpeciesToNationalPokedexNum[EMERALD_POKEDEX_S2N_COUNT];
HOST_DATA u16 gPokedexOrder_Alphabetical[EMERALD_POKEDEX_ALPHABETICAL_COUNT];
HOST_DATA u16 gPokedexOrder_Height[EMERALD_POKEDEX_HEIGHT_COUNT];
HOST_DATA u16 gPokedexOrder_Weight[EMERALD_POKEDEX_WEIGHT_COUNT];

_Static_assert(sizeof(struct PokedexEntry) == EMERALD_POKEDEX_ROW_NATIVE,
               "PokedexEntryNativeSizeMustBe40");
_Static_assert((size_t)EMERALD_POKEDEX_ROW_NATIVE
                   == (size_t)(EMERALD_POKEDEX_ROW_WIRE + 8u),
               "PokedexEntryNativeIsWirePlusPointerPad");
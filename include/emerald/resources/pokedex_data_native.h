#ifndef EMERALD_RESOURCES_POKEDEX_DATA_NATIVE_H
#define EMERALD_RESOURCES_POKEDEX_DATA_NATIVE_H

/* R13-E3b: native Pokédex fill-target declarations.
 *
 * Declares the HOST_DATA arrays the publication seam (emerald_pokedex_compat.c)
 * fills from the production pack: `struct PokedexEntry gPokedexEntries[387]`
 * (40-byte native rows) plus the four ordering/routing u16 arrays
 * (sSpeciesToNationalPokedexNum[411], gPokedexOrder_Alphabetical[411],
 * gPokedexOrder_Height[386], gPokedexOrder_Weight[386]) - all byte-identical
 * GBA/native u16 content. The DEFINITIONS live in
 * src/emerald/resources/pokedex_data_native.c (native-only; compiled by
 * Makefile_pc).
 *
 * struct PokedexEntry is layout-identical to the engine definition in
 * include/pokedex.h, so the seam writes the exact layout engine consumers read
 * through pokedex.h / src/international_string_util.c, without pulling
 * global.h into these global.h-free TUs. sizeof == 40 (EMERALD_POKEDEX_ROW_NATIVE)
 * is checked by _Static_assert in the definitions TU.
 *
 * The compiled const definitions (src/data/pokemon/pokedex_entries.h,
 * src/data/pokemon/pokedex_orders.h, sSpeciesToNationalPokedexNum in
 * src/pokemon.c) are NATIVE_LINUX-guarded out of the link (each keeps an
 * `#else extern` declaration so consumers compile), so there is no fallback: a
 * session whose Pokédex data cannot publish is REFUSED and the loader rolls the
 * whole registration back.
 */

#include "gba/types.h"
#include "gba/defines.h"
#include "emerald/resources/pokedex_native.generated.h"  /* count/stride macros */
#include "pokedex.h"                                     /* struct PokedexEntry */

extern HOST_DATA struct PokedexEntry gPokedexEntries[EMERALD_POKEDEX_ROW_COUNT];
extern HOST_DATA u16 sSpeciesToNationalPokedexNum[EMERALD_POKEDEX_S2N_COUNT];
extern HOST_DATA u16 gPokedexOrder_Alphabetical[EMERALD_POKEDEX_ALPHABETICAL_COUNT];
extern HOST_DATA u16 gPokedexOrder_Height[EMERALD_POKEDEX_HEIGHT_COUNT];
extern HOST_DATA u16 gPokedexOrder_Weight[EMERALD_POKEDEX_WEIGHT_COUNT];

#endif /* EMERALD_RESOURCES_POKEDEX_DATA_NATIVE_H */
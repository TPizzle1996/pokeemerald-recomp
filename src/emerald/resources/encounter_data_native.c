/* R13-E2: native wild-encounter fill-target definitions.
 *
 * The single source of the runtime map-based wild-encounter tables on the
 * native build. Both arrays are HOST_DATA fill targets: the publication seam
 * (emerald_encounter_compat.c) fills them from the production pack at boot
 * (REFUSE-class - the compiled const map-based definitions in
 * src/data/wild_encounters.h are NATIVE_LINUX-guarded out of the link, so
 * there is no fallback), and engine consumers read them through the extern
 * declarations in include/wild_encounter.h. The slot tables themselves are
 * NOT defined here: the seam rebuilds every info slot pointer into its own
 * slot arena (encounter_data_native.h comment).
 * Native-only; compiled by Makefile_pc.
 */

#include "emerald/resources/encounter_data_native.h"

HOST_DATA struct WildPokemonHeader gWildMonHeaders[EMERALD_ENCOUNTER_HEADER_COUNT];
HOST_DATA struct WildPokemonInfo gWildEncounterInfos[EMERALD_ENCOUNTER_INFO_COUNT];

_Static_assert(sizeof(struct WildPokemon) == 4u, "WildPokemonSize");
_Static_assert(sizeof(struct WildPokemonInfo) == 16u, "WildPokemonInfoSize");
_Static_assert(sizeof(struct WildPokemonHeader) == 40u, "WildPokemonHeaderSize");
_Static_assert(sizeof(gWildMonHeaders) == 125u * 40u, "WildMonHeadersSize");
_Static_assert(sizeof(gWildEncounterInfos) == 209u * 16u, "WildEncounterInfosSize");
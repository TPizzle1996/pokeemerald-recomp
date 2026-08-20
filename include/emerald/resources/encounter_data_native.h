#ifndef EMERALD_RESOURCES_ENCOUNTER_DATA_NATIVE_H
#define EMERALD_RESOURCES_ENCOUNTER_DATA_NATIVE_H

/* R13-E2: native wild-encounter fill targets.
 *
 * Declares the HOST_DATA arrays the publication seam (emerald_encounter_compat.c)
 * fills from the production pack: `struct WildPokemonHeader gWildMonHeaders[125]`
 * (40-byte native rows, the four info pointers rebuilt to the seam's published
 * native WildPokemonInfo objects) and `struct WildPokemonInfo
 * gWildEncounterInfos[209]` (16-byte native rows, slot pointer rebuilt into the
 * seam's slot arena). The slots themselves live in a single packed arena owned
 * by the seam (not a fixed array here), exactly like the trainer party arena.
 * The DEFINITIONS live in src/emerald/resources/encounter_data_native.c
 * (native-only); this header is included by BOTH the definitions TU and the
 * seam TU.
 *
 * The structs here are layout-identical to the engine's definitions in
 * include/wild_encounter.h, so the seam writes the exact layout engine
 * consumers read through include/wild_encounter.h (wild_encounter.c,
 * match_call.c, pokedex_area_screen.c), without pulling global.h into these
 * global.h-free TUs. sizeof: struct WildPokemon 4, struct WildPokemonInfo 16,
 * struct WildPokemonHeader 40 (checked by _Static_assert in the definitions
 * TU).
 *
 * The native declaration of gWildMonHeaders in include/wild_encounter.h is
 * non-const under NATIVE_LINUX (extern struct WildPokemonHeader gWildMonHeaders[]);
 * GBA keeps the const definition verbatim in src/data/wild_encounters.h.
 */

#include "gba/types.h"
#include "gba/defines.h"

#define EMERALD_ENCOUNTER_HEADER_COUNT 125u  /* 124 real + MAP_UNDEFINED sentinel */
#define EMERALD_ENCOUNTER_INFO_COUNT    209u /* 95 land + 55 water + 6 rock-smash
                                                + 53 fishing */
#define EMERALD_ENCOUNTER_HEADERS_BYTES 2500u

/* A TU that already includes include/data.h (GUARD_DATA_H) provides the real
 * structs via include/wild_encounter.h plus the `extern struct WildPokemonHeader
 * gWildMonHeaders[]` declaration; skip the layout-identical copies so there is
 * never a redefinition. The definitions TU + the seam TU never pull data.h, so
 * they get the copies here. The layouts are identical (checked by
 * _Static_assert in the definitions TU), so the symbol bytes match. */
#ifndef GUARD_DATA_H

struct WildPokemon
{
    u8 minLevel;
    u8 maxLevel;
    u16 species;
};

struct WildPokemonInfo
{
    u8 encounterRate;
    const struct WildPokemon *wildPokemon;
};

struct WildPokemonHeader
{
    u8 mapGroup;
    u8 mapNum;
    u16 pad;
    const struct WildPokemonInfo *landMonsInfo;
    const struct WildPokemonInfo *waterMonsInfo;
    const struct WildPokemonInfo *rockSmashMonsInfo;
    const struct WildPokemonInfo *fishingMonsInfo;
};

#endif /* GUARD_DATA_H */

/* The native fill targets (see the header comment). Declared OUTSIDE the
 * GUARD_DATA_H guard so a TU that includes data.h (GUARD_DATA_H) and pulls
 * the real structs from include/wild_encounter.h can still bind these
 * (include/wild_encounter.h declares gWildMonHeaders itself under NATIVE_LINUX;
 * this redeclaration is identical and legal; gWildEncounterInfos is unique to
 * this header). A TU that includes this header without data.h gets the copies
 * above. */
extern struct WildPokemonHeader gWildMonHeaders[EMERALD_ENCOUNTER_HEADER_COUNT];
extern struct WildPokemonInfo gWildEncounterInfos[EMERALD_ENCOUNTER_INFO_COUNT];

#endif /* EMERALD_RESOURCES_ENCOUNTER_DATA_NATIVE_H */
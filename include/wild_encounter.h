#ifndef GUARD_WILD_ENCOUNTER_H
#define GUARD_WILD_ENCOUNTER_H

#include "constants/wild_encounter.h"

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
    const struct WildPokemonInfo *landMonsInfo;
    const struct WildPokemonInfo *waterMonsInfo;
    const struct WildPokemonInfo *rockSmashMonsInfo;
    const struct WildPokemonInfo *fishingMonsInfo;
};

/* R13-E2: on native the map-based gWildMonHeaders is the seam-published
 * HOST_DATA fill target (encounter_data_native.c), so it is mutable.
 * GBA keeps the const definition verbatim (src/data/wild_encounters.h). */
#if defined(NATIVE_LINUX)
extern struct WildPokemonHeader gWildMonHeaders[];
#else
extern const struct WildPokemonHeader gWildMonHeaders[];
#endif

/* R13-E3a-2: the Battle Frontier Pike/Pyramid wild-encounter header blocks are
 * seam-published HOST_DATA fill targets (frontier_data_native.c) on native. */
#if defined(NATIVE_LINUX)
extern struct WildPokemonHeader gBattlePikeWildMonHeaders[];
extern struct WildPokemonHeader gBattlePyramidWildMonHeaders[];
#else
extern const struct WildPokemonHeader gBattlePikeWildMonHeaders[];
extern const struct WildPokemonHeader gBattlePyramidWildMonHeaders[];
#endif

void DisableWildEncounters(bool8 disabled);
bool8 StandardWildEncounter(u16 curMetatileBehavior, u16 prevMetatileBehavior);
bool8 SweetScentWildEncounter(void);
bool8 DoesCurrentMapHaveFishingMons(void);
void FishingWildEncounter(u8 rod);
u16 GetLocalWildMon(bool8 *isWaterMon);
u16 GetLocalWaterMon(void);
bool8 UpdateRepelCounter(void);

#endif // GUARD_WILD_ENCOUNTER_H

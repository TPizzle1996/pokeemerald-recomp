#ifndef EMERALD_RESOURCES_GAMEPLAY_DATA_NATIVE_H
#define EMERALD_RESOURCES_GAMEPLAY_DATA_NATIVE_H

/* R13-D1: native gameplay-data fill targets.
 *
 * Declares the HOST_DATA arrays the publication seam
 * (emerald_gameplay_compat.c) fills from the production pack. The
 * DEFINITIONS live in src/emerald/resources/gameplay_data_native.c
 * (native-only); this header is included by BOTH the definitions TU and
 * the seam TU. Only types available without pulling the engine global.h
 * (gba/types.h types, gba/defines.h HOST_DATA, the struct/constant
 * headers the arrays are typed with).
 *
 * The five previously-hydrated arrays (gSpeciesInfo, gBattleMoves,
 * gExperienceTables, gSpeciesNames, gMoveNames) are also already
 * declared (extern, non-const under DESKTOP_EXTERNAL_GAME_CONTENT) by
 * include/pokemon.h and include/data.h; the declarations here are the
 * sized complement that pins the array bounds for the seam.
 */

#include "gba/types.h"
#include "gba/defines.h"
#include "constants/species.h"     /* NUM_SPECIES, SPECIES_EGG */
#include "constants/moves.h"       /* MOVES_COUNT */
#include "constants/pokemon.h"     /* MAX_LEVEL */
#include "constants/global.h"      /* POKEMON_NAME_LENGTH, MOVE_NAME_LENGTH */
#include "constants/party_menu.h"  /* TUTOR_MOVE_COUNT */
#include "fonts.h"                 /* the ten u16 glyph glyph arrays (extern) */
#include "constants/items.h"       /* ITEMS_COUNT + ITEM_USE_PARTY_MENU etc. */

/* D2: the struct Item fill-target type, layout-identical to the engine's
 * include/item.h definition so the seam fills gItems exactly as the engine
 * consumers read them, without dragging global.h into these global.h-free
 * TUs. A TU that already includes item.h (GUARD_ITEM_H, e.g. the loader test
 * harness) keeps the engine struct; the local definition is byte-identical
 * (same members in the same order; GameplayItemUseFunc and ItemUseFunc are
 * both `void (*)(u8)`). */
#ifndef GUARD_ITEM_H
typedef void (*GameplayItemUseFunc)(u8);
struct Item
{
    u8 name[ITEM_NAME_LENGTH];
    u16 itemId;
    u16 price;
    u8 holdEffect;
    u8 holdEffectParam;
    const u8 *description;
    u8 importance;
    bool8 registrability;
    u8 pocket;
    u8 type;
    GameplayItemUseFunc fieldUseFunc;
    u8 battleUsage;
    GameplayItemUseFunc battleUseFunc;
    u8 secondaryId;
};
#endif /* GUARD_ITEM_H */

/* The D1 fill-target structs, layout-identical to the engine's
 * include/pokemon.h:297..325 / include/contest_effect.h:8..21 definitions so
 * the seam fills the arrays exactly as the engine consumers read them,
 * without pulling the engine headers (which drag global.h into these
 * global.h-free TUs). They are only ever used to type the fill-target arrays
 * in this header + the definitions TU; a TU that already includes the real
 * engine structs (global.h / pokemon.h / contest_effect.h, e.g. the loader
 * test harness) skips these via the same include guards, so there is never a
 * redefinition. sizeof: SpeciesInfo 26 / BattleMove 9 / ContestMove 7 /
 * ContestEffect 3 (checked by _Static_assert in the definitions TU). */
#ifndef GUARD_POKEMON_H
struct SpeciesInfo
{
    u8 baseHP;
    u8 baseAttack;
    u8 baseDefense;
    u8 baseSpeed;
    u8 baseSpAttack;
    u8 baseSpDefense;
    u8 types[2];
    u8 catchRate;
    u8 expYield;
    u16 evYield_HP : 2;
    u16 evYield_Attack : 2;
    u16 evYield_Defense : 2;
    u16 evYield_Speed : 2;
    u16 evYield_SpAttack : 2;
    u16 evYield_SpDefense : 2;
    u16 itemCommon;
    u16 itemRare;
    u8 genderRatio;
    u8 eggCycles;
    u8 friendship;
    u8 growthRate;
    u8 eggGroups[2];
    u8 abilities[2];
    u8 safariZoneFleeRate;
    u8 bodyColor : 7;
    u8 noFlip : 1;
};

struct BattleMove
{
    u8 effect;
    u8 power;
    u8 type;
    u8 accuracy;
    u8 pp;
    u8 secondaryEffectChance;
    u8 target;
    s8 priority;
    u8 flags;
};
#endif /* GUARD_POKEMON_H */

#ifndef GUARD_CONTEST_EFFECT_H
struct ContestMove
{
    u8 effect;
    u8 contestCategory : 3;
    u8 comboStarterId;
    u8 comboMoves[4];
};

struct ContestEffect
{
    u8 effectType;
    u8 appeal;
    u8 jam;
};
#endif /* GUARD_CONTEST_EFFECT_H */

/* The TM/HM compatibility array: 8 bytes per species, exposed to consumers
 * as the anonymous union's as_u32s[2]. The engine typedef (tmhm_learnsets.h)
 * the consumers compile against carries the identical 8-byte layout; this
 * layout-identical union lets the seam + definitions TU fill the array
 * without pulling the engine bitfield struct. */
typedef union {
    u32 as_u32s[2];
} GameplayTMHMLearnsetData;

/* The TMHMLearnset union array (see tmhm_learnsets.h for the struct):
 * 8 bytes per species (2 x u32). Filled raw from the /tmhm resources. */

/* Species: 412 x struct SpeciesInfo (26 B packed), wire 28 B. */
extern struct SpeciesInfo gSpeciesInfo[NUM_SPECIES];
/* Moves: 355 x struct BattleMove (9 B packed), wire 12 B. */
extern struct BattleMove gBattleMoves[MOVES_COUNT];
/* Growth curves: 8 rates x (MAX_LEVEL + 1) LE u32. */
extern u32 gExperienceTables[8][MAX_LEVEL + 1];
/* Fixed-width charmap rows, 11 B / 13 B. */
extern u8 gSpeciesNames[NUM_SPECIES][POKEMON_NAME_LENGTH + 1];
extern u8 gMoveNames[MOVES_COUNT][MOVE_NAME_LENGTH + 1];
/* TM/HM compatibility, 8 B row (union typedef from tmhm_learnsets.h). */
extern GameplayTMHMLearnsetData gTMHMLearnsets[NUM_SPECIES];
/* MOVE_TUTOR compatibility bitmasks, 4 B row. */
extern u32 sTutorLearnsets[NUM_SPECIES];
/* The 30 fixed tutor moves, 60 B. */
extern u16 gTutorMoves[TUTOR_MOVE_COUNT];
/* Flat egg-moves stream, 1139 u16 (165 blocks + 0xFFFF terminator).
 * Sized so ARRAY_COUNT(gEggMoves) works for src/daycare.c. */
#define GAMEPLAY_GEGG_MOVES_U16 1139u
extern u16 gEggMoves[GAMEPLAY_GEGG_MOVES_U16];
/* Contest moves (7 B packed, wire 8 B) / effects (3 B packed, wire 4 B). */
extern struct ContestMove gContestMoves[MOVES_COUNT];
extern struct ContestEffect gContestEffects[48];
extern u8 gComboStarterLookupTable[63];
/* Level-up learnset pointer table (rebuilt to the seam's leaf arena).
 * NULL until the seam fills it; seeds NULL zry. */
extern u16 *gLevelUpLearnsets[NUM_SPECIES];

/* D2: the item fill target, 377 x struct Item (72 B). The compiled
 * `const struct Item gItems[]` in src/data/items.h is NATIVE_LINUX-guarded
 * out of the native link; this HOST_DATA array (defined in the definitions
 * TU) is the single native definition the seam fills. item.h already externs
 * `struct Item gItems[]` (non-const under DESKTOP_EXTERNAL_GAME_CONTENT);
 * this sized complement pins the ITEMS_COUNT bound for the seam. */
extern struct Item gItems[ITEMS_COUNT];

#endif /* EMERALD_RESOURCES_GAMEPLAY_DATA_NATIVE_H */
/* R13-D2: native item-use callback table (ENGINE_CONSTANT metadata).
 *
 * Maps every `enum GameplayItemUseAction` token in the 27-value census to the
 * native `ItemUseFunc` the engine link provides. The census source of truth
 * is kGameplayItemCallbacks (gameplay_callbacks.generated.c); this table is
 * the closing half of that registry - the seam resolves a wire row's two GBA
 * use-function addresses through kGameplayItemCallbacks (by gbaAddr) to an
 * action, then via this table to a live function pointer.
 *
 * Note the ENGINE is the one guaranteed native set: the reference item-use
 * census uses exactly these 27 symbols. ItemUseOutOfBattle_Berry is declared
 * in include/item_use.h but is not referenced by any gItems row, so it is
 * deliberately NOT mapped (the census is the 27 actions).
 */

#include "emerald/resources/gameplay_item_callbacks_native.h"
#include "item_use.h"

/* Direct action -> function resolution. A static switch (not a table) so the
 * enum -> function pairing is spelled out next to the census and a mismatch
 * is a compile error. Kept as a single switch over all 27 actions. */
GameplayItemUseFunc GameplayItemUseFunctionForAction(enum GameplayItemUseAction action)
{
    switch (action)
    {
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_MAIL:          return ItemUseOutOfBattle_Mail;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_BIKE:          return ItemUseOutOfBattle_Bike;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_ROD:           return ItemUseOutOfBattle_Rod;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_ITEMFINDER:    return ItemUseOutOfBattle_Itemfinder;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_POKEBLOCKCASE: return ItemUseOutOfBattle_PokeblockCase;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_COINCASE:      return ItemUseOutOfBattle_CoinCase;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_POWDERJAR:     return ItemUseOutOfBattle_PowderJar;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_WAILMERPAIL:   return ItemUseOutOfBattle_WailmerPail;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_MEDICINE:      return ItemUseOutOfBattle_Medicine;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_REDUCEEV:      return ItemUseOutOfBattle_ReduceEV;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_SACREDASH:     return ItemUseOutOfBattle_SacredAsh;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_PPRECOVERY:    return ItemUseOutOfBattle_PPRecovery;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_PPUP:          return ItemUseOutOfBattle_PPUp;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_RARECANDY:     return ItemUseOutOfBattle_RareCandy;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_TMHM:          return ItemUseOutOfBattle_TMHM;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_REPEL:         return ItemUseOutOfBattle_Repel;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_BLACKWHITEFLUTE: return ItemUseOutOfBattle_BlackWhiteFlute;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_ESCAPEROPE:    return ItemUseOutOfBattle_EscapeRope;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_EVOLUTIONSTONE: return ItemUseOutOfBattle_EvolutionStone;
    case GAMEPLAY_ITEM_USE_ACTION_INBATTLE_POKEBALL:         return ItemUseInBattle_PokeBall;
    case GAMEPLAY_ITEM_USE_ACTION_INBATTLE_STATINCREASE:     return ItemUseInBattle_StatIncrease;
    case GAMEPLAY_ITEM_USE_ACTION_INBATTLE_MEDICINE:         return ItemUseInBattle_Medicine;
    case GAMEPLAY_ITEM_USE_ACTION_INBATTLE_PPRECOVERY:       return ItemUseInBattle_PPRecovery;
    case GAMEPLAY_ITEM_USE_ACTION_INBATTLE_ESCAPE:           return ItemUseInBattle_Escape;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_ENIGMABERRY:   return ItemUseOutOfBattle_EnigmaBerry;
    case GAMEPLAY_ITEM_USE_ACTION_INBATTLE_ENIGMABERRY:      return ItemUseInBattle_EnigmaBerry;
    case GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_CANNOTUSE:     return ItemUseOutOfBattle_CannotUse;
    case GAMEPLAY_ITEM_USE_ACTION_COUNT:
        break;
    }
    return NULL; /* unreachable for a census-validated action */
}
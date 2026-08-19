/* R13-C text seam harness stubs (tests/emerald_runtime_loader_test.c).
 *
 * The generated skeleton arrays (text_skeleton_arrays.generated.c) are
 * linked into the runtime-loader harness. Their compiled-constant column
 * references 17 native data symbols (the seven Japanese status strings,
 * gText_DisplaySettings, gTypeNames, sWallyLocationData and the seven
 * sText_* messages that stay compiled) and 55 `void NAME(u8)` table
 * action callbacks. The harness never runs a callback and never
 * dereferences a stub array - the generated tables only take their
 * ADDRESSES - so every definition below is an address stub: values are
 * never inspected, only the link must succeed.
 *
 * In the real native build these symbols are defined by the C-side
 * sources (strings.c, match_call.c, the menu/table consumers); the
 * harness replaces those sources wholesale.
 */

#include "gba/types.h"
#include "constants/global.h"    /* TYPE_NAME_LENGTH */
#include "constants/pokemon.h"   /* NUMBER_OF_MON_TYPES */
#include "match_call.h"          /* struct MatchCallLocationOverride */

/* --- compiled-constant data (17) --- */

const u8 gStatusConditionString_BurnJpn[] = { 0xFF };
const u8 gStatusConditionString_ConfusionJpn[] = { 0xFF };
const u8 gStatusConditionString_IceJpn[] = { 0xFF };
const u8 gStatusConditionString_LoveJpn[] = { 0xFF };
const u8 gStatusConditionString_ParalysisJpn[] = { 0xFF };
const u8 gStatusConditionString_PoisonJpn[] = { 0xFF };
const u8 gStatusConditionString_SleepJpn[] = { 0xFF };
const u8 gText_DisplaySettings[] = { 0xFF };
const u8 gTypeNames[NUMBER_OF_MON_TYPES][TYPE_NAME_LENGTH + 1] = { { 0 } };
const struct MatchCallLocationOverride sWallyLocationData[] = { { 0, 0 } };
const u8 sText_CommunicationStandby[] = { 0xFF };
const u8 sText_OnlyPkmnForBattle[] = { 0xFF };
const u8 sText_TheTradeHasBeenCanceled[] = { 0xFF };
const u8 sText_WaitingForYourFriend[] = { 0xFF };
const u8 sText_YourFriendWantsToTrade[] = { 0xFF };
const u8 sText_Exit[] = { 0xFF };
const u8 sText_Info[] = { 0xFF };

/* --- compiled-constant action callbacks (55, all void(u8)) --- */

#define STUB_FN(name) void name(u8 taskId) { (void)taskId; }

STUB_FN(BagAction_Cancel)
STUB_FN(BagAction_Give)
STUB_FN(BagAction_Toss)
STUB_FN(BagAction_UseInBattle)
STUB_FN(BagAction_UseOnField)
STUB_FN(DecorationMenuAction_Cancel)
STUB_FN(DecorationMenuAction_Decorate)
STUB_FN(DecorationMenuAction_PutAway)
STUB_FN(DecorationMenuAction_Toss)
STUB_FN(ItemMenu_Cancel)
STUB_FN(ItemMenu_CheckTag)
STUB_FN(ItemMenu_ConfirmQuizLady)
STUB_FN(ItemMenu_Give)
STUB_FN(ItemMenu_GiveFavorLady)
STUB_FN(ItemMenu_Register)
STUB_FN(ItemMenu_Show)
STUB_FN(ItemMenu_Toss)
STUB_FN(ItemMenu_UseInBattle)
STUB_FN(ItemMenu_UseOutOfBattle)
STUB_FN(ItemStorage_Deposit)
STUB_FN(ItemStorage_Exit)
STUB_FN(ItemStorage_Toss)
STUB_FN(ItemStorage_Withdraw)
STUB_FN(Mailbox_Cancel)
STUB_FN(Mailbox_DoMailRead)
STUB_FN(Mailbox_Give)
STUB_FN(Mailbox_MoveToBag)
STUB_FN(PlayerPC_Decoration)
STUB_FN(PlayerPC_ItemStorage)
STUB_FN(PlayerPC_Mailbox)
STUB_FN(PlayerPC_TurnOff)
STUB_FN(PokeblockAction_Cancel)
STUB_FN(PokeblockAction_GiveToContestLady)
STUB_FN(PokeblockAction_Toss)
STUB_FN(PokeblockAction_UseInBattle)
STUB_FN(PokeblockAction_UseOnField)
STUB_FN(PokeblockAction_UseOnPokeblockFeeder)
STUB_FN(ReturnToMainRegistryMenu)
STUB_FN(ShowRegistryMenuDeleteConfirmation)
STUB_FN(StartMenuBagCallback)
STUB_FN(StartMenuBattlePyramidBagCallback)
STUB_FN(StartMenuBattlePyramidRetireCallback)
STUB_FN(StartMenuExitCallback)
STUB_FN(StartMenuLinkModePlayerNameCallback)
STUB_FN(StartMenuOptionCallback)
STUB_FN(StartMenuPlayerNameCallback)
STUB_FN(StartMenuPokeNavCallback)
STUB_FN(StartMenuPokedexCallback)
STUB_FN(StartMenuPokemonCallback)
STUB_FN(StartMenuSafariZoneRetireCallback)
STUB_FN(StartMenuSaveCallback)
STUB_FN(Task_FadeAndCloseBagMenu)
STUB_FN(Task_HandleShopMenuBuy)
STUB_FN(Task_HandleShopMenuQuit)
STUB_FN(Task_HandleShopMenuSell)

#undef STUB_FN

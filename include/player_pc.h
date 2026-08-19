#ifndef GUARD_PLAYER_PC_H
#define GUARD_PLAYER_PC_H

#include "menu.h"
#ifdef NATIVE_LINUX
#include "emerald/resources/text_slots.generated.h"
#include "emerald/resources/text_skeleton_arrays.generated.h"
#endif

struct PlayerPCItemPageStruct
{
    u16 cursorPos;
    u16 itemsAbove;
    u8 pageItems;
    u8 count;
    u8 filler[3];
    u8 scrollIndicatorTaskId;
};

extern struct PlayerPCItemPageStruct gPlayerPCItemPageInfo;

#ifndef NATIVE_LINUX
extern const struct MenuAction gMailboxMailOptions[];
#endif

void ReshowPlayerPC(u8 var);
void CB2_PlayerPCExitBagMenu(void);
void Mailbox_ReturnToMailListAfterDeposit(void);
void NewGameInitPCItems(void);


#endif //GUARD_PLAYER_PC_H

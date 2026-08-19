#ifndef GUARD_UNION_ROOM_CHAT_H
#define GUARD_UNION_ROOM_CHAT_H

// R13-C: file-local struct moved here so the generated host arrays
// (text_skeleton_arrays.generated.{c,h}) can type their externs.
struct MessageWindowInfo
{
    const u8 *text;
    u8 boxType;
    u8 x;
    u8 y;
    u8 letterSpacing;
    u8 lineSpacing;
    bool8 hasPlaceholders;
    bool8 useWiderBox;
};

void EnterUnionRoomChat(void);
void InitUnionRoomChatRegisteredTexts(void);

#endif // GUARD_UNION_ROOM_CHAT_H

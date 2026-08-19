#ifndef GUARD_MATCH_CALL_H
#define GUARD_MATCH_CALL_H

enum {
    MATCH_CALL_TYPE_NON_TRAINER,
    MATCH_CALL_TYPE_TRAINER,
    MATCH_CALL_TYPE_WALLY,
    MATCH_CALL_TYPE_BIRCH,
    MATCH_CALL_TYPE_MAY_BRENDAN,
    MATCH_CALL_TYPE_GYMLEADER_ELITEFOUR
};

// R13-C: these file-local structs moved here so the generated host
// arrays (text_skeleton_arrays.generated.{c,h}) can type their externs.
#define NUM_STRVARS_IN_MSG 3

struct MatchCallText
{
    const u8 *text;
    s8 stringVarFuncIds[NUM_STRVARS_IN_MSG];
};

struct MultiTrainerMatchCallText
{
    u16 trainerId;
    const u8 *text;
};

// R13-C: match-call data structs moved from pokenav_match_call_data.c
// (which has no header of its own) so the generated host arrays can type
// their externs; the host .c also re-derives these rows at publish.
typedef struct MatchCallTextDataStruct {
    const u8 *text;
    u16 flag;
    u16 flag2;
} match_call_text_data_t;

struct MatchCallStructNPC {
    u8 type;
    u8 mapSec;
    u16 flag;
    const u8 *desc;
    const u8 *name;
    const match_call_text_data_t *textData;
};

// Shared by MC_TYPE_TRAINER and MC_TYPE_LEADER
struct MatchCallStructTrainer {
    u8 type;
    u8 mapSec;
    u16 flag;
    u16 rematchTableIdx;
    const u8 *desc;
    const u8 *name;
    const match_call_text_data_t *textData;
};

struct MatchCallLocationOverride {
    u16 flag;
    u8 mapSec;
};

struct MatchCallWally {
    u8 type;
    u8 mapSec;
    u16 flag;
    u16 rematchTableIdx;
    const u8 *desc;
    const match_call_text_data_t *textData;
    const struct MatchCallLocationOverride *locationData;
};

struct MatchCallBirch {
    u8 type;
    u8 mapSec;
    u16 flag;
    const u8 *desc;
    const u8 *name;
};

struct MatchCallRival {
    u8 type;
    u8 playerGender;
    u16 flag;
    const u8 *desc;
    const u8 *name;
    const match_call_text_data_t *textData;
};

s32 GetRematchIdxByTrainerIdx(s32 trainerIdx);
void InitMatchCallCounters(void);
bool32 TryStartMatchCall(void);
bool32 IsMatchCallTaskActive(void);
void StartMatchCallFromScript(const u8 *message);
void BufferPokedexRatingForMatchCall(u8 *destStr);
bool32 SelectMatchCallMessage(int trainerId, u8 *str);
void LoadMatchCallWindowGfx(u32 windowId, u32 destOffset, u32 paletteId);
void DrawMatchCallTextBoxBorder(u32 windowId, u32 tileOffset, u32 paletteId);

#endif //GUARD_MATCH_CALL_H

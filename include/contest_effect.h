#ifndef GUARD_CONTEST_EFFECT_H
#define GUARD_CONTEST_EFFECT_H
#ifdef NATIVE_LINUX
#include "emerald/resources/text_slots.generated.h"
#include "emerald/resources/text_skeleton_arrays.generated.h"
#endif

struct ContestMove
{
    u8 effect;
    u8 contestCategory:3;
    u8 comboStarterId;
    u8 comboMoves[4];
};

struct ContestEffect
{
    u8 effectType;
    u8 appeal;
    u8 jam;
};

extern const struct ContestMove gContestMoves[];
extern const struct ContestEffect gContestEffects[];
#ifndef NATIVE_LINUX
extern const u8 *const gContestEffectDescriptionPointers[];
#endif
#ifndef NATIVE_LINUX
extern const u8 *const gContestMoveTypeTextPointers[];
#endif

bool8 AreMovesContestCombo(u16 lastMove, u16 nextMove);

#endif //GUARD_CONTEST_EFFECT_H

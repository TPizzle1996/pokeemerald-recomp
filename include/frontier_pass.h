#ifndef GUARD_FRONTIER_PASS_H
#define GUARD_FRONTIER_PASS_H

// R13-C: file-local struct moved here so the generated host arrays
// (text_skeleton_arrays.generated.{c,h}) can type their externs.
struct MapLandmark
{
    const u8 *name;
    const u8 *description;
    s16 x;
    s16 y;
    u8 animNum;
};

void ShowFrontierPass(void (*callback)(void));
void CB2_ReshowFrontierPass(void);

#endif // GUARD_FRONTIER_PASS_H

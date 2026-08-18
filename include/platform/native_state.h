#ifndef GUARD_PLATFORM_NATIVE_STATE_H
#define GUARD_PLATFORM_NATIVE_STATE_H

#include "gba/types.h"
#include "platform/desktop_profiles.h"

enum NativeStateResult
{
    NATIVE_STATE_OK,
    NATIVE_STATE_UNAVAILABLE,
    NATIVE_STATE_IO_ERROR,
    NATIVE_STATE_INCOMPATIBLE,
    NATIVE_STATE_CORRUPT,
    NATIVE_STATE_UNSUPPORTED,
};

enum NativeStateResult NativeState_Save(u8 slot);
enum NativeStateResult NativeState_Load(u8 slot);
u32 NativeState_GetFormatVersion(void);
const char *NativeState_GetLastError(void);

/* R12-E audio live-pointer canary + scenario battery (defined in sdl2.c and
 * native_audio_scenarios.c). The canary walks a playing player's hydrated
 * song header, tracks and channels and asserts every live pointer is
 * arena-resident (see the implementation comment in sdl2.c); allowBssBytecode
 * exempts the cry players' designed BSS bytecode pointers (cry tone must
 * still be arena-resident). */
struct MusicPlayerInfo;
struct SoundInfo;

const char *NativeAudioCheckLiveCanary(
    const struct MusicPlayerInfo *playing, const struct SoundInfo *soundInfo,
    bool32 allowBssBytecode);
int NativeAudioScenarioTest(bool32 runCanary, u64 perfBaselineNs);
int NativeAudioRefusalTest(void);

#endif

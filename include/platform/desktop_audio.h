#ifndef GUARD_PLATFORM_DESKTOP_AUDIO_H
#define GUARD_PLATFORM_DESKTOP_AUDIO_H

#include "gba/types.h"

bool32 Platform_AudioInit(u32 sampleRate);
void Platform_AudioSetPaused(bool32 paused);
void Platform_AudioClearQueue(void);
void Platform_AudioShutdown(void);

#if defined(HARNESS_REAL_SDL_PROBE)
/* Headless desktop-audio probe only (tests/run_desktop_real_sdl_probe.sh):
 * exposes the opened device id so the probe can query the REAL SDL device
 * state through the desktop save/load sequence. Compiled exclusively into
 * the probe binary; never in production builds. (u32 == SDL_AudioDeviceID,
 * kept header-free so native_state.c's include needs no SDL2.) */
u32 Platform_AudioProbeGetDevice(void);
#endif

#endif

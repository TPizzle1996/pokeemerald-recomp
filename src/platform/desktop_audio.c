#ifdef PLATFORM_SDL2

#ifdef __ANDROID__
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#include "global.h"
#include "platform.h"
#include "platform/desktop_audio.h"

HOST_DATA static SDL_AudioDeviceID sAudioDevice;

bool32 Platform_AudioInit(u32 sampleRate)
{
    SDL_AudioSpec want;
    SDL_AudioSpec have;

    SDL_memset(&want, 0, sizeof(want));
    want.freq = sampleRate;
    want.format = AUDIO_F32;
    want.channels = 2;
    want.samples = 1024;
    sAudioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (sAudioDevice == 0)
    {
        SDL_Log("Failed to open audio: %s", SDL_GetError());
        return FALSE;
    }
    if (have.format != AUDIO_F32 || have.channels != 2 || have.freq != (int)sampleRate)
    {
        SDL_Log("Unsupported SDL audio format (format=%x channels=%u rate=%d)",
                have.format, have.channels, have.freq);
        SDL_CloseAudioDevice(sAudioDevice);
        sAudioDevice = 0;
        return FALSE;
    }
    SDL_PauseAudioDevice(sAudioDevice, 0);
    return TRUE;
}

void Platform_QueueAudio(float *audioBuffer, s32 size)
{
    s32 floatCount;

    if (sAudioDevice == 0 || audioBuffer == NULL || size <= 0
     || size % (s32)sizeof(float) != 0)
        return;
    floatCount = size / sizeof(float);
    {
        float adjustedAudio[floatCount];
        float volume = Platform_GetSetting(PLATFORM_SETTING_VOLUME) / 10.0f;
        s32 i;
        for (i = 0; i < floatCount; i++)
            adjustedAudio[i] = audioBuffer[i] * volume;
        if (SDL_QueueAudio(sAudioDevice, adjustedAudio, size) < 0)
            SDL_Log("Failed to queue audio: %s", SDL_GetError());
    }
}

void Platform_AudioSetPaused(bool32 paused)
{
    if (sAudioDevice != 0)
        SDL_PauseAudioDevice(sAudioDevice, paused);
}

void Platform_AudioClearQueue(void)
{
    if (sAudioDevice != 0)
        SDL_ClearQueuedAudio(sAudioDevice);
}

void Platform_AudioShutdown(void)
{
    if (sAudioDevice != 0)
    {
        SDL_CloseAudioDevice(sAudioDevice);
        sAudioDevice = 0;
    }
}

#if defined(HARNESS_REAL_SDL_PROBE)
/* Headless desktop-audio probe only (tests/run_desktop_real_sdl_probe.sh):
 * exposes the opened device id so the probe can query the REAL SDL device
 * state (status / queued bytes) through the full desktop save/load
 * sequence. Compiled exclusively into the probe binary; never in
 * production builds. */
u32 Platform_AudioProbeGetDevice(void)
{
    return (u32)sAudioDevice;
}
#endif

#endif

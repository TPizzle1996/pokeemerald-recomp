/* R10 desktop-state audio regression: minimal SDL audio surface.
 *
 * The harness links the REAL src/platform/desktop_audio.c against this fake
 * SDL2 header + tests/emerald_audio_device_shim.c, so the production
 * Platform_AudioInit/QueueAudio/SetPaused/ClearQueue/Shutdown code runs
 * verbatim over a controllable device model with an ordered call trace.
 *
 * Only the declarations desktop_audio.c (and the scheduler-port in the
 * harness stub) reference are provided; anything else is out of scope.
 */
#ifndef SHIM_SDL2_SDL_H
#define SHIM_SDL2_SDL_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t Uint32;
typedef uint16_t Uint16;
typedef uint8_t Uint8;
typedef int32_t Sint32;
typedef Uint32 SDL_AudioDeviceID;
typedef Uint16 SDL_AudioFormat;

typedef struct SDL_AudioSpec
{
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    void *callback;
    void *userdata;
} SDL_AudioSpec;

#define AUDIO_F32 0x8120u

typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);

void *SDL_memset(void *ptr, int c, size_t len);

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const struct SDL_AudioSpec *desired,
                                      struct SDL_AudioSpec *obtained,
                                      int allowed_changes);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
int SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len);
void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
void SDL_Log(const char *fmt, ...);
const char *SDL_GetError(void);

/* Fake performance clock (the real scheduler's AudioFrameDue gate reads
 * these; the harness advances the clock deterministically). */
uint64_t SDL_GetPerformanceCounter(void);
uint64_t SDL_GetPerformanceFrequency(void);

#endif /* SHIM_SDL2_SDL_H */

/* R10 desktop-state audio regression: fake SDL audio device + ordered trace.
 *
 * Compiled ONLY into the state harness. Implements the SDL audio surface
 * tests/emerald_audio_device_shim.c/SDL.h declares with a controllable
 * device model:
 *
 *   - a single device that opens PAUSED (SDL's real initial state) and is
 *     un-paused by the production desktop_audio.c init call;
 *   - a queue that accumulates QueueAudio payload bytes and is emptied by
 *     ClearQueuedAudio;
 *   - an ordered textual trace of every operation, with harness-inserted
 *     phase markers, so the desktop load sequence can be asserted verbatim
 *     (task: "Record the ordered call trace").
 *
 * The model intentionally mirrors SDL semantics: OpenAudioDevice on an
 * already-open device fails (returns 0); all device calls on a closed or
 * foreign device id are no-ops.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "SDL2/SDL.h"

#define SHIM_DEVICE_ID 1u
#define AUDIO_TRACE_CAPACITY 8192u

static SDL_AudioDeviceID sDevice; /* 0 = closed, 1 = open */
static bool sPaused;
static uint64_t sQueuedBytes;
static char sTrace[AUDIO_TRACE_CAPACITY];
static size_t sTraceLength;

static void Trace(const char *op)
{
    size_t len = strlen(op);
    if (sTraceLength + len + 1u >= AUDIO_TRACE_CAPACITY)
        return;
    memcpy(sTrace + sTraceLength, op, len);
    sTraceLength += len;
    sTrace[sTraceLength++] = ';';
    sTrace[sTraceLength] = '\0';
}

/* ---------------------------- SDL audio API ---------------------------- */

void *SDL_memset(void *ptr, int c, size_t len)
{
    return memset(ptr, c, len);
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained,
                                      int allowed_changes)
{
    (void)device;
    (void)iscapture;
    (void)allowed_changes;
    if (sDevice != 0 || desired == NULL)
        return 0; /* already open / no spec: SDL fails */
    if (obtained != NULL)
        *obtained = *desired;
    sDevice = SHIM_DEVICE_ID;
    sPaused = true; /* SDL opens devices paused; init unpauses */
    sQueuedBytes = 0;
    Trace("open");
    return SHIM_DEVICE_ID;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on)
{
    if (dev != SHIM_DEVICE_ID || sDevice == 0)
        return;
    sPaused = pause_on != 0;
    Trace(sPaused ? "pause" : "unpause");
}

int SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len)
{
    (void)data;
    if (dev != SHIM_DEVICE_ID || sDevice == 0)
        return -1;
    sQueuedBytes += len;
    Trace("queue");
    return 0;
}

void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev)
{
    if (dev != SHIM_DEVICE_ID || sDevice == 0)
        return;
    sQueuedBytes = 0;
    Trace("clear");
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev)
{
    if (dev != SHIM_DEVICE_ID || sDevice == 0)
        return;
    sDevice = 0;
    sPaused = false;
    sQueuedBytes = 0;
    Trace("close");
}

void SDL_Log(const char *fmt, ...)
{
    (void)fmt;
}

const char *SDL_GetError(void)
{
    return "audio shim: no error";
}

/* --------------------------- fake performance clock -------------------- */

static uint64_t sClock;

uint64_t SDL_GetPerformanceCounter(void)
{
    return sClock;
}

uint64_t SDL_GetPerformanceFrequency(void)
{
    return 1000000000ull; /* ns ticks */
}

/* ------------------------------ harness API ---------------------------- */

/* Advance the fake clock by `nanoseconds` (the scheduler's AudioFrameDue
 * gate then deterministically reports one due frame per 1/60 s). */
void HarnessAudioClock_Advance(uint64_t nanoseconds)
{
    sClock += nanoseconds;
}

void HarnessAudioTraceReset(void)
{
    sTraceLength = 0;
    sTrace[0] = '\0';
    sDevice = 0;
    sPaused = false;
    sQueuedBytes = 0;
    sClock = 0;
}

/* Append a phase marker to the ordered trace (harness side). */
void HarnessAudioTraceMark(const char *tag)
{
    Trace(tag);
}

const char *HarnessAudioTraceGet(void)
{
    return sTrace;
}

bool HarnessDeviceOpen(void)
{
    return sDevice == SHIM_DEVICE_ID;
}

bool HarnessDevicePaused(void)
{
    return sPaused;
}

uint64_t HarnessDeviceQueuedBytes(void)
{
    return sQueuedBytes;
}

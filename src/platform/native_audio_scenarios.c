/* R12-E live audio scenario battery: 15 deterministic headless scenarios on
 * the REAL MP2K engine (no SDL devices; SDL_Init(0) utility mode), each
 * producing a SHA-256 digest of every mixer frame's PCM, a behavioral gate,
 * and (in canary mode) the R12-E live-pointer arena-residency assertions.
 *
 * The battery runs in TWO builds:
 *
 *   - migrated build (--native-audio-scenario-test): the native gSongTable
 *     rows carry ROM logical addresses, so every live pointer resolves into
 *     the published audio arena; the canary asserts arena residency.
 *   - compiled baseline / oracle build (--native-audio-scenario-oracle):
 *     data/sound_data.s flips back to the unconditional compiled
 *     song_table.inc, restoring the pre-cutover table whose rows are native
 *     link addresses; the canary is SKIPPED because compiled-image-resident
 *     pointers are legitimate there. Its digests are the golden: the
 *     migrated build must reproduce every scenario digest byte-for-byte
 *     (the consumed data is the same canonical bytes; only the resolution
 *     path differs).
 *
 * Any PCM delta between the two builds is a STOP condition per the R12-E
 * brief. Each scenario also prints informational stats (deepest pattern
 * stack, wave/phoneme leaf channels observed).
 *
 * Scenario list (brief §7):
 *   1.  looping route BGM          mus_route101 (359)
 *   2.  battle BGM                 mus_vs_wild (474)
 *   3.  BGM change                 359 -> 474 (m4aSongNumStartOrChange)
 *   4.  restart                    359 stop + restart
 *   5.  SFX                        se_use_item (1), completion asserted
 *   6.  fanfare                    mus_level_up (367), completion asserted
 *   7.  ph_* Bard phoneme          ph_nurse_held (608), completion + phoneme
 *                                  leaf residency
 *   8.  PATT-heavy                 mus_vs_aqua_magma (475, PATT 300)
 *   9.  GOTO-heavy                 mus_rg_vs_deoxys (551, GOTO 10/PATT 169,
 *                                  3 wave rows)
 *  10.  programmable-wave inst.    mus_link_contest_p4 (396, voicegroup046
 *                                  with 2 wave rows)
 *  11.  cry over BGM               PlayCry_Normal(25) over mus_route101;
 *                                  cry tone arena-resident, cry completes
 *  12.  overlapping players        BGM + SFX simultaneously
 *  13.  multi-player               all four main players at once
 *  14.  save/load mid-BGM          state v5 save/load during 359
 *  15.  save/load mid-pattern      state v5 save/load during 475
 *  16.  save/load mid-cry          real cry playback save/load: BGM + cry
 *                                  overlap, both slots, tone->transformed
 *                                  cry-table row, sample->canonical cry
 *                                  sample, gotoTarget round-trip, no
 *                                  hydration growth, per-frame PCM (R12-F)
 *  17.  quick-load stress          25 quick-save/load cycles with audio
 *                                  active (R12-F §15)
 *  18.  hydrate-many               387 songs started (>= 300 retained in
 *                                  the permanent cache), then a quick-slot
 *                                  save whose RESOURCE_SIDECAR record count
 *                                  is measured against the 4096 cap (R12-F
 *                                  §11 long-session sidecar gate)
 */
#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "gba/m4a_internal.h"
#include "sound_mixer.h"
#include "cgb_audio.h"
#include "gba/flash_internal.h"
#include "m4a.h"
#include "sound.h"
#include "platform/host_memory.h"
#include "platform/native_state.h"
#include "platform/desktop_state.h"
#include "platform/desktop_assets.h"
#include "platform/desktop_profiles.h"
#include "platform/desktop_runtime.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_audio_compat.h"
#include "emerald/resources/emerald_trainer_native_compat.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/sha256.h"

#include <time.h>   /* clock_gettime (R12-E §8 perf window) */
#include <unistd.h> /* mkstemp/write/close/unlink (damaged-pack fixture) */

extern void RunMixerFrame(void);
extern void m4aSoundVSync(void);
extern float audioBuffer[];

#define SCENARIO_FRAMES 240u
#define SCENARIO_HALF   120u

/* R12-E §8 perf gate (plan gate 12): a fixed window of mixer frames is
 * wall-clocked in BOTH builds (oracle = compiled-baseline resolution, test =
 * arena resolution) and the test build asserts the oracle baseline with the
 * plan's wide margin. Best-of-batches is robust to scheduler noise. */
#define PERF_WINDOW_FRAMES  4000u
#define PERF_WINDOW_BATCHES 3u
#define PERF_MARGIN_RATIO   1.05

struct ScenarioState
{
    struct SoundInfo *soundInfo;
    uintptr_t bssStart;
    uintptr_t bssEnd;
    unsigned char *region;   /* the walked audio game_bss span */
    unsigned char *pristine; /* region as left by m4aSoundInit (no song) */
    size_t regionSize;
    unsigned char pristineIo[0x400]; /* REG_BASE left by m4aSoundInit */
    struct Gen3Sha256Context sha; /* per-scenario PCM digest (all frames) */
    uint8_t controlDigest[SCENARIO_FRAMES][32]; /* per-frame control PCM digests */
    u32 freqHist[SCENARIO_FRAMES * 2][3]; /* per-frame ch1/2/3 freq regs */
    bool32 runCanary;
    u32 maxPatternLevel;   /* deepest pattern stack observed this scenario */
    bool32 waveChannelSeen;
    bool32 phonemeChannelSeen;
};

typedef const char *(*ScenarioFn)(void);

struct ScenarioDef
{
    const char *name;
    ScenarioFn fn;
};

static struct ScenarioState sScenario;

/* One audible frame's merged output window (stereo floats). The real loop
 * (main.c VBlankIntr) runs m4aSoundVSync after RunMixerFrame; the digest
 * must cover the merged audioBuffer, NOT pcmBuffer — CGB synthesis writes
 * gb.outBuffer and only the m4aSoundVSync merge reaches audioBuffer, so a
 * CGB-only song (se_use_item) would otherwise hash as silence. */
static u32 ScenarioFrameSamples(void)
{
    /* The SoundInfo and SoundMixerState layouts alias the same memory in the
     * native build; pcmSamplesPerVBlank sits at the same offset the mixer
     * reads as samplesPerFrame (m4aSoundVSync doubles it for stereo). */
    return (u32)sScenario.soundInfo->pcmSamplesPerVBlank * 2u;
}

static void ScenarioFrame(void)
{
    RunMixerFrame();
    m4aSoundVSync();
    Gen3Sha256_Update(&sScenario.sha, (const void *)audioBuffer,
                      ScenarioFrameSamples() * sizeof(float));
}

/* Re-establish the pristine engine state: pristine audio region + fresh cgb
 * emulator (the cgb state is host .bss, not part of any state slice). The GBA
 * I/O block (REG_BASE, where the CGB NR registers live) is platform .bss,
 * outside game_bss, so its residue must be restored explicitly — otherwise
 * each scenario run starts from the residue the previous run left behind,
 * which makes control/test runs of one scenario non-identical. */
static void ScenarioReset(void)
{
    memcpy(sScenario.region, sScenario.pristine, sScenario.regionSize);
    memcpy(REG_BASE, sScenario.pristineIo, sizeof(sScenario.pristineIo));
    cgb_audio_init(42060);
    sScenario.maxPatternLevel = 0;
    sScenario.waveChannelSeen = FALSE;
    sScenario.phonemeChannelSeen = FALSE;
}

static bool32 ScenarioAnyChannelActive(void)
{
    u32 i;
    for (i = 0; i < MAX_DIRECTSOUND_CHANNELS; i++)
    {
        const struct SoundChannel *ch = &sScenario.soundInfo->chans[i];
        if ((ch->statusFlags & 0xC7) != 0 && ch->envelopeVolume != 0)
            return TRUE;
    }
    return FALSE;
}

static bool32 ScenarioPcmNonzero(void)
{
    u32 i;
    u32 samples = ScenarioFrameSamples();
    for (i = 0; i < samples; i += 97)
    {
        if (audioBuffer[i] != 0.0f)
            return TRUE;
    }
    return FALSE;
}

static struct MusicPlayerInfo *ScenarioPlayer(u32 index)
{
    return (struct MusicPlayerInfo *)HostResolveGbaAddr(gMPlayTable[index].info);
}

/* The (single) main player currently running a song, or NULL. */
static struct MusicPlayerInfo *ScenarioFindPlaying(void)
{
    u32 j;
    for (j = 0; j < MAX_MUSIC_PLAYERS; j++)
    {
        if (ScenarioPlayer(j)->songHeader != NULL)
            return ScenarioPlayer(j);
    }
    return NULL;
}

/* Engine liveness: a player is playing only while it has a track with flags
 * set. MPlayStart marks started tracks; TrackStop zeroes the flags at song
 * end. mplayInfo->songHeader is NOT cleared at song end (MPlayMain keeps the
 * pointer), so songHeader-based counting cannot distinguish a finished song
 * from a playing one. */
static bool32 ScenarioPlayerLive(const struct MusicPlayerInfo *p)
{
    u32 t;
    for (t = 0; t < p->trackCount; t++)
    {
        if (p->tracks[t].flags != 0)
            return TRUE;
    }
    return FALSE;
}

/* Count main players with live tracks; optionally record them. */
static u32 ScenarioLivePlayers(struct MusicPlayerInfo **out, u32 cap)
{
    u32 j;
    u32 n = 0;
    for (j = 0; j < MAX_MUSIC_PLAYERS; j++)
    {
        if (!ScenarioPlayerLive(ScenarioPlayer(j)))
            continue;
        if (out != NULL && n < cap)
            out[n] = ScenarioPlayer(j);
        n++;
    }
    return n;
}

static const char *ScenarioCanaryMain(void)
{
    struct MusicPlayerInfo *live[4];
    u32 n;
    u32 i;
    if (!sScenario.runCanary)
        return NULL;
    n = ScenarioLivePlayers(live, 4);
    for (i = 0; i < n; i++)
    {
        const char *failure = NativeAudioCheckLiveCanary(
            live[i], sScenario.soundInfo, FALSE);
        if (failure != NULL)
            return failure;
    }
    return NULL;
}

/* Cry players: bytecode is BSS by design (allowBssBytecode), the cry tone
 * must still be arena-resident. */
static const char *ScenarioCanaryCries(void)
{
    u32 j;
    if (!sScenario.runCanary)
        return NULL;
    for (j = 0; j < MAX_POKEMON_CRIES; j++)
    {
        struct MusicPlayerInfo *cry = &gPokemonCryMusicPlayers[j];
        if (cry->songHeader == NULL)
            continue;
        {
            const char *failure = NativeAudioCheckLiveCanary(
                cry, sScenario.soundInfo, TRUE);
            if (failure != NULL)
                return failure;
        }
    }
    return NULL;
}

/* Track pattern-stack depth and wave/phoneme leaf channels after every
 * mixer frame of a scenario. */
static void ScenarioObserve(void)
{
    struct MusicPlayerInfo *live[4];
    u32 n;
    u32 i;
    u32 j;
    u32 w;
    const uint8_t *arenaBase;
    size_t arenaSize;

    n = ScenarioLivePlayers(live, 4);
    for (i = 0; i < n; i++)
    {
        for (j = 0; j < live[i]->trackCount; j++)
        {
            u32 level = live[i]->tracks[j].patternLevel;
            if (level > sScenario.maxPatternLevel)
                sScenario.maxPatternLevel = level;
        }
    }
    if (!sScenario.runCanary
     || !EmeraldAudioCompat_GetArena(&arenaBase, &arenaSize))
        return;
    for (i = 0; i < MAX_DIRECTSOUND_CHANNELS; i++)
    {
        const struct SoundChannel *ch = &sScenario.soundInfo->chans[i];
        uintptr_t wav;
        if ((ch->statusFlags & 0xC7) == 0 || ch->wav == NULL)
            continue;
        wav = (uintptr_t)ch->wav;
        for (w = 0; w < EMERALD_AUDIO_WAVE_COUNT && !sScenario.waveChannelSeen;
             w++)
        {
            size_t offset;
            size_t size;
            char name[48];
            snprintf(name, sizeof(name), "emerald:audio/wave/programmable/%u", w);
            if (EmeraldAudioCompat_GetLeafSpan(name, &offset, &size)
             && wav >= (uintptr_t)(arenaBase + offset)
             && wav < (uintptr_t)(arenaBase + offset + size))
                sScenario.waveChannelSeen = TRUE;
        }
        for (w = 1; w <= EMERALD_AUDIO_PHONEME_COUNT
             && !sScenario.phonemeChannelSeen; w++)
        {
            size_t offset;
            size_t size;
            char name[48];
            snprintf(name, sizeof(name), "emerald:audio/sample/phoneme/%u", w);
            if (EmeraldAudioCompat_GetLeafSpan(name, &offset, &size)
             && wav >= (uintptr_t)(arenaBase + offset)
             && wav < (uintptr_t)(arenaBase + offset + size))
                sScenario.phonemeChannelSeen = TRUE;
        }
    }
    /* Programmable-wave rows dispatch to CgbChannels (cgbType = type & 7, so
     * the type-3 wave channel sits at cgbChans[2]); ply_note writes the
     * instrument's WaveData into chan->wav, which aliases CgbChannel's
     * wavePointer slot. These never appear in the DirectSound chans array. */
    if (sScenario.soundInfo->cgbChans != NULL)
    {
        u32 c;
        for (c = 0; c < 4 && !sScenario.waveChannelSeen; c++)
        {
            const struct MixerSource *ch =
                (const struct MixerSource *)&sScenario.soundInfo->cgbChans[c];
            uintptr_t wav;
            if ((ch->status & 0xC7) == 0 || ch->wav == NULL)
                continue;
            wav = (uintptr_t)ch->wav;
            for (w = 0; w < EMERALD_AUDIO_WAVE_COUNT; w++)
            {
                size_t offset;
                size_t size;
                char name[48];
                snprintf(name, sizeof(name),
                         "emerald:audio/wave/programmable/%u", w);
                if (EmeraldAudioCompat_GetLeafSpan(name, &offset, &size)
                 && wav >= (uintptr_t)(arenaBase + offset)
                 && wav < (uintptr_t)(arenaBase + offset + size))
                    sScenario.waveChannelSeen = TRUE;
            }
        }
    }
}

static void ScenarioPrint(const struct ScenarioDef *def,
                          const uint8_t digest[32])
{
    u32 i;
    fprintf(stdout, "SCENARIO %s ", def->name);
    for (i = 0; i < 32; i++)
        fprintf(stdout, "%02x", digest[i]);
    fprintf(stdout, " pat=%u wave=%d phoneme=%d\n",
            sScenario.maxPatternLevel, (int)sScenario.waveChannelSeen,
            (int)sScenario.phonemeChannelSeen);
    fflush(stdout);
}

/* ---------- the 18 scenarios ---------- */

static const char *ScenarioLoopingBgm(void)
{
    u32 frame;
    m4aSongNumStart(359);
    for (frame = 0; frame < SCENARIO_FRAMES; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    if (!ScenarioAnyChannelActive() || !ScenarioPcmNonzero())
        return "route BGM produced no audio";
    if (ScenarioFindPlaying() == NULL)
        return "route BGM stopped unexpectedly";
    return ScenarioCanaryMain();
}

static const char *ScenarioBattleBgm(void)
{
    u32 frame;
    m4aSongNumStart(474);
    for (frame = 0; frame < SCENARIO_FRAMES; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    if (!ScenarioAnyChannelActive() || !ScenarioPcmNonzero())
        return "battle BGM produced no audio";
    if (ScenarioFindPlaying() == NULL)
        return "battle BGM stopped unexpectedly";
    return ScenarioCanaryMain();
}

static const char *ScenarioBgmChange(void)
{
    struct MusicPlayerInfo *player;
    u8 *partBefore;
    u32 frame;
    m4aSongNumStart(359);
    for (frame = 0; frame < SCENARIO_HALF; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    player = ScenarioFindPlaying();
    if (player == NULL)
        return "no BGM player before change";
    partBefore = player->songHeader->part[0];
    m4aSongNumStartOrChange(474); /* both songs run on player 0 */
    for (frame = 0; frame < SCENARIO_HALF; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    if (ScenarioFindPlaying() == NULL)
        return "no player after BGM change";
    if (player->songHeader->part[0] == partBefore)
        return "BGM change did not replace the song stream";
    return ScenarioCanaryMain();
}

static const char *ScenarioRestart(void)
{
    struct MusicPlayerInfo *player;
    u32 frame;
    m4aSongNumStart(359);
    for (frame = 0; frame < SCENARIO_HALF; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    m4aSongNumStop(359);
    for (frame = 0; frame < 30; frame++)
        ScenarioFrame();
    player = ScenarioFindPlaying();
    if (player == NULL || !(player->status & MUSICPLAYER_STATUS_PAUSE))
        return "song not stopped after m4aSongNumStop";
    m4aSongNumStart(359);
    for (frame = 0; frame < SCENARIO_HALF; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    player = ScenarioFindPlaying();
    if (player == NULL || (player->status & MUSICPLAYER_STATUS_PAUSE))
        return "song did not restart";
    if (!ScenarioAnyChannelActive() || !ScenarioPcmNonzero())
        return "restarted song produced no audio";
    return ScenarioCanaryMain();
}

static const char *ScenarioSfx(void)
{
    u32 frame;
    bool32 audioSeen = FALSE;
    m4aSongNumStart(1); /* se_use_item, player 1: a short CGB one-shot */
    ScenarioFrame();
    ScenarioFrame();
    for (frame = 0; frame < SCENARIO_HALF; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
        if (!audioSeen && (ScenarioAnyChannelActive() || ScenarioPcmNonzero()))
            audioSeen = TRUE;
        if (audioSeen && !ScenarioPlayerLive(ScenarioPlayer(1)))
            break; /* completed; drain the fixed digest window below */
    }
    if (!audioSeen)
        return "SFX produced no audio";
    for (; frame < SCENARIO_FRAMES; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    if (ScenarioPlayerLive(ScenarioPlayer(1)))
        return "SFX did not complete";
    return ScenarioCanaryMain();
}

static const char *ScenarioFanfare(void)
{
    u32 frame;
    bool32 audioSeen = FALSE;
    m4aSongNumStart(367); /* mus_level_up, player 2 */
    for (frame = 0; frame < SCENARIO_FRAMES; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
        if (!audioSeen && (ScenarioAnyChannelActive() || ScenarioPcmNonzero()))
            audioSeen = TRUE;
    }
    if (!audioSeen)
        return "fanfare produced no audio";
    return ScenarioCanaryMain();
}

static const char *ScenarioPhoneme(void)
{
    u32 frame;
    bool32 audioSeen = FALSE;
    m4aSongNumStart(608); /* ph_nurse_held, player 2 */
    for (frame = 0; frame < SCENARIO_FRAMES; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
        if (!audioSeen && (ScenarioAnyChannelActive() || ScenarioPcmNonzero()))
            audioSeen = TRUE;
    }
    if (!audioSeen)
        return "phoneme song produced no audio";
    if (sScenario.runCanary && !sScenario.phonemeChannelSeen)
        return "no phoneme leaf sample was live";
    return ScenarioCanaryMain();
}

static const char *ScenarioPattHeavy(void)
{
    u32 frame;
    m4aSongNumStart(475); /* mus_vs_aqua_magma (PATT 300 / GOTO 8) */
    for (frame = 0; frame < SCENARIO_FRAMES; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    if (sScenario.maxPatternLevel == 0)
        return "PATT never entered";
    if (ScenarioFindPlaying() == NULL)
        return "PATT-heavy song stopped unexpectedly";
    return ScenarioCanaryMain();
}

static const char *ScenarioGotoHeavy(void)
{
    u32 frame;
    m4aSongNumStart(551); /* mus_rg_vs_deoxys (GOTO 10 / PATT 169, 3 wave rows) */
    for (frame = 0; frame < SCENARIO_FRAMES * 4; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    if (sScenario.maxPatternLevel == 0)
        return "GOTO-heavy song never entered a pattern";
    if (ScenarioFindPlaying() == NULL)
        return "GOTO-heavy song stopped unexpectedly";
    return ScenarioCanaryMain();
}

static const char *ScenarioProgrammableWave(void)
{
    u32 frame;
    m4aSongNumStart(396); /* mus_link_contest_p4 (voicegroup046, 2 wave rows) */
    for (frame = 0; frame < SCENARIO_FRAMES * 4; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    if (sScenario.runCanary && !sScenario.waveChannelSeen)
        return "no programmable-wave leaf was live";
    if (ScenarioFindPlaying() == NULL)
        return "wave song stopped unexpectedly";
    return ScenarioCanaryMain();
}

static const char *ScenarioCryOverBgm(void)
{
    u32 frame;
    u32 j;
    bool32 crySeen = FALSE;
    m4aSongNumStart(359);
    for (frame = 0; frame < 60; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    PlayCry_Normal(25, 0); /* Pikachu, center */
    for (frame = 0; frame < SCENARIO_FRAMES; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
        for (j = 0; j < MAX_POKEMON_CRIES; j++)
        {
            if (gPokemonCryMusicPlayers[j].songHeader != NULL)
                crySeen = TRUE;
        }
    }
    if (!crySeen)
        return "cry never started";
    for (j = 0; j < MAX_POKEMON_CRIES; j++)
    {
        const struct MusicPlayerTrack *track = &gPokemonCryTracks[j * 2];
        if (track->flags != 0 || track[1].flags != 0)
            return "cry did not complete";
    }
    {
        const char *failure = ScenarioCanaryCries();
        if (failure != NULL)
            return failure;
    }
    return ScenarioCanaryMain();
}

static const char *ScenarioOverlapping(void)
{
    struct MusicPlayerInfo *live[4];
    u32 n;
    u32 frame;
    bool32 bothLive = FALSE;
    m4aSongNumStart(359);
    m4aSongNumStart(1); /* se_use_item */
    for (frame = 0; frame < SCENARIO_FRAMES; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
        n = ScenarioLivePlayers(live, 4);
        if (n >= 2)
            bothLive = TRUE;
    }
    if (!bothLive)
        return "overlapping players never coexisted";
    return ScenarioCanaryMain();
}

static const char *ScenarioMultiPlayer(void)
{
    struct MusicPlayerInfo *live[4];
    u32 n;
    u32 frame;
    bool32 allFour = FALSE;
    m4aSongNumStart(359); /* player 0: looping BGM */
    m4aSongNumStart(1);   /* player 1: se_use_item (completes) */
    m4aSongNumStart(367); /* player 2: mus_level_up (completes) */
    m4aSongNumStart(85);  /* player 3: se_rain (loops) */
    for (frame = 0; frame < SCENARIO_FRAMES; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
        n = ScenarioLivePlayers(live, 4);
        if (n >= 4)
            allFour = TRUE;
    }
    if (!allFour)
        return "all four players were never live";
    n = ScenarioLivePlayers(live, 4);
    if (n != 2 || ScenarioPlayer(0)->songHeader == NULL
     || ScenarioPlayer(3)->songHeader == NULL)
        return "multi-player completion state wrong (want BGM + se_rain live)";
    return ScenarioCanaryMain();
}

static const char *ScenarioSaveLoadMid(u16 song, bool32 requirePattern,
                                       u32 half)
{
    struct Gen3Sha256Context controlSha;
    struct Gen3Sha256Context testSha;
    uint8_t digestControl[32];
    uint8_t digestTest[32];
    u32 frame;

    /* CONTROL: 2*half uninterrupted frames; the last half hashed separately
     * as the golden for the post-restore comparison. */
    ScenarioReset();
    m4aSongNumStart(song);
    Gen3Sha256_Init(&controlSha);
    for (frame = 0; frame < half * 2; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
        if (frame >= half)
        {
            struct Gen3Sha256Context perFrame;
            Gen3Sha256_Update(&controlSha, (const void *)audioBuffer,
                              ScenarioFrameSamples() * sizeof(float));
            Gen3Sha256_Init(&perFrame);
            Gen3Sha256_Update(&perFrame, (const void *)audioBuffer,
                              ScenarioFrameSamples() * sizeof(float));
            Gen3Sha256_Final(&perFrame, sScenario.controlDigest[frame - half]);
            sScenario.freqHist[frame - half][0] = REG_SOUND1CNT_X & 0x7FF;
            sScenario.freqHist[frame - half][1] = REG_SOUND2CNT_H & 0x7FF;
            sScenario.freqHist[frame - half][2] = REG_SOUND3CNT_X & 0x7FF;
        }
    }
    Gen3Sha256_Final(&controlSha, digestControl);
    if (requirePattern && sScenario.maxPatternLevel == 0)
        return "control: PATT never entered";
    if (ScenarioFindPlaying() == NULL)
        return "control: song not playing at save point";

    /* TEST: half frames, save, destroy, load, half frames. */
    ScenarioReset();
    m4aSongNumStart(song);
    for (frame = 0; frame < half; frame++)
    {
        ScenarioFrame();
        ScenarioObserve();
    }
    if (requirePattern && sScenario.maxPatternLevel == 0)
        return "no PATT before save";
    if (Platform_StateSave(PLATFORM_STATE_QUICK_SLOT) == PLATFORM_STATE_OPERATION_FAILED)
        return "state save failed";
    memset(sScenario.region, 0xA5, sScenario.regionSize);
    if (Platform_StateLoad(PLATFORM_STATE_QUICK_SLOT) != PLATFORM_STATE_OPERATION_OK)
        return "state load failed";
    if (ScenarioFindPlaying() == NULL)
        return "song not playing after load";
    {
        const char *failure = ScenarioCanaryMain();
        if (failure != NULL)
            return failure;
    }
    if (requirePattern)
    {
        sScenario.maxPatternLevel = 0;
        ScenarioObserve();
        if (sScenario.maxPatternLevel == 0)
            return "pattern stack not restored by load";
    }
    Gen3Sha256_Init(&testSha);
    for (frame = 0; frame < half; frame++)
    {
        struct Gen3Sha256Context perFrame;
        uint8_t frameDigest[32];
        ScenarioFrame();
        ScenarioObserve();
        Gen3Sha256_Update(&testSha, (const void *)audioBuffer,
                          ScenarioFrameSamples() * sizeof(float));
        Gen3Sha256_Init(&perFrame);
        Gen3Sha256_Update(&perFrame, (const void *)audioBuffer,
                          ScenarioFrameSamples() * sizeof(float));
        Gen3Sha256_Final(&perFrame, frameDigest);
        {
            u32 f0 = REG_SOUND1CNT_X & 0x7FF;
            u32 f1 = REG_SOUND2CNT_H & 0x7FF;
            u32 f2 = REG_SOUND3CNT_X & 0x7FF;
            if (f0 != sScenario.freqHist[frame][0]
             || f1 != sScenario.freqHist[frame][1]
             || f2 != sScenario.freqHist[frame][2])
                return "post-restore CGB freq regs diverge from control";
        }
        if (memcmp(frameDigest, sScenario.controlDigest[frame], 32) != 0)
            return "post-restore PCM diverges from control";
    }
    Gen3Sha256_Final(&testSha, digestTest);
    if (memcmp(digestControl, digestTest, 32) != 0)
        return "post-restore PCM digest diverges from control";
    return ScenarioCanaryMain();
}

static const char *ScenarioSaveLoadBgm(void)
{
    return ScenarioSaveLoadMid(359, FALSE, SCENARIO_HALF);
}

static const char *ScenarioSaveLoadPattern(void)
{
    /* mus_vs_aqua_magma enters its first pattern after the 120-frame point
     * (see patt-heavy), so the save must happen later than the default. */
    return ScenarioSaveLoadMid(475, TRUE, SCENARIO_HALF * 2);
}

/* R12-F §15: 25-cycle quick save/load stress with audio active. Every
 * cycle runs a short audible stretch, takes a quick-slot save (live audio
 * pointers captured each time), scrubs the entire audio state, and loads -
 * so the pre-flight trainer + audio republish and the capture/restore of
 * live resource pointers each run 25 times. The BGM must survive every
 * cycle and keep producing PCM; the aggregate scenario digest covers the
 * whole stress. */
static const char *ScenarioQuickLoadStress(void)
{
    u32 cycle;
    u32 framesWithAudio = 0u;

    ScenarioReset();
    m4aSongNumStart(359);
    for (cycle = 0; cycle < 25; cycle++)
    {
        u32 frame;
        for (frame = 0; frame < 8; frame++)
        {
            ScenarioFrame();
            ScenarioObserve();
        }
        if (ScenarioFindPlaying() == NULL)
            return "stress: BGM stopped mid-stress";
        if (ScenarioPcmNonzero())
            framesWithAudio++;
        if (Platform_StateSave(PLATFORM_STATE_QUICK_SLOT)
                == PLATFORM_STATE_OPERATION_FAILED)
            return "stress: save failed mid-stress";
        memset(sScenario.region, 0xA5, sScenario.regionSize);
        if (Platform_StateLoad(PLATFORM_STATE_QUICK_SLOT)
                != PLATFORM_STATE_OPERATION_OK)
            return "stress: load failed mid-stress";
        if (ScenarioFindPlaying() == NULL)
            return "stress: BGM not playing after cycle";
    }
    if (framesWithAudio == 0u)
        return "stress: no audible PCM observed across 25 cycles";
    return ScenarioCanaryMain();
}

static u32 ScenarioReadLe32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8)
         | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* R12-F §11: long-session sidecar measurement gate. A marathon session -
 * 387 distinct songs started (ids 0..386, all real gSongTable rows; >= 300
 * distinct headers in BOTH table forms: 387 in the native ROM-address
 * table, 309 in the compiled oracle table) so the permanent hydrated cache
 * (in-band game_bss data, never sidecar records) holds >= 300 entries with
 * the last song still live - then the state is saved to the quick slot and
 * the SAVED FILE's RESOURCE_SIDECAR record count is counted from disk: the
 * container the 4096 cap actually bounds. A state that exceeded the cap
 * could never save, so the measurement is made at a legitimate reachable
 * point. If the cap is ever exceeded the scenario stops with the exact
 * measurement (the cap is raised ONLY from such evidence, never
 * proactively). */
static const char *ScenarioHydrateMany(void)
{
    static const u32 kHydrateSongs = 387u; /* ids 0..386: real gSongTable */
    static const u32 kHydratedPin = 300u;  /* brief §11: >= 300 hydrated */
    static const u32 kSidecarCap = 4096u;  /* NATIVE_STATE_MAX_RESOURCE_RECORDS */
    u32 hydratedPre = M4aGetHydratedSongHeaderCount();
    u32 hydrated;
    u32 sidecarCount = 0u;
    bool32 sidecarSeen = FALSE;
    char path[1024];
    u32 i;

    ScenarioReset();
    for (i = 0; i < kHydrateSongs; i++)
    {
        m4aSongNumStart((u16)i);
        ScenarioFrame();
        ScenarioFrame();
        ScenarioFrame();
        ScenarioFrame();
    }
    hydrated = M4aGetHydratedSongHeaderCount() - hydratedPre;
    if (hydrated < kHydratedPin)
        return "hydration: fewer than 300 song headers retained in the "
               "permanent cache";
    if (Platform_StateSave(PLATFORM_STATE_QUICK_SLOT)
            == PLATFORM_STATE_OPERATION_FAILED)
        return "hydration: state save failed";
    if (!Platform_ProfileGetStateFilePath(PLATFORM_STATE_QUICK_SLOT,
                                          PLATFORM_STATE_FILE_NATIVE,
                                          path, sizeof(path)))
        return "hydration: no state file path";
    /* Count RESOURCE_SIDECAR (tag 14) records in the saved container:
     * fixed 64-byte records, u32 count first (native_state.c v5). */
    {
        FILE *file = fopen(path, "rb");
        long fileSize;
        u8 *bytes;
        u32 headerSize, sectionCount;

        if (file == NULL)
            return "hydration: state file unreadable";
        fseek(file, 0, SEEK_END);
        fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);
        bytes = malloc((size_t)fileSize);
        if (bytes == NULL
         || fread(bytes, 1, (size_t)fileSize, file) != (size_t)fileSize)
        {
            free(bytes);
            fclose(file);
            return "hydration: state file unreadable";
        }
        fclose(file);
        if (fileSize < 44 || ScenarioReadLe32(bytes) != 0x4E535431u
         || ScenarioReadLe32(bytes + 4) != 5u)
        {
            free(bytes);
            return "hydration: saved state is not a v5 container";
        }
        headerSize = ScenarioReadLe32(bytes + 8);
        sectionCount = ScenarioReadLe32(bytes + 16);
        for (i = 0; i < sectionCount; i++)
        {
            const u8 *section = bytes + headerSize + i * 12u;
            u32 tag = ScenarioReadLe32(section);
            u32 size = ScenarioReadLe32(section + 4);

            if (tag == 14u)
            {
                sidecarCount = (size - 4u) / 64u;
                sidecarSeen = TRUE;
                break;
            }
        }
        free(bytes);
    }
    if (!sidecarSeen)
        return "hydration: no resource sidecar in saved state";
    if (sidecarCount > kSidecarCap)
    {
        static char msg[192];
        snprintf(msg, sizeof(msg),
                 "hydration: sidecar %u records exceeds the %u cap with %u "
                 "songs hydrated (fixed 64-byte records, count field first; "
                 "one record per live resource-backed pointer - the cap is "
                 "raised only from this exact measurement)",
                 sidecarCount, kSidecarCap, hydrated);
        return msg;
    }
    fprintf(stdout, "HYDRATION sidecar=%u records, %u songs hydrated, "
                    "cap %u\n", sidecarCount, hydrated, kSidecarCap);
    return ScenarioCanaryMain();
}

/* R12-F §10/§11: REAL cry playback save/load. BGM + cry overlap is live at
 * the save point; both slots round-trip. Post-load: the cry tone is the
 * SAME transformed-zone cry-table row (tone == loader arena row, §11), its
 * sample is the canonical cry sample, the bytecode gotoTarget (the BSS
 * image address of the cry's own cont field) round-trips and still resolves,
 * the hydrated-song cache did NOT grow (the load re-derives pointers; it
 * must never hydrate), and the post-load PCM matches the uninterrupted
 * control per frame (CGB freq regs + per-frame digests). */
static const char *ScenarioSaveLoadCry(void)
{
    static const u8 slots[2] = {PLATFORM_STATE_QUICK_SLOT, 1};
    u32 slotIndex;

    /* CONTROL: one uninterrupted run (BGM + cry), storing the per-frame
     * digests + CGB freq regs of the last half as the post-restore golden.
     * (Per-frame comparison is strictly stronger than an aggregate digest:
     * an early divergence cannot be masked by a later equal-sum window.) */
    {
        u32 frame;

        ScenarioReset();
        m4aSongNumStart(359);
        for (frame = 0; frame < 60; frame++)
        {
            ScenarioFrame();
            ScenarioObserve();
        }
        PlayCry_Normal(25, 0); /* Pikachu over the looping BGM */
        for (frame = 0; frame < SCENARIO_HALF * 2; frame++)
        {
            struct Gen3Sha256Context perFrame;
            ScenarioFrame();
            ScenarioObserve();
            if (frame >= SCENARIO_HALF)
            {
                Gen3Sha256_Init(&perFrame);
                Gen3Sha256_Update(&perFrame, (const void *)audioBuffer,
                                  ScenarioFrameSamples() * sizeof(float));
                Gen3Sha256_Final(&perFrame,
                                 sScenario.controlDigest[frame - SCENARIO_HALF]);
                sScenario.freqHist[frame - SCENARIO_HALF][0] =
                    REG_SOUND1CNT_X & 0x7FF;
                sScenario.freqHist[frame - SCENARIO_HALF][1] =
                    REG_SOUND2CNT_H & 0x7FF;
                sScenario.freqHist[frame - SCENARIO_HALF][2] =
                    REG_SOUND3CNT_X & 0x7FF;
            }
        }
    }

    for (slotIndex = 0; slotIndex < 2; slotIndex++)
    {
        u32 frame;
        u32 j;
        u32 cryPlayer = MAX_POKEMON_CRIES;
        u32 hydratedPre;
        struct MusicPlayerInfo *bgm;
        struct SongHeader *bgmHeaderPre;
        struct ToneData *tonePre;
        struct ToneData *tonePost;
        struct WaveData *samplePre;
        GbaAddr gotoTargetPre;
        GbaAddr gotoTargetPost;

        /* TEST: identical timeline, save at the halfway point with BGM +
         * cry both live, scrub the audio state, load. */
        ScenarioReset();
        m4aSongNumStart(359);
        for (frame = 0; frame < 60; frame++)
        {
            ScenarioFrame();
            ScenarioObserve();
        }
        PlayCry_Normal(25, 0);
        for (frame = 0; frame < SCENARIO_HALF; frame++)
        {
            ScenarioFrame();
            ScenarioObserve();
        }
        bgm = ScenarioFindPlaying();
        if (bgm == NULL)
            return "cry save: BGM not playing at save point";
        bgmHeaderPre = bgm->songHeader;
        for (j = 0; j < MAX_POKEMON_CRIES; j++)
        {
            if (gPokemonCryMusicPlayers[j].songHeader != NULL)
            {
                cryPlayer = j;
                break;
            }
        }
        if (cryPlayer == MAX_POKEMON_CRIES)
            return "cry save: cry not live at save point";
        tonePre = gPokemonCryMusicPlayers[cryPlayer].songHeader->tone;
        if (tonePre == NULL)
            return "cry save: cry tone is NULL";
        samplePre = tonePre->wav;
        gotoTargetPre = gPokemonCrySongs[cryPlayer].bytecode.gotoTarget;
        hydratedPre = M4aGetHydratedSongHeaderCount();
        if (Platform_StateSave(slots[slotIndex]) == PLATFORM_STATE_OPERATION_FAILED)
            return "cry save: state save failed";
        memset(sScenario.region, 0xA5, sScenario.regionSize);
        if (Platform_StateLoad(slots[slotIndex]) != PLATFORM_STATE_OPERATION_OK)
            return "cry save: state load failed";
        if (M4aGetHydratedSongHeaderCount() != hydratedPre)
            return "cry save: load grew the hydrated song cache (rehydration)";
        if (M4aGetHydratedSongHeaderCount() > 610u)
            return "cry save: hydrated song count exceeds the 610 reachability bound";
        bgm = ScenarioFindPlaying();
        if (bgm == NULL)
            return "cry save: BGM not playing after load";
        if (bgm->songHeader != bgmHeaderPre)
            return "cry save: BGM songHeader not re-derived to the same hydrated slot";
        cryPlayer = MAX_POKEMON_CRIES;
        for (j = 0; j < MAX_POKEMON_CRIES; j++)
        {
            if (gPokemonCryMusicPlayers[j].songHeader != NULL)
            {
                cryPlayer = j;
                break;
            }
        }
        if (cryPlayer == MAX_POKEMON_CRIES)
            return "cry save: cry not live after load";
        tonePost = gPokemonCryMusicPlayers[cryPlayer].songHeader->tone;
        if (tonePost != tonePre)
            return "cry save: cry tone not re-derived to the same arena row";
        if (gPokemonCryMusicPlayers[cryPlayer].tone != tonePost)
            return "cry save: cry player tone differs from the restored header tone";
        if (!EmeraldAudioCompat_ContainsTransformedPointer((uintptr_t)tonePost))
            return "cry save: cry tone is NOT a transformed-zone cry-table row";
        if (tonePost->wav != samplePre)
            return "cry save: cry sample not re-derived to the same arena sample";
        if (!EmeraldAudioCompat_ContainsCanonicalPointer(
                (uintptr_t)tonePost->wav))
            return "cry save: cry sample is NOT canonical-zone resident";
        gotoTargetPost = gPokemonCrySongs[cryPlayer].bytecode.gotoTarget;
        if (gotoTargetPost != gotoTargetPre)
            return "cry save: cry bytecode gotoTarget did not round-trip";
        if (gotoTargetPost != 0u
         && HostResolveGbaAddr(gotoTargetPost)
                != (void *)&gPokemonCrySongs[cryPlayer].bytecode.cont)
            return "cry save: cry bytecode gotoTarget does not resolve to its cont";
        {
            const char *failure = ScenarioCanaryMain();
            if (failure != NULL)
                return failure;
            failure = ScenarioCanaryCries();
            if (failure != NULL)
                return failure;
        }
        for (frame = 0; frame < SCENARIO_HALF; frame++)
        {
            struct Gen3Sha256Context perFrame;
            uint8_t frameDigest[32];
            ScenarioFrame();
            ScenarioObserve();
            Gen3Sha256_Init(&perFrame);
            Gen3Sha256_Update(&perFrame, (const void *)audioBuffer,
                              ScenarioFrameSamples() * sizeof(float));
            Gen3Sha256_Final(&perFrame, frameDigest);
            if (memcmp(frameDigest, sScenario.controlDigest[frame], 32) != 0)
                return "cry save: post-load PCM diverges from control";
            if ((REG_SOUND1CNT_X & 0x7FF) != sScenario.freqHist[frame][0]
             || (REG_SOUND2CNT_H & 0x7FF) != sScenario.freqHist[frame][1]
             || (REG_SOUND3CNT_X & 0x7FF) != sScenario.freqHist[frame][2])
                return "cry save: post-load CGB freq regs diverge from control";
        }
    }
    return NULL;
}

/* ---------- driver ---------- */

static const struct ScenarioDef sScenarios[] =
{
    { "looping-bgm",        ScenarioLoopingBgm },
    { "battle-bgm",         ScenarioBattleBgm },
    { "bgm-change",         ScenarioBgmChange },
    { "restart",            ScenarioRestart },
    { "sfx",                ScenarioSfx },
    { "fanfare",            ScenarioFanfare },
    { "phoneme",            ScenarioPhoneme },
    { "patt-heavy",         ScenarioPattHeavy },
    { "goto-heavy",         ScenarioGotoHeavy },
    { "programmable-wave",  ScenarioProgrammableWave },
    { "cry-over-bgm",       ScenarioCryOverBgm },
    { "overlapping",        ScenarioOverlapping },
    { "multi-player",       ScenarioMultiPlayer },
    { "save-load-bgm",      ScenarioSaveLoadBgm },
    { "save-load-pattern",  ScenarioSaveLoadPattern },
    { "save-load-cry",      ScenarioSaveLoadCry },
    { "quick-load-stress",  ScenarioQuickLoadStress },
    { "hydrate-many",       ScenarioHydrateMany },
};

int NativeAudioScenarioTest(bool32 runCanary, u64 perfBaselineNs)
{
    char packPath[1024];
    unsigned char *pristine;
    size_t i;

    if (!Platform_AssetGetPath("games/emerald/base/emerald-bpee01-v1.rpack",
                               packPath, sizeof(packPath))
     || EmeraldResourceCompat_RegisterRuntimeSnapshot(packPath)
            != EMERALD_COMPAT_OK)
    {
        fprintf(stderr, "Native audio scenarios: production pack not "
                        "resolvable/registrable\n");
        return 1;
    }
    if (!Platform_RuntimeGetGameBssRange(&sScenario.bssStart, &sScenario.bssEnd)
     || sScenario.bssEnd <= sScenario.bssStart
     || sScenario.bssEnd - sScenario.bssStart > 64u * 1024u * 1024u)
    {
        fprintf(stderr, "Native audio scenarios: no game_bss range\n");
        return 1;
    }
    sScenario.regionSize = (size_t)(sScenario.bssEnd - sScenario.bssStart);
    sScenario.region = (unsigned char *)(uintptr_t)sScenario.bssStart;
    sScenario.soundInfo = &gSoundInfo;
    if ((uintptr_t)sScenario.soundInfo < sScenario.bssStart
     || (uintptr_t)sScenario.soundInfo + sizeof(*sScenario.soundInfo)
            > sScenario.bssEnd)
    {
        fprintf(stderr, "Native audio scenarios: gSoundInfo outside game_bss\n");
        return 1;
    }
    pristine = malloc(sScenario.regionSize);
    if (pristine == NULL)
    {
        fprintf(stderr, "Native audio scenarios: allocation failed\n");
        return 1;
    }
    sScenario.pristine = pristine;
    sScenario.runCanary = runCanary;

    if (!Platform_ProfileInit()
     || !Platform_ProfileLoadSelectedSave(FLASH_BASE, sizeof(FLASH_BASE)))
    {
        fprintf(stderr, "Native audio scenarios: profile initialization failed\n");
        free(pristine);
        return 1;
    }
    cgb_audio_init(42060);
    m4aSoundInit();
    memcpy(pristine, sScenario.region, sScenario.regionSize);
    memcpy(sScenario.pristineIo, REG_BASE, sizeof(sScenario.pristineIo));

    for (i = 0; i < sizeof(sScenarios) / sizeof(sScenarios[0]); i++)
    {
        const struct ScenarioDef *def = &sScenarios[i];
        uint8_t digest[32];
        const char *failure;

        ScenarioReset();
        Gen3Sha256_Init(&sScenario.sha);
        failure = def->fn();
        Gen3Sha256_Final(&sScenario.sha, digest);
        ScenarioPrint(def, digest);
        if (failure != NULL)
        {
            fprintf(stderr, "Native audio scenarios: %s FAILED: %s\n",
                    def->name, failure);
            free(pristine);
            return 1;
        }
    }
    /* R12-F §10 pin: the hydrated-song cache is permanent per address and
     * the 610 gSongTable rows (530 real + 80 dummy) are its only reachable
     * addresses; the 640 bound's overflow fallback to slot 0 must stay
     * unreachable, and no load may have grown the cache (the cry scenario
     * asserts no growth per round trip). */
    if (M4aGetHydratedSongHeaderCount() > 610u)
    {
        fprintf(stderr, "Native audio scenarios: hydrated song count %u "
                        "exceeds the 610 reachability bound\n",
                (unsigned)M4aGetHydratedSongHeaderCount());
        free(pristine);
        return 1;
    }
    fprintf(stdout, "Native audio scenarios passed (%u/%u, canary %s)\n",
            (unsigned)(sizeof(sScenarios) / sizeof(sScenarios[0])),
            (unsigned)(sizeof(sScenarios) / sizeof(sScenarios[0])),
            runCanary ? "on" : "off (oracle)");

    /* R12-E §8 perf gate: a fixed window of the representative looping BGM,
     * best of several batches (best-of is robust to scheduler noise; both
     * builds run the identical frames, so the delta is the arena-resident
     * resolution cost, not scenario noise). */
    {
        uint64_t best = UINT64_MAX;
        uint32_t batch;

        for (batch = 0u; batch < PERF_WINDOW_BATCHES; batch++)
        {
            struct timespec t0, t1;
            uint64_t ns;
            u32 frame;

            ScenarioReset();
            m4aSongNumStart(359); /* mus_route101, the representative BGM */
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (frame = 0u; frame < PERF_WINDOW_FRAMES; frame++)
            {
                RunMixerFrame();
                m4aSoundVSync();
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull
               + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
            if (ns < best)
                best = ns;
        }
        fprintf(stdout, "PERF window: %u frames x %u batches best=%llu ns",
                PERF_WINDOW_FRAMES, PERF_WINDOW_BATCHES,
                (unsigned long long)best);
        if (perfBaselineNs != 0u)
        {
            double ratio = (double)best / (double)perfBaselineNs;

            fprintf(stdout, " (oracle baseline %llu ns, ratio %.3fx)",
                    (unsigned long long)perfBaselineNs, ratio);
            if (ratio > PERF_MARGIN_RATIO)
            {
                fprintf(stdout, "\n");
                fprintf(stderr, "Native audio perf gate FAILED: %llu ns vs "
                                "oracle %llu ns exceeds the +%u%% margin\n",
                        (unsigned long long)best,
                        (unsigned long long)perfBaselineNs,
                        (unsigned)((PERF_MARGIN_RATIO - 1.0) * 100.0));
                free(pristine);
                return 1;
            }
            fprintf(stdout, " PERF ok (margin +%u%%)\n",
                    (unsigned)((PERF_MARGIN_RATIO - 1.0) * 100.0));
        }
        else
        {
            fprintf(stdout, "\n");
        }
    }

    free(pristine);
    return 0;
}

/* R12-E §12/§14.10-11: the runtime refusal contract. A missing or corrupt
 * production pack refuses the session (RegisterRuntimeSnapshot non-OK,
 * TryInitialize a no-op, no arena anywhere), a real pack recovers with no
 * residue from the refusals, a state load whose post-load audio republish
 * cannot run FAILS the load (no silent compiled fallback), and a cleared
 * arena refuses republish (EMERALD_AUDIO_ERR_UNAVAILABLE). Together these
 * pin the startup surface the plan's §12.1 refusal rides on: a broken pack
 * means --verify-game-data exits 2 and the game does not start. */
int NativeAudioRefusalTest(void)
{
    const char *packName = "games/emerald/base/emerald-bpee01-v1.rpack";
    char realPackPath[1024];
    char missingPath[1024];
    char damagedPath[1024];
    struct EmeraldAudioCompatDiagnostics audioDiag;
    const uint8_t *arenaBase;
    size_t arenaSize;
    enum PlatformStateOperationResult result;

    if (!Platform_AssetGetPath(packName, realPackPath, sizeof(realPackPath)))
    {
        fprintf(stderr, "Native audio refusal: production pack not resolvable "
                        "(run from the repo root)\n");
        return 1;
    }

    /* 1. Missing pack: registration refused, TryInitialize a no-op, no
     * arena anywhere (this is what the startup verify surfaces). */
    snprintf(missingPath, sizeof(missingPath), "%s/no-such-pack.rpack",
             realPackPath);
    if (EmeraldResourceCompat_RegisterRuntimeSnapshot(missingPath)
            == EMERALD_COMPAT_OK)
    {
        fprintf(stderr, "Native audio refusal: missing pack REGISTERED\n");
        return 1;
    }
    EmeraldResourceCompat_TryInitialize();
    if (EmeraldAudioCompat_GetPublishedCount() != 0u
     || EmeraldAudioCompat_GetArena(&arenaBase, &arenaSize))
    {
        fprintf(stderr, "Native audio refusal: arena published without a pack\n");
        return 1;
    }

    /* 2. Corrupt pack on disk: one flipped byte in an audio leaf payload
     * fails the per-entry payload SHA-256 at OpenFile (resource_pack.c),
     * so the register path refuses before any snapshot exists. */
    {
        struct Gen3ResourcePack *pack = NULL;
        struct Gen3ResourcePackDiagnosticList openDiag;
        char tmpl[] = "/tmp/emerald-refusal-XXXXXX";
        int fd = -1;
        uint8_t *bytes = NULL;
        long length;
        size_t i, n;
        uint64_t flipOffset = 0;
        FILE *f = NULL;
        bool32 damagedReady = FALSE;

        Gen3ResourcePackDiagnostics_Init(&openDiag);
        if (Gen3ResourcePack_OpenFile(realPackPath, &pack, &openDiag) != GEN3_PACK_OK
         || pack == NULL)
        {
            Gen3ResourcePackDiagnostics_Destroy(&openDiag);
            fprintf(stderr, "Native audio refusal: cannot open the real pack\n");
            return 1;
        }
        /* Reconstruct the first audio entry's file payload offset from the
         * public layout contract: the payload section starts at the header's
         * GEN3_PACK_OFF_PAYLOAD_OFFSET (u64 LE) and the parser places
         * payloads as dense 16-aligned slots in TOC order (resource_pack.c),
         * so the offset is the header value plus the aligned sizes of every
         * preceding entry. The public view exposes no direct file offset. */
        {
            uint8_t header[GEN3_PACK_HEADER_SIZE];
            uint64_t payloadStart;
            uint64_t running = 0;
            f = fopen(realPackPath, "rb");
            if (f == NULL
             || fread(header, 1u, sizeof(header), f) != sizeof(header))
            {
                if (f != NULL)
                    fclose(f);
                Gen3ResourcePack_Destroy(pack);
                Gen3ResourcePackDiagnostics_Destroy(&openDiag);
                fprintf(stderr, "Native audio refusal: pack header unreadable\n");
                return 1;
            }
            fclose(f);
            f = NULL;
            payloadStart = (uint64_t)header[GEN3_PACK_OFF_PAYLOAD_OFFSET]
                         | ((uint64_t)header[GEN3_PACK_OFF_PAYLOAD_OFFSET + 1] << 8)
                         | ((uint64_t)header[GEN3_PACK_OFF_PAYLOAD_OFFSET + 2] << 16)
                         | ((uint64_t)header[GEN3_PACK_OFF_PAYLOAD_OFFSET + 3] << 24)
                         | ((uint64_t)header[GEN3_PACK_OFF_PAYLOAD_OFFSET + 4] << 32)
                         | ((uint64_t)header[GEN3_PACK_OFF_PAYLOAD_OFFSET + 5] << 40)
                         | ((uint64_t)header[GEN3_PACK_OFF_PAYLOAD_OFFSET + 6] << 48)
                         | ((uint64_t)header[GEN3_PACK_OFF_PAYLOAD_OFFSET + 7] << 56);
            n = Gen3ResourcePack_GetEntryCount(pack);
            for (i = 0; i < n; i++)
            {
                const struct Gen3ResourcePackEntry *entry =
                    Gen3ResourcePack_GetEntry(pack, i);
                if (entry == NULL)
                    continue;
                if (strncmp(entry->canonicalName, "emerald:audio/",
                            sizeof("emerald:audio/") - 1u) == 0)
                {
                    flipOffset = payloadStart + running;
                    break;
                }
                running += ((uint64_t)entry->payloadSize + 15u) & ~(uint64_t)15u;
            }
        }
        Gen3ResourcePack_Destroy(pack);
        Gen3ResourcePackDiagnostics_Destroy(&openDiag);
        if (flipOffset == 0u)
        {
            fprintf(stderr, "Native audio refusal: no audio payload to damage\n");
            return 1;
        }

        fd = mkstemp(tmpl);
        if (fd < 0)
        {
            fprintf(stderr, "Native audio refusal: mkstemp failed\n");
            return 1;
        }
        snprintf(damagedPath, sizeof(damagedPath), "%s", tmpl);
        f = fopen(realPackPath, "rb");
        if (f != NULL)
        {
            fseek(f, 0, SEEK_END);
            length = ftell(f);
            rewind(f);
        }
        if (f == NULL || length < 0 || length < (long)flipOffset + 1L)
            goto damage_done;
        bytes = (uint8_t *)malloc((size_t)length);
        if (bytes == NULL
         || fread(bytes, 1u, (size_t)length, f) != (size_t)length)
            goto damage_done;
        fclose(f);
        f = NULL;
        bytes[flipOffset] ^= 0xFFu;
        if (write(fd, bytes, (size_t)length) != length)
            goto damage_done;
        free(bytes);
        bytes = NULL;
        close(fd);
        fd = -1;
        damagedReady = TRUE;

damage_done:
        if (f != NULL)
            fclose(f);
        if (fd >= 0)
            close(fd);
        free(bytes);
        if (!damagedReady)
        {
            unlink(damagedPath);
            fprintf(stderr, "Native audio refusal: damaged-pack fixture failed\n");
            return 1;
        }

        if (EmeraldResourceCompat_RegisterRuntimeSnapshot(damagedPath)
                == EMERALD_COMPAT_OK)
        {
            fprintf(stderr, "Native audio refusal: corrupt pack REGISTERED\n");
            return 1;
        }
        EmeraldResourceCompat_TryInitialize();
        if (EmeraldAudioCompat_GetPublishedCount() != 0u
         || EmeraldAudioCompat_GetArena(&arenaBase, &arenaSize))
        {
            fprintf(stderr, "Native audio refusal: arena published from a "
                            "corrupt pack\n");
            return 1;
        }
        unlink(damagedPath);
    }
    damagedPath[0] = '\0';

    /* 3. Recovery: the refusals left no residue; the real pack registers
     * and publishes the arena. */
    if (EmeraldResourceCompat_RegisterRuntimeSnapshot(realPackPath)
            != EMERALD_COMPAT_OK)
    {
        fprintf(stderr, "Native audio refusal: real pack refused after "
                        "damage tests\n");
        return 1;
    }
    EmeraldResourceCompat_TryInitialize();
    if (EmeraldAudioCompat_GetPublishedCount() == 0u
     || !EmeraldAudioCompat_GetArena(&arenaBase, &arenaSize))
    {
        fprintf(stderr, "Native audio refusal: arena absent after recovery\n");
        return 1;
    }

    /* 4. Load without an arena: a state saved with a live session must FAIL
     * the load once the arena is gone - the post-load audio republish
     * cannot run, so the restored state cannot play a single song (the
     * native gSongTable rows are ROM logical addresses). No silent
     * compiled fallback: the load fails and names the audio republish. */
    if (!Platform_ProfileInit()
     || !Platform_ProfileLoadSelectedSave(FLASH_BASE, sizeof(FLASH_BASE)))
    {
        fprintf(stderr, "Native audio refusal: profile initialization failed\n");
        return 1;
    }
    cgb_audio_init(42060);
    m4aSoundInit();
    if (Platform_StateSave(PLATFORM_STATE_QUICK_SLOT)
            == PLATFORM_STATE_OPERATION_FAILED)
    {
        fprintf(stderr, "Native audio refusal: state save failed\n");
        return 1;
    }
    EmeraldAudioCompat_ClearMigratedEntries();
    if (EmeraldAudioCompat_GetPublishedCount() != 0u
     || EmeraldAudioCompat_GetArena(&arenaBase, &arenaSize))
    {
        fprintf(stderr, "Native audio refusal: arena survived the clear\n");
        return 1;
    }
    result = Platform_StateLoad(PLATFORM_STATE_QUICK_SLOT);
    if (result != PLATFORM_STATE_OPERATION_FAILED)
    {
        fprintf(stderr, "Native audio refusal: load without arena returned "
                        "result %d\n", (int)result);
        return 1;
    }
    if (strstr(Platform_StateGetLastError(),
               "audio arena republish failed") == NULL)
    {
        fprintf(stderr, "Native audio refusal: unexpected load error: %s\n",
                Platform_StateGetLastError());
        return 1;
    }
    if (EmeraldAudioCompat_GetPublishedCount() != 0u
     || EmeraldAudioCompat_GetArena(&arenaBase, &arenaSize))
    {
        fprintf(stderr, "Native audio refusal: arena present after failed load\n");
        return 1;
    }

    /* 5. Republish without an arena is refused (EMERALD_AUDIO_ERR_
     * UNAVAILABLE): the post-load path and any future consumer get a
     * failure, never a compiled fallback. */
    if (EmeraldAudioCompat_Republish(&audioDiag) == EMERALD_AUDIO_OK)
    {
        fprintf(stderr, "Native audio refusal: republish without arena OK\n");
        return 1;
    }

    fprintf(stdout, "Native audio refusal passed (missing pack, corrupt pack, "
                    "recovery, load-without-arena, republish-UNAVAILABLE)\n");
    return 0;
}

#endif /* PLATFORM_SDL2 && NATIVE_LINUX */

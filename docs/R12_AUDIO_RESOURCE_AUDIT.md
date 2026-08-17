# R12-A — Emerald Audio Asset Audit

Scope: establish the exact audio data surface, its pointer graphs, native
consumers, x64 embedding, save-state interaction, and lifecycle before any
R12 migration code. No production code was modified during this audit
(verified: working tree clean except the pre-existing `android/SDL2` submodule
state).

Companion: `docs/R12_AUDIO_OWNERSHIP_ARCHITECTURE.md` (design, stages, tests).

## 1. Asset family inventory

All ROM-backed audio DATA is assembled through exactly one file:
`data/sound_data.s` — `.section .rodata` + `.include`s of
`asm/macros/m4a.inc`, `asm/macros/music_voice.inc`,
`sound/voice_groups.inc`, `sound/keysplit_tables.inc`,
`sound/programmable_wave_data.inc`, `sound/music_player_table.inc`,
`sound/song_table.inc`, `sound/direct_sound_data.inc`.
Songs are separate objects: 110 `sound/songs/*.s` + 420 generated
`sound/songs/midi/*.s` (both built by `Makefile_pc:418-420` /
`audio_rules.mk`).

GBA ROM address anchors (pokeemerald.map):
`voicegroup_dummy` 0x0867709c · `gCryTable` 0x0869f08c ·
`ProgrammableWaveData_1` 0x086b5bc8 · `gMPlayTable` 0x086b5d58 ·
`gSongTable` 0x086b5d88 · `DirectSoundWaveData_sc88pro_glockenspiel`
0x086b709c · `Cry_Bulbasaur` 0x08747a9c · `Cry_Chimecho` 0x088d53a0 ·
phonemes 0x088DBBC0+ · songs 0x088fd3d4 (`mus_dummy`) … 0x089a3db4.
Map span of audio symbols: **0x0867709c → 0x089a3db4 ≈ 3,325,208 B (~3.17 MiB)**.

### 1.1 Song table — `sound/song_table.inc` → `gSongTable` — class B (structural)

- **610 `song` entries** (macro `asm/macros/m4a.inc:1-6`: `.int header;
  .short music_player; .short unknown` → 8 B/entry, **4,880 B** total).
- Composition by music-player index:
  - index 0 (BGM): **268** = 188 `mus_*` + 80 `dummy_song_header` rows
  - index 1 (SE1): **223** `se_*`
  - index 2 (SE2): **112** = 22 `mus_*` jingles + 39 `se_*` + 51 `ph_*`
  - index 3 (SE3): **7** ambient loops (`se_thunderstorm`, `se_rain`, …)
- `dummy_song_header:` is 4 zero bytes at the end of the file (song_table.inc:616-617).
- Native struct: `struct Song { GbaAddr header; u16 ms; u16 me; }`
  (include/gba/m4a_internal.h:392-397, sizeof asserted 8 in src/m4a.c:5).
  Entries hold **4-byte GbaAddr** values even on LINUX64 — never widened.

### 1.2 Songs / track command streams — class A (payload)

- **530 unique song graphs** (188+22 BGM, 269 SE, 51 phoneme); 80 rows alias
  the shared dummy header.
- Sources: 110 hand-written `sound/songs/se_*.s` (288,325 B) + 420
  **git-IGNORED generated** `sound/songs/midi/*.s` (9,036,556 B source)
  produced by `tools/mid2agb` from 420 tracked `.mid` (2,012,577 B) via
  `sound/songs/midi/midi.cfg` (420 lines, per-song `-G<voicegroup>
  -V<vol>` options).
- Structure (verified in `se_dex_page.s` and `mus_abandoned_ship.s` /
  `mus_b_frontier.s`, 9 tracks):
  - per-track label: `.byte` command stream; `GOTO`/`PATT` carry
    **`.4byte` absolute address operands in-stream**
    (mus_abandoned_ship.s: `.byte GOTO` + `.4byte mus_abandoned_ship_8_B1`);
  - header at the end: `.byte trackCount; .byte blockCount; .byte pri;
    .byte rev; .4byte <voicegroup label>; .4byte part1 … partN` —
    byte-identical to `struct SongHeader` (m4a_internal.h:246-254).
- Track stream address operands stay 4-byte on native by contract:
  `MP2K_ADDRESS_OPERAND_SIZE = sizeof(GbaAddr)` (include/music_player.h:9-19,
  m4a_internal.h:559) and `MP2KReadAddressOperand` reads exactly 4 bytes.
- Native-compiled song rodata today: **~708,920 B** (530 song objects).

### 1.3 Voicegroups / instrument banks — class B (structural, transformed on native)

- **195 tables** in `sound/voicegroups/`: 180 root + 10 `drumsets/` + 5
  `keysplits/`; **20,594 instrument entries total** (max 128/tableset).
- `sound/voice_groups.inc` (196 include lines = 195 voicegroup .inc +
  `cry_tables.inc`) — labels `voicegroup_<name>` (185 exported symbols in
  the map; the 10 drumsets are offset-defined via `voice_group label, N`).
- GBA row = **12 B** (`_voice_directsound` in asm/macros/music_voice.inc):
  `byte type, key, pan|0x80, 0; .int sample_ptr; byte a,d,s,r`.
  Type set: 0/8/16 directsound, 1/9/2/10/3/11/4/12 square/prog-wave/noise,
  0x40 keysplit (`voice_keysplit`: `.int group_ptr; .int keysplit_ptr`),
  0x80 keysplit-all.
- **LINUX64 branch already exists** in every macro: native rows are
  **24 B** = 4 scalars + 4 pad + `.quad` sample/group pointer (+ `.quad`
  keysplit pointer) + ADSR + 4 pad — matching `struct MP2KInstrument`
  (include/music_player.h:32-54: type/drumKey/cgbLength/panSweep + union
  {wav|group|cgb3Sample|squareNoiseConfig} + union {ADSR|keySplitTable}).
  Native voicegroup payload today ≈ 20,594 × 24 ≈ **494 KB**.
- References FROM voicegroups: sample/wave labels (payload edges) and
  keysplit table labels (structural edges). No back-references.

### 1.4 Direct-sound samples — class A (payload)

- `sound/direct_sound_data.inc` (2,175 lines): **544 symbols**, each
  `.align 2` + `.incbin` — 105 root `DirectSoundWaveData_*` + 51
  `DirectSoundWaveData_Phoneme_1..51` + **388 `Cry_<Species>`**
  (`sound/direct_sound_samples/cries/*.bin`).
- Payload = `struct WaveData` (m4a_internal.h:43-51): u16 type/status,
  u32 freq, u32 loopStart, u32 size + s8 samples — header 16 B + data.
- Total sample bytes (the .bin set, git-ignored build intermediates):
  **2,383,854 B** — cries 1,630,407 (max `jynx.bin` 12,146 B), phonemes
  111,396, root 642,051 (max `dance_drums_ride_bell.bin` 30,767 B).
- Consumers of phonemes: bard voicegroups (`src/bard_music.c`,
  `src/mauville_old_man.c` — ph_* songs, phoneme samples via voicegroups).

### 1.5 Programmable waves — class A (payload)

- `sound/programmable_wave_data.inc`: **25 symbols**
  `ProgrammableWaveData_1..25`, each `.incbin` of a 16-byte `.pcm`
  (CGB waveform RAM, 32×4-bit); 9 marked `@ Unused`. Total **400 B**.

### 1.6 Keysplit tables — class B (structural)

- `sound/keysplit_tables.inc`: **5 tables** (`piano, strings, trumpet,
  tuba, french_horn`), `.byte` runs of sub-voicegroup indices with the
  label-backshift convention (macro `keysplit`/`split`, m4a.inc) — total
  **516 B**. Referenced only by voicegroup rows.

### 1.7 Music player table — `sound/music_player_table.inc` → `gMPlayTable` — class B (structural)

- **4 entries** (12 B each): BGM 10 tracks / SE1 3 / SE2 9 / SE3 1
  (`music_player` macro: `.int info, .int track, .byte num_tracks, …`).
  Points at engine BSS (`gMPlayInfo_*`, `gMPlayTrack_*`, src/m4a.c:48-55).
  Native struct `struct MusicPlayer { GbaAddr info; GbaAddr track; u8
  numTracks; u16 unk_A; }` (m4a_internal.h:382-390) — GbaAddr-shaped.

### 1.8 Cry tables — `sound/cry_tables.inc` — class B (structural, transformed on native)

- `gCryTable:` **388 entries** + `gCryTable_Reverse:` **388 entries**
  (macros `cry`/`cry_reverse`, music_voice.inc:186-210): GBA 12 B =
  `.byte 0x20/0x30, 60, 0, 0; .int sample; .byte 0xff, 0, 0xff, 0`; native
  24 B with `.quad` sample pointer (LINUX64 branch).
- Both tables point at the **same 388 `Cry_*` samples**; reverse playback is
  the type bit (0x20 vs 0x30), not a second sample set.
- Consumers: `src/sound.c:28-29,475` (`GET_CRY` →
  `gCryTable[(128*tableId)+speciesIndex]`) → `SetPokemonCryTone`
  (src/m4a.c:1797-1834) which copies `gPokemonCrySong` (m4a.c:47) into
  `gPokemonCrySongs[0..1]` (m4a.c:42), sets `.tone` to the cry table row,
  and patches `bytecode.gotoTarget = HostPointerToGbaAddr(&bytecode.cont)`
  (m4a.c:1827).
- Species→cry-id mapping: `gSpeciesIdToCryId[]` (src/data/pokemon/cry_ids.h,
  135 entries) + `SpeciesToCryId` (src/sound.c:470) — class D.

### 1.9 Cry bytecode template — class C/D (compiled engine constant)

- `gPokemonCrySongTemplate` (src/m4a_tables.c:258-268): the 0x21-byte
  `struct PokemonCryBytecode` template (m4a_internal.h:259-282, packed,
  GOTO operand 4-byte `GbaAddr` — the existing precedent for GBA-shaped
  byte streams with 4-byte addresses on host) with
  `.tone = (struct ToneData *)&voicegroup_dummy`. Stays compiled; its
  `.tone` initializer becomes a publication seam if voicegroups migrate.

### 1.10 Engine tables — class C (compiled, never migrated)

- src/m4a_tables.c: `gMPlayJumpTableTemplate`, `gDeltaEncodingTable`,
  `gScaleTable`, `gFreqTable`, `gPcmSamplesPerVBlankTable`, `gCgbScaleTable`,
  `gCgbFreqTable`, `gNoiseTable`, `gCgb3Vol`, `gXcmdTable` — algorithmic
  lookup tables of the m4a library, not ROM asset payloads.
- `sound/MPlayDef.s` (430 lines): pure `.equ` constants (opcodes W/GOTO/
  PATT/…, notes CnM2-Gn8, velocities v000-v127, mxv/c_v) — class D.
- Engine code: src/m4a.c, src/m4a_1.s, src/music_player.c,
  src/sound_mixer.c, src/sound.c, src/bard_music.c, src/mauville_old_man.c,
  include/gba/m4a_internal.h, include/music_player.h,
  include/sound_mixer.h — class C, stays compiled. **Do not migrate the
  MP2K engine** (R12 directive).

### 1.11 Generated constants — class D (compiled indices, no payload bytes)

- `include/constants/songs.h`: **481 defines** (212 MUS_ + 269 SE_).
- `src/data/pokemon/cry_ids.h`: 135 u16 indices.
- Note: `gSongTable`/`gMPlayTable` rows are 4-byte `GbaAddr` — values only,
  no payload bytes — which is why they remain compiled (see architecture
  §6).

### 1.12 Authoring sources — tracked vs generated

- Tracked: 544 `.aif`, 25 `.pcm`, 420 `.mid` + `midi.cfg`, 195 voicegroup
  `.inc`, 110 top-level song `.s`, 8 `.inc`/`.s` (1,303 files).
- Generated + git-ignored (.gitignore:21-22): `sound/**/*.bin` and
  `sound/songs/midi/*.s`. `.bin` from `.aif` via `tools/aif2pcm` (build
  rules audio_rules.mk:22-27, cries with `--compress`); `.s` from `.mid`
  via `mid2agb`. This matches upstream pret: `.aif`/`.mid` are the tracked
  sources; the compiled bytes that actually enter the ROM come from the
  generated intermediates.
- The Makefile_pc native build consumes the **same generated intermediates**
  (Makefile_pc:274-280 assembles the .s objects; data/sound_data.s
  incbins the .bin) — i.e. today the native executable embeds the compiled
  audio bytes.

## 2. Four-way classification

| Family | Class | Count | ROM bytes | Native bytes today |
|---|---|---|---|---|
| direct-sound samples (`DirectSoundWaveData_*` + `Cry_*`) | **A** | 544 | 2,383,854 | 2,383,854 (.rodata) |
| programmable waves (`ProgrammableWaveData_*`) | **A** | 25 | 400 | 400 |
| song graphs (`mus_*`, `se_*`, `ph_*` header+streams) | **A** | 530 (+80 dummy aliases) | ~700 KB (0x088fd3d4-0x089a3db4 + SE region) | ~708,920 |
| voicegroup tables (20,594 entries) | **B** | 195 | 247,128 (12 B/row) | ~494,256 (24 B/row) |
| cry tables `gCryTable`/`_Reverse` | **B** | 776 rows (2 tables) | 9,312 | ~18,624 |
| keysplit tables | **B** | 5 | 516 | 516 |
| `gSongTable` | **B** | 610 | 4,880 | 4,880 |
| `gMPlayTable` | **B** | 4 | 48 | 48 |
| `dummy_song_header` (4 zero bytes) | **B/D** | 1 | 4 | 4 |
| cry bytecode template + engine tables + MPlayDef constants | **C/D** | ~12 tables | 0 (compiled code/data) | compiled |
| songs.h / cry_ids.h constants | **D** | 616 entries | — | — |
| `.aif` / `.mid` authoring sources | **A-source** | 964 files | — (build inputs) | not embedded |

Ambiguous (E):
- **Voicegroups/cry tables**: structural, but carry per-instrument ADSR and
  sample wiring that is original ROM data. On native they are ALREADY
  transformed (12→24 B rows with host pointers) — the executable contains a
  derived representation, not the ROM bytes. Architecture doc §2 decides:
  migrate them as canonical GBA-form pack resources with a publication
  transform; fallback = keep compiled + publish pointer fields.
- **80 dummy song rows**: routing rows pointing at one shared 4-byte header.
  Decision: keep compiled (structural placeholder), documented as
  `GBA_PARITY`-style exception.
- **Track command streams**: bytecode arrangements. They are ROM payload
  (class A) — migrate; the streams' internal 4-byte addresses make them the
  relocation-hardest family (architecture §5).

## 3. Pointer graph analysis (per structure, ROM representation)

| Structure | Representation in ROM | Internal references |
|---|---|---|
| `SongHeader` (4 B + `.4byte tone` + N×`.4byte part`) | absolute 4-byte GBA addresses | voicegroup table start; track stream starts (within same song object) |
| Track command stream | bytecode; `GOTO`/`PATT` carry a 4-byte absolute GBA address **in-stream** | other stream labels (same song, incl. self-loop `_B1`) |
| `xwave`/`xcmd 0x??` operands | 4-byte absolute address in-stream | sample `DirectSoundWaveData_*` starts |
| `ToneData` row (voicegroup/cry) | `.int`/`.quad` absolute pointer | sample / programmable-wave / keysplit-table starts |
| Keysplit table | `.byte` run | nothing (pure index run) |
| `gSongTable` row | `.int` absolute + 2×`.short` | song header start; player index (0-3) |
| `gMPlayTable` row | `.int` absolute ×2 + counts | engine BSS players/tracks |
| `struct WaveData` sample | header + raw samples | nothing (loopStart is a sample index) |
| `PokemonCryBytecode` | packed 0x21 B, `GbaAddr gotoTarget` | BSS continuation (patched at runtime, m4a.c:1827) |

Consequence: songs are **self-contained graphs** (header + streams in one
contiguous object) with edges to voicegroups; voicegroups/cry tables have
edges to samples/waves/keysplits; samples/waves are leaves. The only
pointer-bearing byte streams are the song streams; every other pointer
lives in fixed-size records.

## 4. Native consumer analysis (who reads what, and how)

The native build ALREADY runs a "GBA-shaped data + hydrate via
`HostResolveGbaAddr`" engine — the R12 seam is largely present:

- `gSongTable`/`gMPlayTable`: GbaAddr-shaped tables; every dereference
  goes through `HostResolveGbaAddr` — `GetMusicPlayerInfo`
  (src/m4a.c:71-74), `GetMusicPlayerTracks` (m4a.c:76-79),
  `m4aSongNumStart` family (m4a.c:207-283) → `HydrateSongHeader`
  (m4a.c:81-96): reads the GBA-shaped header bytes
  (`raw[0..3]`, `T1_READ_32(raw+4)` tone, `T1_READ_32(raw+8+i*4)` parts)
  and caches a host-width `HostSongHeader` in `sHostSongHeaders[4]`
  (m4a.c:58-69) — **songs are only ever GBA-shaped at rest**; hydration is
  per-song-start.
- `HostResolveGbaAddr` (src/platform/host_memory.c:308-330): non-handle
  values are the 4-byte symbolic address itself (non-PIE link keeps vanilla
  generated data below 4 GiB; comment at 325-329 explicitly anticipates "a
  future relocation table to replace it without changing users").
- Stream operands: `MP2KReadAddressOperand` + `HostResolveGbaAddr`
  (src/music_player.c:216-217 for GOTO; src/m4a.c:1692-1695 for xwave).
- Voicegroups: song-header tone → `mplayInfo->tone` (m4a.c:759) is read by
  the native player as `player->voicegroup[voice]`
  (src/music_player.c:282 — `MP2KPlayerState.voicegroup` is
  offset-compatible with `MusicPlayerInfo.tone`) and indexes 24-byte
  `struct MP2KInstrument` records; the instrument's `wav` flows into
  `MixerSource.wav` (include/sound_mixer.h) at note-on.
- Cry tables: consumed as native `struct ToneData*` rows
  (`src/sound.c:475` → `SetPokemonCryTone` m4a.c:1797) with 8-byte `wav`
  pointers.
- Samples: `struct WaveData` header (freq/loopStart/size) + s8 data read by
  the mixer resampler; CGB path copies programmable waves into wave RAM
  (m4a.c CgbSound).
- Cry playback: `HydratePokemonCrySong` (m4a.c:98-112) builds a host song
  header pointing at `gPokemonCrySongs[].bytecode` (BSS) — the cry path
  never touches ROM stream data at runtime (the bytecode lives in BSS after
  the template copy).

## 5. Current embedding in x64 executables

- Both `pokeemerald-linux64` (30,259,744 B) and `pokeemerald-linux64-dinfo`
  (42,507,872 B) embed the **identical audio symbol set**: 8,871 `mus_`,
  2,616 `se_`, 195 `voicegroup`, 156 `DirectSoundWaveData_`, 2
  `gCryTable`(+Reverse) symbols (11,844 audio-family symbols total). All
  live in `.rodata` (7,342,896 B section).
- Definitive payload from build/linux64 objects: `data/sound_data.o`
  rodata **2,902,772 B** + 530 song objects **708,920 B** ≈
  **3.44 MiB of audio payload** in the native executable today.
- All audio assembly symbols have `st_size = 0` in the symtab (the known
  "assembly symbols have no size" problem, architecture doc §22 of the
  migration architecture) — extraction must compute sizes from end-labels/
  object layout, never st_size.
- Binaries predate HEAD (built 2026-08-17 14:25 vs commit 15:19); isolation
  audits must rebuild first.
- Link: no audio-specific section placement — `ld_script_native.ld` defines
  only `game_data`/`game_bss`; audio lands in default `.rodata`.

## 6. .aif forensics (the changed direct_sound_samples files)

- **76 of the 544 `.aif` are non-canonical at this checkpoint.** Origin:
  fork commit `ee1173eb9` "update samples, make engine run at 60hz" (2021,
  pc_sound fork work, merged via syncPret PR #25 on 2024-01-12) replaced 76
  named instrument `.aif` with full-length re-exported samples (1-30 KB →
  4-270 KB; 8-bit AIFF, 14-65× more frames; SSND payload differs **from
  byte 0** — different audio, not a re-encoding).
- Only commit in this repo's own history touching the 76 blobs: the
  checkpoint commit itself — `2d1f27a29` (checkpoint-pre-r12-raw) restored
  the canonical pret blobs; the amended `d3a71641f`
  (checkpoint-pre-r12-clean) accidentally flipped them back to fork
  versions. The other 468 `.aif` (cries, phonemes, unknown_*, hex-named)
  are canonical and untouched.
- Verified with the repo's own `tools/aif2pcm` (identical to pret): HEAD's
  `.aif` produce `.bin` 1.5-70× larger than canonical — a GBA ROM built
  from this tree would fail the `rom.sha1` gate
  (f3ae088181bf583e55daf962a92bb46f4f1d07b7). The Makefile_pc build never
  reads `.aif` (it consumes generated `.bin`), so the fork files are
  currently inert for the native build — but they are wrong as tracked
  source.
- `.bin` files are correctly git-ignored and untracked (pret upstream
  removed them from git in 2018; `.aif` are the tracked source of truth).
  Nothing to regenerate.
- **Verdict / R12 action: RESTORE the 76 canonical pret `.aif`** (blobs
  already exist in the object store — a tree edit, not a regeneration; the
  exact target state is `2d1f27a29`'s tree). Do not remove (pret contract:
  `.aif` tracked, `.bin` generated) and do not leave the fork versions
  (silent corruption of any future in-repo GBA build). This is a
  **prerequisite fix to land before R12-B** (R12-A gate).

## 7. State v5 interaction

- **All native audio ENGINE state lives inside the serialized GAME_BSS
  slice** (walked slice): `gSoundInfo` (0x1af21a0), `gMPlayInfo_BGM/SE1-3`,
  `gMPlayTrack_*`, `gMPlayJumpTable`, `gCgbChans`, `gPokemonCry*`,
  `gMPlayMemAccArea`, `sHostSongHeaders` — all inside
  [0x1af0fa0, 0x1b1f9d0) (nm-verified against -dinfo). `ld_script_native.ld`
  places `src/*.o` .bss into game_bss, and m4a.c is a top-level src object.
- During active playback those structs hold **live pointers into audio
  data**: `sHostSongHeaders[].tone/parts[]` → voicegroup/song bytecode;
  `gMPlayTrack[].cmdPtr/patternStack[]` → song streams; `.tone.wav` →
  samples; `gSoundInfo.chans[].wav/currentPointer` → samples;
  `prev/nextChannelPointer` ring, `track`, player chain, memAccArea.
- Today those pointers are image-range (below 4 GiB) and re-derive as
  PDIM/PFIM records; **zero resource-sidecar records are produced for
  audio** (proved by tests/emerald_resource_state_test.c). The
  R10 statement "no resource pointer enters a serialized slice" holds ONLY
  because audio is not yet migrated.
- Post-R12: pointers into published audio arenas inside GAME_BSS → the v5
  walker **fails closed** on hull-inside-unregistered pointers
  (native_state.c:1924-1935). R12-F MUST register audio ranges in the R10
  reverse range index (`RebuildRangeIndex`,
  emerald_trainer_native_compat.c:931-1060) with per-family
  type/schema/role; sidecar records (64 B each: sectionTag, fieldOffset,
  resourceKey, type, schema, role, rangeOffset) then carry audio identities;
  load re-derives arena pointers via `ResolveByKey` (identity lookup +
  offset arithmetic, never saved addresses; mismatch → refuse before any
  write). Range budget: ~2,040 registered today + ~1,300 audio resources
  ≈ 3,340 < 4,096 cap (cap tests must be re-pinned, the R11C pattern).
- Save/load does NOT stop MP2K: the save path only pauses the SDL device
  (sdl2.c:163) with the worker parked at VBlank; load restores mid-song
  state, clears the SDL audio queue and unpauses (native_state.c:3041-3042),
  then `EmeraldResourceCompat_Republish` + `NativeWorldNeighborhood_
  Invalidate` (3060-3074). An audio publication/re-hydration step must join
  that post-load sequence (R12-F).
- The R10 headless proof already shows v5 round-trips active audio
  byte-exactly (`--native-audio-self-test`, sdl2.c:618-1055: save mid-play,
  destroy game_bss, load, 40-frame PCM determinism vs control; asserts
  "wav/currentPointer image-resident (sub-4 GiB)" at 1003-1011 — this
  assertion must flip to "arena-resident" when R12 lands). The
  still-open manual save/load UI suspicion (R10 memory) is separate and
  must stay green.

## 8. Runtime lifecycle (current order)

1. `Platform_GameContentVerifyInstalled(TRUE)` → `EmeraldResourceCompat_
   RegisterRuntimeSnapshot(packPath)` + `TryInitialize` + session
   fingerprint (desktop_game_content.c:751-755; loader
   emerald_runtime_loader.c:105-208) — pack `games/emerald/base/
   emerald-bpee01-v1.rpack` opened from disk, never embedded.
2. SDL audio device init: `Platform_AudioInit(42060)` + `cgb_audio_init`
   (sdl2.c:1436-1437).
3. `Platform_SchedulerInit` → AgbMain thread → `m4aSoundInit()` (main.c:108).
4. Per-frame mixer: `m4aSoundMain()` / `m4aSoundVSync()` (main.c:379-384,
   417).
5. Save/load: worker parked at VBlank, SDL device paused (sdl2.c:147-177).

Ordering hazard for R12: session registration ALWAYS precedes audio init
and any state load (so fingerprint + sidecar resolution have a live
session) — but R12 publication must complete inside step 1 (before the
first `m4aSoundInit`), and post-load re-publication must precede the first
mixer frame after unpause. Missing/corrupt audio resources must hard-fail
step 1 (transactional, R11 seam contract), never crash later in the mixer.

## 9. Resource-system coverage today

Zero audio entries: the production catalog
(`resources/catalogs/emerald/catalog.toml`) holds only the 196 trainer +
1,804 pokémon + R11 overworld families. `GEN3_RESOURCE_MOD_CONTRACT.md`
reserves the TYPES "audio sample, music sequence, sound effect, cry" but no
audio type/schema is wired in the runtime vocabulary
(`resource_types.h`) — R12-B adds them (no pack FORMAT change).

## 10. Prerequisite fixes (must land before/at R12-A)

1. **Restore the 76 canonical `.aif` blobs** (this checkpoint tag is not
   actually clean — see §6). Corrective commit or revert of the .aif
   portion of d3a71641f.
2. Rebuild the two x64 binaries before any isolation-audit baselines (they
   predate HEAD).
3. Re-pin `EMERALD_IMPORT_MAX_RECORDS` cap tests when the audio manifest
   lands (R11C pattern — do it in R12-B, not before).

## 11. Audit conclusions

- The audio data surface is exactly: 544 samples + 25 waves + 530 song
  graphs + 195 voicegroups + 2 cry tables + 5 keysplit tables + 2 routing
  tables + 1 dummy header — assembled from one data root
  (`data/sound_data.s`) plus 530 song objects.
- The native engine is already a GBA-shaped-data consumer: songs hydrate
  per-start through `HostResolveGbaAddr`; only voicegroup/cry tables are
  compiled native-width records (with LINUX64 assembler branches).
- The relocation seam the migration needs is `HostResolveGbaAddr` — its
  source comment already anticipates a relocation table; R12 adds an audio
  logical-address registration, no engine rewrite.
- Save-state mid-play capture is the highest-risk interaction: it will
  hard-fail (correctly) on unregistered arena pointers until R12-F
  registers audio ranges; the sidecar mechanism (identity+offset, refuse on
  mismatch) already covers the reconstruction semantics.
- 1,301 proposed pack resources (~3.1 MiB canonical payload) with the
  taxonomy and stages in `docs/R12_AUDIO_OWNERSHIP_ARCHITECTURE.md`.

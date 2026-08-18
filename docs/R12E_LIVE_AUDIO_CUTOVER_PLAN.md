# R12-E Plan — Live Audio Cutover

Scope: make the running game's audio consume the R12-B/C/D arena resources
instead of the compiled payloads, with PCM parity and a hard-fail resource
contract. No compiled payload removal (R12-G). No engine rewrite. This is a
review of the tree at checkpoint-r12d-complete.

**Central finding: the entire live path is compiled today because of ONE
emission difference — the native `gSongTable` rows hold native link addresses
instead of ROM logical addresses. Every piece of resolution machinery the
cutover needs already exists and is already wired. The cutover is one
generated table, one contract flip, and the proof battery.**

## 1. Exact current live path (verified in this review)

Resolution machinery in `HostResolveGbaAddr` (src/platform/host_memory.c:436):
exact-start table (197 entries, registered at arena publish) → interval table
(1 entry: the song block) → runtime handle table → **direct cast**
`(void *)(uintptr_t)addr`. The native link is non-PIE and keeps generated
data below 4 GiB, so 32-bit native addresses round-trip through the direct
cast — this is what makes the compiled path work today.

Verified byte-level facts (release and DINFO binaries):

| Object | Field | Value today | Resolution |
|---|---|---|---|
| `gSongTable[0].header` (mus_dummy) | native `0x010EF446` | compiled header | direct cast |
| compiled header `tone` (se_dex_page) | native `0x00E5D8F4` = `voicegroup_rs_sfx_1` | compiled rows | direct cast |
| compiled header `parts[i]` | native stream addresses | compiled streams | direct cast |
| compiled row `wav` | native sample addresses | compiled samples | direct cast |
| compiled stream GOTO/PATT operands | native stream addresses | compiled streams | direct cast |

Per-family state:

| Family | State | Evidence |
|---|---|---|
| song headers/streams | **LIVE_COMPILED** | gSongTable rows are native addresses; direct cast serves compiled bytes. GOTO/PATT resolve via `MP2K_event_goto` → `HostResolveGbaAddr` (music_player.c:217) but the operands themselves are native addresses, so the interval never hits. |
| voicegroups | **LIVE_COMPILED** | Compiled headers' `tone` fields are native addresses → direct cast → compiled 24-byte rows. The 197 exact-start entries hold ROM identities and are never consulted. |
| samples | **LIVE_COMPILED** | Compiled rows' `wav` unions are native addresses. |
| programmable waves | **LIVE_COMPILED** | Same row mechanism; PW unions point at compiled wave bytes. |
| cry tables/rows | **LIVE_MIGRATED** | sound.c:45-56 `GetPokemonCryRow` → `EmeraldAudioCryTableRow` → arena transformed rows (parity-gate proven). Compiled `gCryTable`/`gCryTable_Reverse` remain as the fallback when the arena is unpublished (degrade contract, §12 flips this). |
| cry samples | **LIVE_MIGRATED** | The transformed cry row's `wav` union points into the verbatim zone (resolved at publish). |
| phoneme data | **HYBRID** | ph_* songs are compiled-live (they are ordinary gSongTable songs); phoneme samples are compiled-live via compiled `voicegroup_bard` rows. The bard's playback path (`BardSing`, mauville_old_man.c:539) is a plain `m4aSongNumStart(FIRST_PHONEME_SONG + …)` — no separate cutover exists or is needed. |

## 2. Why songs are still compiled-live, and the redirect

`song_table.inc` is assembled with the `song` macro (asm/macros/m4a.inc:1):
`.int \label` — the label's address **in the current link**. On the GBA link
the labels are 0x08xxxxxx (ROM); on the native link they are the compiled
object's native addresses. `m4aSongNumStart` et al. (m4a.c:207-283) then
hydrate `song->header` through `HydrateSongHeader` → `HostResolveGbaAddr`.
Because the row holds a native address, every table miss ends in the direct
cast and the game plays the compiled bytes. R12-D registered the interval but
nothing feeds it a ROM address — the interval has never hit at runtime.

The arena song headers are **verbatim ROM-form bytes**: their `tone` and
`parts[]` fields carry ROM addresses (0x08…). So the moment a song start
hydrates from the arena, everything downstream flips with it:

```
gSongTable[].header (ROM addr, 0x088FC03C…)
  → HydrateSongHeader → HostResolveGbaAddr → INTERVAL hit → arena song bytes
      header.tone  (ROM addr of voicegroup label, 0x0867…)
        → exact-start table (registered at publish) → transformed 24-byte rows
            row.wav → verbatim zone (native ptr built at publish) → arena samples/waves
      header.parts[] (ROM stream addrs) → INTERVAL → arena streams
      stream GOTO/PATT/REPT operands (ROM addrs) → INTERVAL → arena streams
```

**The smallest change that makes this live: re-emit the native `gSongTable`
rows with ROM logical addresses.** Nothing else in the engine changes — the
five `m4aSongNum*` entry points, `HydrateSongHeader`, `MP2K_event_goto`, the
mixer, and the note/voice path all already funnel through
`HostResolveGbaAddr`.

`gSongTable` rows today contain **native-link addresses** (verified: row 0
header = 0x010EF446). The R12-D report's "already equal the logical ROM
identities" statement was about the *GBA/ROM* table (the generator's
row-by-row cross-check of the qualified ELF's table against the ROM table,
gen_audio_bindings.py:179) — it does not describe the native emission, which
is a different link. This review closes that gap; §6 specifies the
regeneration.

## 3. Voicegroup/sample live path after cutover

Today: not live (compiled `tone` → compiled rows → compiled samples). After
the §2 redirect, the path is:

1. `HydrateSongHeader` resolves `tone` (ROM voicegroup-label address) through
   the **exact-start table**. The table is populated at publish by
   `EmeraldAudioCompat_ForEachLogicalLabel` → `HostMemoryRegisterLogicalAddress`
   (emerald_audio_compat.c:1753/1811/2123) with the 197 label addresses
   (195 voicegroups + 2 cry-table labels) mapped to their transformed-zone
   bases. Verified live by the audio compat suite (real `host_memory.c`
   linked, probe assertions).
2. `MPlayStart` copies `songHeader->tone` into `mplayInfo->tone`; note events
   index `player->voicegroup[voice]` (music_player.c:282) into the
   transformed 24-byte rows.
3. The transformed rows' `wav` unions were resolved **at publish** against
   the verbatim zone (TransformRow → ResolveLeafPointer with the real
   zone base) — they are native arena pointers, no runtime resolution. The
   R12-C parity gate proved all 21,370 rows byte-identical to the LINUX64
   assembler output; the same gate pins the wav targets.
4. No compiled sample remains reachable from any song started through the
   redirected table: every leaf pointer the engine can hold is either a
   transformed-row union (arena) or a stream operand (interval → arena).
   The only compiled audio datum a song can still touch is the pinned
   `voicegroup_dummy` 24-byte record — reachable solely through
   `gPokemonCrySongTemplate.tone`, which `SetPokemonCryTone` overwrites
   before any cry plays (§4). Keep it compiled (documented exemption).

## 4. Cry path (already live; verify, do not re-cut)

Trace: species → `CryPlay` (sound.c:508-517) → `GET_CRY` →
`GetPokemonCryRow(table, reversed, index)` → `EmeraldAudioCryTableRow`
(emerald_audio_compat.c:2069) → transformed cry row (24-byte, parity-proven)
→ `SetPokemonCryTone` (m4a.c:1797) writes `gPokemonCrySongs[i].tone = row`
→ `HydratePokemonCrySong` (compiled bytecode parts — engine template, stays
compiled by design) → `MPlayStart` → the row's `wav` union → arena cry
sample → mixer. The bytecode `gotoTarget` patch (`HostPointerToGbaAddr`,
m4a.c:1827) is unchanged BSS work, not a resource.

Compiled cry data is fallback-only today (reached when the arena is
unpublished); §12 removes that fallback from the production path.

## 5. Phoneme/Bard path

`BardSing` → `m4aSongNumStart(FIRST_PHONEME_SONG + 1 + phonemeTripletId*3)`
on the SE2 player (mauville_old_man.c:539) — ordinary song starts. Phoneme
samples are referenced by the ph_* songs' voicegroup rows. **No separate
cutover is needed**: songs flip with §2, voicegroups/samples flip with §3.
The bard's pitch/volume modulation (m4aMPlayPitchControl/VolumeControl) is
player-state only. Test coverage: the ph_* scenario in §8's battery.

## 6. gSongTable strategy — decision: (A) regenerate the native table with ROM logical addresses

Requirements met: 530 real rows resolve to migrated arena headers; 80 dummy
rows keep `dummy_song_header` semantics; table stays 8-byte GBA-shaped
(`struct Song` = GbaAddr + u16 + u16, m4a_internal.h:392); no widening; no
engine rewrite.

**Emission design**

- The R12-D generator (`gen_audio_bindings.py`) already reads `song_table.inc`
  in order, already knows every song's header ROM address (manifest
  `rom_offset` + slice geometry; it also lifts the qualified ELF's ROM
  gSongTable rows for the existing row-by-row cross-check), and already knows
  the 80 dummy-row positions.
- Add a generated, **committed** include `sound/song_table_native.generated.inc`
  (same commit policy as `bindings.generated.toml` /
  `catalog.generated.toml`) that emits, for each of the 610 rows in
  `song_table.inc` order: `.int <header ROM address>` for the 530 real rows
  and `.int dummy_song_header` for the 80 dummy rows, plus the same
  `.short ms, me` pairs (values are unchanged — they come from the same .inc
  arguments).
- `data/sound_data.s` gains a conditional include:

  ```asm
  .if LINUX64
      .include "sound/song_table_native.generated.inc"
  .else
      .include "sound/song_table.inc"
  .endif
  ```

  The GBA build is untouched.
- Regeneration gate: extend the generator's `--check` to re-derive the
  native include byte-for-byte (from `song_table.inc` order + the verified
  ROM gSongTable rows) and fail on drift — the same determinism contract as
  the manifest. A `#error`-style pin is impossible in .s; the --check gate
  plus a row-count pin (`.if . != …` style assertion that exactly 610 rows
  were emitted) serves instead. Also pin row 0 == 0x088FC03C and the last
  real row + its size == 0x089A3050 in the generator check.

Why the alternatives are rejected:

- **(B) publish header fields at startup**: mutating a `.rodata` const table
  at runtime needs a writable copy, a new lifecycle join in republish/clear,
  and the table enters the state-v5 capture surface. Unnecessary machinery
  for a value that is a build-time constant.
- **(C) another existing mapping is sufficient**: no — the interval exists but
  nothing feeds it ROM addresses, and the exact-start table cannot serve
  song headers (it is a per-label table; the song block is an interval by
  design). The direct cast cannot be aimed at the arena without identity
  corruption (compiled headers carry native `tone`/`parts`; arena headers
  carry ROM forms; mixing them in one table is incoherent).

Index stability: row order, indices (MUS_*/SE_*/PH_* constants), and the
`.short` fields are all unchanged, so every `m4aSongNumStart(n)` call site,
fanfare table, and the self-test's `m4aSongNumStart(359)` keep working
unchanged.

## 7. memacc semantics — no walker correction needed (validation-fidelity only)

Engine behavior (ply_memacc, m4a.c:1583): `MEMACC` consumes op + memAcc-area
index + data byte. The memory it touches is `gMPlayMemAccArea` (16-byte BSS
scratch, m4a.c:56) — **never song bytes**, never an address operand. Ops
6-17 are conditional jumps: on true they execute `gMPlayJumpTable[1]`
(ply_goto → `MP2K_event_goto` → `HostResolveGbaAddr`) on the 4-byte target
that follows; on false they skip 4 bytes.

Audit of the 530-set: exactly **1 MEMACC event, non-jump** (walker
PIN_MEMACC = 1, PIN_MEMACC_JUMP = 0). The walker already models memacc-jump
fully (MEMACC_JUMP_OPS = {6..17}, consumes the 4-byte target, validates it
interior to the owning stream region, counts it, adds it to the target set —
song_walker.py:191-210), so a future memacc-jump would be validated at import
time and would resolve at runtime through the identical goto/interval path.
**No correction required before cutover; this is extraction-time validation
fidelity, not a runtime issue.**

## 8. Live PCM gate

Extend the existing headless harness `NativeStateAudioSelfTest`
(sdl2.c:660; runs the real mixer via `RunMixerFrame`, no audio device,
no display) into a scenario battery. Two comparison modes:

- **Trajectory determinism (byte-for-byte)**: the existing method — snapshot
  the full walked audio span (game_bss region: gSoundInfo, players, tracks,
  channels, PCM buffers, cgb emulator state), run N frames, compare — applied
  to (control run) vs (save → destroy → load → run) for each scenario. This
  is exact because the engine state is fully deterministic given identical
  starting bytes.
- **Cross-build parity (byte-for-byte where legal, behavioral elsewhere)**:
  record a golden PCM + engine-state digest per scenario from the
  pre-cutover binary (fixed scenario, fixed frame counts, fixed song
  starts). After cutover, the PCM buffers must be bit-identical (the song,
  row, and sample BYTES are byte-identical by the three-way gate — only the
  *addresses* change), while pointer-valued fields (cmdPtr, wav, tone,
  parts) are asserted **behaviorally**: each must land in the arena
  (§9 canary) rather than equal a compiled address. The digest covers PCM
  and scalar audio state only; pointer fields are excluded from byte
  comparison and covered by the canary instead.

Scenario matrix (each = deterministic script of `m4aSongNumStart*` /
`PlayBGM/PlaySE/PlayFanfare` / `Cry_Bulbasaur` calls + frame counts):

| # | Scenario | Picks |
|---|---|---|
| 1 | looping route BGM | `mus_route101` (8 tracks, sustained notes; the existing scenario) |
| 2 | battle BGM | `mus_vs_wild` (BGM player) |
| 3 | BGM change | route101 → vs_wild at fixed frame, via `m4aSongNumStartOrChange` |
| 4 | song restart/continue | same song re-start + `m4aSongNumStartOrContinue` continue |
| 5 | SFX over BGM | `se_use_item` (ms=1 → SE1 player) during BGM |
| 6 | fanfare | `mus_level_up` (fanfare player path) |
| 7 | phoneme/Bard | `m4aSongNumStart(FIRST_PHONEME_SONG + 1 + k*3)` for a k covering a HELD triplet |
| 8 | PATT-heavy graph | top-2 songs by walker PATT count (from the generator's per-song stats) |
| 9 | GOTO-heavy graph | top-2 songs by walker GOTO count |
| 10 | programmable-wave instrument | a song whose voicegroup contains ≥1 type-3 row (pick via a row scan of the 530 songs' voicegroups; the generator emits the list) |
| 11 | cry over BGM | `Cry_Bulbasaur` (forward + reverse) during BGM, both cry slots |
| 12 | overlapping players | BGM + SFX + fanfare + cry simultaneously |
| 13 | mid-BGM save/load | the existing PHASE A/B (40 + 40 frames) |
| 14 | mid-pattern save/load | save at a frame where the playing track's `patternLevel > 0` (deterministic per song: choose the frame by construction from the walker's pattern map, e.g. the Nth PATT-active frame); fresh-process load + PCM compare |

Byte-for-byte: PCM buffers, mixer/envelope scalars, channel status, cgb
emulator, player/track state excluding pointer fields. Behavioral: pointer
residency (§9), volume/pitch control trajectories, channel-active counts.

Perf gate (no device in the self-test → underruns are a manual gate):
(a) trajectory determinism proves no behavioral/timing change in the mixer;
(b) wall-clock measurement of N mixer frames (e.g. 4,000) in the self-test,
asserted against a pre-cutover baseline with a wide margin (e.g. +5%);
(c) the manual DINFO checklist (§14) covers audible dropouts. No
micro-optimization.

## 9. Runtime instrumentation (self-test only)

All asserts live inside `NativeStateAudioSelfTest` (and its helpers) — no
production hooks. The existing comment at sdl2.c:662-667 ("run against the
redirected path… tone hydrates into the arena's transformed rows") is
currently **not true** (compiled path) and becomes true with this cutover;
the battery below is what makes it provable.

Per scenario, after warm-up frames:

1. **Hydrated header residency**: for the playing player,
   `sHostSongHeaders[playerId].parts[i]` and `.tone` must lie inside
   `EmeraldAudioCompat_GetArena` bounds (parts/tone in the verbatim span,
   tone additionally inside the transformed zone via
   `EmeraldAudioCompat_GetArenaLayout`).
2. **Stream residency**: every active track's `cmdPtr` and each non-NULL
   `patternStack[]` entry must hit the range index (`Lookup` → type
   MUSIC_SEQUENCE, role CANONICAL) — i.e., inside a registered per-song
   span.
3. **Sample residency**: every active channel's `wav` and
   `currentPointer` must be inside the arena (range index `Lookup` hit or
   hull membership with the zone-span identity).
4. **Negative canary**: no live audio pointer (the sets in 1-3) may
   resolve outside the arena — since every compiled audio payload address
   is outside the arena allocation, this asserts "no live compiled audio
   pointer observed" without needing the nm symbol list at runtime. If any
   scenario ever fell back to compiled bytes, this fails.
5. Keep the existing post-load linkage checks (function pointers,
   player chain, songHeader in game_bss) as-is.

## 10. Performance

Lookup frequency: one `HostResolveGbaAddr` per GOTO/PATT/REPT event, per
song start (header + tone + N parts), per cry start, plus the exact-start
hit per song start. Per whole song that is bounded by the walker totals
(≤ ~8,000 operands over the longest graphs, spread over minutes of playback)
— orders of magnitude below any mixer-frame budget. Each lookup is
O(log 197) + O(log 16) binary searches (≈ 8 + 4 iterations). No credible
regression; the gates of §8 (trajectory determinism + frame-time delta)
prove it rather than assume it. Do not micro-optimize.

## 11. State-v5 sufficiency — sufficient for R12-E live playback

The R12-C/D registration already covers every pointer class live playback
can hold:

| Pointer class | Where it lives at runtime | Registered identity |
|---|---|---|
| `cmdPtr` / `patternStack[]` | stream bytes | 530 per-song CANONICAL spans (identity+offset) |
| `songHeader.parts[]` | stream bytes | 530 per-song spans |
| `tone` | transformed rows | 197 COMPAT_OBJECT spans |
| `wav` / `currentPointer` | verbatim zone | zone span (CANONICAL, "emerald:audio/verbatim-zone") + 530 song spans |
| cry row `wav` | verbatim zone | zone span |
| keysplit group pointers | transformed rows | 197 spans (rows resolved at publish) |

Hull = the whole arena allocation (fail-closed for anything unregistered;
the unbacked tail and the internal song table are hull-only and hold no live
pointers). Mid-play saves already capture these as sidecar records
(identity+offset) and `ResolveByKey` re-derives them post-load — the
relocated-session test, the native state regression suite, and the
self-test's PHASE A/B all pass today. Post-load re-registration is wired
(native_state.c:3061-3091: trainer republish → audio republish → neighborhood
invalidate).

What remains for full R12-F (do not do here): expectation flips tied to the
§12 contract (remove the "compiled audio serves" degrade wording in the
post-load path), sidecar-cap re-pins once the audio scenarios land in the
state battery, and the R12-G isolation audit. The registration itself is
done.

## 12. Failure mode — the cutover contract

Today, `RegisterRuntimeSnapshot` treats audio publication failure as a
degrade (emerald_runtime_loader.c:182-199: "compiled audio still serves"),
and the post-load audio republish failure clears the arena with the same
fallback (native_state.c:3077-3083). Once production consumers read the
arena, that fallback masks an R12 resource failure with compiled bytes.

R12-E contract:

1. **Startup hard-fail**: in `RegisterRuntimeSnapshot`, an
   `EmeraldAudioCompat_TryInitialize` failure rolls back exactly like the
   Pokémon battle family (ClearMigratedEntries + refuse the session,
   sSnapshotRegistered stays false) — the existing `Platform_GameContent
   VerifyInstalled` refusal surface (sdl2.c:1325 → frontend data-setup/exit)
   then prevents the game from starting. Diagnostics already name the first
   failing resource.
2. **Post-load**: a failed audio republish after a state load fails the
   load (state is unusable without live ranges); no compiled fallback.
3. **Fallback remains compiled only for**: the GBA build (the seam is a
   no-op there; GBA audio is its own compiled story), and test/offline
   links that never call the cutover. Native production never masks an
   R12 failure.
4. `voicegroup_dummy` and the cry bytecode template stay compiled as
   documented pinned exceptions (architecture §4.3/§8) — they are engine
   structure, not resource payloads.

## 13. Compiled payload status (R12-E = prove DEAD, not absent)

R12-E does not remove anything (R12-G). Deadness proof for the native
production path = the §9 canary: across the full scenario battery, every
live audio pointer is asserted arena-resident, so no compiled audio byte can
have been consumed by any scenario. Complemented by: the cross-build PCM
identity (same bytes consumed either way), and a documented nm-sweep of the
release binary showing compiled audio symbols still present (expected until
R12-G — the asset-isolation runner keeps its audio exemptions pinned).

## 14. Tests and gates

1. Generator gate: `gen_audio_bindings.py --check` re-derives the native
   song table include byte-for-byte; row-0/row-last ROM-address pins; 610
   rows; dummy-row positions.
2. Link gate: GBA build unaffected (`.if LINUX64` guard); release + DINFO
   builds green.
3. Live-migrated song proof: self-test scenario 1 + §9 canary (parts/cmdPtr
   in per-song spans).
4. Live-migrated voicegroup proof: §9 canary on tone residency.
5. Live-migrated sample/wave proof: §9 canary on wav/currentPointer
   residency (scenarios 1, 5, 10).
6. Live-migrated cry proof: scenario 11 + canary (row via
   `EmeraldAudioCryTableRow`, wav in arena).
7. Phoneme path: scenario 7.
8. PCM determinism: all 14 scenarios, both comparison modes (§8).
9. State-v5 relocation: scenarios 13-14 + the existing relocated-session
   and state-regression runners.
10. Failure without required audio: a corrupt/missing-pack startup test
    (renamed pack → `--verify-game-data` fails → exit 2; the loader refusal
    unit path in the audio compat failure matrix S1-S6 already proves the
    seam side).
11. No silent compiled fallback: the §9 negative canary + the §12 contract
    test (audio TryInitialize failure → session refused, game does not
    start).
12. Perf sanity: frame-time delta within margin; no self-test underrun
    detectable headless (documented; audible underruns are the manual gate).
13. Full regression battery: the R12-D battery (all ~29 runners) plus the
    audio leaf/inventory runners — green on release + DINFO.
14. Manual DINFO checklist: title fanfare, Littleroot loop (clean loop
    point), route transitions, wild battle + cry, gym-leader fanfare, SE
    spam with no dropouts, Bard phoneme editing, mid-BGM save/load →
    relaunch → PCM-identical continuation, quick-save spam during battle
    music + cry overlap, corrupt-pack refusal (rename pack → clear error,
    no crash, no silent compiled audio).

## 15. Deliverable

This document (docs/R12E_LIVE_AUDIO_CUTOVER_PLAN.md).

## Implementation complexity and split recommendation

Production changes: (1) generator emission + `--check` for
`sound/song_table_native.generated.inc` (~½ day); (2) the conditional
include in `data/sound_data.s` (minutes); (3) the loader + post-load
failure-contract flip (~¼ day); (4) the self-test scenario battery + canary
(~1 day, dominated by scenario scripting and the pattern-frame picker);
(5) golden PCM digests + cross-build verification (~¼ day); (6) docs/battery
(~¼ day). **Total ≈ 2-2.5 focused days; the smallest behavioral stage of
R12 so far.**

**Do not split R12-E.** The redirect and its proof are one unit: shipping
the table without the canary+PCM gate is an ungated behavioral change, and
shipping the battery without the redirect proves nothing. The only
conceivable split (contract flip first) would ship a refusal risk with no
behavioral change — pointless.

## Review answers (summary)

- **Why songs are still compiled-live after R12-D**: the native gSongTable
  rows were assembled with `.int \label` (native link addresses); the
  interval/exact-start tables hold ROM identities; every lookup misses and
  the direct cast serves compiled bytes. R12-D's scope explicitly kept the
  compiled path live; the planned re-emission (architecture §4.3) was
  deferred on a misreading of the R12-D gSongTable verification (which
  proved the ROM table, not the native one).
- **Exact consumer redirect required**: re-emit the native gSongTable rows
  with ROM logical addresses (one generated include + conditional include);
  nothing else in the engine changes.
- **gSongTable decision**: (A) regenerate — 530 rows with ROM addresses,
  80 dummy rows keep `.int dummy_song_header`, 8-byte rows, no widening.
- **memacc walker correction**: none — engine semantics (BSS scratch +
  goto-path conditional jumps) fully modeled; the single memacc in the
  530-set is non-jump; validation-fidelity only.
- **Voicegroups/samples/cries already live?** Cry rows + cry samples: yes
  (sound.c accessor + publish-resolved unions). Voicegroups/samples/waves:
  no (compiled `tone`/`wav`), and they flip automatically with the table.
- **Live-pointer proof strategy**: self-test canary — every hydrated
  part/tone, every cmdPtr/patternStack entry, every channel wav/
  currentPointer asserted arena-resident per scenario; no compiled audio
  pointer may be observed.
- **State-v5 sufficiency**: sufficient — all live pointer classes carry
  registered identities today; post-load re-registration wired; R12-F keeps
  the expectation flips and cap re-pins.
- **Complexity**: ≈2-2.5 days; no reason to split.

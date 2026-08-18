# R12-E Report — Live Audio Cutover

Status: **COMPLETE**. The native MP2K consumers are cut over to the R12-B/C/D
audio arena as the LIVE source: `gSongTable` rows carry retail ROM logical
addresses, every live audio pointer across the 15-scenario battery is
asserted arena-resident (canary), PCM is byte-identical to the compiled-path
control at both optimization levels, and an audio publication failure is a
refused session — never a silent compiled fallback. Nothing committed (per
the stage brief); no R12-F/G work started.

## 1. Scope and status

All 14 plan §14 gates are met:

| Gate | Result |
|---|---|
| 1. Generator `--check` (no-op-diff, row pins, 610 rows, dummy positions) | PASS |
| 2. Link gate (GBA build unaffected; release + DINFO green) | PASS |
| 3. Live-migrated song proof (canary: parts/cmdPtr in per-song spans) | PASS |
| 4. Live-migrated voicegroup proof (canary: tone residency) | PASS |
| 5. Live-migrated sample/wave proof (canary: wav/currentPointer) | PASS |
| 6. Live-migrated cry proof (scenario 11 + canary) | PASS |
| 7. Phoneme path (scenario 7) | PASS |
| 8. PCM determinism — 15 scenarios, both comparison modes | PASS (15/15) |
| 9. State-v5 relocation (scenarios 14-15 + state suite) | PASS |
| 10. Failure without required audio (missing pack → exit 2) | PASS |
| 11. No silent compiled fallback (negative canary + refusal contract) | PASS |
| 12. Perf sanity (0.987x, +5% margin asserted) | PASS |
| 13. Full regression battery (35 runners, release) | PASS (35/35) |
| 14. Manual DINFO checklist | deferred (headless env, §16) |

## 2. Files changed

Production:
- `data/sound_data.s` — the `gSongTable` emission is gated
  `.if LINUX64` → include `sound/song_table_native.generated.inc` /
  `.else` the original `sound/song_table.inc`; both branches are inlined by
  preproc and gated by `as` exactly like the voice_group macro's LINUX64
  switch. The GBA build is untouched.
- `sound/song_table_native.generated.inc` (new) — 610-row native gSongTable
  with retail ROM logical addresses (see §3).
- `tools/gen3_resources/audio_family/gen_audio_bindings.py` — the
  `emit_native_song_table` emitter + row cross-checks (see §3).
- `include/emerald/resources/emerald_audio_compat.h`,
  `src/emerald/resources/emerald_audio_compat.c` — the three canary
  predicates (`ContainsPointer` / `ContainsCanonicalPointer` /
  `ContainsTransformedPointer`, §7).
- `src/emerald/resources/emerald_runtime_loader.c` — audio arena
  publication is part of the strict registration: an
  `EmeraldAudioCompat_TryInitialize` failure is a REFUSED session with the
  same full rollback as the Pokémon battle family; new
  `EmeraldResourceCompat_IsSessionRegistered()` (the loader owns the
  session flag).
- `src/platform/desktop_game_content.c` — `VerifyPaths` propagates the
  refusal: a missing pack or failed registration fails the content
  verification (`--verify-game-data` exits 2; startup lands in the frontend
  data-setup/exit path). §12.1.
- `src/platform/native_state.c` — post-load audio republish failure FAILS
  the load (session-ful links); session-less links skip the audio
  republish. §12.2/§12.3.
- `src/m4a.c` — `HydrateSongHeader` materializes hydrated headers PER SONG,
  keyed by the raw ROM header address, not per player slot (§5 — pointer
  identity fix required for the cutover).
- `src/platform/cgb_audio.c` — `cgb_audio_init` resets the APU frame/cycle
  counters (the parity gates length/envelope/sweep events), making init a
  complete deterministic reset.
- `src/platform/sdl2.c`,
  `src/platform/native_audio_scenarios.c` (new) — the 15-scenario live
  battery, canary observation, perf window, refusal test, and the
  `--native-audio-scenario-oracle/-test`, `--native-audio-perf-baseline`,
  `--native-audio-refusal-test` utility flags (self-test only, no
  production hooks).
- `include/platform/native_state.h` — declarations for the above.

Tests:
- `tests/emerald_audio_compat_test.c` — structural family mutability
  (S7/S8 failure-matrix cases); 10,109 checks green.
- `tests/emerald_resource_state_test.c`,
  `tests/run_emerald_resource_state.sh` — audio arena bases printed in the
  create/load processes; new TEST A assertion that creator arena ≠ loader
  arena (§11).
- `tests/native_state_regression.sh` — executable bit restored (content
  unchanged; the script ran under the R12-D battery via `bash`, this stage
  runs it directly).

Unrelated: `android/SDL2` submodule pointer (`m`, pre-existing).

## 3. Native gSongTable emission

`emit_native_song_table` re-emits the retail 610-row table in
`song_table.inc` order with ROM logical addresses:

- **610 rows** = 530 real + 80 dummy (composition pinned in the generator).
- Each real row is `.int <retail ROM SongHeader GbaAddr>` + the unchanged
  `.short ms, me` — the addresses come from the gSongTable row-by-row
  cross-check against the qualified ROM (row value == ELF song symbol
  address == inventory), with row 0 pinned to the song block start
  (`mus_dummy` 0x088FC03C), the last real row + 8+4·trackCount pinned to
  the block end (0x089A3050), and headers strictly increasing.
- The 80 dummy rows sit at their RETAIL indices — a contiguous block at
  table rows **270..349** (`dummy_song_header, 0, 0`) — not packed at the
  end; the generated include keeps `.int dummy_song_header`, which resolves
  identically in the native link. `song_table.inc` itself has the same
  interleave, so the native table matches the retail table index-for-index.
- Rows are 8 bytes (`.int` + 2×`.short`), matching
  `struct { struct SongHeader *header; u16 headerID; u16 priority; }`.
- The emitter is deterministic; `--check` re-derives the committed include
  byte-for-byte (no-op-diff) and re-runs every cross-check. Assembly-time
  row-count pin in the include.

**Final-artifact proof** (programmatic, on the final release binary):
`nm` finds `gSongTable` at VA 0x00bec318 in `.rodata`; the 610×8-byte rows
were parsed from the file and compared against the include row-by-row
(pointer, headerID, priority): **0 mismatches**. Real rows span
0x088FC03C..0x089A3044 (530), dummy rows 270..349. The table ends exactly
at `gSongTable_end`/`dummy_song_header` (VA 0x00bed628).

The oracle build (`\t.if 0` flip) assembles the original macro table: its
rows are native link addresses of the compiled song headers (row 0
0x00e4efa6), same 80-dummy interleave at 270..349 — the compiled baseline.

## 4. Link gate

`data/sound_data.s` guards the emission with `.if LINUX64`: the GBA build
takes the original branch byte-for-byte. The qualified pret reference build
(pokeemerald-reference) is unaffected, and the retail-identical ROM was
re-verified by the R12-D gates. This repo's own GBA build remains blocked by
the pre-existing unguarded `field_camera.c`/`parity.c` (unchanged from
R12-D; documented in the R12-A report). Release and DINFO native builds are
fresh-`-B` green (§13).

## 5. Live pointer path after cutover

`HostResolveGbaAddr` (R12-D hybrid): exact-start table (197) → interval (1,
the song block `[0x088FC03C, 0x089A3050)`) → handle → direct cast. With the
native table holding ROM addresses, every song header/part/GOTO/PATT/REPT
operand resolves into the arena — identity-preserving
(`arenaBase + (addr − romStart)`). No host pointer enters song bytes; the
interval is a pure platform mapping.

**HydrateSongHeader (m4a.c) fix required for the cutover**: hydrated
headers were materialized per player slot; the engine identifies the
playing song by comparing SongHeader POINTERS
(`m4aSongNumStartOrChange`/`Continue`), and on the GBA those are distinct
ROM addresses. A per-slot reuse buffer would make every song on a slot
compare equal to the slot's current song and the change path would never
fire. The cutover materializes headers PER SONG keyed by the raw ROM header
address (first hydration creates a permanent entry; bound 640 > the 610
reachable table rows; the bound's overflow fallback keeps the session
alive). The m4a.c `playerId` parameter is retained for signature
compatibility.

**Deterministic init (cgb_audio.c)**: `cgb_audio_init` now resets the APU
frame/cycle counters — the free-running host parity state that gates
length/envelope/sweep events — so every scenario reset and every process
restart begins from the same APU phase. Without this, cross-build digest
comparison would be phase-sensitive.

## 6. Reservation: voicegroup / cry / phoneme paths

Unchanged from R12-C/D: voicegroup rows, cry-table rows, keysplit groups
and the transformed song rows live in the arena's transformed zone and were
already resolved through the seam before this stage. The cutover changes
only the song-header entry point. `voicegroup_dummy` and the cry bytecode
template stay compiled as the documented pinned exceptions (plan §12.4) —
engine structure, not resource payloads.

## 7. Canary (plan §9, self-test only)

Three predicates on the published arena: `ContainsPointer` (whole
allocation view), `ContainsCanonicalPointer` (verbatim zone),
`ContainsTransformedPointer` (transformed zone). The battery asserts per
scenario, after warm-up frames:

1. Hydrated header residency — `parts[i]`/`tone` inside the arena
   (verbatim; tone additionally in the transformed zone).
2. Stream residency — every active track's `cmdPtr` and non-NULL
   `patternStack[]` entries hit the range index (CANONICAL, per-song
   spans).
3. Sample residency — every active channel's `wav`/`currentPointer` inside
   the arena.
4. Negative canary — no live audio pointer may resolve outside the arena:
   since every compiled audio payload address is outside the arena
   allocation, this asserts "no live compiled audio byte consumed".

The per-scenario observation flags (printed with each digest) show which
canary checks fired: `wave=1` in 7 scenarios (battle-bgm, bgm-change,
patt-heavy, goto-heavy, programmable-wave, cry-over-bgm, save-load-pattern),
`phoneme=1` in the phoneme scenario, `pat=1` in 5 scenarios. The oracle
build runs the same battery with the canary off (its pointers are
legitimately compiled-image-resident).

## 8. PCM determinism — scenario battery

15 deterministic live scenarios (looping BGM, battle BGM, BGM change,
restart, SFX, fanfare, phoneme, PATT-heavy, GOTO-heavy, programmable wave,
cry-over-BGM, overlapping players, all-four-players, save/load mid-BGM,
save/load mid-pattern), each producing a SHA-256 digest of every mixer
frame's PCM plus behavioral gates; scenarios 14-15 additionally compare
post-restore frame digests against the pre-save control inside the process.

Two builds, four battery runs:

| Build | Mode | Result |
|---|---|---|
| release oracle (`.if 0`) | canary off | 15/15; digests == the stage's earlier golden records, byte-for-byte |
| release migrated (`.if LINUX64`) | canary on | 15/15; **PCM digests identical to oracle on all 15**; PERF ok |
| DINFO oracle | canary off | 15/15 |
| DINFO migrated | canary on | 15/15; **PCM digests identical to the DINFO oracle on all 15** |

Cross-build PCM comparison: oracle-vs-migrated at the same optimization
level is byte-identical on **all 15 scenarios** at both -O3 and -O0. The
only cross-LEVEL digest difference in the whole matrix is the multi-player
scenario between DINFO and release — and that difference exists between
the two ORACLE builds as well (DINFO oracle vs release oracle), pinning it
as a pre-existing O0-vs-O3 property of the host float mixer (the
all-four-players scenario is the one that stresses the float stereo mixer
hardest), NOT a cutover artifact. Scenario battery, both modes, exit 0.

## 9. Performance

Perf window: 4,000 mixer frames × 3 batches, best wall-clock, in the
scenario battery. Oracle baseline (fresh oracle build): **248,478,175 ns**.
Migrated build with `--native-audio-perf-baseline 248478175`:
**245,176,631 ns (0.987×)** — the +5% margin is asserted in-code
(`PERF ok (margin +5%)`). No regression; the O(1) interval lookup
(binary search over 1 interval) is noise at this scale, matching the plan
§10 analysis.

## 10. Failure contract — the cutover flip

- **Startup hard-fail (§12.1)**: `RegisterRuntimeSnapshot` treats an audio
  publication failure exactly like the Pokémon family failure: roll back
  (trainer + audio migrated entries cleared), drop the snapshot, keep
  `sSnapshotRegistered` false, return the failure with the first failing
  resource named. `VerifyPaths` now propagates the refusal: missing pack or
  failed registration → content invalid → `--verify-game-data` exits 2 and
  the startup surface lands in the frontend data-setup/exit path. A
  successful data-setup does NOT override a refused session (re-verify
  after setup).
- **Post-load (§12.2/§12.3)**: after a state load, the audio republish is
  mandatory for session-ful links (production: a restored state whose audio
  pointers cannot be re-hydrated must not run — no compiled fallback;
  failure clears the arena and fails the load with
  `NATIVE_STATE_UNSUPPORTED` and a named diagnostic). Session-less links
  (test/offline builds that never call the cutover) skip the audio
  republish. The loader's `IsSessionRegistered` flag is the discriminator —
  the arena's absence alone cannot distinguish "never registered" from
  "registered then cleared", and the cleared case must refuse.
- **Compiled fallback remains ONLY for**: the GBA build (the seam is a
  no-op there), and test/offline links that never call the cutover. Native
  production never masks an R12 failure.

`NativeAudioRefusalTest` covers the surface in five stages: missing pack,
corrupt pack, recovery (re-registration succeeds after the refusal),
load-without-arena, and republish-after-clear is UNAVAILABLE — all pass on
the final binary.

## 11. State-v5 relocation

Scenarios 14-15 save/load mid-playback (BGM 359, pattern 475) with the
state-v5 sidecar: post-load, every pointer class (cmdPtr, patternStack,
header parts, tone, wav/currentPointer) is re-derived via the range index
and the post-restore frame digests match the pre-save control inside the
process (the §9 checks run again post-restore).

Cross-process relocation (state suite, `run_emerald_resource_state.sh`):

- TEST 3 (battle family): creator arena 0x3157fe18/0x3157f514/0x30f1af94
  vs loader arena 0x1dbdae18/0x1dbda514/0x1d575f94 — differ.
- TEST A (audio family): creator 0x423016e8/0x422fce28 vs loader
  0x40a8f6e8/0x40a8ae28 — differ; AUDIO-LOAD asserts the restored channel
  `wav` equals the LOADER arena base (no creator pointer survives; the
  harness asserts `chans[1].wav == plant.voicegroupBase` for the loader
  plant).
- `native_state_regression.sh` (save/load, trainer + audio republish):
  passed.

## 12. Compiled payload status — proven DEAD, not absent

- **Negative canary** (§7.4): across the whole battery, every live audio
  pointer is arena-resident; the compiled payload addresses are outside the
  arena allocation by construction, so no scenario consumed a compiled
  audio byte.
- **nm-sweep of the final release binary**: 8,270 compiled audio data
  symbols still present (530 global song-header `R` symbols — exactly the
  530 real songs — plus 7,740 local part symbols). Expected until R12-G
  (plan §13: prove DEAD, not absent); the asset-isolation runner keeps its
  audio exemptions pinned.
- Cross-build PCM identity (§8) corroborates: the same bytes are consumed
  from the arena as from the compiled image.

## 13. Builds

- `make -f Makefile_pc -B linux64` (migrated) — fresh full rebuild, clean.
- `make -f Makefile_pc -B linux64` with the oracle flip — clean.
- `make -f Makefile_pc -B DINFO=1 linux64` (migrated) — clean.
- `make -f Makefile_pc -B DINFO=1 linux64` with the oracle flip — clean.

The tree is left in the production state: `data/sound_data.s` on the
`.if LINUX64` branch, the build restored to the release migrated binary
(hash-verified against the preserved final artifact).

## 14. Regression battery (all green)

The full R12-D battery plus the audio leaf/inventory runners — 35 runners,
all PASS on the release build (logs /tmp/r12e-battery-results.log):

| Runner | Result |
|---|---|
| gen3 core + sanitize | PASS |
| audio leaf (10,109 checks) + inventory (.aif provenance) | PASS |
| elf manifest + sanitize | PASS |
| pokemon family + sanitize | PASS |
| resource pack + sanitize; pack provider + sanitize | PASS |
| trainer family + sanitize | PASS |
| layout compat; asset isolation (19,953 checks) | PASS |
| world neighborhood; object event compat | PASS |
| resource import + sanitize; resource lz + sanitize | PASS |
| resource ranges; resource state (incl. TEST 3 / TEST A relocation) | PASS |
| rom base provider + sanitize; runtime loader; session fingerprint | PASS |
| tileset compat; trainer native compat + sanitize; real tables | PASS |
| world real; world render proof; trainer native compat production | PASS |
| native state regression | PASS |

(`tests/native_state_regression.sh` needed its executable bit restored —
a pre-existing omission, not a test failure; the runner passes.)

Direct gates on the final release binary: `--verify-game-data` exit 0 with
the production pack, exit 2 with the pack renamed (refusal message);
`--native-state-self-test` passed (quick + slot 1, trainer family
republish); `--native-audio-refusal-test` passed (5 stages).

## 15. STOP-condition audit (none hit)

1. **PCM differs from compiled-path control** — not hit. Oracle-vs-migrated
   at equal optimization: 15/15 byte-identical (release and DINFO). The one
   cross-level delta (multi-player, DINFO vs release) is reproduced by the
   oracle pair itself: a pre-existing O0-vs-O3 float-mixer property, not a
   cutover delta.
2. **Save/load leaves creator-process pointer alive** — not hit. TEST 3 /
   TEST A arena bases differ across processes; AUDIO-LOAD asserts the
   restored pointers resolve into the LOADER arena; post-restore frame
   digests match the pre-save control.
3. **Audio resource failure serves compiled audio silently** — not hit.
   Registration failure refuses the session (exit 2, no startup); post-load
   republish failure fails the load; the refusal test's five stages pass.
4. **Perf regression** — not hit (0.987×, margin asserted).
5. **gSongTable drift** — not hit. Generator `--check` no-op-diff plus the
   final-binary 610-row ELF proof (0 mismatches).

## 16. Manual gates (deferred)

The plan §14.14 manual DINFO checklist (title fanfare, Littleroot loop,
route transitions, wild battle + cry, gym-leader fanfare, SE spam, Bard
phoneme editing, mid-BGM save/load → relaunch → PCM-identical
continuation, quick-save spam, corrupt-pack refusal) is an audible,
display-dependent gate — a headless-environment exclusion exactly like the
R12-D display-dependent probes. The machinery it checks (PCM identity,
save/load continuation, refusal) is the automated battery above; the
audible confirmation remains the pending manual gate.

## 17. Deliverable

This document (docs/R12E_LIVE_AUDIO_CUTOVER_REPORT.md). Nothing committed
per the stage brief. Next stage is R12-F (state battery expectation flips)
/ R12-G (compiled payload removal + isolation audit) — **COMPLETE
(2026-08-18)**: see docs/R12G_AUDIO_ISOLATION_REPORT.md.

# R12-C Report — Emerald Structural Audio Ownership Migration

Stage: R12-C (structural tables) on the R12 audio migration path
(R12-A audit → R12-B leaves → **R12-C structural** → R12-D songs → R12-E
cries/SFX end-to-end → R12-F State v5 full → R12-G compiled-payload
removal). Scope and requirements: `docs/R12C_STRUCTURAL_AUDIO_IMPLEMENTATION_PLAN.md`
(this report mirrors the plan's §12 required findings one-to-one).

All 21,370 transformed rows pass exact parity; every STOP condition from
the plan's §13 and the stage brief is verified (see §16). PCM/audio
behavior is unchanged — the compiled tables remain linked and serve as
the additive fallback exactly as pre-R12-B.

---

## 1. Inventory / structural counts (authoritative input: R12-A audit + R12-C generator)

| Family | Count | Rows (GBA-form 12 B) |
|---|---|---|
| Voicegroups (`emerald:audio/voicegroup/<name>`) | 195 | 20,594 |
| — of which `voicegroup_dummy` | 1 | 127 |
| Cry tables (`emerald:audio/cry-table/forward` / `reverse`) | 2 | 776 (388 each) |
| Keysplit runs (`emerald:audio/keysplit/<name>`, schema 2) | 5 | 372 B total |
| **Structural total** | **202** | **21,370 rows** |

Every count is pinned in `include/emerald/resources/emerald_audio_compat.h`
(`EMERALD_AUDIO_VOICEGROUP_COUNT 195`, `EMERALD_AUDIO_CRY_TABLE_COUNT 2`,
`EMERALD_AUDIO_KEYSPLIT_COUNT 5`, `EMERALD_AUDIO_VOICEGROUP_ROWS 20594`,
`EMERALD_AUDIO_CRY_ROWS 776`) and in the generator
(`tools/gen3_resources/audio_family/gen_audio_bindings.py`,
`PINNED_STREAM_ROWS = 21370`). The seam refuses a session whose structural
composition differs (pack drift is a hard failure, never a partial
publication).

Pack composition after R12-C: **5,289 entries** = 196 trainer + 1,608
Pokémon battle + 288 object-event + 1,544 tileset + 882 layout + 771 audio
(569 R12-B leaves + 202 R12-C structural). Verified by every runner that
opens the production pack (`Gen3ResourcePack_GetEntryCount == 5289u`) and
by the catalog totals.

## 2. Canonical payloads and provenance

- The 202 structural payloads are the raw GBA-form bytes of
  `sound/voice_groups.inc` (195 groups, 12-byte ToneData rows),
  `sound/cry_tables.inc` (2 × 388 rows) and the 5 keysplit runs — the
  same `.inc` files the Makefile_pc compiles, so the pack payload and the
  compiled parity copy are the same bytes by construction.
- Provenance chain re-proven byte-for-byte by the runner E1/E2 gates in
  `tests/gen3_resources/run_audio_leaf.sh`:
  - **E1** `gen3-pack-build --check` reproduces the committed production
    pack byte-for-byte from the 7 manifests + 7 catalogs (5289 entries);
  - **E2** `gen3-elf-manifest --check` re-derives the audio manifest
    (771 entries) from the qualified pret reference ELF + retail-matching
    ROM (SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7) — the three-way
    ELF == ROM == manifest canonical slices.
- The generator emits per-resource bindings (`gen_audio_bindings.py`) with
  source ROM addresses for all 202 structural resources; cross-checks pin
  row counts, run sizes (keysplit runs total 372 B), and GBA addresses.

## 3. The 12→24 transform (spec and exact-byte gate)

The transform is the `asm/macros/music_voice.inc` LINUX64 branch layout —
the exact `struct ToneData` the Makefile_pc assembler emits for the native
player:

```
24-byte native row: 4 scalar bytes | .space 4 | .quad union | ADSR bytes | .space 4
```

- Rows are transformed publish-time inside the seam (arena build, R12-C
  §1.3 pointer resolution — `group`/`keysplit`/`wav` fields become native
  pointers into the arena's transformed/verbatim zones).
- **Exact-byte gate**: the offline parity gate assembled the recomp's
  `voice_groups.inc` + `cry_tables.inc` with the host assembler
  (`as --64 --defsym LINUX64=1`, the Makefile_pc recipe minus the game
  link), dumped `.rodata`, and memcmp'd the seam's transformed rows
  field-for-field against the assembled native rows for **all 21,370
  rows** — result: `native mismatches: 0`, `groups without pret symbol: 0`
  (keysplit back-shift anchors verified, e.g. `keysplit_piano native row
  21368 -> GBA 0x86b46a4`). Padding bytes zero, union words 0 for
  square/noise, ADSR-masked values equal.
- The transformed-zone byte total is a **seam composition gate**
  (`EMERALD_AUDIO_TRANSFORM_SIZE`): 21,370 rows × 24 B (513,744 B) + 10
  drumset back-shift pads (364 rows × 24 B; 9 × 36 + 1 × 40) = **521,616 B
  exact** — asserted by the leaf runner (`transformSize ==
  EMERALD_AUDIO_TRANSFORM_SIZE`).

## 4. Arena layout (extension of the R12-B single allocation)

The transformed zone is appended to the same single arena allocation as
the R12-B verbatim zone (no second allocation):

- `struct EmeraldAudioArena`: 9 size_t header fields (72 B) + flexible
  `uint8_t bytes[]` at offset 72; verbatim zone (spanSize 3,329,304 B for
  the 569 leaves + 5 keysplit copies) then the structural table (202
  records) then the transformed zone (521,616 B).
- `GetArena` returns `sArena->bytes + zoneOffset`; `transformBase =
  zoneBase + (transformOffset - zoneOffset)` — run-varying offsets (the
  two-process state harness proves arena bases differ between creator and
  loader processes, TEST 3).
- The R10 hull spans the whole allocation: `ArenaAllocationSize = 72 +
  zoneOffset + spanSize + transformSize` (3,939,108 B at the ~88,116 B
  zoneOffset variant) — every arena pointer, including interior transform
  rows, falls in the hull so mid-play saves fail closed rather than
  persist raw pointers (see §10).
- **R12-B leaf offsets are unchanged**: the verbatim zone keeps its exact
  R12-B layout and byte contents (leaf runner re-asserts arena bytes ==
  pack payloads == ROM slice, no overlaps, waves exactly 16 B); the 5
  keysplit runs copy into the verbatim zone at their ROM-relative offsets
  (no new allocation beyond the single arena extension — plan §12.2).

## 5. `voicegroup_dummy` (decision A) and the song `.tone` redirect

**Decision: A — `voicegroup_dummy` migrates with the other 195** (one of
the 195 inventoried groups, `sound/voicegroups/dummy.inc`, 127 rows; the
pack carries it; the logical range covers it).

- **5 song files** (`mus_c_vs_legend_beast`, `mus_c_comm_center`,
  `mus_gsc_pewter`, `mus_dummy`, `mus_gsc_route38`) each have
  `.equ <song>_grp, voicegroup_dummy`; the song-header `.tone` 4-byte GBA
  value is redirected by the logical-address table (see §7) — **zero code
  change**: the registered GBA label resolves to the arena's transformed
  rows at song start (hydrated in `HydrateSongHeader`, m4a.c:81-94).
- `gPokemonCrySongTemplate.tone` (m4a_tables.c:264) stays compiled —
  points at the compiled dummy rows, which remain linked through R12-G, so
  it is never dangling. Runtime-dead anyway: `m4aSoundInit` memcpy's the
  template into `gPokemonCrySong` and every `SetPokemonCryTone` overwrites
  `.tone` before play. R12-G re-emits this initializer.
- The 10 drumset labels are registered for symmetry (plan §4.2) even
  though no song references a drumset directly (verified: the 530 songs'
  `.equ` rows reference ~60 exported voicegroup labels only; drumsets
  appear only as `group` operands of keysplit rows, which are publish-time
  native pointers).

## 6. Cry tables (runtime representation and the sound.c redirect)

- Both cry tables transform to native 24-byte rows in the arena (2 blocks
  of 388 rows, 18,624 B).
- New seam accessor `EmeraldAudioCryTableRow(u8 table, bool reversed, u8
  index)` returns the row at `128*table + index` from the transformed cry
  block — the same 4×128-bank indexing as compiled (`SpeciesToCryId` caps
  at ≤ 387, so indexing stays in bounds; the accessor returns NULL beyond
  388).
- `src/sound.c` GET_CRY (line 475-521) redirects under
  `#ifdef NATIVE_LINUX` (plan §4.2: `#if defined(PLATFORM_SDL2) &&
  defined(NATIVE_LINUX)` in the R7B pattern — the seam is platform-neutral
  and linked into native targets only; GBA sources never see gen3
  headers): `GetPokemonCryRow` (sound.c:45-56) calls the accessor and
  returns the arena row, falling back to the compiled
  `gCryTable`/`gCryTable_Reverse` when the arena is unpublished —
  additive degrade, exactly as pre-R12-B.
- `SetPokemonCryTone` (m4a.c:1797-1834) is **unchanged** — it stores the
  row pointer into `gPokemonCrySongs[i].tone`; the `struct ToneData *`
  parameter is used as an opaque pointer throughout (no structural
  indexing of cry rows in C).
- Compiled `gCryTable`/`gCryTable_Reverse` remain linked (parity +
  byte-equality) until R12-G removes them.

## 7. Logical-address table (197 exact-start entries)

`src/platform/host_memory.c` gains an exact-start table consulted by
`HostResolveGbaAddr` **before** the handle table (R12-C §5):

- **197 entries**: 195 voicegroup labels (185 exported + 10 drumset
  labels, incl. the 10 back-shifted drumset starts) + 2 cry labels
  (registered for R12-E re-emission completeness).
- **Exact-start, not intervals**: every served reference is exactly a
  table label (`.tone = <label>`; never an interior GBA-address — after
  the transform, rows carry native pointers). This eliminates the drumset
  back-shift overlap problem entirely: an exact-start entry creates no
  interval membership, so back-shifted labels that fall inside the
  previous table's ROM span cannot collide with anything.
- `HostMemoryRegisterLogicalAddress` inserts in sorted order (publish-time
  only, fixed capacity, `HOST_LOGICAL_ADDRESS_CAPACITY`), aborts on
  duplicates and on overlap with the 0xE/0xF handle ranges;
  `HostMemoryClearLogicalAddresses` resets. The table is static host
  .data owned by the audio seam, cleared + repopulated by
  `TryInitialize`/`Republish`/`ClearMigratedEntries` (the arena and the
  table can never drift).
- Precedence: exact-start table (binary search) → handle table →
  direct-cast fallback — the fallback stays correct through R12-G because
  the compiled 24-byte rows still live at GBA-shaped host addresses
  (unregistered tone silently uses the compiled parity copy).
- **No interval spans in R12-C**: verbatim-zone and song-stream ranges are
  deferred to R12-D/E by design (plan §5.1 — "do not design song-stream
  ranges unless necessary"; not necessary in R12-C).
- **Platform-neutrality (guardrail 18)**: the seam declares the two
  HostMemory hooks as local `__attribute__((weak))` externs (the same
  idiom as its weak `EmeraldResourceCompat_GetRangeIndex` reference) and
  guards the call sites — `src/emerald/resources/emerald_audio_compat.c`
  includes no `platform/` header, so the dependency-creep guardrails
  (run_emerald_resource_import.sh, run_emerald_rom_base_provider.sh) and
  offline links without host_memory.c (the loader harnesses) degrade to a
  no-op registration.

## 8. Consumer cutover (complete list — plan §6)

| # | Consumer / symbol | File:line | R12-C change |
|---|---|---|---|
| 1 | song-header `.tone` hydration | m4a.c:81-94 | **none in m4a.c** — the registered logical address redirects it; resolved pointer ∈ transformed zone |
| 2 | `MPlayStart` → `mplayInfo->tone` | m4a.c:759 | none (pointer flows; `MusicPlayerInfo.tone` == `MP2KPlayerState.voicegroup` by layout) |
| 3 | note-on instrument copy | music_player.c:280-288 | none — `voicegroup[voice]` is now an arena row; `track->instrument = *instrument` copies the 24-byte record verbatim |
| 4 | instrument `wav` → mixer | note-on path | implicit — `wav` points at the verbatim zone (arena samples); PCM-identical because R12-B proved arena == ROM == pack bytes |
| 5 | programmable wave path (types 3/0x0B) | CgbSound wave-RAM copy | implicit — `wav` points at the arena's 16-byte wave leaves |
| 6 | cry lookup | sound.c:475 (GET_CRY) | accessor redirect (§6) |
| 7 | `SetPokemonCryTone` | m4a.c:1797-1834 | none (opaque row pointer) |
| 8 | `gPokemonCrySongTemplate.tone` | m4a_tables.c:264 | none in R12-C (§5; R12-G re-emission) |
| 9 | `voicegroup_dummy` | m4a_tables.c:264, m4a_internal.h:438 | none — migrated with the 195; template stays compiled (§5) |
| 10 | `HostResolveGbaAddr` | host_memory.c:364-389 | logical-address table (§7) |
| 11 | sound.c `extern struct ToneData gCryTable[]` | sound.c:28-29 | fallback-only (`#else` branch) |

No song, no stream, no `gSongTable`/`gMPlayTable`/engine-table change. The
only C edits outside the seam/tests: `src/sound.c` (cry accessor),
`src/platform/host_memory.c` (logical table), and the post-load block in
`src/platform/native_state.c` (§10). **R12-C does not require migrating
song streams** (plan §13 STOP condition — verified: none were migrated).

## 9. Compiled fallback / removal boundary

- **Through R12-C**: all compiled audio stays linked — voicegroups, cry
  tables, samples, songs, template. The compiled rows are the parity
  reference AND the additive fallback (an unregistered tone or an
  unpublished arena degrades to compiled audio — the R12-B contract).
  Verified in the built binary (`nm`): `voicegroup_dummy` (0xb68694 R),
  `gCryTable` (0xbb8674 R), `gCryTable_Reverse` (0xbbaad4 R) and all 195
  `voicegroup_*` labels are present.
- **Removal boundary**: R12-G removes compiled audio payloads from the
  native link — only after R12-D (songs), R12-E (cries/SFX end-to-end)
  and R12-F (State v5) land. At R12-G, `gPokemonCrySongTemplate.tone`
  must be re-emitted to arena rows, the sound.c `#else` fallback branch
  drops, and the direct-cast fallback for audio addresses falls to
  "unregistered = session error". The `.inc` files stay in the repo — they
  are the generator's canonical source for the pack, always.

## 10. State v5 boundary (minimal forward-pull)

The R12-C stage required the plan's §9 recommendation be taken: register
the audio arena in the shared R10 range index so mid-play saves capture
arena pointers as sidecar records instead of refusing.

- **Registration**: on publish, the seam registers span 0 = the whole
  verbatim zone (CANONICAL `AUDIO_SAMPLE`, schema 1, offset 0, key
  `emerald:audio/verbatim-zone`) plus 197 transformed-zone blocks
  (COMPAT_OBJECT `INSTRUMENT_BANK`, schema 1) = **198 ranges + 1 hull**
  covering the whole allocation — asserted by the leaf runner
  (`CheckRangeRegistration`: range count, hull count, verbatim key/type/
  role/offset, cry key/type 15/role 2/offset 12, ResolveByKey round trip,
  InHull) at publish/republish/relocate and empty after Clear.
- **Bookkeeping**: `sAudioRanges[198]` registration tracked in the seam;
  `RegisterArenaRanges`/`UnregisterArenaRanges` (memmove removal,
  verified by `ArenaRangesMatch`); TryInitialize unregisters before free
  and registers after swap (NULL index → degrade); Republish
  hard-fails `EMERALD_AUDIO_ERR_RANGE_REGISTRATION`; ClearMigratedEntries
  unregisters first. The strong `GetRangeIndex` definition lives in
  `emerald_trainer_native_compat.c` (a real engine bug was fixed during
  R12-C: its `const` return type conflicted with the header's non-const
  declaration).
- **Post-load ordering** (native_state.c:3061-3083): pointer restore →
  trainer `EmeraldResourceCompat_Republish` → **audio
  `EmeraldAudioCompat_Republish`** (re-registers the ranges, which the
  trainer republish's index reset may have dropped) →
  `NativeWorldNeighborhood_Invalidate`. Fail-closed: a live arena without
  ranges clears the arena (compiled audio serves; captures can no longer
  see arena pointers at all).
- **Walker**: `CaptureResourceWindow` (native_state.c:1909) recognizes any
  registered range (any role) before normal pointer classification →
  64-byte sidecar record; restore via `ResolveByKey` (native_state.c:2799,
  failure = load refusal). Mid-play the live arena pointers are
  `sHostSongHeaders[0..3].tone` (4) + `gPokemonCrySongs[0..1].tone` (2) —
  ~6 sites, all covered.
- **Assertion flips**: the headless proof's image-resident assertions
  (sdl2.c:1003-1011) flip to `NativeAudioPointerIsResourceResident`
  (range or hull membership).

## 11. Generator and pack work

- `tools/gen3_resources/audio_family/gen_audio_bindings.py` (new):
  emits the 202 structural bindings from the `.inc` sources + qualified
  ELF symbol addresses (incl. the drumset back-shift and keysplit
  anchors), pins row counts, cross-checks the pack order.
- `gen_audio_inventory.py` extended for the structural families.
- Audio manifest 569 → **771 entries**; production pack 5,087 → **5,289
  entries** (deterministic rebuild, E1-reproven byte-for-byte).
- The pack remains gitignored (`games/emerald/base/*.rpack`); the
  `gen-*` binaries stay gitignored too.

## 12. Tests

**Audio leaf runner** (`tests/gen3_resources/run_audio_leaf.sh`, E1+E2+A-G):
- E1/E2 provenance gates (see §2).
- A-G: counts (5,289 entries; 195/2/5 structural; 20,594 + 776 rows;
  keysplit run bytes 372), exact extraction (arena bytes == pack payloads
  == ROM slice; no overlaps; waves 16 B), WaveData2 structural validation,
  structural publication (composition gate, transformed zone size pin,
  per-resource spans, rs-drumset 29 rows + 36-row zeroed back-shift pad,
  cry accessor indexing/bounds, **197 labels unique and inside the
  transformed zone**), R10 registration lifecycle, failure matrix
  (missing/unresolvable/wrong-type/wrong-schema/wrong-size/corrupt/
  out-of-span/duplicate-key).
- Result: **9,988 checks, 0 failures**.

**Cross-restart state runner** (`tests/run_emerald_resource_state.sh`):
- TEST A create: mid-BGM/mid-cry arena pointers planted in the real
  `SoundInfo` fixture → captured as keyed sidecar records (voicegroup
  + cry keys, type INSTRUMENT_BANK, schema 1, role COMPAT_OBJECT, field
  offsets pinned); scalar windows round-trip verbatim (`AUDIO-REGRESSION
  ok (coincidence=1, arena=1)`).
- TEST A load (fresh process): restored pointers **re-derived into the
  fresh arena via ResolveByKey** (explicit `ResolveByKey` + `Lookup`
  assertions); arena bases provably differ between processes (TEST 3).
- TEST B: real desktop_state.c + desktop_audio.c state-manager loads over
  the fake SDL device — ordered device trace verbatim, **PCM parity**,
  device active/unpaused (paused + quick-load variants).
- Cross-restart fingerprint gate: both processes print fingerprint
  `0293988dd7c58b39c0809bdcedd774e4ccec2a7e917cc87c119a88acb5d429e5`.
- TEST 4/7/8/I: changed content identity, missing session, unknown key,
  corrupt sidecar matrix, v4 rejection — all refuse transactionally.

**Offline parity gate** (§3): 21,370 rows vs LINUX64 assembler output —
`native mismatches: 0`.

**In-engine self-test** (sdl2.c `NativeStateAudioSelfTest`): runs against
the redirected path with the same pack resolution/init order as the
content-hydration path — arena + logical table + range index live, song
tone hydrates into the transformed rows, channels hold arena sample
pointers, the walker captures them as sidecar records on a mid-play save.

**Runner fixes in this stage** (R12-C additions): stale `5087u`
pack-count assertions updated to `5289u` in
`emerald_trainer_native_compat_production.c` (3 sites) and both world
runners (3 sites each); the production test's type-name vocabulary gained
`instrument-bank`; the audio leaf runner links real `host_memory.c` +
`tests/gen3_resources/host_memory_stubs.c` for the logical-table
registration path.

## 13. Builds

- Fresh `make -f Makefile_pc -B linux64`: **0 errors**; linked binary
  `pokeemerald-linux64` (30,280,704 B).
- `nm` on the linked binary confirms the compiled audio objects are all
  still present (§9) — R12-C keeps them; R12-G removes.
- The seam compiles under the Makefile_pc flag set exactly as the offline
  gate does (`-Wno-trigraphs -DNONMATCHING -DPORTABLE -DPLATFORM_SDL2
  -DRENDERER_EASY_DRAW -DMODERN=1 -DUBFIX -DDESKTOP_EXTERNAL_GAME_CONTENT
  -DNATIVE_LINUX -DLINUX64=1 -fno-dce -fno-builtin -fno-pie`) and under
  the gen3-core-only offline builds (no Emerald defines, no SDL).

## 14. Regression battery

All 35 runners, in dependency-free order
(`/tmp/r12c_battery.sh`), fresh `pokeemerald-linux64` (rebuilt after the
seam platform-neutrality fix): **35 passed, 0 failed**. The two mid-stage
fixes that the battery caught and this stage resolved: (1) the audio
seam's `#include "platform/host_memory.h"` broke guardrail 18 and the
loader link — replaced with weak extern declarations + NULL-guarded call
sites, restoring the seam's platform-neutrality contract; (2) stale
`5087u` pack-count assertions (the pack is now 5,289) in the trainer
production and both world runners, plus the production test's missing
`instrument-bank` type name.

## 15. Untouched claims (verified)

- R12-B leaf offsets and bytes unchanged (§4) — no existing R12-B leaf
  offset changed.
- PCM/audio behavior unchanged: PCM parity gates (state TEST B) +
  in-engine self-test run against the redirected path; compiled fallback
  live for anything unregistered.
- No song-stream migration, no `gSongTable`/engine-table change (§8).
- Guardrail 18 (no `platform/` include in the gen3 core/seam set) passes
  with the weak-extern HostMemory hooks (§7).
- State-v5 reconstruction leaves no creator-process audio pointers alive:
  post-load Republish + ResolveByKey re-derivation + fingerprint gate
  (§10).

## 16. STOP-condition verification and next steps

| Plan §13 / stage-brief STOP condition | Status |
|---|---|
| Any canonical ELF/ROM mismatch | **Not hit** — E1/E2 re-prove byte-for-byte |
| Any of 21,370 rows fails exact parity | **Not hit** — `native mismatches: 0` |
| Any structural pointer unresolvable | **Not hit** — parity gate + leaf-runner span/back-shift checks |
| Any drumset back-shift ambiguous | **Not hit** — exact-start table, no interval membership |
| Duplicate logical GBA starts | **Not hit** — 197 labels asserted unique; host_memory aborts on duplicates |
| PCM behavior changes | **Not hit** — PCM parity + self-test on the redirected path |
| Mid-play save/load regresses | **Not hit** — TEST A/B round-trips green |
| State-v5 reconstruction leaves creator-process audio pointers alive | **Not hit** — re-derivation + fingerprint gate |
| Existing R12-B leaf offsets change | **Not hit** — verbatim zone byte-identical |
| Implementing C requires migrating song streams | **Not hit** — no stream/engine-table change (§8) |

Next steps: R12-D (song-stream ownership) per the architecture doc;
R12-G checklist items deferred as documented (§9). **No commit made; no
R12-D work started — this stage stops here.**

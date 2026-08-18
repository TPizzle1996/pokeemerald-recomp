# R12-C Plan — Structural Audio Implementation Plan

R12-C moves the 202 structural audio resources (195 voicegroups, 2 cry
tables, 5 keysplit tables) to canonical GBA-form pack ownership, publishes
their native-width runtime forms into the R12-B audio arena, and redirects
the existing MP2K voicegroup/cry consumers — without changing PCM/audio
behavior.

Scope note: this document is the review deliverable for R12-C. It does not
implement anything; it settles the 11 design questions and fixes the
implementation order. Everything below was verified against the tree at
HEAD (agent/prepare-v0.1.0-alpha, d3a71641f).

---

## 1. Resources and rows (item 1: the exact 12→24 byte transform)

### 1.1 Inventory

| Family | Resources | Rows | GBA bytes (pack) | Native bytes (transformed) |
|---|---|---|---|---|
| Voicegroups `emerald:audio/voicegroup/<canonical>` | 195 (180 root + 10 drumsets + 5 keysplits) | 20,594 | 247,128 (12 B/row) | 494,256 (24 B/row) |
| Cry tables `emerald:audio/cry-table/forward\|reverse` | 2 | 776 (388 each) | 9,312 | 18,624 |
| Keysplit runs `emerald:audio/keysplit/<canonical>` | 5 | — (`.byte` runs, 516 B audit-pinned) | ~516 | 516 (no transform — see §5.3) |
| **Total** | **202** | **21,370** | **256,956** | **513,396 + backshift padding** |

Pins: `PINNED_VOICEGROUP_INCLUDES 195` / `PINNED_CRY_ROWS 388` /
`PINNED["keysplit"] 5` already enforced by `gen_audio_inventory.py`
(tools/gen3_resources/audio_family/gen_audio_inventory.py:84-85, 334-338,
377-380). The generator emits the structural families today (inventory
`kind = "voicegroup" / "cry-table" / "keysplit"`,
`representation = "gba-tone-data-12" / "gba-keysplit-run"`, lines 48-66 of
`inventory.generated.toml`) — only the BINDINGS + manifest + pack rows are
missing (the production manifest still holds just the 569 leaves; see §11).

### 1.2 The transform — spec is `asm/macros/music_voice.inc` LINUX64 branches

The mandate "reproduce LINUX64 assembler output exactly" means: the seam's
24-byte row bytes must equal what `as --64 --defsym LINUX64=1` emits for the
same row. The macros (read in full) define exactly this. Per type, the
24-byte native row:

```
offset  size  field          source
0       1     type           GBA byte 0 verbatim
1       1     key/drumKey    GBA byte 1 verbatim
2       1     cgbLength      GBA byte 2 verbatim (pan|0x80 if pan≠0 else 0)
3       1     panSweep       GBA byte 3 verbatim (sweep for squares, else 0)
4       4     pad            zero — `.space 4` in every LINUX64 branch
8       8     union{wav|group|cgb3Sample|squareNoiseConfig}   see per-type
16      4     ADSR           GBA bytes 8-11 verbatim (already masked:
                             attack&7, decay&7, sustain&0xF, release&7)
20      4     pad            zero — `.space 4`
```

Per-type union contents (from music_voice.inc:16-33, 43-65, 75-97, 107-128,
138-160, 162-184, 186-210):

| Type | macro | union @8 (`.quad`) | union2 @16 (`.quad`) | ADSR bytes 16-19 |
|---|---|---|---|---|
| 0x00 directsound | `voice_directsound` | `wav` = sample pointer | — | attack, decay, sustain, release |
| 0x08 no-resample | `voice_directsound_no_resample` | `wav` | — | same |
| 0x10 directsound alt | `voice_directsound_alt` | `wav` | — | same |
| 0x01 square_1 | `voice_square_1` | **0** — duty cycle (GBA byte 4 = duty&3) is **dropped on native** | — | same |
| 0x09 square_1 alt | `voice_square_1_alt` | 0 | — | same |
| 0x02 square_2 | `voice_square_2` | 0 | — | same |
| 0x0A square_2 alt | `voice_square_2_alt` | 0 | — | same |
| 0x03 programmable wave | `voice_programmable_wave` | `wav` = wave pointer | — | same |
| 0x0B programmable wave alt | `voice_programmable_wave_alt` | `wav` | — | same |
| 0x04 noise | `voice_noise` | **0** — period&1 (GBA byte 4) **dropped on native** | — | same |
| 0x0C noise alt | `voice_noise_alt` | 0 | — | same |
| 0x40 keysplit | `voice_keysplit` | `group` = subgroup rows | `keySplitTable` = keysplit run | **absent** — union2 is the pointer |
| 0x80 keysplit_all | `voice_keysplit_all` | `group` = subgroup rows | **0** | absent |
| 0x20 cry | `cry` | `wav` = Cry_* sample | — | 0xff, 0, 0xff, 0 verbatim |
| 0x30 cry_reverse | `cry_reverse` | `wav` | — | 0xff, 0, 0xff, 0 verbatim |

Exactness checks baked into the transform tests (see §10): (a) scalar bytes
0-3 and ADSR bytes 16-19 memcmp-equal to the LINUX64-assembled row; (b)
padding bytes 4-7 and 20-23 zero (matches `.space`); (c) union word is 0 for
square/noise rows; (d) the keysplit-union pointer is NULL-equivalent only for
0x80 (`.quad 0`).

### 1.3 Pointer resolution in the transform (publish-time, not runtime)

GBA addresses are resolved **at publish** against the arena's leaf table
(`sourceRomOffset` + size, R12-B `emerald_audio_compat.c`):

- **Sample/wave pointers** (types 0/8/16/0x20/0x30/3/0x0B): the GBA address
  must hit exactly one of the 569 leaf spans (105 root + 51 phoneme + 388
  cry + 25 waves) — resolution produces `verbatimZone + (romAddr -
  EMERALD_AUDIO_ROM_START)`. A miss (a sample outside the migrated set) is a
  hard publication failure, never a dangling pointer. The generator
  cross-checks every row's sample reference against the leaf set first, so a
  miss is a generator failure before it can become a seam failure.
- **Subgroup pointers** (0x40/0x80): the GBA address of a `voicegroup_*`
  label → that voicegroup's transformed block. For the **10 drumset**
  subgroups the label is offset-defined (`voice_group <name>, N` →
  `voicegroup_<name> = . - N*0x18` native, m4a.inc:15-27) — the published
  pointer is `rowsStart - N*24` (native element width; the GBA form uses
  `N*0xC`). The transform must reproduce the **native** back-shift (24
  B/row), and the transformed zone must place N zeroed rows before each
  drumset block so the back-shifted pointer lands on mapped zeroed memory,
  never on another table's rows (see §2.2). Dereferences only ever occur at
  `group + note` with note ≥ N (drum authoring invariant — identical to the
  compiled layout), so the pad is a memory-hygiene measure, not semantics.
- **Keysplit pointers** (0x40 union2): the GBA address of a `keysplit_*`
  label → the keysplit run in the verbatim zone. The label is back-shifted
  by the table's offset (`keysplit piano, 36` → `keysplit_piano = . - 36`,
  m4a.inc:29-55): the published pointer is `runBytes - 36` **in the verbatim
  zone at ROM-relative offset** — i.e. `verbatimZone + (runRomAddr - 36 -
  EMERALD_AUDIO_ROM_START)`. The 36 bytes before the run are in-span
  (voicegroup region), zeroed or prior-table bytes; `keySplitTable[key]`
  with key ≥ 36 always lands in the run (verified against the five tables'
  split ranges: piano/strings/trumpet/french_horn cover [36,108), tuba
  [24,108) — notes below the first split are never played with these
  instruments, same invariant as compiled).

## 2. Transformed-zone size and arena layout (item 6)

### 2.1 Sizes

- 20,594 voice rows × 24 B = **494,256 B**
- 776 cry rows × 24 B = **18,624 B**
- Drumset back-shift padding: 10 drumsets × N×24 B (rs 36, route110 40,
  others per the generator's parsed offsets; ≈ 8–10 KiB) — **≈ 8,640 B**
- 8-byte block alignment ≤ 7×8 = **≈ 56 B**
- **Total transformed zone ≈ 521,520 B (≈ 509.3 KiB)**; exact figure is a
  generator output (sum of per-table row counts × 24 + padding), pinned by a
  seam composition gate.

Keysplit runs (≈516 B) are NOT part of the transformed zone — they are
copied into the **existing verbatim zone** at their ROM-relative offsets
(§4.1 of the architecture doc; the span 0x0867709C–0x089A3DB4 already
covers the keysplit region ~0x086b5xxx, and R12-B zero-initialised the
holes).

### 2.2 Layout (extension of the R12-B single allocation)

R12-B arena today (emerald_audio_compat.c:356-371): `malloc(sizeof(header)
+ recordBytes + EMERALD_AUDIO_SPAN_SIZE)`; leaf table at `[0, recordBytes)`,
verbatim zone at `[recordBytes, +3,329,304)`. R12-C appends:

```
header                     (unchanged, + structural counts)
leaf table (569)           (unchanged — offsets stable, R12-B tests hold)
structural table (202)     NEW — canonicalName, kind, labelRomAddr (incl.
                           back-shift for drumsets), backshiftRows,
                           rowCount, verbatimOffset (keysplits only),
                           transformOffset (voicegroups/cries)
verbatim zone 3,329,304 B  (unchanged; + 5 keysplit run copies at
                           ROM-relative offsets; zeroed elsewhere)
transformed zone ≈521,520  NEW — 195 voicegroup blocks (10 drumset blocks
                           prefixed by N×24 B zero rows) + 2 cry blocks,
                           8-byte aligned, 8-aligned bases so all
                           `.quad` fields stay aligned
```

`EMERALD_AUDIO_SPAN_SIZE` (verbatim zone) is unchanged; `GetArena` now
returns `spanSize + transformSize` — the R12-B test F assertion
(`arena == SPAN_SIZE`) must be re-pinned. The atomic `sArena` swap,
transactional two-phase build, and Republish semantics carry over; the
existing leaf `arenaOffset` values are untouched, so no leaf offset breaks.

## 3. voicegroup_dummy decision (item: A vs B)

**Decision: A — migrate `voicegroup_dummy` into the transformed arena with
the other 195.** It is one of the 195 inventoried voicegroups
(`sound/voicegroups/dummy.inc`, 127 rows); the pack carries it; the logical
range covers it; and keeping it compiled would create a 196th special case
for zero benefit.

Every initializer/pointer that touches it:

| Site | What | R12-C disposition |
|---|---|---|
| 5 song files (`mus_c_vs_legend_beast`, `mus_c_comm_center`, `mus_gsc_pewter`, `mus_dummy`, `mus_gsc_route38`), each line 3: `.equ <song>_grp, voicegroup_dummy` | the song header `.tone` 4-byte GBA value | resolved by the logical range → transformed rows (zero code change; ranges are the redirect) |
| `gPokemonCrySongTemplate.tone` (m4a_tables.c:264) `= (struct ToneData *)&voicegroup_dummy` | compiled pointer initializer | **stays compiled, unchanged** — points at the compiled dummy rows, which remain linked through R12-G, so it is never dangling. Runtime-dead anyway: m4aSoundInit memcpy's the template into `gPokemonCrySong` (m4a.c:187) and every `SetPokemonCryTone` call overwrites `.tone` before any play (m4a.c:1824). R12-G re-emits this initializer to the arena rows (documented in the R12-G checklist, NOT in R12-C). |
| `include/gba/m4a_internal.h:438` extern | declaration | unchanged |
| `gen_audio_inventory.py` (lines 25, 297, 517) | inventory seam | unchanged |

The dummy.inc rows themselves are ordinary rows (types 0x40/0x80/1/2/0:
`voice_keysplit_all voicegroup_rs_drumset`, `voice_keysplit voicegroup_
piano_keysplit, keysplit_piano`, squares, one directsound) — no special
transform handling; their sample/group/keysplit pointers resolve normally.

## 4. Cry tables (item 3: runtime representation)

**Decision: transform the 2 cry tables into native 24-byte rows in the
arena's transformed zone (2 blocks of 388 rows), publish via a seam
accessor, and redirect `sound.c`'s GET_CRY to it.** Not in-place published
arrays (no relocation-safe way to repoint the compiled symbol), not
interior GBA-address aliasing (the cry path never resolves through
`HostResolveGbaAddr` — `SetPokemonCryTone` receives a direct native
pointer).

Concretely:

1. New seam accessor, e.g.
   `const struct ToneData *EmeraldAudioCryTableRow(u8 table, bool reversed,
   u8 index)` returning the row `forward/reverse` at `128*table + index`
   from the transformed cry block (same 4×128-bank indexing as compiled —
   the pack row set is 388, and `SpeciesToCryId` caps ids at ≤ 387 so
   indexing stays in bounds, exactly as compiled).
2. `src/sound.c:28-29,475`: replace `GET_CRY(...)` (which resolves
   `&gCryTable[(128*tableId)+speciesIndex]`) with the accessor under
   `#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)`, keeping the
   compiled-table path as the GBA/fallback `#else` (R7B pattern — the seam
   is platform-neutral and linked into native targets only; GBA sources
   never see gen3 headers).
3. `SetPokemonCryTone` (m4a.c:1797-1834) is **unchanged**: it stores the
   row pointer into `gPokemonCrySongs[i].tone` and
   `HydratePokemonCrySong` (m4a.c:98-112) forwards it; the native player
   reads the 24-byte row. The `struct ToneData *` parameter type is
   nominally 12-byte but is used as an opaque pointer throughout — no
   structural indexing of cry rows exists in C.
4. Compiled `gCryTable`/`gCryTable_Reverse` remain linked (parity +
   byte-equality tests) until R12-G removes them (and with them the
   `extern` at sound.c:28-29).

## 5. Logical-address registration (item 5 — the `HostResolveGbaAddr` change)

### 5.1 What must be registered NOW

Only the 197 table labels that `.tone` values can reference:

- **195 voicegroup labels** — 185 exported (`voicegroup_<name>`) + 10
  drumset labels. **Verified: no song references a drumset directly** (the
  530 songs' `.equ <song>_grp, voicegroup_*` rows reference ~60 distinct
  voicegroups, all exported labels; drumsets are referenced only as
  `group` operands of keysplit_all rows, which are publish-time native
  pointers, never GBA-address lookups). Drumset label entries are
  registered anyway (symmetry with §4.2 of the architecture doc; harmless).
- **2 cry table labels** (gCryTable/gCryTable_Reverse) — not needed for the
  current cry path (native accessor), registered for §4.2 completeness and
  the R12-E re-emission.

**Not registered now:** verbatim-zone interval spans (samples/waves stay
compiled; `ply_xwave` stream operands still resolve by direct cast to the
compiled leaves) and song-stream ranges (songs stay compiled; R12-D). This
answers "do not design song-stream ranges unless necessary" — not necessary
in R12-C.

### 5.2 Representation, precedence, lifetime

- **Exact-start table**, not intervals: every reference the table serves is
  exactly a table label (`.tone = <label>`; never an interior address — the
  12-byte rows are never addressed from GBA-form data after the transform;
  the transformed rows carry native pointers). 197 sorted `(GbaAddr,
  hostBase)` entries. This eliminates the drumset back-shift overlap
  problem entirely — an exact-start entry creates no interval membership,
  so the back-shifted drumset labels (which fall inside the *previous*
  table's ROM span for tables with < 36 rows, e.g. the 5 four-row keysplit
  voicegroups) cannot collide with anything. Overlap rules therefore apply
  only to future interval spans (verbatim zone, R12-E), which are disjoint
  by construction.
- **Precedence**: exact-start table (binary search) → handle table →
  direct-cast fallback. The fallback remains correct through R12-G because
  the compiled 24-byte rows still live at GBA-shaped host addresses — an
  unregistered tone silently uses the compiled parity copy (additive
  degrade, matching the R12-B contract).
- **Ownership/lifetime**: the table is static host .data owned by the audio
  seam, populated in `TryInitialize` after the arena validates, cleared in
  `ClearMigratedEntries`/`Shutdown`, and **cleared + repopulated by
  `Republish`** (allocation-free — fixed 197-entry array). It is outside
  every serialized slice. Thread/lifecycle: publication happens on the
  registration thread before `Platform_AudioInit`/`m4aSoundInit`
  (audit §8 step 1); the mixer thread reads the table only through
  `HostResolveGbaAddr`, and the table is swapped with the arena
  publication (single store of the table pointer, same atomic pattern as
  `sArena`).
- **API**: `HostMemoryRegisterLogicalRange(romStart, romEnd, hostBase)`
  per the architecture §4.2 is refined to an exact-start form:
  `HostMemoryRegisterLogicalAddress(GbaAddr addr, void *hostBase)` (plus a
  clear-all). The `host_memory.c:325-329` comment already anticipates this
  table. Exact-change list for the required finding: (a) one struct + one
  static array + register/clear functions in `src/platform/host_memory.c`;
  (b) `HostResolveGbaAddr` consults it before the handle-table check; (c)
  the audio seam calls the register/clear functions at
  publish/republish/clear. No other engine change.

## 6. Consumer cutover (item 7 — complete list)

| # | Consumer / symbol | File:line | R12-C change |
|---|---|---|---|
| 1 | song-header `.tone` hydration | m4a.c:81-94 (`HydrateSongHeader`, `HostResolveGbaAddr(T1_READ_32(raw+4))`) | **none in m4a.c** — the registered logical address redirects it; resolved pointer ∈ transformed zone |
| 2 | `MPlayStart` → `mplayInfo->tone` | m4a.c:759 | none (pointer flows; `MusicPlayerInfo.tone` == `MP2KPlayerState.voicegroup` by layout, STATIC_ASSERT pattern at m4a.c:11-26; cast in `MP2KPlayerMain`, music_player.c:339) |
| 3 | note-on instrument copy | music_player.c:280-288 (`MP2K_event_voice`) | none — `voicegroup[voice]` is now an arena row; `track->instrument = *instrument` copies the 24-byte record verbatim |
| 4 | instrument `wav` → mixer | note-on path (mixer reads `MixerSource.wav`) | implicit — `wav` now points at the verbatim zone (arena samples). **First behavior-visible effect**: mixer reads arena bytes; PCM-identical because R12-B proved arena == ROM == pack bytes |
| 5 | programmable wave path (types 3/0x0B) | CgbSound wave-RAM copy | implicit — `wav` points at the arena's 16-byte wave leaves |
| 6 | cry lookup | sound.c:475 (`GET_CRY`) | accessor redirect (§4) |
| 7 | `SetPokemonCryTone` | m4a.c:1797-1834 | none (opaque row pointer) |
| 8 | `gPokemonCrySongTemplate.tone` | m4a_tables.c:264 | none in R12-C (§3; R12-G re-emission) |
| 9 | `voicegroup_dummy` | m4a_tables.c:264, sound.c (via songs), m4a_internal.h:438 | none — migrated with the 195; template stays compiled (§3) |
| 10 | `HostResolveGbaAddr` | host_memory.c:308-330 | logical-address table (§5) |
| 11 | sound.c `extern struct ToneData gCryTable[]` | sound.c:28-29 | becomes fallback-only (`#else` branch) |

No song, no stream, no `gSongTable`/`gMPlayTable`/engine-table change. The
only C edits outside the seam/tests are: `src/sound.c` (cry accessor),
`src/platform/host_memory.c` (logical table), and the post-load block in
`src/platform/native_state.c` (§9).

## 7. Compiled fallback / removal boundary (item 8)

- **Through R12-C (and R12-D/E/F)**: all compiled audio stays linked —
  voicegroups, cry tables, samples, songs, template. The compiled rows are
  the parity reference AND the additive fallback (an unregistered tone or
  an unpublished arena degrades to compiled audio — the R12-B contract).
  Tests select/prove the migrated path by asserting resolved pointers land
  **inside the arena** (∈ transformed zone / verbatim zone), and the
  parity test memcmps the seam's transformed rows against the
  LINUX64-assembled compiled rows for all 21,370 rows (see §10).
- **Removal boundary**: R12-G removes compiled audio payloads from the
  native link (`voice_groups.inc` include skip / section exclusion, the
  R11-B pattern) — only after R12-D (songs), R12-E (cries/SFX samples
  end-to-end) and R12-F (State v5) land. At R12-G the following must be
  re-emitted before the compiled data can go: `gPokemonCrySongTemplate.
  tone` → arena rows; the sound.c `#else` fallback branch; the
  direct-cast fallback for audio addresses (falls to "unregistered =
  session error"). The `.inc` files themselves stay in the repo — they are
  the generator's canonical source for the pack, always.

## 8. State v5 boundary (item 9 — the sequencing recommendation)

**Facts established in the review:**

- Mid-play, GAME_BSS holds exactly these live arena pointers after R12-C:
  `sHostSongHeaders[0..3].tone` (4) and `gPokemonCrySongs[0..1].tone` (2)
  — ~6 sites. Everything else that audio touches (sample `wav` pointers in
  tracks/channels, stream cmdPtr) still points at **compiled** data until
  R12-D/E.
- The v5 walker already covers every 8-byte window of the slice
  (native_state.c:2036-2058): an arena pointer that is neither a registered
  range nor an image address hits `LooksLikeExternalHostPointer` or the
  `knownDataPointer` check → **save refused with a named runtime-pointer
  error** (native_state.c:2070-2085). It fails loudly — never silent
  corruption — but it breaks the alpha's mid-play save feature.
- The R10 range index (`EmeraldResourceRangeIndex`, cap 8192; ~4518
  registered post-R11D) + sidecars (cap 4096 records) + `ResolveByKey` are
  all generic and already proven; the audio ranges fit (≈ +200 → ~4,715
  ranges; ≈ +6 live records).
- The post-load sequence (native_state.c:3060-3074) calls
  `EmeraldResourceCompat_Republish` + `NativeWorldNeighborhood_Invalidate`.
  The audio seam's `Republish` is NOT in it today.
- The headless proof's "wav/currentPointer image-resident (sub-4 GiB)"
  assertion (sdl2.c:1003-1011) is the canary that must flip when arena
  pointers become live.

**Recommendation: pull the minimal State-v5 range work forward INTO
R12-C — register + resolve, nothing more.** Do NOT disable native-state
saving (that breaks the R10 headline feature and the manual gate), and do
NOT pull the full R12-F (per-resource CANONICAL sub-spans for all 771
resources, walker-coverage extensions, cap re-pinning) — none of that is
needed until R12-D/E migrate songs and samples. The minimal set:

1. At audio publication: register the 197 transformed-zone table spans +
   the verbatim zone in the R10 index (roles: transformed = COMPAT_OBJECT,
   verbatim = CANONICAL; keys per resource) via the existing
   `EmeraldResourceRangeIndex` API (the seam reaches it through
   `EmeraldResourceCompat_GetRangeIndex()`, native_state.c:244-247), plus
   one hull over the whole audio arena (caps: hulls 8, currently 5 used —
   +1 OK). Registering happens in the seam's `TryInitialize`/`Republish` —
   the R10 index API is allocation-free static arrays, so the "allocation-
   free republish" contract holds.
2. Wire `EmeraldAudioCompat_Republish(diag)` into the post-load block,
   **after** `EmeraldResourceCompat_Republish` (the trainer rebuild resets
   the index; audio must re-register after it) and before the
   `NativeWorldNeighborhood_Invalidate` line. Republish re-registers the
   ranges against the current (relocated-session) arena; the arena itself
   persists (load does not reallocate it), so range bases stay valid.
3. Flip the self-test assertion (sdl2.c:1003-1011) from image-resident to
   arena-resident for the migrated pointer sites.
4. Extend the state regression + `--native-audio-self-test` with mid-play
   saves during BGM playback AND mid-cry playback, asserting sidecar
   records carry the correct keys and load re-derives arena pointers via
   `ResolveByKey` (mismatch → refuse — the existing hard-fail path).

Sequencing honesty: this makes R12-C "register enough ranges" (option a of
the review question) — the alternative "temporarily disable saving" is
rejected as a user-visible regression, and "move all of R12-F forward" is
rejected as unnecessary scope. The remaining R12-F work stays scheduled
after R12-D/E where it becomes load-bearing.

## 9. Pack/generator work (item: manifest + pack extension)

`gen_audio_bindings.py` currently emits only the 569 leaves
(`LEAF_KINDS`, line 53). R12-C extends it to emit the 202 structural
bindings:

- voicegroup (195): type `instrument-bank`, schema 1
  (`gba-tone-data-12`), size = rows × 12, **first-row ROM address** as the
  record offset (label + backshift for the 10 drumsets — the pack payload
  must be the 12-byte rows only, not the preceding pad bytes), backshift
  rows as metadata (schema field);
- cry-table (2): same schema, 388 rows each, offset = gCryTable /
  gCryTable_Reverse label;
- keysplit (5): type `instrument-bank`, schema 2 (`gba-keysplit-run`),
  size = run bytes, backshift (36/24) as metadata.

`gen_audio_inventory.py` gains cross-checks: every sample/wave pointer in
the 21,370 rows resolves to a defined leaf; every 0x40 group to one of the
195; every 0x40 keysplit to one of the 5; every 0x80 group to one of the 10
drumsets; row-count pins (20,594 / 388×2). Then regenerate the audio
manifest (`gen3-elf-manifest --check` re-derivation) and the production
pack (`gen3-pack-build --check`): **5087 → 5,289 entries**, +256,956 B
payload, deterministic — the R12-B proof machinery re-runs unchanged.

Prerequisite check (NOT a pack gate): the R12-A §6 action to restore the 76
canonical `.aif` — verify the tree state at R12-C start; the pack path
derives from ROM slices so R12-C does not depend on it, but the audit
action should land before R12-C ships (it affects any future in-repo GBA
build, not the pack).

## 10. Test gate (item 10)

New runner `tests/run_audio_structural.sh` (+ `_sanitize.sh`) compiling
`emerald_audio_compat.c` standalone (platform-neutral, R12-B harness
pattern) with the REAL production pack:

1. **Counts**: 5,289 pack entries; 195/2/5 structural resources; 20,594 +
   776 rows; per-table row counts from the `.inc` parse match the manifest.
2. **Transform parity vs LINUX64 assembler output**: assemble
   `sound/voice_groups.inc` + `sound/keysplit_tables.inc` + cry tables with
   host `as --64 --defsym LINUX64=1` (the Makefile_pc recipe minus the
   game link), dump `.rodata` (`objcopy -O binary`), and memcmp the seam's
   transformed rows against the assembled rows for **all 21,370 rows** —
   the "reproduce LINUX64 output exactly" gate. Padding bytes zero, union
   words 0 for square/noise, ADSR masked values equal.
3. **Pointer targets**: every resolved sample/wave pointer lies inside the
   verbatim zone and its bytes == pack payload (memcmp); every subgroup
   pointer lies inside a transformed block with the correct back-shift
   (`group + N*24 == rowsStart` for drumsets); every keysplit pointer lies
   in the verbatim zone at `runStart - backshift`, and
   `keysplit[label+note]` for notes in the split ranges equals the
   compiled run bytes.
4. **Backshift/interior-pointer cases**: all 10 drumset back-shifts, all 5
   keysplit back-shifts; keysplit_all rows → drumset blocks; keysplit rows
   → keysplit voicegroups (4-row blocks); the 175/195 voicegroups
   containing keysplit rows (588 rows total).
5. **BGM/SFX/fanfare/cry determinism**: PCM byte-identical vs the compiled
   baseline (native self-test: real SDL2 device, BGM + SFX + fanfare +
   cry forward + cry reverse + programmable-wave + PCM paths; save/load
   leaves the device playing — the R12-B battery items re-run against the
   redirected path).
6. **Save/load** (per §8): mid-BGM and mid-cry save/load round-trips;
   sidecar records carry the resource keys; load re-derives arena pointers;
   cross-restart round-trip (fresh process, fingerprint gate) green.
7. **Republish/relocation**: relocated session → TryInitialize replaces the
   arena atomically → ranges re-registered → next song start hydrates into
   the NEW arena (assert pointer ∈ new arena); Clear/Shutdown fail closed
   (arena absent, ranges cleared, compiled fallback live, save refuses
   loudly with the named error — not silently).
8. **Corrupt/missing refusal**: renamed/truncated/wrong-type structural
   resource → transactional failure naming the first failing resource,
   arena absent, no consumer change; pack duplicate-key guard.
9. **Full regression battery**: all 37 R12-B runners + the new runner(s)
   + fresh `make -f Makefile_pc -B linux64`.

## 11. Implementation order for Flash

1. **Pre-flight**: verify the 76 `.aif` tree state (§9 prerequisite); fresh
   `-B` build baseline; confirm the R12-B pack hash (5087 /
   1df96748…) re-derives.
2. **Generator**: extend `gen_audio_bindings.py` (202 structural bindings
   with backshift metadata) + `gen_audio_inventory.py` cross-checks;
   regenerate inventory (no-op diff), manifest (569 → 771 records),
   pack (5087 → 5,289, deterministic re-proof E1/E2).
3. **Seam arena**: structural table + transformed-zone build (per-row
   transform §1.2/§1.3, drumset paddings, keysplit verbatim copies) +
   query helpers (`GetVoicegroupSpan`, `GetCryTableSpan`, `GetKeysplitSpan`,
   `GetTransformedRows`, `EmeraldAudioCryTableRow`) + diagnostics +
   composition gate (202 / 21,370 / 521,520).
4. **Offline structural tests** (§10.1-4, 8) — no engine changes yet; the
   build still sounds exactly as pre-R12-C (additive proof).
5. **Logical table** in host_memory.c (§5.2) + unit tests (precedence,
   clear/republish, 197 entries).
6. **Consumer redirect**: cry accessor in sound.c (§4); song path goes
   live implicitly the moment ranges register — re-run §10.5 determinism
   (first behavior-visible step; expect PCM-identical).
7. **State-v5 minimal forward-pull** (§8): R10 index registration at
   publish/republish, post-load block wiring, assertion flip, state
   regression extensions.
8. **Full battery** (§10.9) + fresh `-B` build + `nm` checks.
9. **docs/R12C report** (resources/rows, transformed bytes, decisions,
   results) — then STOP before R12-D.

## 12. Required findings (summary)

1. **Resources/rows**: 202 resources (195 voicegroups 20,594 rows, 2 cry
   tables 776 rows, 5 keysplit runs ≈516 B) → 21,370 transformed rows.
2. **Transformed-zone bytes**: ≈521,520 B (494,256 voice + 18,624 cry +
   ≈8,640 drumset back-shift padding + ≈56 alignment); exact value is
   generator-pinned. Keysplits live in the existing verbatim zone (no new
   allocation beyond the single arena extension).
3. **voicegroup_dummy**: migrate with the 195 (decision A); 5 song `.equ`s
   redirect via the logical range; `gPokemonCrySongTemplate.tone` stays
   compiled (never dangling; runtime-dead) — R12-G re-emission.
4. **Cry tables**: transformed 24-byte rows in the arena + seam accessor
   `EmeraldAudioCryTableRow`; sound.c GET_CRY redirect under NATIVE_LINUX
   with compiled fallback; SetPokemonCryTone unchanged.
5. **HostResolveGbaAddr changes**: exact-start logical-address table (197
   entries: 195 voicegroup labels incl. 10 back-shifted drumset labels + 2
   cry labels) registered/cleared by the audio seam; consulted before the
   handle table; no interval spans in R12-C (verbatim + song-stream ranges
   deferred to R12-D/E); direct-cast fallback stays correct through R12-G.
6. **State v5**: minimal forward-pull required — register the audio ranges
   + arena hull, wire audio Republish into the post-load block, flip the
   self-test assertion, extend state tests. Without it, mid-play saves
   fail loudly (named error) — acceptable as a floor, but R12-C should not
   ship that regression when ≈20 lines of registration fix it. Full R12-F
   stays after R12-D/E.
7. **Highest-risk cutover**: the song-header `.tone` redirect (every song
   start, 60 voicegroups, silent wrong-audio on range-table error — no
   crash). Second: drumset back-shift pointer building (10 sites, interior
   pointer semantics).
8. **Implementation order**: §11 (generator → arena/transform → offline
   tests → logical table → cry redirect (song path implicit) → state
   forward-pull → battery).
9. **C1/C2 split**: not recommended as separate stages. The pack →
   arena → transform → registration → redirect is one transactional unit
   (a partial stage either duplicates the generator/manifest/arena work or
   leaves a half-live arena); every sub-step is individually testable
   within the single stage (additive proof before the redirect, PCM proof
   after). If review risk forces smaller PRs, the natural seam is
   C1 = generator + arena + transform + parity tests (no consumer change,
   additive), C2 = logical table + redirect + state forward-pull — but the
   build must stay green at the C1 boundary, which it will, since nothing
   consumes the transformed zone yet.

## 13. STOP

R12-C is scoped and sequenced above. Per instruction: do not implement, do
not modify production code, do not begin R12-D, do not alter State v5 —
beyond the explicitly scoped forward-pull — until this plan is approved and
R12-C is executed as its own stage.

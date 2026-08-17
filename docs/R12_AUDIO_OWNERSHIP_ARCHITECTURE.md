# R12 Audio Ownership Architecture

Design for migrating Emerald audio assets to the verified-ROM resource
system (R12-A…G). Facts verified against the tree at checkpoint-pre-r12-clean
(d3a71641f); see `docs/R12_AUDIO_RESOURCE_AUDIT.md` for the inventory,
pointer graphs, consumers, .aif forensics, and State v5 placement evidence.
Read before starting: `docs/EMERALD_ROM_BACKED_ASSET_MIGRATION_ARCHITECTURE.md`
(§9 GBA-shaped pointer assembly, §13 audio strategy, §22 risks),
`docs/R10_NATIVE_STATE_V5_DESIGN.md`, and the R11C plan/report pair (the
machinery to reuse).

Scope: sample/wave payloads, song graphs, voicegroups, cry tables, keysplit
tables, State v5 range integration, isolation. OUT of scope: MP2K engine
rewrites (explicitly forbidden — the engine in src/m4a.c, src/music_player.c,
src/sound_mixer.c, src/m4a_1.s stays compiled), new resource system,
audio-format changes (no OGG/WAV; MP2K bytecode preserved).

## 1. Goals and invariants

1. Original Emerald audio asset payloads are absent from distributed native
   release + DINFO executables; they are derived from the verified user ROM
   (BPEE01, SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7), represented in
   the production .rpack, published through the existing session/compat
   architecture. One resource system, same as R9-R11.
2. GBA/gameplay behavior and audio parity unchanged (pinned by the
   `--native-audio-self-test` PCM determinism harness).
3. Native x64 runtime stays stable; save/load mid-song keeps working.
4. Generated GBA logical addresses remain 32-bit logical identities. Never
   widen a GBA-shaped record in place; never write a 64-bit host pointer
   into a 4-byte field; no raw native pointers inside pack payloads.
5. Canonical pack payload = verbatim ROM slice (provenance: retail ROM +
   qualified reference ELF, the R9-R11 proof chain).

## 2. Key taxonomy and resource catalog

Canonical component = lowercase symbol suffix, `_` → `-` (the R11-B rule),
prefixed by family. All names under `emerald:audio/...`.

| Family | Count | Resource id | Type (new) | Schema | Representation | Payload |
|---|---|---|---|---|---|---|
| BGM/SE/phoneme songs | 530 | `emerald:audio/song/<canonical>` (mus-*, se-*, ph-*) | music-sequence | 1 | gba-mp2k-song-graph | header + track streams, verbatim slice |
| direct-sound samples (root) | 105 | `emerald:audio/sample/<canonical>` | audio-sample | 1 | gba-wave-data | WaveData hdr + s8 PCM |
| cry samples | 388 | `emerald:audio/sample/cry/<canonical>` | audio-sample | 1 | gba-wave-data | WaveData hdr + s8 PCM (compressed-cry encoding) |
| phoneme samples | 51 | `emerald:audio/sample/phoneme/<n>` | audio-sample | 1 | gba-wave-data | WaveData hdr + s8 PCM |
| programmable waves | 25 | `emerald:audio/wave/programmable/<n>` | audio-sample (or wave-data) | 1 | gba-cgb-wave | 16 B (32×4-bit) |
| voicegroups | 195 | `emerald:audio/voicegroup/<canonical>` (+ `-keysplit` for the 5) | instrument-bank | 1 | gba-tone-data-12 | 128×12 B rows |
| cry tables | 2 | `emerald:audio/cry-table/forward`, `emerald:audio/cry-table/reverse` | instrument-bank | 1 | gba-tone-data-12 | 388×12 B rows |
| keysplit tables | 5 | `emerald:audio/keysplit/<canonical>` | instrument-bank | 2 | gba-keysplit-run | `.byte` run |

**Total: 1,301 resources ≈ 3.1 MiB canonical payload.** Aliasing (the R9
alias rule): none — every song/sample/bank has exactly one canonical id.
The 80 dummy song rows share one compiled 4-byte header and get NO resource
(the dummy header stays compiled — structural placeholder, documented as an
exemption).

**DECISION (audit item E, voicegroups/cry tables): MIGRATE them.** The pack
carries the canonical GBA 12-byte rows (verbatim ROM slices); the native
publication transform builds the 24-byte `MP2KInstrument`/native-`ToneData`
records in the session arena at publish time. Rationale: removes ~513 KB of
ROM-derived instrument data from the native binary; the 12→24 B transform is
mechanical (known macro mapping, already encoded in the LINUX64 assembler
branches); the engine is untouched (it indexes whatever array the song
header's tone field resolves to). Fallback (if the transform proves risky in
R12-C): keep voicegroups compiled and publish pointer fields in place
(R11C pattern) — a contained retreat that costs only the 513 KB of
isolation.

New runtime type vocabulary: add `GEN3_RESOURCE_TYPE_AUDIO_SAMPLE`,
`MUSIC_SEQUENCE`, `INSTRUMENT_BANK` (resource_types.h — the R11C
vocabulary-addition pattern, no pack format change) + elf-manifest
`TypeCompatibleWithRepresentation` rows.

## 3. Canonical payloads: extraction and provenance

Same chain as R9-R11: verified retail ROM + qualified reference ELF
(`../pokeemerald-reference/pokeemerald.elf`) → per-symbol ROM slices →
manifest → `.rpack`.

- Symbols (from the qualified ELF, addresses from pokeemerald.map):
  530 song symbols (header label; slice covers the whole object's rodata —
  tracks precede the header in each `.o`), 544 `DirectSoundWaveData_*`/
  `Cry_*`, 25 `ProgrammableWaveData_*`, 195 `voicegroup_*`, 2 cry tables,
  5 `keysplit_*`.
- Sizes: assembly symbols have `st_size == 0` (audit §5). The audio family
  generator computes sizes structurally: song slice = tracks + header from
  the .s layout (`8 + 4×trackCount` header, streams up to the header label);
  voicegroup/cry rows = 12 × count (count from the .inc); keysplit runs =
  516 B total (known splits); samples = end-label gaps from the qualified
  ELF's object layout. Every computed size is validated against the ELF
  section layout and the retail ROM slice; any mismatch → manifest error.
- Three-way equivalence gate (the R9 proof): extract(symbol) from retail
  ROM == extract(symbol) from qualified ELF == pack payload byte-for-byte.
  For songs, additionally walk each stream with the MP2K command table
  (MPlayDef opcodes + operand widths; GOTO/PATT/xwave = 4-byte address) and
  assert every address operand lands inside the song's own slice or a known
  sample/voicegroup start — this walker is extract-time only (validation),
  NOT a runtime dependency.

## 4. Native publication architecture

One new compat seam `src/emerald/resources/emerald_audio_compat.c` (+
header), the R11 seam contract (TryInitialize/Republish/ClearMigratedEntries/
Shutdown, transactional, allocation-free republish).

**4.1 Publication layout — ONE consolidated audio arena.**

`TryInitialize` allocates a single arena (one `EmeraldResourceCompatibility
Image`-style block) laid out in two zones:

- **Verbatim zone** — songs, samples, waves, keysplit tables are copied
  byte-verbatim at ROM-relative offsets: `arenaOffset = romAddr -
  AUDIO_ROM_START` (AUDIO_ROM_START = 0x0867709c, the `voicegroup_dummy`
  anchor; the audio symbol span ends at 0x089a3db4, which covers the full
  sample tail — ~3.3 MiB of VA, holes left unbacked). Internal 4-byte
  addresses (GOTO/PATT/xwave operands, header part[]/tone fields) remain
  **untouched ROM addresses** — the pack payload, the arena copy, and the
  ROM slice are byte-identical.
- **Transformed zone** — voicegroup and cry tables are built as native-width
  records (24 B/row: type/drumKey/panSweep fields, 8-byte `wav`/`group`
  pointer, ADSR or 8-byte `keySplitTable` pointer) at publish time from the
  canonical 12-byte pack rows; pointer fields = arena addresses of the
  target resource (verbatim-zone offsets for samples/waves, transformed-zone
  addresses for keysplit groups). The 12→24 B macro mapping already exists
  in `asm/macros/music_voice.inc` (LINUX64 branches) — the generator emits
  the same transform as a build table, and the seam applies it.

**4.2 Logical-address registration (the ONE engine-side change).**

Extend `HostResolveGbaAddr` with a registered logical-range table
("relocation table" — the mechanism anticipated by the comment at
host_memory.c:325-329): `HostMemoryRegisterLogicalRange(GbaAddr romStart,
GbaAddr romEnd, void *hostBase)` / lookup `hostBase + (addr - romStart)`
before the direct-cast fallback. Ranges registered: (a) one span for the
verbatim zone; (b) 197 per-table spans for the transformed voicegroup/cry
tables. Lookup = one span check + a binary search (~200 entries) — the hot
path (GOTO/xwave resolution) stays O(log n). No MMAP at fixed addresses, no
low-4 GiB allocator, no stream rewriting. Registered ranges are cleared/
re-registered by Republish (cross-restart re-derivation).

Alternative rejected: rewriting operands at publish time into host addresses
would require the streams to sit below 4 GiB (a custom low-memory pool) or
in a handle table (process-local, grows unbounded across republishes).
Keeping operands as ROM identities preserves "32-bit logical identities"
end-to-end and makes the arena copy canonical.

**4.3 Publication targets.**

- Song arenas + verbatim zone: nothing written at publish time; resolution
  flows through `HydrateSongHeader`/`MP2KReadAddressOperand` +
  `HostResolveGbaAddr` unchanged.
- `gSongTable` (compiled): the native branch's 610 `GbaAddr header` values
  are re-emitted as **ROM addresses** (constants — generated
  `song_table_native.generated.h`, include-once toggle, the R11-B pattern);
  the two `.short` fields stay compiled. No runtime publish needed.
- `gCryTable`/`gCryTable_Reverse` + `gPokemonCrySongTemplate.tone`
  (m4a_tables.c:264) + `voicegroup_dummy`-adjacent pinned slots: seam
  publishes their pointers to transformed-zone records (cry tables become
  published arrays; the cry-template tone pointer is published to the
  transformed `voicegroup_dummy` slot — or `voicegroup_dummy` stays a
  compiled 24-byte record, the simpler pinned exception; decide in R12-C).
- `gMPlayTable`, `dummy_song_header`, engine tables (m4a_tables.c), MPlayDef
  constants, songs.h/cry_ids.h: stay compiled untouched.

**4.4 What the pack never contains.** Native pointers (all pack bytes are
GBA-form), hydration caches, per-process identities — publication is the
only place native pointers exist.

## 5. Pointer graph reconstruction at consumption

| Reference | Where it lives | Resolution |
|---|---|---|
| song header part[i] | verbatim zone | `HydrateSongHeader` → `HostResolveGbaAddr` → verbatim zone |
| GOTO/PATT/xwave operand | stream bytes | `MP2KReadAddressOperand` → `HostResolveGbaAddr` → verbatim zone |
| song header tone | verbatim zone (ROM addr of voicegroup) | `HostResolveGbaAddr` → transformed zone (per-table span) |
| instrument wav/group/keysplit | transformed zone (built at publish) | direct native pointer (no runtime resolution) |
| cry table row wav | transformed zone (built at publish) | direct native pointer via `SetPokemonCryTone` |
| `gSongTable[].header` | compiled (ROM addr constant) | `HydrateSongHeader` → `HostResolveGbaAddr` |
| cry bytecode gotoTarget | BSS | runtime patch (`HostPointerToGbaAddr`, m4a.c:1827) — unchanged |

## 6. State v5 integration (R12-F)

- Register every published audio range in the R10 reverse range index:
  verbatim zone per-resource sub-spans (identity+offset semantics preserved
  even inside the consolidated arena) with role **CANONICAL** (payload
  bytes) — one range per resource; transformed voicegroup/cry records with
  role **COMPAT_OBJECT** (hydrated objects). Overlap rejection keeps the
  consolidated arena honest (per-resource sub-spans are disjoint by
  construction).
- Mid-play saves now produce audio sidecar records for every live pointer
  into audio data inside GAME_BSS (`sHostSongHeaders[].tone/parts`,
  `cmdPtr`/`patternStack`, `tone.wav`, `chans[].wav/currentPointer`): the
  v5 walker resolves them via the range index (hard-fail on any
  hull-inside-unregistered pointer — the current behavior, native_state.c:
  1924-1935, becomes the regression canary).
- Post-load: sidecar `ResolveByKey` (identity + offset, refuse on mismatch)
  re-derives arena pointers before any live write; then the R12 audio
  republish joins `EmeraldResourceCompat_Republish` +
  `NativeWorldNeighborhood_Invalidate` (native_state.c:3060-3074): rebuild/
  re-register the audio arena (republish is allocation-free and idempotent —
  the arena persists; only registration + transformed-zone pointers are
  re-derived), re-hydrate `sHostSongHeaders` against the restored values,
  keep `Platform_AudioClearQueue()` + unpause (3041-3042) exactly as today.
- Session fingerprint (provider digest over pack content) already gates
  arena-layout identity across restarts — no new fingerprint work; a
  different pack → refuse before mutation (2860-2879).
- Range budget: ~2,040 existing + ~1,301 audio ≤ 3,341 < 4,096
  (`NATIVE_STATE_MAX_RESOURCE_RECORDS` is a sidecar-records bound, 4096 —
  re-pin both cap tests when adding the audio harness).

## 7. Runtime lifecycle ordering

1. ROM verification + pack open + session + fingerprint (unchanged,
   desktop_game_content.c:751-755).
2. `EmeraldAudioCompat_TryInitialize` (new, inside step 1's transactional
   batch, after tileset): resolve + verify ALL 1,301 resources (type/schema/
   winner==ROM_BASE/exact sizes), build the arena, transform voicegroups/
   cry tables, register logical ranges + range index entries, publish the
   compiled slots. Failure → hard-fail init (compiled leaves are gone by
   then; the R7A contract). This is BEFORE `Platform_AudioInit` and
   `m4aSoundInit` (sdl2.c:1436, main.c:108) — the first mixer frame can only
   see published data.
3. Save-state load: sidecar resolution → republish (allocation-free) → SDL
   queue clear → mixer continues from restored state.
Ordering hazard to keep pinned in tests: no `m4aSongNumStart` may run
between `ClearMigratedEntries` and republish completion (the seam's
transactional contract already covers this via init/clear/republish
sequencing).

## 8. Isolation proof chain

For every migrated family, two equalities must hold:

1. **Source == GBA ELF/ROM**: decomp/generated artifact (compiled song
   stream bytes, `.bin`-generated sample bytes — both derived from tracked
   `.s`/`.mid`/`.aif` via the pinned tools) == qualified-ELF symbol slice ==
   retail ROM bytes (the three-way gate of §3).
2. **ROM extraction == pack == published representation**: pack payload ==
   ROM slice byte-for-byte; at runtime the verbatim zone contains the pack
   bytes unchanged; transformed-zone records are a documented, deterministic
   function of the canonical 12-byte rows.

**Counts as migrated audio symbols (must be ABSENT from release + DINFO
nm/objdump after R12-G):** `mus_*` (210), `se_*` (269), `ph_*` (51) song
symbols and their `_N`/`_N_B1` stream labels; `DirectSoundWaveData_*` (156);
`Cry_*` (388); `ProgrammableWaveData_*` (25); `voicegroup_*` (195 incl. the
5 `_keysplit`); `keysplit_*` (5); cry-table row data. Total payload bytes
removed from the executable ≈ **3.2-3.4 MiB** (audit §5).

**Do NOT count as migrated (remain compiled, documented):** `gSongTable`,
`gMPlayTable`, `gCryTable`/`gCryTable_Reverse` routing symbols (values are
published/re-emitted constants), `dummy_song_header` (4 B),
`gPokemonCrySongTemplate` (33 B engine template), m4a_tables.c engine
tables, MPlayDef constants, songs.h/cry_ids.h indices, and all MP2K engine
code. Isolation runner exemptions: 4-byte-pattern coincidences (the R11-B
sha-keyed mechanism) and the pinned compiled exceptions above.

## 9. Automated test plan (new tests/emerald_audio_compat_test.c + runners)

- **Extractor exactness**: generated audio inventory (`--check`): 530/544/
  25/195/2/5 pinned counts; every computed size validated against qualified
  ELF + retail ROM; three-way byte equality per family.
- **Manifest/pack determinism**: `gen3-elf-manifest` + `gen3-pack-build`
  re-run byte-identical (`--check`); production proof pins updated.
- **Sample parity**: every published WaveData == pack == ROM slice; loop
  metadata (freq/loopStart/size) spot-checked against the retail ROM.
- **Song graph reconstruction**: stream walker validates every GOTO/PATT/
  xwave operand target inside slice/known start (extract-time); runtime
  GOTO loop test — a looping BGM's `_B1` jump resolves to the same arena
  address across two republishes (cross-restart).
- **Voicegroup/instrument reconstruction**: all 20,594 rows transform to
  24-byte records with correct union dispatch (type 0/8/16 → wav; 0x40 →
  group+keysplit; 0x80 → group; PSG rows carry config, NULL pointers);
  pointer fields == target arena addresses; byte-level parity of the
  canonical 12-byte rows vs pack.
- **Cries**: `Cry_Bulbasaur` forward/reverse (type 0x20/0x30) resolve to
  the same sample; `SetPokemonCryTone` publishes the transformed row;
  bytecode gotoTarget patch intact.
- **SFX/fanfares**: `se_use_item`, `mus_level_up` start/stop/priority via
  the self-test harness.
- **Looping BGM / transitions / map-music change / battle music**:
  `m4aSongNumStartOrChange` on the same song continues; song switch swaps
  hydrated headers; covered by the PCM-determinism harness with
  `mus_route101` → `mus_b_frontier` (or equivalents) at fixed frame counts.
- **Cry playback**: cry over BGM, both cry slots (MAX_POKEMON_CRIES=2).
- **Active audio + State v5 save/load**: extend
  `tests/emerald_resource_state_test.c` (create/load + audio modes): save
  mid-play with arena-resident pointers; assert sidecar records carry audio
  keys (count = battle rows + audio rows) with in-band zeros; fresh-process
  load re-derives arena pointers via ResolveByKey (`!=` creator addresses,
  `==` loader arena base+offset); scalar-coincidence plants that fall inside
  an audio hull must fail-if-unregistered / round-trip-if-registered;
  fingerprint-change rejection with an audio pack edit; PHASE-B-style 40-
  frame mixer PCM determinism after load. Flip `--native-audio-self-test`
  assertions (sdl2.c:1003-1011) to arena-resident.
- **Resource-session relocation**: rebuild session/republish under a moved
  arena base; all consumers (song start, note-on, xwave, cry) follow.
- **Corrupt/missing audio refusal**: transactional TryInitialize failure on
  wrong-size sample, bad song graph, missing resource → init hard-fails,
  diagnostics named; no partial publication.
- **Release + DINFO builds**: full 13-runner battery on both.
- **Sanitizers**: ASan/UBSan compat test variant.
- **Executable asset-isolation audit**: nm/objdump sweep of both binaries
  for every §8 migrated symbol class; sha-keyed exemptions only.

## 10. Manual validation checklist (after implementation)

DINFO build, then: 1. boot → title fanfare; 2. Littleroot BGM loops cleanly
(loop point, no click); 3. route transitions change music; 4. wild battle
music + cry; 5. gym-leader battle fanfare; 6. SE spam (menu, item, save,
PC) with no dropouts; 7. Bard phoneme editing plays edited sounds; 8.
save-state mid-BGM → quit → relaunch → load → music continues at the same
spot, PCM-identical to no-save control; 9. quick-save/quick-load spam
during battle music + cry overlap; 10. volume/stereo pan sane; 11. release
build same as 1-8; 12. corrupt-pack refusal (rename pack → clear error, no
crash). Compare against pre-R12 behavior; any audible delta is a stop.

## 11. Implementation stages (R12-A…G)

Each stage: owned family, seam, automated gate, manual gate where noted,
rollback boundary. No one giant patch.

- **R12-A — audit finalization + prerequisite fix.** Land the 76 canonical
  `.aif` restore (audit §6/§10 — prerequisite). Add the audio type
  vocabulary + elf-manifest compatibility rows. Write
  `gen_audio_inventory.py` (530/544/25/195/2/5 pinned, `--check`).
  Gate: inventory green; `git diff --check`; .aif restoration verified
  against the pret blobs. Rollback: revert of the .aif commit; no code
  behavior changed.
- **R12-B — samples + programmable waves.** Extract/pack the 569 leaf
  resources; publish verbatim zone; no logical-range registration yet
  (nothing consumes the arena). Gate: extractor three-way equality; pack
  determinism; published bytes == ROM; no behavior change. Rollback:
  seam not wired into init.
- **R12-C — voicegroups + cry tables + keysplits.** Migrate 202 structural
  resources; publication transform (12→24 B); register transformed-zone
  logical spans; publish cry-template/voicegroup_dummy pinned slots; keep
  compiled data as the GBA parity path. Gate: transform parity test
  (20,594 rows); sample-pointer publication test; BGM still PCM-identical
  (self-test) — this is the first behavior-visible stage. Rollback: keep
  compiled tables until the self-test gate passes (additive-degrade seam).
- **R12-D — songs.** Migrate 530 song graphs into the verbatim zone; add
  `HostMemoryRegisterLogicalRange` + `HostResolveGbaAddr` lookup; emit
  `gSongTable` ROM-address values; `HydrateSongHeader` path unchanged.
  Gate: song-graph walker validation; GOTO/loop runtime test; BGM/SE/
  fanfare parity battery. Rollback: compiled song objects remain in the
  link until the parity battery is green (R7A removal is the last commit of
  the stage).
- **R12-E — cries/SFX/fanfares end-to-end.** Cry playback paths, phoneme/
  Bard flows, dummy-header pins, priority/transition matrix. Gate: cry +
  overlap + Bard tests; manual checklist items 1-7, 10.
- **R12-F — State v5 + lifecycle.** Register audio ranges/roles in the
  range index; sidecar capture/load tests; post-load republish wiring;
  self-test expectation flips; cap re-pins. Gate: full state battery incl.
  mid-play save/load PCM determinism; manual items 8-9. Rollback: audio
  stays fully published but range registration can be disabled (saves
  during playback then fail closed — the documented interim contract).
- **R12-G — final isolation.** Remove audio leaves from the native link
  (data/sound_data.s native branch emptied; song objects dropped from
  Makefile_pc OBJS — GBA build untouched); isolation runner with §8
  exemptions; release + DINFO rebuilds; absence proof; finalize `.aif`
  status documentation. Gate: isolation sweep clean on both binaries;
  full 13-runner battery; manual checklist complete. Rollback: the compiled
  objects are restored by reverting the link-line change only.

Stop conditions: STOP before R12-C if the voicegroup transform cannot be
made bit-exact against the canonical rows; STOP before R12-D if
`HostResolveGbaAddr` hot-path regression appears (perf or behavior);
STOP after R12-F if any mid-play save/load PCM mismatch; no commit/push
without the manual gate.

## 12. Highest-risk items (ranked)

1. **Mid-play save/load with arena pointers** (R12-F): the v5 walker fails
   closed on unregistered arena pointers — correct, but a missed range or
   role means every mid-song save hard-fails. Mitigation: per-resource
   sub-span registration in R12-F with the hull-canary test, plus the
   existing sidecar refusal semantics.
2. **`HostResolveGbaAddr` hot path** (R12-D): one span check + binary
   search per resolution; must not regress mixer determinism. Mitigation:
   the two-zone lookup; self-test PCM battery as the gate.
3. **Song slice size computation** (R12-A/B): `st_size == 0` for asm
   symbols; sizes must come from structure + ELF layout, with the
   three-way gate catching every mistake.
4. **Voicegroup transform fidelity** (R12-C): the 12→24 B mapping must
   reproduce the LINUX64 assembler branch exactly (union dispatch, padding).
   Mitigation: generator emits the transform from the same macro table the
   assembler uses; parity test on all 20,594 rows.

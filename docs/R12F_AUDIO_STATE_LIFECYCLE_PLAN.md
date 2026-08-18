# R12-F Plan — Audio State v5 Lifecycle Completion

Read-only completion review of the R12-E working tree (branch
`agent/prepare-v0.1.0-alpha`). No production code modified. R12-G is
explicitly out of scope (§15).

Grounding docs: `docs/R12_AUDIO_RESOURCE_AUDIT.md`,
`docs/R12_AUDIO_OWNERSHIP_ARCHITECTURE.md` (§6 is the R12-F contract),
`docs/R12C_STRUCTURAL_AUDIO_MIGRATION_REPORT.md` (§7/§10),
`docs/R12D_SONG_GRAPH_MIGRATION_REPORT.md` (§11),
`docs/R12E_LIVE_AUDIO_CUTOVER_REPORT.md`, `docs/R10_NATIVE_STATE_V5_DESIGN.md`.

## 1. Current audio State-v5 coverage (Q1)

Every live audio pointer family that can exist in serialized GAME_BSS, with
its audited owner, current range role/granularity, and proof status. All
audio engine globals are top-level `src/*.o` `.bss` → `game_bss`
(`ld_script_native.ld:18-24`) → serialized as `STATE_SECTION_GAME_BSS`
(`native_state.c:325-327`). The mixer emulator (`struct AudioCGB gb`,
`apuFrame`/`apuCycle`, `soundChannelPos`) is host `.bss` — NOT serialized.
All const audio tables (`gSongTable`, `gMPlayTable`, cry templates,
`voicegroup_dummy`) are `.rodata` — NOT serialized. The capture walker is
generic/address-space based (`NormalizeRuntimeBytes`,
`native_state.c:1968-2124`): every 8-byte window at 4-byte stride.

| Family (audited site) | Container / section | Current range role + granularity | Capture proven | ResolveByKey proven | Coverage today |
|---|---|---|---|---|---|
| Hydrated song header `tone` | `sHydratedSongHeaders[640].header.tone` (m4a.c:58-85, game_bss) | COMPAT_OBJECT, per-voicegroup span (24-B rows incl. drumset pad) | canary (sdl2.c:686-702) + byte-equality (1060-1072) | scenario save/load 13-15 (behavioral) | **final granularity**, not named in a test |
| Hydrated song header `parts[i]` | same array | CANONICAL, per-song span (byte granularity) | canary | behavioral | **final**, not named |
| Player `songHeader`, `tracks`, `memAccArea`, `musicPlayerNext` | `gMPlayInfo_*` + `gPokemonCryMusicPlayers[2]` | in-band PDIM (game_bss image identity) | harness (emerald_resource_state_test.c:1594-1599); self-test (sdl2.c:1138-1205) | same | final (image identity) |
| Player `tone` (voicegroup) | `MusicPlayerInfo.tone` | COMPAT_OBJECT, voicegroup span | **not explicitly asserted** | behavioral only | **final granularity, untested** |
| Track `cmdPtr`, `patternStack[3]` (patternStack is `u8*[3]` inside the track struct; no separate area) | `gMPlayTrack_*`, `gPokemonCryTracks[4]` | CANONICAL, per-song span (interior offsets) — cry tracks: PDIM (BSS bytecode) | canary pre/post load (sdl2.c:707-727, 944, 1295) | behavioral | final |
| Track instrument copy `tone.wav` / `tone.group` / `tone.keySplitTable` (the whole 24-B row is copied into the track at note time and dereferenced from the LIVE COPY) | `MusicPlayerTrack.tone` | wav: **coarse verbatim-zone span (synthetic key)**; group: voicegroup span; keySplitTable: **coarse verbatim-zone span, wrong type (AUDIO_SAMPLE for an INSTRUMENT_BANK/schema-2 run)** | **not explicitly asserted (keysplit family untested)** | behavioral | **coarse — must change (§3)** |
| Channel `wav`, `currentPointer` | `gSoundInfo.chans[12]` | **coarse verbatim-zone span** | harness (1602-1605, file 1902-1905); self-test (1229-1242) | harness AUDIO-LOAD (`chans[1].wav == loader base`) + TEST A | **coarse — must change** |
| Channel `track`, `prev/nextChannelPointer` ring | `SoundChannel` | PDIM | harness (1600-1606); self-test (1210-1226) | same | final |
| `cgbChans`, `musicPlayerHead`, `MPlayJumpTable`, `*Func` heads | `gSoundInfo` heads | PDIM / PFIM (36 jump entries PFIM) | harness (1570-1577, 1848-1861); self-test (1090-1132) | same | final |
| CGB channel `wavePointer`, `currentPointer`, `track`, `prev/next` | `gCgbChans[4]` | wave/current: **coarse verbatim-zone span**; track/ring: PDIM | **not individually asserted** (byte-equality only) | behavioral | **coarse — must change** |
| Cry `tone` (transformed cry-table row) | `gPokemonCrySongs[2].tone` + `sHostPokemonCrySongHeaders[2].tone` | COMPAT_OBJECT, per-cry-table span (forward/reverse keys, row×24 offset) | harness plant (chans[2].wav with cry-table key: 1740-1743, 1885-1899, 1997-2000) | harness (fixture `== cryForwardBase`) | final (real cry play-through untested) |
| Cry bytecode `parts[0/1]`, cry track `cmdPtr`, `bytecode.gotoTarget` | `gPokemonCrySongs[].bytecode` etc. | PDIM (BSS bytecode — the designed `allowBssBytecode` exception); gotoTarget = opaque 4-byte GbaAddr | self-test chain (1181-1191) | same | final by design |
| `gMPlay_PokemonCry` | EWRAM slice | PDIM | not explicitly asserted | — | final (image identity) |
| `gMPlayMemAccArea`, `sHydratedSongHeaders[].address` keys | game_bss | opaque data (no pointers) | scalar classes (harness 1767-1789) | — | final |

Audited live-pointer gaps R12-F must close: **leaf `wav`/`currentPointer`/
`wavePointer`/`currentPointer`, track `tone.wav`, and track
`tone.keySplitTable`** all capture under the synthetic `emerald:audio/
verbatim-zone` key today — no per-resource identity, and for keysplits the
wrong type/schema. A transformed keysplit pointer IS live and serialized
(the 24-B instrument row copied into `track->tone` at note time), so this is
a real, current coverage gap, not a hypothetical one.

## 2. Range-index state today (Q2)

Audited facts (see §16 for the arithmetic summary):

- Index: `src/emerald/resources/emerald_resource_ranges.c` (+header),
  owned by the trainer seam, `EmeraldResourceCompat_GetRangeIndex()`.
  **8,192 ranges / 8 hulls.** Sorted insert, overlap rejected, half-open
  `[base, base+length)`. Roles `CANONICAL=0 / LEGACY_LZ=1 /
  COMPAT_OBJECT=2` (header:68-73; mirror enum native_state.c:79-84).
  Hulls: separate `{base,length}` list, may overlap ranges, linear `InHull`.
  `ResolveByKey` (ranges c:180-204): unknown key → false; type/schema/role
  mismatch → false ("refuse, never guess"); `rangeOffset >= length` →
  false.
- Audio registration (`emerald_audio_compat.c:378-497`): 728 spans —
  **1 verbatim leaf-zone span** `[zoneBase, +0x284FA0)` = 2,640,288 B
  (CANONICAL, AUDIO_SAMPLE, synthetic key `emerald:audio/verbatim-zone`;
  covers all 569 leaves + 5 keysplit runs + ~255 KB of unbacked holes) +
  **530 per-song CANONICAL** spans (MUSIC_SEQUENCE; tile
  `[0x284FA0, 0x32BFB4)` = 684,052 B exactly) + **197 COMPAT_OBJECT
  spans** (INSTRUMENT_BANK: 195 voicegroups + 2 cry tables; one WHOLE
  transformed block per span, `(backshiftRows+rowCount)×24` B, base
  includes the drumset back-shift pad). Keysplit records are explicitly
  skipped (`if (record->kind == STRUCTURAL_KEYSPLIT) continue;`, c:419-420).
  The 364×24-B keysplit-group region of the transformed zone gets no span.
- Merged: 4,518 pre-audio (196 trainer + 1,608 pokémon + 288 object-event +
  1,544 tileset + 882 layout) + 728 = **5,246 of 8,192** (64%), pinned at
  `tests/emerald_native_world_real_test.c:1121`.
- Overlap behavior: rejected, no merge — the leaf/keysplit per-resource
  split (§3) must therefore prove exact non-overlapping tiling, the same
  phase-1c method as the songs (all leaves end by 0x088FC03B; keysplit runs
  at distinct ROM offsets).
- Leaf/wave identity: NOT represented (zone key). Keysplit identity: NOT
  represented. Cry tables: individually distinguishable (forward/reverse
  keys, `GetCryTableSpan`). Voicegroups: individually registered (195).
  The coarse zone range loses per-resource identity — confirmed.
- Hull today: ONE whole-allocation hull
  (`sizeof(arena)+zoneOffset+spanSize+transformSize` = 3,939,108 B at the
  current layout), added at publish/republish; the arena struct header,
  record tables, transform alignment gap, verbatim holes and the 3,428-B
  unbacked tail `[0x089A3050, 0x089A3DB4)` are hull-only. Hulls in use:
  5 trainer-family exposed-span hulls + 1 audio = **6 of 8**.

## 3. What R12-F should change (Q3)

**Replace the coarse verbatim-zone span with per-resource CANONICAL ranges
for the 569 leaves + 5 keysplit tables** (574 ranges). The architecture §6
contract ("per-resource sub-spans … one range per resource") was only
forward-pulled minimally in R12-C/D; this is the R12-F completion.

Per family — which resources need individual ranges:

| Family | Per-resource ranges? | Why |
|---|---|---|
| 105 root samples | **YES** (105, CANONICAL, AUDIO_SAMPLE/1) | live `wav`/`currentPointer`/`tone.wav` targets; per-sample sidecar identity |
| 388 cry samples | **YES** (388) | live channel pointers during cry playback |
| 51 phoneme samples | **YES** (51) | Bard phoneme path live pointers |
| 25 programmable waves | **YES** (25) | `gCgbChans[].wavePointer/currentPointer` live targets |
| 5 keysplit tables | **YES** (5, INSTRUMENT_BANK/**schema 2**) | `track->tone.keySplitTable` is a LIVE serialized pointer; today it captures under the zone key with the wrong type |
| 530 songs | already per-resource (unchanged) | final |
| 195 voicegroups | already per-table COMPAT_OBJECT (unchanged) | final |
| 2 cry tables | already per-table COMPAT_OBJECT (unchanged) | final |

Nothing may remain covered only by a coarse hull/span: the hull's job after
this change is failure detection (§5), not identity. The 80 dummy song rows
stay non-resources (compiled 4-byte header, PDIM) — no ranges.

Consequences (each is a R12-F deliverable):
1. Sidecar records for every leaf/keysplit pointer carry the resource's own
   key + type/schema (§7 tests).
2. Pointers into the ~255 KB of unbacked holes fail capture instead of
   capturing as `zone+offset` (which today resolves to unbacked arena
   memory — a silent mis-resolution).
3. `ResolveByKey` refuses foreign/stale leaf keys at load.
4. Registration must prove leaf+keysplit non-overlap tiling (phase-1c
   method) and re-pin the count tests: 728 → 1,301 audio spans, merged
   5,246 → 5,819 (§6, §13).

## 4. Canonical vs COMPAT_OBJECT role mapping (Q4)

Verified against the audited registration and the R12 taxonomy:

| Family | Arena representation | Role | Type | Schema | Granularity |
|---|---|---|---|---|---|
| songs (530) | verbatim GBA bytes | CANONICAL | MUSIC_SEQUENCE | 1 | per song (unchanged) |
| samples root/cry/phoneme (544) | verbatim WaveData | CANONICAL | AUDIO_SAMPLE | 1 | per sample (new) |
| programmable waves (25) | verbatim 16 B | CANONICAL | AUDIO_SAMPLE | 1 | per wave (new) |
| keysplit tables (5) | verbatim `.byte` runs | CANONICAL | INSTRUMENT_BANK | **2** | per table (new) |
| voicegroups (195) | transformed 24-B rows | COMPAT_OBJECT | INSTRUMENT_BANK | 1 | per table (unchanged) |
| cry tables (2) | transformed 24-B rows | COMPAT_OBJECT | INSTRUMENT_BANK | 1 | per table (unchanged) |

Transformed resources: **one resource = the whole transformed table block.**
Do NOT create per-row identities: rows are fixed-width engine records;
`rangeOffset` inside the table span pins the exact row and the sidecar key
pins the table. Per-row subranges would add 21,370 ranges with zero
identity value. The drumset back-shift pad stays inside the block range
(the back-shifted label points at it — audited, c:421-431).

## 5. Hull semantics (Q5)

**Decision: replace the ONE whole-allocation hull with TWO zone hulls — the
verbatim zone minus its unbacked tail, plus the transformed zone.** This is
option C/D from the review framing, applied with the R10 exposed-span
lesson:

- Hull 1 = `[zoneBase, zoneBase + 0x32BFB4)` — leaf sub-zone + song block
  (contiguous; the 3,428-B tail `[0x32BFB4, +0x32CD18)` is EXCLUDED).
- Hull 2 = `[transformBase, transformBase + transformSize)` — the whole
  transformed zone.

Rationale (audited mechanics):

1. **R10 exposed-span lesson**: the shipped R10 fix hulls only the exposed
   stream region, NOT the arena prefix ("an 8-byte window that happens to
   be numerically inside it is ordinary data … hulling it would fail
   captures on coincidental values such as packed sprite pixels or M4A
   channel byte fields", `emerald_resource_ranges.h:35-44`; regression test
   `emerald_resource_state_test.c:852-979`). The current audio hull
   INCLUDES its arena header + record tables — the class R10 excluded — and
   has already produced one false positive (the `apuCycle` window special
   case, `native_state.c:1992-1997`). `RuntimeLocationIsModeledScalarOrPadding`
   does not model M4A channel fields, so the audio band remains the
   riskiest hull band in the tree.
2. **Fail-closed is preserved without the full band**: with §3's per-resource
   tiling, every exposed audio byte is ranged. A true live pointer always
   captures. A stale/corrupt pointer into the current arena's zones hits a
   hull → hard capture failure with the resource diagnostic. A stale
   pointer into a PREVIOUS (freed) arena — or into the excluded tail/prefix —
   falls through to the existing unmanaged-pointer gate
   (`LooksLikeExternalHostPointer` = value ∈ [0x50T, 0x80T) AND
   `IsMappedHostAddress`, native_state.c:386-400; freed malloc arena memory
   remains mapped, so the gate catches it) → hard capture failure.
3. **False positives are confined to the exposed bands**: scalar pairs
   numerically inside the arena prefix/tail now pass as ordinary data (R10
   behavior); only the exposed zones can false-positive, and those are the
   only bands where a true pointer can legitimately exist. No hull covers
   "everything", so no scalar class is newly exposed.
4. Hull cap: 5 trainer exposed-span hulls + 2 audio = **7 of 8** — fits
   without a cap change.

Explicit non-goal: per-hole hulls (in-expressible with `[base,length)` and
unnecessary — the unmanaged gate covers them).

## 6. Range caps / sidecar caps (Q6)

Current constants and usage (audited):

- `EMERALD_RESOURCE_RANGE_INDEX_MAX_RANGES = 8192` — usage **5,246 (64%)**.
- `EMERALD_RESOURCE_RANGE_INDEX_MAX_HULLS = 8` — usage **6**.
- `NATIVE_STATE_MAX_RESOURCE_RECORDS = 4096` (64-B records; capture
  hard-fails past the cap; file reserves 4 + 64·4096 B).

Projected after §3/§5:

- Ranges: 4,518 + **1,301** = **5,819 of 8,192 (71%)** — headroom 2,373
  (29%). NO raise.
- Hulls: 5 + 2 = **7 of 8**. NO raise.
- Sidecar records — worst-plausible live audio count:

  | Source | Worst plausible | Theoretical max |
  |---|---|---|
  | hydrated cache `tone` + `parts[]` (idle entries stay in the permanent cache forever) | ~200 songs started × ~5 = 1,000 | 610 × 11 = 6,710 |
  | player `tone` (4+2), cry `tone` (2+2) | 10 | 10 |
  | tracks (23+4): `cmdPtr` + `patternStack[≤3]` + instrument `wav`/`group`/`keySplitTable` (≤3) | ~120 | 189 |
  | `chans[12]` `wav`+`currentPointer` | 24 | 24 |
  | `gCgbChans[4]` `wavePointer`+`currentPointer` | 8 | 8 |
  | overworld/game-state records (pre-audio classes) | ~50 | ~100 |
  | **Total** | **~1,200** | **~7,000** |

  The theoretical maximum (hydrate all 610 songs with all 10 tracks live in
  the cache in one session) exceeds 4,096; the plausible bound is ~1,200.
  **Decision: NO cap raise now** (the R11C discipline — raise only with
  exact arithmetic + re-pinned tests). Instead R12-F adds a **measurement
  gate**: a constructed save that hydrates many songs (target ≥ 300
  distinct songs started in one harness run) and pins the observed record
  count well below 4,096. If the measured count ever reaches ~70% of the
  cap, raise to 8,192 with this arithmetic and re-pin both cap tests
  (the file header already reserves the sidecar lazily; the 32-MiB file
  cap and u32 counts accommodate 8,192 records = 512 KiB sidecar).
  Capture fails CLOSED past the cap (refuses the save, never corrupts), so
  an over-cap session is loud, not silent.

## 7. Sidecar identity tests (Q7)

New tests in `tests/emerald_resource_state_test.c` + the scenario battery,
for every family from §1. For each: save with the pointer live, then assert

1. in-band pointer zeroed in the file,
2. sidecar record present with correct `sectionTag`/`fieldOffset`,
3. `resourceKey` == the family's key (leaf/keysplit keys after §3 — NOT the
   zone key),
4. type/schema/role == §4 mapping,
5. `rangeOffset` == pointer − range base (row×24 for tables),
6. fresh-process load resolves to the loader's arena address
   (== loader base + offset, ≠ creator address),
7. the adjacent scalar bytes round-trip unchanged.

Minimum family set: song `cmdPtr` (interior offset), song `patternStack[]`,
hydrated `parts[]`, player `tone` (voicegroup), track `tone.wav`,
track `tone.keySplitTable` (NEW — keysplit key, schema 2),
channel `wav`, channel `currentPointer` (interior), cry `tone`
(cry-table row), `gCgbChans[].wavePointer` (NEW — per-wave key),
plus a real cry play-through (NEW) and a hydrating-many-songs record-count
run (§6). Plant-based (harness) where the real path is hard to script;
real-path (scenario battery) for cmdPtr/patternStack/wav/currentPointer/
cry tone.

## 8. Session relocation (Q8)

The R10 TEST 4/5/7/8 matrix exists for the battle family; R12-F extends
every cell to audio:

| Case | Expectation | Where |
|---|---|---|
| same content, different arena base (fresh process) | loads; pointers == loader arena (TEST 3/TEST A already prove this for vg/cry bases) | existing + extended |
| same content, different pack path | loads (fingerprint is content-derived — TEST 5) | extend with live audio pointer |
| different content fingerprint (audio payload edit) | refuse before mutation (canary intact) | extend TEST 4 |
| missing audio resource | session REGISTRATION refuses (R12-E refusal contract) — no load path reaches it | refusal test + seam S1-S6 |
| changed audio resource (same pack size class) | fingerprint mismatch → refuse | extend TEST 4 |
| corrupt sidecar resourceKey | refuse before mutation | extend TEST 7 |
| corrupt rangeOffset (OOB) | refuse before mutation (`ResolveByKey` bounds, audited ranges c:196-199) | extend TEST 8 |

No native address identity participates anywhere: sidecar = key + offset;
resolution = loader's range base + offset; the fingerprint hashes logical
content only.

## 9. Post-load ordering (Q9)

Audited current order (`native_state.c:2817-3117`): header/buildId → session
fingerprint (refuse) → slice validation → sidecar parse+validate →
**ResolveResourceRecords (all records, pre-restore)** → raw restore →
pointer patch → frame counter → `Platform_AudioClearQueue` +
`AudioSetPaused(FALSE)` → video framebuffer → trainer republish (non-OK →
clear + continue) → audio republish (session gate; non-OK → fail load) →
`NativeWorldNeighborhood_Invalidate`.

Two audited defects to fix:

1. **Unpause precedes the republishes** (steps at 3042-3043 run before
   3061-3106). The worker is parked at VBlank during load (sdl2.c:155-177;
   `VBlankIntrWait`), and the mixer runs only on the worker
   (main.c:375-385), so no frame runs mid-load — but the unpause-early
   order makes the claim structural rather than accidental.
2. **Refusal after mutation**: if the audio republish fails, the load
   refuses AFTER game memory was restored, patched, and unpaused, with the
   arena freed — the resumed game holds pointers into freed memory. This
   violates the R12-E "must not run" contract in spirit (the load fails,
   but the machine is left in a corrupt state).

**Final required order** (the R12-F change):

1. header/buildId/CRC → refuse (no mutation)
2. session fingerprint → refuse (no mutation)
3. slice validation (no mutation)
4. sidecar parse + validate (no mutation)
5. resolve EVERY record against the active index (no mutation)
6. **NEW — pre-flight republishes**: trainer republish then audio republish
   (session gate). Either failure → refuse; live GAME memory untouched
   (the republishes touch only session registration + compat tables, not
   the saved game state). Resolution in step 5 runs BEFORE the trainer
   republish rebuilds the range index (the rebuilt index drops the audio
   spans; the audio republish re-registers them; resolved pointers remain
   valid because republish is allocation-free — same arena).
7. raw slice restore (in-band zeros decode harmlessly)
8. pointer patch (pre-verified in step 5 — cannot fail)
9. frame counter, video framebuffer restore
10. `NativeWorldNeighborhood_Invalidate` (post-patch derived state)
11. `Platform_AudioClearQueue()` then `Platform_AudioSetPaused(FALSE)`
    (LAST — no mixer frame can run until every republish and invalidate
    succeeded)

Window proof: no mixer frame runs while the worker is parked (semaphore);
steps 6-11 run inside the parked region; the first frame after unpause
sees a fully republished session, repatched pointers, and registered
ranges. There is no window with restored pointers referring to an old
arena (resolution + patch use only the current arena), none with absent
logical mappings (audio republish re-registered them before unpause), and
none where the mixer can run before republish completes.

Contract note: the trainer republish failure currently degrades (clear +
continue, R6 behavior). R12-F makes BOTH republishes refusal-grade — a
load whose trainer tables were cleared cannot run anyway, and symmetric
refusal keeps the transactional story simple.

## 10. Hydrated song identity (Q10)

Audited: `sHydratedSongHeaders` is a top-level `src/*.o` static →
**game_bss → serialized in-band**, including `sHydratedSongHeaderCount`.
`tone`/`parts[]` are sidecar-captured (zeroed in-band) and re-derived by
the patch pass; the `GbaAddr address` keys round-trip as opaque data (the
8-byte window over the key + scalar bytes is never pointer-shaped); the
player's `songHeader` pointer into the array is a PDIM image-relative
record re-derived to the SAME slot after load (build-stable addresses).

Conclusion: **no special post-load treatment is required.** The R12-E
per-song cache is not a process-local structure — it is persistent state
with sidecar-repaired arena pointers, and its identity semantics
(pointer-equality song comparison) survive restart by raw bytes plus
re-derived pointers. No process-local cache pointer can leak into state
semantics because the cache itself is the serialized state.

R12-F adds only proofs + pins, no code path:
- a test: save/load round-trips a populated cache entry (same key, same
  slot, `parts/tone` == loader arena, `songHeader` == same slot),
- a test: `m4aSongNumStartOrContinue` after load continues (identity
  preserved),
- a pin: `sHydratedSongHeaderCount ≤ 610` reachability (the 640 bound's
  overflow fallback to slot 0 is unreachable — assert it stays so in the
  harness rather than silently trusting the comment).

## 11. Cry lifecycle (Q11)

Audited: `EmeraldAudioCryTableRow` returns a DIRECT transformed-zone row
pointer (no `HostResolveGbaAddr`); the compiled `gCryTable`/`_Reverse`
fallback is the session-less/offline path only (sound.c:53-55).
`gPokemonCrySongTemplate` is a compiled const (m4a_tables.c:258-288) —
engine template, pinned compiled, `.tone = &voicegroup_dummy` dead by
construction session-ful (SetPokemonCryTone always overwrites the tone).
`gPokemonCrySongs[2]` + `gPokemonCrySong` + `sHostPokemonCrySongHeaders[2]`
+ cry players/tracks are all game_bss, serialized; the tone is
sidecar-captured (cry-table span) and re-derived; the bytecode
parts/gotoTarget/cmdPtr are BSS-image identities (the designed exception).

Conclusion: **no cry-specific post-load rehydration is needed** — it works
by construction (in-band restore + sidecar tone + image-identity bytecode
pointers). R12-F adds the missing proofs: a real cry play-through
save/load (sidecar carries the cry-table row record; tone == loader arena
row; PCM continuation), and a both-slots test.

## 12. Failure contract (Q12)

Final R12-F contract (extending R12-E):

| Event | Behavior |
|---|---|
| startup publication failure (missing/corrupt required audio) | session refused; `--verify-game-data` exit 2; no startup (R12-E, unchanged) |
| save capture with unregistered audio pointer (zone hull / mapped-band) | hard capture failure — the save refuses, named diagnostic (unchanged mechanism; §5 narrows the bands) |
| load with missing resource key | refuse before mutation (`ResolveByKey` unknown key, audited) |
| load with wrong session fingerprint | refuse before mutation (fingerprint check precedes everything) |
| trainer or audio republish failure | refuse BEFORE restore (pre-flight, §9) — zero game-state mutation |
| logical-range registration failure | republish fails → load refuses (audio compat Republish → `RANGE_REGISTRATION`) |

All transactional; no silent compiled fallback in session-ful production
(unchanged R12-E contract). Save refusal is preferred over corruption in
every case.

## 13. Full R12-F test battery (Q13)

Automated gates (each a runner step; release + DINFO; sanitize variants
for the harness):

1. per-resource range registration: 1,301 audio spans pinned at
   publish/republish/relocated; 574 leaf+keysplit keys verified
   (type/schema/role per §4); overlap/tiling proof (phase-1c method);
2. hull pins: 2 audio hulls (verbatim-minus-tail + transformed), 7 total;
   prefix/tail excluded; hole pointer → capture refusal; prefix scalar
   window → passes as data (R10-style regression test);
3. merged-count re-pins: 5,246 → **5,819** (< 8,192) in
   `emerald_native_world_real_test.c` + import tests; 728 → 1,301 in
   `emerald_audio_compat_test.c`;
4. sidecar content inspection for every §7 family (7 assertions each);
5. same-process save/load (TEST 1) with live audio;
6. cross-process relocation (TEST 3/TEST A) with live audio;
7. same content different pack path (TEST 5) with live audio;
8. fingerprint mismatch refusal (TEST 4) with an audio edit;
9. unknown-key refusal, corrupt-offset refusal (TEST 7/8 matrix) extended
   to audio keys;
10. missing audio resource refusal (seam failure matrix S1-S6 + loader
    refusal test — existing, re-run);
11. mid-BGM save/load (scenario 13); mid-pattern (14);
12. cry-over-BGM save/load (NEW real cry); overlapping SFX/BGM save/load
    (NEW);
13. programmable-wave live pointer (scenario 10 + wavePointer sidecar
    record); phoneme live pointer (scenario 7 + sidecar record);
14. PCM continuation after load (40-frame digest == control) for every
    save/load scenario;
15. repeated quick-save/quick-load loop (spam test — existing regression
    runner extended);
16. hydration measurement gate (§6): hydrate ≥300 distinct songs, save,
    pin record count;
17. full regression battery: the R12-E 35-runner list + the new steps;
18. release + DINFO fresh builds; sanitizer variants.

## 14. Manual gate (Q14)

Short checklist (DINFO build):

1. BGM save/load mid-song → music continues in place, no dropout;
2. battle save/load (music + cry window) → identical continuation;
3. cry-over-BGM save/load → cry and BGM both resume;
4. quick save/load spam (~20 cycles) → no crash, no silence, no stale
   audio;
5. quit → relaunch → load → same music position, PCM-identical feel;
6. map transition after loading a mid-BGM save → new map music starts
   correctly (no stuck old song);
7. no audible dropout/click around the load boundary.

(Items 1-7 map to the R12-E checklist items 8-9, now the gate.)

## 15. What remains for R12-G (Q15)

Explicitly NOT in R12-F:

- removal of compiled audio payloads from the native link
  (`data/sound_data.s` native branch emptied, song objects dropped from
  Makefile_pc OBJS — GBA build untouched);
- link-line / sound_data.s cleanup;
- nm/objdump isolation proof (today's baseline: 8,270 compiled audio data
  symbols present and DEAD — the R12-E sweep);
- executable size reduction (~3.2-3.4 MiB);
- final full binary audit + isolation runner exemption finalization.

**All of the above is COMPLETE (2026-08-18)** — see
docs/R12G_AUDIO_ISOLATION_REPORT.md.

R12-F changes nothing about compiled payload presence — it only completes
state/lifecycle identity correctness on top of the R12-E live cutover.

## 16. Summary

- **Current audio range breakdown**: 728 = 1 coarse verbatim-zone span
  (CANONICAL, synthetic key, 2,640,288 B incl. holes) + 530 per-song
  CANONICAL + 197 per-table COMPAT_OBJECT (195 voicegroups + 2 cry
  tables). Merged 5,246/8,192; 1 whole-allocation hull (6/8 in use).
- **Final proposed breakdown**: 1,301 = 574 CANONICAL per-resource
  (569 leaves + 5 keysplits) + 530 song CANONICAL (unchanged) + 197
  COMPAT_OBJECT (unchanged). Merged **5,819/8,192 (71%)**.
- **Projected hulls**: **2 audio** (verbatim zone minus tail + transformed
  zone) → 7/8 total.
- **Cap changes**: NONE. Sidecar 4,096 retained with a measurement gate;
  conditional raise to 8,192 documented with exact arithmetic (§6).
- **Coarse verbatim coverage**: MUST be replaced — it loses per-resource
  identity for all 569 leaves and 5 keysplits, gives keysplit pointers the
  wrong type/schema (live serialized family), and silently resolves
  hole-band pointers to unbacked arena memory.
- **Final role mapping**: §4 table (songs/samples/waves/keysplits
  CANONICAL; voicegroups/cry tables COMPAT_OBJECT, whole-table, no
  per-row).
- **HydrateSongHeader**: no special post-load treatment — the cache is
  serialized in-band with sidecar-repaired pointers; R12-F adds proofs and
  the 610-bound pin only.
- **Final post-load ordering**: pre-flight both republishes (refusal before
  any restore) → restore → patch → invalidate → clear queue → unpause
  last (§9).
- **Highest remaining State-v5 risk**: the permanent hydration cache's
  sidecar record growth (theoretical max 6,710 > the 4,096 cap; plausible
  ~1,200) — capture fails closed, but a long session that starts many
  songs could refuse saves. Mitigated by the §6 measurement gate.
  Second: hull false-positive residuals in the M4A channel band — confined
  by the §5 two-zone hulls to exposed bytes only.
- **Complexity / estimate**: small-to-medium, ~4 code areas (span-list
  build + hull registration in the audio seam; two re-pins; the
  native_state pre-flight ordering; the test battery). Roughly 2-3 focused
  work sessions — comparable to one R12 sub-stage.
- **Flash implementation order** (inside R12-F):
  1. per-resource leaf/keysplit span build + tiling proof + 2-zone hulls;
     re-pin 728→1,301 / 5,246→5,819 / hull tests;
  2. native_state pre-flight republish + unpause-last (+ refusal-grade
     trainer republish);
  3. sidecar identity tests (§7) + relocation matrix (§8) + cry/wave/
     keysplit family tests;
  4. hydration measurement gate + record-count pin;
  5. full battery, release + DINFO + sanitizers;
  6. report + manual gate (§14).
- **Split R12-F?** No — the pieces share the same span/hull model and the
  same test harness, and each depends on the previous; a split would
  leave an intermediate tree whose saves capture under the coarse key
  (silently wrong identity) — worse than one coherent stage. Steps 1-2
  may land as sequential commits inside the stage, in that order.

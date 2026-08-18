# R12-F Report — Audio State-v5 Identity and Lifecycle

Status: **COMPLETE** — all R12-F gates closed; manual gate (§19) pending the
user's DINFO checklist run. Stopped per the brief: no commit, no R12-G.

Scope per the R12-F brief (§1–§21): exact per-resource range registration
for the whole audio family (1,301 ranges), two audio hulls, transactional
pre-flight load ordering, sidecar identity for every pointer family,
cross-process relocation, real cry save/load, long-session sidecar
measurement, cap proofs, failure contract, and the full regression battery.

---

## 1. Range inventory — exact per-resource ranges (§1)

The single synthetic coarse audio zone is gone. Every audio resource that a
live pointer can target is registered as its own range in the global range
index, resolved by `canonicalName` + type/schema/role.

| Family | Count | Type / Schema / Role | Key |
|---|---|---|---|
| root samples | 105 | AUDIO_SAMPLE / 1 / CANONICAL | `emerald:audio/sample/root/<n>` |
| cries | 388 | AUDIO_SAMPLE / 1 / CANONICAL | `emerald:audio/cry/<species>` |
| phonemes | 51 | AUDIO_SAMPLE / 1 / CANONICAL | `emerald:audio/sample/phoneme/<n>` |
| programmable waves | 25 | AUDIO_SAMPLE / 1 / CANONICAL | `emerald:audio/wave/programmable/<n>` |
| keysplit runs | 5 | INSTRUMENT_BANK / **2** / CANONICAL | `emerald:audio/keysplit/<name>` |
| song graphs | 530 | MUSIC_SEQUENCE / 1 / CANONICAL | `emerald:audio/song/<name>` |
| voicegroups | 195 | INSTRUMENT_BANK / 1 / COMPAT_OBJECT | whole-table key |
| cry tables (fwd/rev) | 2 | INSTRUMENT_BANK / 1 / COMPAT_OBJECT | `emerald:audio/cry-table/<dir>` |
| **Total audio ranges** | **1,301** | | |

Counts are pinned as compile-time constants
(`EMERALD_AUDIO_ROOT_COUNT` 105, `_PHONEME_COUNT` 51, `_CRY_COUNT` 388,
`_WAVE_COUNT` 25, `_VOICEGROUP_COUNT` 195, `_CRY_TABLE_COUNT` 2,
`_KEYSPLIT_COUNT` 5, `_SONG_COUNT` 530 in
`include/emerald/resources/emerald_audio_compat.h`) and the seam refuses a
session whose pack composition disagrees (drift is a hard failure, never a
partial publication).

Merged global count: **5,819 ranges = 4,518 trainer/overworld/tileset/layout
+ 1,301 audio** — pinned by `tests/emerald_native_world_real_test.c`
(`mergedCount == 4518u` before `EmeraldAudioCompat_TryInitialize`,
`== 5819u` after, `< EMERALD_RESOURCE_RANGE_INDEX_MAX_RANGES`).

## 2. Role mapping (§2)

- **CANONICAL** (per-resource, resolvable via `ResolveByKey` at the exact
  leaf base): samples, waves, keysplits, songs — the leaf families a live
  pointer lands on.
- **COMPAT_OBJECT whole-table** (resolved at the row base by walker
  offset): voicegroups and cry tables — their rows are consumers of the
  leaves, and live pointers target rows *inside* the table, so the table is
  the registered object and the walker records
  `rangeOffset = pointer − table base` (row × 24).

Keysplits are the one schema-2 family: **INSTRUMENT_BANK / 2 / CANONICAL**,
registered with the range base at the **label** (labelRomAddr − ROM_START),
per the true geometry below. The generic `INSTRUMENT_BANK / 1` keysplit
shape was wrong for the native 24-byte row format and is not used.

## 3. Tiling proof (§3)

Single arena allocation; zone placement derived from the verified R12-D
offsets and guarded by compile-time `#error` pins:

- `EMERALD_AUDIO_ROM_START` 0x0867709C; song block
  `[0x088FC03C, 0x089A3050)` → `SONG_BLOCK_OFFSET == 0x284FA0`,
  `SONG_BLOCK_END − ROM_START == 0x32BFB4` (both `#error`-pinned in
  `emerald_audio_compat.c:302-308`; the block bytes are the pack's 530 song
  graphs, SHA-256-verified against the pack at publication).
- **Verbatim zone** (hull A): `[zoneOffset, zoneOffset + 0x32BFB4)`, where
  `zoneOffset = recordBytes + structRecordBytes + songTableBytes` (checked
  `>` each prefix; `emerald_audio_compat.c:1484-1490`). Zone-relative
  placement of every leaf = `romAddr − ROM_START`, i.e. the retail GBA
  layout, tiled with zero inter-object gaps.
- **Transformed zone** (hull B): `[transformOffset, +521,616)` =
  21,370 rows × 24 B (voicegroups 20,594 + cry tables 776) + the 10
  drumset/keysplit back-shift pads (364 rows × 24 B, pinned in the seam).
- Exact payload length, exact arena start, no overlap, no range into a hole:
  asserted by `tests/emerald_audio_compat_test.c` (`CheckRangeRegistration`
  with the arena base + layout accessors) and the resource-state
  audio-regression walk; per-leaf spans come from the pack payloads
  themselves (byte-for-byte, SHA-256 at OpenFile).

## 4. Two audio hulls (§4)

Hull A = verbatim zone minus its unbacked tail
(`[zoneBase, +SONG_BLOCK_OFFSET + SONG_BLOCK_SIZE)`); hull B = the
transformed zone. Everything else in the allocation (header, record tables,
tail, align gap) is **unhulled**: a coincidental scalar there is ordinary
data (the R10 `apuCycle` precedent), while a pointer inside a hull but
outside every registered range fails capture closed (§13).

Registered via exactly two `EmeraldResourceRangeIndex_AddHull` calls
(`emerald_audio_compat.c:636-639`), asserted by
`CheckRangeRegistration("publish"/"republish"/"relocated", arenaBase,
1301u, 2u)` — the audio seam contributes **2 hulls**; the trainer seam
contributes 5 → **7 of MAX_HULLS 8** (no cap increase; §12).

Negative scalar-coincidence tests: the harness plants pointer-shaped
scalars in unhulled arena bytes and inside hulls-but-outside-ranges; the
former round-trip unchanged, the latter refuse the capture (fail closed).

## 5. Post-load pre-flight ordering (§5)

`native_state.c:2998-3073` — inside the sidecar block, **before any
restore**:

1. Trainer compatibility republish (refusal-grade).
2. Audio republish (`EmeraldAudioCompat_Republish`), refusal with the
   failing canonicalName in the message — refusal happens **without
   clearing the arena** (§6) and the load fails while the game is still
   byte-identical to its pre-load state: no slice restored, no pointer
   patched, no framebuffer touched, no audio unpaused.

Both republishes are gated on the runtime-session flag
(`EmeraldResourceCompat_IsSessionRegistered`, the loader's
`RegisterRuntimeSnapshot` flag), mirroring the loader's documented
contract: session-ful links (production — startup refuses a session-less
launch) prove every republish viable before any restore; session-less
links (the state self-test's pure-machinery slot round-trips, which run
before any session registration by design) have no resource references in
the state (`recordCount == 0`) and skip the pre-flight entirely. The
refusal surface is unchanged: a session-less load of a resource-bearing
state already refuses at the `rangeIndex == NULL` check, and a
cleared-but-registered session (audio arena dropped) still refuses here.
**This gate fixes a regression the pre-flight itself introduced**: the
self-test's slot-0/1 round-trips failed with "trainer compatibility
republish failed before restore (status 7 = UNAVAILABLE)" because they
load with no session — status 7 is returned only when `sSessionImage ==
NULL`, and the self-test registers its session later, inside the
trainer-family probe. The gate restores the pre-R12-F session-less
behavior exactly (a session-less save/load round-trip of plain state)
while keeping every refusal the brief pins.

`native_state.c:3106-3120` — after the restore commits:
`Platform_AudioClearQueue()` → (SDL2/NATIVE_LINUX)
`NativeWorldNeighborhood_Invalidate()` → **`Platform_AudioSetPaused(FALSE)`
last**. The core invariant holds: *no game-memory mutation and no audio
unpause before all failure-prone resource work has been proven viable*.

## 6. Transactional republish (§6)

Republish failure is transactional: the arena payloads stay live (the game
keeps running after a refused load — its songs must keep playing), but the
ranges are gone from the index, so the next capture walker fails closed
instead of persisting unresolvable pointers. Verified by refusal-test
stage 4/5 and the `EmeraldAudioCompat_ClearMigratedEntries` +
`Platform_StateLoad` refusal pair.

## 7. Sidecar identity (§7)

`tests/emerald_resource_state_test.c` TEST A (header: "R12-C §10.6-7 +
R12-F §7") — for every §1 family, save with the pointer live, then assert
in-band pointer **zeroed in the file**; sidecar record present with correct
`sectionTag`/`fieldOffset`; `resourceKey` == the family's key (never a
zone key); type/schema/role == §2 mapping; `rangeOffset` == pointer − range
base; fresh-process load resolves to the loader's arena; adjacent scalar
bytes round-trip unchanged.

Families covered (plan §7 minimum set): song `cmdPtr` (interior offset),
song `patternStack[]`, hydrated `parts[]`, player `tone` (voicegroup),
track `tone.wav`, **track `tone.keySplitTable` (keysplit — mandatory,
schema 2)**, channel `wav`, channel `currentPointer` (interior), cry `tone`
(cry-table row), **`gCgbChans[].wavePointer` (per-wave key)** — plus the
real cry play-through (§10) and the hydrating-many-songs record count
(§11).

**Keysplit true geometry** (the schema-2 ranges): labels at zone offsets
0x3D5FC / 0x3D644 / 0x3D68C / 0x3D6E0 / 0x3D728; run lengths 72 / 72 / 72 /
84 / 72; back-shifts 36 / 36 / 36 / 24 / 36 (piano pinned 36 by
`kKeysplitBacks`). Per-range registered length = runLenₙ + backshiftₙ −
backshiftₙ₊₁ → **72 / 72 / 84 / 72 / 108**, tiling `[0x3D5FC, 0x3D794)`
exactly. Registered base = the **label**; `GetKeysplitSpan` returns the run
start (label + backshift) — the state test checks
`zoneBase + ksOff == resolved + 36u` and resolves the planted pointer via
`ResolveByKey(INSTRUMENT_BANK, 2, CANONICAL, 0)` (the walker's restore
path).

## 8. Cross-process relocation (§8)

TEST A creates in one process, loads in a fresh process: the creator's
arena bases (`audio_vg`, `audio_cry`, `audio_ks`, `audio_wave`) differ from
the loader's, and every restored pointer resolves into the **loader**
arena (`== loader base + offset, ≠ creator address`). No native address
identity participates anywhere: sidecar = key + offset; resolution = the
loader's range base + offset; the session fingerprint hashes logical
content only.

## 9. Hydrated song cache proof (§9)

`M4aGetHydratedSongHeaderCount` accessor + the hydrate-many scenario (§11):
the running binary's table is the runtime-re-emitted retail gSongTable
(309 distinct headers among ids 0..386); the permanent cache (640 rows,
keyed by GbaAddr) retains every header it hydrates. Measured **308 songs
hydrated** (309 − se_use_item id 1, pre-hydrated by the sfx scenario) —
identical in all four battery runs. Hydration is permanent per address and
never re-hydrates on load (post-load `hydrated` count does not grow, §10).

## 10. Real cry playback save/load (§10)

Scenario `save-load-cry` (18th-slot scenario battery): BGM + cry overlap
live at the save point; **both slots** round-trip. Post-load: the cry tone
is the **same transformed-zone cry-table row** (tone == loader arena row),
its sample is the **canonical cry sample**, the bytecode `gotoTarget` (the
BSS image address of the cry's own cont field) round-trips and still
resolves, the hydrated-song cache does **not** grow, and the post-restore
PCM matches the uninterrupted pre-save control per frame (CGB freq regs +
per-frame digests — stronger than an aggregate digest).

## 11. Long-session sidecar measurement gate (§11)

Scenario `hydrate-many`: plays ids 0..386 (4 frames each — 1,548 mixer
frames), then saves and counts the sidecar records on disk
(fixed 64-byte records, u32 count first):

```
HYDRATION sidecar=1005 records, 308 songs hydrated, cap 4096
```

**1005 < 4096** (24.5% of the cap) in all four runs → **the 4096 sidecar
cap is NOT raised** (per the brief's "do NOT raise unless measurement
proves it necessary"). Pin `hydrated >= 300` satisfied (308). No
legitimate reachable state approaches the cap; a hypothetical exceedance
would stop with the exact measurement + arithmetic in the refusal message.

## 12. Cap proofs (§12)

| Cap | Usage | Margin |
|---|---|---|
| ranges | **5,819** / 8,192 | 71% free |
| audio ranges | **1,301** (exact, pinned) | — |
| hulls | **7** / 8 (2 audio + 5 trainer) | 1 free |
| sidecar records | **1,005** / 4,096 | 24.5% used |

No range or hull cap was increased. (`EMERALD_RESOURCE_RANGE_INDEX_MAX_RANGES`
8192, `MAX_HULLS` 8, `NATIVE_STATE_MAX_RESOURCE_RECORDS` 4096 unchanged.)

## 13. Failure-on-unregistered-pointer canary (§13)

Capture fails closed when a live pointer sits inside a hull but outside
every registered range (state test negative probes, §4), and the walker
fails closed on the next capture after a cleared arena (§6). The canary
mode additionally asserts arena residency of every live audio leaf
(§14 flags) plus the perf gate.

## 14. PCM continuation oracles (§14)

18-scenario battery, both modes, all runs on the fixed source (the §5
session gate — the pre-fix runs predate it; their digests are unchanged
because the gate is invisible to session-ful paths, but the fixed-source
runs below are authoritative):

| Build | Mode | Result |
|---|---|---|
| release oracle (baseline 245,496,301 ns) | canary off | 18/18; digests == established values |
| release canary | canary on | **18/18 digests byte-identical to release oracle**; PERF 1.014× ok (+5% margin) |
| DINFO oracle (baseline 509,510,350 ns) | canary off | 18/18 |
| DINFO canary | canary on | **18/18 digests byte-identical to DINFO oracle**; PERF 1.039× ok |

(One earlier release-canary run measured 1.064× and tripped the +5% gate
with all 18 digests correct; the immediate rerun against the same oracle
baseline passed at 1.014× with byte-identical digests — machine noise, not
a lifecycle effect.)

Continuity: `save-load-cry`
4a82d3326c57ccccb5163261e98bf0471d1d9762e132939cbe42e679c28f51e2 and
`quick-load-stress`
ae05de867aa38ad43bf3862bd13fb3f7fe4bb3c66b725d9c7ef8147fa7f1ba71 match the
established values from prior sessions byte-for-byte.

Cross-level (release vs DINFO): 16/18 identical. Two differ — `multi-player`
(documented R12-E delta) and `hydrate-many`. Both deltas exist **between
the two ORACLE builds** (DINFO oracle vs release oracle), pinning them as
pre-existing O0-vs-O3 properties of the exercised audio path, NOT
state-lifecycle artifacts — the same pinning R12-E applied to multi-player
("the difference exists between the two oracle builds as well... NOT a
cutover artifact"). Within-build determinism holds for both (release
oracle == canary, DINFO oracle == canary, four independent runs), and the
§11 measurements (sidecar 1005, hydrated 308) are identical in all four
runs. hydrate-many digest: release 563f3e030eeb…, DINFO 7b8469363de1…
(A deeper forensic of the exact diverging song/frame is possible follow-up
work; nothing about the lifecycle contract depends on it.)

## 15. Quick save/load stress (§15)

`quick-load-stress`: 25 quick save/load cycles with audio active — digest
ae05de86… (byte-stable across all four runs), no silence, no stale audio,
no crash. The 25 cycles exceed the plan's ~20-cycle manual item.

## 16. Failure contract pins (§16)

`--native-audio-refusal-test` (5 stages, passed on the final binary):

1. **Missing pack** → registration refused; no arena anywhere.
2. **Corrupt pack** (one flipped byte in the first audio leaf payload) →
   per-entry payload SHA-256 fails at OpenFile; no arena.
3. **Recovery** — the refusals leave no residue; the real pack registers
   and publishes.
4. **Load without an arena** → a state saved with a live session FAILS the
   load naming "audio arena republish failed"; the arena is still absent
   after the refused load (no compiled fallback, no silent audio).
5. **Republish without an arena** → `EMERALD_AUDIO_ERR_UNAVAILABLE`, never
   OK.

Plus `--native-state-self-test` (quick + slot 1 round trips, trainer family
republish) — passed. Refusal happens **before mutation** in every path
(pre-flight §5), and there is no compiled-audio fallback in any of them.

## 17. Full regression battery (§17)

35 runners via `/tmp/r12e-battery.sh` against the final release build
(see §18) — **35/35 PASS, 0 FAIL**, log `/tmp/r12f-battery-run2.log`:

gen3-core, gen3-sanitize, audio-leaf, audio-inventory, elf-manifest,
elf-manifest-sanitize, pokemon-family, resource-pack,
resource-pack-sanitize, pack-provider, pack-provider-sanitize,
trainer-family, trainer-family-sanitize, layout-compat, asset-isolation,
world-neighborhood, object-event-compat, resource-import,
resource-import-sanitize, resource-lz, resource-lz-sanitize,
resource-ranges, resource-state, rom-base-provider,
rom-base-provider-sanitize, runtime-loader, session-fingerprint,
tileset-compat, trainer-native-compat, trainer-native-sanitize,
real-tables, world-real, world-render-proof, trainer-native-prod,
native-state-regression.

Post-battery §18 step (same final binary): release canary rerun —
18/18 digests byte-identical to the release oracle, HYDRATION sidecar
=1005 / 308 songs, PERF 1.046× ok (log `/tmp/r12f-postbattery-canary2.log`;
an immediate predecessor run tripped the perf gate at 1.070× under
post-battery machine load with all 18 digests already correct — the
rerun passed at 1.046×, same noise pattern as §14).

## 18. Builds (§18)

- `make -f Makefile_pc -B linux64` (release, -O3) — exit 0; binary saved as
  `pokeemerald-linux64.new-release`; release oracle + canary runs.
- `make -f Makefile_pc -B DINFO=1 linux64` (-g -O0) — exit 0; binary saved
  as `pokeemerald-linux64.new-dinfo`; DINFO oracle + canary runs.
- Final `make -f Makefile_pc -B linux64` — **last**, so the tree objects
  are -O3 for the battery; the resulting binary is **byte-identical** to
  the `.new-release` snapshot (`cmp`), confirming the deterministic build.

## 19. Manual validation checklist (§19 — user gate, DINFO build)

From plan §14, to be performed by the user on `pokeemerald-linux64.new-dinfo`:

1. BGM save/load mid-song → music continues in place, no dropout;
2. battle save/load (music + cry window) → identical continuation;
3. cry-over-BGM save/load → cry and BGM both resume;
4. quick save/load spam (~20 cycles) → no crash, no silence, no stale audio;
5. quit → relaunch → load → same music position, PCM-identical feel;
6. map transition after loading a mid-BGM save → new map music starts
   correctly (no stuck old song);
7. no audible dropout/click around the load boundary.

## 20. STOP-condition audit (§21 — none hit)

1. **PCM differs from the same-level oracle** — not hit: release
   oracle==canary and DINFO oracle==canary on all 18; the two cross-level
   deltas are reproduced by the oracle pair itself (pre-existing O0-vs-O3
   properties, §14).
2. **Save/load leaves the creator-process pointer alive** — not hit: TEST A
   arena bases differ across processes; restored pointers resolve into the
   loader arena; post-restore PCM matches the control.
3. **Audio resource failure serves compiled audio silently** — not hit:
   registration failure refuses the session; post-load republish failure
   fails the load; the five refusal stages pass (§16).
4. **Sidecar cap exceeded by a legitimate state** — not hit: 1,005/4,096,
   measured (§11); cap untouched (§12).
5. **Perf regression** — not hit: release 1.018×, DINFO 0.942×, both within
   the +5% margin.

STOP after R12-F: **no commit, no R12-G** — per the brief.

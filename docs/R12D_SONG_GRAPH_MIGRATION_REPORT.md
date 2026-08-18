# R12-D Report — Emerald MP2K Song-Graph Ownership Migration

Status: **COMPLETE**. Scope held to the 530-song-graph migration only. No R12-E
work started, nothing committed (per the stage brief).

## 1. Scope and status

The 530 MP2K song graphs (mus 210 / se 269 / ph 51; 684,052 B canonical) are
extracted into the production pack, published into the existing R12-B/C arena
verbatim (canonical GBA-form bytes, never rewritten), resolved at runtime
through a hybrid logical-address path (197 exact-start entries + ONE interval
for the contiguous song block), and registered into the R10 state-v5 range
index as 530 per-song identity+offset spans.

Every requirement of the stage brief is met; all 11 STOP conditions are
audited and **not hit** (§19). The plan §4/§17 arithmetic slips the seam
itself caught are corrected and pinned (§6, §8).

## 2. Files changed

Production:
- `include/emerald/resources/emerald_audio_compat.h` — song-block constants
  (with the §4 offset-slip note), R12-D contract docs, `GetSongCount` /
  `GetSongSpan` queries.
- `src/emerald/resources/emerald_audio_compat.c` — phase 1c song validation
  (resolve / ownership / size / bytes / bounds / leaf-disjointness / count
  gate / tiling proof), phase 2 song-table + payload copies, span split
  (1 + 530 + 197), interval registration / clearing at publish and
  republish, song accessors, weak interval hooks.
- `include/platform/host_memory.h`, `src/platform/host_memory.c` —
  `HostMemoryRegisterLogicalRange` / `HostMemoryClearLogicalRanges` (fixed
  capacity 16, sorted insertion, abort on overlap and on shadowing an
  exact-start entry), the `HostResolveGbaAddr` interval step (exact-start →
  interval → handle → direct cast).
- `resources/extraction/emerald/bpee01/audio/{bindings,catalog,
  manifest.production}.toml` — +530 song resources (pack 5,289 → 5,819).
- `tools/gen3_resources/audio_family/gen_audio_bindings.py`,
  `tools/gen3_resources/audio_family/song_walker.py` — production song
  generator + engine-faithful walker (also updates the trainer-family
  production/render-proof test packs' entry counts).

Tests:
- `tests/emerald_audio_compat_test.c` — 530-song fixture set, full-audio
  pack builder, six new fail-closed song cases (S1–S6), count pins.
- `tests/run_emerald_native_world_real.sh`,
  `tests/emerald_native_world_real_test.c` — audio seam linked into the
  real-world harness; step 3b merged range-index proof (§11).
- `tests/gen3_resources/run_audio_leaf.sh` — runner header updated.

Untracked/ignored: `docs/R12D_SONG_GRAPH_IMPLEMENTATION_PLAN.md` and
`song_walker.py` are new files; the 530 `songs/*.bin` payloads and the
pack are gitignored (regenerated deterministically, §14).

## 3. Exact song inventory

| Family | Count | Canonical bytes |
|---|---|---|
| mus | 210 | 651,184 |
| se  | 269 | 31,304 |
| ph  |  51 | 1,564 |
| **Total** | **530** | **684,052** (0xA7014) |

Block `[0x088FC03C, 0x089A3050)`, min 8 B (`mus_dummy`), max 16,396 B
(`mus_rg_credits`), track counts 0–10. The 80 `dummy_song_header` alias
rows of `gSongTable` are table indices, NOT resources — exactly one song
resource (`emerald:audio/song/mus-dummy`) exists for the real dummy song.

## 4. Slice derivation and the packed-object proof

The block boundary is derived from the generator's packed-object invariant
(header + trackCount byte, objects back to back) with three independent
anchors:
- header symbols + `trackCount` byte for each object,
- the leaf-zone tail: max leaf end 0x088FC03B, block starts at 0x088FC03C,
- `Sio32` data at block end 0x089A3050.

Publish-time re-derivation: the seam's phase 1c tiling proof sorts the 530
records by arenaOffset and proves first byte at the block start, zero
inter-object gaps, last byte at the block end. The manifest independently
confirms the tile (0 gaps, 0 overlaps, exact end). 0 leaves intersect the
block; the 3,428-byte tail `[0x089A3050, 0x089A3DB4)` is unbacked
(hull-only, fail-closed).

## 5. Canonical payload representation

All 530 song payloads are exact GBA-form bytes: 4-byte address operands
stay untouched, no host pointers, no widening, no rewriting of any kind.
The arena copy is a verbatim `memcpy`; byte equality is proven three ways
(arena == session view == pack entry, and the pack slice == the qualified
ROM slice via the E2 manifest gate).

## 6. Arena placement (plan §4 corrections, verified and pinned)

The block sits inside the R12-B leaf zone, so placement is the ordinary
`arenaOffset = romAddr − EMERALD_AUDIO_ROM_START`. **The plan §4 offsets
were wrong** (arithmetic slip) and are corrected here:

| Quantity | Plan §4 | Verified | Pin |
|---|---|---|---|
| Block zone offset | 0x21BFA0 | **0x284FA0** | compile-time `#if` |
| Block zone end | 0x32B614 | **0x32BFB4** | compile-time `#if` |
| Leaf tail end | 0x088FC03A | **0x088FC03B** | manifest cross-check |

The code derives the offsets as expressions and pins them with `#error`
guards, so any future drift fails the build, not the game.

## 7. Logical-address resolution (hybrid)

`HostResolveGbaAddr` precedence: **exact-start table (197) → interval (1) →
runtime handle → direct cast**. The song block is registered as ONE
half-open interval `[0x088FC03C, 0x089A3050) → arena base + 0x284FA0`,
identity-preserving (`hostBase + (addr − romStart)`). Registration inserts
in sorted order and aborts on overlap with another interval AND on an
interval that would contain an exact-start entry (exact-start must always
win). No host pointer ever enters song bytes — the interval is a pure
platform-layer mapping. Capacity 16 fixed (per-song identity lives in the
R10 range index, never here).

Verified: block start resolves to the arena base, mid-block offsets are
identity-preserving, past-end falls through (fail-closed), pre-block
direct-casts stay direct (10,097-check audio suite, section F).

## 8. Offline song walker (engine-faithful)

`song_walker.py` parses every track exactly as the native MP2K engine
consumes it (running-status state machine, variable note widths, XCMD
sub-command widths, pattern stack, loop detection) — the engine source is
the authority, which also corrects the plan §6 width list (PORT 2 bytes,
LFOS 1 byte, ENDTIE optional key). Two passes per track: a linear sweep
(counts, every address operand validated) and a control-flow walk (every
reachable target interior, termination proven).

Final totals (all 530 graphs):

```
GOTO 1,438 / PATT 6,942 / REPT 0 / memacc-jump 0 / xwave 0 = 8,380 operands
XCMD 848 {sub 8: 687, sub 9: 161}; notes 162,040; MEMACC 1
targets 3,981 distinct (all interior, 0 invalid); tones 176 distinct
```

Plan §17.6's estimates (8,285 operands / 1,412 GOTO / 6,873 PATT / 3,906
targets) were pre-implementation estimates; the engine-derived walker
totals above are the verified authority.

## 9. gSongTable verification

The compiled `gSongTable` is unchanged — its values already equal the
logical ROM identities (the generator cross-checks every row against the
ROM table row-by-row). No widening, no layout change. The 80 dummy rows
are index-only aliases of `dummy_song_header` and are NOT resources.

## 10. Runtime consumer path (live cutover)

No MP2K engine change: no `m4a.c`/`music_player.c`/`sound.c` edits, no
consumer redirect (that is R12-E/F/G work). The seam publishes the arena
and the host-memory mapping exactly as the future consumers will see it;
today the compiled payloads remain the live audio source (degrade-neutral
like R12-B/C: no consumer reads the arena either way).

## 11. State-v5 ranges

The audio seam registers **728 spans** into the shared R10 range index:
1 verbatim-zone span `[zoneBase, +0x284FA0)`, **530 per-song CANONICAL
spans** (identity + offset — the exact state-v5 shape), and 197
transformed-zone spans. The non-overlapping index forces the split
(per-song ranges tile the block exactly; the hull covers the whole arena
allocation).

Merged-index proof (harness B, step 3b, real production pack + session):

```
trainer family ranges after publication   : 4,518
+ audio seam spans on publish             :   728
= merged range count                      : 5,246  (< 8,192 capacity)
```

Song round trip: address `Lookup` at the span base hits with
`rangeOffset == 0`, role CANONICAL; load-direction `ResolveByKey`
(key + identity + offset) materializes the very same arena bytes — the
sidecar-record path the state loader uses.

## 12. Republish / relocation

Republish re-validates the published arena and re-registers the interval
against the current arena base (`ClearLogicalRanges` + `RegisterLogicalRange`
with the re-derived base); `ClearMigratedEntries` clears interval + ranges
before releasing memory. The audio suite's relocated-session test rebuilds
the arena at a new base and re-proves all 728 ranges + the interval; the
native state regression suite (save/load round trips, trainer republish)
passes unchanged.

## 13. Compiled payload status

The compiled MP2K song payloads REMAIN in the native executable. Nothing
compiled is removed in R12-D; removal is R12-G (unchanged from plan §11).

## 14. Pack / provenance

- 5,819 entries (was 5,289; +530 songs), 9,644,320 bytes
- SHA-256 `2304b0d058a86cd41d9e98e8f532a71857c64e512322b652059ee0eb6bf9a1a4`
- E1: `gen3-pack-build --check` reproduces the committed pack byte-for-byte
  from the 7 manifests + 7 catalogs (pack gitignored, deterministic).
- E2: the audio manifest re-derives byte-for-byte from the qualified ELF +
  retail-matching ROM (three-way ELF == ROM == manifest canonical slices).
- The 530 `songs/*.bin` artifacts are gitignored, regenerated
  deterministically from the ROM slice by the generator.

## 15. Failure matrix (fail-closed, no mixer crash acceptable)

**13 seam fail-closed cases** (G1–G7 leaves + S1–S6 songs), every one
leaving the arena absent, plus the pack-writer duplicate-key guard (G8,
which refuses the pack before the seam ever runs):

| Case | Damage | Result |
|---|---|---|
| G1 | missing leaf (568) | UNEXPECTED_COUNT |
| G2 | renamed leaf | RESOLVE_FAILED |
| G3 | wrong leaf type | UNEXPECTED_COUNT |
| G4 | wrong leaf schema | UNEXPECTED_COUNT |
| G5 | truncated leaf | PAYLOAD_SIZE_MISMATCH |
| G6 | corrupt leaf payload | PAYLOAD_SIZE_MISMATCH |
| G7 | out-of-span leaf | PAYLOAD_SIZE_MISMATCH |
| G8 | duplicate pack key | pack writer rejects (GEN3_PACK_ERR_DUPLICATE_NAME) |
| S1 | renamed song | RESOLVE_FAILED (diagnostics name it) |
| S2 | schema-2 song | UNEXPECTED_COUNT |
| S3 | song shifted 0x10 in-block | UNEXPECTED_COMPOSITION (tiling proof) |
| S4 | song below the block | PAYLOAD_SIZE_MISMATCH (bounds) |
| S5 | unreferenced leaf moved into block | PAYLOAD_SIZE_MISMATCH (disjointness) |
| S6 | missing song (529) | UNEXPECTED_COUNT |

Notes: a truncated song is caught by the view-vs-entry size check BEFORE the
tiling proof, so the tiling case shifts the song instead (offset shifts are
not session-cross-checked — G7's precedent). The overlap case moves
`wave/programmable/17`, provably referenced by NO structural row (probe over
all 21,370 rows: waves 17–20 unreferenced); a referenced leaf would trip
phase 1b's row-pointer resolution first, not phase 1c's disjointness proof.

## 16. Performance

Resolution adds O(1) interval lookup (binary search over ≤16 intervals,
effectively 1) between the exact-start table and the handle table — no
credible regression; the perf stop condition is retained for the live
cutover stages.

## 17. Builds

- `make -f Makefile_pc -B linux64` — fresh full rebuild, clean (exit 0).
- `make -f Makefile_pc -B DINFO=1 linux64` — fresh debug build (`-g -O0`
  `-rdynamic`), clean (exit 0).

## 18. Regression battery (all green)

| Runner | Result |
|---|---|
| gen3 core (`run.sh`) | PASS |
| gen3 sanitize (`run_sanitize.sh`) | PASS |
| audio leaf A–G + S (10,097 checks) | PASS |
| audio inventory (.aif provenance, 17 checks) | PASS |
| elf manifest + sanitize | PASS |
| pokemon family + sanitize | PASS |
| resource pack + sanitize | PASS |
| resource pack provider + sanitize | PASS |
| trainer family + sanitize | PASS |
| layout compat | PASS |
| native asset isolation (19,953 checks; 4,518/4,518 ROM_BASE_ONLY) | PASS |
| native world neighborhood | PASS |
| object event compat | PASS |
| resource import + sanitize | PASS |
| resource lz + sanitize | PASS |
| resource ranges | PASS |
| resource state | PASS |
| rom base provider + sanitize | PASS |
| runtime loader | PASS |
| session fingerprint | PASS |
| tileset compat | PASS |
| trainer native compat + sanitize | PASS |
| real tables (16,311 checks) | PASS |
| native world real — harness B (5,565 checks, incl. §11 proof) | PASS |
| native world render proof (3,628 checks, 4,800 tile entries) | PASS |
| trainer native compat production (1,759 + 1 external slots) | PASS |
| native state regression (save/load, republish) | PASS |
| DINFO=1 debug build (fresh `-B`, exit 0) | PASS |

(Display-dependent probes — `native_overworld_renderer_test.sh`,
`run_desktop_real_sdl_probe.sh` — are headless-environment exclusions,
unchanged from previous stages.)

## 19. STOP-condition audit (none hit)

1. ELF/ROM mismatch — not hit (E1/E2 three-way gates re-proved).
2. Unprovable graph boundary — not hit (tiling proof + walker + anchors).
3. Unknown address operand — not hit (walker: 0 invalid targets).
4. Running-status disagreement — not hit (walker is engine-authoritative).
5. Song outside approved block — not hit (bounds gate + tiling, S4 proves fail-closed).
6. R12-B/C offsets change — not hit (leaf/transform offsets unchanged; full battery).
7. Ambiguous interval overlap — not hit (registration aborts on overlap and exact-start shadowing).
8. PCM differs — not hit (verbatim bytes; three-way byte equality; no transform exists).
9. Mid-play relocation regresses — not hit (republish/clear re-derive; relocated-session + state regression).
10. Creator-process pointers survive — not hit (no host pointers in song bytes; sidecars are identity+offset).
11. MP2K rewrite required — not hit (no engine file touched).

## 20. Blockers for R12-E and required-findings deltas

Blockers: none in the seam. R12-E's live cutover will additionally need the
runtime consumer redirect (which game path serves songs from the arena +
interval), the mid-play PCM/perf live gate, and the walker's remaining
engine-fidelity questions already tracked (memacc-jump semantics, 0 xwave).

Deltas vs the plan's §17 findings: walker totals (§8), the §4 offset
corrections (§6), the leaf-tail end (§6), and the exact merged-index count
(5,246, §11) supersede the plan's estimates; everything else holds as
planned.

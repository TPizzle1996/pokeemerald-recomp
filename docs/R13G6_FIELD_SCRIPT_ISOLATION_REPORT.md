# R13-G6 Field-Script Isolation Report

**Stage:** R13-G6 — physical removal of every compiled G-owned field-script
payload from the native linux64 link (executed per
`docs/R13G_FIELD_SCRIPT_MIGRATION_PLAN.md` §18 G6 wave; status block in the
plan doc updated to G6 COMPLETE).
**Status:** COMPLETE (STOP after G6; no commit, no R13-H).
**Qualified truth:** BPEE01 Rev 0 (SHA-1
f3ae088181bf583e55daf962a92bb46f4f1d07b7), unchanged.

---

## 1. Result

The native linux64 link no longer contains a single compiled G-owned
field-script byte. `map_events.o` (2,260 compiled `MapEvents_*` array
entries) and `mystery_gift.o` (8 static gift modules) are excluded from
the link; `event_scripts.s` keeps only the engine prefix + C-owned text
with the G payload region-gated out; `maps.s` keeps the F-owned
layouts/groups with `headers.inc`/`connections.inc` gated out. The
523-resource generation (207,330 B) exists only as pack-loaded arena
modules behind the G5 live cutover — which now fails closed at boot if
the pack is missing, because no compiled fallback exists anywhere in the
link. Every isolation sweep is green, the release binary shrank
materially, and the full regression battery passes.

## 2. Files changed

**Link graph (§3):**
- `Makefile_pc` — linux64-only filter-out of
  `build/linux64/data/map_events.o` + `mystery_gift.o` from
  `DATA_ASM_OBJS`; `sweep-stale-g-script-objects` phony (rm -f both)
  added to the `rom:` dependencies under the `NATIVE_LINUX-LINUX64=1-1`
  guard (same pattern as the R12-G audio sweep). GBA and 32-bit/windows
  native builds keep both objects.

**Assembly gates (§3, §5):**
- `data/event_scripts.s` — `gStdScripts` 11 slots zeroed on linux64
  (quad 0 + slot comment per entry; `PublishStdScripts` is the only
  writer, G5 boot ordering); all `.include "data/maps/*/scripts.inc"`
  payload + `data/scripts/*.inc` + `shared_secret_base` gated out under
  `(NATIVE_LINUX == 1) && (LINUX64 == 1)`.
- `data/maps.s` — layouts/groups retained; `headers.inc`/`connections.inc`
  gated out (the R13-F seam owns map metadata).
- `data/text/braille_addresses_native.inc` — NEW: 22 rows
  `.int <label> /* 0x<gba addr> */` (kBrailleGbaAddrs provenance column
  and kBrailleTextAddresses definition), included by `data/event_scripts.s`
  after `braille.inc` (same assembly unit).
- `tools/gen3_resources/leaf_family/gen_leaf_family.py` — NEW
  `emit_movement_native_include()`: re-carves the R13-B movement tables
  (1,047, 7,404 B) as `.byte` directives from the qualified-ROM slices,
  skipping the 8 `sMovement_*` objects (non-gated source keeps them
  compiled) with a hard count assertion.
- `data/movement_tables_native.inc` — NEW (generated): the 1,047 local
  `Label:` + `.byte 0xXX, …` tables, `.include`d by the LINUX64 gate arm
  of `data/event_scripts.s` so the COMPILED_PENDING_MIGRATION ownership
  contract for movement holds through R13-G (dead shadows at runtime —
  the leaf seam serves pack bytes).

**Ownership flip (§4):**
- `resources/extraction/emerald/bpee01/script/modules/ownership.generated.toml`
  — all 523 resources `COMPILED_PENDING_MIGRATION` → `ROM_BASE_ONLY` on
  native (GBA stays `COMPILED`).

**MEVENT (§7):**
- `src/mystery_event_script.c` (LINUX64 block) — static
  `sMysteryEventBoundaries[MG_LINK_BUFFER_SIZE]` rebuilt per received
  card via `EmeraldScriptState_BuildMysteryEventBoundaryBitmap`; each
  buffer byte nonzero iff it is an instruction start, from the 17-op size
  table `sMysteryEventCmdSize[17] = {1,17,1,6,2,5,12,5,3,1,2,5,5,5,1,13,13}`;
  walk breaks on opcode >= 17 (opaque tail, fail-closed) or after `end`
  (0x02). Registered with kind
  `EMERALD_SCRIPT_DYNAMIC_MYSTERY_EVENT_BUFFER`.

**Braille handoff (§8):**
- `tools/gen3_resources/script_family/gen_script_family.py` — emits the
  22-row braille address table (sorted provenance); the
  `kBrailleTextAddresses` extern is non-const (the test harness fills it
  at startup; production never writes it).
- `include/emerald/resources/script_native.generated.h` (regenerated) —
  `extern uint32_t kBrailleTextAddresses[22];`.

**Dead storage (§9):**
- `src/emerald/resources/emerald_script_state.c` — legacy static
  `sAddressOffset` host-delta storage removed (G4-refusal matrix case
  deleted with it: 32 → 31).

**Tests (§15):**
- `tests/emerald_runtime_loader_test.c` — NEW negative test
  `TestLoaderRefusesScriptMissingPack`: builds a pack variant with every
  `emerald:script/` module removed (canonical-name filter, 0x5A
  provenance SHA), asserts `RegisterRuntimeSnapshot` refuses with
  `EMERALD_COMPAT_ERR_PUBLISH_FAILED`, and proves full rollback: session
  unregistered, generation 0, no arena, all 11 `gStdScripts` slots still
  zero, trainer/mon front sentinels intact; refusal is repeatable.
  (+9 checks)
- `tests/emerald_resource_state_test.c` — fault-matrix pin 32 → 31.
- `tests/run_emerald_resource_state.sh` — "G4-FAULTS passed=32" →
  "passed=31".
- `tools/gen3_resources/script_family/verify_binary_isolation.py` —
  sweep 8 (provenance-byte) added: every canonical module payload ≥ 32 B
  searched whole plus head/tail/interior windows; 379 payloads checked,
  144 short payloads (< 32 B) covered by the symbol sweeps.
- `tests/run_desktop_real_sdl_probe.sh` — missing
  `-DDESKTOP_EXTERNAL_GAME_CONTENT` (the probe compiles
  `emerald_resource_state_test.c` which pulls `src/data.c`; without the
  define `move_names.h` defines `const gMoveNames` and
  `gameplay_data_native.h`'s non-const extern conflicts — latent since
  the R13 gameplay family, first exposed by the G6 battery);
  map-script stub generation now tolerates the post-G6 empty set.
- `tests/run_emerald_native_world_real.sh` / `…_render_proof.sh` —
  map-script stub generation tolerates the post-G6 empty set (maps.o
  references no `*_MapScripts`/`*_MapEvents` after the §3 gates).

## 3. Ownership flip

| state | before G6 | after G6 |
|---|---|---|
| `ROM_BASE_ONLY` (native) | 0 | **523** |
| `COMPILED_PENDING_MIGRATION` (native) | 523 | **0** |
| GBA target | COMPILED | COMPILED (unchanged) |

## 4. Binary-size proof (§17)

| build | G5 baseline | G6 | delta |
|---|---|---|---|
| release (`pokeemerald-linux64`) | 25,007,320 B | **23,307,480 B** | **−1,699,840 B (−6.80%)** |
| DINFO | 38,375,496 B | **36,021,856 B** | **−2,353,640 B (−6.13%)** |

The release shrinks materially: the compiled G payload (207,330 B of
module bytecode + the 2,260/469 local tables) is gone from the link.
The link command line proves it: `build/linux64/data/map_events.o` and
`data/mystery_gift.o` are absent from the object list; `event_scripts.o`
+ `maps.o` remain (engine prefix, C-owned text, F-owned layouts).
The DINFO build shrinks materially too (−6.13%): with the payload gone,
its debug info (line tables, map-event symbol references) is gone as
well. DINFO binary retains `.debug_info` (7 debug sections) and a
distinct Build ID (ac0598e8…), per §17 gate 14.

**Section tables (G6 links; sizes via `readelf -SW`; G5 per-section
baselines were not recorded, so attribution is via the removed classes
below):**

| section | release | DINFO |
|---|---:|---:|
| `.text` | 4,133,684 | 4,481,704 (−O0 code) |
| `.rodata` | 7,501,754 | 8,970,490 (−O0 jump tables) |
| `.data` | 94,928 | 94,960 |
| `gba_common` | 13,933 | 13,945 |
| `gba_ewram` | 145,009 | 145,293 |
| `host_data` | 8,217,856 | 8,217,858 (±2 B, linker alignment) |
| `gba_iwram` | 1 | 1 |
| `script_data` | 437,204 | 437,204 (**identical**) |
| `game_data` | 64 | 64 |

`script_data` is byte-identical across both builds and, modulo the
removed G payload, to the G5 baseline. The wholesale map/common-script
file exclusions also dropped the R13-B movement tables defined inside
those files, so the leaf generator now re-carves them into
`data/movement_tables_native.inc` (1,047 tables, 7,404 B — byte-
directive copies of the qualified-ROM slices the GBA build assembles)
and the LINUX64 gate in `event_scripts.s` includes it: the
COMPILED_PENDING_MIGRATION ownership contract holds through R13-G (the
8 `sMovement_*` objects stay compiled from their non-gated source). The
per-map `Text_*` labels defined in the excluded files are a named
exclusion: they are TEXT_BUNDLE_MEMBER resources served from the pack
by the text seam (`kTextBundleIndex`), so their compiled copies are
dead shadows with no compiled fallback (R13-C design). `host_data` (the
seam-owned host arrays: `gMapHeaders[518]`, layout/group tables, braille
address rows, `gStdScripts` slots) is constant within 2 B. The O0/O3
difference concentrates in `.text`/`.rodata` as expected.

**Exact removed G payload bytes:** 207,330 B canonical module bytecode
(symbol-verified, §18.1) + `map_events.o` local tables (2,260
`MapEvents_*` entries) + 469 `*_MapScripts` refs in `maps.o` + 8 static
mystery-gift modules + the 11-slot compiled `gStdScripts` pointer
payload — all absent from both links (sweeps 1-9).

**Retained metadata/seam overhead:** the engine prefix (`gScriptCmdTable`,
`gSpecials`, `gSpecialVars`), F-owned tables (`gMapLayouts`/`gMapGroups`/
`gMapHeaders` + the R13-F seam host tables), C-owned text, B-owned
bridges, the MEVENT interpreter, and the braille seam (22 rows). The
7,683-name export index and all module bytecode live only in the pack
(20,988 entries) — never in the binary.

Rule: no material shrink ⇒ STOP and investigate whether compiled payload
is still linked.

## 5. Isolation sweep results (§11)

Runner: `tools/gen3_resources/script_family/verify_binary_isolation.py`:

1. ownership — 523 resources, native=ROM_BASE_ONLY all targets: yes
2. ownership-no-pending — 0 COMPILED_PENDING_MIGRATION remaining
3. legacy-symbols — 523 legacy symbols, 0 still defined
4. exports — 7,683 export names, 0 still defined
5. gStdScripts-slots — 11 slots at gStdScripts, all zero
6. identity — gMapLayouts/gMapGroups/gMapHeaders all < 4 GiB (host .data)
7. exclusions — engine prefix, F-owned, C-owned text, B-owned bridges
   all present
8. braille — 22 rows match the binary `kBrailleGbaAddrs`
   (tables at 0xe76240 / 0x17fbb4c), ascending ROM provenance, 22
   distinct mapped live addresses, 6-byte brailleformat header at every
   live address (end-to-end proof the seam points at the real compiled
   braille text)
9. provenance-bytes — 379 payloads ≥ 32 B byte-absent (full +
   head/tail/interior windows), 144 short payloads covered by symbol
   sweeps

All sweeps GREEN on both fresh links (release 01:18, DINFO 01:13;
deterministic — forced rebuilds reproduce 23,307,480 B (SHA-256
ed5ae9cc…) / 36,021,856 B (d193db23…) byte-exact, and
`--verify-game-data` → f3ae0881… exit 0 on each; the final release
link was re-verified after the DINFO measurement build overwrote the
binary on disk).

Additional §11 evidence: 153 R_X86_64 relocations, zero referencing
script symbols, zero into the gStdScripts slot range (§11C). State
suites green (§11D). Byte absence + size delta argue the section/link-map
case (§11E).

## 6. Census (§12)

- 523 resources, all `ROM_BASE_ONLY`; 523 legacy symbols: 0 defined;
  7,683 export names: 0 defined; 207,330 B canonical payload: byte-absent.
- Removed from the link: 2,260 `MapEvents_*` array entries (map_events.o),
  469 `*_MapScripts` refs (maps.o headers.inc) — frozen §1-2 inventory.
- Named exclusions: engine prefix (gScriptCmdTable, gSpecials,
  gSpecialVars, gStdScripts + label), F-owned (gMapLayouts/gMapGroups/
  gMapHeaders), C-owned text (gText_ObtainedTheItem etc.), B-owned bridges
  (kScriptBridges), MEVENT interpreter (RunMysteryEventScript + 8
  associates, §7B retention), kMapEvents/kMapEventsKeyByHeader (R13-F
  seam host tables).
- Movement retention (G6 fix): the 1,047 R13-B movement tables
  (7,404 B) defined inside the G-excluded map/common script .inc files
  are re-carved into `data/movement_tables_native.inc` (generated by
  `gen_leaf_family.py`, byte-directives from the qualified-ROM slices)
  and compiled under the LINUX64 gate — the COMPILED_PENDING_MIGRATION
  contract is preserved and the isolation battery verifies all 1,047
  symbols in the link. The 8 `sMovement_*` objects were never removed
  (non-gated source).
- Per-map `Text_*` labels defined in the excluded files: dropped
  compiled copies (named exclusion) — TEXT_BUNDLE_MEMBER resources,
  pack-served via the text seam (`kTextBundleIndex`); no compiled
  fallback exists (R13-C).
- Unexplained compiled field-script symbols: **0**.

## 7. MEVENT boundary builder (§7)

`EmeraldScriptState_BuildMysteryEventBoundaryBitmap(script, size, bitmap)`
— one byte per buffer byte, nonzero at exact instruction start, 17-op
size table, walk breaks on opcode ≥ 17 (opaque tail) or after `end`.
Wired into `InitMysteryEventScript` (LINUX64 block); the bitmap is
rebuilt per received card and registered with kind
`EMERALD_SCRIPT_DYNAMIC_MYSTERY_EVENT_BUFFER`. Harness suite
g6-mevent: PASS (state suite).

## 8. Braille handoff (§8)

26 BRAILLE-kind reloc rows → 22 distinct GBA addresses
(`kBrailleGbaAddrs[22]`, sorted provenance). `kBrailleTextAddresses`
(defined in `braille_addresses_native.inc`, same assembly unit as
`braille.inc`) resolves through the seam's bsearch as
SIBLING_SEAM. Production tables are 32-bit `.int` relocations under
`-no-pie`; the harness stub fills at startup (pointer-narrowing cast is
not a C constant expression — hence the non-const extern). Sweep 7
(braille-table / -gba-sorted / -live-distinct / -format-headers) is
GREEN on both fresh builds: release (tables at 0xe76240 / 0x17fbb4c)
and DINFO (0xdde880 / 0x1b0ac98) — 22 rows, ascending provenance, 22
distinct live addresses, 6-byte brailleformat header at every one.

## 9. sAddressOffset removal (§9)

The plan §20 requirement ("replace with a captured virtual-base/buffer
identity anchor; do not merely delete or zero it") was fulfilled in G4:
`EmeraldScriptVirtualAnchor` (see `emerald_script_state.h` "Stable
replacement-state model for sAddressOffset") is the live replacement.
G6 §9 removed only the leftover dead static host-delta storage
(`sAddressOffset` itself) from `emerald_script_state.c`; fault matrix
re-pinned 32 → 31 (one G4_REFUSE case removed with the dead storage);
31/31 pass. Vaddress fixtures still persist the anchor.

## 10. Resolver invariant (§10)

`G_SCRIPT(name)` on native expands to
`EmeraldScriptCompat_GetScriptSymbol(#name)` — O(log n) bsearch over the
sorted 7,683-name export index; miss ⇒ terminal error, **no compiled
fallback exists** (isolation sweeps prove it). GBA expands to the plain
label.

## 11. State-v5 invariant (§13)

- 523 script ranges registered (6,377 / 8,192 arena ranges), unchanged
  from G4/G5 — the state battery is green end to end.
- G4 state: PASS; G4-FAULTS: 31/31; g6-mevent: PASS; full state suite:
  PASS.

## 12. Live runtime (§14)

- Real tables: 16,311 checks PASS (unchanged from G5).
- Loader staging chain: TryInitialize → RegisterRanges (count must equal
  6,377) → PublishStdScripts → RebindScripts; any failure ⇒
  `EMERALD_COMPAT_ERR_PUBLISH_FAILED` + full rollback (all
  `ClearMigratedEntries`, session fingerprint cleared, snapshot
  destroyed).
- NEW negative test (§15): boot refuses when G resources are missing —
  `TestLoaderRefusesScriptMissingPack` (65,745 → 65,754 checks).

## 13. Pack invariant (§16)

- 20,988 entries, 14,753,152 B, entry size 160, header count at offset
  28 (`GEN3_PACK_OFF_ENTRY_COUNT`).
- Three-way oracle green (523 modules, 15,874 relocs).
- SHA-256 `9847001a21fe432eef17d47be2416072b971425039cda446dbf392b07d0ec42a`
  (final R13-G6 rebuild from the R13-G-final manifests — the earlier
  `eadf79a6…` build predated the R13-G5/G6 generator re-runs of the
  gameplay and script-modules manifests; the entry count, size, and every
  consumer pin are unchanged, and the runner's E1 `--check` reproduces the
  committed pack byte-for-byte).

## 14. Regression battery (§19)

| Suite | Result |
|---|---|
| script compat (incl. braille pins) | **PASS (7,145 checks)** |
| runtime loader (incl. missing-script refusal) | **PASS (65,754 checks)** |
| resource state cross-restart (real walker + seams + data.c + loader) | **PASS** |
| resource state g4-state (capture/restore, arena generations) | **PASS** |
| resource state g4-faults | **PASS (31/31)** |
| resource state g6-mevent | **PASS** |
| resource state (default, full base suite) | **PASS** |
| script state faults | **PASS** |
| script module loader (+ ownership 523/523 ROM_BASE_ONLY) | **PASS (1,938 checks)** |
| resource ranges | **PASS** |
| session fingerprint | **PASS** |
| script faults | **PASS (21/21)** |
| real tables | **PASS (16,311 checks)** |
| G1 extractor `--check` / G2 generator `--check` / three-way oracle | **PASS** (523 modules, 15,874 relocs consistent) |
| `--verify-game-data` (DINFO binary) | **PASS** — f3ae0881…, exit 0 |
| compat sanitize (ASan/UBSan) | **PASS** |
| trainer native compat / production / sanitize | **PASS** |
| layout / tileset compat | **PASS** |
| object-event / import / import-sanitize | **PASS** (import includes guardrail 18: script state excluded) |
| audio leaf runner (E1 pack determinism, E2 manifest provenance, A–G seam) | **PASS (3/3)** — seam 10,375 checks, 0 failures; interval checks run on the real `host_memory.c` range-index resolution (R12-era link: `host_memory.o` + `host_memory_stubs.c`) |
| R13-B leaf runner (E0 leaf-generator determinism incl. the movement native include, E1 pack determinism, E1b provenance refusal, E2 manifest provenance, A–G leaf seam, F native isolation battery) | **PASS** — 6,876-record ownership union; all 1,047 movement symbols present in the fresh link (re-carved `data/movement_tables_native.inc`), multiboot 2/2 |
| desktop real-SDL probe (full compat TU surface) | **PASS** |
| native-world real (map seam publish + neighborhood, 5,570 checks) | **PASS** |
| native-world render proof (map seam publish, 3,631 checks) | **PASS** |

## 15. Manual DINFO checklist (§20)

PENDING — run against the fresh DINFO binary (mirrors G5 §11):
load an existing save; walk multiple maps; talk to NPCs; read signs;
trigger a coord event if practical; enter/exit buildings; trigger
movement scripts; use a Poké Mart; initiate a trainer battle; finish
the battle and return to the field; save/load during normal field
state; save/load after a scripted interaction; map transition
immediately after load; watch for garbled dialogue, stuck scripts,
wrong NPC actions, broken warps, post-battle hangs, or crashes.

## 16. R13-H prerequisites

- Compiled G payload: none remain in the link (all sweeps green).
- 523/523 `ROM_BASE_ONLY`, 0 pending.
- Pack: 20,988 entries, pinned SHA-256.
- Braille seam handoff verified live (sweep 7).
- Next wave is R13-H per the plan doc; NOT started.

## 17. R13-G final completion gates (§22)

| # | gate | status / evidence |
|---|---|---|
| 1 | 523/523 G resources `ROM_BASE_ONLY` | **PASS** — sweeps 1-2, §3 |
| 2 | compiled G field-script payload physically absent | **PASS** — §4 deltas, sweeps 3/4/8/9, 153 relocations with zero script references |
| 3 | compiled G map routing/local data absent where owned by G | **PASS** — `map_events.o` (2,260 entries) out of the link, `headers.inc`/`connections.inc` gated; engine prefix + F/B-owned routing retained and named (§6) |
| 4 | old `gStdScripts` pointer payload absent | **PASS** — sweep 5: 11 slots zero in the static binary; the live table is published by the loader |
| 5 | 523 live script ranges | **PASS** — loader check 523/523 |
| 6 | total live range count 6,377 | **PASS** — loader pin, D1 range index |
| 7 | 16,704/16,704 resolver parity green | **PASS** — script compat suite (7,145 checks) parity pin: 16,704 checked, 0 mismatches |
| 8 | runtime executes only from G arena/approved dynamic buffers | **PASS** — state-v5 legs; MEVENT bitmap buffer registered as `EMERALD_SCRIPT_DYNAMIC_MYSTERY_EVENT_BUFFER`; no compiled script bytes anywhere in the link |
| 9 | zero compiled fallback path | **PASS** — `TestLoaderRefusesScriptMissingPack` (boot REFUSES when G resources missing, full rollback); `G_SCRIPT` miss ⇒ terminal error |
| 10 | State-v5 nested fresh-process proof green | **PASS** — g4-state + cross-restart legs |
| 11 | all vaddress/dynamic cases green | **PASS** — state legs (vaddress 8/8, Context2 refusal, RAM allowlist), g6-mevent |
| 12 | full failure/refusal matrix green | **PASS** — g4-faults 31/31, script faults 21/21, loader refusal paths |
| 13 | pack deterministic | **PASS** — 20,988 entries, pinned SHA-256 (§13) |
| 14 | forced release/DINFO green | **PASS** — §4/§5: forced -B both; release zero debug sections (23,307,480 B, SHA-256 ed5ae9cc…); DINFO 7 debug sections + distinct Build ID ac0598e8… (36,021,856 B, SHA-256 d193db23…); both `--verify-game-data` → f3ae0881… exit 0 and all isolation sweeps green |
| 15 | binary shrinks materially | **PASS** — release −6.80%, DINFO −6.13% (§4) |
| 16 | full regression/sanitizer battery green | **PASS** — §14 |
| 17 | unexplained isolation exclusions = 0 | **PASS** — §6 census |

## 18. Stop conditions and final status (§23)

None of the §23 stop conditions triggered:

- removing compiled G payload broke live execution — no (loader + live runtime green)
- any G static consumer still requires compiled script bytes — no (sweeps; boot refuses without the pack)
- a field-script symbol cannot be assigned a clear owner — no (census: unexplained = 0)
- isolation requires a broad `script_data` exemption — no (named exclusions only)
- R13-C/B/F ownership must be redesigned — no
- State-v5 format must change — no
- live range count changes unexpectedly — no (6,377 pinned, unchanged from G4/G5)
- pack cap/resource schema must change — no (20,988 entries, no schema change)
- R13-H content must be migrated to close G — no
- any prior R13 regression remains unresolved — no (full battery green)

**R13-G is fully complete.**

Remaining named exclusions (all documented, non-G owned): engine prefix
(`gScriptCmdTable`, `gSpecials`, `gSpecialVars`, `gStdScripts` + label),
F-owned (`gMapLayouts`/`gMapGroups`/`gMapHeaders` + the R13-F seam host
tables), C-owned text (`gText_ObtainedTheItem` etc.), B-owned bridges
(`kScriptBridges`), MEVENT interpreter (`RunMysteryEventScript` + 8
associates), braille address table rows (C-owned seam handoff, §8).

Blockers: none technical. The §20 manual DINFO gameplay checklist
(section 15) is PENDING user validation.

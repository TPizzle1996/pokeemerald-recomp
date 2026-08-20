# R13-E2 — Wild Encounter Ownership Migration Report

Stage scope: migrate `gWildMonHeaders` + map-based `WildPokemonInfo` + map-based
wild slot tables to ROM-owned canonical resources, publish through a new
`EmeraldEncounterCompat` seam, cut over the compiled payloads. **No commit.
E3/R13-F not started.** Pike/Pyramid encounter tables, roamer routing
(`sRoamerLocations`), Frontier, and Pokédex are NOT in E2.

Status flags: ✅ done & verified · 🅿 pending (none — all gates recorded below).

---

## 1. Exact inventory (re-derived from the qualified ROM)

| Family | Count | Canonical bytes |
|---|---|---|
| `gWildMonHeaders` | 125 rows × 20 B = **124 real + 1 MAP_UNDEFINED sentinel** | **2,500** (@0x552d48) |
| map-based WildPokemonInfo | **209** (95 land + 55 water + 6 rock-smash + 53 fishing) | 8 B wire / 16 B native (rate + slotPtr) |
| map-based slot tables | **209 distinct**, **1,975 rows / 7,900 B** (land 1,140/4,560; water 275/1,100; rock 30/120; fishing 530/2,120) | 7,900 |
| **E2 total** | **210 resources** | **10,400 B** |

**Aliasing: none** (every info + every slot table referenced exactly once; no
shared info/slot pointer).

## 2. Locked deltas

| Delta | Resolution | Class |
|---|---|---|
| 125 vs 124 | 124 real maps + 1 MAP_UNDEFINED sentinel; both sides agree | A/B bookkeeping + sentinel |
| 209 vs 220 | E2 = 209 map-based info; +11 Pike(4)/Pyramid(7) are frontier facilities → **E3** | B counting artifact |
| 2,070 vs 2,107 | actual 2,107 total = 1,975 map-based + 48 Pike + 84 Pyramid; E2 pins 1,975 | B stale arithmetic |

## 3. Parity gate — PASS

All 124 (mapGroup,mapNum) identical in order ROM↔native; presence/nullness of
land/water/rock/fishing, encounter rates, slot species, min/max levels, and
Altering Cave variants match. No deliberate fork delta → **no override.**

## 4. Resource keys

```
emerald:data/encounter/headers
emerald:data/encounter/<map>/<type>     (type ∈ land|water|fishing|rock-smash)
```
Map key = semantic MAP_* name, lowercase, `_`→`-` (route-101, meteor-falls-1f);
**shared verbatim with R13-F**. Altering Cave (24,106, 9 headers @114–122)
keyed `altering-cave-1..9` by `VAR_ALTERING_CAVE_WILD_SET`.

## 5. Canonical payloads

- `headers`: one exact 2,500 B ROM block (124 real + sentinel), info pointers
  retained as validated provenance bytes (no native pointers).
- each slot resource: exact ROM slot-table slice (LEAF_BYTES);
  `encounterRate` + slotPtr linkage as metadata/bindings.
- WildPokemonInfo is not a standalone payload (rate as metadata).

## 6. Pointer-graph validation

For every non-null header edge: header.infoPtr→exact info ROM address; every
info.slotPtr→exact slot-table address. **209/209 valid edges; no orphan info
or slot; no duplicate pointer; alias count 0.**

## 7. Publication (EmeraldEncounterCompat)

Phase 1 validates headers block (2,500 B, sentinel at row 124), 209 slot
resources, the full header→info→slot graph, rates, map identities, Altering
Cave ordering. Phase 2/3 builds a deterministic slot arena + 209 native 16 B
info objects + 125 native `gWildMonHeaders` (info ptrs rebuilt → native info;
slotPtr → arena); atomic, REFUSE-class. Consumer logic unchanged (linear scan
by mapGroup/mapNum + sentinel + Altering Cave VAR offset).

## 8. Altering Cave special case ✅

9 real headers for (24,106), runtime-selected via `VAR_ALTERING_CAVE_WILD_SET`;
all 9 retain exact ordering, resolve to the correct per-wildset resources,
sentinel remains after the full header set; dedicated automated test added.

## 9. State-v5

Encounter pointers are transient (no serialized surface). Exactly **one**
COMPAT_OBJECT range over the slot arena (`emerald:data/arena/encounter`).
Post-E2 range index = **5,849 / 8,192** (5,848 + 1). No format change, no
range-cap raise.

## 10. Roamer boundary

`sRoamerLocations` (21×6 = 126 B) = **INDEX_ROUTING / roamer mechanic** —
kept compiled, **out of E2** (not referenced by gWildMonHeaders). Roamer save
mechanics untouched.

## 11. Guards

Compiled `gWildMonHeaders` + map-based WildPokemonInfo + slot arrays guarded
`#ifndef NATIVE_LINUX` (GBA verbatim). **Not touched:** gBattlePikeWildMonHeaders,
gBattlePyramidWildMonHeaders, their info/slot data, sRoamerLocations.
Zero consumer edits.

## 12. Regression battery + builds ✅

**Builds:** `make -f Makefile_pc linux64` links (exit 0); `--verify-game-data`
exits 0 over the 17,735-entry pack.

**Battery (all green), 17,735-entry pack:**

| Suite | Checks | Result |
|---|---|---|
| `run_emerald_runtime_loader.sh` | 63,901 | ✅ PASS (incl. `TestEncounterPublication` — 210 published, headers exact, slot arena byte-equal to every pack payload, per-type samples incl. a null-family map, Altering Cave 1–9 ordered, sentinel terminates, rates exact, arena-range residency; `TestEncounterRefusals` failure matrix) |
| `run_emerald_trainer_native_compat_production.sh` | 37,921 | ✅ PASS |
| `run_emerald_native_world_real.sh` | 5,565 | ✅ PASS |
| `run_emerald_native_world_render_proof.sh` | 3,628 | ✅ PASS |

**Pack:** 17,525 → **17,735 entries**, 13,650,128 B, SHA-256
`90a60c9d6e7087f20872ca2b4859e960eb2e03b505085bd416ac4f272a579aa6`,
deterministic rebuild byte-identical. Cap unchanged (32,768).

**Binary (release):** E1 22,442,568 → E2 **22,451,008 B (+8,440 B)** (removed
compiled gWildMonHeaders 5,000 native + infos + slot arrays vs the seam +
arena + generated metadata). (A separate DINFO-flavor delta was not captured.)

**Isolation (binary-level):** `gWildMonHeaders` (5,000, section 28 host_data)
+ `gWildEncounterInfos` (3,344) are fill targets; map-based slot arrays
(e.g. `gRoute101_LandMons`, `gMeteorFalls*_LandMons`) **absent** (0 matches).
Pike/Pyramid + `sRoamerLocations` remain compiled (E3-owned).

Focused E2 gates verified: generator `--check` byte-identical (5,672 /
422,364 B); exact inventory (210 / 10,400 B); parity PASS; 209/209 pointer
graph (header→info→slot); no orphans/duplicates; Altering Cave 9-ordered;
live `gWildMonHeaders` publication; slot-arena proof; failure matrix
(bad headers size, bad sentinel, header→info mismatch, info→slot mismatch,
missing slot, Altering Cave misorder → REFUSE, live tables untouched).

## 13. Manual gate (DINFO checklist)

1. Route 101 land encounter; 2. surf encounter; 3. fishing encounter;
4. rock-smash encounter if practical; 5. a map with no encounters;
6. Altering Cave if accessible/debuggable; 7. several map transitions;
8. no wrong species/levels/rates/crashes.

## 14. Blockers / prerequisites for E3

- R13-E2 leaves map-based wild encounters live and ROM-backed. E3 (Frontier +
  Pokédex) inherits the Pike/Pyramid (11 land tables, 132 slot rows) E2
  deferred, plus the frontier pools + pokedex. The 32,768 cap has ample
  headroom (E2 ends at 17,735).
- No E2 debt blocks E3.

**STOP — R13-E2 complete. No commit made. E3/R13-F not started.**
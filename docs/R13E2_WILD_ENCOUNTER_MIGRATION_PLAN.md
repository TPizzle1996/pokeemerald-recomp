# R13-E2 — Wild Encounter Migration Plan

Stage scope: migrate **wild encounters only** (`gWildMonHeaders` + the
land/water/fishing/rock-smash info + slot tables reachable from it) to
ROM-owned canonical resources and publish them through a new
`EmeraldEncounterCompat` seam. **Design/delta-parity review only — no
production code modified, no commit. E3 (Frontier + Pokédex) and R13-F not
started.** Pike/Pyramid facility wild mons are **out of E2** (they belong to
E3's frontier lane; see §3).

Grounding: re-derived directly from the qualified reference ELF/ROM
(`../pokeemerald-reference/pokeemerald.{elf,gba}`, SHA-1
`f3ae088181bf583e55daf962a92bb46f4f1d07b7`), not from R13-A estimates.
Current proven state entering E2: pack 17,525 entries / 13,595,744 B; cap
32,768; ranges 5,848/8,192.

---

## 1. Exact qualified-ROM inventory (re-derived)

### gWildMonHeaders
| | Value |
|---|---|
| ROM address | `0x08552d48` → file off `0x552d48` |
| Size | **2,500 B = 125 rows × 20 B** |
| GBA row layout | `u8 mapGroup, u8 mapNum, u16 pad, u32 landInfo, u32 waterInfo, u32 rockSmashInfo, u32 fishingInfo` (20 B) |
| Real map headers | **124** |
| Sentinel rows | **1** (`mapGroup=0xFF, mapNum=0xFF` = MAP_UNDEFINED terminator) |
| First / last real map | (0,16) Route 101 … (24,107) |
| Pointer-bearing | 4 info ptrs/row |

### WildPokemonInfo (via gWildMonHeaders)
| type | info count | slot/table | slot rows | slot bytes |
|---|---|---|---|---|
| land | 95 | 12 slots × 4 B | 1,140 | 4,560 |
| water | 55 | 5 × 4 | 275 | 1,100 |
| rock-smash | 6 | 5 × 4 | 30 | 120 |
| fishing | 53 | 10 × 4 | 530 | 2,120 |
| **total** | **209** | — | **1,975** | **7,900** |

- Info struct wire = **8 B** (`u8 encounterRate, pad, u8[2]?, u32 slotPtr` —
  rate + slot pointer). Native = 16 B.
- **Aliasing: NONE** — distinct info pointers == counts in every type
  (0 duplicates); every info is referenced exactly once; every slot table is
  referenced exactly once.

### Slot tables (`struct WildPokemon` = `u8 minLevel, u8 maxLevel, u16 species` = 4 B)
- Distinct slot tables: **209** (one per info, no sharing).
- Total slot rows: **1,975** (map-based). Total bytes **7,900**.

## 2. 125 vs 124 — RESOLVED (A bookkeeping + B sentinel)

The ROM `gWildMonHeaders` is **125 rows = 124 real map headers + 1
MAP_UNDEFINED sentinel** (`0xFF 0xFF`). The recomp `wild_encounters.json`
lists the 124 real maps; the generated `.h` (and hence the native binary)
appends the sentinel, giving 125. Both sides agree on 124 real + 1 sentinel.
**Canonical resource count = 124 real headers** (the sentinel is reconstructed,
not a resource). Classification: **A/B — no content delta.**

## 3. 209 vs 220 — RESOLVED (B counting artifact)

- **209** = info structs reachable **via gWildMonHeaders** (95 land + 55 water
  + 6 rock + 53 fishing). This is E2's set.
- **220** = 209 + **11 frontier-facility land tables** reached through the
  *separate* header arrays `gBattlePikeWildMonHeaders` (4) and
  `gBattlePyramidWildMonHeaders` (7). Those are frontier-facility wild mons.
- **Decision:** E2 = the 209 map-based info tables. The 11 Pike/Pyramid tables
  are **deferred to E3** (frontier lane). This avoids the double-ownership
  hazard flagged in the E-plan inventory (gBattlePyramidWildMonHeaders belongs
  to the frontier facility set, not the map-header set). No content delta.

## 4. 2,070 vs 2,107 — RESOLVED (stale arithmetic; actual 2,107 total)

- Total wild slot rows across the whole framework = **2,107** =
  map-based 1,975 + Pike 48 (4×12) + Pyramid 84 (7×12). Verified:
  1,975 + 48 + 84 = 2,107.
- **2,070 could not be reconstructed** from any reachable subset
  (headers-only = 1,975; adding Pike/Pyramid = 2,107). It is stale R13-A
  arithmetic, not a real count. Classification: **B — no content delta.**
- E2 pins: **1,975 map-based slot rows / 7,900 B** (the Pike/Pyramid 132 rows
  go to E3).

## 5. Recomp-vs-vanilla parity — PASS

Exhaustive recomp-vs-ROM comparison (E-plan encounter inventory agent;
re-confirmed here for headers):
- All **124 (mapGroup,mapNum) pairs identical in order** ROM vs native
  (first Route 101 (0,16), last (24,107)).
- Wild-mon slot content (species/level ranges), encounter rates, and
  null/non-null families match; Pike/Pyramid tables byte-identical (E3 scope).
- No deliberate fork behavior delta detected in the map-based encounters.
  Classification: **PASS — no override needed.** (If the implementation's
  own gate finds any D-class delta, STOP that family.)

## 6. Resource granularity

No aliasing (proven §1) → per-(map,type) resources are safe and there is no
shared-table materialization problem.

Canonical model:
- **`emerald:data/encounter/headers`** — ONE resource, the exact contiguous
  2,500 B ROM header block (124 real + 1 sentinel). GBA_POINTER_GRAPH; the 4
  info pointers per row are retained as validated provenance bytes and rebuilt
  by the seam. (gWildMonHeaders is contiguous in ROM, so a single block
  resource is the faithful, non-fragmented identity.)
- **`emerald:data/encounter/<map>/<type>`** — one per non-null (map,type),
  `type ∈ {land, water, fishing, rock-smash}`. Payload = the slot table
  (exact ROM slice, LEAF_BYTES); `encounterRate` is carried as resource
  metadata (extracted from the info struct; the info's slotPtr is validated
  == the slot-table ROM address). **209 resources.**

Mod target: `override emerald:data/encounter/route-101/land` changes one
map's land encounters without touching other maps or the header block.
A hack adding encounters to a new map overrides the `headers` block
(structural); changing species/levels/rates overrides the per-(map,type) slot
resource (direct).

## 7. Map identity contract (stable for R13-F)

Map key = the semantic map constant name, lowercased with `_`→`-`
(e.g. `MAP_ROUTE101` → `route-101`, `MAP_METEOR_FALLS_1F` →
`meteor-falls-1f`), derived from (mapGroup,mapNum) via the map
constants/group tables. **E2 and R13-F must use this identical key** — R13-F's
map-metadata resources will key `emerald:data/map/<map>` with the same
`<map>` component, so no later translation is needed.

**Altering Cave special case:** (24,106) has **9 headers** (indices 114–122),
selected at runtime by `VAR_ALTERING_CAVE_WILD_SET`. Key the 9 variants
`altering-cave-<wildset>` (wildset 1..9). All other 115 maps are unique
(group,num) → one key each. (116 unique map keys; 124 real headers.)

## 8. Canonical representation

| Table | Class | Canonical payload |
|---|---|---|
| `gWildMonHeaders` | GBA_POINTER_GRAPH (transformed) | exact 2,500 B ROM block; info ptrs retained as provenance, rebuilt by seam |
| WildPokemonInfo | transformed (rate = metadata) | not a standalone payload; rate + validated slotPtr linkage |
| slot tables | LEAF_BYTES | exact ROM slice per (map,type) |

No native pointers in the pack. Exact ROM provenance is preserved because the
headers block and every slot table are byte-exact ROM slices; the seam
validates every pointer edge (header.infoPtr → info; info.slotPtr → slot
table address) rather than zeroing them blindly.

## 9. Native publication model (EmeraldEncounterCompat)

Desired live graph: published `gWildMonHeaders` → published info structs →
slot arena. Keep `src/wild_encounter.c` consumer logic unchanged
(`GetCurrentMapWildMonHeaderId` linear-scan by mapGroup/mapNum, Altering-Cave
offset at :321–324).
- `gWildMonHeaders` → HOST_DATA native array (125 rows × 40 B), info pointers
  rebuilt to the published native info objects.
- WildPokemonInfo → HOST_DATA native info array (209 × 16 B), `slotPtr`
  rebuilt into the slot arena.
- Slot tables → ONE slot arena (concatenated in deterministic order); slot
  arrays remain byte-exact (4 B/row, no transform).
- One family arena (slot arena) is cleaner than several; info/headers are
  fixed HOST_DATA arrays.

## 10. Aliasing / shared tables

Proven: **no** info or slot table is referenced from multiple headers
(§1). Therefore publication does not need to preserve shared pointer identity
— every (map,type) materializes its own object. If a future ROM hack reuses a
table across maps, that is a MOD-provider override (new resource bound to
multiple headers), not an E2 concern.

## 11. State-v5

No encounter pointer enters serialized state (R13-A §6: all consumers
transient; roamer state is save-resident but uses species/level, not table
pointers). Register **one** COMPAT_OBJECT range over the slot arena (no
per-resource ranges). Post-E2 range index = **5,849 / 8,192**. No format
change, no range-cap raise.

## 12. Roamer boundary (sRoamerLocations)

`sRoamerLocations` (21 × 6 = 126 B) is the Latias/Latios roamer candidate-
route list — part of the roamer subsystem (runtime/save-driven), **not**
referenced by `gWildMonHeaders`. Classification: **INDEX_ROUTING (roamer
mechanic). DEFERRED from E2** — stays compiled; can be absorbed with a later
encounter-mechanic/misc stage. Not left ambiguous.

## 13. ROM-hack compatibility

| Mod | Mechanism | Class |
|---|---|---|
| Replace Route 101 land encounters | override `emerald:data/encounter/route-101/land` | direct resource override |
| Change water encounters on one map | override `…/<map>/water` | direct |
| Change fishing encounters | override `…/<map>/fishing` | direct |
| Reuse one table across maps | MOD-provider binds one slot resource to multiple headers | direct (mod provider) |
| Hack adds encounters to new maps | override the `headers` block (+ new slot resources) | requires headers-block override (structural) |
| New encounter mechanic (e.g. new slot type) | engine | mechanic adapter |

## 14. Pack arithmetic

- E2 resources: **210** (1 headers block + 209 slot tables).
- Canonical payload bytes: **2,500 (headers) + 7,900 (slots) = 10,400 B**.
- Pack: 17,525 + 210 = **17,735 entries** (well under the 32,768 cap; no cap
  change).
- Expected pack size ≈ 13,595,744 + ~10,400 payload + ~210×160 TOC + names ≈
  **~13.66 MB**.

## 15. Isolation design

- Compiled `gWildMonHeaders` payload absent (symbol sweep + whole-table hash).
- Compiled WildPokemonInfo objects absent.
- Compiled slot arrays absent (leaf-table scans; short-row 4 B rows handled by
  whole-table hashes, not per-row scans).
- ROM == pack == published graph: every pointer edge validated; the 209
  (map,type) identities proven; no aliasing.
- No blanket encounter exemption.

## 16. Implementation order

1. Inventory/delta resolution (this plan; all three deltas resolved A/B).
2. Generator/resource identities (headers block + 209 slot resources + map-key
   derivation incl. Altering Cave variants).
3. Pack rebuild (17,525 → 17,735).
4. `EmeraldEncounterCompat` publication seam.
5. Live cutover (guard compiled gWildMonHeaders/info/slots; GBA verbatim).
6. State-v5 (one slot-arena range; verify 5,849).
7. Isolation.
8. Encounter behavioral tests (per-type, Altering Cave, roamer-absent).
9. Battery/build/report.

## 17. Highest-risk exact gate (must pass before live cutover)

Every count delta resolved (125=124+sentinel; 209=95+55+6+53, +11 to E3;
slots 1,975 map-based / 2,107 total) **AND** every pointer edge classified +
validated (header.infoPtr→info, info.slotPtr→slot) **AND** no-aliasing proven
**AND** recomp-vs-vanilla behavior classified (PASS / explicit C preserved).
**Any unresolved D-class delta → STOP.** The two-level pointer graph plus
Altering Cave's 9 same-key headers is the concentration of risk.

---

## Projected totals

| Quantity | Value |
|---|---|
| gWildMonHeaders | 124 real + 1 sentinel; 2,500 B block |
| WildPokemonInfo | 209 (95/55/6/53); 8 B wire (rate as metadata) |
| slot tables | 209 distinct; 1,975 rows; 7,900 B |
| 125/124 reason | MAP_UNDEFINED sentinel (A/B) |
| 209/220 reason | +11 Pike/Pyramid frontier tables → E3 (B) |
| 2,070/2,107 reason | stale arithmetic; actual 2,107 total, 1,975 map-based (B) |
| parity | PASS (no override) |
| aliasing | none |
| resource model | 1 headers block + 209 per-(map,type) slot resources |
| map identity | semantic map name; Altering Cave `-<wildset>`; shared with R13-F |
| E2 resources | **210** / 10,400 B |
| pack after E2 | **17,735** entries (~13.66 MB) |
| State-v5 | one slot-arena range; 5,849/8,192; no format change |
| roamer | deferred (INDEX_ROUTING) |
| complexity | medium (2-level pointer graph + Altering Cave); ~the E1 effort |

**STOP — R13-E2 is a design/delta review; no production code modified, no
commit made, E3/R13-F not started.**

---

## Implementation delta (R13-E2 — supersedes the plan projections)

R13-E2 is **implemented and verified** (report:
`docs/R13E2_WILD_ENCOUNTER_MIGRATION_REPORT.md`). Every projection above landed
exactly (210 resources / 10,400 B; pack 17,735 entries / 13,650,128 B, SHA-256
`90a60c9d…aa6`; range 5,849/8,192; cap unchanged). `EmeraldEncounterCompat`
publishes the headers block + slot arena + native gWildMonHeaders/infos;
Altering Cave (9 headers, altering-cave-1..9) preserved; Pike/Pyramid +
`sRoamerLocations` stay compiled (E3-owned); all four deltas resolved (A/B),
parity PASS. Battery: loader 63,901 / trainer 37,921 / world 5,565 / render
3,628 — all green. Release 22,442,568 → 22,451,008 B (+8,440 B).

**R13-E2 STOP.** No commit made. E3 and R13-F not started.
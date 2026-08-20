# R13-E1 — Trainer Migration Report

Stage scope: migrate `gTrainers` (855) + trainer party leaves (854) +
`gTrainerClassNames` (66) to ROM-owned canonical resources, publish the native
48 B trainer table + party arena, cut over the compiled payloads. **No commit.
E2/E3/R13-F not started.** Trainer dialogue (map scripts), money table
(engine-adjacent), easy-chat, encounters, Frontier, Pokédex are NOT in E1.

Status flags: ✅ done & verified · 🅿 pending (none — all gates recorded below).

---

## 1. Trainer inventory (verified against the qualified reference ELF/ROM)

| Family | Count | Canonical ROM bytes | Native bytes |
|---|---|---|---|
| `gTrainers` | 855 rows × **40 B** wire | **34,200** (@0x310030) | 855 × 48 = 41,040 |
| trainer parties `sParty_*` | **854** leaves (no aliasing) | **18,088** (contiguous @0x30b62c..0x30fcd4) | 15,066 |
| trainer party mons | 1,825 | (in leaves) | — |
| `gTrainerClassNames` | 66 × 13 B | **858** (@0x30fcd4) | 858 |

Corrections vs the R13-A/brief baseline: the GBA wire row is **40 B (not 48)**;
48 B is the native `struct Trainer` (party pointer widened 4→8 + host padding).
Training party leaves total **18,088 B (not 15,066** — that is the native
packed size). `gTrainerMoneyTable` (56×4 B ROM) is a class-keyed engine
(money/AI) table — **deferred** from E1.

## 2. Trainer parity gate — PASS (zero divergence)

Exhaustive recomp-vs-vanilla comparison of all 855 rows + all 854 party
leaves (not a sample), pointer semantics transformed to identity:

- Row scalar fields (partyFlags/class/music-gender/pic/name/items/double
  battle/aiFlags/partySize): **0/855 mismatches**.
- Trainer names (in-row charmap): **0/855 mismatches**.
- Party pointer identity (GBA→symbol vs native→symbol): **0/854**.
- Party contents (iv/lvl/species/heldItem/moves, 1,825 mons, stride-adjusted):
  **0 mismatches**.

**Verdict: clean.** Recomp trainer metadata is byte-faithful to vanilla BPEE01;
no override is needed (unlike D1 evolution / D2 held-item rows). The only
deltas are structural: row stride 40→48, party strides {8,8,16,16}→{6,8,14,16},
pointer width 4→8. None is behavioral.

## 3. Party variants (from partyFlags)

Two bits: `F_TRAINER_PARTY_CUSTOM_MOVESET` (bit0), `F_TRAINER_PARTY_HELD_ITEM`
(bit1). Four variants, counts + strides (ROM per-mon / native per-mon):

| Variant | Tables | ROM stride | Native stride | Fields |
|---|---|---|---|---|
| NoItemDefaultMoves | 672 | 8 | 6 | iv u16, lvl u8, species u16 |
| NoItemCustomMoves | 87 | 16 | 14 | + moves u16[4] |
| ItemDefaultMoves | 31 | 8 | 8 | + heldItem u16 |
| ItemCustomMoves | 64 | 16 | 16 | + heldItem, moves[4] |

The variant is metadata (determined by partyFlags), not a payload rewrite; the
canonical party resource is the exact ROM leaf slice at `partySize × stride`.

## 4. Resource identities (profile-scoped, semantic)

```
emerald:data/trainer/<name>            (855; name = TRAINER_<suffix> lowercased,_->-)
emerald:data/trainer/<name>/party      (854)
emerald:data/trainer-class/<cls>       (66; class display string lowercased, spaces->-)
```
Trainer metadata and party are separate resources (mod can override either
independently). No raw numeric IDs in keys; no single-table resource.

## 5. Canonical representation

- Canonical trainer metadata = exact **40 B ROM wire row**, GBA party pointer
  retained as validated provenance bytes (not a native pointer).
- Canonical party = exact ROM leaf slice (variant tagged in metadata).
- Canonical class-name = 13 B charmap row.
- Publication reconstructs the native party pointer into the party arena. No
  native pointers in the pack.

## 6. Publication (EmeraldTrainerCompat) ✅

New `EmeraldTrainerCompat` seam (transactional, mirrors EmeraldGameplayCompat):
- Phase 1 validates all 855 metadata (schema 16) + 854 party (17) + 66 class
  (18) M0/M1 resolves (ROM_BASE winner), size/byte equality, generated-inventory
  set equality (pack scan for exactly 1,775 trainer-family entries),
  variant↔partyFlags agreement, wire partySize==generated meta, party leaf
  size==partySize×ROM-stride, row party pointer==leaf ROM offset, 854-leaves
  tile the contiguous 18,088 B block, class-row charmap validity.
- Phase 2/3 builds the native party arena (leaves in trainer-index order at
  native strides 6/14/8/16) + publishes `HOST_DATA struct Trainer gTrainers[855]`
  (48 B rows, party pointers rebuilt to the arena, TRAINER_NONE=NULL) and
  `gTrainerClassNames`. REFUSE-class, atomic. Zero consumer edits.

## 7. Pack-cap change ✅

`GEN3_PACK_MAX_ENTRIES`/`EMERALD_IMPORT_MAX_RECORDS` 16,384 → **32,768**
(entry + merged-import + refusal pins 32,769 / 16,385×2). Verified by the
resource-import battery (155 checks, 0 failures). Range cap 8,192 untouched.

## 8. State-v5 ✅

No format change; **one** COMPAT_OBJECT range over the trainer party arena
(key `emerald:data/arena/trainer-party`, schema 17); no per-trainer ranges.
Post-E1 range index = **5,848 / 8,192** (5,847 + 1). Trainer-row pointers are
transient (no serialized surface).

## 9. Regression battery + builds ✅

**Builds:** `make -f Makefile_pc linux64` links (exit 0); `--verify-game-data`
exits 0 over the 17,525-entry pack.

**Battery (all green), 17,525-entry pack:**

| Suite | Checks | Result |
|---|---|---|
| `run_emerald_runtime_loader.sh` | 62,993 | ✅ PASS (incl. `TestTrainerPublication` — 1,775 published, party-arena bytes exact, TRAINER_NONE NULL, SAWYER default/6-mon/double-battle/custom/items+custom variants exact, class-name rows, arena-resident + failure-matrix refusals) |
| `run_emerald_trainer_native_compat_production.sh` | 37,921 | ✅ PASS |
| `run_emerald_native_world_real.sh` | 5,565 | ✅ PASS |
| `run_emerald_native_world_render_proof.sh` | 3,628 | ✅ PASS |
| `run_emerald_resource_import.sh` | 155 | ✅ PASS (incl. 32,768-cap refusal pins) |

**Pack:** 15,750 → **17,525 entries**, 13,595,744 B, SHA-256
`9f8ca87b59b52f15fab3472b01c3ae4e3f11edee60656eaa11756256c236b6cb`,
deterministic rebuild byte-identical.

**Binary (release):** D2 22,363,584 → E1 **22,442,568 B (+78,984 B)** —
removed compiled gTrainers (41,040 native) + native-packed parties (15,066) +
class names (858) vs the seam + party arena + generated metadata. (A separate
DINFO-flavor delta was not captured.)

**Isolation (binary-level):** `gTrainers` (41,040, sector 28 host_data) and
`gTrainerClassNames` (858) are fill targets; compiled `sParty_*` leaf symbols
**absent** (0 matches); no compiled trainer payload remains.

Focused E1 gates verified: generator `--check` byte-identical; trainer/party/
class inventory (855/854/66); parity (0 divergence); every party-leaf
provenance + trainer→party binding; all 4 variants exercised; compiled-leaf
isolation; failure matrix (missing party, variant/partyFlags mismatch, bad
party size → REFUSE, published tables untouched).

## 10. Manual gate (DINFO checklist)

1. ordinary trainer battle; 2. trainer using held-item party; 3. trainer with
custom moves; 4. double battle; 5. rival/story battle; 6. trainer rematch;
7. save/load during a trainer battle; 8. no wrong names/classes/party/moves/
items/AI behavior.

## 11. Blockers / prerequisites for E2

- R13-E1 leaves trainers/parties/class names live and ROM-backed. E2 wild
  encounters is independent (its four delta flags are all resolved as
  non-divergences). The 32,768 cap gives ample headroom (E1 ends at 17,525).
- No E1 debt blocks E2.

**STOP — R13-E1 complete. No commit made. E2/E3/R13-F not started.**
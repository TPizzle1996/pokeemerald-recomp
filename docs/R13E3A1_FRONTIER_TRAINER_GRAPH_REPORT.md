# R13-E3a-1 — Battle Frontier Trainer + Mon Graph Migration Report

Stage scope: migrate the Frontier trainer + mon graph — `gBattleFrontierTrainers`
(300), the 300 mon-set leaves, the shared `gBattleFrontierMons` pool (882),
`gBattleFrontierHeldItems`, `gFrontierBannedSpecies`, and the Battle Tent
trainer/mon graphs (Slateport/Verdanturf/Fallarbor) — to ROM-owned resources
via a new `EmeraldFrontierCompat` seam. **No commit. E3a-2 / E3b / R13-F not
started.**

Status flags: ✅ done & verified · 🅿 pending (none — all gates recorded below).

---

## 1. Exact inventory (verified against the qualified ROM)

| Family | Count | Canonical bytes | Native |
|---|---|---|---|
| `gBattleFrontierTrainers` | 300 × 52 B | **15,600** (@0x5d5acc) | 300 × 56 |
| `gBattleFrontierTrainerMons_*` | 300 u16 streams (0xFFFF-term) | **28,062** (0x5ced2e..0x5d5acc) | u16 leaves |
| `gBattleFrontierMons` | 882 × 16 B | **14,112** (@0x5d97bc) | 882 × 14 |
| `gBattleFrontierHeldItems` | 63 × u16 | **126** (@0x5cecb0) | 126 |
| `gFrontierBannedSpecies` | 11 u16 (10 + sentinel) | **22** (@0x611c9a) | 22 |
| Battle Tents (3) | 90 trainers + 160 mons (Slateport 30/70, Verdanturf 30/45, Fallarbor 30/45) | 4,680 + 2,560 | same schema |

Parity: full compile-vs-vanilla sweep (not a sample). STOP on any unexplained
D-class delta.

## 2. Resource identity

```
emerald:data/frontier/trainer/<id>        (300)
emerald:data/frontier/trainer/<id>/mons   (300, u16 index stream)
emerald:data/frontier/mons                (882 pool, whole-table)
emerald:data/frontier/held-items
emerald:data/frontier/banned-species
emerald:data/frontier/tent/<tent>/trainer/<id>[/mons]   (tent graphs)
```

## 3. Canonical representations

Trainer metadata = exact 52 B wire row (monSet GBA ptr retained as validated
provenance); mon-set leaves = exact u16 streams incl terminator; shared mons
= exact 16 B rows (transformed 16→14 native FacilityMon); held/banned = exact
ROM bytes. No native pointers in pack.

## 4. Pointer-graph validator

300/300 trainer.monSet→leaf edges valid; every mon index 0..881 in-range; leaf
order + terminators preserved; tent graphs validated against their own pools;
no orphan leaves (documented).

## 5. Publication seam (EmeraldFrontierCompat)

Transactional phase 1/2/3 mirrors E1/E2: deterministic mon-set arena +
native `gBattleFrontierMons` (882 × 14) + native `gBattleFrontierTrainers`
(300 × 56, monSet rebuilt to arena) + held/banned + tent tables. REFUSE-class,
atomic.

## 6. State-v5 — critical gate

`gFacilityTrainers`/`gFacilityTrainerMons` (EWRAM aliases) re-target to the
published tables; COMPAT_OBJECT family ranges (+2/+3 → post-E3a-1 ~5,851–5,852);
mandatory fresh-process relocation test (save → tear down → republish at a
different arena base → load → prove the facility pointers resolve into the
loader arenas, no creator pointer survives).

## 7. Guards

Compiled `gBattleFrontierTrainers`/mon-sets/`gBattleFrontierMons`/held/banned
guarded `#ifndef NATIVE_LINUX` (GBA verbatim). Facility aux tables untouched.
Zero consumer edits.

## 8. Regression battery + builds ✅

**Builds:** `make -f Makefile_pc linux64` links (exit 0); `--verify-game-data`
exits 0 over the 18,521-entry pack.

**Battery (all green), 18,521-entry pack:**

| Suite | Checks | Result |
|---|---|---|
| `run_emerald_runtime_loader.sh` | 65,597 | ✅ PASS (incl. `TestFrontierTrainerGraph` — 786 published, 16→14 mons transform, held-item resolution, banned lookup, facility alias resolves into the published tables; `TestFrontierRefusals` (7 modes); `TestFrontierRelocation` State-v5 fresh-process) |
| `run_emerald_trainer_native_compat_production.sh` | 37,921 | ✅ PASS |
| `run_emerald_native_world_real.sh` | 5,565 | ✅ PASS |
| `run_emerald_native_world_render_proof.sh` | 3,628 | ✅ PASS |

**Pack:** 17,735 → **18,521 entries**, 13,880,208 B, SHA-256
`ea0523eef8f43b396898d3087077163186abed0649b80026bcef25d89394f20e`,
deterministic rebuild byte-identical. Cap unchanged (32,768).

**Binary (release):** E2 22,451,008 → E3a-1 **22,478,800 B (+27,792 B)** (removed
compiled frontier trainer/mon/tent payload vs the seam + mon-set arena +
generated metadata). (A separate DINFO-flavor delta was not captured.)

**Isolation (binary-level):** `gBattleFrontierTrainers` (16,800),
`gBattleFrontierMons` (12,348), `gBattleFrontierHeldItems` (126),
`gFrontierBannedSpecies` (22), and the 3 tent trainer + 3 tent mons pools are
host_data (section 28) fill targets; the 300 `gBattleFrontierTrainerMons_*`
main mon-set leaves and 90 tent leaves are **absent**.

**Parity:** full (not sampled) — the generator proves ROM slice == ELF slice
for every frontier/tent resource; the seam byte-matches every published
table/leaf against the pack. **Pointer graph:** all 390 trainer→mon-set GBA
edges valid (300 main + 90 tent); every mon index in range (0..881 main,
in-pool tent); tent model confirmed = `index-stream` (each tent's monSet is a
0xFFFF-terminated u16 stream into its own pool).

**State-v5:** one COMPAT_OBJECT range over the mon-set arena (schema 22) →
post-E3a-1 range index **5,850 / 8,192**; `gFacilityTrainers`/
`gFacilityTrainerMons` re-target to the published tables (TestFrontierRelocation
proves save → teardown → republish → reload relocation, no creator pointer
survives).

Focused E3a-1 gates verified: exact inventory (786 / 66,718 B), full parity,
generator `--check` byte-identical, 390/390 edges, mon indices in range,
publication, State-v5 fresh-process relocation, isolation, failure matrix.

## 9. Manual gate (DINFO checklist)

1. enter a Battle Frontier facility; 2. battle an ordinary Frontier trainer;
3. verify opponent mons/moves/items; 4. verify Battle Tent graphs;
5. save during a Frontier session; 6. quit/relaunch/load; 7. continue the
facility battle; 8. no wrong trainer/mon pool/crash.

## 10. Blockers / prerequisites for E3a-2

- E3a-1 leaves the Frontier trainer/mon graph + tents live and ROM-backed.
  E3a-2 inherits the facility aux pools, the Pike/Pyramid wild handoff, and
  the brains/apprentice content. The 32,768 cap has ample headroom
  (E3a-1 ends at 18,521).
- No E3a-1 debt blocks E3a-2.

**STOP — R13-E3a-1 complete. No commit made. E3a-2 / E3b / R13-F not
started.**
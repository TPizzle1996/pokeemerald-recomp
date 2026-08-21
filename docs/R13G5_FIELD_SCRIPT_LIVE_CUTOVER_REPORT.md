# R13-G5 Field-Script Live Cutover Report

**Stage:** R13-G5 — the FIRST LIVE field-script cutover (executed per
`docs/R13G_FIELD_SCRIPT_MIGRATION_PLAN.md` §17 G5 wave).
**Status:** COMPLETE (STOP after G5; no G6, no commit until review).
**Qualified truth:** BPEE01 Rev 0 (SHA-1
f3ae088181bf583e55daf962a92bb46f4f1d07b7), unchanged.

---

## 1. Result

The production game binary now boots through the atomic live cutover:
`EmeraldScriptCompat` stages the complete generation (523 modules,
207,330 B payload + the materialized 5,749 B routing class), registers
exactly **523 live module ranges** (total **6,377 / 8,192**), publishes
the live `gStdScripts` table, and rebinds every R13-F script surface —
before any script entrypoint can execute. `--verify-game-data` reports
the qualified ROM. Compiled field scripts are never executed after
publication.

## 2. Files changed

**Generator (routing materialization, G4 §8 prerequisite):**
- `tools/gen3_resources/script_family/gen_script_family.py` — per-module
  routing runs staged as span suffixes (5,749 B from the qualified ROM,
  emitted as `kScriptRoutingBytes` + 581 `kScriptRoutingSegments` rows,
  globally GBA-sorted); module spans become payload + routing (arena
  217,360 B); the bridge externs now reference the recomp-compiled
  names; every G1/G2/G3 pin still gates green.

**Seam (live publication + engine-facing resolver):**
- `include/emerald/resources/emerald_script_compat.h`,
  `src/emerald/resources/emerald_script_compat.c` — routing-suffix
  staging; the 830 routing sources become live operand rows (16,704
  live sources total); SCRIPT_ROUTING/DISPATCH targets resolve into
  the staged routing tables; all 3,501 F rows now carry staged
  addresses; `RegisterRanges`/`UnregisterRanges` (identity-based,
  the G2 text-seam lesson) + the 6,377 pin helper; `PublishStdScripts`
  (pure stores into the live table); `ResolveLiveOperand` (the
  ScriptReadPointer path), `ResolveRoutingTable` (mapScripts
  provenance), `ResolveFEntrypoint` (the F-only entrypoints the dynamic
  index never carries), `IsArenaAddress` (the O(log n) membership fast
  path), `ResolveObjectScript`/`ReverseResolveToGba` (the accessor
  pair), `ResolveVAddress` (static span case); the live arena
  registers as one dynamic buffer for the stable virtual anchor.

**State adapter:**
- `include/emerald/resources/emerald_script_state.h`,
  `src/emerald/resources/emerald_script_state.c` —
  `EmeraldScriptState_BuildVirtualAnchorFromBase` (the live
  setvaddress path); the STATIC_G_ARENA buffer kind; re-registration
  of the same (kind, owner) now REPLACES the generation (the restage
  model) instead of refusing.

**Engine (the §5.3 budget — central reader + the named sites):**
- `src/scrcmd.c` — `ScriptReadPointer` resolves arena operands through
  the typed source index (hard refusal, no compiled fallback for
  static G operands; battle/AI/H bytecode and dynamic buffers keep the
  legacy bridge); the eight vaddress handlers switch to the stable
  anchor; `sAddressOffset` is dead zeroed storage (the G4 adapter's
  ABI) with zero live readers/writers.
- `src/script.c` — `MapHeaderGetScriptTable` reads the staged routing
  table directly (published by the map rebind) and resolves each
  dispatch entry through the typed source index; the RAM-script
  storage registers as a dynamic buffer.
- `src/battle_setup.c` — the trainer parameter loader resolves
  trainerbattle text (TEXT → R13-C) and explicit field continuations
  (SCRIPT → G arena) by type; the implicit post-command return stays a
  live instruction pointer; no 4-byte encoded value is ever stored
  into a host pointer object.
- `include/global.fieldmap.h`, `src/field_control_avatar.c` — the
  ObjectEventTemplate accessors resolve/reverse-map through the live
  generation; the BG hidden-item case reverse-maps G pointers to
  their export GBA.
- `src/mystery_event_script.c` — the received buffer registers as
  MYSTERY_EVENT_BUFFER for the anchor + capture validation.
- `data/maps/Route103/scripts.inc`,
  `data/maps/SouthernIsland_Exterior/scripts.inc` — the three
  recomp-local movement bridge labels are `.globl`-exported (they were
  local asm symbols; the seam's bridge table references them).

**Map seam (R13-F rebind):**
- `include/emerald/resources/emerald_map_compat.h`,
  `src/emerald/resources/emerald_map_compat.c` —
  `EmeraldMapCompat_RebindScripts()` rewrites every header mapScripts
  pointer (staged routing table) and every coord/bg script pointer
  (staged G entrypoint) from the validated GBA provenance — all-or-
  nothing. Object rows keep their GbaAddr field; the accessors
  resolve. Per-map event-bundle bookkeeping retained at publish.

**Loader + build:**
- `src/emerald/resources/emerald_runtime_loader.c` — the boot ordering
  (plan §3): after the map seam → `TryInitialize` → `RegisterRanges`
  (pinned 6,377) → `PublishStdScripts` → `RebindScripts` → only then
  the session registers; any failure rolls back every seam (REFUSE-
  class, no compiled fallback).
- `Makefile_pc` — the G3/G4 filter-out is removed; the seam + its
  generated inventory + the state adapter are production-linked.

**Tests:** `tests/emerald_script_compat_test.c` (7,143 checks: live
range lifecycle, F-live rebind, arena 217,360 B, dispositions
8,441/8,216/3/18/26), `tests/emerald_script_fault_test.c` (live-
generation-preserving refusals), `tests/fault_script_table.py` +
`tests/run_emerald_script_faults.sh` (21/21), the state suites (live
6,377 pins), `tests/emerald_script_compat_harness.h` (the full-loader
and without-script setup variants), the runtime loader test (65,745
checks over the live cutover), stubs, and the runners.

## 3. Exact VM/interpreter changes

- **ScriptReadPointer**: one central change. Arena operands → typed
  resolver (O(log n) membership + O(log n) source lookup) → live
  pointer; refusals stop the script. No opcode handler grammar
  changed; no operand widened or patched; no second bytecode dialect.
- **vaddress family**: 8 handlers → `EmeraldScriptState_ResolveVirtualTarget`
  with the stable anchor; `setvaddress` → `BuildVirtualAnchorFromBase`.
  No creator-process delta persisted anywhere.
- **sAddressOffset**: retired (dead zeroed storage + accessor only;
  the G4 adapter's refusal contract still reads it as zero).
- **gotostd/callstd**: unchanged — the live `gStdScripts` table is the
  11-slot native publication table the seam fills.
- **Map dispatch**: 2 sites — the table base (staged routing) + the
  entry reads (typed source index).
- **Trainer loader**: 2 new parameter kinds (LOAD_TEXT/LOAD_SCRIPT);
  the speech/continuation spec rows converted.
- **Deferred messages**: already flow through ScriptReadPointer; the
  NULL fallback only fires for genuinely null pointers.

## 4. Boot ordering (live, in the R6 loader)

R13-B leaf → R13-C text → gameplay → trainer → encounter → frontier →
Pokédex → R13-F map → **script phase-1 validation → arena staging →
523-range registration (pinned 6,377) → gStdScripts publication →
R13-F rebind** → session registration. No script entrypoint is
exposed before every step commits; every failure path rolls back all
seams.

## 5. Range publication

523 spans (payload + routing suffix), exact resource key (derived id),
STRUCTURED_DATA type, schema 45/46, role CANONICAL. Identity-based
unregister (position-independent); the clear path unregisters before
freeing. Final count pinned at **6,377 / 8,192** in the loader and in
the focused test's lifecycle check (clear → −523 → restage →
re-register → 6,377).

## 6. Live resolution counts

16,704/16,704 parity (re-run on both generations, identical canonical
aggregates); dispositions across all rows: 8,441 staged arena, 8,216
sibling seam, 3 compiled bridge, 18 host RAM, 26 deferred (braille
pending); 95/8,113 root/interior; all 3,501 F rows staged (the 518
routing rows now materialized).

## 7. State-v5 live proof

The G4 adapter's strong path is live (the seam is production-linked
and published): nested interior fresh-process proof, RAM return,
trainer continuations, vaddress 8/8, Context2 refusal, 32/32 fault
matrix — all green against the live 6,377-range session (state
suites PASS).

## 8. No-compiled-fallback / runtime isolation proof

- Static G operand resolution refuses hard (no HostResolveGbaAddr
  fallback in the arena path);
- the live gStdScripts table holds only arena pointers;
- the F rebind leaves no compiled script pointer in any header or
  coord/bg row;
- the map dispatch reads only staged routing bytes;
- compiled field scripts remain LINKED (G6 owns physical removal) but
  are unreachable from execution after publication.

## 9. Pack invariant

20,988 entries, byte-identical; no payload changed; the three-way
oracle re-verifies all 16,704 relocations.

## 10. Regression battery + builds

| Suite | Result |
|---|---|
| G1 extractor `--check` / G2 generator `--check` / three-way | PASS |
| runtime loader (live cutover) | PASS (65,745 checks) |
| script compat (focused) | PASS (7,143 checks) |
| script fault matrix | PASS (21/21) |
| script state / cross-restart / state faults | PASS |
| resource state / ranges / script-module loader | PASS |
| native world real / render proof / real tables / isolation | PASS |
| release build | **25,007,320 B**, not stripped, zero debug sections, `--verify-game-data` → f3ae0881… |
| DINFO build | **38,375,496 B**, `with debug_info` |
| sanitizers | clean |

## 11. Manual validation checklist (DINFO)

1. load an existing save
2. walk through multiple maps
3. talk to NPCs
4. read signs
5. trigger a coord event if practical
6. enter/exit buildings
7. trigger movement scripts
8. use a Poké Mart
9. initiate a trainer battle
10. finish the battle and return to the field
11. save/load during normal field state
12. save/load after a scripted interaction
13. map transition immediately after load
14. watch for garbled dialogue, stuck scripts, wrong NPC actions,
    broken warps, post-battle hangs, or crashes

The automated cross-process nested-stack proof remains authoritative
if a natural mid-script save cannot be produced.

## 12. Exact G6 prerequisites

1. Physically remove the compiled G-owned payloads/local tables and
   flip every G resource to ROM_BASE_ONLY (isolation sweeps already
   modeled);
2. the compiled `gStdScripts` .s table can become zero-initialized
   slots once the publication is the only writer;
3. the 17-op MEVENT grammar boundary builder (the live dynamic
   buffers register with NULL instruction bitmaps; active
   mystery-event capture remains fail-closed until then);
4. the braille C handoff (26 edges) if the deferred targets must
   resolve live.

## 13. Blockers

None. Stopped after G5; G6 and R13-H not started; nothing committed.

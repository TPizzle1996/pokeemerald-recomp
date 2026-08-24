# R13-H6 — AI + Contest AI Live Cutover Report

Status: **complete**. The battle-AI and contest-AI VMs execute from the
H battle-ai / contest-ai arenas through the production live seam
(`emerald_battle_live.c` + `battle_live_table.generated.c` +
`battle_live_native.generated.c`), published transactionally by the R6
loader. Every AI control-flow operand (branch/jump/call/tail-call)
resolves through the typed relocation source index at consumption; the
two AI entry tables are arena-owned (32 rows each); the shared
`gAIScriptPtr` surface carries either AI family into State-v5 unchanged.
The live range count is **6,382 / 8,192** (6,377 + exactly 5 H ranges:
battle, battle-anim, battle-ai, contest-ai, field-effect). Ownership
stays `COMPILED_PENDING_MIGRATION`; no compiled H payload was removed
and nothing was committed.

Baseline: `checkpoint-r13h5-complete` (the H5 live cutover).
Qualified ROM: `../pokeemerald-reference/pokeemerald.gba`
(SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`).

## 1. Result

Both AI VMs run on their H arenas: the current AI IP, the 8-slot AI
call stack and every pointer-bearing AI operand live in the arena; the
compiled AI payload is never executed after the loader publishes
(brief sec 25). The mandatory AI fresh-process proofs (brief sec 22)
pass for BOTH families: an AI IP parked at a module root with one
active stack frame in a DIFFERENT module captures through the ordinary
State-v5 sidecar and restores in a fresh process at a forced-different
arena base with the perturbed layout, executing the exact next
canonical opcode, with the family's own stack surface relocated by
module+offset identity. The 235-opcode differential oracle covers every
AI opcode slot (67 battle-ai + 36 contest-ai qualified, 132 synthetic
fixtures), the AI fail-closed matrix passes 10/10, the
generation-replacement proof holds the 6,382 invariant with both AI
arenas participating, the AI pointer sweep finds zero unexpected
script-target words, and the pack stays at exactly 23,069 entries.

## 2. Files changed

Core (production-linked):

- `tools/gen3_resources/battle_family/h4_generate.py` — extended to the
  five live families (battle + anim + battle-ai + contest-ai +
  field-effect): the two AI arenas (battle-ai 9,303 B hull, 553
  modules; contest-ai 2,524 B, 165 modules), the AI routing-table word
  constants (`EMERALD_BATTLE_ROUTING_BATTLEAI` 0x082dbef8 /
  `EMERALD_BATTLE_ROUTING_CONTESTAI` 0x082de350 — the canonical table
  words, equal to the arena starts), the family-tagged AI grammar rows
  (99 battle-ai + 136 contest-ai opcode slots), the 38 data-target
  reloc rows (25 distinct `if_in_*` byte/hword list tables, offset 0 by
  map kind), and the shared script-target word set. `--check` is a
  no-op diff (verified).
- `src/emerald/resources/battle_live_table.generated.c` +
  `include/emerald/resources/battle_live.generated.h` (regenerated) —
  2,081 payload modules, 15,861 boundaries, 2,109 export rows, 7,549
  relocs, 718 bindings, 1,880 script-target words, 554 grammar rows
  (300 battle + 99 battle-ai + 136 contest-ai + 19 anim/FE), 5 arenas,
  199 labels.
- `src/emerald/resources/emerald_battle_live.c` (+65) — the AI family
  gates in `ResolveLaunchTarget`/`ResolveScriptTarget` (family-tagged
  word resolution; zero cross-family edges), `ResolveRoutingTarget`
  made family-generic (arena-row read + typed resolve under the
  table's own family — battle and both AI tables share one resolver),
  `ArenaCanonicalName` for `emerald:battle-ai/@arena` /
  `emerald:contest-ai/@arena`, the 5-range registration block, and the
  AI State-v5 surface registration at Publish.
- `include/emerald/resources/emerald_battle_live.h` — `RANGE_COUNT 5`,
  H6 API notes.
- `src/emerald/resources/emerald_runtime_loader.c` — the live block now
  validates/stages/publishes 5 arenas; the count gate is **6,382**;
  failure keeps the full fail-closed rollback.
- `src/battle_ai_script_commands.c` (+6) — `T1_READ_PTR`/`T2_READ_PTR`
  already funnel through `EmeraldBattleLive_ReadPointerOperand` on
  linux64 (H5); the AI entry/routing sites convert to
  `EmeraldBattleLive_RoutingScriptPtr(BATTLEAI, row)`; AI command
  bodies unchanged.
- `src/contest_ai.c` (+6) — the contest entry converts to
  `EmeraldBattleLive_RoutingScriptPtr(CONTESTAI, row)`; command bodies
  unchanged.
- `src/emerald/resources/emerald_battle_state.c` — the shared
  `gAIScriptPtr` surface (`FAMILY_COUNT` sentinel: either AI family at
  capture and at resolve) and the two stack surfaces (aiStackPtrs
  family BATTLE_AI, contestStackPtrs family CONTEST_AI). No State-v5
  format change. (Prepared in H5 — verified no H6 diff; the surface is
  live and exercised by the h6-state proofs.)

Tests:

- `tests/emerald_battle_live_test.c` (+1,699 lines vs H5 commit) —
  ai-oracle (2,351
  instructions / 1,586 relocs / 38 data targets / 64 routing rows /
  pointer sweep, both layouts), ai-faults (10 cases), ai-249
  differential (235 slots, 103 real + 132 synthetic, mini-VM execution
  of both AI entry roots with the production end/pop semantics),
  h6-state per-family fresh-process proof, DoReplace AI probes (5-range
  unregister sequence), H4/H5 pin updates (6,382, 2,043 entry words).
- `tests/run_emerald_battle_live.sh` — ai-oracle / ai-faults / ai-249 /
  h6-state modes, updated oracle/replace pins.

Unchanged by design: `src/battle_message.c` (STRINGID text path), the
anim/FE ownership, battle-script semantics, State-v5 format, the pack
contents (23,069 entries), the compiled AI tables (dead at runtime,
removed physically in H7).

## 3. AI publication model (brief sec 2/3)

The loader transaction stages all five arenas from the pack into one
host buffer (layout 0 = GBA-preserving; layout 1 = tight-packed
reversed for semantic-identity proofs), registers exactly five live
ranges, and publishes. The AI families join at the same transaction:
`EmeraldBattleLive_TryInitialize` -> `RegisterRanges` (6,382) ->
`Publish`. No partial cutover: every failure before publish leaves the
compiled runtime untouched; every failure after ranges registered rolls
the ranges back (the loader refuses the session).

## 4. Exact live range count (brief sec 4 — HARD GATE)

**6,382 / 8,192** with exactly 5 H ranges:

| Range | Canonical name |
|---|---|
| battle | `emerald:battle-script/@arena` |
| battle-anim | `emerald:battle-anim-script/@arena` |
| battle-ai | `emerald:battle-ai/@arena` |
| contest-ai | `emerald:contest-ai/@arena` |
| field-effect | `emerald:field-effect-script/@arena` |

Asserted at every test session boundary (oracle, faults, replace, both
AI state modes, H4/H5 state modes) and after every re-registration.
Unregister sequence (identity-specific, position-independent):
FE -> 6,381, anim -> 6,380, battle -> 6,379, battle-ai -> 6,378,
contest-ai -> 6,377; re-registration restores 6,382.

## 5. Battle-AI bytecode stays canonical (brief sec 5)

The battle-ai arena hull is exactly **9,303 bytes** (553 modules: 527
bytecode + 25 data tables + 1 routing) and contest-ai **2,524 bytes**
(165 modules: 164 bytecode + 1 routing) — staged byte-exact from the
committed `.bin` artifacts (byte-exact oracle). Every AI instruction
(1,738 battle-ai + 613 contest-ai = **2,351**) decodes to a qualified
(opcode, size) pair of the family-tagged grammar with the exact
boundary-map delta. No transform, patch, widen, or native-only opcode
encoding exists; control-flow resolution happens at consumption (sec 7).
The two AI grammars carry no `return` opcode — the mini-VM has no
return branch (the battle 0x42 branch is excluded from the AI decoder).

## 6. Typed AI relocation resolution (brief sec 6)

All **1,586** AI relocs (1,222 battle-ai + 364 contest-ai) resolve
through the chain raw operand -> relocation row -> semantic
module/export -> current arena pointer:

- 1,548 script-target relocs re-validate the target as an
  INSTRUCTION_START boundary of the semantic module and resolve to
  `targetBase + targetOffset`, checked equal to the arena pointer;
  the raw word itself (`ResolveScriptTarget`) yields the same pointer.
- **38** data-target relocs (the `if_in_bytes`/`if_in_hwords`
  byte/hword list tables, 25 distinct tables) pin `target_offset == 0`
  by map kind — no instruction boundaries (brief sec 6).
- Reverse containment: every resolved pointer reverse-resolves to the
  target module id at the row's offset under the row's family.

## 7. Narrow central interpreter reader (brief sec 7)

All AI control-flow operands funnel through
`EmeraldBattleLive_ReadPointerOperand` (the T1_READ_PTR/T2_READ_PTR
body on linux64): operand words read from any live arena span resolve
through the typed relocation source index (word-checked,
class-dispatched; legal NULL literals return NULL; everything else is a
hard fail-closed refusal). Words read anywhere else keep the legacy
path. The AI VMs hold no other pointer reader.

## 8. Current AI IP in the active arena (brief sec 9)

During AI execution the battle-ai/contest-ai IP is always an
instruction start inside the family's live arena: the mini-VM verifies
at every step that the IP reverse-resolves under the executing family
and re-validates the INSTRUCTION_START boundary. The state adapter
captures the shared `gAIScriptPtr` under the family that contains it
(FAMILY_COUNT sentinel) and the AI stack under its own family surface.

## 9. AI call-stack semantics (brief sec 9/22)

The AI VM stack carries only call returns: `call` (battle-ai 0x58,
contest-ai 0x80) pushes IP+5 (call size 5); `end` (0x5A/0x81) POPS the
stack — resuming the saved frame when non-empty and terminating the
run when empty (the production `Cmd_end` / `AIStackPop` semantics in
`battle_ai_script_commands.c` and `contest_ai.c`, re-read and mirrored
exactly). The mini-VM implements exactly this: no return opcode, no
0x42 branch, depth capped at 8.

## 10. Entry / routing publication (brief sec 11)

The two AI entry tables (`gBattleAI_ScriptsTable` 32 rows,
`gContestAI_ScriptsTable` 32 rows) are arena-owned: the compiled tables
are dead at runtime and every row word is read from the canonical arena
bytes and resolved as a SCRIPT_TARGET root of the table's own family.
The routing enum values ARE the canonical table words and equal the
arena starts (verified per arena). All 64 rows resolve to a script root
of the table's own family; a cross-family row word is refused by the
family gate (verified by direct corruption test, sec 18).

## 11. AI game-data dependencies (brief sec 12)

AI condition evaluation stays C-side: the `if_in_*` data tables
(byte/hword lists) resolve as DATA targets (offset 0), and all other
game state (attacker/defender stats, move data, score arrays) is
compiled C data — no AI bytecode change.

## 12. Cross-family isolation (brief sec 16)

Zero cross-family bytecode edges: every AI reloc targets the SAME
family; a battle-ai script-target word under the contest-ai family
refuses (`ERR_WRONG_FAMILY`) and vice versa; a battle-ai routing row
rewritten to a contest-ai target word refuses; a non-AI word under an
AI family refuses. The AI pointer sweep finds zero unexpected
script-target words of ANY family in AI bytecode (brief sec 17 — no
false data-pointer classification: scalar words never convert).

## 13. Battle-AI differential oracle (brief sec 18)

The `ai-oracle` mode (both layouts) pins:

- instructions **2,351** (battle-ai 1,738 + contest-ai 613)
- AI relocs **1,586** (data-targets **38**)
- routing rows **64** (32 + 32)
- pointer sweep **2,051** non-reloc 4-byte words, zero in the
  script-target word set
- entry-word arithmetic 2,043 = 2,081 payload − 13 routing − 25 data

Every opcode slot is covered: 67 battle-ai + 36 contest-ai qualified
from data, 32 + 100 synthetic fixtures whose width-4 operands carry a
REAL same-family script-target word (the `ai-249` mode: decoded 2,351,
slots-present 103, synthetic 132, executed 2). The mini-VM executes
routing row 0 of both AI tables end-to-end with typed control-flow
resolution and the production end/pop semantics (no host pointer
equality anywhere — all checks are module+offset identities).

## 14. AI fail-closed matrix (brief sec 26)

`ai-faults` **10/10**:

1. stage + publish under the 6,382 invariant
2. wrong-family launch refused (battle-ai root under contest-ai and
   vice versa)
3. corrupt battle-ai script-target operand word refuses; restore
   resolves
4. corrupt DATA-target operand word (an `if_in_*` table pointer)
   refuses; restore resolves
5. routing row out of range refuses (row 31 resolves, row 32
   boundary-invalid) on both AI tables
6. a non-routing word under `ResolveRoutingTarget` refuses
7. cross-family raw word gate (both directions)
8. cross-family routing row: a battle-ai row rewritten to a contest-ai
   target refuses; restore resolves
9. battle quiescence blocks replacement; generation A survives
10. identity unregister of both AI ranges (6,381 -> 6,380) and
    re-registration restores 6,382

## 15. Quiescence proof (brief sec 22)

Stack depth 0 at the capture boundary for both families by policy (the
AI VMs are synchronous and finish before battle actions complete; the
state adapter's stack surfaces classify empty stacks as INACTIVE and
scrub them). The h6-state proofs capture the AI IP at a module root
with one planted frame (stack size 1) and restore it exactly.

## 16. Generation replacement A->B (brief sec 23/24)

The `replace` mode: 6,382 -> clear (6,377) -> stage B (layout 1) ->
register (6,382) -> publish. The battle-ai root and the arena-owned
32-row entry table republish into the layout-1 arena at the exact
`layoutOffset[1]`; routing row 0 resolves to a battle-AI bytecode root
family-verified. The invalid-layout restage refuses and leaves the
current generation resolving. The unregister sequence (sec 4) holds the
identity pins at every step: before 6,382, after 6,382, failed
replacement 6,382, successful replacement 6,382.

## 17. AI fresh-process State-v5 proof (brief sec 22/30)

`h6-state-create`/`h6-state-load` per family: capture parks `gAIScriptPtr`
at the first bytecode module's root (offset 0) with one stack frame at
an instruction-start return in a DIFFERENT bytecode module, on the
family's own stack surface (aiStack for battle-ai, contestStack for
contest-ai; the other surface stays empty and scrubs). A fresh process
stages layout 1 (forced-different arena base + generation) and restores:

- the shared AI IP under the same family at the same module/offset
- the single stack frame at the same module/offset on the family's
  stack surface
- the exact next canonical opcode at the restored IP (committed `.bin`
  byte equality)
- the restored return is a valid instruction boundary; all restored
  pointers sit inside the new AI arena

State-v5 format unchanged (verified per H5 record + this run).

## 18. Compiled-fallback prohibition (brief sec 25)

ZERO runtime compiled AI execution: every entry (both AI tables) and
every pointer operand (all 1,586 reloc rows) resolves through the arena
with word-checked typed resolution; the sweep proves no non-reloc word
in AI bytecode equals any script-target word (a silent compiled-fallback
or cross-family raw pointer candidate); the routing tables are read from
the arena, never from the compiled image.

## 19. Missing/corrupt AI resource fail-closed (brief sec 26)

The loader refuses the session on any missing/corrupt AI module (the
AI modules join the pack digest/schema validation and the staging
byte-exact check); the seam's resolver refuses malformed lengths, raw
operand mismatches, missing targets, invalid boundaries, wrong-family
words, and missing entry bindings — all exercised by the matrix above
(sec 14) plus the H2 loader/staging fault suites.

## 20. Manual DINFO checklist (brief sec 36)

Interactive battle flows with the AI live on the DINFO build:
wild/trainer/double battles, AI move selection + switch logic, contest
AI routines, damage/status/stat/faint/switch/victory, trainer
continuation back to the field, save/load mid-battle (AI quiescent at
capture; stale IP never resumed).

## 21. H5/H4 regression protection (brief sec 28/29)

The full live suite re-runs with the H6 pins: battle-oracle (238
routing rows / 199 labels / 50 EWRAM, both layouts), battle-faults 8/8,
battle-249 (3,390 instructions, 238 + 11 slots), h5-state nested
blocking proof, H4 oracle (2,043 entry words / 2,081 byte-exact /
7,549 relocs, both layouts), faults 11/11, replace 6,382 invariant,
anim fresh-process proof — all green (see battery).

## 22. Regression battery (brief sec 34)

All green (2026-08-23):

- **H1** analyzer `--check`: 10 files byte-identical (grammar fixtures
  incl. field_effect_scripts 67/67 ops, interior 0, roots 67).
- **H2** loader/staging/four-way oracle + pack determinism: 2,089
  modules / 2,081 payload / 90,047 B / 7,549 relocs / 65 NULLs /
  3,058 addends / 2,109 exports / 718 bindings / pack **23,069**
  entries (byte-identical on regeneration).
- **H3** state readiness: 2,089 modules (2,081 payload, 8 aliases),
  15,861 boundaries, 2,109 export rows, 5 arenas, 90,047 B.
- **H4** live suite: oracle both layouts (2,043 entry words / 2,081
  byte-exact / 7,549 relocs / 3,798 script-targets / 20 refuse-only),
  faults 11/11, generation replacement (6,382 invariant, identity
  unregister/restore), anim fresh-process proof (module 553 ip 4 ret
  19), 249-opcode differential (3,390 decoded).
- **H5** battle live suite: oracle both layouts (238 routing rows / 199
  labels / 50 EWRAM / 2,587 swept), faults 8/8, nested blocking
  fresh-process proof (module 1208 ip 3 rets 1253/5 + 1255/5,
  opcode 0x12), battle-249 (3,390 / 238 + 11 slots).
- **H6** AI live suite (this cutover): ai-oracle both layouts,
  ai-faults 10/10, ai-249, h6-state both families, replace — all pins
  in sec 4-17.
- **Sanitize** (ASan/UBSan, full H4/H5/H6 suite): all modes clean, 0
  findings, exit 0.
- **General suites** (24, all exit=0): battle-module-loader,
  battle-state, battle-state-faults, battle-state-cross,
  runtime-loader, script-compat, script-faults, script-state,
  script-state-faults, script-state-cross, trainer, object-event,
  tileset, layout, real-tables, ranges, rom-base, lz, import,
  fingerprint, neighborhood, render-proof, world-real, desktop-probe.

## 23. Pack invariant (brief sec 32)

`games/emerald/base/emerald-bpee01-v1.rpack`: **23,069 entries** —
unchanged since H2 (no new AI resource, no schema change). Canonical H
bytes **90,047** across the 5 arenas (battle-ai 9,303 + contest-ai
2,524 verified in this cutover; totals per H2/H3/H4 census).
Generators h1/h2/h3/h4 `--check` byte-identical on regeneration.

## 24. Ownership (brief sec 31)

All H resources stay `COMPILED_PENDING_MIGRATION` (battle 645/645,
anim 658/658, battle-ai 553/553, contest-ai 165/165, FE 68/68). H7 owns
physical removal and the ROM_BASE_ONLY flip.

## 25. Forced builds (brief sec 35)

Command (the H5 report's "-B linux64" notation is inaccurate — the GBA
Makefile has no `linux64` goal; the canonical forced build is
`make -f Makefile_pc NATIVE_LINUX=1 LINUX64=1 -B rom`, per the
Makefile_pc memory, verified again here):

| Build | H5 baseline | H6 | Delta |
|---|---:|---:|---:|
| Release (`-B DINFO=0 rom`) | 23,913,784 B | 24,106,384 B | +192,600 B |
| DINFO (`-B DINFO=1 rom`) | 36,644,768 B | 36,837,640 B | +192,872 B |

The release delta is the AI live structures: the two AI arena table
sections (718 module rows incl. 553 battle-ai + 165 contest-ai, 1,586
AI reloc rows, 64 routing rows, 554 grammar rows incl. 99 + 136 AI
slots), the family-tagged AI resolvers, and the AI entry publication.
Release: zero debug sections (DINFO: 7), unique build IDs. Both builds:
`--verify-game-data` -> `f3ae088181bf583e55daf962a92bb46f4f1d07b7`,
exit 0.

## 26. H6 hard completion gates (brief sec 38)

- battle-ai live: 553 modules / 9,303 B / 1,222 relocs / IP in H arena /
  entry+routing in H arena / zero compiled fallback ✓
- contest-ai live: 165 / 2,524 B / 364 relocs ✓
- ranges exactly 5 H + 6,382 total ✓
- State-v5 unchanged + quiescent policy proven ✓
- zero cross-family edges ✓
- H4/H5/G green (battery) ✓
- pack exactly 23,069 ✓
- ownership COMPILED_PENDING_MIGRATION ✓
- full battery green (sec 22) ✓

## 27. STOP conditions (brief sec 39)

None triggered: total count 6,382 (never deviated); no raw GBA pointer
arithmetic needed (typed resolution covers every AI operand); no
persistent nested AI call stack contrary to H3 (AI stack is
call-returns only, depth 0 at capture by policy); no unsafe active AI
capture (quiescent policy + fresh-process proofs); no cross-family raw
edge (sec 12); no battle/anim regression (battery); no compiled
fallback required (sec 18); State-v5 format unchanged; pack/schema
unchanged; no physical removal before cutover (H7 owns it).

## 28. H7 prerequisites

H6 leaves the five families live with ownership
COMPILED_PENDING_MIGRATION. H7 removes them from the linux64 link
(assembly gates per data file), flips ownership to ROM_BASE_ONLY, and
runs the isolation sweeps — the entry tables and all 1,586 AI relocs
resolving through the arena mean the compiled AI payload is already
execution-dead; removal is mechanical.

## Final pins

| Pin | Value |
|---|---:|
| Live ranges | 6,382 / 8,192 (6,377 + 5) |
| Battle-ai modules / bytes / relocs | 553 / 9,303 / 1,222 |
| Contest-ai modules / bytes / relocs | 165 / 2,524 / 364 |
| AI instructions (battle-ai + contest-ai) | 2,351 (1,738 + 613) |
| AI relocs / data-target relocs | 1,586 / 38 (25 tables) |
| AI routing rows | 64 (32 + 32) |
| Opcode slots: battle-ai / contest-ai | 99 / 136 (67 / 36 real, 32 / 100 synthetic) |
| Entry words | 2,043 (2,081 − 13 routing − 25 data) |
| Grammar rows | 554 |
| Pack entries | 23,069 |
| Ownership | COMPILED_PENDING_MIGRATION |

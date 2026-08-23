# R13-H3 — Battle-Family State-v5 Readiness Report

Status: **complete**. This was a State-v5 execution-readiness / shadow
relocation stage only. No battle/animation/AI/contest/field-effect
execution was cut over, no H arena was registered with the production
range index (live count stays 6,377, zero H ranges), no interpreter was
modified, no ownership flipped, no compiled H payload was removed, and
nothing was committed.

Baseline: `checkpoint-r13h2-complete` (commit `ee80a7f7d`).
Qualified ROM: `../pokeemerald-reference/pokeemerald.gba`
(SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`).

## 1. Result

The hard gate passes: a nested battle-script execution state whose
current IP is parked at a blocking command (`waitmessage`, 0x12) with
two active call-stack frames holding interior IP+5 return offsets in
different modules captures through the ordinary State-v5 sidecar,
destroys its staging generation, restores in a fresh process at a
forced-different arena base with a deliberately perturbed physical
layout, and executes the exact next canonical opcode and return
sequence. The State-v5 file format is unchanged; the production live
range count stays **6,377 / 8,192** with **0 H ranges**; the five-family
dry run projects **6,382 / 8,192**. The H refusal matrix passes
**16/16**; the G State-v5 regressions stay green (5/5 + 5/5 + 31/31).

## 2. Files changed

Core (production-linked):

- `src/platform/native_state.c` — the battle-family hook ladder (weak
  prepare/capture/resolve/static-record hooks ordered after the
  field-script adapter; the trainer-continuation fields stay G-owned).
- `Makefile_pc` — links the weak adapter only; the shadow seam
  (`emerald_battle_compat.c`) and its generated inventory
  (`battle_native_table.generated.c`) are excluded from the native link
  through H3.
- `src/emerald/resources/emerald_battle_state.c` (new) — the weak
  State-v5 battle-family adapter.
- `include/emerald/resources/emerald_battle_state.h` (new).

Shadow seam (harness-linked through H3):

- `src/emerald/resources/emerald_battle_compat.c` (new) — the five-family
  shadow staging + State-v5 identity bridge.
- `include/emerald/resources/emerald_battle_compat.h` (new).
- `src/emerald/resources/battle_native_table.generated.c` +
  `include/emerald/resources/battle_native.generated.h` (new,
  generated).
- `tools/gen3_resources/battle_family/h3_generate.py` (new) — emits the
  table from the H2 sidecars; `--check` is a no-op diff.

Tests:

- `tests/emerald_resource_state_test.c` — H3 fixture block, h3-create /
  h3-load / h3-faults drivers, five variant fixtures, the sidecar
  patcher with CRC repair, the five-family dry-run transaction, and the
  alias policy proofs.
- `tests/emerald_resource_state_stub.c` — the battle execution globals
  are now exact production-shaped EWRAM_DATA declarations (battle_main.c
  184-188 / battle_ai_script_commands.c 154 / battle_anim.c 92-94), the
  anim statics added, and the TextPrinter registry returns real
  all-inactive storage (the pre-existing NULL return crashed the EWRAM
  failure forensics — a latent stub gap the H3 fault matrix exposed).
- `tests/run_emerald_resource_state.sh` — h3-state / h3-cross /
  h3-faults / h3-sanitize modes.
- `tests/run_emerald_battle_state.sh`,
  `tests/run_emerald_battle_state_cross_restart.sh`,
  `tests/run_emerald_battle_state_faults.sh` (new wrappers).
- `tests/run_emerald_rom_base_provider.sh` — adds
  `emerald_script_state.c`/`emerald_battle_state.c` to the
  platform-neutral exclude list (the G4-era state adapters are native-
  only consumers, like the excluded native_compat files; the runner's
  list had not been updated since R13-F).
- `tests/run_emerald_script_compat.sh`,
  `tests/run_emerald_script_compat_sanitize.sh`,
  `tests/run_emerald_script_faults.sh` — restore the executable bit
  (tracked as 644 by accident; the runners are shebang scripts).

No battle/animation/AI/contest/field-effect interpreter TU is modified
(`git diff -- src/battle_main.c src/battle_anim.c
src/battle_ai_script_commands.c src/contest_ai.c src/field_effect.c
src/battle_script_commands.c src/battle_util.c` is empty).

## 3. State-v5 format status (brief §1)

Unchanged. Staged H execution pointers use the ordinary 64-byte v5
resource sidecar record exactly as G4 established:

```text
resourceKey          = the module's derived key (canonical payload owner)
resourceType         = STRUCTURED_DATA
resourceSchema       = the family schema (47-51)
representationRole   = CANONICAL
rangeOffset          = the module payload offset
```

No persisted host pointer, no creator-process arena delta, no new file
field, no new section, no version bump. The boundary role is derived
from the exact destination field (IP / return / entrypoint), never
stored. Engine function pointers keep their existing engine-image
handling; scalars stay scalars.

## 4. Exact H state surfaces and slots (brief §5)

Census of the exact production structures (source-verified, not the
architectural list):

| Surface | Storage | Decl | Slots | Slice | Boundary role |
|---|---|---:|---|---|---|
| `gBattlescriptCurrInstr` | EWRAM | battle_main.c:184 | 1 | EWRAM | INSTRUCTION_START |
| `gSelectionBattleScripts[4]` | EWRAM | battle_main.c:187 | 4 | EWRAM | INSTRUCTION_START |
| `gPalaceSelectionBattleScripts[4]` | EWRAM | battle_main.c:188 | 4 | EWRAM | INSTRUCTION_START |
| `battleScriptsStack.ptr[8]` | gHeap (GAME_BSS) | battle.h:202 | 8 size-gated | GAME_BSS | NEXT_INSTRUCTION |
| `battleCallbackStack.function[8]` | gHeap | battle.h:208 | 8 | GAME_BSS | engine function image |
| `gAIScriptPtr` | EWRAM | battle_ai_script_commands.c:154 | 1 | EWRAM | INSTRUCTION_START (quiescent stale; shared with contest AI) |
| `AI_ScriptsStack.ptr[8]` | gHeap | battle.h:228 | 8 size-gated | GAME_BSS | NEXT_INSTRUCTION |
| `sBattleAnimScriptPtr` | EWRAM static | battle_anim.c:92 | 1 | EWRAM | INSTRUCTION_START |
| `sBattleAnimScriptRetAddr` | EWRAM static | battle_anim.c:93 | 1 | EWRAM | NEXT_INSTRUCTION |
| `gAnimScriptCallback` | EWRAM static | battle_anim.c:94 | 1 | EWRAM | engine function image |
| `ContestAIInfo.stack[8]` | gContestResources heap | contest.h:238 | 8 size-gated | GAME_BSS | NEXT_INSTRUCTION |
| `sTrainerBattleEndScript` + A/B | EWRAM | battle_setup.c:118-120 | 3 | EWRAM | G-owned today; H targets at H5 |

Field-effect bytecode has **no persistent instruction pointer** (the
interpreter cursor is function-local, field_effect.c:705) — capture
cannot legally observe active FE execution; the capture policy is an
explicit refusal (sec 11).

## 5. Maximum active sidecars (brief §26)

H1's projection was "9 surfaces / 40 slots / worst case ≈27 records".
H3 truth: 36 H-typed slots + 9 engine-typed slots.

- Maximum simultaneously active H resource records (mid-battle +
  mid-animation + stale AI): 1 IP + 8 battle stack + 4 selection + 4
  palace + 2 anim + 1 AI = **20** of the 4,096-record cap.
- Test/debug support adds up to 8 AI-stack + 8 contest-stack records
  (never simultaneously capturable in production: the AI stack is
  provably size-0 at every VBlank, and contest resources never exist
  during a battle capture).
- Engine-image records: 8 battle callbacks + 1 anim callback.
- Verdict: **20 < 27** — no plan correction required; headroom ≈ 200×.

## 6. Module identity representation (brief §21/§22)

The range registry (at H4+) will identify only the five family arenas.
The adapter refines arena address → internal module reverse containment
→ semantic module key → module offset, and the serialized form is the
module key + module-local offset (never a family-relative aggregate
offset). Proven by two physical layouts:

- layout 0: GBA-preserving placement inside each arena, arenas in
  sidecar order (the h2_stage.py geometry);
- layout 1: modules tight-packed in module-id order, arenas in reversed
  order — a deliberate physical perturbation.

The creator stages layout 0, the restorer layout 1, and every restored
pointer resolves to the same (module, offset) identity. No sidecar field
or format change was needed; the module key's 32-byte derived form fits
the existing record.

## 7. Five-family range dry run (brief §2/§20)

On a scratch clone of the live range index (the production index is
never mutated):

- live count 6,377 → register the 5 arena ranges (synthetic identity
  `emerald:<family>/@arena`, the family schema, CANONICAL role) →
  **6,382**, no overlaps;
- a failed replacement (overlapping span) refuses and rolls back
  (count unchanged);
- unregister by exact family identity (key + type splice) → 6,377;
- the other 6,377 entries are byte-identical to the clone afterwards
  (unrelated G/C/B/F ranges untouched);
- `ValidateProjectedRanges` pins the arithmetic and refuses overflow
  (8,190 + 5 > 8,192).

## 8. Zero-width alias policy (brief §3/§23)

Rule: **state identity always canonicalizes to the payload owner** — the
module whose export at the shared GBA address carries `boundary_kind =
"offset-zero"`. Proven for all 8 aliases by construction (the generator
derives each alias's owner from region containment; exactly one owner
exists per alias). Tests pin:

- `GetStateIdentity(alias)` returns the owner's key/schema (effect-
  morning-sun → effect-moonlight, schema 47);
- `ValidateBoundary(alias, ...)` refuses with ALIAS_IDENTITY;
- resolving the alias's own derived key refuses with ALIAS_IDENTITY
  (zero-width identities never resolve to pointers);
- `ReverseResolve(owner base)` returns the owner, never the alias;
- the owner's ENTRYPOINT at offset 0 remains valid;
- no zero-length range, no ambiguous reverse mapping, deterministic
  export lookup (sorted export index).

## 9. Battle current-IP proof (brief §6)

The IP is placed at a real `waitmessage` command start (a command that
re-executes each frame while paused), captured as a stable identity,
the generation destroyed (same-process restage relocates the arena
provably: candidate-first allocation), a new generation built at a
different base with a different layout, and the restored pointer proves
the same module + offset with INSTRUCTION_START validity and executes
the exact canonical opcode (staged byte == pack payload byte == 0x12),
including its 2-byte pause operand. Host pointer equality is not
expected and is not asserted.

## 10. Nested battle call-stack proof — HARD GATE (brief §7)

The main fixture holds two active frames with IP+5 return offsets
(`call` 0x41 = opcode + u32 target; `BattleScriptPushCursor` pushes the
already-advanced pointer) in **different modules**, plus scrubbed
inactive scratch. Fresh process B (perturbed layout, new base) restores:

- current IP module + offset identical;
- every active stack entry module + offset identical
  (NEXT_INSTRUCTION-validated);
- the exact next opcode executes at the parked blocking command;
- both frames pop in the native LIFO order, each executing the exact
  canonical opcode at its return offset.

Result: `nested-return=ok blocking=ok`.

## 11. Blocking-command suspension proof (brief §9 — mandatory)

The parked command is `waitmessage` (0x12, battle_script_commands.c
2159): while `gPauseCounterBattle < operand`, the IP never advances and
the command re-executes each frame. The fixture parks the IP exactly
there with a nonempty call stack; the restore proves the wait/block
state (pause operand round-trips verbatim), the exact next instruction
semantics, the preserved call stack, no command replay/skip, and no
stale host pointer (every restored pointer re-derives from the current
generation).

## 12. Callback-stack classification (brief §8)

`battleCallbackStack.function[8]` and `gAnimScriptCallback` hold engine
function pointers (the main-loop and anim-driver callbacks) — never H
bytecode. The harness plants real engine functions; they persist through
the existing image-relative path and restore by identity. Planting H
bytecode in a callback slot refuses with a precise misclassification
diagnostic (fault matrix #4). Empty/one-entry/deep/NULL-slot cases are
covered by the main fixture (2 entries), the quiescent variant (0), and
the fault fixtures.

## 13. Selection/continuation pointers (brief §10)

`gSelectionBattleScripts[4]` and `gPalaceSelectionBattleScripts[4]` are
EWRAM parking slots (battle_main.c:4497/4502 swap them with the IP).
Both are captured as INSTRUCTION_START identities and restored through
the current generation; NULL slots stay NULL. The trainer continuations
(`sTrainerBattleEndScript` + A/B) remain G-owned slots through H3; their
class widens to accept H targets at H5. Exact slot counts: 4 + 4
selection/palace, 3 trainer.

## 14. Animation State-v5 readiness (brief §11/§12)

`gBattleAnimScriptPtr` (INSTRUCTION_START) and the single
`sBattleAnimScriptRetAddr` (NEXT_INSTRUCTION — the anim VM has no stack;
its `call` 0x0E stores IP+5 into the one slot) relocate through the
sidecar. The anim-only variant proves the parked IP at a frame-wait
command (`waitforvisualfinish` 0x05), the active return, the exact next
command execution, and the return landing at the next instruction.
Animation task/callback references (sprite templates, `createvisualtask`
functions) are engine bindings — never H identities; `gAnimScriptCallback`
stays engine-image-relative. No live anim-VM change.

## 15. Battle-AI capture policy (brief §14)

`BattleAI_DoAIProcessing` runs synchronously to completion inside the
controller callback — at every VBlank the VM is quiescent
(`aiState == FinishedProcessing`) with a stale-but-valid `gAIScriptPtr`.
Policy: **relocate the stale pointer, never execute it** (the next turn
re-derives it from `gBattleAI_ScriptsTable`); the AI stack is size-0 at
capture by construction. The representation is relocation-safe for
test/debug captures: the AI variant round-trips a stale IP plus two
synthetic stack frames (the battle-AI grammar has no return-valid call
sites — all five AI `call` commands are tail calls — so the frames sit
at ordinary instruction starts, the identical NEXT_INSTRUCTION
predicate). No AI interpreter change.

## 16. Contest-AI capture policy (brief §15)

Independent audit: the contest interpreter shares only the
`gAIScriptPtr` EWRAM slot; its stack lives in
`gContestResources->aiData` (heap, allocated only during a contest, so
it never exists during a battle capture). The adapter accepts either
battle-AI or contest-AI pointers on the shared slot (the surface family
is "either AI") and refuses anything else. The contest variant captures
a mid-contest state (IP in a contest module + one stack frame) and
restores it in process B with exact identities.

## 17. Field-effect capture policy (brief §13)

Source-verified: the FE interpreter cursor is function-local
(field_effect.c:705); no static/EWRAM/COMMON storage holds an FE script
pointer, so capture cannot legally observe active FE bytecode. Policy:
an FE-arena pointer in any serialized field is a precise policy refusal
(`ERR_TRANSIENT`). Positive coverage proves the FE family is otherwise
fully modeled (instruction boundaries + entrypoints + arena staging),
so the policy is the only FE-specific rule.

## 18. Boundary validation (brief §17/§18)

Per family, per module: INSTRUCTION_START and NEXT_INSTRUCTION both
require an exact bytecode-map instruction start (runtime return
addresses are interior instruction boundaries by construction — H1's
interior RELOCATION count of 0 says nothing about runtime returns);
ENTRYPOINT requires a generated export at the offset. Refused
precisely: middle-of-operand, holes and inter-module gaps, past-end
offsets, data/routing/empty spans for instruction roles, zero-width
alias keys, and wrong-family keys. The distinction between
relocation-target roots and runtime interior returns is exercised by
the nested stack fixture itself.

## 19. Reverse containment across generations (brief §19)

Reverse containment is per-generation over the current module spans
(bsearch, 2,081 entries); holes, gaps, and the battle hull's unmapped
FE region refuse. The adapter's stale-generation gate (the fixture's
stamp vs the current monotonic generation id) refuses before any
pointer check. Fault matrix #1 proves a capture stamped for generation
A refuses after a same-process restage to generation B (whose arena
base provably moved). The seam's transaction is candidate-first: a
failed restage preserves the previous generation untouched (fault #13
exercises the cleared-generation fallthrough, and the dry run
exercises rollback).

## 20. Generation A/B bases

Recorded passing runs (ASLR per process, plus the deliberate layout
perturbation; layout 0 arena total 90,868 B, layout 1 total 90,047 B):

- h3-state: creator `0x1ed92f40` (layout 0) → restorer `0x31dec9e7`
  (layout 1);
- h3-cross: creator `0x206fbf40` → restorer `0x2ae289e7`;
- same-process restage inside h3-faults: base moves by construction
  (candidate-first allocation, fault #1 asserts `a != b`).

## 21. Failure/refusal matrix (brief §25)

16/16, each with a precise diagnostic and an unchanged prior state:

1. stale family generation (stamp A while B is current) — refuse
   before any pointer check;
2. corrupt current IP (middle of operand);
3. corrupt call-stack entry (genuine mid-operand return);
4. corrupt callback-stack H entry (bytecode on an engine slot);
5. missing selected script (battle hull hole);
6. wrong family for pointer (anim pointer on the battle IP);
7. engine function pointer mislabeled as an H script;
8. field-effect transient state (prohibited by policy);
9. anim return slot middle-of-operand;
10. inactive stack scratch holding a live H pointer;
11. instruction pointer into a typed data span;
12. corrupt battle stack depth (9 > 8);
13. partial generation (no shadow generation — safe generic refusal);
14. missing module (sidecar key flip — load side, CRC-repaired);
15. offset out of bounds (load side);
16. wrong family schema (load side, schema 47→48).

Plus the alias refusals (sec 8), the failed-replacement rollback
(sec 7), the projected-range overflow refusal, and the generic
sidecar-capacity coverage in the existing corrupt matrix.

## 22. Pack invariant (brief §30)

No H resource changed: pack entries **23,069**, size 15,278,272 B,
SHA-256 `b711d35877332c43ce671caacda7640905205579a367e8d60bc683c5d0aac5bb`,
`gen3-pack-build --check` byte-identical. H2 generator `--check`
byte-identical; H3 generator `--check` byte-identical (2,089 modules /
15,861 boundaries / 2,109 export rows / 90,047 B).

## 23. Production execution isolation (brief §28/§29)

- `git diff -- src/battle_main.c src/battle_anim.c
  src/battle_ai_script_commands.c src/contest_ai.c src/field_effect.c
  src/battle_script_commands.c src/battle_util.c` is empty — no
  interpreter was modified; compiled H payloads remain the live runtime
  source.
- The production link contains the weak adapter only; the shadow seam
  and its generated inventory are excluded (Makefile_pc filter). `nm`
  finds no `EmeraldBattleCompat` symbol in either production binary.
- The adapter without a shadow generation returns NOT_BATTLE for every
  field, so the compiled-script image-relative path is byte-for-byte
  unchanged (the G4 regression suite re-proves it).
- Live range count stays 6,377; 0 H ranges.

## 24. Regression battery (brief §28/§31)

| Suite | Result |
|---|---|
| G4 script state (5 capture + 5 restore, 2 generations) | PASS |
| G4 cross-process (nested interior, RAM return, trainer, vaddress 8/8) | PASS |
| G4 fault matrix | 31/31 |
| Generic cross-restart state suite (TESTS 1-8, A/B/T/M/R/S/I) | PASS |
| G6 MEVENT | PASS |
| H1 analyzers + oracle (7,549) | PASS (spans 2,089, instructions 15,093) |
| H2 generator --check / staging 54 gates | PASS / 54/0 |
| H3 generator --check | PASS |
| Battle module loader (pack 23,069, tamper triad) | 8,346 checks, 0 failures |
| Pack determinism (gen3-pack-build --check) | PASS |
| H3 state (nested blocking + variants) | PASS |
| H3 fresh-process cross-restart | PASS (bases differ) |
| H3 fault matrix | 16/16 |
| H3 sanitize (ASan/UBSan) | PASS (create/load/faults clean) |
| Runtime loader | 65,754 checks (range index 6,377) |
| Trainer native compat production | PASS (236 front/back + 1,759 battle slots) |
| Trainer native compat sanitize | 8,377 checks |
| Native world real / render proof / neighborhood | 5,570/0 · 3,631/0 · 319/0 |
| Script compat / sanitize / module loader | PASS (isolation sweep clean) / PASS / 1,938 checks, 0 failures |
| Script faults | 21/21 |
| Native asset isolation | 25,165 ok, 0 failed (5,819 ROM_BASE_ONLY) |
| R13-B leaf runner | 7/7 |
| Audio leaf runner | 3/3 |
| Real tables | 16,311 checks |
| Session fingerprint / resource ranges / import / LZ / ROM-base / tileset / layout / object event / trainer compat | PASS · PASS · 155/0 · 144 checks (sanitized detect_leaks=1) · 140 checks · PASS · PASS · PASS · 8,377 checks |

## 25. Forced builds (brief §32)

| Build | H2 baseline | H3 | Delta |
|---|---:|---:|---:|
| release (`-B`) | 23,307,480 B | **23,320,512 B** | **+13,032 B** |
| `DINFO=1` (`-B`) | 36,021,856 B | **36,040,560 B** | **+18,704 B** |

Both `--verify-game-data` runs report
`f3ae088181bf583e55daf962a92bb46f4f1d07b7` (exit 0); release carries
zero `.debug_*` sections, DINFO 7. The seam contributes 0 B to the
production link (no `EmeraldBattleCompat` symbol in either binary); the
adapter + walker hooks are the only linked additions (adapter object:
~7.9 KB text, measured via `nm`/`size` on the release binary).

## 26. H4/H5/H6 prerequisites (brief §33)

- **H4 (anim + field-effect live cutover):** exact state slots =
  `sBattleAnimScriptPtr` + `sBattleAnimScriptRetAddr` (2 sidecar
  records); capture policy = full support (frame-wait parks the IP at a
  command start, proven here); range publication = 1 anim + 1 FE arena
  range; FE needs no state support (no persistent IP).
- **H5 (battle live cutover):** current IP + 8 stack + 8 callback
  (engine) + 4+4 selection/palace slots, all proven here including the
  nested fresh-process gate; production must additionally add the
  read-only surface accessors (BindLiveLayout), widen the three
  sTrainerBattle* continuations to accept H targets, and republish the
  routing tables.
- **H6 (AI + contest live cutover):** capture policy = relocate the
  quiescent stale IP (never execute); no refusal needed; the shared
  gAIScriptPtr slot accepts both AI families (proven); contest stack
  relocation is ready if a future capture path needs it.

## 27. STOP conditions (brief §36)

No STOP was triggered: no State-v5 format change, no range-cap rise,
no aggregate-offset dependence, no alias ambiguity, the nested
fresh-process restore and the blocking-command continuation both
succeed, the anim return relocates, AI/contest policies are proven and
tested, the field-effect policy is proven and tested, no production
interpreter changed, the pack is untouched, and the G regressions stay
green. Nothing committed; H4 not begun; R13-I not begun.

## Final pins

- 5 family arenas / 2 layouts / 90,047 B canonical / 2,089 modules
  (2,081 payload + 8 aliases) / 15,861 boundary rows / 2,109 export
  rows;
- 36 H-typed slots / 9 engine-typed slots / max active H records **20**
  of 4,096;
- live ranges 6,377 (0 H); projected dry run **6,382**;
- fault matrix 16/16; G faults 31/31; nested blocking-command fresh-
  process restore green; bases differ; pack 23,069;
- verify-game-data (release + DINFO): `f3ae088181bf583e55daf962a92bb46f4f1d07b7`;
- release 23,320,512 B (+13,032), DINFO 36,040,560 B (+18,704).

# R13-G4 field-script State-v5 readiness report

Date: 2026-08-21  
Scope: **R13-G4 only** — staged execution-state capture/restore readiness;
no live field-VM cutover, range publication, compiled-payload removal, G5,
G6, or R13-H work.

## 1. Result

G4 is implemented with the existing State-v5 file format. Static staged G
pointers use the ordinary 64-byte State-v5 sidecar identity (resource key,
type, schema, representation role, and payload offset). The exact destination
field supplies the validation-only boundary role. Mutable script pointers use
the existing in-band persistent-storage identity and a G4 family registry that
validates buffer kind, captured owner/storage identity, generation, offset,
and instruction boundary.

The hard process-A/process-B gate passes at deliberately different arena
bases and generations. The live production range index remains exactly
**5,854 / 8,192**, with **zero script-family ranges**. The G5 dry run proves
**523** non-overlapping module identities and a projected count of
**6,377 / 8,192**. No additional C range is needed.

## 2. Exact execution-pointer inventory and serialization slices

| Surface | Slots | Native owner / State-v5 slice | G4 rule |
|---|---:|---|---|
| `sGlobalScriptContext.scriptPtr` | 1 | `src/script.c`, game BSS / `GAME_BSS` | `INSTRUCTION_START` |
| `sGlobalScriptContext.stack[0..stackDepth)` | 20 max | same / `GAME_BSS` | `NEXT_INSTRUCTION`; inactive entries scrubbed |
| `gRamScriptRetAddr` | 1 | `src/scrcmd.c`, EWRAM / `EWRAM` | null or exact static-G return boundary |
| `gApproachingTrainers[0..1].trainerScriptPtr` | 2 | `src/trainer_see.c`, common / `COMMON` | trainer instruction start |
| `sTrainerBattleEndScript` | 1 | `src/battle_setup.c`, EWRAM / `EWRAM` | parser's post-command `NEXT_INSTRUCTION` |
| `sTrainerABattleScriptRetAddr`, `sTrainerBBattleScriptRetAddr` | 2 | same / `EWRAM` | encoded continuation `ENTRYPOINT` |
| **Static possible G total** | **27** | four existing slices | no new slice/layout |
| `sImmediateScriptContext.scriptPtr + stack[20]` | 21 prohibited | `src/script.c`, game BSS / `GAME_BSS` | all stopped/null or capture refuses |
| Mystery Event context IP + stack | 21 max | `src/mystery_event_script.c`, EWRAM / `EWRAM` | registered mutable-buffer offsets only |
| `sMysteryEventScriptNativeBase` | 1 | same TU, game BSS / `GAME_BSS` | registered buffer base/offset |

Read-only accessors pin file-local native storage without changing
`ScriptContext`, trainer, or interpreter layouts. Context native callbacks
remain engine-image-relative. The six trainer text pointers remain C-owned.
The active map header and R13-F event graph remain existing F State-v5
surfaces and are not part of the 27 direct G slots.

The forced release symbol map pins the containing native objects as follows
(addresses are evidence for this build, not persisted identities):
`sGlobalScriptContext=0x16e5460` (0xd8 B),
`sImmediateScriptContext=0x16e5380` (0xd8 B),
`sGlobalScriptContextStatus=0x16e5538`,
`gRamScriptRetAddr=0xdbfb74`, `sAddressOffset=0xdbfb6c`,
`gApproachingTrainers=0xda0d18` (0x30 B),
`sTrainerBattleEndScript=0xda275c`,
`sTrainerABattleScriptRetAddr=0xda2754`,
`sTrainerBBattleScriptRetAddr=0xda274c`,
`sMysteryEventScriptContext=0xdbb540` (0xd8 B), and
`sMysteryEventScriptNativeBase=0x16e4f90`.

## 3. Adapter and transaction design

`emerald_script_state.c` is a narrow family adapter registered at the two
existing State-v5 pointer phases:

1. capture preflight validates Context1 bounds/IP and rejects any Context2
   transient state before allocation or output;
2. an exact field address selects its boundary policy;
3. the G3 reverse index uniquely maps a staged pointer to module key + payload
   offset and proves bytecode segment and boundary;
4. the ordinary State-v5 record is emitted and the serialized pointer bytes
   are zeroed;
5. load resolves every record against the current generation into temporary
   slice buffers, validates all dynamic fields, and only then commits slices.

There is no script-specific file, tag, header, version, or sidecar layout.
Generic walker changes are limited to weak family callbacks around its
existing capture/resolve transaction. When the G3 seam is absent (the G4
production binary), the adapter deliberately falls through to the pre-G4
engine-image-relative path for compiled scripts. The focused harness links
the strong G3 seam and exercises the staged paths.

Static lookup rejects unknown/stale generations, padding, non-bytecode
segments, operand interiors, past-end offsets, missing keys, wrong
type/schema/role, and wrong destination-boundary roles. `ENTRYPOINT` now
requires a generated export rather than merely any decoded instruction.

## 4. Boundary-table closure found during G4

The G3 report described the eight 692-byte ordinary field-script
mystery-gift modules as opaque because its supplementary walk was bounded to
the main `script_data` section. The opcode grammar already contained the full
virtual-address family (`0xB8..0xBF`); no grammar or format extension was
needed. G4 changed the supplementary walk to the exact sparse union of each
module's owned bytecode addresses.

Regeneration is deterministic and yields **52,042 instruction boundaries**
(+228) and **1,423 opaque bytes** (-692). All eight gift modules now have
usable instruction boundaries. The G1 extractor remains byte-identical at
its 12 artifacts, and G2 `--check` remains byte-identical at its generated
surface.

## 5. Mandatory fresh-process proofs

The dedicated runner launches distinct executable processes. Process A
stages generation 1 and writes five State-v5 files. Process B stages once,
then replaces it with generation 2 before loading. A recorded passing run was:

```text
G4-CREATE arena=0x3b9702c0 size=210880 generation=1 records=15 projected=6377
G4-LOAD   arena=0x3fdc8220 size=210880 generation=2 nested-return=ok dynamic-vaddress=8/8
```

Host pointer values are deliberately excluded from parity; the runner asserts
that both the arena base and generation differ.

### 5.1 Nested interior Context1 — hard gate

The staged Context1 fixture has an interior nonzero current IP at `RETURN`,
`stackDepth == 2`, and two nonzero interior return offsets. Capture emits
module identities for only the two active frames; planted garbage in
`stack[19]` is scrubbed and emits no record. Process B proves the same three
module/offset identities and boundary roles, executes the exact next opcode,
pops both frames in native order, and lands at the exact final canonical
offset. No creator arena pointer is present in serialized pointer fields or
equals a restored pointer.

### 5.2 Dialogue and movement waits

The dialogue fixture captures Context1 at the instruction immediately after
`message` (`0x67`), with the wait callback still engine-image-relative and an
outstanding text pointer in the R13-C arena. Process B restores the exact next
G opcode, callback identity, and C text byte/offset.

The movement fixture captures immediately after `waitmovement` (`0x51`). The
G IP relocates, the callback remains native, and the planted movement identity
round-trips as a B-owned scalar. No B movement resource is cut over.

### 5.3 F-to-G provenance and trainer continuations

Staged synthetic entry paths cover NPC/object, corrected coordinate event,
BG/sign, and a map conditional-dispatch target resolved through a G3 routing
row. Object/coord and map-dispatch/BG pairs are restored through fields whose
policy is exact `ENTRYPOINT`. Live R13-F structures are never rebound.

Both approaching-trainer slots, the parser end pointer, and A/B explicit
continuations relocate. Trainer IDs remain byte-identical; six trainer text
pointers restore through C. Negative coverage includes data-segment
continuations, non-export continuations, stale generation, missing key, wrong
schema/role, and past-end offsets. No battle-script/H ownership is entered.

### 5.4 RAM script and dynamic Mystery Event

The RAM fixture runs Context1 from captured mutable `SAVE_RAM_SCRIPT` storage
at a nonzero offset, persists a static G `gRamScriptRetAddr`, restores the RAM
IP relative to recreated storage in process B, executes `RETURN`, and lands at
the exact static module/offset. RAM bytes never become immutable G resources.

The separate Mystery Event VM is registered as
`MYSTERY_EVENT_BUFFER`; its IP, two active stack entries, and native base
restore relative to the recreated buffer. The 17-op VM/table/downloaded bytes
remain dynamic and are not G resources.

## 6. Stable virtual-address anchor

G4 provides a pointer-free persisted representation for the future
`sAddressOffset` replacement:

```text
encodedVirtualBase:GbaAddr
bufferKind + capturedOwnerStorageId + bufferGeneration
liveBaseOffset:u32
```

It stores no host pointer or creator-process delta. Fixtures cover
`setvaddress`, `vgoto`, `vcall`, `vgoto_if`, `vcall_if`, `vmessage`,
`vbuffermessage`, and `vbufferstring`: after restart each computes
`encodedTarget - encodedVirtualBase` and resolves the same buffer-relative
byte/opcode. Underflow, out-of-bounds, stale generation, wrong owner,
reserved/stale-anchor data, and ambiguous overlapping buffers refuse. Live
`sAddressOffset` and all live vaddress handlers are unchanged; G5 owns their
atomic switch. Capture preflight explicitly refuses a legacy nonzero
`sAddressOffset`, so no creator-process delta can enter a G4 state; successful
staged vaddress cases carry the stable anchor above. This refusal remains
until G5 can switch the live handler and buffer registration atomically.

## 7. Refusal matrix and all-or-nothing behavior

The focused fault runner passes **32/32** capture, resolve, boundary, dynamic,
and generation checks. It covers Context1 depth/null IP/invalid return and
inactive-stack scrubbing; four Context2 transient variants; pointer outside,
one-past, padding/hole, operand interior, and static-data targets; trainer
continuation type/boundary; unknown and out-of-bounds mutable buffers; missing
key, wrong schema/role, invalid resource offset, non-export entrypoint;
vaddress underflow/OOB/stale generation/wrong owner/stale anchor, legacy
nonzero `sAddressOffset` host-delta capture; overlapping
dynamic containment; old-generation pointers after replacement; and missing
generation publication.

State-v5 already resolves all sidecars into temporary slice images and aborts
before `RestoreRuntimeBytes` on the first failure. The G4 dynamic validation
also runs against those temporary images before commit. Existing corrupt
sidecar tests (tag, offset, key, role, schema, count, duplicate field,
reserved data, truncation, and version policy) remain green, proving no
partial live stack patch.

## 8. Capacity and future range proof

- live ranges after every G4 staged capture/restage: **5,854 / 8,192**;
- live production G ranges: **0**;
- projected G5 ranges: `5,854 + 523 = 6,377 / 8,192`;
- all module spans/keys/schemas validated, sorted, and non-overlapping;
- routing-only zero-byte catalog identities overlap no bytes in the G4 dry
  run; G5 must materialize their routing span policy before publication;
- maximum static G sidecars: **27**;
- maximum accompanying trainer C text sidecars: **6**;
- dynamic IP/stack identities: existing in-band mutable-storage records, not
  resource sidecars;
- therefore the field-execution resource-sidecar maximum is **33 / 4,096**.

No range or sidecar cap changes.

## 9. Test classification and focused runners

- Real production state: the native symbol/slice inventory and generic
  State-v5 walker/transaction, plus unchanged compiled-script fallthrough.
- Staged synthetic state: exact native `ScriptContext` and trainer-pointer
  layout clones driven through the real walker and G3 seam.
- Dynamic-buffer state: captured mutable clone storage with explicit stable
  owner/generation registration.

Focused entry points:

```text
tests/run_emerald_script_state.sh
tests/run_emerald_script_state_cross_restart.sh
tests/run_emerald_script_state_faults.sh
tests/run_emerald_resource_state.sh g4-sanitize
```

Counts: **5 capture cases, 5 restore cases, 32 fault/identity checks, two
forced script generations/bases**, plus all eight virtual-address operations.

## 10. No-live-change proof

- `ScriptReadPointer` and every live opcode handler are unchanged.
- Live `gStdScripts` and all R13-F bindings are unchanged.
- Live `sAddressOffset` semantics are unchanged; only out-of-band persistence
  helpers and fixtures exist.
- The runtime loader does not initialize `EmeraldScriptCompat`.
- The production Makefile still excludes the G3 seam/generated inventory;
  only the weak G4 adapter is linked.
- No script range is registered and the live count remains 5,854.
- Compiled field scripts and the G resources' current ownership policy remain
  live; no resource is flipped to `ROM_BASE_ONLY`.
- No G5/G6 or R13-H consumer was changed.

## 11. Regression and build results

The following were rerun during G4 (final results are recorded without
changing their pre-G4 ownership expectations):

- G1 extractor `--check` — PASS, 16,704 operands / 207,330 B;
- G2 generator `--check` — PASS, 523 modules / 812 segments / 7,683 exports;
- G2 three-way verifier — PASS, 16,704 total relocation sources;
- G3 compat — PASS, 7,141 checks with 52,042 boundaries;
- G4 cross-process state — PASS, 5/5 capture + 5/5 restore;
- G4 faults — PASS, 32/32;
- existing resource State-v5 cross-restart/corrupt matrix — PASS.

The wider battery also passes: runtime loader (65,745 checks), resource
ranges, native-state self-test, trainer compat/production, layout, object
event, tileset, real tables, native world real/render/neighborhood, and native
asset isolation (25,165 checks). The G3 21-fault matrix and the resource
import/LZ/ROM-base/trainer sanitizer suites pass. G4 ASan/UBSan is clean with
the established process-lifetime R6 session excluded from leak detection.

The required G3 sanitizer exposed a pre-existing R13-F host-alignment bug:
12-byte connection rows made the following `struct MapConnections` start at a
4-byte address. G4's regression closure adds only zero inter-block padding via
`NativeConnectionBlockSize`; wire bytes, schemas, range count, F script
bindings, and consumers are unchanged. The affected sanitizer is rerun clean.

That alignment correction changed allocator reuse and deterministically
exposed a second pre-existing cleanup bug in the runtime-loader fault loop: a
refused Frontier AUX republish could orphan the preceding mon-set/wild range
generation after replacing its two remembered bases. Frontier clear now
removes the two exact arena resource keys from the range index, including an
older refused generation. The isolated runtime-loader rerun passes all 65,745
checks; Frontier schemas, payloads, tables, and production behavior are
unchanged.

Forced linux64 builds:

| Build | G3 | G4 | Delta | Inspection |
|---|---:|---:|---:|---|
| release (`-B`) | 22,719,576 B | **22,737,376 B** | **+17,800 B** | not stripped; zero `.debug_*` sections |
| `DINFO=1` (`-B`) | 35,619,632 B | **35,647,184 B** | **+27,552 B** | `with debug_info`; 7 expected debug sections |

The release `emerald_script_state.o` is 11,258 B text + 968 B BSS =
**12,226 B**. The G3 seam/table contribute **0 B** to the production link.
Both release and DINFO `--verify-game-data` runs report
`f3ae088181bf583e55daf962a92bb46f4f1d07b7`; `nm` finds no defined
`EmeraldScriptCompat` symbol in either binary.

## 12. Exact G5 prerequisites

All G4-owned hard prerequisites are green: nested two-frame fresh-process
relocation and return sequence, RAM return, trainer continuations,
dynamic/vaddress anchor, Context2 refusal, stale-generation refusal, unchanged
State-v5 format, 5,854 live ranges, 6,377 projected capacity, and no live VM
consumer change.

G5 must still perform its own atomic responsibilities: link and initialize the
seam, materialize/publish all 523 ranges, switch the central reader/opcodes and
vaddress anchor, bind live `gStdScripts` and F consumers in one transaction,
and add true live-execution differential tests. None of that began in G4.

**G5 hard-gate status: READY from the G4 side; G5 not started.**

## 13. Files changed

Core integration and native accessors:

- `Makefile_pc`;
- `include/emerald/resources/emerald_script_compat.h`;
- `include/emerald/resources/emerald_script_state.h` (new);
- `src/emerald/resources/emerald_script_compat.c`;
- `src/emerald/resources/emerald_script_state.c` (new);
- `src/platform/native_state.c`;
- `src/script.c`, `src/scrcmd.c`, `src/battle_setup.c`, and
  `src/mystery_event_script.c`.

Generator and generated boundary inventory:

- `tools/gen3_resources/script_family/gen_script_family.py`;
- `src/emerald/resources/script_native_table.generated.c`;
- the eight `resources/extraction/emerald/bpee01/script/modules/meta/`
  `mystery-gift/gift_*.toml` modules.

Tests and runners:

- `tests/emerald_resource_state_test.c`;
- `tests/emerald_script_compat_test.c`;
- `tests/emerald_script_harness_stubs.c`;
- `tests/run_emerald_resource_state.sh`;
- `tests/run_emerald_script_state.sh` (new);
- `tests/run_emerald_script_state_cross_restart.sh` (new);
- `tests/run_emerald_script_state_faults.sh` (new).

Regression closure and documentation:

- `src/emerald/resources/emerald_map_compat.c`;
- `src/emerald/resources/emerald_frontier_compat.c`;
- `docs/R13G_FIELD_SCRIPT_MIGRATION_PLAN.md`;
- this report (new).

The pre-existing dirty `android/SDL2` submodule worktree was not modified.

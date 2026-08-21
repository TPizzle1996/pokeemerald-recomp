# R13-G3 Script Compat Shadow Report

**Stage:** R13-G3 — EmeraldScriptCompat shadow staging + typed resolver
seam (executed per `docs/R13G_FIELD_SCRIPT_MIGRATION_PLAN.md` §17 G3 wave).
**Status:** COMPLETE (STOP after G3; no G4, no commit until review).
**Qualified truth:** pret reference build + retail-matching ROM BPEE01 Rev 0
(SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7), unchanged from G2.

---

## 1. Files changed

**New (seam + inventory + tests):**
- `include/emerald/resources/emerald_script_compat.h` — the seam contract
  (shadow-mode, REFUSE-class, transactional; query API surface).
- `src/emerald/resources/emerald_script_compat.c` — the seam
  implementation (~1,450 lines): phase-1 validation (structural table
  gates, pack surface, snapshot resolution, typed target resolution,
  parity oracle), phase-2 staging (arena, source index, staged
  gStdScripts, staged F rebind plan, raw-value proofs), phase-3 shadow
  commit; query APIs.
- `include/emerald/resources/script_native.generated.h` — the generated
  C inventory contract (7.5 KB): count pins, enums, row structs.
- `src/emerald/resources/script_native_table.generated.c` — the
  generated C inventory (4.75 MB / 111K lines): 523 module rows, 812
  segments, 7,683 exports, 15,874 module relocs + 830 routing relocs,
  12,016 dynamic targets, 51,814 instruction boundaries, 11 std rows,
  3,501 F rows, 38 marts, 18 RAM rows + 1 allowlist, 3 bridges (with
  embedded qualified-ROM bytes), 17,884-string pool.
- `tests/emerald_script_compat_test.c` — the focused suite (7,1xx
  checks).
- `tests/emerald_script_fault_test.c` — the fault-matrix driver.
- `tests/emerald_script_compat_harness.h` — shared setup (pack ->
  catalog -> ROM_BASE snapshot -> R6 loader -> sibling seams).
- `tests/emerald_script_harness_stubs.c` — gStringVar4 + the 3 bridge
  symbols for the harness link.
- `tests/fault_script_table.py` — the generated-table mutator (20
  named faults).
- `tests/run_emerald_script_compat.sh` — main runner (incl. the
  isolation sweep gate).
- `tests/run_emerald_script_compat_sanitize.sh` — ASan/UBSan variant.
- `tests/run_emerald_script_faults.sh` — the 21-case fault matrix.

**Modified:**
- `tools/gen3_resources/script_family/gen_script_family.py` — G3
  enrichment + C-table emission (the graph extraction is untouched;
  every G1/G2 pin gate still runs and still passes): instruction
  boundaries from the G1 BFS walk + a supplementary walk seeded from
  every script entry point; target sub-kind/payload-offset
  enrichment; the dynamic encoded-GBA target index; per-meta
  `instruction_count`/`target_kind`/`target_payload_offset`/
  `[[boundaries]]` fields; the two new C files.
- `tools/gen3_resources/script_family/field_script_extractor.py` — the
  BFS now records instruction sizes and returns starts+sizes (8 lines).
- `resources/extraction/emerald/bpee01/script/modules/meta/*/*.toml`
  (523 files) — the enrichment fields above; every other line is
  byte-identical to the G2 generation (`--check` green).
- `docs/R13G_FIELD_SCRIPT_MIGRATION_PLAN.md` — G3 confirmed facts.

**Untouched by G3 (proven):** `emerald_resource_ranges.c`,
`ScriptReadPointer` and every interpreter handler, `sAddressOffset`,
`gStdScripts` (compiled form), the R13-F map/event structures, the
production pack (20,988 entries, byte-identical), the 523 G2 payloads,
the State-v5 format and the range registration path.

## 2. Arena layout

One deterministic candidate arena per generation:

- 523 module spans in **bytewise canonical-id order** (the manifest
  order), each 16-aligned; payload bytes contiguous per span.
- Total arena: **210,880 B** (207,330 B payload + alignment padding).
- The 56 routing-only modules carry **zero-size spans** (identity
  only; their dispatch-table bytes stay ROM-resident through G3).
- Bytes are copied from the pack entries (digest-validated in phase 1)
  and proven **byte-identical to the pack payloads** (memcmp over all
  467 entries in the test); **no operand is ever patched** (every
  operand's raw bytes are re-proved against the recorded encoded
  values during staging).
- Per-module provenance: GBA region start, primary symbol, digest,
  schema, segment/export/reloc/boundary ranges, kind, embedded flag.

## 3. Index designs (plan §22)

| Index | Rows | Ordering | Lookup |
|---|---|---|---|
| Module spans (per generation) | 523 | by arena base | O(log n) bsearch |
| Source relocation index (per generation) | 15,874 | by operand address | O(log n) exact match |
| Routing source index (table) | 830 | by GBA source | O(log n) |
| Dynamic target index (table) | 12,016 | by (encoded GBA, class) | O(log n) |
| Exports | 7,683 | per module by payload offset | O(log m) |
| Instruction boundaries | 51,814 | per module by payload offset | O(log m) |
| Bundle-member index (text seam) | 7,953 | by canonical label | O(log n) |
| Bridges / marts / allowlist | 3 / 38 / 1 | fixed order | linear (bounded) |

No lookup path scans the 16,704 relocation rows or the 523 modules
linearly; the hot paths (operand resolution, encoded-target
resolution, reverse containment) are all binary searches. The only
linear scans are the 3-bridge and 38-mart tables and a per-module
export-name scan in `ResolveBinding` (the canonical binding API for
tests/G4; the hot `ResolveOperand` path is O(log n)).

## 4. Source index + target resolution counts

- **16,704 indexed sources** = 15,874 module rows (live operand
  addresses) + 830 routing rows (GBA sources, ROM-resident through
  G3). Every operand: width 4, inside its module payload, non-
  overlapping with the next operand, raw staged bytes == recorded
  encoded value.
- **Target sub-kind partition** (measured, pinned in the generator):
  SCRIPT_PAYLOAD 8,198 + SCRIPT_ROUTING 7 + SCRIPT_BRIDGE 3 = 8,208;
  TEXT_BUNDLE_MEMBER 6,187 + TEXT_LABEL 20 = 6,207;
  MOVEMENT_RESOURCE 2,009 (MOVEMENT_BRIDGE 0 — the 3 bridges are
  SCRIPT-class operands); MART_TABLE 38 + DISPATCH 198 + BRAILLE 26 =
  262; RAM_HOST 18.
- **Dispositions across all 16,704 rows:** 8,236 staged-arena
  (script payload + marts), 8,216 sibling-seam (text + movement), 3
  compiled bridge, 18 host RAM (gStringVar4), 231 deferred (198
  dispatch + 26 braille + 7 routing-class script targets).
- **Root/interior pin: 95 / 8,113** (region-relative target offset 0
  vs nonzero over the whole SCRIPT class) — gated in phase 1.
- **Parity oracle (§18): 16,704/16,704 checked, 0 mismatches** — every
  relocation's encoded operand resolves through the dynamic index to
  the identical canonical identity (class/kind/key/label/offsets);
  run on BOTH staged generations with identical canonical aggregates
  (the test's cross-base checksum) — pointer values differ, identities
  never. No host pointer equality is ever used as proof.

## 5. Typed target resolution

- **SCRIPT (8,208):** target module exists, the export at the payload
  offset exists with the matching boundary kind, the live pointer
  lands exactly in the staged span. Routing-class and bridge targets
  carry their identities with DEFERRED/COMPILED_BRIDGE dispositions.
- **TEXT (6,207):** bundle members resolve (canonical label ->
  `kTextBundleIndex` -> bundle id -> the published label bytes, size
  cross-checked); the 20 gift labels resolve per-label. **Zero
  missing text targets.** No text bytes are copied or republished.
- **MOVEMENT (2,009):** every edge resolves to the R13-B published
  surface; the 3 recomp-local cases resolve as named compiled bridges
  (live symbols, never dereferenced by the seam; their qualified bytes
  are embedded in the table for G5's byte-identity proof). No movement
  live cutover, no movement payload removed.
- **STATIC_DATA (262):** the 38 marts resolve to their typed
  static-data segments with the ITEM_NONE sentinel validated on the
  staged bytes; the 198 dispatch targets and 26 braille edges stay
  DEFERRED (routing bytes ROM-resident; braille labels await their C
  handoff — post-G2 work, unchanged).
- **RAM_DATA (18):** all 18 resolve to the single allowlisted host
  symbol (EWRAM 0x02021fc4 = gStringVar4, 1,000 B) — permission comes
  from the allowlist row, never from address arithmetic.

## 6. Boundary index + reverse containment

- Boundary rows = the **G1 census walk** (proven roots, break on
  unknown opcode/terminal) plus a **supplementary walk** seeded from
  every script entry point (all SCRIPT_TARGET targets + F entries +
  gStdScripts targets + non-data exports), with the same grammar and
  with chains stopping before any census-decoded start. 51,814
  instructions.
- **Opaque zones (2,115 bytes):** bytecode-class bytes neither walk
  decoded — data islands and dynamically-reached code (the G1 census
  models them the same way; 53 census operands live there). Export
  identities resolve; INSTRUCTION_START/NEXT_INSTRUCTION queries
  refuse. **The 8 mystery-gift modules' 692 B are entirely opaque** —
  their virtual-address opcode family (0xE3–0xFF) is not in the
  generated grammar; adding it is an explicit G4 prerequisite.
- Export position classes: 466 offset-zero roots, 39 mart-span data
  positions (incl. trainerbattle inline slots the G1 symbol model
  labels), 19 opaque positions, the rest decoded instruction starts.
- **Reverse containment:** every staged pointer resolves to exactly
  one module + payload offset + segment kind (O(log n) over the span
  index); span-end, arena-hull, inter-span and stale-generation
  pointers all refuse. Cross-module ambiguity is impossible (spans are
  disjoint by construction and gated).

## 7. Staged surfaces (shadow only)

- **gStdScripts:** 11 staged candidate rows — every slot maps to its
  G2 export identity (Std_ObtainItem … Std_MsgboxPokenav), the staged
  pointer is an arena pointer (never a compiled GBA payload), slots
  verified in order. The live compiled `gStdScripts` is untouched
  (G5 owns publication).
- **F inbound:** 3,501 staged rebind-plan rows — 518 map-scripts
  (routing boundary, DEFERRED, full identity: owner map, provenance
  GBA, module/export/region offset) + 2,163 object + 289 coord + 531
  bg (ENTRYPOINT boundaries, STAGED arena pointers). **The live R13-F
  structures are never mutated.**

## 8. Generation lifecycle + failure matrix

- Initial staging, restage at a **different arena base** (the old
  generation stays alive until the new one fully validates, so the
  bases are guaranteed distinct), failed replacement preserves the old
  generation, stale-generation pointers refuse on every query. Each
  generation carries a monotonic identity for G4/G5.
- **Fault matrix (21 cases, all refused with the exact pinned
  status):** seg-overlap / seg-gap / seg-kind → SEGMENT_INVALID;
  export-outside / export-dup → EXPORT_INVALID; reloc-overlap →
  RELOC_INVALID; reloc-raw / reloc-class / text-missing /
  export-missing → TARGET_UNRESOLVED; boundary-dup →
  BOUNDARY_INVALID; mart-sentinel → SEGMENT_INVALID; f-missing /
  std-missing → EXPORT_INVALID; ram-bad / dyn-gap / module-digest /
  module-arena / f-routing-object → TABLE_MISMATCH; bridge-bad →
  UNEXPECTED_COUNT; plus the pack-variant RESOURCE faults in the main
  suite (missing module / extra module → UNEXPECTED_COUNT, wrong
  schema / flipped payload → TABLE_MISMATCH) and the deterministic
  **partial-allocation failure** (test-only alloc-fail hook) →
  OUT_OF_MEMORY with no generation left behind. No fallback anywhere.

## 9. State-v5 invariant (plan §23)

The seam never touches the range index (no registration call exists
in the TU; the isolation sweep gates it). The test proves: after full
staging, restage, clear and restage, the index holds **exactly 5,854
ranges** (the G2-pinned post-F figure) with **zero ranges
intersecting the shadow arena** and **zero script-family ranges**.
The arena is intentionally invisible to the range walker — no
serialized pointer can ever point into a shadow generation.

## 10. Runtime behavior invariant (plan §24)

- The seam TU references none of the live execution surfaces
  (ScriptReadPointer, sAddressOffset, gStdScripts, the F structures,
  HostResolveGbaAddr) — the runner's grep sweep gates this on every
  run.
- The staged bytes are byte-identical to the pack payloads (no
  patching); the seam publishes nothing outside its private shadow
  state (the range-index proof above).
- The seam is not linked into the game binary in G3 (Makefile_pc
  untouched): the live VM, gStdScripts and the R13-F pointers keep
  their compiled forms by construction. G5 links the seam and wires
  publication.

## 11. Pack / resource invariant (plan §25)

Production pack: **20,988 entries, byte-identical** (no entry added,
removed, or altered). The 523 G2 payloads are untouched. The meta
enrichment is additive metadata only; `gen_script_family.py --check`
regenerates all 1,059 artifacts byte-identically, and the G1
extractor's own `--check` stays green (12 artifacts).

## 12. Regression battery (plan §27)

| Suite | Result |
|---|---|
| G1 extractor `--check` | PASS (12 artifacts byte-identical) |
| G2 generator `--check` | PASS (1,059 artifacts byte-identical) |
| `verify_three_way.py` | PASS (16,704 relocs consistent) |
| `run_emerald_script_module_loader.sh` | PASS (1,938 checks; 523/523 ownership) |
| `run_emerald_script_compat.sh` (G3) | PASS (7,141 checks) |
| `run_emerald_script_faults.sh` (G3) | PASS (21/21 faults) |
| `run_emerald_script_compat_sanitize.sh` (G3) | PASS (ASan/UBSan clean; the R6 loader's process-lifetime session is excluded from leak detection by design) |
| `run_emerald_runtime_loader.sh` | PASS (65,745 checks) |
| `run_emerald_resource_ranges.sh` | PASS |
| `run_emerald_resource_state.sh` | PASS (cross-restart) |
| `run_emerald_script_module_loader.sh` / `trainer_native_compat(_sanitize)` / `session_fingerprint` / `layout` / `object_event` / `tileset` | PASS |
| `run_emerald_native_world_real.sh` / `render_proof` / `neighborhood` / `real_tables` | PASS |
| `run_emerald_trainer_native_compat_production.sh` | PASS (236 slots byte-identical) |
| `run_emerald_native_asset_isolation.sh` | PASS (25,165 checks; fresh binary) |
| `run_emerald_resource_import_sanitize.sh` / `resource_lz_sanitize.sh` / `rom_base_provider_sanitize.sh` | PASS |
| fresh release + DINFO builds + `--verify-game-data` | PASS (see §13) |

**G2-carryover harness fixes (pre-existing, found by this battery):**
the native-world-real, render-proof and trainer-production suites still
pinned the pre-G2 pack count (20,501) and passed static catalogs that
did not declare the script family. Updated: the 20,988 pin, and each
runner now derives a pack-matched script catalog (467 embedded ids;
the 56 routing-only catalog identities have no pack entry and stay out
of the session's construction catalog, exactly like the R6 loader's
pack-derived catalog). No production code changed.

## 13. Fresh build validation (plan §28)

Release linux64 and DINFO linux64 rebuilt with forced rebuilds (the
known `Makefile_pc` `.SECONDARY` behavior): release **22,719,576 B**
(`not stripped`, no debug sections), DINFO **35,619,632 B** (`with
debug_info`), and `--verify-game-data` reports the qualified ROM
f3ae088181bf583e55daf962a92bb46f4f1d07b7. `nm` on the release binary
finds **zero `EmeraldScriptCompat` symbols** and the `Makefile_pc`
change is a two-line `filter-out` that keeps the new files out of the
link entirely — the game binary links exactly the G2 object set (plus
the G2 working tree's uncommitted text-seam unregister fix), so G3
adds nothing to the game binary. The G3 seam is compiled and tested by
the dedicated harnesses only.

## 14. G4 prerequisites (exact)

1. Add the **virtual-address opcode family** (0xE3–0xFF) to the
   generated grammar so the 8 mystery-gift modules' 692 B decode
   (currently opaque) — needed before gift-module IP/return capture.
2. Capture/restore walks the 27 static G pointer slots (§9.1) through
   `EmeraldScriptCompat_ReverseResolve` + `ValidateBoundary` with the
   generation id stamped; Context2 inactivity enforced.
3. Replace the captured `sAddressOffset` delta with the stable
   dynamic-buffer anchor; `ResolveEncodedTarget` is the resolver for
   dynamic RAM scripts.
4. Register the 523 module spans with the range index only at G5's
   atomic publication (post-G count 6,377, or gated 6,378 if C proves
   a new family range) — never from the shadow generation.
5. Stage the routing segments (or publish them from the F side) before
   the map-scripts rebind rows can carry live pointers; publish
   gStdScripts and the F rebind atomically; trainer-argument loader
   resolves trainerbattle text/continuations through the typed
   targets (no battle-script target exists in the closed class set).
6. The differential VM oracle (12-handler/central-reader budget)
   gates the G5 cutover; G4's nested-call save/restore STOP gate
   (plan §18) remains the highest invariant.

## 15. STOP conditions

All G3 stop conditions hold: the 523 resources stage exactly; all
16,704 relocations resolve; no target class required weakened typed
resolution (the sub-kind model is the G2 census's own); live
ScriptReadPointer, R13-F pointers, live gStdScripts, the State-v5
format and sAddressOffset are all untouched; the pack needs no
redesign; no G1/G2 pin was weakened (every pin re-gates green); no
interpreter edit exists in the diff; every R13 regression is green.
**Stopped after G3. Not committed. G4 and R13-H not started.**

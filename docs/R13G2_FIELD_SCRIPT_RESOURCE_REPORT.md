# R13-G2 Field-Script Module Resource Report

**Stage:** R13-G2 — additive Emerald field-script module resources + dependency
completion (executed per `docs/R13G_FIELD_SCRIPT_MIGRATION_PLAN.md`).
**Status:** COMPLETE (STOP after G2; no G3, no commit until review).
**Qualified truth:** pret reference build (d8e405c4f6b48f1faf3b26a3e045f0df2ff3ecb7 +
upstream symbol rename b89722500), retail-matching ROM **BPEE01 Rev 0
(SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7)**.

---

## 1. Resource surface (section 3 pins, all confirmed)

| Pin | Value | Status |
|---|---|---|
| Script module resources | **523** (map 468 / common 47 / mystery-gift 8) | confirmed |
| G-owned bytecode | **207,330 B** = 206,638 main + 692 mystery gift | confirmed (qualified truth; the former 206,283-B figure was recomp-era bookkeeping) |
| Relocations | **16,704** = 15,874 module + 830 routing sidecar | confirmed |
| Class partition | 8,208 script / 6,207 text / 2,009 movement / 262 mart / 18 RAM | confirmed (oracle-gated) |
| Bindings | 95 root + 8,113 interior | confirmed (G1 convention: module-relative offsets, offset 0 = module root) |
| F inbound | 518 map-scripts + 2,163 object + 289 coord + 531 bg = **3,501**, zero unresolved | confirmed |

### 1.1 Mystery-gift decomposition
The 8 gift modules hold 692 B of bytecode (gift-mystic-ticket, gift-old-sea-map,
gift-pichu, gift-stamp-card, gift-trainer, etc. — 45–161 B each).

### 1.2 RAM targets (gStringVar4 discovery)
All 18 RAM_DATA_TARGET operands target the **same** EWRAM address
`0x02021fc4` = `gStringVar4` (the 1,000-byte string-variable buffer —
`bufferstring`-family destination). Split pinned **(18 EWRAM, 0 IWRAM)** and
gated; the generator emits `[[ram_targets]]` into the inventory
(`gba_address = 0x2021fc4`, per-operand `source_gba_offset`).

### 1.3 Mart / local data segments
38 mart tables (742 B) emitted as typed `data_kind = "mart-table"` segments,
ITEM_NONE sentinel validated, operand-driven target detection (no name
heuristics); 198 routing dispatch-table targets; 22 braille labels (26 edges,
552 B) → C handoff (`emerald:text/braille/<label>` keys, absent from C's
catalog by design — the oracle verifies their correct absence; naming them
in C is post-G2 work, not started).

## 2. Module payload / manifest / pack integration

- **467 of 523 modules embed payloads** in the production pack; **56
  routing-only modules** (terminator `.byte 0` map-script tables, plan §5)
  carry no payload (their region holds routing-class bytes that stay
  ROM-resident) and remain **catalog/ownership/meta surface only**. The pack
  model rejects zero-length records (`encoded_length <= 0`), so the manifest
  deliberately embeds only the 467 payload-bearing modules
  (`MANIFEST_RECORDS_PIN = 467`).
- 11th manifest+catalog pair: `script/modules/manifest.production.toml`
  (467 records) + `script/modules/catalog.generated.toml` (523 identities)
  joins the ten existing families in `build_d1_pack.sh`.
- Production pack rebuilt: **20,988 entries (64% of the 32,768 cap)**,
  14,753,152 B, `--check` byte-identical.
- **Correction vs earlier bookkeeping:** the interim "20,521 entries" figure
  was measured before the script family's 467 records landed; the pack itself
  is authoritative at 20,988.
- Qualification fix: the text manifest was regenerated in Task #3 (adding the
  20 mystery-gift records) without `--qualification production` → fixture
  qualification → production pack build refused it. Regenerated with the
  pinned provenance string + `--qualification production`; diff vs committed
  = exactly the 20 mystery-gift records, zero other drift.

## 3. Cross-family handoffs

- **Movement (B):** all 2,009 MOVEMENT_TARGET edges resolve to R13-B
  ownership; 3 recomp-local movement bridges remain explicit named compiled
  bridges (12 B): `Route103_EventScript_RivalExitFacingNorth2` @rel 0x10f45
  (7 B), `Ferry_EventScript_DepartIslandBoardSouth` @rel 0x673bb (2 B),
  `Ferry_EventScript_DepartIslandBoardWest` @rel 0x673bd (3 B) — keys
  `emerald:movement/bridge/<slug>`.
- **gStdScripts:** exactly 11 shadow-only binding records (slot span
  0x81dc2a0..0x81dc2cc).
- **Text (C):** 6,207 TEXT_TARGET edges into the 5,207-id text catalog;
  20 mystery-gift labels (2,682 B) handed to C as
  `emerald:text/mystery-gift/<slug>` (catalog records, `sText_*`); 22 braille
  labels named as pending keys.

## 4. Determinism (section 10)

All generators support `--check`; second generation byte-identical/no-op.
Verified in this stage: `gen_script_family.py --check` exit 0 with every pin
green (2,009 movement edges, 11 gStdScripts, 3,501 F inbound, 38 mart tables,
18 RAM targets, 467 embedded records).

## 5. Three-way oracle (section 11) — `verify_three_way.py`

Independent re-verification (does not import the generator): for all 523
modules — ROM slice == payload bytes == manifest digests; every one of the
15,874 module relocs — ROM operand bytes == recorded target address, target
export identity in the target module's exports, offset convention per class
(SCRIPT_TARGET region-relative, MART_TABLE_TARGET payload-relative), bridges
contained in exactly one module span; all 830 routing relocs — operand ==
target; cross-family keys exist in the C text catalog / B movement bindings
(and braille keys are correctly absent). §3 pins gated: 16,704 total, class
partition exact, 56 routing-only, 467 embedded.

Result: **523 modules, 812 segments, 7,683 exports, 16,704 relocs — all
consistent.**

## 6. Loader / provider tests (section 13) — `emerald_script_module_loader_test.c`

Pack-reader harness (resource_core/pack/toml/sha256 TUs) over the real
production pack + committed artifacts, 1,938 checks:

1. pack opens; profile ROM SHA-1 = f3ae0881...; entry count 20,988;
2. all 467 embedded modules: found by canonical name, byte-exact vs the
   committed `.bin`, payload SHA-256 == both manifest digests;
3. all 56 routing-only identities: absent from the pack; no pack script entry
   outside the 523-entry catalog (pack surface == catalog surface);
4. tamper refusal: payload-byte flip → `PAYLOAD_HASH_MISMATCH`; TOC-byte
   flip → refused (key/name integrity); truncated image → `TRUNCATED_HEADER`.

Run script additionally verifies 523/523 ownership records are
`COMPILED_PENDING_MIGRATION` (additive-only; the ownership file's
`[resources.targets]` dotted headers are outside toml.c's subset, so the
state check runs in python).

## 7. State-v5 impact pin (section 14)

**G2 registers NO live script ranges.** Evidence:

1. zero references to the `emerald:script` namespace (or script-family
   paths) in `emerald_runtime_loader.c`, `emerald_resource_ranges.c`,
   `emerald_resource_session.c`;
2. the only G2 runtime-visible changes are the Task #3 C text handoff
   completion and the text seam's unregister fix (§8):
   - **+20 passive inventory records** in `text_native_table.generated.c`
     (kTextNativeResources, schema 15, 2,682 B): count bumps 5,187→5,207
     / 4,824→4,844; the seam pins in `emerald_text_compat.h` move to
     **12,797 labels / 905,839 B** and `text_arenas.generated.c`'s misc
     summary to **6,211 labels / 551,273 B**. The generator's CENSUS
     model is unchanged (12,777 labels / 903,157 B); the seam's
     INVENTORY model counts the 20 gift labels in both representations
     (per-label rows + bundle membership — by design, plan §7.3), which
     is the +20 label / +2,682 B the seam sees. Validation-inventory
     entries only: **no range registration** (the gift rows are schema
     15u passive records);
3. runtime range-index impact measured: the runtime-loader suite's final
   index holds **5,854 ranges, zero of them script-family** (the script
   modules never reach the range index at any session state);
4. regression suites green (below).

## 8. Regression battery (section 16)

Final run (all six suites, exit 0, real exit capture):

| Suite | Result |
|---|---|
| `gen_script_family.py --check` (determinism) | PASS — byte-identical, pins green (2,009+3 movement, 11 gStdScripts, 3,501 F inbound, 38 marts, 18 RAM, 467 embedded) |
| `verify_three_way.py` (three-way oracle) | PASS — 523 modules / 812 segments / 7,683 exports / 16,704 relocs consistent |
| `run_emerald_script_module_loader.sh` | PASS — 1,938 checks, 0 failures; 523/523 ownership COMPILED_PENDING_MIGRATION |
| `run_emerald_resource_ranges.sh` | PASS — all checks passed |
| `run_emerald_resource_state.sh` (State-v5 cross-restart) | PASS — ALL PASSED |
| `run_emerald_runtime_loader.sh` | PASS — 65,745 checks |

**Seam fix surfaced by G2 (in final diff, `emerald_text_compat.c`):** the
initial runtime-loader run failed two checks (frontier E3a-2 refusal baseline
deploy refused, status 13 RANGE_REGISTRATION at stage 'publish'). Root cause:
a latent mark-based text-range unregister. The D1 gameplay font ranges
(type 2/3) registered after the text block but sorted BELOW it (heap reuse
from G2's pack growth shifted the layout), so the unregister's
`TextArenaRangesMatch` block-match failed → fail-closed ORPHANED the 16 text
ranges; the freed text arena's addresses stayed covered, and the next
session's frontier mon-set arena malloc landed inside the orphaned span →
overlap refusal. Fixed by replacing the mark + block-match with
position-independent **base-identity removal** (each of the 16 sub-arena
bases searched in the index and memmove-removed — the same pattern as
`UnregisterMonSetRange`). This also removes a same-process session-restart
robustness bug: previously, ranges sorting below the text block (any family
reusing freed buffer addresses) could orphan the text spans and poison every
later publish. Also fixed in the same pass: the stale runtime-loader
pack-count pin 20,501 → **20,988** (the pre-script-family bookkeeping value).

**Isolation proof:** G2's diff (tools/script_family, resources/script/modules,
text handoff, tests, docs) contains no script-execution or State-v5 runtime
changes; `emerald_resource_ranges.c` is net-untouched (temporary overlap
diagnostics added during root-causing, then fully reverted). The runtime
source edits are exactly: the passive text inventory records + the seam pins
described in §7, and the unregister robustness fix above.

## 9. File inventory (new / changed in G2)

- `tools/gen3_resources/script_family/gen_script_family.py` — the generator
  (523 modules, routing sidecar, f_inbound, marts, RAM allowlist, gStdScripts,
  inventory, ownership, manifest, catalog, meta, bindings, `--check`).
- `tools/gen3_resources/script_family/verify_three_way.py` — §11 oracle.
- `resources/extraction/emerald/bpee01/script/modules/` — manifest (467),
  catalog (523), ownership (523), inventory, routing_relocations (830),
  bindings, meta/ (523), data/ (467 payloads).
- `resources/extraction/emerald/bpee01/script/` — extraction scripts.
- `resources/extraction/emerald/bpee01/text/` — regenerated (20 gift records)
  + `labels/mystery-gift/`.
- `src/emerald/resources/text_native_table.generated.c` (+20 gift records),
  `include/emerald/resources/text_native.generated.h` (count bumps),
  `src/emerald/resources/text_arenas.generated.c` (misc 6,211 / 551,273),
  `include/emerald/resources/emerald_text_compat.h` (12,797 / 905,839) —
  the §7.3 handoff completion.
- `src/emerald/resources/emerald_text_compat.c` — the position-independent
  text-range unregister fix (§8).
- `tools/gen3_resources/pack_build/build_d1_pack.sh`,
  `gen3_pack_build.c` — 11th manifest+catalog pair.
- `tests/emerald_script_module_loader_test.c`,
  `tests/run_emerald_script_module_loader.sh` — §13 harness.
- `games/emerald/base/emerald-bpee01-v1.rpack` — rebuilt (gitignored).

## 10. Out of scope (per brief, not started)

G3 (resolver seam + live publication), R13-H, removing `sAddressOffset`,
repointing R13-F map/event script pointers, flipping to ROM_BASE_ONLY,
changing `ScriptReadPointer` behavior, State-v5 script execution handling.

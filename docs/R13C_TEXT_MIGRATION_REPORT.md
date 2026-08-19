# R13-C: Text Payload Migration — Seam Phase Report

Date: 2026-08-19
Branch: agent/prepare-v0.1.0-alpha (uncommitted; R13-C stops here by directive)

Pins verified this phase:
- ROM (qualified retail-matching reference build): SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`
- Pack `games/emerald/base/emerald-bpee01-v1.rpack`: SHA-256 `a57b51be17f8e94235366f032f48a526f1e6d6d226e38869b03b572a7d47a784`, 12,063 entries

## 1. What R13-C delivered

The text publication seam (emerald_text_compat.c/.h + seven generated text
TUs + the native-only export TU text_native_exports.c) went live for the
eight approved C-side families:

| Quantity | Value |
|---|---|
| Native text resources (inventory pin) | 5,187 |
| Label records | 4,824 |
| Bundle records | 363 |
| Pointer slots (bindings) | 3,118 |
| Skeleton tables | 283 |
| Skeleton fills | 3,335 |
| Published text bytes | 903,157 |
| Family arenas | 16 |
| Live (ROM_BASE_ONLY) resources | 3,119 |
| Deferred (COMPILED_PENDING_MIGRATION) resources | 2,068 |
| Dual-ref/deferred cases | 97 (preserved, all deferred) |

Cutover scope (plan §11 tranches 1–2): battle, move, ability, nature
(skeleton families) + shared, system, match-call, ribbon (pointer-slot
families). Item/pokedex/easy-chat and script-side text remain
COMPILED_PENDING_MIGRATION — **no script-side isolation is claimed**; the
script families' compiled definitions and script operands are untouched by
R13-C.

## 2. The publication seam

- **Deterministic family arenas**: one allocation per publish = header (16
  sub-arena descriptors) + 16 sub-arenas (records sorted by
  (name, isBundleLocal), payloads at zone-relative offsets). Labels resolve
  by resource id (DashNameOf), bundle-local labels by bundle-index name.
- **Transactional validation**: the generated inventory is validated before
  any memory is touched (phase 1a tiling, phase 1b per-row identity, count
  pins, bundle index geometry); slots/skeleton arrays are filled only after
  the arena is published; publication is atomic with fail-restore (a
  previous arena survives a refused republish).
- **No broad ROM interval**: text ranges are per-family-arena spans
  (`emerald:text/arena/<key>`, role COMPAT_OBJECT), not a ROM window.
- **No compiled fallback for live families**: the 3,118 slot definitions
  are `HOST_DATA` (zero-filled, seam-filled); live labels' compiled
  definitions are guard-wrapped `#ifndef NATIVE_LINUX` (a duplicate would
  fail the native link — the fresh builds re-prove this).
- **Kind-0 byte-offset fills (#112)**: fill rows are byte offsets INTO the
  label payload (`gText_123Dot` slices at 0/3/6 of the 9-byte record),
  guarded by `row >= size` and the phase-1a tiling proof for interior
  offsets. No new text identity model was introduced.

## 3. State-v5 TextPrinter.currentChar

- The only proven interior-pointer class is the walker's text-range hit:
  any pointer inside a registered text range captures as a sidecar record
  (key = `emerald:text/arena/<key>`, type TEXT, schema 1, role
  COMPAT_OBJECT, zone-relative rangeOffset) and restores via
  ResolveByKey + GetLabelAtOffset.
- Pointers outside registered ranges keep the opaque in-band fallback
  verbatim; pointers in a hull without identity fail closed.
- Fresh-process relocation proven by the cross-restart suite (TEST T) in
  both default and ASAN builds: the creator's `currentChar` at
  label start + 3 relocates into the loader's fresh arena (bases differ
  across processes), while an opaque compiled pointer round-trips verbatim.

## 4. Isolation proof (R13-C §17–18)

Ownership classification (ownership.generated.toml, generator-derived):

- **3,119 ROM_BASE_ONLY** — live cut-over: 3,118 bound to pointer slots,
  1 (gtext-123dot) reached via kind-0 fills.
- **2,068 COMPILED_PENDING_MIGRATION** — deferred (item/pokedex/easy-chat
  and other non-live families), incl. all 363 bundles.
- **97/97 dual-refs** recorded in the ownership TOML, all inside deferred
  resources — preserved exactly as classified.

Cross-checks (all verified this phase):

- Slot bindings: 3,118/3,118 unique ids resolve in the ownership table;
  **0 unbound, 0 deferred**; every bound symbol has exactly one
  definition in the generated set (2,201 g-scalars + 917 s-scalars +
  79 skeleton arrays incl. `gText_123Dot[3]`); 0 duplicates.
- Skeleton fills: 3,335 rows = 2,674 kind-0 (all 2,174 unique ids live;
  the only nonzero rows are the gtext-123dot slices at 3/6) + 625 kind-1
  (bundle-local, all in the bundle index) + 36 kind-2 (all target real
  skeleton tables; values are compiler-resolved addresses of seam-owned
  HOST_DATA rows).
- No compiled fallback: live compiled definitions guard-wrapped; kind-2
  values point at HOST_DATA arrays; the native link is the completeness
  proof (duplicate definitions cannot link).
- State walker: 16 text spans registered in the shared range index;
  capture/restore exercised by the full cross-restart suite.

## 5. Focused gates (all green)

| Gate | Where |
|---|---|
| Publication test | runtime loader harness (see §7) |
| Skeleton fill test (label-relative) | TestTextSkeletonFills |
| Pointer-slot test (slot == label start) | TestTextSlotPointers |
| currentChar relocation (fresh process) | cross-restart suite TEST T |
| Malformed/missing refusal | status 10 naming the record / status 7 via phase-1b ordering |
| Live-pointer residency + fail-closed clear | loader harness residency proof |

## 6. Seam bugs fixed during R13-C

1. **Fill-zone header overflow** (emerald_text_compat.c publish): the fills
   zone was laid out bytes-relative but guarded against the block-relative
   allocation size — off by exactly `sizeof(struct EmeraldTextArena)`; the
   zone ran one arena header past the block end (ASAN heap-buffer-overflow
   at the first applied-fill store). Fixed by sizing the allocation from
   the exact running layout cursor; guards retained as fail-closed checks.
2. **Phase-1b ordering**: the per-row seen[] identity check now runs before
   the count pin, so a dropped entry is refused with its canonical name
   (TABLE_MISMATCH); the count pin remains the composition backstop.
3. **Audio range unregistration stale mark** (emerald_audio_compat.c,
   found by the R13-C state suite under ASAN): `UnregisterArenaRanges`
   verified the audio block at the registration-time position; seams
   registering after audio whose spans sort below the audio block (the
   text seam's 16 arena spans, in the sanitize allocator layout) shifted
   the block, so the removal was skipped while the spans stayed in the
   index and the re-registration collided with itself (status 10 =
   EMERALD_AUDIO_ERR_RANGE_REGISTRATION at load). Fixed by recomputing the
   block position by base scan at unregister time (symmetric with
   register). Both state-suite variants are green after the fix.
4. **Fresh-build link gap in the #110 unstatic pass**: the generated
   skeleton tables reference three compiled constants the pass left
   `static` in their GBA-compiled sources (`sText_Info`/`sText_Exit` in
   union_room.h, `sWallyLocationData` in pokenav_match_call_data.c);
   the fresh release link refused (undefined references). Resolved with
   a new native-only TU `src/emerald/resources/text_native_exports.c`
   (auto-globbed by Makefile_pc, GBA-excluded by the pret Makefile
   `src/emerald/resources/*.c` filter) carrying the exact GBA content —
   the two GBA sources stay untouched. This is the completeness proof in
   reverse: every symbol the generated tables dereference must exist
   exactly once in the native link.

## 7. Test battery

All 25 scripts green against the final fresh release build (full listing in
§9). The R13-C-fallout harness failures found by the battery — 8 scripts, 6
fix items, all test-harness pins, not generator defects:

1. `run_emerald_real_tables.sh`: the harness links the REAL runtime loader
   (which now publishes the text seam) but lacked the text TUs — linked the
   text seam + generated tables + harness stubs, mirroring the state
   harness; 16,311 checks pass.
2. `run_emerald_resource_import.sh` / `run_emerald_rom_base_provider.sh`:
   guardrail 18 (dependency-creep) flagged the new
   `text_native_exports.c` for its `global.h` include — added to the
   allowlist with the other deliberately platform-coupled TUs.
3. `emerald_resource_import_test.c` (both flavors): the manifest record
   limit pins (8,193 / merged-8,192 refusals) predated the R13-C raise to
   16,384 (12,063 merged records incl. the 5,187-record text family) —
   re-pinned to 16,385 / merged-16,384.
4. `emerald_trainer_native_compat_production.c` (+ script): pack/catalog/
   provider pins 6,876 → 12,063 (added the text family's 5,187) and the
   ROM-base candidate now receives the text family's generated catalog
   (all 5,187 entries, type text/schema 1).
5. `emerald_native_world_real_test.c` / `emerald_native_world_render_proof.c`
   (+ scripts): same pin update (6,876 → 12,063, three pins each) and both
   scripts pass the text family catalog to the ROM-base candidate build.
6. `run_desktop_real_sdl_probe.sh`: the SDL probe compiles the same
   `emerald_resource_state_test.c` as the state harness but lacked the
   text seam TUs (the test references `gText_123Dot` and the
   `EmeraldTextCompat_*` range/arena APIs) — linked the text seam +
   generated tables + harness stubs, mirroring the state harness.

## 8. Fresh builds

| Build | Size | Delta vs R13-B baseline |
|---|---|---|
| release (`make -f Makefile_pc -B linux64`) | 22,176,784 B | +1,067,472 B |
| +DINFO (`make -f Makefile_pc -B linux64 DINFO=1`) | 34,842,144 B | +1,458,512 B |

The release flavor was rebuilt fresh after the DINFO measurement and is the
final binary the §9 build-linked runners ran against.

## 9. Battery results

All 25 battery scripts passed against the final fresh release build
(2026-08-19). Standalone suites (19): native_overworld_renderer_test,
native_overworld_sanitize, layout_compat, native_world_neighborhood,
object_event_compat, real_tables (16,311 checks), resource_import_sanitize,
resource_import, resource_lz_sanitize, resource_lz, resource_ranges,
rom_base_provider_sanitize, rom_base_provider, runtime_loader,
session_fingerprint, tileset_compat, trainer_native_compat_production
(37,921 checks), trainer_native_compat_sanitize, trainer_native_compat.

Binary-linked runners (6, rerun last against the final fresh build):
resource_state (cross-restart suite incl. R13-C TEST T currentChar
relocation + audio arena relocation), native_asset_isolation,
native_world_real, native_world_render_proof, native_state_regression,
desktop_real_sdl_probe. All green.

## 10. Deferred to R13-D/E

- item/pokedex/easy-chat label families (additive seam work).
- Script-side text: script operands, script-family labels, and the 97
  dual-ref sites stay compiled exactly as classified — R13-C does not
  claim script-side text isolation.

**STOP after R13-C.** No commit; no R13-D work begun (per directive).

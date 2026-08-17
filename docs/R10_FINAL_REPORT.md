# R10 Final Report — Resource-Pointer Native Save States (State v5)

## 1. State v4 audit findings

Full audit: `docs/R10_NATIVE_STATE_V4_AUDIT.md`. Highlights:

- Container: magic `0x4E535431` ("NST1"), formatVersion 4, packed header
  {magic, formatVersion, headerSize, totalSize, sectionCount≤16, payloadSize,
  payloadCrc (CRC32 over payload), reserved, frame u64, buildId[64],
  contentFingerprint[64]}, then 13 sections {tag,size,crc} + payload.
- 13 serialized sections: GAME_BSS/REGISTERS/VIDEO_MEMORY/FLASH/EWRAM/IWRAM/
  COMMON/GAME_DATA/FRAMEBUFFER/TASK_SIDECAR/SPRITE_SIDECAR/RTC/BATTLE_SIDECAR.
  Walked (runtimePointers) slices: GAME_BSS, EWRAM, IWRAM, COMMON, GAME_DATA.
- Existing sidecars (TASK/SPRITE/BATTLE) store 8-byte HostPersistentAddress
  records {kind,value} at 4-byte stride; function pointers get stable IDs,
  data pointers get image-relative u32 offsets.
- Normalization: staged-copy walk at 4-byte stride; known function/data
  locations → persistent records; everything else opaque; final
  ValidateNoRuntimeHandles rejects only 0xE/F0000000-encoded registered
  handles. Heap pointers are NOT encodable (HostPointerToPersistentAddress
  returns FALSE outside the image range).
- contentFingerprint was a hardcoded compile-time constant
  ("f3ae088181bf583e55daf962a92bb46f4f1d07b7").
- **Decisive placement finding:** the migrated battle tables live in the
  executable's host `.data` (~0x129…), OUTSIDE the GAME_DATA slice (a 64-byte
  opt-in section); no resource-owned pointer enters any serialized slice
  today, and the v4 walker would hard-fail on one ("unmanaged native
  pointer": the compat arenas are mmap'd 0x7f…, mincore-mapped). v5 makes
  the first such pointer safe and provable.

## 2. Exact State v5 format changes

- `NATIVE_STATE_FORMAT_VERSION` 4 → **5**; header struct unchanged
  (`contentFingerprint[64]` now carries the computed session fingerprint
  hex).
- New section tag **14 `STATE_SECTION_RESOURCE_SIDECAR`**, always present in
  v5 (record count 0 when nothing was captured); sectionCount ≤ 16 still
  holds (14 sections).
- Sidecar payload: `u32 LE recordCount || record[recordCount]`, records
  fixed 64 bytes, packed, explicit little-endian, no pointer values, no
  handles, no host padding.
- Capture: resource-range pointers detected before all normal pointer
  classification; in-band bytes zeroed; walk advances past the whole 8-byte
  field. Record limit 4096 (hard failure on overflow, never silent).
- Load: version gate → buildId → session fingerprint → structural/CRC →
  sidecar parse + strict record validation → resolve EVERY record → only
  then any live-memory write → restore → patch → existing republish
  lifecycle. No partial restoration; no force-load path.
- Battle sidecar capture/restore/validate converted to bytewise record
  access (memcpy) — fixes a pre-existing alignment UB flagged by UBSan.

## 3. Resource-reference record format

64 bytes, `__attribute__((packed))`, explicit LE:

| field | offset | meaning |
|---|---|---|
| sectionTag | 0 | owning walked-slice section tag |
| fieldOffset | 4 | byte offset of the 8-byte pointer field in the section payload |
| resourceKey | 8 | `Gen3ResourceKey` (32 raw bytes, derived from the canonical name) |
| resourceType | 40 | `Gen3ResourceType` |
| resourceSchema | 44 | schema/version |
| representationRole | 48 | 0 canonical / 1 legacy-literal-LZ / 2 compat-object |
| rangeOffset | 52 | offset within the immutable resource range |
| reserved | 56 | must be 0 |
| reserved2 | 60 | must be 0 (makes the packed record exactly 64 bytes) |

Strict limits/validation: count ≤ 4096 and `count == (size-4)/64`;
sectionTag ∈ walked slices; fieldOffset %4==0 and fieldOffset+8 ≤ section
size; strictly increasing (sectionTag, fieldOffset) with ≥8-byte separation
(no duplicates/overlaps); role ≤ 2; both reserved words 0; key nonzero;
type/schema/role must equal the registered range's identity at resolution;
rangeOffset < range length.

## 4. Session content fingerprint

`EmeraldResourceSession_ComputeContentFingerprint` /
`EmeraldResourceSession_ComputeBaseFingerprint`
(`src/emerald/resources/emerald_resource_session.c`): raw 32-byte SHA-256
(`Gen3Sha256`) over the exact construction `"gen3-session-content-v1\0" ||
LE32(len(gameId))||gameId || LE32(adapterVersion=1) ||
LE32(RESOURCE_API_VERSION) || baseProvider.logicalContentDigest[32] ||
LE32(providerCount) || per provider ascending precedence: LE32(kind),
LE32(precedence), length-prefixed id, length-prefixed version,
logicalContentDigest[32]`. Logical content only — no paths/timestamps/
pointers/PIDs; provider order meaningful; exec build identity stays the
separate buildId check. The R6 loader computes it from the pack-derived
session info at registration and sets it on the seam; save stamps the hex
into the header, load compares and rejects with the expected-vs-active pair.
Pinned vectors in the fingerprint test prove machine independence.
Equivalent content at a different path matches (TEST 5); changed content or
a reordered provider set cannot (TEST 4/6).

## 5. Reverse resource-range design

`include/emerald/resources/emerald_resource_ranges.h` +
`src/emerald/resources/emerald_resource_ranges.c` (new module, R10-C):

- One registered range per compat-image entry: the entry's stream region
  `[GetStream(i), +GetStreamSize(i))` with the derived `Gen3ResourceKey`,
  type, schema (retained per entry by the seams — the image does not store
  schemas) and `ROLE_LEGACY_LZ`. Production session: 196 trainer + 1804
  pokémon = 2000 ranges.
- Sorted by base, insert-time overlap rejection (no ambiguous overlaps),
  uintptr_t overflow-safe arithmetic, 4096-range bound, 8-hull bound.
- Arena "hulls" (whole image spans, `EmeraldResourceCompatImage_GetArenaSpan`)
  detect resource-owned pointers with NO registered identity → capture error.
- Address lookup (`Lookup`) for capture; key-based resolution
  (`ResolveByKey`) for load — the only function that turns a record back
  into a pointer, refusing on any identity mismatch or OOB offset.
- Owned by the compat seam session state (rebuilt whenever a new session
  image is adopted, reset at Shutdown); dies with the session; lookups only
  during capture/load. No runtime-handle identity anywhere.
- Unit tests pin range start / interior / last byte / one-before /
  one-after / adjacency / overlap / overflow / hull semantics (plus the
  sanitizer variant).

## 6. Save capture lifecycle

1. `SaveStateToPath` fetches the active range index, resets the capture-side
   record array, reserves the maximum sidecar size in the container.
2. Each walked slice is staged (memcpy) and `NormalizeRuntimeBytes` walks
   4-byte strides: for each 8-byte window, resource detection runs FIRST —
   range hit → record {sectionTag, offset, key, type, schema, role,
   ptr−base} + zero the in-band 8 bytes + advance past the field; arena-hull
   hit without a range → hard failure; otherwise the pre-existing
   classification runs unchanged (its "unmanaged native pointer" gate now
   only ever sees genuinely non-resource host pointers).
3. Record limit exceeded → hard failure. `ValidateNoRuntimeHandles` still
   runs over the final bytes (zeroed resource fields are inert).
4. The sidecar section (tag 14) is serialized after the slices (explicit
   LE), section CRC computed, header totals reflect the actual record
   count, payload CRC over everything.
5. `WriteContentFingerprint` stamps the session fingerprint hex (or the
   legacy constant when no session exists — non-Linux targets).

## 7. Load/transaction lifecycle

Exact order in `LoadStateFromPath`: (1) header parse; version gate first —
v4 → precise message, unsupported → clear message (§8); structural bounds;
payload CRC; (2) buildId; (3) session fingerprint compare (expected vs
active in the diagnostic); (4) section table vs BuildSlices (+1 sidecar
section), per-section tag/size/CRC and ValidateRestoreSlice; (5) sidecar
section: tag/size/CRC, count consistency, count limit; (6)
`ValidateResourceRecords` (every §3 rule); (7) `ResolveResourceRecords`
against the active index — any failure names the record, section, field
offset, key hex, type/schema/role/offset; a record-carrying state with no
active session is rejected; **live memory untouched through all of the
above**; (8) RestoreSlice for every section; (9) patch pass writes each
resolved 8-byte pointer into its owning field; (10) frame/audio/video
restore + the existing `EmeraldResourceCompat_Republish` lifecycle. A
failed restore names the section (new diagnostic).

## 8. v4 compatibility decision

**Reject v4** with "state format v4 predates resource-aware serialization
and cannot safely express resource-backed pointers; re-save with a v5
build". Rationale: v4 cannot express resource-backed pointers, so
preserving v4 load would require maintaining the old walker semantics
forever (the unsafe class) — rejected precisely instead. The version gate
runs before ANY section parsing, so v4 bytes (even with a forged tag-14
section) are never interpreted as a v5 sidecar. Tests: v4-version and
unsupported-version corruptions in the cross-restart suite, plus the
version-first gate ordering in HeaderMatches.

## 9. Files changed

Modified:
- `src/platform/native_state.c` — v5 container, resource sidecar
  write/parse/validate/resolve, capture detection + zeroing, transactional
  load, fingerprint stamp/check, v4 policy, battle-sidecar alignment fix,
  per-section rehydrate diagnostic.
- `include/emerald/resources/emerald_resource_session.h` +
  `src/emerald/resources/emerald_resource_session.c` — fingerprint
  construction + adapter version constant.
- `include/emerald/resources/emerald_resource_compat.h` +
  `src/emerald/resources/emerald_resource_compat.c` — `GetArenaSpan`.
- `include/emerald/resources/emerald_pokemon_native_compat.h` +
  `src/emerald/resources/emerald_pokemon_native_compat.c` — `GetImage`,
  `GetEntrySchema`.
- `include/emerald/resources/emerald_trainer_native_compat.h` +
  `src/emerald/resources/emerald_trainer_native_compat.c` — range index
  state + `RebuildRangeIndex`, schema retention, fingerprint state +
  accessors.
- `src/emerald/resources/emerald_runtime_loader.c` — computes and sets the
  session fingerprint at registration; clears it on a refused session.
- Test runners: `run_emerald_trainer_native_compat.sh`,
  `run_emerald_trainer_native_compat_production.sh`,
  `run_emerald_real_tables.sh`, `run_emerald_runtime_loader.sh`,
  `run_emerald_trainer_native_compat_sanitize.sh` — added the ranges TU to
  the compile lists.

New:
- `include/emerald/resources/emerald_resource_ranges.h`,
  `src/emerald/resources/emerald_resource_ranges.c` — reverse range index.
- `tests/emerald_resource_ranges_test.c`, `tests/run_emerald_resource_ranges.sh`.
- `tests/emerald_session_fingerprint_test.c`,
  `tests/run_emerald_session_fingerprint.sh`.
- `tests/emerald_resource_state_test.c`, `tests/emerald_resource_state_stub.c`,
  `tests/emerald_resource_state_corrupt.py`, `tests/run_emerald_resource_state.sh`.
- Docs: `docs/R10_NATIVE_STATE_V4_AUDIT.md`, `docs/R10_NATIVE_STATE_V5_DESIGN.md`,
  this report.

## 10. Tests added

- **Range index** (unit): start/interior/last-byte/before/after/adjacency,
  overlap rejection, overflow rejection, hull membership, hull-only span,
  identity round trip, ResolveByKey (match, type/schema/role mismatches,
  OOB offset, unknown key) — plus ASan/UBSan variant.
- **Fingerprint** (unit): determinism, pinned vectors (two), provider-order
  sensitivity (TEST 6), content sensitivity, game-id sensitivity, argument
  validation — plus ASan/UBSan variant.
- **Cross-restart suite** (integration, real walker + real seams + real
  pack + real data.c): TEST 1 round trip, TEST 2 fresh-process load, TEST 3
  provably different arena bases, TEST 4 changed session identity →
  transactional refusal (canary intact), TEST 5 equivalent content at a
  different path, TEST 7a no session / 7b unknown key → refusal before
  mutation, TEST 8 ten-corruption matrix (bad tag, OOB field offset,
  all-zero key, bad role, bad schema, OOB resource offset, oversized count,
  duplicate fields, nonzero reserved, truncated sidecar), TEST I v4 +
  unsupported-version policy — plus a full ASan/UBSan variant.
- **H**: capture recognizes 10 real migrated battle resources (pokémon
  front/back/palette/shiny + trainer front sheet/palette + back sheet/
  palette), the state carries exactly 10 sidecar records with the real
  keys/types/schemas, and a fresh process re-derives every pointer by key.

## 11. Sanitizer results

ASan/UBSan clean on: resource_lz (144), resource_import (155),
rom_base_provider (140), trainer_native_compat (8377), resource_ranges,
session_fingerprint, and the full cross-restart resource-state suite
(create/load/rejection matrix). UBSan found and we fixed a real
pre-existing alignment UB in the battle-sidecar capture/validate/restore
paths (typed struct access over a container-offset payload → bytewise
memcpy access).

## 12. Cross-restart proof (different host addresses)

TEST 2/3 output (one run): creator arena pointers
`front=0x7f06ffd45e88 back=0x7f06ffd45584 trainer=0x5600b0f255a4`; loader
`front=0x7f4eb2145e88 back=0x7f4eb2145584 trainer=0x555c6ae9d5a4` — provably
different allocations in a fresh process, and the load re-derived every
migrated row's `.data` to the LOADER's arena pointer (identity by key, never
address equality), with scalars/size/tag fields round-tripping intact.
Fingerprint identical across both processes (4f97ece5…).

## 13. Mismatch tests

- Changed session content identity → "state resource-session fingerprint
  does not match the active session (state 4f97ece5…, active deadbeef…)" →
  rejection, canary byte-exact.
- Missing session → rejected (fingerprint gate: session digest vs legacy
  constant), canary intact.
- Unknown resource key → "state resource reference 0 could not be resolved
  (section GAME_DATA field +0x8 key a11afbe7… type 2 schema 1 role 1 offset
  0)" → rejection before mutation.
- All 10 corrupt-sidecar kinds reject safely with the canary untouched.
- v4 and unsupported versions reject with precise messages before any
  section parsing.

## 14. R9 regression counts (all green, unchanged)

resource_lz 144 · resource_import 155 · rom_base_provider 140 ·
trainer_native_compat 8377 · runtime_loader 2013 · real_tables 16311 ·
trainer production 37921 (186 front + 44 back published slots = 236 trainer
byte-identical; 1759 pokémon battle slots + 1 external back-EGG) · native
asset isolation 5429 ok (10842 ok lines). All seven sanitizer variants also
green.

## 15. Isolation proof

`run_emerald_native_asset_isolation.sh`: 1804/1804 ROM_BASE_ONLY isolated
(196 trainer + 1608 pokémon battle), 0 COMPILED_PENDING_MIGRATION; migrated
leaf symbols absent from the rebuilt release binary; production pack
byte-unchanged (sha1 `88221a774f819330…`, identical to the R9 baseline).
No payload was linked back into the executable for testing; the harness
opens the pack read-only and every mutation happens on temp copies.

## 16. Remaining warnings/issues

- Pre-existing `-Wformat-truncation` notes in the native_state forensic
  dump helpers (snprintf into 96/112-byte symbol/owner buffers — output is
  truncated safely; cosmetic).
- Pre-existing string-initializer length warnings in
  `data/text/trainer_class_names.h` under the standalone-TU charmap
  identity macros (test-harness context only).
- The harness compiles with `-Wall -Wextra`; the two unused-parameter
  notes in the test main are cosmetic.
- The missing-session load is rejected at the fingerprint gate (session
  digest vs legacy constant) before the dedicated "no active resource
  session" message — same transactional outcome, message names the
  fingerprints; the dedicated message still guards the crafted
  fingerprint-matching-without-index case.

## 17. Manual validation checklist / result

DINFO binary (`pokeemerald-linux64-dinfo`) rebuilt from the validated tree;
automated self-test green (quick slot + slot 1 round trips + trainer-family
republish). Checklist for the interactive gate:

1. load existing gameplay; 2. enter battle; 3. save native state
mid-battle; 4. exit the process completely; 5. relaunch; 6. load the same
state; 7. confirm battle resumes with correct Pokémon/trainer sprites,
palettes and continuing animation; 8. overworld state cross-restart load;
9. repeated save/load; 10. mismatch rejection via the controlled
non-production harness (`tests/run_emerald_resource_state.sh` load-fail
fingerprint — does not touch the production pack).

**Result: PENDING USER CONFIRMATION** (interactive steps; the automated
cross-restart + rejection proofs above are green).

## 18. Clean Linux64 build result

Both `pokeemerald-linux64` (release) and `pokeemerald-linux64-dinfo` rebuild
from the validated tree with no errors; `git diff --check` clean; the
11-runner regression battery exits 0; all seven sanitizer variants exit 0.

**R10 STOP CONDITION: met (item 17 pending the interactive checklist).
STOP — R11 was not begun.**

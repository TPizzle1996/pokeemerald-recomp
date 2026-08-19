# R13-B — Emerald Leaf Payload Migration Report

Scope: migrate the two R13-B leaf families — **movement scripts** (1,055
resources) and **multiboot programs** (2 resources) — from compiled native
binaries into the ROM_BASE production pack as canonical GBA-form byte
payloads, published through an additive seam, with every isolation and
provenance gate proven. Completed 2026-08-18 against the qualified pret
reference build (d8e405c4f6b48f1faf3b26a3e045f0df2ff3ecb7 + upstream symbol
rename b89722500; BPEE01 Rev 0, SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7).

**Additive contract (brief §6):** no consumer is redirected. Movement
consumers are deferred (script-bytecode operands + C immediates need the
map/script pointer graph, R13-G); the ereader multiboot pointer is LIVE but
not trivially redirectable (EReaderHandleTransfer may write the buffer); the
colosseum program is dead data in the native build. A failed publication is
a DEGRADE (the compiled payloads still serve the game exactly as
pre-R13-B), never a session refusal.

## 1. Inventory (re-verified)

| Family | Resources | Bytes | Contents |
|---|---|---|---|
| Movement | 1,055 | 7,428 | 1,047 pret `*_Movement_*` script_data labels (7,404 B) + 8 `sMovement_*` .rodata objects (24 B) |
| Multiboot | 2 | 176,352 | ereader 12,512 B + pokemon-colosseum 163,840 B |
| **Total** | **1,057** | **183,780** | canonical GBA-form bytes (type binary, gba-bytes) |

The 3 recomp-local movement labels (12 B) stay compiled (not resources).
The audit's earlier 7,576 B movement figure was 148 B high; the
re-verified figure is 7,428 B. Multiboot matches the audit exactly
(176,352 B = 12,512 + 163,840; two programs, not three).

## 2. Vocabulary

- Movement: `emerald:movement/<object>-<label>` — type `binary`, schema 1,
  representation `gba-bytes`, binary/1+gba-bytes.
- Multiboot: `emerald:multiboot/<program>` (ereader, pokemon-colosseum) —
  type `binary`, schema 2, representation `gba-bytes`, binary/2+gba-bytes.

## 3. Canonical chain proof

movement: 1,047 pret labels byte-identical across ELF ↔ ROM ↔ manifest
(`gen3-elf-manifest --check`, three-way) — **0/1,047 mismatches**; 8
`sMovement_*` objects likewise. multiboot: both programs byte-identical.
Manifest provenance pins the qualified ROM digest
(f3ae0881…) per record.

## 4. Generator

`tools/gen3_resources/leaf_family/gen_leaf_family.py` — reusable generator
for both families (owns movement 7-tuples, multiboot 7-tuples with the
pret/recomp symbol split for the ereader rename, the seam slot table
`src/emerald/resources/leaf_native_table.generated.c` +
`include/emerald/resources/leaf_native.generated.h`, and the ownership
tables). `--check` regenerates every artifact byte-identically (runner E0).

## 5. Production pack

| Metric | Value |
|---|---|
| Entries | 6,876 (1,055 movement + 2 multiboot added) |
| Size | 10,076,400 B |
| SHA-256 | b4f134e8d37445646dab3690f4b97875ee376f988384f649ea9fed1b09d5e440 |
| Provenance | `gen3-pack-build --check` reproduces the pack byte-for-byte from the 8 manifests + 8 catalogs (E1); a tampered manifest `rom_sha1` is refused (E1b) |

## 6. Publication seam (additive)

`src/emerald/resources/emerald_leaf_compat.{c,h}` — `EmeraldLeafCompat_`
API. Phase 1 validates everything before allocation: composition pins
(1,055 + 2), M0/M1 resolution, type/schema, ROM_BASE winner, size + byte
equality against the session's pack views, generated slot-table set
equality, and the claimed-ROM-slice disjointness proof; phase 2 performs
one allocation + all copies and publishes atomically. Failure keeps the
prior arena (fail-soft additive). Wired into
`src/emerald/resources/emerald_runtime_loader.c` (status stays
`EMERALD_COMPAT_OK`, degrade message on failure).

Defect fixed during the failure matrix: the record table's 96-char name
buffer truncated the longest movement id (exactly 96 chars) — the sorted
table cross-check failed at position 78. Buffer enlarged to 128
(`canonicalName`, `legacySymbol`).

## 7. Failure matrix (fail-closed, all 8 mandated cases + writer guard)

Test suite `tests/emerald_leaf_compat_test.c`: **74 checks, 0 failures**.

| Case | Injection | Expected | Result |
|---|---|---|---|
| E1 | multiboot-only pack (movement family missing) | UNEXPECTED_COUNT | PASS |
| E2 | one movement entry dropped | UNEXPECTED_COUNT | PASS |
| E3 | movement-only pack (multiboot family missing) | UNEXPECTED_COUNT | PASS |
| E4 | movement entry re-schema'd (unclassifiable) | UNEXPECTED_COUNT | PASS |
| E5 | multiboot entry re-schema'd | UNEXPECTED_COUNT | PASS |
| E6 | movement payload truncated vs real session | PAYLOAD_SIZE_MISMATCH | PASS |
| E7 | multiboot payloads truncated (bytes pin) | PAYLOAD_SIZE_MISMATCH | PASS |
| E8 | two entries claim one ROM slice | OVERLAPPING_SLICE | PASS |
| E9 | duplicate canonical name | writer GEN3_PACK_ERR_DUPLICATE_NAME | PASS |

Plus: publication byte/span equality for all 1,057 leaves (sorted-name
zone == pack payloads), slice disjointness, pointer canary, invalid-arg
fail-closed, additive republish semantics (failed republish keeps the
published arena; clear/shutdown idempotent), and the truncated-pack
writer guard (zero-size payload refusal) covering the build path.

## 8. State-v5

**No impact.** The seam holds no R10 range registration (nothing in the
arena is serialized; payloads are pure bytes, the record table holds
names/offsets/sizes/schemas only — no host pointers anywhere in the arena).

## 9. Isolation (ownership state)

`tests/run_emerald_native_asset_isolation.sh` — movement + multiboot
families = **COMPILED_PENDING_MIGRATION** (symbols must remain compiled;
movement labels are LOCAL symbols, so the presence check runs against the
full `nm --defined-only` table). Battery passes exit 0: **5819/5819
ROM_BASE_ONLY** isolated (196 trainer + 1,608 pokemon battle + 288
object-event + 1,544 tileset + 882 layout + 1,301 audio) and **1057/1057
COMPILED_PENDING_MIGRATION** present (1,055 movement + 2 multiboot).

## 10. Runtime status

- Movement: **compiled** (live consumers deferred to R13-G — no redirect).
- Multiboot: **compiled**; ereader pointer live, colosseum dead data.
  Physical removal of the 176,352 B from the native binaries is NOT part
  of R13-B (deferred — depends on the live-cutover stages).

## 11. Builds (fresh `-B`, R13-B-complete tree)

| Flavor | Size | vs checkpoint-r12g-complete |
|---|---|---|
| release (`make -f Makefile_pc -B linux64`) | 21,109,312 B | +156,736 B |
| +DINFO (`make -f Makefile_pc -B linux64 DINFO=1`) | 33,383,632 B | +169,504 B |

Both build exit 0. The size deltas are the expected R13-B content: the leaf
publication seam (`emerald_leaf_compat.c`, picked up by the `C_SRCS`
wildcard) plus the 1,057-row `leaf_native_table.generated.c` (names +
legacy symbols + offsets, all native-side metadata; the 183,780 payload
bytes themselves stay out of the binary). Determinism re-proven: a
second `-B` release rebuild is byte-identical (SHA-256
`42764ea82461e8e7221c53e3d13e3dfd4a84fd8faff8b970d75277dd2bedd4e7`).
Current baselines saved as `pokeemerald-linux64.r13b-fresh-release` /
`.r13b-fresh-dinfo`; the pre-R13-B checkpoint binaries renamed to
`pokeemerald-linux64.r12g-complete` / `.r12g-complete-dinfo` for
delta comparison.

## 12. Regression

R13-B runner (`tests/gen3_resources/run_r13b_leaf.sh`): **7/7 PASS**
(E0 generator determinism, E1 pack determinism, E1b tamper refusal, E2
manifest provenance, A–G seam tests, F isolation battery).

Full R9–R12 regression battery (35 suites incl. gen3-core/sanitize,
audio leaf/inventory, elf-manifest + sanitize, pokemon-family,
resource-pack/pack-provider + sanitize, trainer-family + sanitize,
layout-compat, asset-isolation, world-neighborhood, object-event-compat,
resource-import/lz/ranges/state, rom-base-provider + sanitize,
runtime-loader, session-fingerprint, tileset-compat,
trainer-native-compat + sanitize, trainer-native-prod, real-tables,
world-real, world-render-proof, r13b-leaf): **35/35 PASS** on the final
clean run.

Three suite updates were required to carry the R13-B pack growth and the
new seam dependency, each fixed and re-verified before the final run:

1. **Pack-count pins 5,819 → 6,876** in `emerald_audio_compat_test.c`,
   `emerald_trainer_native_compat_production.c`,
   `emerald_native_world_real_test.c`,
   `emerald_native_world_render_proof.c` (the R10 range-index pin at
   world-real stays 5,819 — R13-B registers no ranges, State-v5
   unaffected).
2. **Catalog list for the native-suite runners**: the production-pack
   runners (trainer-native-prod, world-real, world-render-proof) now pass
   the movement + multiboot catalogs so their 6,876-resource catalog
   checks see the real pack contract.
3. **Link wiring for `EmeraldLeafCompat_TryInitialize`**: runners that
   compile `emerald_runtime_loader.c` (resource-state, runtime-loader,
   desktop-sdl-probe) link `emerald_leaf_compat.c` +
   `leaf_native_table.generated.c`; `emerald_real_tables_test.c` includes
   them alongside the loader (its own include pattern).

All other suites passed unchanged, including asset-isolation (5,819/5,819
ROM_BASE_ONLY + 1,057/1,057 COMPILED_PENDING_MIGRATION, exit 0) and
audio-leaf (10,375 checks, 0 failures).

## 13. R13-C blockers

None technical. R13-C (text families) consumes the R13-B vocabulary and
the logical-address registry pattern; per the R13-A architecture §7 the
text migration is zero-consumer-edit once registered.

## 14. STOP

R13-B complete. No commit was made; R13-C has NOT been started, per the
brief's STOP conditions.

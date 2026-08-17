# R12-A: Emerald Audio Inventory — Implementation Report

**Stage:** R12-A (inventory + vocabulary + representation compatibility only)
**Branch:** `agent/prepare-v0.1.0-alpha` (HEAD `d3a71641f`)
**Date:** 2026-08-17
**Scope rule:** *No pack format change, no audio payload in the pack, no MP2K
runtime change, no `HostResolveGbaAddr` change, no State v5 change, no removal
of audio objects from the native executable.* This report confirms all six.

---

## 1. Inventory (generated files)

| Output | Path |
|---|---|
| Inventory | `resources/extraction/emerald/bpee01/audio/inventory.generated.toml` |
| Provenance pins | `resources/extraction/emerald/bpee01/audio/canonical_sample_pins.generated.txt` |
| Generator | `tools/gen3_resources/audio_family/gen_audio_inventory.py` |
| Test runner | `tests/gen3_resources/run_audio_inventory.sh` |

### 1.1 Exact family counts (all verified against checked-in sources)

| Kind | Count | Source of truth |
|---|---|---|
| `song` | **530** | `sound/song_table.inc`: 610 rows − 80 `song dummy_song_header` aliases (incl. `mus_dummy`; the 80 alias rows get one `[[excluded]]` block, `count = 80`) |
| `sample/root` | **105** | `sound/direct_sound_data.inc`, `DirectSoundWaveData_*` root symbols |
| `sample/phoneme` | **51** | `DirectSoundWaveData_Phoneme_<n>` (n = 1..51) |
| `sample/cry` | **388** | `Cry_*` symbols |
| `programmable-wave` | **25** | `sound/programmable_wave_data.inc` |
| `voicegroup` | **195** | `sound/voice_groups.inc`: 195 voicegroup includes (180 root + 10 `drumsets/` + 5 `keysplits/`; `voicegroup_dummy` is one of the 195 — the audit pins 195 tables and the task's exclusion list does not cover it) + 1 `cry_tables.inc` include |
| `cry-table` | **2** | `gCryTable` (388 rows) + `gCryTable_Reverse` (388 rows) |
| `keysplit` | **5** | piano 36, strings 36, trumpet 36, french-horn 36, tuba 24 |
| **Total** | **1,301** | = 1301 keys, one canonical id per audio object |

### 1.2 Cross-checks the generator enforces (hard failures)

- **Songs ↔ files**: every one of the 530 table rows resolves to exactly one
  `sound/songs/*.s` or `sound/songs/midi/*.s`; every song's header declares
  NumTrks matching its `.int`/`.4byte` track-pointer rows (hand-written songs
  use `.int`, mid2agb output uses `.4byte`; both place the song object at the
  end of the file before `.end`); every `.equ <song>_grp, voicegroup_*`
  resolves to a defined voicegroup (176 unique voicegroups referenced;
  `voicegroup_dummy` allowed for 5 songs incl. `mus_dummy`).
- **Cry tables**: all 776 `cry`/`cry_reverse` rows reference defined `Cry_*`
  samples; both tables pinned at exactly 388 rows.
- **Samples**: `direct_sound_data.inc` symbols ↔ `.incbin` artifacts is a
  1:1 bijection, and the 544 artifacts ↔ the 544 tracked `.aif` authoring
  sources is a 1:1 bijection (a missing or extra `.aif` is a hard failure).
- **Voicegroups**: names come from the `voice_group <name>` macro *inside*
  each `.inc` (never the filename); exactly one macro per file; 195 includes
  + exactly one `cry_tables.inc` include; `voicegroup_<name>` labels unique.
- **Keysplits**: exactly 5 `keysplit <name>, <offset>` rows, unique names.
- **Duplicates**: duplicate symbols, duplicate resource keys, duplicate
  artifacts all refused.

### 1.3 Song prefix / track profile

se- 269, mus- 210, ph- 51 (no other prefixes). Track counts 0..10
(0: `mus_dummy`, 1: 195, 2: 122, 8: 51, 9: 46, …).

## 2. Type vocabulary (R12 §2, item 1)

`GEN3_RESOURCE_TYPE_INSTRUMENT_BANK` appended after `GEN3_RESOURCE_TYPE_BINARY`
in `include/gen3/resources/resource_types.h`, with `GEN3_RESOURCE_TYPE_COUNT`
as the bounds sentinel; `GEN3_PACK_TYPE_INSTRUMENT_BANK = 15` (+
`GEN3_PACK_TYPE_COUNT`) in `resource_pack.h`. **Append-only**: the on-disk
type codes mirror the enum and existing codes 1–14 are untouched, so the
`.rpack` v1 format is byte-compatible (no format/version change). Wired
through:

- `Gen3ResourceType_Name` → `"instrument-bank"` (`resource_core.c`)
- `Gen3ResourcePack_TypeToCode` / `TypeFromCode` bounds now use the `_COUNT`
  sentinels (`resource_pack.c`)
- `ParseTypeName` (importer) and the two test mirrors
  (`emerald_native_world_real_test.c`, `emerald_native_world_render_proof.c`)
  accept `"instrument-bank"`

## 3. elf-manifest representation compatibility (item 2)

`TypeCompatibleWithRepresentation` (`tools/gen3_resources/elf_manifest/manifest.c`)
now takes the catalog schema and accepts **exactly** the approved R12 §2
pairs — anything else fails closed:

| Type | Schema | Representation(s) |
|---|---|---|
| `music-sequence` | 1 | `gba-mp2k-song-graph` |
| `audio-sample` | 1 | `gba-wave-data` \| `gba-cgb-wave` |
| `instrument-bank` | 1 | `gba-tone-data-12` |
| `instrument-bank` | 2 | `gba-keysplit-run` |

No loose wildcards: cross-type mixes, cross-representation mixes, and
wrong-schema mixes (e.g. `instrument-bank/1/gba-keysplit-run`,
`instrument-bank/2/gba-tone-data-12`, `audio-sample/2/gba-wave-data`,
`instrument-bank/3/...`) are all `GEN3_MANIFEST_TYPE_MISMATCH`. The
pre-existing families (tile-graphics, palette, tileset, tilemap) are
unchanged; the checked-in `manifest.generated.toml` still regenerates
byte-for-byte.

## 4. Key taxonomy (item 6)

R11 canonical naming: lowercase symbol suffix, `_` → `-`; phonemes and waves
use their number. Zero keys contain `_`; keys are bytewise sorted and unique.

```
emerald:audio/song/<canonical>              music-sequence/1/gba-mp2k-song-graph
emerald:audio/sample/<canonical>            audio-sample/1/gba-wave-data
emerald:audio/sample/cry/<canonical>        audio-sample/1/gba-wave-data
emerald:audio/sample/phoneme/<n>            audio-sample/1/gba-wave-data
emerald:audio/wave/programmable/<n>         audio-sample/1/gba-cgb-wave
emerald:audio/voicegroup/<canonical>        instrument-bank/1/gba-tone-data-12
emerald:audio/cry-table/forward|reverse     instrument-bank/1/gba-tone-data-12
emerald:audio/keysplit/<canonical>          instrument-bank/2/gba-keysplit-run
```

## 5. Determinism / --check (item 3, 5)

The generator parses only checked-in sources, emits bytewise-sorted TOML, and
`--check` byte-compares both committed outputs against a fresh regeneration
(`run_audio_inventory.sh` Part A/B/C: byte-identical regen, no-op diff, two
runs identical). Validation order puts table parsing before file
cross-checks so the failure-matrix tests can run against a temp root of just
the 6 table files. `--check` refuses malformed rows, duplicate symbols,
duplicate artifacts, missing family sources, count deviations, unresolved
cry references, missing `voice_group` macros, song track-count mismatches,
and `.aif`/`.bin` bijection breaks (Part D: 9/9 mutations refused with the
expected message).

## 6. Canonical .aif provenance (item 8)

Provenance-anchor approach (no hardcoded sizes):

- **Pins**: `canonical_sample_pins.generated.txt` = SHA-256 of all 544
  working-tree `.aif`, sorted by repo-relative path.
- **Git anchor**: the 544 `.aif` *contents* at HEAD are byte-identical as a
  set to the pre-fork canonical blobs at `ee1173eb9^` (verified in Part E by
  hashing `git cat-file` blobs on both sides; both sides exactly 544
  entries). The fork's 76 sample expansions and its renames were restored to
  canonical content in `a620dddbc`.
- `--check` fails if any working-tree `.aif` drifts from its pinned hash.

## 7. Tests run

| Suite | Result |
|---|---|
| `tests/gen3_resources/run_audio_inventory.sh` (new, Parts A–E) | **17/17 PASS** |
| `tests/gen3_resources/run.sh` (M0/M1 core + guardrail 18) | PASS (incl. new R12-A vocabulary test: names, catalog accept, COUNT rejection) |
| `tests/gen3_resources/run_elf_manifest.sh` | 51 checks, 0 failures (5 accepted + 10 rejected audio pairs) |
| `tests/gen3_resources/run_resource_pack.sh` | 260 checks, 0 failures (incl. type-code round-trip 1..15) |
| `tests/gen3_resources/run_resource_pack_provider.sh` | 109 checks, PASS |
| `tests/gen3_resources/run_trainer_family.sh` | 34 passes, 0 failures |
| Emerald resource battery (`run_emerald_resource_import.sh`, `_sanitize`, `_lz`, `_ranges`, `run_emerald_rom_base_provider.sh`, `run_emerald_session_fingerprint.sh`, `run_emerald_resource_state.sh`) | 7/7 PASS (covers the edited `ParseTypeName` importer) |
| `tests/gen3_resources/run_pokemon_family.sh` | **BROKEN at HEAD — pre-existing, not R12-A** (see §9) |
| `git diff --check` | clean |
| Native build `make -f Makefile_pc -B linux64` | (see §8) |

## 8. Runtime behavior

**Unchanged.** The native executable's audio objects, MP2K runtime, save-state
layout, and `HostResolveGbaAddr` are untouched; the vocabulary additions only
extend an enum and its name/parse tables (no audio object is referenced or
migrated). Native build result: `make -f Makefile_pc -B linux64` →
**BUILD EXIT: 0** (full `-B` rebuild, fresh `pokeemerald-linux64` binary;
only pre-existing warnings such as the unused `show` variable in `src/tv.c`).

GBA build check: the repo's GBA build is already blocked by unguarded
`field_camera.c`/`parity.c` (known, pre-existing). The relevant check for
R12-A is whether the vocabulary can reach GBA sources — it cannot: no file
outside `src/gen3/`, `tools/`, `tests/` includes a `gen3/` header
(`src/emerald/resources/*` is host-only), and the `.rpack` type-code change
is a host-side enum mirror. `#include` sweep confirmed.

## 9. Deviations and pre-existing issues

1. **`run_pokemon_family.sh` is broken at HEAD, before and independent of
   R12-A.** `gen_pokemon_inventory.py` matches only the pre-R9-§7
   `SPECIES_SPRITE(...)` macro form; commit `41db96487` (R9 §7) converted the
   four battle tables to `SPECIES_BATTLE_SPRITE(...)`, so the generator now
   sees only the single back-EGG external row (`SPECIES_SPRITE(EGG,
   gMonStillFrontPic_Egg)`, species 412) and fails the designated-initializer
   check. Proven: fails with R12-A edits stashed; fails in a detached
   worktree at `41db96487`; green at `8bccbc868` (R9 §5) and `5bb6fcdab`
   (R9 stage 4). Not fixed here — it is an R9-family repair that would
   regenerate committed R9 outputs, outside R12-A scope.
2. **`voicegroup_dummy` is inventoried** as one of the 195 voicegroups (the
   audit's 195-table count includes it, and the task's exclusion list
   doesn't). Its compiled default state remains the R12-C publication seam.
3. **The 80 `dummy_song_header` aliases are excluded** with a single
   `[[excluded]]` block (`count = 80`) — they share one compiled placeholder
   header, as approved.
4. The two cry tables are covered by the `cry-table/forward` and
   `cry-table/reverse` keys; they were not split into 388 individual cry
   resources (cries are payloads referenced by both tables; the approved §2
   mapping treats each table as one bank).

## 10. Ownership transfer

**None.** No audio payload entered any pack; the native executable still
compiles every song/sample/wave/voicegroup/cry table/keysplit; the inventory
is metadata only (the future R12-B pack input). R12-B and later are not
started; nothing is committed.

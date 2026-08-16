# Emerald ROM-backed asset migration architecture

## Status and scope

This document defines how Pokémon Emerald content will move from the native executable to a locally generated resource pack backed by the user's verified ROM. It uses the existing M0/M1 Gen3 resource registry and resolver as the only resource authority.

This is an engineering and distribution architecture, not a legal conclusion. It does not authorize implementation of Stage 4 renderer work, FireRed, releases, code mods, or a second resource abstraction.

## Executive decision

For Emerald native builds, use this flow:

```text
verified BPEE01 ROM
    → generated extraction manifest
    → local deterministic .rpack
    → ROM_BASE provider
    → existing Gen3 resolver
    → one-time Emerald compatibility image
    → existing tables and consumers
```

The canonical representation for graphics schema 1 should be decoded GBA-native tile and palette data, not original LZ streams. This matches the implemented M1 validators and gives future mods a clean contract. The compatibility layer should generate simple valid GBA-LZ streams in a permanent arena for legacy consumers that still call LZ decompression.

The first real family should be:

- `emerald:trainer/brendan/battle/front/sheet`
- `emerald:trainer/brendan/battle/front/normal-palette`

These are clean proof targets: their payload symbols are referenced only through two trainer tables, while multiple real game consumers exercise those tables.

Two bounded extensions will eventually be needed around M1:

1. Per-resource catalog constraints such as exact decoded size. The current type validator only checks tile alignment and broad palette validity.
2. Save-state identities for pointers into immutable resource and compatibility arenas before migrating families whose pointers persist in game state.

Neither extension changes canonical identity, provider precedence, handles, or resolution semantics.

## 1. Current Emerald asset pipeline

The native build still links almost the entire pret content graph. [`Makefile_pc`](../Makefile_pc) collects C, data assembly, songs, and MIDI objects into the native executable. PNG and palette transformations and GBA compression remain native build dependencies.

The repository contains approximately 7,873 `INCBIN` uses: about 6,302 below `graphics/` and 1,558 below `data/`. The inspected Linux executable contains, among many others:

- `gTrainerFrontPic_Brendan`: 804 bytes
- `gTrainerPalette_Brendan`: 40 bytes
- `gTrainerFrontPicTable`: 0x5d0 bytes
- `gTrainerFrontPicPaletteTable`: 0x5d0 bytes

### Current source-to-consumer paths

| Family | Source and transformation | Linked representation | Principal consumer |
| --- | --- | --- | --- |
| Pokémon pictures | PNG/palette → `gbagfx` 4bpp/gbapal → `.lz` → `src/data/graphics/pokemon.h` | `gMonFrontPic_*`, back pics, normal/shiny palettes and `gMon*Table` | `decompress.c`, battles, summary, storage, evolution |
| Trainer fronts | PNG/palette → 4bpp/gbapal → `.lz` → `src/data/graphics/trainers.h` | `gTrainerFrontPic_*`, palettes and front tables | battle graphics, field effects, Pokenav |
| Trainer backs | PNG → raw 4bpp with compiled `SpriteFrameImage` slices | Back-pic arrays and tables | Battle controllers |
| Overworld graphics | PNG → raw 4bpp using metatile-width rules | `gObjectEventPic_*` → frame tables → `ObjectEventGraphicsInfo` | `GetObjectEventGraphicsInfo`, sprite engine |
| Tilesets | PNG/pal → 4bpp/gbapal; tiles compressed; metatiles/attributes raw | Tiles, palettes, metatiles and callback-bearing `Tileset` objects | Field map loading and tileset animations |
| Map layouts | Layout JSON and `border.bin`/`map.bin` → generated assembly | `MapLayout`, `MapHeader`, `gMapGroups`, four-byte `host_ptr` operands | `overworld.c`, `fieldmap.c` |
| UI/battle/title/intro graphics | Feature-local PNGs → 4bpp/8bpp/tilemap/palette → usually LZ | Global and file-local arrays | Direct LZ, BG and sprite loaders |
| Fonts | PNG → `.latfont`/Japanese font wire formats | GBA const arrays; native hydrated writable arrays | Text glyph renderer |
| Songs/SFX | MIDI → `mid2agb` assembly; song table assembly | GBA-shaped song headers, tracks and pointer tables | m4a |
| Samples/cries | AIF → PCM or compressed PCM `.bin`; `.incbin` assembly | Wave data, voicegroups and cry tables | m4a mixer |
| Maps/events | Map JSON → `mapjson` generated assembly | Pointer-bearing map events, connections and layouts | Overworld and script setup |
| Scripts/dialogue | Script assembly plus charmap preprocessing | Bytecode and encoded text, frequently with embedded pointers | `ScriptContext` and script commands |
| Encounters | JSON → `jsonproc` generated C | `WildPokemon*` arrays and pointer tables | Wild encounter logic |
| Species/moves | C tables; excluded for desktop native | Writable arrays hydrated from the current content package | Mechanics |
| Items | C table combining data, text pointers and function pointers | `gItems` | Bag, field use and battle use |
| Animation data | C commands, pointer tables and callbacks plus graphics | Animation scripts, templates and tables | Battle and field animation engines |

### Existing local content package

[`src/platform/desktop_game_content.c`](../src/platform/desktop_game_content.c) already copies 15 hardcoded ROM ranges into `content.pak` and hydrates:

- species and move names
- battle moves
- experience tables
- species information
- ten font tables

This proves the launcher/import lifecycle, but it is a second identity/package abstraction based on numeric IDs and hardcoded offsets. It must be absorbed into the Gen3 registry, not extended independently.

## 2. Engineering ownership classification

| Resource family | Classification | Recommendation | Consequence |
| --- | --- | --- | --- |
| SDL/platform/frontend/resource core | A — port-original code | No migration | Remains in native distribution |
| Renderer and VRAM/OAM emulation | A | No migration | Consumes runtime GBA state |
| Decompression, sprite, BG, text, script and m4a engines | A/B | Keep compiled | Functional code interprets resources |
| Hardware lookup/math tables | B — functional data | Keep compiled | Little benefit from early migration |
| Mechanics constants/type charts | B | Keep compiled initially | Future data mods need deliberate schemas |
| Pokémon/trainer pictures and palettes | C — expressive | Move first/second | Strong ownership benefit and compatible tables |
| Overworld pictures and palettes | C | Move after save-state resource references | Long-lived sprite image pointers |
| Tileset graphics and palettes | C | Move in a later graphics wave | Requires runtime `Tileset` objects |
| Metatiles, borders and map blockdata | C/D | Move later | Valuable for mods but tied to layout metadata |
| UI, battle, title and intro graphics | C | Move after table machinery | Many file-local symbols and direct calls |
| Fonts | C | Refactor in an early later wave | Already ROM-backed through the old package |
| Music, SFX, samples and cries | C/D | Move much later | Pointer-rich MP2K graph |
| Dialogue and descriptions | C | Move later | Scripts and tables embed text pointers |
| Map layouts/events | D — mixed | Keep compiled initially | Pointer-rich structures and callbacks |
| Scripts | D | Keep compiled initially | Bytecode contains address operands |
| Encounter tables | B/D | Keep compiled initially | Future typed data modding |
| Species/move numeric data | B/D | Keep current ROM hydration temporarily | Needs structured schemas |
| Items | D | Keep compiled | Mixes text, numeric data and callbacks |
| Sprite/animation commands | D | Keep compiled initially | Pointer tables and callbacks |
| Pure animation graphics | C | Move with broader graphics | Commands can remain compiled |

### Recommended migration order

First:

1. Brendan battle front sheet and palette.
2. All trainer front sheets and palettes using the same tables.

Second:

1. Trainer back graphics.
2. Pokémon front/back pictures and normal/shiny palettes.
3. Bounded global UI, battle-background, title and intro graphics.
4. Refactor already-ROM-backed fonts into ROM_BASE.

Pokémon follows trainers because Deoxys, Spinda, Castform and form/animation handling introduce special cases.

Later:

1. Object-event graphics.
2. Tilesets, metatiles and raw map blocks.
3. Audio.
4. Dialogue, scripts, events, encounters, structured gameplay data and animation command graphs.

## 3. M0/M1 audit and required extensions

The implemented contract in [`GEN3_RESOURCE_MOD_CONTRACT.md`](GEN3_RESOURCE_MOD_CONTRACT.md) remains authoritative. The resolver already recognizes BOOTSTRAP, LEGACY_COMPILED and ROM_BASE as base providers.

### Catalog constraint gap

The current validator accepts any nonempty tile payload divisible by 32 and any nonempty even palette no larger than 512 bytes. It cannot prove that Brendan's sheet is exactly 2,048 decoded bytes or its palette exactly 32 bytes.

Before mods are enabled, add catalog-owned constraints:

```text
exact_payload_size
minimum_payload_size
maximum_payload_size
required_alignment
optional schema-specific validation profile
```

These constraints must participate in normal provider validation so an invalid optional mod override falls through to ROM_BASE. This is a Resource API minor extension, not a new resource system.

### Representation clarification

Preserve `tile-graphics` schema 1 as decoded tile data. Do not reinterpret it as arbitrary GBA-LZ data: Brendan's 804-byte stream fails the current tile rule.

## 4. Extraction-metadata generation

### Recommended source of truth

Use a hybrid of:

1. The canonical catalog for ID/type/schema.
2. A game/revision binding source for canonical ID → GBA symbol and extraction rule.
3. A matching GBA ELF for exact linked addresses and object sizes.
4. The matching GBA ROM for byte verification.
5. Generated `INCBIN`/assembly provenance.
6. Strict format decoders for decoded lengths and canonical hashes.

The symbol is maintainer extraction metadata only. It is never the public resource identity.

### Maintainer flow

```text
pinned pret source + pinned tools
    → exact GBA build
    → pokeemerald.elf + pokeemerald.gba
    → verify canonical ROM hashes
    → resource binding generator
    → generated BPEE01 extraction manifest
```

Players need only the shipped metadata, not the ELF, pret tree or GBA toolchain.

### Binding input

The input records semantic choices, not offsets:

```toml
manifest_version = 1
game = "emerald"
rom_profile = "bpee01-rev0"

[[bindings]]
id = "emerald:trainer/brendan/battle/front/sheet"
gba_symbol = "gTrainerFrontPic_Brendan"
source_artifact = "graphics/trainers/front_pics/brendan.4bpp.lz"
source_encoding = "gba-lz77"
canonical_representation = "decoded"
expected_decoded_size = 2048

[[bindings]]
id = "emerald:trainer/brendan/battle/front/normal-palette"
gba_symbol = "gTrainerPalette_Brendan"
source_artifact = "graphics/trainers/palettes/brendan.gbapal.lz"
source_encoding = "gba-lz77"
canonical_representation = "decoded"
expected_decoded_size = 32
```

`source_artifact` is maintainer provenance and is not copied into the runtime pack.

### Generated record

Each generated record contains:

```text
canonical name
32-byte canonical key
resource type and schema
catalog constraints
GBA symbol
ROM offset
source encoded length
canonical decoded length
source encoding
source encoded SHA-256
canonical payload SHA-256
alignment
```

Records are sorted bytewise by canonical name. Timestamps, absolute paths, host ordering and linker-map formatting do not enter the output.

### Address and length rules

For C `INCBIN` arrays:

- Read ELF `st_value`.
- Require an allocated object in the ROM range.
- Calculate `rom_offset = st_value - 0x08000000`.
- Use `st_size` as encoded length.
- Verify ELF bytes, source artifact bytes and ROM slice are identical.

For assembly `.incbin` objects whose symbol size is zero, do not infer length from an arbitrary following symbol. Use generated end labels, emitted provenance lengths, a format-defined length with an explicit region, or a generated family descriptor.

For large families, use declarative/X-macro family lists to generate both GBA tables and extraction bindings. Parsing arbitrary C is a transitional inventory technique only.

### Mandatory generator rejection cases

- wrong ROM revision or hashes
- GBA build not matching the reference ROM
- missing, ambiguous, non-ROM or invalid symbol
- range outside the 16 MiB ROM
- artifact, ELF and ROM byte mismatch
- malformed or over-reading decompression
- decoded length mismatch
- decoded content mismatch with decomp artifact
- catalog type/schema/name/key mismatch
- unexpected duplicate names or ranges

## 5. Emerald base resource-pack format

Recommended path:

```text
games/emerald/base/emerald-bpee01-v1.rpack
```

The file is generated locally and never included in a release.

### Encoding rules

- Fixed-width little-endian integers.
- Never serialize C structs or enums directly.
- Explicit pack codes for resource types and encodings.
- Exact accepted canonical-name bytes.
- TOC sorted bytewise by canonical name.
- Payloads aligned to 16 bytes.
- Zero alignment and reserved bytes.
- Checked 64-bit arithmetic for all offsets and lengths.

### Header: 448 bytes

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic `G3RPACK\0` |
| 8 | 4 | Header size, 448 |
| 12 | 4 | `RESOURCE_PACK_FORMAT_VERSION`, 1 |
| 16 | 4 | Endian tag, `0x01020304` |
| 20 | 4 | Flags, zero in v1 |
| 24 | 4 | Emerald base-pack version |
| 28 | 4 | Entry count |
| 32 | 4 | Entry size, 160 |
| 36 | 4 | Resource API major |
| 40 | 4 | Resource API minor |
| 44 | 4 | Resource API patch |
| 48 | 4 | Catalog version |
| 52 | 4 | Extraction-manifest version |
| 56 | 4 | Canonical representation version |
| 60 | 4 | Reserved |
| 64 | 8 | Exact file size |
| 72 | 8 | TOC offset |
| 80 | 8 | TOC size |
| 88 | 8 | Names offset |
| 96 | 8 | Names size |
| 104 | 8 | Payload offset |
| 112 | 8 | Payload size |
| 120 | 8 | Source ROM size |
| 128 | 20 | Source ROM SHA-1 |
| 148 | 32 | Source ROM SHA-256 |
| 180 | 4 | Game code, `BPEE` |
| 184 | 2 | Maker code, `01` |
| 186 | 1 | Software revision, 0 |
| 187 | 1 | Reserved |
| 188 | 16 | NUL-padded game ID, `emerald` |
| 204 | 32 | Catalog SHA-256 |
| 236 | 32 | Extraction-manifest SHA-256 |
| 268 | 32 | TOC SHA-256 |
| 300 | 32 | Names SHA-256 |
| 332 | 32 | Payload SHA-256 |
| 364 | 32 | Logical pack-content SHA-256 |
| 396 | 32 | Header SHA-256 |
| 428 | 20 | Reserved |

`header_sha256` is calculated over all 448 bytes with that field zeroed.

### TOC entry: 160 bytes

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 32 | Stable resource key |
| 32 | 4 | Name offset relative to names section |
| 36 | 2 | Name length |
| 38 | 2 | Explicit resource-type code |
| 40 | 4 | Schema |
| 44 | 4 | Flags including `required_for_base` |
| 48 | 4 | Canonical representation code |
| 52 | 4 | Source encoding code |
| 56 | 8 | Absolute payload offset |
| 64 | 8 | Canonical payload size |
| 72 | 8 | Source ROM offset |
| 80 | 8 | Source encoded size |
| 88 | 32 | Canonical payload SHA-256 |
| 120 | 32 | Source encoded SHA-256 |
| 152 | 8 | Reserved |

ROM offsets are provenance, not identity, and are not exposed through `Gen3ResourceView`.

### Logical content digest

```text
SHA-256(
    "gen3-base-resource-pack-v1\0"
    || LE version fields
    || fixed game/profile fields
    || source ROM SHA-1 and SHA-256
    || catalog SHA-256
    || extraction-manifest SHA-256
    || TOC SHA-256
    || names SHA-256
    || payload SHA-256
)
```

Also calculate a provider-content digest from sorted keys, types, schemas, sizes and canonical payload hashes. Save-state compatibility uses this logical provider digest so a physical pack-format change that preserves all resources does not invalidate states.

## 6. Encoded-versus-decoded policy

| Family | Pack representation | Compatibility behavior |
| --- | --- | --- |
| LZ/RL tile graphics | Decoded GBA tile bytes | Generate permanent literal-only GBA-LZ if needed |
| Compressed palettes | Decoded BGR555 bytes | Generate permanent GBA-LZ if needed |
| Raw overworld 4bpp | Original raw bytes | Direct permanent pointer |
| Raw palettes | Original BGR555 bytes | Direct pointer/load |
| Tilemaps | Decoded little-endian tilemap bytes | Wrap in LZ only for legacy call sites |
| Tileset graphics | Decoded tiles | Permanent LZ plus compatibility `Tileset` |
| Metatiles/attributes | Original little-endian wire | Direct aligned pointer |
| Map blockdata/borders | Original little-endian wire | Direct pointer |
| Fonts | Original font wire | Hydrate/endian-convert where required |
| Text | Original Emerald charmap bytes | Direct permanent pointer |
| Music/SFX sequences | Original MP2K bytecode with relocation metadata | Hydrate pointer graph later |
| Samples/cries | Original MP2K sample encoding | Preserve current mixer |
| Structured data | Versioned canonical little-endian wire | Convert to host structs |
| Scripts | Original bytecode plus relocation records | Resolve references later |
| Pointer/callback tables | Not raw public resources | Rebuild from resources and functional metadata |

Decoded graphics fit the current schema and future mod tooling. Legacy compatibility uses a deterministic literal-only GBA-LZ encoder:

```text
4-byte 0x10 header with 24-bit decoded size
for each group of up to eight bytes:
    zero flag byte
    literal bytes
pad to four-byte alignment
```

The compatibility stream need not match the original compressed stream; its decoded output must match exactly.

## 7. ROM import and extraction lifecycle

### Supported-profile validation

1. Open a regular readable file.
2. Require exactly 16,777,216 bytes.
3. Require game code `BPEE`, maker `01`, revision 0.
4. Require SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`.
5. Also require the locked SHA-256 once generated.
6. Extract only after all checks pass.

### Atomic installation

1. Acquire an import lock.
2. Create a unique temporary file in the final directory.
3. Strictly extract and decode manifest ranges.
4. Verify every entry and catalog contract.
5. Flush and close the temporary file.
6. Reopen it through the normal pack reader and validate it.
7. Durably flush where supported.
8. Atomically replace the final single file.
9. Reopen and verify the installed file.
10. Release the lock.

Interrupted temporary files are ignored and removed on the next import. The old valid pack remains untouched until the new candidate is complete.

### Existing pack behavior

- Expected and valid: use it without reading the ROM.
- Corrupt: refuse gameplay and request reimport.
- Older physical format: keep but do not load; request reimport to a versioned filename.
- Newer unsupported format: require an engine update.
- Reimport: verify off-side, then atomically replace.
- Never persist the user's ROM path.

One self-contained pack replaces the current two-file manifest/package pair.

## 8. ROM_BASE provider and publication

Recommended explicit precedence:

```text
100  BOOTSTRAP
200  LEGACY_COMPILED
300  ROM_BASE
1000+ deterministic future mod order
```

ROM_BASE construction:

1. Fully validate the pack.
2. Recompute keys from exact names.
3. Match each entry against the finalized catalog.
4. Validate type, schema and constraints.
5. Create provider `emerald.rom-base.bpee01`, kind ROM_BASE, precedence 300.
6. Treat each present base-pack entry as required for that provider.
7. Build a normal resolver candidate.
8. Build the Emerald compatibility image from that snapshot.
9. Start Emerald only after both succeed.

BOOTSTRAP contains only engine-owned recovery resources and must not provide migrated expressive IDs.

LEGACY_COMPILED is developer/parity-only for cataloged resources still being compared. It is absent for migrated required IDs in release isolation builds. Unmigrated ordinary symbols need no fake registry entries.

## 9. Emerald compatibility tables

Build one immutable `EmeraldResourceCompatibilityImage` after final resolution. It owns:

- permanent legacy-format payload wrappers
- tables mutable only during construction
- cloned metadata structures
- a reverse range map for save states
- diagnostics naming the resource and winning provider for each entry

### Flat leaf tables

These can become native runtime arrays with unchanged indexing:

- `gTrainerFrontPicTable`
- `gTrainerFrontPicPaletteTable`
- `gMonFrontPicTable`
- `gMonBackPicTable`
- normal/shiny palette tables
- item icon and similar sheet/palette tables

Tags, decoded sizes and functional metadata remain compiled. Only payload pointers are resolved.

### Cloneable pointer graphs

Rebuild in a permanent arena:

- `SpriteFrameImage` arrays
- `ObjectEventGraphicsInfo`
- sprite sheets, palettes and templates with compiled callbacks
- `Tileset` objects with compiled animation callbacks
- selected UI tables

Object-event access is mostly centralized through `GetObjectEventGraphicsInfo`, which provides a useful seam.

### GBA-shaped pointer assembly

These require explicit hydration:

- `gMapGroups`
- map headers/events/layouts
- song tables and headers
- scripts and voicegroups
- any table containing four-byte `host_ptr` values

Never write a 64-bit host pointer into a four-byte GBA field. Clone to host-native structures or translate with a generated relocation table.

### File-local resources

File-local UI/title/intro arrays need generated native compatibility slots or module-level runtime tables. This changes declarations, not every load call.

### First trainer-table implementation

1. Generate native table skeletons from the same trainer list used by GBA.
2. Keep legacy pointers for unmigrated entries.
3. Resolve Brendan's two IDs.
4. Generate permanent literal-LZ sheet and palette streams.
5. Preserve existing tags and decoded sizes.
6. After isolation, omit the two leaf payload symbols from native linking.

## 10. Pointer lifetime and memory

Use eager, permanent, immutable session storage:

```text
Gen3ResourceSession
├── active immutable resolver snapshot
│   └── owned canonical payload bytes
└── EmeraldResourceCompatibilityImage
    ├── legacy-LZ arena
    ├── runtime pointer tables
    └── reverse resource-range index
```

Rules:

- Select providers before gameplay.
- Do not unload or reorder providers during a session.
- Views and compatibility pointers live until process exit.
- No hot reload during this migration.
- Reimport applies to the next session.
- Align arena allocations to at least 16 bytes.
- Construct compatibility data transactionally.

Do not require mmap in v1. M1 currently deep-copies provider payloads, so mmap would not remove that copy. Profile peak memory before broad migration; an internal ref-counted immutable backing may be added later without changing public resource semantics.

## 11. Save states

The current native state format is v4 in [`src/platform/native_state.c`](../src/platform/native_state.c). It serializes game BSS/data, EWRAM/IWRAM/common, registers, video memory, flash, framebuffer and several sidecars.

Pointer-bearing slices are normalized, but `HostPointerToPersistentAddress` accepts game-image data and rejects unmanaged host allocations. A pointer into a malloc-backed resource snapshot or compatibility arena will therefore fail save capture.

The current content fingerprint contains only the fixed Emerald ROM SHA-1. It does not identify pack contents, provider order or mods.

### Required state v5 design

Add a resource-reference sidecar with records containing:

```text
owning section tag
pointer field byte offset
resource key, 32 bytes
resource type and schema
representation role
offset within the immutable range
```

Roles include canonical payload, legacy-LZ payload and compatibility object/table.

During save:

1. Detect pointers inside registered resource ranges.
2. Record stable identity and offset.
3. Zero the in-band pointer before normal pointer processing.
4. Never serialize runtime resource handles.

During load:

1. Validate header and session fingerprint.
2. Resolve every sidecar identity against the already-built session.
3. Validate all references before mutating memory.
4. Restore normal sections.
5. Patch resource pointers.
6. Fail transactionally if any reference cannot resolve.

Resources themselves remain external immutable state and are not serialized.

### Session fingerprint

Use a raw 32-byte digest:

```text
SHA-256(
    "gen3-session-content-v1\0"
    || length-prefixed game ID and adapter version
    || RESOURCE_API_VERSION
    || base provider logical content digest
    || LE provider count
    || each content provider in ascending precedence:
         kind
         precedence
         length-prefixed ID and version
         logical content digest
)
```

The executable build ID remains a separate check. On mismatch, reject before restoration and report expected/current provider sets. No default force-load path.

The Brendan pair is normally decompressed immediately, so it is unlikely to persist source pointers. Still add targeted save-state tests during that migration. Complete the resource sidecar before overworld, maps, scripts or audio.

## 12. Renderer impact

No renderer architecture change is required.

The renderer consumes runtime BG VRAM, palettes, OAM, OBJ VRAM and sprite state. ROM_BASE resources should enter the existing Emerald loading path and produce identical runtime GBA state. The renderer does not need to know which provider supplied a tile.

Any save-state issue involving sprite resource pointers belongs to state/resource identity, not Stage 4.

## 13. Audio strategy

Audio is a later wave. The current engine preserves GBA-shaped song tables and hydrates four-byte logical addresses into host pointers.

The graph includes song headers, tracks, voicegroups, key splits, drumsets, samples, cries and internal pointers. Original ROM addresses cannot simply be copied into a pack and dereferenced.

Recommended order:

1. Keep audio compiled while graphics proves the architecture.
2. Define audio sample, cry, music-sequence and SFX schemas.
3. Migrate standalone sample payloads while structures remain compiled.
4. Build permanent runtime voicegroup/song graphs.
5. Migrate sequences after relocation and save-state coverage.

Preserve MP2K bytecode and sample encoding. Do not replace the audio engine with OGG/WAV playback.

## 14. Maps, text, scripts and data

| Family | Current representation | Recommendation |
| --- | --- | --- |
| Map blockdata/borders | Raw binary referenced by layouts | Good later resource target |
| Metatiles/attributes | Raw u16 arrays | Good later target with tileset constraints |
| Map layouts | Dimensions plus block/tileset pointers | Rebuild as compatibility objects |
| Map headers | Pointer-bearing generated assembly | Keep compiled initially |
| Events/connections | Records containing script/header pointers | Later typed data migration |
| Scripts | Bytecode with embedded addresses | Requires relocation records; defer |
| Dialogue | Charmap bytes referenced by scripts/tables | Move after text/script reference design |
| Encounters | Generated structs and pointers | Future data schema |
| Species/moves | Current aggregate ROM hydration | Keep, then migrate to typed resources |
| Items | Data, text and callbacks in one struct | Split before migration |
| Animation commands | Commands, pointer arrays and callbacks | Keep while graphics move independently |

Relocation metadata is internal to the game adapter. Public mod references remain canonical IDs.

## 15. Repository and source-tree strategy

### Short term

Keep the current fork during migration and parity work. For each family:

- retain source assets temporarily as a parity oracle
- remove them from the native dependency graph after parity succeeds
- retain them for the traditional GBA target
- mechanically prove their absence from native releases

Recommended layout:

```text
include/gen3/resources/
    resource_pack.h
    resource_pack_provider.h

src/gen3/resources/
    resource_pack_reader.c
    resource_pack_provider.c
    resource_content_digest.c

include/emerald/resources/
    emerald_rom_profile.h
    emerald_resource_session.h
    emerald_resource_compat.h

src/emerald/resources/
    emerald_rom_profile.c
    emerald_resource_import.c
    emerald_resource_session.c
    emerald_resource_compat_trainers.c

resources/extraction/emerald/bpee01/
    bindings.toml
    manifest.generated.toml
    ownership.generated.toml

tools/gen3_resources/
    elf_manifest/
    pack/
    verify/
    isolation/
```

Game-specific adapter code must not enter the shared pack reader or resolver.

### Long term

Prefer a native-port overlay with a pinned pret source dependency:

```text
Gen3Recomp shared infrastructure
├── resource/mod core
├── pack/import/tooling
└── platform/runtime

game adapter
├── Emerald catalog/extraction profile
├── compatibility layer
└── narrow native integration patches

pinned upstream pret source
└── traditional GBA source/build and reference metadata
```

A pinned submodule or reproducible source-fetch step plus an adapter/overlay is preferable to indefinitely vendoring all expressive assets in the native port repository. A raw patch stack alone is likely too brittle.

## 16. GBA versus native target

Maintain one engine with target-specific ownership:

```text
GBA target
    → current compile-time assets and const tables

Native target
    → ROM_BASE and runtime compatibility tables
```

Family-level declarative lists generate both forms. The GBA initializer references compiled symbols; the native initializer retains functional metadata and fills payload pointers from the resource session.

Do not fork battle, field, sprite, text or audio consumers.

CI should require:

1. Traditional GBA output still matches its expected ROM.
2. Shared resource tests pass.
3. Native build has no migrated asset dependency.
4. Migrated symbols and payload hashes are absent from the native executable.

## 17. Mod integration

Future precedence remains:

```text
BOOTSTRAP
    ↓
LEGACY_COMPILED, during migration only
    ↓
ROM_BASE
    ↓
enabled MOD providers in explicit order
```

The compatibility image is built from the final resolved snapshot, not directly from the base pack:

```text
mod decoded Brendan sheet
    → MOD provider wins
    → resolver view
    → literal-LZ compatibility payload
    → gTrainerFrontPicTable[BRENDAN]
    → unchanged Emerald consumer
```

Invalid optional mod entries fall back to ROM_BASE with trace evidence. Invalid required entries reject candidate construction. ROM_BASE has no special lookup path.

The old desktop content package eventually disappears as fonts, text and structured data acquire proper resource schemas.

## 18. First representative family

Use the existing Brendan pair.

| Property | Sheet | Palette |
| --- | --- | --- |
| Canonical ID | `emerald:trainer/brendan/battle/front/sheet` | `emerald:trainer/brendan/battle/front/normal-palette` |
| Source | `graphics/trainers/front_pics/brendan.4bpp.lz` | `graphics/trainers/palettes/brendan.gbapal.lz` |
| Symbol | `gTrainerFrontPic_Brendan` | `gTrainerPalette_Brendan` |
| Encoded size | 804 | 40 |
| Decoded size | 2,048 | 32 |
| Table index | `TRAINER_PIC_BRENDAN` | `TRAINER_PIC_BRENDAN` |

This pair exercises tile and palette resources, real battle consumers and compatibility tables without introducing persistent sprite-frame pointers, tileset callbacks or script relocation.

Success means both leaf payload symbols are absent from the native link while table indices and consumers remain unchanged.

## 19. Byte-parity strategy

### Encoded parity

```text
decomp-generated .lz
==
GBA ELF symbol bytes
==
verified ROM slice
```

### Canonical payload parity

```text
strictly decoded ROM slice
==
uncompressed decomp artifact
==
.rpack canonical payload
```

### Compatibility parity

```text
decompress(permanent compatibility LZ)
==
resolved canonical payload
```

### Runtime parity

Exercise the existing table entry through the normal loader and compare destination tile/palette bytes with the legacy result.

For structured data later, compare normalized little-endian serialization rather than host padding or pointer bytes.

## 20. Packaging and isolation proof

### Build graph

Generate `ownership.generated.toml` with canonical ID, legacy payload symbol, source artifacts, ownership state, hashes and allowed targets. The native build fails if a migrated source enters `scaninc`, generated dependencies or linked inputs.

### Symbol inspection

Forbid migrated leaf payload symbols in native objects and executables. Compatibility table symbols may remain.

### Binary scans

Scan release binaries for full encoded payloads, sufficiently distinctive decoded payloads and known family hashes. Treat this as defense-in-depth because small payloads can occur coincidentally.

### Link ownership report

Generate a machine-readable report dividing linked content into engine/functional data, bootstrap resources, ROM_BASE-only resources and temporary exceptions.

### Release archive inspection

Use an allowlist of binaries, libraries, licenses and port-original assets. Inspect debug symbols and loose files as well as the main executable.

The strongest proof combines dependency-graph enforcement, symbol absence and payload scans.

## 21. Incremental implementation stages

Every stage forbids Stage 4 work, FireRed implementation, a second resource abstraction, code mods, and release publication.

| Stage | Goal | Stop condition | Approximate scope |
| --- | --- | --- | --- |
| R0 — audit baseline | Authoritative ownership inventory | No behavior change | 1–2 days |
| R1 — metadata generator | ELF/ROM/binding generator for Brendan pair | Deterministic manifest; no pack/runtime | 2–4 days |
| R2 — pack codec | Shared writer/reader and logical digest | Synthetic tests and sanitizers; no Emerald | 4–7 days |
| R3 — local importer | Exact ROM validation and two-resource pack | Atomically installed valid pack; no provider | 3–5 days |
| R4 — ROM_BASE provider | Pack → normal M1 provider/snapshot | Provider tests; no tables | 2–4 days |
| R5 — Brendan compatibility | Literal-LZ arena and trainer entries | Parity and targeted state tests | 4–7 days |
| R6 — Brendan isolation | Remove two payloads from native link | Symbols/hashes absent; GBA unchanged | 2–4 days |
| R7 — trainer graphics | Complete trainer front/palette, then backs | Family parity and isolation | 1–2 weeks |
| R8 — Pokémon/global graphics | Mon tables, UI/title/intro/battle families | Special-form and family tests | 2–4 weeks |
| R9 — old-package convergence | Fonts first; typed data later | No second package at completion | 1–3+ weeks |
| R10 — resource-pointer states | State v5, fingerprint and sidecar | Cross-restart and mismatch tests | 1–2 weeks |
| R11 — overworld/tilesets | Clone pointer graphs and raw map blocks | State and visual parity | 2–5 weeks |
| R12 — audio | Samples/cries, then relocated MP2K graphs | Audio and state regressions green | 3–6 weeks |
| R13 — maps/text/scripts/data | Domain schemas and relocation | Separate domain milestones | Multi-month |
| R14 — final isolation | Release proof and source overlay | No migrated native dependencies | 2–6 weeks |

### Rollback boundaries

- R1–R4 do not alter Emerald behavior.
- R5 retains developer-only LEGACY comparison.
- R6 removes only Brendan's two leaf payloads.
- Broader work remains family-scoped.
- No release build may silently fall back to redistributed migrated payloads.

ROM_BASE is populated from user-verified ROM extraction. “Payloads from shipped data” is not the target architecture; legacy content is only a temporary parity oracle.

## 22. Risk register

| Risk | Mitigation |
| --- | --- |
| Generated offsets drift | Match ROM+ELF and verify artifact/ELF/ROM bytes |
| Assembly symbols have no size | End labels or emitted provenance; never arbitrary next-symbol inference |
| Wrong-sized mod replacement passes M1 | Catalog-owned exact constraints |
| Schema 1 is reinterpreted as compression | Freeze decoded graphics semantics |
| Compatibility compression differs | Compare decoded output; literal-only GBA-LZ |
| High candidate memory usage | Migrate incrementally; profile before immutable-backing optimization |
| Resource pointer enters state | State v5 resource-reference sidecar |
| Host pointer written into `GbaAddr` | Hydrate/clone all four-byte pointer graphs |
| Runtime handle becomes persistent | Serialize keys and offsets only |
| File-local resources resist replacement | Generated module compatibility slots |
| Corrupt pack | Strict bounds, hashes, caps and checked arithmetic |
| Interrupted/concurrent import | Same-directory atomic replace plus import lock |
| Old `content.pak` persists | Treat it as migration debt and retire it |
| Audio complexity underestimated | Defer and require generated relocation metadata |
| ROM offsets leak into mod API | Keep relocation internal; expose canonical IDs |
| GBA/native tables diverge | Generate both from shared descriptors; CI both |
| Release still embeds content | Dependency, symbol, link-map and payload gates |
| Source tree remains expressive | Temporary parity retention; long-term pret dependency/overlay |
| FireRed assumptions enter shared code | Generic pack profile; Emerald-specific adapter |
| Equivalent pack revision breaks states | Fingerprint logical provider content |
| Dimension-changing mod corrupts consumer | Exact catalog constraints; fixed v1 layouts |

## 23. Exact next DeepSeek prompt: R1 only

```text
Implement Stage R1A only in:

/home/tristen/work/pokeemerald-recomp

DO NOT implement a resource pack.
DO NOT implement ROM_BASE.
DO NOT change the M0/M1 resolver, providers, snapshots, IDs, hashing, or precedence.
DO NOT integrate Emerald runtime tables.
DO NOT modify desktop_game_content.c.
DO NOT launch the game.
DO NOT touch Stage 4.
DO NOT touch FireRed.
DO NOT publish anything.

GOAL

Implement a deterministic maintainer-side extraction-metadata generator for
exactly these two existing catalog resources:

1. emerald:trainer/brendan/battle/front/sheet
   GBA symbol: gTrainerFrontPic_Brendan
   source artifact:
   graphics/trainers/front_pics/brendan.4bpp.lz
   source encoding: GBA LZ77
   canonical representation: decoded GBA 4bpp tile bytes
   expected encoded size: 804
   expected decoded size: 2048

2. emerald:trainer/brendan/battle/front/normal-palette
   GBA symbol: gTrainerPalette_Brendan
   source artifact:
   graphics/trainers/palettes/brendan.gbapal.lz
   source encoding: GBA LZ77
   canonical representation: decoded GBA BGR555 palette bytes
   expected encoded size: 40
   expected decoded size: 32

AUTHORITATIVE CONTRACT

Read and follow:

docs/GEN3_RESOURCE_MOD_CONTRACT.md
docs/GEN3_RESOURCE_CURRENT_CONTENT_INVENTORY.md
resources/catalogs/emerald/catalog.toml
include/gen3/resources/
src/gen3/resources/

Canonical IDs and keys remain exactly as M0/M1 defines them:

SHA-256("gen3-resource-id-v1\0" + exact canonical-name bytes)

Do not normalize canonical names.
Do not use runtime handles as persistent identity.

IMPLEMENT

Create a narrowly scoped shared Gen3 extraction-manifest tool and an
Emerald/BPEE01 binding input.

Recommended locations:

tools/gen3_resources/elf_manifest/
resources/extraction/emerald/bpee01/bindings.toml
resources/extraction/emerald/bpee01/manifest.generated.toml

The binding input may contain semantic declarations such as canonical ID,
GBA symbol, source artifact, source encoding, canonical representation, and
expected decoded size.

It must NOT contain hand-maintained ROM offsets.

The generator must accept explicit paths for:

--catalog
--bindings
--elf
--rom
--output

It must:

1. Validate the ROM is exactly 16 MiB.
2. Validate game code BPEE, maker code 01, and software revision 0.
3. Validate SHA-1:
   f3ae088181bf583e55daf962a92bb46f4f1d07b7
4. Compute and record the ROM SHA-256.
5. Read the matching GBA ELF symbol table.
6. Require each symbol to be an allocated ROM object at or above 0x08000000.
7. Derive ROM offset from ELF st_value - 0x08000000.
8. Use ELF st_size for these two C INCBIN arrays.
9. Bounds-check every ROM range.
10. Verify source artifact bytes == ELF symbol bytes == ROM slice bytes.
11. Strictly decode GBA LZ77, rejecting truncation, invalid backreferences,
    overflow, over-read, and decoded-size mismatch.
12. Verify the decoded sheet equals:
    graphics/trainers/front_pics/brendan.4bpp
13. Verify the decoded palette equals:
    graphics/trainers/palettes/brendan.gbapal
14. Recompute the resource key using the existing exact M0/M1 algorithm.
15. Emit deterministic records sorted bytewise by canonical name containing:
    canonical name, diagnostic key hex, resource type, schema, GBA symbol,
    ROM offset, encoded length, decoded length, source encoding, source
    encoded SHA-256, and canonical decoded SHA-256.
16. Emit no timestamps, absolute paths, host ordering, pointer values, or
    filesystem paths used as identity.
17. Provide --check mode that fails if deterministic regeneration differs
    byte-for-byte from the checked-in manifest.

TESTS

Add standalone tests for:

- deterministic output
- catalog/binding order independence
- canonical key derivation through the existing resource ID code
- successful strict LZ decoding
- truncated LZ
- invalid backreference
- decoded-size mismatch
- missing ELF symbol
- zero-sized or non-ROM symbol
- ROM-range overflow
- artifact/ROM mismatch
- duplicate canonical ID
- duplicate range unless explicitly allowed
- output changes when authoritative metadata changes

Tests must not require a user-owned ROM. Use synthetic ELF/ROM fixtures or
fixture generation.

If the matching GBA toolchain is available, additionally run the generator
against the exact locally built pokeemerald.elf/pokeemerald.gba and report the
derived offsets and hashes. This is integration verification, not an ordinary
unit-test requirement.

Run relevant tests under ASan and UBSan where supported.

STOP BOUNDARY

Stop when:

- bindings exist only for the two Brendan resources
- the deterministic generated extraction manifest exists
- standalone tests pass
- optional exact-GBA verification is reported
- no pack writer/reader exists
- no ROM_BASE provider exists
- no launcher/import changes exist
- no Emerald compatibility tables or runtime behavior changed

FINAL REPORT

Report files changed, generator inputs/output, tests and sanitizers, exact-GBA
verification availability, generated offsets/sizes/hashes if verified, and any
blocker discovered.

Do not begin R2.
```

R1 answers the highest-risk question—automatic, verified extraction metadata—before committing to the pack or runtime integration.

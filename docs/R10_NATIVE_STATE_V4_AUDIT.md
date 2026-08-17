# R10-A — Native State v4 Audit

Scope: establish the exact current save-state format, extension seams, and every
way a resource-owned pointer can enter serialized game state. No code was
modified during this audit.

Audited: `src/platform/native_state.c` (2600+ lines, the whole format lives
here; `include/platform/native_state.h` is the public API only),
`src/platform/host_memory.c`, the R4/R5/R6/R9 resource session/provider/
compat chain (`src/gen3/resources/*`, `src/emerald/resources/*`), the loader
registration (`emerald_runtime_loader.c`), and the linker-defined ranges
(`ld_script_native.ld` + `src/platform/linux_runtime.c`).

## 1. State v4 header / layout

All in `src/platform/native_state.c`:

- `NATIVE_STATE_MAGIC` = `0x4E535431` ("NST1"), line 41.
- `NATIVE_STATE_FORMAT_VERSION` = `4u`, line 42.
- `NATIVE_STATE_ABI_ID` = `"linux64-v5"` (line 47) — an ABI string used as the
  **buildId** prefix. Pre-existing name; it is NOT the state format version.
- `NATIVE_STATE_CONTENT_FINGERPRINT` = `"f3ae088181bf583e55daf962a92bb46f4f1d07b7"`
  (line 49) — a **hardcoded compile-time constant** (64 hex chars). This is the
  field R10-F replaces with a computed resource-session fingerprint.

Header (lines 68–82, `__attribute__((packed))`), all fields little-endian:

| field | type | meaning |
|---|---|---|
| magic | u32 | 0x4E535431 |
| formatVersion | u32 | 4 |
| headerSize | u32 | sizeof(header) |
| totalSize | u32 | entire file size |
| sectionCount | u32 | ≤ 16 (line ~80 check) |
| payloadSize | u32 | payload byte count |
| payloadCrc | u32 | CRC32 over payload bytes only |
| reserved | u32 | zero (extension seam) |
| frame | u64 | frame counter |
| buildId[64] | char | ABI id + `Platform_RuntimeGetBuildId` |
| contentFingerprint[64] | char | hardcoded constant today |

File layout: `[header][sectionCount × section header][payload]`.
Section header = `{tag u32, size u32, crc u32}`. Write path (2247–2278):
header → section headers → payload, `payloadCrc = Crc32(file+sizeof(header), payloadSize)`.
Read path (2328–2379) walks the section headers; `HeaderMatches` (2167+) checks
magic, formatVersion, headerSize, totalSize, sectionCount bounds, payloadSize,
payloadCrc, buildId `strncmp`, contentFingerprint `strncmp`.

## 2. Every serialized memory section

13 sections, tags 1–13 (`STATE_SECTION_*`, lines ~55–67):

| tag | name | source | runtimePointers | capture special case |
|---|---|---|---|---|
| 1 | GAME_BSS | `Platform_RuntimeGetGameBssRange` (ld-script `game_bss`) | TRUE | normalize |
| 2 | REGISTERS | GBA register state | FALSE | raw |
| 3 | VIDEO_MEMORY | VRAM copy | FALSE | raw |
| 4 | FLASH | save flash | FALSE | raw |
| 5 | EWRAM | `__start/__stop_gba_ewram` | TRUE | normalize |
| 6 | IWRAM | `__start/__stop_gba_iwram` | TRUE | normalize |
| 7 | COMMON | `__start/__stop_gba_common` | TRUE | normalize |
| 8 | GAME_DATA | `Platform_RuntimeGetGameDataRange` (ld-script `game_data`) | TRUE | normalize |
| 9 | FRAMEBUFFER | `DISPLAY_WIDTH*DISPLAY_HEIGHT*4` | FALSE | raw |
| 10 | TASK_SIDECAR | `Task_GetStateSize()` | FALSE | `CaptureSlice` special |
| 11 | SPRITE_SIDECAR | `Sprite_GetStateSize()` | FALSE | `CaptureSlice` special |
| 12 | RTC | RTC state | FALSE | raw |
| 13 | BATTLE_SIDECAR | `sizeof(NativeStateBattleSidecar)` | FALSE | `CaptureSlice` special |

The GAME_BSS/GAME_DATA ranges come from `ld_script_native.ld`
(`__start/__stop_game_bss`, `__start/__stop_game_data`, `INSERT AFTER .data`),
which deliberately exclude the native executable `.bss/.data` including libc
COPY relocations (comment at BuildSlices, lines ~228–231). SectionCount ≤ 16,
so a new sidecar tag **14 fits without a header change**.

## 3. Existing sidecars

Three sidecars already exist; all store `struct HostPersistentAddress` records
(8 bytes: `{u32 kind, u32 value}`, `STATIC_ASSERT` size 8, host_memory.c):

- **TASK_SIDECAR**: `sTaskFollowupFuncs`, `sTaskStoredFunctions` — function
  pointers via stable function IDs (`HostRegisterPersistentFunction`).
- **SPRITE_SIDECAR**: `sSpriteStoredCallbacks` — same mechanism.
- **BATTLE_SIDECAR**: `struct NativeStateBattleSidecar { struct
  HostPersistentAddress commandBufferData[MAX_BATTLERS_COUNT]; }` — battle
  command-buffer DMA3 pointer, 8-byte persistent records.

Record walk idiom: 8-byte records at **4-byte stride** (bounds-checked with
`sizeof(u64)` per record) — this is the established in-band pointer-slot
convention v5 extends for resource references.

## 4. Pointer normalization workflow

- `BuildSlices` sets `runtimePointers = TRUE` for GAME_BSS/EWRAM/IWRAM/COMMON/
  GAME_DATA; `CaptureSlice` (2054+) copies the slice to a **staged buffer**
  first, then `NormalizeRuntimeBytes(slice, dest, size)` — capture never
  mutates live memory.
- `NormalizeRuntimeBytes` (1662–1830) walks 4-byte words of the staged copy:
  1. word whose value is a **known function location** → `HostFunctionToPersistentAddress` (8-byte record with stable function ID);
  2. word at a **known data location** (`RuntimeLocationIsKnownDataPointer`:
     `sSpriteTemplateSidecars` oam/anims/images/affineAnims,
     `sSpriteTemplateImageSidecars.data`, `gSprites` anims/images/affineAnims/
     template/subspriteTables, `gMonSpritesGfxPtr`, battle runtime buffers) →
     `HostPointerToPersistentAddress`;
  3. everything else → **opaque**, copied verbatim.
- Final gate: `ValidateNoRuntimeHandles` (1526–1567) rejects only words whose
  top 16 bits are `HOST_HANDLE_BASE` (0xF0000000) or
  `HOST_FUNCTION_HANDLE_BASE` (0xE0000000) **with a live registered index**.
  Raw 64-bit pointers (FILE*, heap) pass untouched.
- `RestoreRuntimeBytes` (1985–2070) decodes 8-byte records at 4-byte stride
  (skipping battle-buffer locations) and revalidates.
- Error reporting: `SetRuntimePointerError` (1420–1520) prints phase, section,
  section offset, **nearest symbol to the FIELD address**
  (`DescribeRuntimeLocation` resolves `slice->source + offset` — the field, not
  the pointer value), owner, field, raw pointer, classification, a ±16-byte
  window, handle tables, and sidecar dumps.

## 5. HostPointerToPersistentAddress behavior

`host_memory.c:122+`. On LINUX64:

- NULL → `{kind 0, value 0}`, OK.
- Pointer inside the **game image range** (`Platform_RuntimeGetImageRange` =
  `__executable_start.._end`, non-executable) → `HOST_PERSISTENT_DATA_IMAGE`
  with `u32` image offset.
- 32-bit value → `HOST_PERSISTENT_DATA_LOGICAL`.
- **Heap pointer (outside image range) → returns FALSE.** It cannot be
  encoded; capture of a modeled field holding a heap pointer fails.

This is the load-bearing fact for R10: the only reason migrated-table heap
pointers serialize today is that those fields are NOT in the known-location
set and pass as opaque words (see §9).

## 6. Runtime-handle encoding

`host_memory.c:11–14`: `HOST_HANDLE_BASE 0xF0000000`,
`HOST_FUNCTION_HANDLE_BASE 0xE0000000`, `HOST_HANDLE_INDEX_MASK 0x0000FFFF`
(64K capacity). Handles are transient index-based IDs in `sHostPointers`;
`HostAddressIsRegisteredRuntimeHandle` (405–418) is the only thing
`ValidateNoRuntimeHandles` rejects. Handles are explicitly transient — never
persistent identity (the directive's invariant is already enforced by the
final gate, which v5 keeps).

## 7. Executable / build-ID checks

`HeaderMatches` compares the header `buildId` against
`NATIVE_STATE_ABI_ID "linux64-v5"` + `Platform_RuntimeGetBuildId()` via
`strncmp`. Executable identity is a separate compatibility check from content
identity — R10-F preserves this split (fingerprint replaces only the content
side).

## 8. Existing ROM/content fingerprint

`contentFingerprint[64]` is compared against the hardcoded
`NATIVE_STATE_CONTENT_FINGERPRINT` constant — identical for every process of
every build; it encodes nothing about actual resource content. R10-F replaces
this with the computed session fingerprint written at save and checked at
load. `EmeraldResourceSessionInfo` already exposes the required inputs
(`providerId`, `providerVersion`, `kind`, `precedence`, `basePackVersion`,
`catalogVersion`, `extractionManifestVersion`,
`canonicalRepresentationVersion`, `gameId`,
`providerContentDigest[32]`, `logicalContentDigest[32]`, flags, entryCount) —
its header comment anticipates "future session fingerprint bookkeeping (R4:
digests exposed without a save-state v5)".

## 9. Resource-owned ranges that can enter serialized state

Three resource-world ranges exist at runtime:

1. **Trainer compat arena** — one `EmeraldResourceCompatibilityImage`
   (malloc'd, session-lifetime) holding 236 trainer-family entries. Referenced
   by `gTrainerFrontPicTable`/`gTrainerPaletteTable[71].data` and
   `gTrainerBackPicPaletteTable[0].data`.
2. **Pokémon compat arena** — one image holding 1804 battle entries (front/
   back/palette/shinyPalette families). Referenced by
   `gMonFrontPicTable/BackPicTable/PaletteTable/ShinyPaletteTable` row
   `.data` fields.
3. **Canonical provider payload bytes** — consumed transiently during image
   build; **no game state references them** post-init, so no canonical
   pointers exist in serialized slices today (the canonical role is defined
   in v5 for completeness/future use).

### How the compat pointers enter slices — the decisive placement finding

Verified against the current DINFO binary (`nm`):

- `__start_game_data = 0x1b5f8ec`, `__stop_game_data = 0x1b5f92c` — the
  GAME_DATA slice is **64 bytes** (a single `WAV` symbol). The `gba_data`
  section (ld_script_native.ld) is opt-in and almost nothing opts in on
  native.
- `gMonFrontPicTable = 0x12964a0`, `gTrainerFrontPicTable = 0x1298700` — the
  migrated battle tables live in the **executable's ordinary host `.data`**,
  outside GAME_DATA and outside every serialized slice.

Consequences, all verified:

1. **No resource-owned pointer enters serialized game state today.** The
   compat-arena pointers (published into the table rows at init) are never
   walked by `NormalizeRuntimeBytes`, never round-tripped, and can never go
   stale across processes. Cross-restart is safe by placement: a fresh
   process re-publishes from its own session image at init; the R6
   post-load republish is idempotent belt-and-braces.
2. Had the rows been in GAME_DATA, capture would FAIL — `NormalizeRuntimeBytes`
   rejects unmanaged host pointers: compat arenas are `malloc`'d mmap
   allocations in `[0x50TB, 0x80TB)` (verified by probe), which satisfies
   `LooksLikeExternalHostPointer` (`mincore`-mapped) →
   "state contains unmanaged native pointer". This is exactly the hazard
   State v5 must make safe for future migrations that do place
   resource-backed pointers in game-owned memory (e.g. the R11 overworld
   tables will live in game `.data` and will hold resource pointers).
3. Battle runtime structs (`gMonSpritesGfxPtr` = `AllocZeroed` on the game
   heap, image range; `frameImages` point into decompressed gfx buffers)
   hold only image-range pointers — modeled known locations, encodable today.
4. The v4 container is therefore **not currently exposed** to resource
   pointers; v5 adds the general capture/load mechanism (sidecar records,
   reverse range index, fingerprint, transactional load) so the first
   resource-pointer-bearing slice is handled correctly from day one.

The earlier R5-era claim that the tables serialize "in the GAME_DATA slice
as opaque 4-byte words" described the pre-ld-script era range
(`[__bss_start+8, FLASH_BASE)`, which did include the tables); the Aug 13
linker script (b7a74b43f) moved them out of every serialized range.

Compiled rows that are NOT migrated (`gMonStillFrontPicTable`, the external
back-EGG row `back[412]`, GBA-logical pointers) hold image-range/GBA values
and remain ordinary pointer handling — unchanged by v5.

### Arena layout determinism (basis for stable offsets)

`EmeraldResourceCompatibilityImage` layout is computed from payload sizes
(`emerald_resource_compat.c:284–319`): `entryTableOffset = 0`,
`nameArenaOffset = entryTableSize`, then `canonicalArenaOffset`,
`streamArenaOffset`, all 4-aligned and deterministic per content. Entries
carry name/canonical/stream offsets relative to `bytes[]`. Therefore
**arena-relative stream offsets are stable across processes** for identical
content — a sidecar `(key, role, offset-in-range)` resolves deterministically
in any process whose session content matches.

## 10. Extension seams (v5 insertion points)

1. `formatVersion` 4→5 — v5 is distinguishable before any section parsing;
   a v4 reader rejects v5 at `HeaderMatches`, and v5 rejects v4 explicitly
   (§I decision).
2. New section tag **14 `STATE_SECTION_RESOURCE_SIDECAR`** — sectionCount ≤ 16
   accommodates it with no header change. Sidecar is its own crc-protected
   section; v4 files have no tag-14 section so no v4 bytes can parse as v5
   sidecar records.
3. `contentFingerprint[64]` — the hardcoded constant becomes a computed
   session fingerprint written at save / compared at load (64 hex chars =
   exactly one SHA-256).
4. Capture seam: `NormalizeRuntimeBytes` (detect resource-range pointers,
   emit sidecar records, zero in-band). Load seam: pre-restore validation
   phase (fingerprint + sidecar parse + resolve EVERY reference) before the
   first section mutation; post-restore patch pass + existing republish
   lifecycle.
5. Resource identity inputs exist: `Gen3Sha256` (src/gen3/resources/sha256.c),
   `Gen3ResourceSnapshot` registry (active snapshot, key/canonicalName/type/
   schema per view, `RESOURCE_API_VERSION` in
   include/gen3/resources/resource_version.h), the compat images expose
   per-entry key/type/schema (`entries[i].canonicalName = r->id` in
   emerald_pokemon_native_compat.c Phase 2).

## Audit conclusions

- v5 needs **one new section tag**, a **version bump**, and a **computed
  fingerprint** — the container itself is otherwise unchanged.
- **No resource-owned pointer exists in serialized state today** (placement
  finding, §9): the v4 walker would hard-fail on one, and the container never
  sees the migrated tables. v5's job is to make the first such pointer safe:
  today those are the compat-arena streams (heap, `0x7f…`); v5 serializes
  them as zeroed in-band words + sidecar records. All existing pointer
  classes (image-range, logical-32, function IDs, handles, opaque GBA words)
  keep their current handling.
- The existing staged-capture (normalize-on-copy) and forensic-dump
  infrastructure carries over directly; load needs a whole-state validation
  phase before the first live-memory write (today's per-section restore is
  not transactional across sections).
- The real-battle-resource test (H) must arrange a resource pointer inside a
  walked slice through the real machinery (real pack, real tables, real
  walker), since no in-game path produces one today.

# R11-B — Object-Event Graphics Migration Design

## 1. Scope and shape

Migrate the overworld object-event EXPRESSIVE graphics to ROM_BASE:

- 253 raw 4bpp pic sheets (`gObjectEventPic_*`) + 35 gbapal palettes
  (`gObjectEventPal_*`) — the leaf payloads.
- The frame tables (`sPicTable_*` SpriteFrameImage arrays, 249 arrays /
  1,690 frames) stay COMPILED on the native link (metadata) but become
  non-const, and their `.data` fields are published to session arena
  streams at init — exactly the R9 battle-table pattern. No struct
  cloning is needed: the compiled tables ARE native-width host
  representations on LINUX64 (C-compiled per target), and hydration =
  publishing session payload pointers into them.
- `ObjectEventGraphicsInfo` structs, `oam`/`subspriteTables`/`anims`/
  `affineAnims` pointers, anim cmd sequences, berry-tree master tables:
  all compiled data (functional/animation metadata), NOT migrated.
- The centralized seam stays `GetObjectEventGraphicsInfo`
  (event_object_movement.c:1917); the migration publishes behind it, no
  call-site changes.

## 2. Resource families

Canonical id patterns (descriptor-driven, like the trainer family):

- `emerald:object-event/<canonical>/sheet` — type `sprite-sheet`,
  schema 1, canonical payload = the raw 4bpp bytes verbatim
  (`source_encoding "raw"`; no LZ77 anywhere — the consumers read frames
  straight out of the sheet).
- `emerald:object-event/<canonical>/palette` — type `palette`, schema 1,
  32 decoded bytes (16 u16), `source_encoding "raw"`.

288 resources total. Canonical component = ASCII-lowercase of the pic/pal
symbol suffix with `_` → `-` (the trainer rule).

## 3. Descriptor + generated artifacts (L)

New `resources/extraction/emerald/bpee01/object_event_family.toml`:
per-sheet records {canonical, slug (graphics/object_events/pics/...),
symbol (gObjectEventPic_<X>), size}; per-palette records {canonical,
symbol, is_reflection flag}; plus the frame-consumer table generated from
the qualified GBA ELF: for each `sPicTable_*` frame array, each frame's
{frame array symbol, frame index, sheet symbol, width, height, frame}
(the generator walks the compiled SpriteFrameImage arrays in the ELF —
the same provenance discipline as trainer_front_consumers).

New generator `tools/gen3_resources/object_event_family/gen_object_event_family`
emits: catalog additions (288), bindings.generated.toml,
ownership.generated.toml, object_event_consumers.generated.toml. The
elf_manifest tool extends to the new ids (rom_offset/encoded_length/
source_encoding "raw"/sha256s from the qualified ELF + retail-matching
ROM — provenance chain unchanged).

## 4. Compat seam (new module)

`src/emerald/resources/emerald_object_event_compat.c` (+ header),
following the R9 pokémon seam contract:

- `EmeraldObjectEventCompat_TryInitialize(snapshot, diagnostics)`:
  resolve + verify ALL 288 resources (type/schema/winner==ROM_BASE/size
  from the bindings), build ONE `EmeraldResourceCompatibilityImage` with
  EMERALD_COMPAT_ENTRY_RAW entries (payload verbatim — the consumers read
  raw bytes), then publish:
  - each sheet stream pointer into the compiled frame tables via the
    generated consumers table (frame .data = sheet stream + offset, where
    offset = width*height*frame*32 — re-derived from the generated
    metadata, never from a compiled base),
  - each palette into the native `gObjectEventPal_*` tables (non-const on
    native, NULL sentinel on the GBA-less rows — the R8S4a pattern),
  - berry-tree pic table pointers (`gBerryTreePicTablePointers`,
    `gBerryTreeObjectEventGraphicsIdTablePointers`,
    `gBerryTreePaletteSlotTablePointers`) and the Mauville old-man tables
    are compiled identity tables pointing at the same compiled frame
    tables — nothing further needed once the frame tables are published.
- Transactional: resolve/verify/build ALL before any table write;
  publication changes ONLY migrated `.data` fields (indices, sizes,
  tags, and every non-migrated field stay identical).
- `Republish` (allocation-free, idempotent, post-state-load),
  `ClearMigratedEntries` (NULL sentinels), `Shutdown`; driven by the
  trainer seam's lifecycle (InitializeFromSnapshot delegates to the new
  family like the pokémon one; strict by default — a pack that cannot
  serve the object-event family is a refused session).
- Range registration: the 288 streams register with the R10 reverse
  index under **ROLE_CANONICAL (0)** — the streams are byte-identical
  copies of the canonical payloads (RAW entries, no LZ transformation),
  so the representation semantics are "canonical payload bytes", not
  "legacy/literal-LZ stream" (that role names the LZ re-encoded battle
  streams) and not "compatibility object/table" (that names objects, not
  payloads). No State v5 format change: the persisted role field already
  permits 0-2 and capture/resolution are role-agnostic. Any future
  pointer into these streams that enters a serialized slice is therefore
  captured as identity+offset with role CANONICAL. Today no such pointer
  exists: the published `.data` fields live in host `.data` (outside
  serialized slices — the R10-A placement finding), and `gSprites[i].
  images` continues to reference the compiled frame tables (image range,
  already encodable by the walker).

## 5. Native data changes

- `src/data/object_events/object_event_graphics.h`: the 253 sheet + 35
  palette INCBIN leaves become GBA-only; the native branch declares
  non-const `u32/u16` tables published by the seam (the R9 leaf-removal
  pattern: native rows NULL sentinel, sizes retained).
- The frame arrays (`object_event_pic_tables.h`) become non-const on
  native (mutable `.data`), layout/sizes unchanged.
- `GetObjectEventGraphicsInfo` and all consumers unchanged.

## 6. R10 State v5 (K)

- All new persistent resource pointers (published frame `.data`, palette
  tables) live in host `.data` — never serialized; cross-restart restores
  them via the existing post-load republish chain (extended to the
  object-event family). Walked-slice fields (gSprites images/anims etc.)
  remain image-range pointers.
- The 288 arena streams register with the range index so capture
  RECOGNIZES any resource pointer that ever does enter a slice; the
  cross-restart state harness gains an overworld case proving republish
  after load re-derives frame pointers at new arena addresses.

## 7. Isolation (M)

- Native link loses all 253 sheet + 35 palette payload bytes; symbol
  absence + binary scan + no-fallback proofs via the isolation runner
  (extended). GBA build unchanged (leaves stay INCBIN).
- Compiled frame arrays / info structs / anim tables remain (structural
  metadata — allowed).

## 8. Tests (N)

- resolution of all 288 (winner/size/type), transactional init
  (no partial publish), frame hydration (every frame .data == stream +
  width*height*frame*32; byte-parity vs the pack payloads), palette
  parity, lifecycle (stale/republish/clear), berry-tree variant tables,
  state cross-restart with republish, sanitizer variants.

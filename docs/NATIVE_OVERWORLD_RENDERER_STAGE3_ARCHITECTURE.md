# Native overworld renderer Stage 3 architecture

Research date: 2026-08-14

Scope: native sprite/OBJ rendering at 240x160. This document is architecture
and implementation planning only; it does not implement the renderer.

## Executive decision

Use a hybrid capture:

- Final hardware `OAM` remains the authoritative 240x160 oracle and fallback
  source.
- A latched, pre-wrap command stream records the higher-level origin of each
  emitted OAM primitive.
- Current OBJ VRAM and OBJ palette RAM provide pixels.
- The command stream is double-buffered and committed with `LoadOam()`, because
  host-time `gSprites` is already one simulation frame ahead of the OAM being
  presented.
- Final packed OAM coordinates must never become the only stored position.
- Screen-culled but still-instantiated object events may be retained as
  expansion candidates, but Stage 3 must not create or stream additional object
  events.

This gives exact 240x160 parity without blocking later 300x200, 360x240, 16:9,
or ultrawide rendering.

## 1. Existing Emerald sprite pipeline

### 1.1 Creation and graphics loading

Sprite graphics and palette descriptions originate in:

- `struct SpriteTemplate`
- `struct SpriteSheet` / `struct CompressedSpriteSheet`
- `struct SpritePalette` / `struct CompressedSpritePalette`
- `struct SpriteFrameImage`

Their definitions are in `include/sprite.h`.

`CreateSprite()` or `CreateSpriteAtEnd()` calls `CreateSpriteAt()` in
`src/sprite.c`. That function:

1. Allocates one of 64 `gSprites` entries.
2. Copies the template's `OamData`.
3. Stores `x`, `y`, callback, animation tables, and subpriority.
4. Calls `CalcCenterToCornerVec()`.
5. Resolves the sprite sheet tile allocation:
   - A tagged sheet uses `GetSpriteTileStartByTag()`.
   - An image-table sprite allocates OBJ tiles directly.
6. Allocates an affine matrix through `InitSpriteAffineAnim()` when needed.
7. Resolves the palette tag to `oam.paletteNum`.

`LoadSpriteSheet()` copies materialized graphics into `OBJ_VRAM0`.
`LoadSpritePalette()` ultimately calls `LoadPalette()` for the OBJ palette
region. Compressed variants decompress before reaching the same runtime memory.

The renderer should not decode source sprite assets. By presentation time,
Emerald has already resolved:

- Animation frame
- Sheet allocation
- Dynamic image upload
- Palette slot
- Weather-adjusted/reflection palette
- Affine matrix
- Flip state

### 1.2 Per-frame gameplay and animation update

The overworld frame sequence is `OverworldBasic()` in `src/overworld.c`:

```text
ScriptContext_RunScript
-> RunTasks
-> AnimateSprites
-> CameraUpdate
-> UpdateCameraPanning
-> BuildOamBuffer
-> UpdatePaletteFade
-> UpdateTilesetAnimations
-> DoScheduledBgTilemapCopiesToVram
```

`AnimateSprites()` visits `gSprites[0..63]`. For every in-use sprite it:

1. Calls the sprite callback.
2. If the callback did not destroy the sprite, calls `AnimateSprite()`.

The callback updates logical/presentation fields including:

- `x`, `y` (`pos1` in later decomp terminology)
- `x2`, `y2` (`pos2`)
- `invisible`
- `subpriority`
- `oam.priority`
- affine mode/matrix
- object mode
- linked field-effect position

`AnimateSprite()` then advances:

- Frame animation and `oam.tileNum`
- Non-affine H/V flips through `SetSpriteOamFlipBits()`
- Affine animation and `gOamMatrices`

Image-table sprites enqueue a `RequestSpriteFrameImageCopy()` rather than
copying OBJ pixels immediately.

`UpdateCameraPanning()` then updates:

```c
gSpriteCoordOffsetX = gTotalCameraPixelOffsetX - sHorizontalCameraPan;
gSpriteCoordOffsetY = gTotalCameraPixelOffsetY - sVerticalCameraPan - 8;
```

### 1.3 Object-event visibility and vanilla screen culling

Ordinary player/NPC/object-event callbacks eventually call:

```text
UpdateObjectEventVisibility
-> UpdateObjectEventOffscreen
-> UpdateObjectEventSpriteVisibility
```

`UpdateObjectEventOffscreen()` computes a signed top-left and bottom-right
using:

```c
sprite->x + sprite->x2 + sprite->centerToCornerVecX
sprite->y + sprite->y2 + sprite->centerToCornerVecY
```

plus `gSpriteCoordOffsetX/Y` when `coordOffsetEnabled` is set.

It marks an object offscreen outside the GBA-sized bounds plus 16 pixels of
padding. `UpdateObjectEventSpriteVisibility()` then sets:

```c
sprite->invisible = objectEvent->invisible || objectEvent->offScreen;
```

This is information loss: the single `Sprite.invisible` bit does not
generically distinguish intentional hiding from vanilla viewport culling.
`ObjectEvent.offScreen` retains that distinction for object-event-owned
sprites, but arbitrary sprite callbacks may assign `invisible` directly.

### 1.4 OAM construction

`BuildOamBuffer()` performs:

```text
UpdateOamCoords
-> BuildSpritePriorities
-> SortSprites
-> AddSpritesToOamBuffer
-> CopyMatricesToOamBuffer
```

#### Coordinate serialization

`UpdateOamCoords()` assigns:

```c
sprite->oam.x =
    sprite->x + sprite->x2 + sprite->centerToCornerVecX
    + optional gSpriteCoordOffsetX;

sprite->oam.y =
    sprite->y + sprite->y2 + sprite->centerToCornerVecY
    + optional gSpriteCoordOffsetY;
```

But `OamData.x` is a 9-bit unsigned bitfield and `OamData.y` is an 8-bit
unsigned bitfield. The assignment immediately reduces coordinates modulo 512
and modulo 256. This is the exact pre-wrap boundary.

#### Sorting and subpriority

`BuildSpritePriorities()` produces:

```c
sprite->subpriority | (sprite->oam.priority << 8)
```

`SortSprites()` orders sprite owners so that:

1. Lower OBJ priority number is emitted earlier.
2. Within a priority, lower subpriority is emitted earlier.
3. At equal priority/subpriority, the sprite with greater interpreted Y is
   emitted earlier.
4. Exact ties retain the existing stable `sSpriteOrder`.

The Y comparison already uses wrapped `oam.y`, with special handling for large
double-size affine sprites.

Subpriority is not a hardware compositing field. Its only presentation effect
is the OAM index chosen by Emerald's sort.

#### Subsprite expansion

`AddSpriteToOamBuffer()` either emits the sprite's OAM record directly or calls
`AddSubspritesToOamBuffer()`.

Subsprites receive:

- Parent OAM attributes
- Per-part shape and size
- `tileNum + tileOffset`
- Per-part priority unless `SUBSPRITES_IGNORE_PRIORITY`
- Positions derived from the parent and per-part offset
- Mirrored per-part offsets for non-affine parent flips

Each subsprite consumes a distinct OAM index. The table's earlier part receives
the lower index and wins same-priority overlap.

This path is heavily used by ordinary overworld graphics, including normal
16x32 objects and large/asymmetric graphics. Subsprite support is not optional
even in the first useful Stage 3 renderer.

### 1.5 Matrix serialization

`CopyMatricesToOamBuffer()` writes each `gOamMatrices[i]` 8.8 fixed-point matrix
into the `affineParam` fields of OAM entries `4i..4i+3`.

The same OAM memory simultaneously contains:

- Sprite attributes in words 0-2 of every entry
- Affine matrix components in word 3 of selected entries

### 1.6 VBlank presentation commit

`VBlankCB_Field()` runs:

```text
LoadOam
-> ProcessSpriteCopyRequests
-> ScanlineEffect_InitHBlankDmaTransfer
-> FieldUpdateBgTilemapScroll
-> TransferPlttBuffer
-> TransferTilesetAnimsBuffer
```

`LoadOam()` copies `gMain.oamBuffer[128]` into final hardware-storage `OAM`.

`ProcessSpriteCopyRequests()` then materializes image-table animation frames in
OBJ VRAM. Palette transfer follows later in the same VBlank.

In the desktop scheduler, `Platform_VideoDrawFrame()` runs while the worker is
blocked at the next `VBlankIntrWait`. Consequently:

- `OAM`, OBJ VRAM, OBJ palette RAM, and display registers describe the
  currently presented frame.
- `gSprites`, `gOamMatrices`, and `gMain.oamBuffer` have already been advanced
  by the next `OverworldBasic()` and describe the pending frame.

A host-time walk of `gSprites` is therefore not presentation-coherent.

### 1.7 Pixel rendering

The current GBA oracle in `src/platform/gba_easy_draw.c`:

1. Iterates final OAM from index 127 down to 0.
2. Rejects disabled/prohibited records.
3. Derives size from shape/size.
4. Canonicalizes packed screen coordinates for the 240x160 display.
5. Applies affine transformation or non-affine flips.
6. Applies OBJ mosaic when enabled.
7. Computes OBJ tile addressing from `DISPCNT_OBJ_1D_MAP`.
8. Samples OBJ VRAM.
9. Treats pixel index zero as transparent.
10. Processes object-window or semi-transparent modes.
11. Stores the winning OBJ pixel in one of four OBJ priority layers.
12. Composites BG and OBJ priority layers with window/blend rules.

The complete actual pipeline is:

```text
logical object / field effect / weather task
-> SpriteTemplate + OBJ resource allocation
-> gSprites logical state
-> AnimateSprites
   -> sprite callback
   -> AnimateSprite / affine animation
-> CameraUpdate / UpdateCameraPanning
-> BuildOamBuffer
   -> coordinate packing in UpdateOamCoords
   -> subpriority/Y sort in SortSprites
   -> subsprite expansion in AddSubspritesToOamBuffer
   -> gMain.oamBuffer
   -> matrix serialization
-> VBlankCB_Field
   -> LoadOam -> final OAM
   -> ProcessSpriteCopyRequests -> presented OBJ VRAM
   -> TransferPlttBuffer -> presented OBJ palette
-> DrawSprites
-> BG/OBJ/window/blend composite
-> framebuffer
```

## 2. Exact capture-point recommendation

Capture two phases.

### Pending command construction

During `BuildOamBuffer()`, generate an immutable pending command frame from the
same sorted sprite/subsprite emission that creates `gMain.oamBuffer`.

For each emitted primitive, retain:

- Actual destination OAM index
- Exact packed OAM attributes
- Signed pre-wrap top-left
- Source `gSprite` index
- Parent/subsprite identity
- Emerald subpriority and emission order
- Matrix values used
- Optional object-event association
- Coordinate-space classification

Do not independently re-walk and re-sort `gSprites` later.

### Presentation commit

Only publish the pending command frame when `LoadOam()` actually performs its
`CpuCopy32()`.

If `gMain.oamLoadDisabled` prevents the OAM copy, the command frame must not
advance either.

At host snapshot time, copy together:

- Published command frame
- Final `OAM[128]`
- Full `OBJ_VRAM0` region
- OBJ palette RAM
- `DISPCNT`
- `MOSAIC`
- `BLDCNT`, `BLDALPHA`, `BLDY`
- Window registers relevant to OBJ/windowing
- Existing BG/map snapshot

This matches the command stream to the exact OAM/VRAM/palette state read by
`DrawFrame()`.

## 3. Final-OAM-only risk analysis

| Option | 240x160 parity | Offscreen preservation | Reconstruction coverage | Complexity | Future viewports |
|---|---:|---:|---:|---:|---:|
| A. Final OAM only | Excellent | Poor | Excellent for what reached OAM | Low | Unsuitable |
| B. `gSprites` only | Risky because of timing, culling, sorting, subsprites, and raw OAM writers | Better positions, incomplete visibility cause | Requires reproducing all OAM construction | Medium/high | Incomplete |
| C. `gSprites` + matrices + OBJ memory | Better than B | Still loses callback-culled sprites and has frame-latch mismatch | Most sprite-system objects | High | Better, but not parity-authoritative |
| D. Pre-OAM command stream | Excellent if generated in the actual emitter and latched with OAM | Good for emitted sprites | Needs raw-OAM escape handling | Medium | Good |
| E. Final OAM + pre-wrap commands | Best | Best available without Stage 5 streaming | Covers sprite-system and low-level OAM cases | Medium | Recommended |

### Coordinate ambiguity examples

For X, OAM stores:

```text
packedX = intendedX mod 512
```

For Y:

```text
packedY = intendedY mod 256
```

| Intended position | Packed OAM | GBA 240x160 interpretation | Information lost |
|---:|---:|---:|---|
| X = -20 | 492 | 492 >= 240, so interpreted as -20 | Could also have intended X=492 |
| X = 280 | 280 | Interpreted as -232 | Cannot know it was 280 to the right |
| X = 520 | 8 | Interpreted as 8 | Indistinguishable from X=8 |
| Y = -20 | 236 | 236 >= 160, so interpreted as -20 | Could also have intended Y=236 |
| Y = 200 | 200 | Interpreted as -56 | Cannot know it was below the screen |

A 360-wide view centered on the original view has a 60-pixel left/right
extension. An NPC at original screen X=-20 should appear at expanded X=40. Raw
OAM contains 492; treating that as its real position produces X=552.
Conversely, an NPC at X=280 should appear at expanded X=340, but the 240x160
interpretation turns it into -232.

Final OAM preserves enough for the modulo-and-clip behavior of the original
viewport. It does not preserve a unique position for an expanded viewport.

### Exact pre-wrap fields

`struct Sprite` preserves the components in:

- `x`, `y`
- `x2`, `y2`
- `centerToCornerVecX`, `centerToCornerVecY`
- `coordOffsetEnabled`
- `gSpriteCoordOffsetX`, `gSpriteCoordOffsetY`

For a non-subsprite:

```text
signedTopLeftX =
    x + x2 + centerToCornerVecX
    + (coordOffsetEnabled ? gSpriteCoordOffsetX : 0)

signedTopLeftY =
    y + y2 + centerToCornerVecY
    + (coordOffsetEnabled ? gSpriteCoordOffsetY : 0)
```

For a subsprite, the intended signed position should be derived from the
unwrapped parent anchor plus the transformed subsprite offset, while the actual
packed OAM record remains the 240x160 authority.

## 4. Recommended snapshot and command-stream structure

Keep the existing map snapshot stable and append a bounded OBJ presentation
section or reference a separate embedded `NativeObjSnapshot`.

A suitable design is:

```c
enum NativeSpriteSourceKind {
    NATIVE_SPRITE_SOURCE_GSPRITE,
    NATIVE_SPRITE_SOURCE_RAW_OAM,
    NATIVE_SPRITE_SOURCE_FUTURE_PROVIDER,
};

enum NativeSpritePlacement {
    NATIVE_SPRITE_PLACEMENT_WORLD,
    NATIVE_SPRITE_PLACEMENT_CAMERA_RELATIVE,
    NATIVE_SPRITE_PLACEMENT_SCREEN_FIXED,
    NATIVE_SPRITE_PLACEMENT_UNKNOWN,
};

enum NativeSpriteVisibility {
    NATIVE_SPRITE_GBA_EMITTED,
    NATIVE_SPRITE_VANILLA_VIEWPORT_CULLED,
    NATIVE_SPRITE_INTENTIONALLY_HIDDEN,
    NATIVE_SPRITE_HIDDEN_UNKNOWN,
};

struct NativeSpriteDrawCommand {
    u32 commandId;
    u32 emissionOrdinal;

    u8 sourceKind;
    u8 spriteId;
    u8 objectEventId;
    u8 partIndex;

    u8 oamIndex;
    u8 placement;
    u8 visibility;
    u8 expandedEligible;

    s32 signedX;
    s32 signedY;
    s32 worldX;
    s32 worldY;

    u16 tileNum;
    u8 shape;
    u8 size;
    u8 priority;
    u8 subpriority;
    u8 paletteNum;
    u8 bpp;
    u8 objMode;
    u8 affineMode;
    u8 mosaic;
    u8 flipX;
    u8 flipY;

    s16 pa;
    s16 pb;
    s16 pc;
    s16 pd;
};
```

The containing snapshot should carry:

```c
struct NativeObjSnapshot {
    u32 schemaVersion;
    u64 presentationSequence;

    u16 dispCnt;
    u16 mosaic;
    u16 bldCnt;
    u16 bldAlpha;
    u16 bldY;
    u16 win0H, win0V, win1H, win1V, winIn, winOut;

    u16 logicalViewportWidth;
    u16 logicalViewportHeight;
    s32 commandCameraOriginX;
    s32 commandCameraOriginY;

    struct OamData finalOam[128];
    u8 objVram[OBJ_VRAM0_SIZE];
    u16 objPalette[256];

    u16 commandCount;
    struct NativeSpriteDrawCommand commands[NATIVE_SPRITE_COMMAND_MAX];
};
```

Design decisions:

- Do not store `SpriteTemplate`, callbacks, animation scripts, or source-sheet
  pointers. The host does not need them.
- Do not store an OBJ VRAM base per command. Stage 3 has one captured OBJ pixel
  store.
- Dimensions are derivable from shape/size.
- Store matrix values in each affine command even though the raw OAM matrix
  table also exists. This makes commands immutable and independent of later
  matrix reuse.
- Preserve exact `oamIndex`; do not reconstruct order from subpriority in the
  renderer.
- `subpriority` is diagnostic provenance only.
- `sourceKind=RAW_OAM` supports direct OAM writers. Such a command is valid at
  240x160 but not expanded-eligible because no signed source position exists.
- A hidden sprite must not silently become expanded-visible. Only
  `VANILLA_VIEWPORT_CULLED` with `expandedEligible=true` may be revealed.
- Initially, only object-event-owned sprites can be confidently marked
  viewport-culled from `ObjectEvent.offScreen && !ObjectEvent.invisible`.
- Unknown hidden `gSprites` remain hidden.
- Serialize values, not live pointers.

The command sink should eventually accept multiple producers. Stage 3 supplies
the live Emerald producer; Stage 5 may append streamed-world commands without
replacing the renderer.

## 5. Native OBJ pixel-sampling algorithm

### 5.1 Common geometry

| Shape | Size 0 | Size 1 | Size 2 | Size 3 |
|---|---|---|---|---|
| Square | 8x8 | 16x16 | 32x32 | 64x64 |
| Horizontal | 16x8 | 32x8 | 32x16 | 64x32 |
| Vertical | 8x16 | 8x32 | 16x32 | 32x64 |

Shape 3 is prohibited and should trigger a capability failure or be ignored
exactly as the oracle does.

For original 240x160 parity, use the final packed OAM position or prove that:

```text
packed(signedX) == finalOam[oamIndex].x
packed(signedY) == finalOam[oamIndex].y
```

Any disagreement means the provenance mapping failed; render from raw OAM for
parity and emit diagnostics.

### 5.2 Non-affine sampling

For each destination pixel within the sprite rectangle:

```text
texX = flipX ? width  - 1 - localX : localX
texY = flipY ? height - 1 - localY : localY
tileX = texX & 7
tileY = texY & 7
blockX = texX >> 3
blockY = texY >> 3
```

Ordinary overworld mode is 1D OBJ mapping:

```text
rowStrideTiles = width / 8
blockOffset = blockY * rowStrideTiles + blockX
```

For 4bpp:

```text
address = (tileNum + blockOffset) * 32
        + tileY * 4
        + tileX / 2

index = low/high nibble
color = objPalette[paletteNum * 16 + index]
```

Index zero is transparent regardless of the stored palette color.

For 8bpp:

```text
address = (tileNum + blockOffset * 2) * 32
        + tileY * 8
        + tileX

color = objPalette[index]
```

`paletteNum` is ignored for 8bpp.

The initial overworld gate should require 1D mapping. No ordinary overworld
8bpp OBJ use was found in the audited field/object/weather paths, so 8bpp can
initially fall back.

Do not implement the repository's unneeded 2D path speculatively. Gate
`DISPCNT_OBJ_1D_MAP == 0` until 2D behavior has dedicated fixtures.

### 5.3 Mosaic

When the OAM mosaic bit is set, sizes are:

```text
objMosaicWidth  = ((MOSAIC >> 8)  & 0xF) + 1
objMosaicHeight = ((MOSAIC >> 12) & 0xF) + 1
```

The oracle snaps global destination coordinates to the mosaic grid before
deriving local source coordinates. Mosaic is screen-aligned rather than
sprite-origin-aligned.

No ordinary field OBJ mosaic assignments were found. Gate initially, then add
only with dedicated fixtures.

### 5.4 Memory source

Capture:

- `OBJ_VRAM0`, size `0x8000`
- OBJ palette RAM, size `0x200`
- Relevant registers

Do not reconstruct tiles from `SpriteSheet` or object graphics tables. Current
live memory automatically contains dynamic animation frames, decompression
results, berry palettes, reflections, weather transforms, and field effects.

## 6. Draw-order and compositing rules

### 6.1 OBJ against OBJ

OBJ priority values are 0-3, with 0 foremost.

For two opaque OBJ pixels:

1. Lower OBJ priority number wins.
2. At the same OBJ priority, lower OAM index wins.
3. `subpriority` is not consulted by the hardware/native compositor.
4. Transparent pixel index zero contributes no candidate.

Emerald makes subpriority meaningful by sorting sprites before emission:

```text
oam.priority
-> subpriority
-> interpreted Y
-> stable previous order
```

Subsprites then occupy consecutive OAM indices.

The native renderer must use the emitted OAM index. It must not approximate
sprite order with `(priority << 8) | subpriority`, as the current
expanded-margin renderer does.

### 6.2 OBJ against BG

The existing oracle composites priorities from 3 to 0 so lower numbers
overwrite higher ones.

At a given priority:

- Higher-numbered BGs are drawn first.
- Lower-numbered BGs overwrite them; BG0 wins same-priority BG ties.
- OBJ for that priority is drawn after the BGs and therefore wins an
  equal-priority BG tie.

For the ordinary overworld:

- BG1 priority 1
- BG2 priority 2
- BG3 priority 3
- BG0 is normally transparent during eligible Stage 3 frames

The Stage 2 BG0 content gate should remain. Stage 3 should not reopen BG0
rendering merely to add sprites.

### 6.3 Recommended compositor

For every output pixel:

1. Establish backdrop and BG candidates using the parity-hardened Stage 2
   renderer.
2. Rasterize all eligible OBJ primitives.
3. Resolve the foremost OBJ at each OBJ priority using lowest OAM index.
4. Merge BG and OBJ candidates according to priority and layer tie rules.
5. Retain enough information about the next visible candidate for blend
   decisions.
6. Apply window masking.
7. Apply semi-transparency or global blend/brightness.
8. Output BGR555.

A diagnostic pixel record should identify:

- Winning layer
- Winning OAM/command
- Source tile and palette index
- Second visible candidate
- Blend/window decision
- Final color

### 6.4 Semi-transparent OBJ

`ST_OAM_OBJ_BLEND` forces the OBJ pixel to act as alpha target A even when OBJ
is not selected as ordinary BLDCNT target 1.

It blends with the appropriate target-B BG/backdrop beneath it when enabled by
`BLDCNT`, using:

```text
channel = min(31, (A * EVA + B * EVB) >> 4)
```

The field's persistent `BLDCNT=0x1E40` is neutral for ordinary BG and normal
OBJ pixels, but it is not necessarily neutral for semi-transparent weather
OBJ. The Stage 3 snapshot must retain successful-frame blend registers even
though Stage 2 did not need them for clean BG pixels.

Clouds, horizontal/diagonal fog, and sandstorm use blend OBJ mode in the
audited weather code.

### 6.5 Object window

Object-window OBJ pixels do not draw a color. Nonzero pixels alter the window
mask, subject to WIN0/WIN1/OBJWIN precedence and `WINOUT`.

No object-window sprite usage was found in the ordinary overworld paths
audited. Initially gate it by capability rather than callback/map identity.

### 6.6 Backdrop

Palette entry `PLTT[0]` supplies the backdrop. It participates as a blend
target according to `BLDCNT_TGT1_BD`/`TGT2_BD`.

## 7. Affine strategy

### 7.1 Matrix state

Matrices live in:

```c
struct OamMatrix gOamMatrices[32];
```

Each component is signed 8.8 fixed point. `CopyMatricesToOamBuffer()`
serializes them into the final OAM matrix slots.

Matrices may be:

- Allocated and freed dynamically
- Reused by multiple sprites
- Updated by affine animation
- Updated directly by effect code
- Shared by reflections

The snapshot must copy the actual matrix values used by each presented command.
Never retain only a live matrix index.

### 7.2 Sampling geometry

Let source dimensions from shape/size be `srcW`, `srcH`.

For affine-normal:

```text
destination bounding box = srcW x srcH
```

For affine-double:

```text
destination bounding box = 2*srcW x 2*srcH
```

Double size expands only the destination clipping box. It does not double
source dimensions.

For a destination coordinate relative to the destination box center:

```text
texX = ((pa * localX + pb * localY) >> 8) + srcW/2
texY = ((pc * localX + pd * localY) >> 8) + srcH/2
```

Discard the pixel when `texX/texY` is outside the source rectangle.

Important details:

- Use signed arithmetic.
- Preserve the implementation's arithmetic right-shift behavior for negative
  values.
- Affine sprites do not use H/V flip bits; those bits are part of the matrix
  index.
- Affine-double `CalcCenterToCornerVec()` doubles the negative corner vector so
  OAM points at the expanded bounding box.
- Matrix origin is the source/destination center.
- Clipping occurs against the destination bounding box and output viewport.
- Palette-zero transparency happens after transformed source lookup.
- Mosaic, when combined with affine, snaps the destination coordinate before
  applying the matrix.

### 7.3 Actual overworld need

Affine support is required for broad field coverage, not merely exotic
cutscenes:

- Water reflections use affine distortion matrices.
- Object movement actions can temporarily enable double-size affine mode.
- Rotating gates are affine.
- Some field-effect transformations temporarily switch object sprites to
  double-size affine.

Affine may be deferred to a later Stage 3 substage, but the Stage 3 final exit
cannot claim ordinary overworld coverage without it.

## 8. Sprite-category inventory

| Category | Classification | Findings |
|---|---|---|
| Player | A | Ordinary 4bpp object sprite; subsprite tables and dynamic graphics changes must be respected |
| NPCs/trainers | A | Same generic path; object-event metadata supplies stable identity |
| Walking/running animations | A | Already materialized as tileNum/OBJ VRAM and signed offsets |
| Bike | A | Player object graphics variants, commonly 32x32/subsprites |
| Fishing | A | Player fishing graphics are object-event sprite variants, including larger subsprite layouts |
| Shadows | A | Normal linked field-effect sprites; no special pixel renderer |
| Reflections | A after affine | Generic copied sprite with reflection palette and affine distortion |
| Surf sprite/blob | A | Player object plus linked normal field-effect sprite |
| Tall/long/short grass | A | Normal field-effect sprites with calculated subpriority |
| Ledge/jump dust/splash/impact | A | Normal generic sprites |
| Footprints/bike tracks/sand piles | A | Normal generic field sprites |
| Item balls | A | Ordinary object-event graphics |
| Berry trees | A | Object events with dynamically switched image and palette state |
| Large/asymmetric sprites | A | Generic subsprite primitives; tables include 48x48, 64x64, 96x40, 88x32 cases |
| Rotating gates | A after affine | Generic affine objects |
| Rain/snow/ash/bubbles | A for 240x160 | Mostly normal 4bpp OBJ |
| Clouds/fog/sandstorm | A after OBJ blend | Semi-transparent 64x64 sprite patterns |
| Wider weather fill | B | Existing patterns cover only the vanilla screen; later native providers are needed |
| Door animation | D | Ordinary overworld doors are BG-ring/metatile effects already handled by Stage 2 |
| Field-move banners/cinematic effects | C/D | Mix BG, alternate VBlank, window, and sprite behavior; initially full-frame fallback |
| OBJ-window effects | D for ordinary field | No ordinary overworld use found |
| Followers | D | This repository has no follower system |
| Screen UI/menu sprites | D | Continue full-frame fallback for UI/menu scenes |

A generic primitive command handles all category-A pixel rendering.
Relationships such as reflection owner or surf owner are useful for future
world positioning and diagnostics, not for special-case rasterizers.

## 9. World-space versus screen-space treatment

### World-owned sprites

Player, NPCs, item balls, berry trees, and similar objects have stable identity
through:

```text
(objectEvent.mapGroup, objectEvent.mapNum, objectEvent.localId)
```

Their `ObjectEvent.currentCoords` supplies map-grid ownership. Their current
sprite position carries sub-tile movement, jump, bob, camera, and callback
adjustments.

At command-build time, record both:

- Exact signed presentation position
- Object-event association and a derived world position when available

The renderer should not recompute movement from `ObjectEvent.currentCoords`;
it should use Emerald's final signed sprite position. Object coordinates are
metadata for ownership and expanded-view placement.

### Camera-relative field effects

Effects with `coordOffsetEnabled` participate in the field camera offset and
can generally be represented as:

```text
worldTopLeft = latchedCommandCameraOrigin + signedPresentationTopLeft
```

The camera origin used must be recorded with the pending command frame and
latched with it. Reading the current camera later risks another one-frame
mismatch.

Linked shadows, surf blobs, reflections, grass, footprints, and splash effects
can retain an optional owner identity, but rendering remains generic.

### Screen-space sprites

Weather tiling patterns and some transient field effects are authored in GBA
screen space. They should be classified as `SCREEN_FIXED` or `UNKNOWN`, not
silently treated as world objects.

For expanded viewports:

- Confident `WORLD`/`CAMERA_RELATIVE` commands may move with the enlarged camera
  rectangle.
- `SCREEN_FIXED` commands need an explicit centering/anchoring rule.
- `UNKNOWN` commands remain restricted to the original 240x160 center or cause
  fallback outside it.

Exact 240x160 rendering does not depend on this classification; it uses the
packed presentation record.

## 10. Future object-streaming interface

Stage 3's contract is:

> Render everything Emerald currently instantiated and intends to draw.

It does not:

- Increase `OBJECT_EVENTS_COUNT`
- Load connected-map event templates
- Run distant NPC callbacks
- Invent NPC animation state
- Change trainer/script/collision activation
- Populate weather outside the vanilla authored region

Expose renderer input as an append-only command sink:

```c
void NativeSpriteCommandSink_Begin(...);
bool32 NativeSpriteCommandSink_Append(const struct NativeSpriteDrawCommand *);
void NativeSpriteCommandSink_End(...);
```

Stage 3 supplies commands from the latched Emerald sprite/OAM producer.

A future Stage 5 producer may append additional world-owned commands. It must
provide:

- Stable world identity
- World position
- Priority/order key
- Immutable pixel source
- Palette source
- Visibility/interaction policy

The renderer should not know whether a command came from a vanilla `gSprite` or
future object streaming except for diagnostics and capability checks.

Stage 5 may eventually need a pixel store beyond captured OBJ VRAM for
non-instantiated objects. Reserve a versioned pixel-source field, but implement
only captured OBJ VRAM in Stage 3.

## 11. Parity/oracle design

Use two complementary oracles.

### OBJ sampling/order oracle

Extend the easy renderer with a dev-only OBJ trace that records, before final
BG composition:

- Whether an OBJ pixel exists
- Winning OAM index
- OBJ priority
- Source texture X/Y
- Tile number and byte address
- Raw pixel/palette index
- Palette bank
- Pre-blend color
- OBJ mode
- Window contribution

This directly distinguishes sampling/order errors from BG/compositing errors.

### Final-composite oracle

Continue using the real `DrawFrame()` output as the authoritative final frame.
Extend its existing layer metadata so each pixel can report:

- Final winning layer
- Winning OAM index if OBJ
- Target A
- Immediate lower candidate/target B
- Window region/mask
- Blend operation
- Final color

### Isolation strategy

For every compared frame:

1. Confirm existing native BG-only output still matches the BG-only oracle.
2. Compare native OBJ coverage/sample trace against the OBJ oracle.
3. Compare native final composite against `DrawFrame()`.
4. Classify mismatch as:
   - BG regression
   - OBJ geometry/coordinate
   - OBJ ordering
   - OBJ VRAM address
   - Palette
   - Affine
   - Window
   - Blend/composite
   - Unmapped raw OAM

### Per-pixel diagnostic

A mismatch should be able to print:

```text
pixel=(x,y)
nativeColor=... oracleColor=...
nativeCommand=17 sourceSprite=4 objectEvent=2 part=1 oamIndex=6
position signed=(-12,48) packed=(500,48)
shape/size=... priority=... subpriority=...
tex=(11,19) tileNum=0x123 block=5 address=0x...
pixelIndex=7 paletteNum=3 paletteEntry=55
objMode=blend matrix=[...]
oracleOamIndex=6 oracleTile=... oraclePixelIndex=...
belowLayer=BG2 belowColor=...
blend=alpha eva=13 evb=7
```

### Command validation

Before rendering, validate every mapped command against final OAM:

- Packed X/Y
- Shape/size
- Tile number
- Priority
- Palette/BPP/mode/mosaic
- Affine mode/matrix index relationship
- Actual OAM index

Unmapped final OAM entries become `RAW_OAM` commands for 240x160 or produce a
capability reason. Do not silently omit them.

### Fixture format

Serialize a versioned fixture containing:

- Existing map snapshot
- OBJ snapshot
- Raw final OAM
- Command array
- OBJ VRAM
- OBJ palette
- Display/blend/window/mosaic registers
- Native OBJ trace
- Oracle OBJ trace
- Native final frame
- GBA final frame
- Manifest with counts, hashes, and first mismatch

Avoid serializing raw pointers or native struct layouts without a schema
header. The existing raw `NativeOverworldSnapshot` fixture style should evolve
to include magic, version, byte order, and section sizes before Stage 3 fixtures
become permanent.

### Required fixtures

- Stationary player/NPC
- One-pixel movement frames
- Left/right/top/bottom partial clipping
- H/V flips
- Equal-priority overlap with OAM-index tie
- Subsprite overlap/order
- Large/asymmetric object
- Shadow
- Surf
- Reflection
- Grass/jump effect
- Bike/fishing
- Item ball/berry tree
- Affine normal/double
- Semi-transparent weather
- Dynamic OBJ palette
- Screen-culling candidate
- State-load/recovery frame

## 12. Capability and fallback design

Capabilities must be derived from presented commands/registers, not callback
or map whitelists.

Recommended reasons include:

```text
OBJ_DISABLED
OBJ_MAPPING_2D
OBJ_8BPP
OBJ_AFFINE
OBJ_AFFINE_DOUBLE
OBJ_BLEND
OBJ_WINDOW
OBJ_MOSAIC
OBJ_PROHIBITED_SHAPE
OBJ_COMMAND_OVERFLOW
OBJ_OAM_PROVENANCE_MISMATCH
OBJ_RAW_OAM_ONLY
OBJ_WINDOW_CONFIG
OBJ_BLEND_CONFIG
OBJ_PRESENTATION_STREAM_STALE
```

Only require a capability when a presented, nontransparent-capable OAM entry
uses it. Invisible reserved affine sprites must not cause fallback.

### First useful implementation

Support:

- Final OAM indices and exact order
- 1D OBJ mapping
- 4bpp
- Normal non-affine OBJ
- H/V flips
- All legal shapes/sizes
- Subsprite-expanded commands
- Palette-zero transparency
- BG/OBJ priority ties
- Raw-OAM 240x160 fallback rendering
- Ordinary neutral windows

This covers player, NPCs, bike, fishing, item balls, berry trees, shadows,
surf, and most grass/ledge effects.

### Later Stage 3 increments

Add:

1. Affine normal and double-size
2. Semi-transparent OBJ and exact target-B behavior
3. Weather coverage at 240x160
4. Mosaic if an actual field fixture requires it
5. 8bpp/2D mapping only if real eligible overworld state uses it
6. Object window only if real field coverage requires it

No audited ordinary-overworld use of OBJ window, 8bpp OBJ, 2D mapping, or OBJ
mosaic was found. These should have explicit fallback reasons rather than
speculative, unverified implementations.

## 13. Stage 3 implementation substages

### Stage 3A: presentation-coherent capture and provenance

Implementation target:

- Introduce versioned OBJ snapshot and command definitions.
- Factor signed coordinate calculation from `UpdateOamCoords()`.
- Emit pending primitive commands from the actual sorted/subsprite OAM
  construction.
- Publish them only on successful `LoadOam()`.
- Capture final OAM, OBJ VRAM/palette, registers, and published commands.
- Add OAM-to-command validation and serialization.
- Add no native OBJ rasterizer yet.

Likely files:

- `src/sprite.c`
- `include/sprite.h`
- New `include/platform/native_sprite_snapshot.h`
- New `src/platform/native_sprite_snapshot.c`
- `include/platform/native_overworld_snapshot.h`
- `src/platform/native_overworld_renderer.c`
- `src/platform/desktop_video.c`
- `src/platform/native_overworld_parity.c`
- New capture/serialization unit tests
- `Makefile_pc`

Tests:

- Signed coordinate packing
- Negative and out-of-range coordinates
- Subsprite expansion and flips
- OAM ordering/provenance
- Matrix snapshot
- `oamLoadDisabled` commit behavior
- Pending versus presented frame timing
- Object-event viewport-cull classification
- Raw OAM detection
- Deterministic serialization

Stop condition:

- Every presented OAM primitive is mapped byte-exactly to one emitted command
  or explicitly classified raw OAM.
- Published commands always correspond to final `OAM`, not current `gSprites`.
- Offline snapshot round-trip is deterministic.

Do not implement pixel rendering, compositing, affine sampling, expanded
viewports, or object streaming.

### Stage 3B: non-affine OBJ sampler

Implementation target:

- Add a CPU OBJ rasterizer for 1D, 4bpp, normal non-affine OBJ.
- Support flips, all legal dimensions, clipping, transparency, and exact OAM
  ordering.
- Emit a native OBJ coverage/sample trace.

Likely files:

- New `include/platform/native_obj_renderer.h`
- New `src/platform/native_obj_renderer.c`
- OBJ snapshot/capability files
- Unit tests and fixture runner

Tests:

- Shape/size matrix
- H/V flips
- Tile row stride
- Palette bank
- Transparent index zero
- Negative/overflow coordinates
- Large and subsprite objects
- Same-priority OAM-index overlap

Stop condition:

- OBJ-only native trace is pixel/sample exact for all supported synthetic
  fixtures.

Do not integrate final BG composition, affine, blend, mosaic, or wider
viewports.

### Stage 3C: normal OBJ final compositing

Implementation target:

- Merge native normal OBJ output with the parity-hardened BG frame.
- Implement exact OBJ priority, BG tie, OAM-index, backdrop, and neutral-window
  behavior.
- Add OBJ and final-composite oracle metadata.

Likely files:

- Native OBJ renderer
- `src/platform/native_overworld_renderer.c`
- `src/platform/gba_easy_draw.c`
- `include/platform/framedraw.h`
- Parity module and desktop integration
- Oracle/compositor tests

Stop condition:

- Exact final-frame parity for normal non-affine ordinary scenes.
- BG-only parity remains unchanged.

Do not add affine or semi-transparent OBJ.

### Stage 3D: affine OBJ

Implementation target:

- Implement signed 8.8 matrix sampling, center semantics, normal/double bounding
  boxes, matrix reuse, clipping, and the affine+mosaic ordering seam.
- Cover reflections and rotating/special field objects.

Tests:

- Identity, scale, negative scale, rotation, shear
- Normal versus double size
- Negative transformed coordinates and rounding
- Shared matrices
- Reflection distortion real fixture
- Affine source transparency

Stop condition:

- Affine OBJ trace and final composite match the oracle pixel-exactly.

Do not extend the viewport.

### Stage 3E: OBJ blend and weather

Implementation target:

- Implement semi-transparent OBJ.
- Capture and consume successful-frame BLDCNT/BLDALPHA/BLDY.
- Implement exact target-B and backdrop behavior.
- Cover cloud/fog/sandstorm plus normal rain/snow/ash/bubbles.

Tests:

- Semi-transparent OBJ over each BG priority
- Target-B enabled/disabled
- No target B
- Backdrop target
- Persistent field `BLDCNT=0x1E40`
- Window effects enabled/disabled where applicable
- Real weather fixtures

Stop condition:

- All ordinary 240x160 weather frames within the supported gate match exactly.

Do not synthesize wider weather coverage.

### Stage 3F: field-effect coverage and rare capability audit

Implementation target:

- Exercise every category in the inventory.
- Implement mosaic, 8bpp, 2D mapping, or OBJ window only when an actual
  eligible field fixture demonstrates the need.
- Otherwise retain explicit, tested fallback reasons.
- Validate raw-OAM-only handling.

Tests:

- Player variants, NPCs, fishing, bike, shadows, surf, reflections
- Grass/ledge/jump/footprint effects
- Item balls and berry trees
- Large/asymmetric sprites
- Dynamic palettes
- Special affine movement
- Each unsupported capability's fallback test

Stop condition:

- Every audited ordinary overworld category is either pixel-exact or rejected
  for a named capability, with no callback/map whitelist.

Do not treat cinematic field-move screens or menus as ordinary overworld.

### Stage 3G: runtime parity hardening

Implementation target:

- Run the combined BG+OBJ runtime oracle.
- Capture permanent moving/animated/effect fixtures.
- Harden mismatch accounting and state-load behavior.
- Enable the native complete 240x160 frame only after parity.

Tests:

- Long real-runtime capture
- Moving camera and moving sprites
- Map transitions/fades allowed by existing gates
- State save/load invalidation
- Fixture replay under sanitizers
- No regression in the existing 859-frame BG corpus

Stop condition:

- Zero mismatches across the supported real-frame corpus.
- Every scheduled frame is compared or accounted by an explicit
  fallback/outcome.
- No center copy is needed for supported native 240x160 frames.

Do not begin Stage 4 widening here.

## 14. Files and functions likely affected

### Core sprite/OAM path

- `src/sprite.c`
  - `BuildOamBuffer`
  - `UpdateOamCoords`
  - `SortSprites`
  - `AddSpritesToOamBuffer`
  - `AddSpriteToOamBuffer`
  - `AddSubspritesToOamBuffer`
  - `CopyMatricesToOamBuffer`
  - `LoadOam`
- `include/sprite.h`

The core change should be a narrow observer/capture seam, not platform
rendering logic embedded in the sprite engine.

### Snapshot and renderer

- `include/platform/native_overworld_snapshot.h`
- `include/platform/native_overworld_renderer.h`
- `src/platform/native_overworld_renderer.c`
- New native sprite snapshot and OBJ renderer modules

### Presentation integration

- `src/platform/desktop_video.c`
- Possibly `src/platform/native_state.c`

The latched command stream is derived presentation state. On state load, either
serialize it or mark it invalid and use raw final OAM at 240x160 until the next
successful OAM commit. Never present stale provenance.

### Oracle and parity

- `src/platform/gba_easy_draw.c`
- `include/platform/framedraw.h`
- `src/platform/native_overworld_parity.c`
- `include/platform/native_overworld_parity.h`

### Tests/build

- New OBJ capture, sampler, compositor, affine, and fixture tests
- `Makefile_pc`
- `tests/fixtures/native-parity/obj/`

## 15. Widescreen compatibility analysis

| Design choice | 240x160 | 300x200 / 360x240 | 16:9 / ultrawide |
|---|---|---|---|
| Final OAM position only | Exact | Ambiguous/wrong | Unusable |
| Signed pre-wrap command position | Exact after packing validation | Valid for instantiated objects | Valid within available object coverage |
| Actual OAM index retained | Exact order | Useful for current commands | Future added commands need compatible order keys |
| Live OBJ VRAM/palette | Exact | Works for existing live sprites | Does not supply graphics for nonexistent streamed objects |
| `ObjectEvent.offScreen` retained | Exact | Allows already-instantiated viewport-culled objects | Still bounded by vanilla object activation |
| Generic command sink | Exact | Accepts current live sprites | Stage 5 can append streamed commands |
| Screen-fixed/unknown classification | No effect | Prevents false world placement | Requires explicit wider weather/UI policy |
| Raw-OAM fallback commands | Exact | Center-only | Must not be treated as world-positioned |
| Capability gates | Safe | Honest partial coverage | Prevent silent approximations |

### Renderer limitations

- Packed-coordinate ambiguity if signed commands are missing
- Unsupported OBJ formats/modes
- Screen-space effect extent
- Compositing/window/blend support
- Pixel-source availability

### World/object-streaming limitations

- Only 16 live object events
- Vanilla spawn/removal envelope
- Connected-map NPC templates not instantiated
- Distant AI/movement state absent
- Save/collision/trainer/script consequences of activating more objects
- Weather density outside the vanilla region

The renderer must report the latter as coverage limitations; it must not try to
solve them.

## 16. Top architectural risks

1. **Pending-versus-presented timing.** A command stream built from host-time
   `gSprites` will be a frame ahead of hardware OAM.
2. **Visibility ambiguity.** `Sprite.invisible` conflates intentional hiding
   and callback-driven screen culling.
3. **Coordinate information loss.** Nine-bit/eight-bit OAM positions cannot
   identify intended offscreen positions.
4. **Subsprite provenance.** Ordinary overworld objects routinely expand to
   multiple OAM entries.
5. **Order reconstruction.** Subpriority is not compositing order; actual OAM
   index is.
6. **Affine rounding.** Signed 8.8 arithmetic, negative shifts, double-size
   bounds, and center semantics must match exactly.
7. **Semi-transparent weather.** The persistent field blend configuration
   becomes visually active for blend-mode OBJ.
8. **Raw OAM writers.** Direct OAM entries need explicit 240x160 handling and
   must not be widened.
9. **State-load staleness.** Hardware OAM may restore without matching pre-wrap
   provenance.
10. **Oracle attribution.** A final-color diff alone cannot distinguish
    geometry, tile addressing, priority, or blending.
11. **Command overflow/OAM limit.** Preserve both Emerald's emission budget and
    non-emitted expansion candidates; do not quietly merge them.
12. **World-position overconfidence.** Not every generic field/weather sprite
    has a trustworthy world anchor.
13. **Object-streaming scope creep.** Rendering already-instantiated offscreen
    objects is presentation work; creating new NPC state is not.
14. **Premature rare-mode support.** 2D mapping, 8bpp, OBJ window, and mosaic
    require fixtures before being called exact.

## 17. First DeepSeek prompt for Stage 3A

```text
You are implementing ONLY Stage 3A of the native overworld renderer in:

/home/tristen/work/pokeemerald-recomp

Read first:
- docs/NATIVE_OVERWORLD_RENDERER_ARCHITECTURE.md
- docs/NATIVE_OVERWORLD_RENDERER_STAGE3_ARCHITECTURE.md
- include/platform/native_overworld_snapshot.h
- src/platform/native_overworld_renderer.c
- src/platform/native_overworld_parity.c
- src/platform/desktop_video.c
- include/sprite.h
- src/sprite.c
- src/overworld.c around OverworldBasic and VBlankCB_Field
- src/platform/gba_easy_draw.c around DrawSprites

Current state:
- Stage 0, 1, 2, and 2.1 are complete.
- The native BG renderer is parity-hardened.
- 859/859 real frames matched pixel-exactly.
- Do not change BG rendering, BG0 gating, persistent neutral BLDCNT behavior,
  camera-vs-scroll timing, or existing parity accounting except where a new OBJ
  snapshot field/interface must be appended.
- The worktree contains user changes. Preserve all unrelated changes.
- Do not launch the game.

Implement ONLY Stage 3A:
PRESENTATION-COHERENT NATIVE SPRITE/OBJ SNAPSHOT, PRE-WRAP COMMAND CAPTURE, AND
DIAGNOSTICS.
Do not implement OBJ pixel rendering or final OBJ compositing yet.

Critical timing rule:
- Platform_VideoDrawFrame reads final hardware OAM/OBJ memory for the currently
  presented frame.
- Current gSprites/gOamMatrices/gMain.oamBuffer have already advanced toward
  the next VBlank.
- Build a PENDING command frame during the actual BuildOamBuffer emission.
- Publish/commit that command frame only when LoadOam successfully copies
  gMain.oamBuffer to OAM.
- If oamLoadDisabled prevents the OAM copy, do not advance the published
  command frame.
- Host snapshot capture must copy the PUBLISHED command frame together with
  final OAM, current OBJ VRAM, OBJ palette RAM, and relevant registers.

Architecture requirements:
1. Add a versioned immutable NativeObjSnapshot / NativeSpriteDrawCommand
   contract.
2. Capture:
   - final OAM[128]
   - OBJ_VRAM0_SIZE bytes
   - 256 OBJ palette entries
   - DISPCNT, MOSAIC, BLDCNT, BLDALPHA, BLDY
   - WIN0H/WIN0V/WIN1H/WIN1V/WININ/WINOUT
   - presentation sequence/validity
3. Emit one command per actual sprite/subsprite OAM primitive from the same
   sorted path that writes gMain.oamBuffer.
4. Each emitted command must contain:
   - deterministic command/emission ID
   - source gSprite index
   - optional objectEvent index
   - subsprite part index
   - exact destination OAM index
   - signed pre-wrap destination top-left
   - shape/size/tileNum/priority/subpriority/palette/bpp/objMode/affineMode/mosaic
   - non-affine flip bits
   - copied affine matrix values
   - placement/visibility/expanded-eligibility classification
5. Do not use final packed OAM coordinates as the only stored position.
6. For object-event sprites, distinguish:
   - intentionally hidden: objectEvent.invisible
   - vanilla viewport culled: objectEvent.offScreen && !objectEvent.invisible
7. For arbitrary invisible gSprites where the reason is not provable, classify
   hidden-unknown and do not mark expanded-eligible.
8. Preserve exact final OAM as the 240x160 authority.
9. Validate every presented command against its final OAM entry:
   - packed signed X/Y
   - shape/size
   - tileNum
   - priority/palette/bpp/modes/mosaic
10. Detect visible/unmapped final OAM entries and classify them explicitly as
    raw-OAM-only or provenance mismatches. Do not silently omit them.
11. Keep platform capture hooks narrow. Do not place rendering code in sprite.c.
12. Use bounded arrays and deterministic value copies; serialize no live
    pointers.
13. On state load, prevent stale published command provenance. Either restore
    it correctly or invalidate it and require raw-OAM/fallback behavior until
    the next successful LoadOam commit.

Required tests:
- signed coordinate packing:
  x=-20 -> 492, x=280 -> 280, y=-20 -> 236, y=200 -> 200
- non-affine centerToCornerVec and coordOffsetEnabled math
- subsprite signed positions, H/V mirrored offsets, tile offsets, and per-part
  priority
- exact OAM-index/emission order
- affine matrix copying and matrix reuse
- pending stream does not become presented before LoadOam
- oamLoadDisabled leaves the presented stream unchanged
- objectEvent intentional-hidden versus viewport-culled classification
- arbitrary invisible sprite remains hidden-unknown
- raw final-OAM entry detection
- deterministic snapshot serialization/round-trip
- presentation sequence mismatch/stale-state rejection
- all existing native overworld tests remain passing

Stage 3A stop condition:
- No OBJ pixels are rendered.
- Every final presented OAM primitive is mapped byte-exactly to a published
  command or explicitly classified as raw/unmapped.
- Published commands always match final OAM rather than the newer gSprites
  state.
- The snapshot is replayable offline and deterministic.
- Existing Stage 0-2.1 BG behavior and fixtures are unchanged.

Do NOT implement:
- native OBJ sampling
- OBJ/BG compositing
- affine rasterization
- blend/window/mosaic rendering
- zoom, expanded viewport, or widescreen
- object streaming
- weather extension
- callback or map whitelists
- GPU rendering
- FireRed
- release packaging

Before editing, inspect git status and preserve unrelated changes.
After implementation, run only the relevant unit/module/sanitizer tests; do not
launch the game.
Report:
- files changed
- exact capture/commit lifecycle
- command/OAM validation invariants
- tests run and results
- any raw-OAM or visibility limitations left for Stage 3B
```

## Bottom line

Stage 3 should render the exact 240x160 presentation from final OAM while
retaining a presentation-coherent, signed pre-wrap command stream for the same
frame. Final OAM is indispensable for parity, but insufficient as future world
truth. The pending/presented command latch at `BuildOamBuffer()`/`LoadOam()` is
the architectural seam that satisfies both requirements.

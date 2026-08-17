# Stage 3D — OBJ capability audit + support boundary

Audit of every unsupported OBJ capability the native overworld renderer can hit,
with repo evidence for what the runtime `obj_unsupported` frames represent, and
the chosen Stage 3D support boundary. Companion to
`NATIVE_OVERWORLD_RENDERER_STAGE3_ARCHITECTURE.md`.

## 1. Capability inventory (exact enums and call sites)

Capability flags (`enum NativeObjCapabilityFlags`, `include/platform/native_obj_renderer.h`):

| bit | flag                    | meaning                                        |
|-----|-------------------------|------------------------------------------------|
| 0   | `NATIVE_OBJ_CAP_2D_MAPPING`    | DISPCNT_OBJ_1D_MAP clear (frame-wide)   |
| 1   | `NATIVE_OBJ_CAP_8BPP`          | per-primitive 8bpp                      |
| 2   | `NATIVE_OBJ_CAP_AFFINE`        | affine normal-size                      |
| 3   | `NATIVE_OBJ_CAP_AFFINE_DOUBLE` | affine with double-size bounds          |
| 4   | `NATIVE_OBJ_CAP_OBJ_BLEND`     | semi-transparent objMode==1             |
| 5   | `NATIVE_OBJ_CAP_OBJ_WINDOW`    | OBJ-window objMode==2                   |
| 6   | `NATIVE_OBJ_CAP_MOSAIC`        | sprite mosaic bit                       |
| 7   | `NATIVE_OBJ_CAP_PROHIBITED_SHAPE` | shape==3                            |
| 8   | `NATIVE_OBJ_CAP_BLEND_CONFIG`  | alpha blend would recolor normal OBJ    |

Per-primitive reject reasons (`enum NativeObjRejectReason`): `NONE`, `AFFINE`,
`AFFINE_DOUBLE`, `8BPP`, `OBJ_BLEND`, `OBJ_WINDOW`, `MOSAIC`, `PROHIBITED_SHAPE`.
Non-affine entries with the double-size bit set are oracle-disabled and skipped
silently (no reject record), exactly like the oracle's `continue`.

Reject/flag call sites (all in `src/platform/native_obj_renderer.c`,
`NativeObjRender_Rasterize`):

- 2D mapping is a frame gate: `if (!(snap->dispCnt & DISPCNT_OBJ_1D_MAP))`
  sets `produced=FALSE` + `NATIVE_OBJ_CAP_2D_MAPPING` and returns (lines 220-226).
- `BLEND_CONFIG`: `(snap->bldCnt & BLDCNT_TGT1_OBJ) && (((bldCnt>>6)&3)!=0)` (lines 232-233).
- Per-primitive loop, OAM 127..0 (lines 237+):
  - shape==3 -> PROHIBITED_SHAPE (250-257)
  - non-affine + double -> silent continue (260-261)
  - offscreen (`RectIntersectsViewport`) -> silent continue, never capability-
    rejected (267-268)
  - affine -> AFFINE or AFFINE_DOUBLE reject (270-284)
  - bpp&1 -> 8BPP (286-291); objMode==1 -> OBJ_BLEND (293-299);
    objMode==2 -> OBJ_WINDOW (301-306); mosaic==1 -> MOSAIC (307-312)

Composite fallback mapping (`src/platform/native_field_compositor.c:144-156`):
`produced==FALSE` -> `NATIVE_COMPOSITE_FALLBACK_OBJ_NOT_PRODUCED`;
`capabilityFlags!=0` -> `NATIVE_COMPOSITE_FALLBACK_OBJ_UNSUPPORTED` with
`report->objCapabilityFlags` / `report->objRejectCount` preserved.

Runtime accounting (`src/platform/native_overworld_parity.c:1020-1083`):
`capabilityFlags!=0` and the two OBJ fallback reasons all increment
`sCompObjUnsupported` (the counter that read 209 in the prior manual session).
A first-occurrence log prints `flags=0x%x rejects=%u` but no per-reason
histogram yet — that is added by this stage (see the histogram section below).

## 2. Repo evidence: what the 209 frames actually were

`obj_unsupported=209` is the accumulated count of frames skipped because a
presented OBJ primitive used an unsupported capability. The audit cannot split
the 209 by reason (no histogram existed); the per-reason split is resolved by
the new histogram on the next manual session. The REPO evidence for what the
ordinary overworld can present is conclusive:

**Affine normal-size — water/bridge reflections (Class A-common):**
- `event_object_movement.c:1207 CreateReflectionEffectSprites` creates two
  `FLDEFFOBJ_REFLECTION_DISTORTION` sprites with `ST_OAM_AFFINE_NORMAL`, running
  `sAffineAnim_ReflectionDistortion_0/1` (`field_effect_objects.h:849-889`,
  non-identity matrices e.g. `AFFINEANIMCMD_FRAME(0xFF00, 0x100, ...)`). These
  are invisible OAM-priority-31 holders that drive matrices 0 and 1.
- `event_object_movement.c:7858 GroundEffect_WaterReflection` ->
  `field_effect_helpers.c SetUpReflection(..., FALSE)` -> the reflection copy
  sets `ST_OAM_AFFINE_NORMAL`; `UpdateObjectReflectionSprite`
  (`field_effect_helpers.c:137,156-160`) points it at matrix 0 or 1. So any
  frame the player/NPC stands on a water-reflection metatile, a normal-size
  affine 4bpp reflection sprite presents. Very common across the game.
- `GroundEffect_IceReflection` uses `stillReflection=TRUE` (affineMode NOT set)
  and is already supported.

**Affine double-size — scripted movements (Class B-occasional):**
- `event_object_movement.c:6605 MovementAction_InitAffineAnim_Step0` sets
  `ST_OAM_AFFINE_DOUBLE`. The affine movement actions
  (`MOVEMENT_ACTION_INIT_AFFINE_ANIM`, `WALK_DOWN/LEFT/RIGHT_AFFINE`) are used
  only in `data/maps/SootopolisCity`, `TerraCave_End`, `MarineCave_End` scripts.

**Semi-transparent OBJ — weather (Class A-common, but composite-time blend):**
- Every non-rain/snow weather sprite in `src/field_weather_effect.c` is
  `objMode = ST_OAM_OBJ_BLEND`: `sCloudSpriteOamData` (line 56), `sOamData_FogH`
  (1280), `sAshSpriteOamData` (1615), `sFogDiagonalSpriteOamData` (1833),
  `sSandstormSpriteOamData` (2073). So sandstorm (Route 111), ash (Route 113),
  fog, and clouds present semi-transparent OBJ every frame they are active.
- The persistent field blend `BLDCNT=0x1E40` is neutral for normal OBJ but NOT
  for objMode==1 OBJ: the oracle's `isSemiTransparent` branch applies
  `alphaBlendSelectTargetB` + `alphaBlendColor` against the pixel underneath
  (`gba_easy_draw.c:676-683`), which the OBJ-only layer pass cannot reproduce
  without the underlying BG/OBJ pixel. This is why semi-transparent OBJ is
  deliberately out of the Stage 3D boundary (see section 4).

**Not used by the ordinary overworld (Class D):**
- 8bpp: no `ST_OAM_8BPP` in any field/overworld file (menus/screens only).
- OBJ-window: only `pokedex.c`, `contest_util.c`, `title_screen.c`.
- Mosaic: no sprite mosaic anywhere in field code.
- 2D OBJ mapping: every field screen sets `DISPCNT_OBJ_1D_MAP`.

## 3. A/B/C/D/E classification

- **A (common ordinary, already supported):** player/NPC/bike/fishing/shadows/
  surf/grass/ledge/items normal OBJ; rain + snow weather sprites (objMode normal).
- **A after affine normal-size:** water/bridge reflections (`GroundEffect_WaterReflection`).
- **B (occasional field effect):** double-size affine movement actions (3 maps).
- **A after OBJ blend (composite-time):** sandstorm / fog / ash / clouds weather.
- **C (transition/special, safe fallback):** none newly identified; existing BG
  fallbacks cover transitions.
- **D (not used):** 8bpp, OBJ-window, mosaic, 2D mapping.
- **E (unknown until the histogram runs):** the exact per-reason split of the 209.

## 4. Stage 3D support boundary (chosen)

**Implement: affine OBJ, normal-size and double-size.** Repo evidence: water
reflections are the single most common ordinary-overworld affine use, they are
normal-size affine, and the snapshot already carries the exact presented matrix
state (section 5). Double-size is included because it is a small, well-understood
extension of the same path (the raster box doubles; the sampling formula is
unchanged) and covers the scripted `ST_OAM_AFFINE_DOUBLE` movements.

**Explicitly NOT in Stage 3D:**
- Semi-transparent OBJ (weather): requires composite-time alpha blending against
  the underlying pixel; retained as the `OBJ_BLEND` fallback. Documented in the
  remaining-unsupported list of the final report.
- 8bpp, OBJ-window, mosaic, 2D mapping, shape==3: remain explicit rejects (no
  ordinary overworld use; not a hardware emulator).
- `BLEND_CONFIG` gate unchanged (no ordinary field use; oracle-neutral 0x1E40).

Do NOT implement rare capabilities merely to drive the fallback count to zero:
the new histogram (next session) will show the real mix, and the OBJ_BLEND
weather frames will still be present.

## 5. Affine matrix snapshot coherency (Section 6 requirement)

Confirmed sufficient — the existing snapshot already carries the exact matrix
state committed for the same presented OAM frame, in the exact layout the oracle
reads:

- `CopyMatricesToOamBuffer` (`src/sprite.c:501-512`) writes matrix i's a/b/c/d
  into `gMain.oamBuffer[4*i + 0..3].affineParam`.
- `LoadOam` (`src/sprite.c:760-768`) `CpuCopy32`'s `gMain.oamBuffer` -> `OAM`.
- `NativeObjSnapshot_Capture` (`native_sprite_snapshot.c:540`) `memcpy`'s `OAM`
  -> `snap->finalOam`.

Therefore `snap->finalOam[oam->matrixNum*4 + k].affineParam` (k=0..3) is exactly
the presented matrix, and the gba_easy_draw.c oracle reads the identical values
(`OAM[oam->matrixNum*4+k].affineParam`, `gba_easy_draw.c:561-571`). The native
affine sampler MUST read the matrix from `snap->finalOam`, NOT from
`cmd->pa/pb/pc/pd`: for GSPRITE commands those are copied from the LIVE
`gOamMatrices` at BuildOamBuffer time (`native_sprite_snapshot.c:265-273`), i.e.
the next-frame matrix, which can differ while a distortion anim runs. No live
matrix read during rasterization. Presentation timing is unchanged.

## 6. Affine oracle authority (what the native must reproduce exactly)

`gba_easy_draw.c DrawSprites` affine path (lines 489-699) is the authority:

- Center-origin: `x += half_width; y += half_height` after coordinate wrap.
- Matrix read: `oam->matrixNum * 4` into four consecutive `affineParam` s16 8.8
  values; identity `0x100/0/0/0x100` for non-affine.
- Double-size: `rect_width/rect_height/half_width/half_height *= 2`; the texture
  offset stays `width/2`, `height/2` (original dims).
- Sampling: per scanline `local_y = vcount - y`; `local_x` runs
  `-half_width .. +half_width` INCLUSIVE; skip `global_x` outside `[0,240)`.
  `tex_x = ((pa*local_x + pb*local_y) >> 8) + width/2`;
  `tex_y = ((pc*local_x + pd*local_y) >> 8) + height/2`;
  cull `tex < 0 || tex >= width/height`. The inclusive right column is culled
  for the identity matrix but CAN emit for rotated/scaled matrices, so it must
  be reproduced, not "cleaned up".
- Vertical extent is HALF-OPEN: `vcount >= y-half_height && vcount < y+half_height`.
- Affine sprites ignore the flip bits (`flipX/flipY` are `!isAffine`).
- 4bpp 1D tile fetch identical to the existing sampler
  (`block_y * (DISPCNT & 0x40 ? width/8 : 16) + block_x`); transparent index 0
  never writes; write `palette[pixel] | 1<<15`.

## 7. Files that will change

- `src/platform/native_obj_renderer.c` — affine raster path in the Stage 3B
  loop (replaces the AFFINE/AFFINE_DOUBLE reject for presented entries), affine
  sampling seam, double-size bounding rect, matrix read from `finalOam`.
- `include/platform/native_obj_renderer.h` — doc comment updates; output struct
  gains the per-reason capability histogram (see below).
- `src/platform/native_overworld_parity.c` — capability histogram accounting on
  the `capabilityFlags` + reject records; keep `obj_unsupported` total for
  continuity.
- `tests/native_obj_renderer_oracle_unit.c` + `native_obj_renderer_shared.h` —
  affine matrix helpers + affine real-oracle scenarios (Section 10).
- `tests/native_obj_renderer_unit.c` — affine unit coverage.
- `tests/native_field_compositor_oracle_unit.c` — affine-in-composite scenarios.
- `tests/native_field_compositor_unit.c` — affine-over-BG/OBJ composite tests.
- `tests/native_overworld_renderer_test.sh` / `native_overworld_sanitize.sh` —
  already drive the oracle harnesses; no new binaries needed.

#!/bin/sh
set -eu

test_dir=$(mktemp -d "${TMPDIR:-/tmp}/native-overworld-renderer.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT

gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_renderer_unit.c -o "$test_dir/native-overworld-renderer-test"

"$test_dir/native-overworld-renderer-test"

gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_zoom_input_unit.c \
    -o "$test_dir/native-overworld-zoom-input-test"

"$test_dir/native-overworld-zoom-input-test"

gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_parity_unit.c -o "$test_dir/native-overworld-parity-test"

"$test_dir/native-overworld-parity-test"

gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_parity_module_unit.c -o "$test_dir/native-overworld-parity-module-test"

"$test_dir/native-overworld-parity-module-test"

gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_bg0_gate_unit.c \
    -o "$test_dir/native-overworld-bg0-gate-test"

# Offline repro of the runtime BG0-overlay mismatches (frames 1131-1138) plus
# regression coverage for the BG0-content gate. Fixtures are committed under
# tests/fixtures/native-parity/.
"$test_dir/native-overworld-bg0-gate-test" tests/fixtures/native-parity

# Offline repro of the two REAL moving-frame (bg-scroll-divergent) captures from
# the Stage 2.1 manual parity run (frames 218, 219). Re-renders each snapshot and
# asserts it reproduces the runtime's native frame bit-for-bit and the captured
# GBA oracle pixel-for-pixel with 0 mismatches -- permanently protecting the
# presentation-origin timing fix. Fixtures: tests/fixtures/native-parity/scroll/.
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_scroll_fixture_unit.c \
    -o "$test_dir/native-overworld-scroll-fixture-test"

"$test_dir/native-overworld-scroll-fixture-test" tests/fixtures/native-parity/scroll

# The real-oracle harness compiles the production gba_easy_draw.c as its own TU
# (as the real build does), because its `extern void (*const gIntrTable[])`
# conflicts with main.h's non-const declaration in a single TU.
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 -DRENDERER_EASY_DRAW \
    -c src/platform/gba_easy_draw.c -o "$test_dir/gba_easy_draw.o"

gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 -DRENDERER_EASY_DRAW \
    tests/native_overworld_real_oracle_unit.c "$test_dir/gba_easy_draw.o" \
    -o "$test_dir/native-overworld-real-oracle-test"

"$test_dir/native-overworld-real-oracle-test"

# Stage 3A OBJ command-sink / snapshot contract: signed packing, subsprite
# offsets, per-command OAM validation, presentation-commit semantics, raw-OAM
# classification, and serialization round-trip. Self-contained (no fixtures).
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_sprite_snapshot_unit.c -o "$test_dir/native-sprite-snapshot-test"

"$test_dir/native-sprite-snapshot-test"

# Stage 3B OBJ sampler unit tests: shape/size matrix, multi-tile row-stride
# addressing, flips, position/clipping/signed-wrap, exact OAM order (3-way
# overlap + transparent exposing next), subsprite multi-command, raw-OAM
# provenance, capability rejection per reason, 2D-mapping/NULL rejection, and the
# blend-config predicate. Self-contained (no fixtures).
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_obj_renderer_unit.c -o "$test_dir/native-obj-renderer-test"

"$test_dir/native-obj-renderer-test"

# Stage 3B real-oracle harness: publishes the same snapshot into live OAM/OBJ
# VRAM/OBJ palette/registers and compares the sampler's four per-OBJ-priority
# layers byte-for-byte against gba_easy_draw.c's captured spriteLayers (via the
# gParityOBJLayers seam). Links the same gba_easy_draw.o as the BG real-oracle
# harness above.
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 -DRENDERER_EASY_DRAW \
    tests/native_obj_renderer_oracle_unit.c "$test_dir/gba_easy_draw.o" \
    -o "$test_dir/native-obj-renderer-oracle-test"

"$test_dir/native-obj-renderer-oracle-test"

# Stage 3C compositor unit tests: the pure BG+OBJ final merge, the composite
# blend predicate, and the on-demand composite trace. Self-contained (no
# fixtures); links only native_field_compositor.c + the Stage 3B shared model
# (gc-sections drops the never-called DrawCompositeFrame wrapper and its BG
# renderer dependencies).
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_field_compositor_unit.c -o "$test_dir/native-field-compositor-test"

"$test_dir/native-field-compositor-test"

# Stage 3C real-oracle harness: hand-built BG snapshot (including REORDERED
# priorities the capture gate rejects by design) + Stage 3B OBJ snapshot
# published into the live VRAM/PLTT/OAM/registers, compared pixel-for-pixel
# (color AND per-pixel winner) against gba_easy_draw.c's real final composite
# via the gParityCompositeLayers seam. Links the same gba_easy_draw.o as the
# BG/OBJ real-oracle harnesses above.
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 -DRENDERER_EASY_DRAW \
    tests/native_field_compositor_oracle_unit.c "$test_dir/gba_easy_draw.o" \
    -o "$test_dir/native-field-compositor-oracle-test"

"$test_dir/native-field-compositor-oracle-test"

# Stage 4A expanded-viewport parity: NativeOverworldRenderer_DrawExpandedComposite
# at 300x200. Proves the 240x160 center crop equals the proven 240x160 composite
# (color AND winner), that the margins reveal independent world (ring + border),
# and that the inactive-viewport / NULL / BG0 / blend / 8bpp OBJ gates each
# report the explicit named fallback reason.
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_expanded_parity_unit.c \
    -o "$test_dir/native-overworld-expanded-parity-test"

"$test_dir/native-overworld-expanded-parity-test"

# Stage 4A continuous NativeViewport module: default/inactive at exactly
# 240x160, arbitrary scale derivation (1.25x -> 300x200, 1.5x -> 360x240, plus
# non-preset 1.02x -> 245x163 and 1.13x -> 271x181), POKEEMERALD_NATIVE_VIEWPORT
# parse+clamp with invalid rejection, continuous step/zoom bounds, reset to
# exactly 240x160, and TopLeft centering with deterministic odd rounding.
# Self-contained (no fixtures).
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_viewport_unit.c \
    -o "$test_dir/native-overworld-viewport-test"

"$test_dir/native-overworld-viewport-test"

# Stage 4A expanded WORLD-coordinate correctness (tests A-K): every pixel of
# full-frame sweeps (300x200 static origin, grid-only, non-preset 245x163 and
# 271x181, and moving-frame presentation scroll) must equal an INDEPENDENT
# world-coordinate / tile-provider reference (direct ring/grid/border + VRAM +
# palette sampling, never the renderer's resolve functions); plus the
# map-connection grid-vs-border strip, the captured-priority winner across
# ring/grid providers, and sprite expansion positions (WORLD signedX + margin,
# RAW center, and a revealed VANILLA_VIEWPORT_CULLED command) through the real
# expanded composite. Self-contained (no fixtures).
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_expanded_world_unit.c \
    -o "$test_dir/native-overworld-expanded-world-test"

"$test_dir/native-overworld-expanded-world-test"

# Stage 4A ring-residency regression tests A-J: the stale-ring-sentinel spec.
# Proves the fixed world-residency gate (IsWorldCoordinateResidentInRing)
# against the OLD origin-[-16,+256) gate: central 240x160 ring wins; the
# left/right/top/bottom margins and corners OUTSIDE the ring's 256x256 world
# window resolve from the grid (never the &0xFF-aliased stale ring tile); a
# resident coordinate near the wrap still maps through the physical &0xFF
# index; one tile beyond residency does NOT wrap back into the ring; a
# transient ring write inside residency wins over the grid; and connection-
# strip grid content wins outside the ring. Self-contained (no fixtures).
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_ring_residency_unit.c \
    -o "$test_dir/native-overworld-ring-residency-test"

"$test_dir/native-overworld-ring-residency-test"

# Stage 4A sequential-movement residency tests A-G: drives a synthetic
# field-camera simulation built from the real field_camera.c mechanics
# (CameraUpdate crossings, u8 mod-32 tile offset, RedrawMapSlice* geometry,
# one-frame latched-scroll lag) through walks east/west/north/south, the
# 31->0 physical ring wrap, and direction reversal. Every frame is rendered
# through the real DrawMapFrameWithMetaEx at 300x200 and every MARGIN pixel is
# asserted against the independent world reference (resident cells show the
# true cell content -- cells 16 apart carry distinct colors so a stale ring
# cell can never hide -- non-resident cells show the uniform M0 grid).
# Self-contained (no fixtures).
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_sequential_movement_unit.c \
    -o "$test_dir/native-overworld-sequential-movement-test"

"$test_dir/native-overworld-sequential-movement-test"

# Stage 4A Issue B stable-canvas fallback tests A-F: the presentation helper
# behind "STOP VISUAL ZOOMING DURING FALLBACK" -- viewport-sized canvas, the
# authoritative 240x160 core centered 1:1 and bit-exact, PURE-black margins on
# all four sides and corners, deterministic no-hidden-state output, the
# SELECTED viewport never mutated by a fallback, and non-preset / degenerate
# viewport handling. Self-contained (no fixtures).
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_overworld_fallback_canvas_unit.c \
    -o "$test_dir/native-overworld-fallback-canvas-test"

"$test_dir/native-overworld-fallback-canvas-test"

# Stage 3C offline reproducer of the 7 runtime composite-parity mismatch frames
# (178, 610, 614, 620, 2753, 2798, 3084). Deserializes the captured BG + OBJ
# snapshots, re-runs the exact runtime pipeline, and for a PRE-fix fixture
# (fallbackReason NONE) reproduces the runtime mismatch counts / first pixel /
# winner histograms from report-N.txt; for a POST-fix transitional fixture
# (fallbackReason BG_TILEMAP_IN_FLIGHT) asserts the precise named fallback
# instead of comparing. Fixtures: tests/fixtures/native-parity/objcomp/.
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_field_compositor_fixture_unit.c \
    -o "$test_dir/native-field-compositor-fixture-test"

"$test_dir/native-field-compositor-fixture-test" tests/fixtures/native-parity/objcomp

# Stage 3C transitional-tilemap fallback unit tests: the capture-time detector
# (BgRingDivergesFromScreenbase) that turns the 7 transitional mismatch frames
# into the NATIVE_FALLBACK_BG_TILEMAP_IN_FLIGHT named fallback -- settled frames
# pass, the frame-610 index-463 divergence flags, the fade-transparent (Class A)
# screenbase flags, off-window divergence does NOT, and the capture gate rejects
# end to end. Self-contained (no fixtures).
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    tests/native_field_compositor_transition_unit.c \
    -o "$test_dir/native-field-compositor-transition-test"

"$test_dir/native-field-compositor-transition-test"

printf '%s\n' 'native overworld renderer unit test passed'

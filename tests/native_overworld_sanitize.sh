#!/bin/sh
# ASan+UBSan build-and-run for every native-overworld unit test binary.
# Usage: sh tests/native_overworld_sanitize.sh
set -eu

SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Wl,--gc-sections"
COMMON="-iquote include -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1"
TMP=$(mktemp -d /tmp/native-overworld-sanitize.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

fail=0

run() {
    name=$1
    shift
    if ! gcc -std=gnu99 $SAN $COMMON "$@" -o "$TMP/$name"; then
        echo "BUILD FAIL: $name"
        fail=1
        return
    fi
    if ! "$TMP/$name" "${FIXTURES_DIR:-tests/fixtures/native-parity}"; then
        echo "RUNTIME FAIL: $name"
        fail=1
    else
        echo "PASS: $name"
    fi
}

run native_renderer_unit tests/native_overworld_renderer_unit.c
run native_zoom_input_unit tests/native_overworld_zoom_input_unit.c
run native_parity_unit tests/native_overworld_parity_unit.c
run native_parity_module_unit tests/native_overworld_parity_module_unit.c
run native_bg0_gate_unit tests/native_overworld_bg0_gate_unit.c
run native_sprite_snapshot_unit tests/native_sprite_snapshot_unit.c
run native_obj_renderer_unit tests/native_obj_renderer_unit.c
run native_field_compositor_unit tests/native_field_compositor_unit.c
# Transitional-tilemap fallback tests are self-contained (no fixtures).
run native_field_compositor_transition_unit tests/native_field_compositor_transition_unit.c
# Stage 4A expanded-viewport parity tests are self-contained (no fixtures).
run native_expanded_parity_unit tests/native_overworld_expanded_parity_unit.c
# Stage 4A continuous-viewport module tests are self-contained (no fixtures).
run native_viewport_unit tests/native_overworld_viewport_unit.c
# Stage 4A expanded world-coordinate tests A-K (independent-reference sweeps,
# connection strip, priority-across-providers, sprite expansion) are
# self-contained (no fixtures).
run native_expanded_world_unit tests/native_overworld_expanded_world_unit.c
# Stage 4A ring-residency regression tests A-J (stale-ring sentinel) are
# self-contained (no fixtures).
run native_ring_residency_unit tests/native_overworld_ring_residency_unit.c
# Stage 4A sequential-movement residency tests A-G (synthetic field-camera
# simulation) are self-contained (no fixtures).
run native_sequential_movement_unit tests/native_overworld_sequential_movement_unit.c
# Stage 4A Issue B stable-canvas fallback tests A-F (viewport-sized canvas,
# authoritative core centered bit-exact, pure-black margins, deterministic,
# selected viewport never mutated, non-preset/degenerate) are self-contained
# (no fixtures).
run native_fallback_canvas_unit tests/native_overworld_fallback_canvas_unit.c

# The Stage 3C runtime-fixture reproducer needs its own fixtures dir
# (tests/fixtures/native-parity/objcomp/), not the BG0 fixtures dir.
if ! gcc -std=gnu99 $SAN $COMMON tests/native_field_compositor_fixture_unit.c \
        -o "$TMP/native_field_compositor_fixture_unit"; then
    echo "BUILD FAIL: native_field_compositor_fixture_unit"
    fail=1
elif ! "$TMP/native_field_compositor_fixture_unit" "tests/fixtures/native-parity/objcomp"; then
    echo "RUNTIME FAIL: native_field_compositor_fixture_unit"
    fail=1
else
    echo "PASS: native_field_compositor_fixture_unit"
fi

# The scroll-fixture offline repro needs its own fixtures dir
# (tests/fixtures/native-parity/scroll/), not the BG0 fixtures dir.
if ! gcc -std=gnu99 $SAN $COMMON tests/native_overworld_scroll_fixture_unit.c \
        -o "$TMP/native_scroll_fixture_unit"; then
    echo "BUILD FAIL: native_scroll_fixture_unit"
    fail=1
elif ! "$TMP/native_scroll_fixture_unit" "tests/fixtures/native-parity/scroll"; then
    echo "RUNTIME FAIL: native_scroll_fixture_unit"
    fail=1
else
    echo "PASS: native_scroll_fixture_unit"
fi

if ! gcc -std=gnu99 $SAN $COMMON -DRENDERER_EASY_DRAW -c src/platform/gba_easy_draw.c -o "$TMP/gba_easy_draw.o"; then
    echo "BUILD FAIL: gba_easy_draw.o"
    fail=1
else
    if ! gcc -std=gnu99 $SAN $COMMON -DRENDERER_EASY_DRAW tests/native_overworld_real_oracle_unit.c "$TMP/gba_easy_draw.o" -o "$TMP/native_real_oracle_unit"; then
        echo "BUILD FAIL: native_real_oracle_unit"
        fail=1
    elif ! "$TMP/native_real_oracle_unit" "tests/fixtures/native-parity"; then
        echo "RUNTIME FAIL: native_real_oracle_unit"
        fail=1
    else
        echo "PASS: native_real_oracle_unit"
    fi
fi

# Stage 3B OBJ real-oracle harness under ASan+UBSan: same gba_easy_draw.o
# compile as the BG real-oracle harness above, then link+run the OBJ harness.
if ! gcc -std=gnu99 $SAN $COMMON -DRENDERER_EASY_DRAW -c src/platform/gba_easy_draw.c -o "$TMP/gba_easy_draw.o"; then
    echo "BUILD FAIL: gba_easy_draw.o"
    fail=1
else
    if ! gcc -std=gnu99 $SAN $COMMON -DRENDERER_EASY_DRAW tests/native_obj_renderer_oracle_unit.c "$TMP/gba_easy_draw.o" -o "$TMP/native_obj_oracle_unit"; then
        echo "BUILD FAIL: native_obj_oracle_unit"
        fail=1
    elif ! "$TMP/native_obj_oracle_unit"; then
        echo "RUNTIME FAIL: native_obj_oracle_unit"
        fail=1
    else
        echo "PASS: native_obj_oracle_unit"
    fi
fi

# Stage 3C field-compositor real-oracle harness under ASan+UBSan: same
# gba_easy_draw.o compile as the BG/OBJ real-oracle harnesses above.
if ! gcc -std=gnu99 $SAN $COMMON -DRENDERER_EASY_DRAW -c src/platform/gba_easy_draw.c -o "$TMP/gba_easy_draw.o"; then
    echo "BUILD FAIL: gba_easy_draw.o"
    fail=1
else
    if ! gcc -std=gnu99 $SAN $COMMON -DRENDERER_EASY_DRAW tests/native_field_compositor_oracle_unit.c "$TMP/gba_easy_draw.o" -o "$TMP/native_field_compositor_oracle_unit"; then
        echo "BUILD FAIL: native_field_compositor_oracle_unit"
        fail=1
    elif ! "$TMP/native_field_compositor_oracle_unit"; then
        echo "RUNTIME FAIL: native_field_compositor_oracle_unit"
        fail=1
    else
        echo "PASS: native_field_compositor_oracle_unit"
    fi
fi

if [ "$fail" -eq 0 ]; then
    echo "ALL SANITIZER BINARIES PASSED"
else
    echo "SANITIZER FAILURES PRESENT"
    exit 1
fi

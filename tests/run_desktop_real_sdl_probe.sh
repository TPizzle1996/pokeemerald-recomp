#!/usr/bin/env bash
#
# TEST C: production desktop-audio path probe (real SDL2 device, headless).
#
# The fake-SDL harness (TEST B in run_emerald_resource_state.sh) proves the
# desktop save/load call sequence and the resulting device state against a
# controllable device model. This probe runs the SAME sequence against the
# REAL SDL audio device (SDL_INIT_AUDIO only - no video, no window, no
# gameplay): real desktop_audio.c + real desktop_state.c linked against the
# system SDL2 library, with the real-time scheduler gate and its 0.25 s
# stall clamp. Assertions: the device opens with AUDIO_F32, mixer frames
# flow, save/load leaves the device PLAYING, the stall clamp resumes with
# exactly one due frame, the restored fixture is valid, and the saved frame
# counter rolls back.
#
# Compiles the same real machinery as run_emerald_resource_state.sh, but
# with -DHARNESS_REAL_SDL_PROBE: the fake-SDL shim is excluded, the probe
# accessor in desktop_audio.c is compiled, and SDL2/SDL.h resolves to the
# system headers.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"
pack="$root/games/emerald/base/emerald-bpee01-v1.rpack"

sdl_cflags="$(pkg-config --cflags sdl2)"
sdl_libs="$(pkg-config --libs sdl2)"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== generating map script/event stub symbols =="
nm -u "$root/build/linux64/data/maps.o" | awk '/_(MapScripts|MapEvents)$/ {print $2}' | sort -u > "$tmp/stub_names.txt"
python3 - "$tmp" <<'EOF'
import sys
tmp = sys.argv[1]
names = [line.strip() for line in open(tmp + "/stub_names.txt") if line.strip()]
# R13-G6: maps.o no longer references any *_MapScripts/_MapEvents
# (headers.inc is gated out of maps.s on linux64; the G-owned payload is
# pack-loaded only). An empty set is the EXPECTED post-G6 state — the
# stubs exist only to satisfy legacy undefined refs, so emit whatever
# (possibly nothing) the object actually needs.
print(f"map script/event stub symbols: {len(names)} (0 expected post-R13-G6)")
with open(tmp + "/map_script_stubs.s", "w") as f:
    f.write("# R11-E/F Harness C: inert script/event pointer targets\n")
    f.write("# (from nm -u maps.o; never dereferenced by the module).\n")
    for name in sorted(names):
        f.write("    .balign 8\n")
        f.write("    .globl %s\n%s:\n" % (name, name))
        f.write("    .quad 0\n")
EOF
gcc -c "$tmp/map_script_stubs.s" -o "$tmp/map_script_stubs.o"

echo "== compiling desktop-audio probe (real SDL2, no video, no window) =="
cd "$root"
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra -no-pie \
    -iquote include -iquote "$core_dir" -iquote "$emerald_dir" \
    $sdl_cflags \
    -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    -DDESKTOP_EXTERNAL_GAME_CONTENT \
    -DHARNESS_REAL_SDL_PROBE=1 \
    "$core_dir/sha256.c" \
    "$core_dir/sha1.c" \
    "$core_dir/resource_lz.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$core_dir/resource_content_digest.c" \
    "$core_dir/resource_pack.c" \
    "$core_dir/resource_pack_provider.c" \
    "$core_dir/toml.c" \
    "$core_dir/util.c" \
    "$core_dir/lz77.c" \
    "$emerald_dir/emerald_rom_profile.c" \
    "$emerald_dir/emerald_resource_compat.c" \
    "$emerald_dir/emerald_resource_ranges.c" \
    "$emerald_dir/emerald_resource_session.c" \
    "$emerald_dir/emerald_trainer_native_compat.c" \
    "$emerald_dir/emerald_pokemon_native_compat.c" \
    "$emerald_dir/emerald_object_event_compat.c" \
    "$emerald_dir/emerald_tileset_compat.c" \
    "$emerald_dir/emerald_layout_compat.c" \
    "$emerald_dir/emerald_runtime_loader.c" \
    "$emerald_dir/emerald_audio_compat.c" \
    "$emerald_dir/emerald_leaf_compat.c" \
    "$emerald_dir/leaf_native_table.generated.c" \
    "$emerald_dir/emerald_text_compat.c" \
    "$emerald_dir/text_arenas.generated.c" \
    "$emerald_dir/text_native_table.generated.c" \
    "$emerald_dir/text_bundle_index.generated.c" \
    "$emerald_dir/text_slot_bindings.generated.c" \
    "$emerald_dir/text_slots_table.generated.c" \
    "$emerald_dir/text_skeletons_table.generated.c" \
    "$emerald_dir/text_skeleton_arrays.generated.c" \
    "$here/emerald_text_harness_stubs.c" \
    "$emerald_dir/gameplay_data_native.c" \
    "$emerald_dir/gameplay_native_table.generated.c" \
    "$emerald_dir/gameplay_levelup.generated.c" \
    "$emerald_dir/gameplay_callbacks.generated.c" \
    "$emerald_dir/gameplay_item_callbacks_native.c" \
    "$emerald_dir/emerald_gameplay_compat.c" \
    "$here/emerald_gameplay_harness_stubs.c" \
    "$emerald_dir/trainer_data_native.c" \
    "$emerald_dir/trainer_native.generated.c" \
    "$emerald_dir/emerald_trainer_compat.c" \
    "$emerald_dir/encounter_data_native.c" \
    "$emerald_dir/encounter_native.generated.c" \
    "$emerald_dir/emerald_encounter_compat.c" \
    "$emerald_dir/frontier_data_native.c" \
    "$emerald_dir/frontier_native.generated.c" \
    "$emerald_dir/frontier_aux_native.generated.c" \
    "$emerald_dir/emerald_frontier_compat.c" \
    "$emerald_dir/pokedex_data_native.c" \
    "$emerald_dir/pokedex_native.generated.c" \
    "$emerald_dir/emerald_pokedex_compat.c" \
    "$emerald_dir/map_data_native.c" \
    "$emerald_dir/map_native.generated.c" \
    "$emerald_dir/emerald_map_compat.c" \
    "$emerald_dir/emerald_script_compat.c" \
    "$emerald_dir/emerald_script_state.c" \
    "$emerald_dir/script_native_table.generated.c" \
    "$here/emerald_script_harness_stubs.c" \
    "$emerald_dir/emerald_battle_compat.c" \
    "$emerald_dir/emerald_battle_state.c" \
    "$emerald_dir/battle_native_table.generated.c" \
    "$root/src/platform/native_state.c" \
    "$root/src/platform/host_memory.c" \
    "$root/src/platform/native_world_neighborhood.c" \
    "$root/src/platform/desktop_audio.c" \
    "$root/src/platform/desktop_state.c" \
    "$root/build/linux64/data/maps.o" \
    "$tmp/map_script_stubs.o" \
    "$here/emerald_native_world_overworld_stub.c" \
    "$here/emerald_resource_state_stub.c" \
    "$here/emerald_resource_state_test.c" \
    $sdl_libs -lm \
    -o "$tmp/emerald_desktop_real_sdl_probe"

cd "$tmp"
state="harness-real-sdl-slot-7.st"

echo "== TEST C: desktop save/load sequence on the REAL SDL audio device =="
"$tmp/emerald_desktop_real_sdl_probe" desktop-real-sdl "$pack" "$state" > probe.log
cat probe.log
grep -q "DESKTOP-REAL-SDL ok" probe.log
echo "TEST C ok (real device opens AUDIO_F32, mixer flows, load leaves device playing)"

echo
echo "desktop real-SDL probe: ALL PASSED"

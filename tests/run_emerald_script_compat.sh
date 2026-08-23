#!/usr/bin/env bash
#
# R13-G3 focused test: EmeraldScriptCompat shadow staging + typed
# resolver seam (tests/emerald_script_compat_test.c).
#
# Same REAL native harness as the runtime loader test (real pack
# reader + writer, real session core, the published R13-C text and
# R13-B leaf seams as sibling dependencies) plus the new shadow seam
# and its generated inventory. The seam is SHADOW-ONLY: the test
# proves the arena, the 16,704-row source index, the typed
# resolutions, the staged gStdScripts/F surfaces, reverse
# containment, the generation lifecycle, pack-variant refusals and
# the State-v5 range-index invariant (no registration, no range over
# the shadow arena).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling emerald script compat test (real native flags) =="
cd "$root"
FLAGS=(-std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -no-pie -Wall -Wextra \
    -iquote include -iquote "$core_dir" \
    -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    -DDESKTOP_EXTERNAL_GAME_CONTENT)

# The trainer fixture TU defines the native pic-table fill targets the
# trainer compat seam references; its own main() is renamed away.
gcc "${FLAGS[@]}" -Dmain=emerald_script_trainer_fixtures_main \
    -c "$here/emerald_trainer_native_compat_test.c" -o "$tmp/trainer_fixtures.o"

gcc "${FLAGS[@]}" \
    "$core_dir/sha256.c" \
    "$core_dir/sha1.c" \
    "$core_dir/resource_lz.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$core_dir/resource_content_digest.c" \
    "$core_dir/resource_pack.c" \
    "$core_dir/resource_pack_writer.c" \
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
    "$here/emerald_tileset_compat_stubs.c" \
    "$emerald_dir/emerald_audio_compat.c" \
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
    "$here/emerald_map_harness_host.c" \
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
    "$emerald_dir/emerald_runtime_loader.c" \
    "$emerald_dir/emerald_script_compat.c" \
    "$emerald_dir/emerald_script_state.c" \
    "$emerald_dir/script_native_table.generated.c" \
    "$here/emerald_script_harness_stubs.c" \
    "$here/emerald_script_compat_test.c" \
    "$tmp/trainer_fixtures.o" \
    -o "$tmp/emerald_script_compat_test"

echo "== running =="
"$tmp/emerald_script_compat_test" \
    "$tmp" \
    "$root/games/emerald/base/emerald-bpee01-v1.rpack"

# Isolation sweep (plan sec 24/25): the seam must not touch the live
# VM, the live tables, or the pack pipeline. A structural grep gate -
# any reference to the live-execution surfaces is a G3 refusal.
echo "== isolation sweep (seam sources) =="
# Comments are excluded: the header documents that these surfaces stay
# untouched, which is exactly the contract. The gStdScripts reference
# must be exactly the publication stores (PublishStdScripts).
# R13-G5: gStdScripts is now the seam's own publication target (plan
# sec 9) - the sweep still gates every other live-execution surface.
for banned in ScriptReadPointer sAddressOffset MapHeader ObjectEventTemplate CoordEvent BgEvent HostResolveGbaAddr; do
    if grep -n "$banned" "$emerald_dir/emerald_script_compat.c" \
       include/emerald/resources/emerald_script_compat.h \
       | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)' > /dev/null; then
        echo "ISOLATION FAIL: $banned referenced by the G3 seam"
        exit 1
    fi
done
echo "isolation sweep clean"
echo "emerald script compat test passed"

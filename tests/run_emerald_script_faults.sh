#!/usr/bin/env bash
#
# R13-G3 fault-injection matrix (plan sec 17): each fault mutates one
# generated-table row (tests/fault_script_table.py) and the seam is
# compiled against the mutated copy. TryInitialize must refuse with
# the exact status the fault spec names - no fallback, no partial
# publication. Also covers the OOM path via ulimit (partial
# allocation failure) and the pack-level resource faults via the main
# test binary's variant packs.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

FLAGS=(-std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra \
    -iquote include -iquote "$core_dir" \
    -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    -DDESKTOP_EXTERNAL_GAME_CONTENT)

CORES=( \
    "$core_dir/sha256.c" "$core_dir/sha1.c" "$core_dir/resource_lz.c" \
    "$core_dir/resource_id.c" "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" "$core_dir/resource_content_digest.c" \
    "$core_dir/resource_pack.c" "$core_dir/resource_pack_writer.c" \
    "$core_dir/resource_pack_provider.c" "$core_dir/toml.c" \
    "$core_dir/util.c" "$core_dir/lz77.c" )

SEAMS=( \
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
    "$emerald_dir/emerald_runtime_loader.c" )

cd "$root"
gcc "${FLAGS[@]}" -Dmain=emerald_script_trainer_fixtures_main \
    -c "$here/emerald_trainer_native_compat_test.c" -o "$tmp/trainer_fixtures.o"

TABLE="$emerald_dir/script_native_table.generated.c"
PACK="$root/games/emerald/base/emerald-bpee01-v1.rpack"
failures=0
run=0

for fault in seg-overlap seg-gap seg-kind export-outside export-dup \
             reloc-raw reloc-overlap reloc-class text-missing \
             boundary-dup mart-sentinel f-missing std-missing ram-bad \
             bridge-bad dyn-gap module-digest module-arena \
             f-routing-object export-missing; do
    run=$((run + 1))
    expected="$(python3 "$here/fault_script_table.py" "$fault" \
        "$TABLE" "$tmp/${fault}_table.c" | cut -d' ' -f3)"
    echo "== fault $fault (expect $expected) =="
    if ! gcc "${FLAGS[@]}" \
        "${CORES[@]}" "${SEAMS[@]}" \
        "$emerald_dir/emerald_script_compat.c" \
        "$emerald_dir/emerald_script_state.c" \
        "$tmp/${fault}_table.c" \
        "$here/emerald_script_harness_stubs.c" \
        "$here/emerald_script_fault_test.c" \
        "$tmp/trainer_fixtures.o" \
        -o "$tmp/fault_${fault}" 2> "$tmp/fault_${fault}.cc.log"; then
        echo "FAULT COMPILE FAILED: $fault"
        tail -3 "$tmp/fault_${fault}.cc.log"
        failures=$((failures + 1))
        continue
    fi
    if ! "$tmp/fault_${fault}" "$expected" "$tmp" "$PACK"; then
        failures=$((failures + 1))
    fi
done

# OOM path (plan sec 17 GENERATION faults): the seam is compiled with
# the test-hook macro; the driver caps the stage allocation budget so
# the source-index malloc fails after the arena + span index succeed -
# a deterministic partial-allocation failure. The refusal must leave no
# generation behind.
echo "== fault oom-partial (alloc budget) =="
run=$((run + 1))
if gcc "${FLAGS[@]}" -DEMERALD_SCRIPT_COMPAT_TEST_HOOKS \
    "${CORES[@]}" "${SEAMS[@]}" \
    "$emerald_dir/emerald_script_compat.c" \
    "$TABLE" \
    "$here/emerald_script_harness_stubs.c" \
    "$here/emerald_script_fault_test.c" \
    "$tmp/trainer_fixtures.o" \
    -o "$tmp/fault_oom" 2>/dev/null; then
    if "$tmp/fault_oom" EMERALD_SCRIPT_ERR_OUT_OF_MEMORY \
        "$tmp" "$PACK"; then
        echo "oom fault refused as expected"
    else
        echo "OOM FAULT FAILED"
        failures=$((failures + 1))
    fi
else
    echo "OOM FAULT COMPILE FAILED"
    failures=$((failures + 1))
fi

if [ "$failures" != "0" ]; then
    echo "emerald script fault matrix FAILED: $failures/$run"
    exit 1
fi
echo "emerald script fault matrix passed ($run faults)"

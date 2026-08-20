#!/usr/bin/env bash
#
# R9 §8d runtime loader registration test
# (tests/emerald_runtime_loader_test.c).
#
# Same REAL native harness as the R7B/R8 unit runner (real decompressor
# bios.c LZ77UnCompWram, real native trainer tables, real decompress.c load
# path, the compatibility seams), plus the R4 session + pack reader + pack
# WRITER, and the R6 runtime loader (emerald_runtime_loader.c) compiled into
# the test TU:
#
#   1. a trainer-ONLY pack (196 entries, built in-test from the committed
#      fixtures via the pack writer) is REFUSED by RegisterRuntimeSnapshot -
#      R9 §8 makes registration publish through the strict init, so a pack
#      that cannot serve the Pokémon battle family is a refused session;
#      the loader rolls every table back to its NULL sentinel;
#   2. NULL/empty/absent pack paths are fail-closed UNAVAILABLE;
#   3. writer-rebuilt production-pack variants whose text family is
#      malformed (gText_123Dot payload zeroed) or incomplete (the record
#      dropped) are REFUSED sessions - the loader rolls every published
#      pointer back and the snapshot is not registered;
#   4. the REAL production pack
#      (games/emerald/base/emerald-bpee01-v1.rpack) registers OK and
#      publishes the full trainer + Pokémon battle families AND the
#      sixteen R13-C text arenas (slots + skeleton fills, including the
#      #112 gText_123Dot byte-offset fills), with the State-v5
#      currentChar range routing round-trip;
#   5. a failed direct republish (malformed/missing variant) leaves the
#      live registered session untouched, and the fail-closed clear
#      NULLs every applied pointer.
#
# R13-C text link: the generated skeleton machinery
# (text_skeleton_arrays.generated.c + the seven inventory/slot/skeleton
# tables + emerald_text_compat.c) and the compiled-constant stubs
# (emerald_text_harness_stubs.c: 17 data externs + 55 void(u8) action
# callbacks the generated arrays reference but the harness never runs).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling runtime loader test (real native flags + R4 session + writer) =="
cd "$root"
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra \
    -iquote include -iquote "$core_dir" \
    -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    -DDESKTOP_EXTERNAL_GAME_CONTENT \
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
    "$emerald_dir/emerald_trainer_native_compat.c" \
    "$emerald_dir/emerald_pokemon_native_compat.c" \
    "$emerald_dir/emerald_object_event_compat.c" \
    "$emerald_dir/emerald_tileset_compat.c" \
    "$emerald_dir/emerald_layout_compat.c" \
    "$here/emerald_tileset_compat_stubs.c" \
    "$emerald_dir/emerald_resource_session.c" \
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
    "$here/emerald_runtime_loader_test.c" \
    -o "$tmp/emerald_runtime_loader_test"

echo "== running =="
"$tmp/emerald_runtime_loader_test" \
    "$tmp" \
    "$root/games/emerald/base/emerald-bpee01-v1.rpack"

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
#   3. the REAL production pack
#      (games/emerald/base/emerald-bpee01-v1.rpack) registers OK and
#      publishes the full trainer + Pokémon battle families.
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
    "$here/emerald_runtime_loader_test.c" \
    -o "$tmp/emerald_runtime_loader_test"

echo "== running =="
"$tmp/emerald_runtime_loader_test" \
    "$tmp" \
    "$root/games/emerald/base/emerald-bpee01-v1.rpack"

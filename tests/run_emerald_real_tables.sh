#!/usr/bin/env bash
#
# R9 §8f real-table-headers test (tests/emerald_real_tables_test.c).
#
# Compiles the REAL src/data.c native branch (the actual
# data/pokemon_graphics/ table headers) into a standalone TU with the real
# compatibility seams, the R4 session/pack chain, the R6 runtime loader and
# the real decompressor (src/platform/bios.c), then verifies:
#
#   1. the compiled tables against the generated slot map - every battle
#      table spans exactly POKEMON_BATTLE_SLOTS_PER_TABLE rows, every row
#      carries tag == row index, every slot starts at the NULL sentinel
#      except the one permanent external slot (back[412] back-EGG row =
#      compiled gMonStillFrontPic_Egg leaf), and the generated slot map has
#      exactly one external marker at slot 852;
#   2. the REAL production pack
#      (games/emerald/base/emerald-bpee01-v1.rpack) registers through the
#      strict publish-at-registration path: every non-external battle slot
#      publishes with tags/sizes retained, the external back-EGG row is
#      untouched, the compiled still-front table is never touched, and the
#      trainer family publishes into the real data.c tables.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling real tables test (real data.c native branch + seams + session) =="
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
    "$core_dir/resource_pack_provider.c" \
    "$core_dir/toml.c" \
    "$core_dir/util.c" \
    "$core_dir/lz77.c" \
    "$emerald_dir/emerald_rom_profile.c" \
    "$emerald_dir/emerald_resource_compat.c" \
    "$emerald_dir/emerald_trainer_native_compat.c" \
    "$emerald_dir/emerald_pokemon_native_compat.c" \
    "$emerald_dir/emerald_resource_session.c" \
    "$here/emerald_real_tables_test.c" \
    -o "$tmp/emerald_real_tables_test"

echo "== running =="
"$tmp/emerald_real_tables_test" \
    "$root/games/emerald/base/emerald-bpee01-v1.rpack"

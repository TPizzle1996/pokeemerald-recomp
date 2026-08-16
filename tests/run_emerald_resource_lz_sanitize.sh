#!/usr/bin/env bash
#
# Stage R5 codec + compatibility-image ASan + UBSan + leak-detection run.
# Same isolation as run_emerald_resource_lz.sh, plus sanitizers.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling codec + compat image + tests (ASan+UBSan) =="
gcc -std=c99 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -iquote "$root/include" \
    -iquote "$core_dir" \
    "$core_dir/sha256.c" \
    "$core_dir/resource_lz.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$core_dir/toml.c" \
    "$core_dir/util.c" \
    "$emerald_dir/emerald_resource_compat.c" \
    "$here/emerald_resource_lz_unit.c" \
    -o "$tmp/emerald_resource_lz_unit_san"

echo "== running (detect_leaks=1) =="
cd "$root"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
"$tmp/emerald_resource_lz_unit_san"

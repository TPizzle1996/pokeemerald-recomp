#!/usr/bin/env bash
#
# Stage R5 native trainer-table test under ASan + UBSan + leak detection.
# Same sources and flags as run_emerald_trainer_native_compat.sh, plus
# sanitizers. The test frees every fixture and the module releases the session
# image at Shutdown, so leak detection must come back clean.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling native trainer-table test (ASan+UBSan) =="
cd "$root"
gcc -std=gnu99 -O1 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -iquote include -iquote "$core_dir" \
    -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    "$core_dir/sha256.c" \
    "$core_dir/resource_lz.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$core_dir/toml.c" \
    "$core_dir/util.c" \
    "$emerald_dir/emerald_resource_compat.c" \
    "$emerald_dir/emerald_trainer_native_compat.c" \
    "$emerald_dir/emerald_pokemon_native_compat.c" \
    "$here/emerald_trainer_native_compat_test.c" \
    -o "$tmp/emerald_trainer_native_compat_test_san"

echo "== running (detect_leaks=1) =="
cd "$root"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
"$tmp/emerald_trainer_native_compat_test_san"

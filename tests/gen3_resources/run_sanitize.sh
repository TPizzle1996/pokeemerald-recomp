#!/usr/bin/env bash
#
# Stage M0/M1 ASan + UBSan run of the shared Gen3 resource core tests
# (guardrail 19). Same isolation flags as run.sh, plus the two sanitizers and
# leak detection.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
core_dir="$root/src/gen3/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling shared core + tests (ASan+UBSan) =="
gcc -std=c99 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -iquote "$root/include" \
    -iquote "$core_dir" \
    "$core_dir/sha256.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$here/resource_core_test.c" \
    -o "$tmp/gen3_resource_core_test_san"

echo "== running (detect_leaks=1) =="
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
"$tmp/gen3_resource_core_test_san"

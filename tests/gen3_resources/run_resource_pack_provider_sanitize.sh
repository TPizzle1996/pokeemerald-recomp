#!/usr/bin/env bash
#
# Stage R4 shared pack->provider adapter ASan + UBSan + leak-detection run.
# Same build isolation as run_resource_pack_provider.sh, plus sanitizers.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
core_dir="$root/src/gen3/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling adapter + deps + tests (ASan+UBSan) =="
gcc -std=c99 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -iquote "$root/include" \
    -iquote "$core_dir" \
    "$core_dir/sha256.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$core_dir/resource_content_digest.c" \
    "$core_dir/resource_pack.c" \
    "$core_dir/resource_pack_writer.c" \
    "$core_dir/resource_pack_provider.c" \
    "$here/resource_pack_provider_test.c" \
    -o "$tmp/gen3_resource_pack_provider_test_san"

echo "== running (detect_leaks=1) =="
cd "$root"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
"$tmp/gen3_resource_pack_provider_test_san"

#!/usr/bin/env bash
#
# R10-F: session content fingerprint tests (tests/emerald_session_fingerprint_test.c).
#
# Compiles the session module against the real sha256/resource core and pins
# the exact construction: determinism, provider-order sensitivity (TEST 6),
# content sensitivity, game-id sensitivity, strict argument validation.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mode="${1:-}"
sanitize_flags=""
if [ "$mode" = "sanitize" ]; then
    sanitize_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
fi

echo "== compiling session fingerprint test =="
cd "$root"
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra $sanitize_flags \
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
    "$emerald_dir/emerald_resource_session.c" \
    "$here/emerald_session_fingerprint_test.c" \
    -o "$tmp/emerald_session_fingerprint_test"

echo "== running =="
"$tmp/emerald_session_fingerprint_test"

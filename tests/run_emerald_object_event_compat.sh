#!/usr/bin/env bash
#
# R11-B: object-event compatibility seam tests
# (tests/emerald_object_event_compat_test.c).
#
# Compiles the real seam against the generated native tables + a synthetic
# snapshot carrying all 288 object-event resources, then pins resolution,
# frame hydration (all 1,788 rows), palette publication, byte parity,
# lifecycle and transactional failure.
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

echo "== compiling object-event compat test (real seam + generated tables) =="
cd "$root"
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra $sanitize_flags \
    -iquote include -iquote "$core_dir" -iquote "$emerald_dir" \
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
    "$emerald_dir/emerald_object_event_compat.c" \
    "$here/emerald_object_event_compat_test.c" \
    -o "$tmp/emerald_object_event_compat_test"

echo "== running =="
"$tmp/emerald_object_event_compat_test"

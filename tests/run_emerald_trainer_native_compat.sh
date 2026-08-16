#!/usr/bin/env bash
#
# Stage R5/R6/R7B native trainer-table test runner
# (tests/emerald_trainer_native_compat_test.c).
#
# Compiles the REAL Emerald decompressor (bios.c LZ77UnCompWram), the REAL
# native trainer tables (front_pic_tables.h + back_pic_tables.h non-const
# branches), the REAL load-path functions (decompress.c), the gen3 M0/M1
# resolver stack, and the R7B compatibility seam with the REAL native target
# flags, then drives the whole pipeline from the committed trainer-front
# family .lz assets driven by the R7A family descriptor
# (resources/extraction/emerald/bpee01/trainer_front_family.toml) - no user
# ROM.
#
# Assertions it pins (R5 §7/§9/§10/§11/§12/§13/§14/§17/§24/§25, R7B §6):
#   - pristine pre-publish NULL-sentinel baseline across all 192 migrated
#     slots (front sheets + front palettes + 6 shared back-pic palettes),
#     Red/Leaf back-only palettes keep compiled pointers;
#   - publication mutates ONLY the migrated .data slots - size/tag and every
#     non-migrated entry (Red/Leaf back palettes, back SHEET table) are
#     byte-identical before/after;
#   - decompress(published stream) == canonical fixture for all 93 sheets +
#     93 palettes, at the descriptor's table index (mapping-drift check);
#     shared back-pic palette consumers reuse the owner's front palette
#     stream; real DecompressPicFromTable_2 + LoadCompressedSpritePalette
#     load path for the full family;
#   - fail-closed resolve (missing/wrong-size/wrong-winner) and mismatched
#     image publish leave the tables untouched; transactional publish;
#   - runtime hook gating + idempotence; R7B republish repairs stale
#     process-local words in all 192 slots, is idempotent, and never touches
#     non-migrated entries; republish unavailable after shutdown.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling native trainer-table test (real native flags) =="
cd "$root"
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra \
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
    "$here/emerald_trainer_native_compat_test.c" \
    -o "$tmp/emerald_trainer_native_compat_test"

echo "== running =="
"$tmp/emerald_trainer_native_compat_test"

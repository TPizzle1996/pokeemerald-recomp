#!/usr/bin/env bash
#
# Stage 7 production compatibility / linker-load proof (family-wide, R7B)
# (tests/emerald_trainer_native_compat_production.c).
#
# Same REAL native harness as run_emerald_trainer_native_compat.sh (real
# decompressor bios.c LZ77UnCompWram, real native trainer tables, real
# decompress.c load path, R7B compat seam, native target flags), plus the R4
# session + pack reader + catalog loader, but the snapshot is built from the
# REAL installed production pack (games/emerald/base/emerald-bpee01-v1.rpack)
# rather than from the committed assets:
#
#   retail ROM -> production manifest -> emerald-bpee01-v1.rpack
#     -> EmeraldResourceSession_BuildRomBaseCandidate
#     -> Gen3ResourceCandidate_Build (3636-resource ROM_BASE snapshot,
#        196 trainer + 1608 Pokemon battle + 288 object-event + 1544 tileset,
#        R11-C)
#     -> EmeraldResourceCompat_InitializeFromSnapshot
#     -> EmeraldResourceCompatibilityImage (trainer + Pokemon battle +
#        object-event + tileset families)
#     -> live native trainer + Pokemon battle tables -> real loaders
#
# Byte-for-byte vs canonical = the family descriptor
# (resources/extraction/emerald/bpee01/trainer_front_family.toml) drives the
# 186 raw source artifacts (graphics/trainers/front_pics/<slug>.4bpp.lz +
# graphics/trainers/<palette_dir>/<slug>.gbapal.lz, decoded by the real
# decompressor) and the resource-id -> table-slot mapping.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling production compat proof (real native flags + R4 session) =="
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
    "$here/emerald_trainer_native_compat_production.c" \
    -o "$tmp/emerald_trainer_native_compat_production"

echo "== running =="
"$tmp/emerald_trainer_native_compat_production" \
    "$root/games/emerald/base/emerald-bpee01-v1.rpack" \
    --catalog "$root/resources/catalogs/emerald/catalog.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/pokemon_battle/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/object_event/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/tileset/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/layout/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/audio/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/movement/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/multiboot/catalog.generated.toml" \
    --descriptor "$root/resources/extraction/emerald/bpee01/trainer_front_family.toml"

#!/usr/bin/env bash
#
# Stage R7A ASan + UBSan + leak-detection run of the trainer-front family
# tooling. Same build isolation as run_trainer_family.sh (R7A §24: only
# -std=c99 -Wall -Wextra -Werror and the Gen3 core include roots; no Emerald
# defines, no global.h/gba/SDL), plus the two sanitizers.
#
# Coverage (R7A §25 "new R7A tests plain + ASan/UBSan"):
#   1. the family generator over the COMMITTED descriptor (full 93-trainer /
#      186-resource parse, strict LZ77 decode of every artifact, M0/M1 key
#      derivation, catalog/bindings/ownership/consumers generation);
#   2. the generator's failure paths (malformed descriptor + a duplicate-id
#      diagnostic) so error handling runs under ASan too;
#   3. the R2 full-family pack unit (186-entry deterministic v1 .rpack).
#
# No user ROM, no game launch. Run from anywhere (repo root is derived).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
family_dir="$root/tools/gen3_resources/trainer_family"
core_dir="$root/src/gen3/resources"
bpee01="$root/resources/extraction/emerald/bpee01"
catalog="$root/resources/catalogs/emerald/catalog.toml"
descriptor="$bpee01/trainer_front_family.toml"
back_descriptor="$bpee01/trainer_back_family.toml"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"; rm -f "$root/graphics/trainers/front_pics/zzzslug.4bpp.lz" "$root/graphics/trainers/front_pics/zzzslug.gbapal.lz"' EXIT

failures=0
passes=0
pass() { passes=$((passes+1)); echo "  PASS $1"; }
fail() { failures=$((failures+1)); echo "  FAIL $1" >&2; }

echo "== compiling family generator (ASan+UBSan) =="
gcc -std=c99 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -iquote "$root/include" \
    -iquote "$core_dir" \
    "$family_dir/gen_trainer_family.c" \
    "$core_dir/sha256.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/util.c" \
    "$core_dir/lz77.c" \
    "$core_dir/toml.c" \
    -o "$tmp/gen-trainer-family-san"
pass "gen-trainer-family builds under ASan/UBSan"

echo "== compiling R2 full-family pack unit (ASan+UBSan) =="
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
    "$core_dir/util.c" \
    "$core_dir/lz77.c" \
    "$core_dir/toml.c" \
    "$here/trainer_family_pack_unit.c" \
    -o "$tmp/trainer_family_pack_unit_san"
pass "trainer_family_pack_unit builds under ASan/UBSan"

cd "$root"
SAN_OPTS="ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"

echo "== generator --check over the committed descriptors =="
if env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$tmp/gen-trainer-family-san" --descriptor "$descriptor" \
        --back-descriptor "$back_descriptor" \
        --catalog-out "$catalog" \
        --bindings-out "$bpee01/bindings.generated.toml" \
        --ownership-out "$bpee01/ownership.generated.toml" \
        --consumers-out "$bpee01/trainer_front_consumers.generated.toml" \
        --back-bindings-out "$bpee01/back_bindings.generated.toml" \
        --back-ownership-out "$bpee01/back_ownership.generated.toml" \
        --back-consumers-out "$bpee01/trainer_back_consumers.generated.toml" \
        --check >"$tmp/check.log" 2>&1; then
    pass "generator --check under ASan/UBSan (196 resources)"
else
    fail "generator --check under ASan/UBSan (see $tmp/check.log)"
fi

echo "== generator failure paths under ASan/UBSan =="
# Malformed descriptor: must fail with the structured diagnostic, not a crash.
printf 'this is not toml [[[[\n' >"$tmp/fail_malformed.toml"
if env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$tmp/gen-trainer-family-san" --descriptor "$tmp/fail_malformed.toml" \
        --catalog-out "$tmp/failout/c.toml" --bindings-out "$tmp/failout/b.toml" \
        --ownership-out "$tmp/failout/o.toml" --consumers-out "$tmp/failout/cn.toml" \
        >"$tmp/fail_malformed.log" 2>&1; then
    fail "malformed descriptor under ASan/UBSan (generator unexpectedly succeeded)"
elif grep -qF "malformed family descriptor" "$tmp/fail_malformed.log"; then
    pass "malformed descriptor fails closed under ASan/UBSan"
else
    fail "malformed descriptor under ASan/UBSan (wrong diagnostic)"
fi

# Duplicate canonical id: needs a resolvable throwaway artifact so the intended
# validation (not artifact loading) fires. Copied from hiker; removed on exit.
cp "$root/graphics/trainers/front_pics/hiker.4bpp.lz" \
   "$root/graphics/trainers/front_pics/zzzslug.4bpp.lz"
cp "$root/graphics/trainers/front_pics/hiker.gbapal.lz" \
   "$root/graphics/trainers/front_pics/zzzslug.gbapal.lz"
python3 - "$descriptor" "$tmp/fail_dup.toml" <<'PY'
import re, sys
src, out = sys.argv[1], sys.argv[2]
text = open(src).read()
parts = text.split('[[trainers]]')
header, blocks = parts[0], parts[1:]
dup = blocks[-1]
for key, val in (('slug', '"zzzslug"'), ('symbol', '"zzz"'), ('pic', '"ZZZ"'),
                 ('index', '999'), ('palette_dir', '"front_pics"')):
    dup = re.sub(r'^%s = .*$' % key, '%s = %s' % (key, val), dup, flags=re.M)
blocks.append(dup)
open(out, 'w').write(header + ''.join('[[trainers]]' + b for b in blocks))
PY
if env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$tmp/gen-trainer-family-san" --descriptor "$tmp/fail_dup.toml" \
        --catalog-out "$tmp/failout/c.toml" --bindings-out "$tmp/failout/b.toml" \
        --ownership-out "$tmp/failout/o.toml" --consumers-out "$tmp/failout/cn.toml" \
        >"$tmp/fail_dup.log" 2>&1; then
    fail "duplicate canonical id under ASan/UBSan (generator unexpectedly succeeded)"
elif grep -qF "duplicate canonical id" "$tmp/fail_dup.log"; then
    pass "duplicate canonical id fails closed under ASan/UBSan"
else
    fail "duplicate canonical id under ASan/UBSan (wrong diagnostic)"
fi
rm -f "$root/graphics/trainers/front_pics/zzzslug.4bpp.lz" \
      "$root/graphics/trainers/front_pics/zzzslug.gbapal.lz"

echo "== R2 full-family pack under ASan/UBSan =="
if env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$tmp/trainer_family_pack_unit_san" >"$tmp/pack.log" 2>&1; then
    cat "$tmp/pack.log"
    if grep -q "0 failures" "$tmp/pack.log"; then
        pass "full-family pack under ASan/UBSan"
    else
        fail "full-family pack under ASan/UBSan (checks failed)"
    fi
else
    cat "$tmp/pack.log" >&2
    fail "full-family pack under ASan/UBSan"
fi

echo ""
echo "== $passes passes, $failures failures =="
[ "$failures" -eq 0 ]

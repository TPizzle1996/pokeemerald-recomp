#!/usr/bin/env bash
#
# Stage R7A/R8 trainer family test runner (front + back).
#
# Covers the family generator end to end without a user ROM:
#   A  generator --check: the seven committed outputs (front + back families)
#      regenerate byte-for-byte.
#   B  regeneration to a temp dir is byte-identical to the committed files.
#   C  order-independence (§17): shuffled [[trainers]] descriptors produce
#      the identical output; two independent runs are byte-identical.
#   D  failure matrix (§18): malformed descriptors fail closed with the
#      expected structured diagnostics.
#   E  completeness + cross-reference (§19): both families against the C
#      tree and every generated file, plus the canonical-name mapping (§4),
#      consumer-map integrity (§13) and M0/M1 key independence.
#   F  R1A scale (§21): a representative multi-resource fixture (6 trainers,
#      12 resources) runs through gen3-elf-manifest; the checked-in 186-record
#      bindings are also exercised by the completeness pass.
#   G  R2 full-family pack (§22): the pack unit test builds a deterministic
#      v1 .rpack over all 186 resources (run separately by
#      run_trainer_family_sanitize.sh for the sanitizer variant).
#
# Build-isolated like the R1A runner: -std=c99 -Wall -Wextra -Werror plus the
# tool include roots and the shared Gen3 core. No Emerald defines, no
# global.h/gba/SDL. Tests never require a user ROM and never launch the game.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cd "$root"
gen_dir="$root/tools/gen3_resources/trainer_family"
elf_dir="$root/tools/gen3_resources/elf_manifest"
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

# The seven output path pairs: (emitted name, committed path). The generator
# emits the catalog as a union and the per-family bindings/ownership/consumers.
gen4() { # gen4 <front-descriptor> <back-descriptor> <outdir>
    "$gen_dir/gen-trainer-family" --descriptor "$1" --back-descriptor "$2" \
        --catalog-out "$3/catalog.toml" \
        --bindings-out "$3/bindings.generated.toml" \
        --ownership-out "$3/ownership.generated.toml" \
        --consumers-out "$3/trainer_front_consumers.generated.toml" \
        --back-bindings-out "$3/back_bindings.generated.toml" \
        --back-ownership-out "$3/back_ownership.generated.toml" \
        --back-consumers-out "$3/trainer_back_consumers.generated.toml"
}

commit_for() { # commit_for <name>
    if [ "$1" = "catalog.toml" ]; then printf '%s' "$catalog"; else printf '%s' "$bpee01/$1"; fi
}

compare_outputs() { # compare_outputs <outdir> <label>
    local dir="$1" label="$2" f ref
    for f in catalog.toml bindings.generated.toml ownership.generated.toml \
             trainer_front_consumers.generated.toml back_bindings.generated.toml \
             back_ownership.generated.toml trainer_back_consumers.generated.toml; do
        ref="$(commit_for "$f")"
        if cmp -s "$ref" "$dir/$f"; then
            pass "$label: $f byte-identical"
        else
            fail "$label: $f differs from committed"
        fi
    done
}

echo "== building the family generator =="
make -C "$gen_dir" all
pass "gen-trainer-family builds"

echo "== Part A: --check on the committed outputs =="
if "$gen_dir/gen-trainer-family" --descriptor "$descriptor" \
        --back-descriptor "$back_descriptor" \
        --catalog-out "$catalog" \
        --bindings-out "$bpee01/bindings.generated.toml" \
        --ownership-out "$bpee01/ownership.generated.toml" \
        --consumers-out "$bpee01/trainer_front_consumers.generated.toml" \
        --back-bindings-out "$bpee01/back_bindings.generated.toml" \
        --back-ownership-out "$bpee01/back_ownership.generated.toml" \
        --back-consumers-out "$bpee01/trainer_back_consumers.generated.toml" \
        --check >"$tmp/check.log" 2>&1; then
    pass "generator --check passes on all seven committed outputs"
    if grep -q "196 resources (186 front + 10 back), 93/8 trainers" "$tmp/check.log"; then
        pass "--check reports 196 resources / 93+8 trainers"
    else
        fail "--check count message missing (see $tmp/check.log)"
    fi
else
    fail "generator --check failed"
fi

echo "== Part B: regeneration to temp is byte-identical =="
mkdir -p "$tmp/genb"
cd "$root"
gen4 "$descriptor" "$back_descriptor" "$tmp/genb" >/dev/null 2>&1
compare_outputs "$tmp/genb" "Part B"

echo "== Part C: order-independence + determinism (§17) =="
# The shuffled descriptors keep the committed basenames so the emitted
# header comment (which names its descriptor) stays identical; only the
# [[trainers]] block ORDER changes.
mkdir -p "$tmp/shuffle"
python3 - "$descriptor" "$tmp/shuffle/trainer_front_family.toml" <<'PY'
import random, sys
src, out = sys.argv[1], sys.argv[2]
text = open(src).read()
parts = text.split('[[trainers]]')
header, trainers = parts[0], ['[[trainers]]' + p for p in parts[1:]]
random.Random(0).shuffle(trainers)
open(out, 'w').write(header + ''.join(trainers))
PY
python3 - "$back_descriptor" "$tmp/shuffle/trainer_back_family.toml" <<'PY'
import random, sys
src, out = sys.argv[1], sys.argv[2]
text = open(src).read()
parts = text.split('[[trainers]]')
header, trainers = parts[0], ['[[trainers]]' + p for p in parts[1:]]
random.Random(1).shuffle(trainers)
open(out, 'w').write(header + ''.join(trainers))
PY
mkdir -p "$tmp/genc"
gen4 "$tmp/shuffle/trainer_front_family.toml" "$tmp/shuffle/trainer_back_family.toml" "$tmp/genc" >/dev/null 2>&1
compare_outputs "$tmp/genc" "Part C shuffled"

mkdir -p "$tmp/genc2"
gen4 "$descriptor" "$back_descriptor" "$tmp/genc2" >/dev/null 2>&1
if diff -rq "$tmp/genb" "$tmp/genc2" >/dev/null 2>&1; then
    pass "Part C: two independent runs are byte-identical"
else
    fail "Part C: regeneration is not deterministic"
fi

echo "== Part D: failure matrix (§18) =="
# The dup-slug cases need a resolvable (but unique) artifact path. Copy a real
# sheet/palette under a throwaway slug so BuildResources succeeds and the
# intended validation failure fires; removed on exit.
cp "$root/graphics/trainers/front_pics/hiker.4bpp.lz" \
   "$root/graphics/trainers/front_pics/zzzslug.4bpp.lz"
cp "$root/graphics/trainers/front_pics/hiker.gbapal.lz" \
   "$root/graphics/trainers/front_pics/zzzslug.gbapal.lz"

expect_fail() { # expect_fail <label> <mode> <diagnostic>
    local label="$1" mode="$2" diag="$3"
    local log="$tmp/fail_${2}.log"
    mkdir -p "$tmp/failout"
    rm -f "$tmp/failout/"*.toml
    python3 - "$descriptor" "$mode" "$tmp/fail_$mode.toml" <<'PY'
import re, sys
src, mode, out = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(src).read()
parts = text.split('[[trainers]]')
header, blocks = parts[0], parts[1:]

def fields(b):
    d = {}
    for m in re.finditer(r'^\s*([A-Za-z0-9_]+)\s*=\s*("([^"]*)"|(-?\d+))\s*$', b, re.M):
        d[m.group(1)] = m.group(3) if m.group(2).startswith('"') else int(m.group(4))
    return d

def dup(overrides):
    b = blocks[-1]
    for key, val in overrides.items():
        b = re.sub(r'^%s = .*$' % key, '%s = %s' % (key, val), b, flags=re.M)
    return b

first = fields(blocks[0])
# The dup trainer copies the LAST block (rs-may, palette_dir=palettes), whose
# artifacts must resolve so validation (not artifact loading) fires. Force
# palette_dir to front_pics and give the throwaway slug a resolvable artifact
# (created by the runner) for the sheet + palette lookups.
if mode == 'dup-canonical':
    blocks.append(dup({'slug': '"zzzslug"', 'symbol': '"zzz"', 'pic': '"ZZZ"',
                       'index': '999', 'palette_dir': '"front_pics"'}))
elif mode == 'dup-index':
    blocks.append(dup({'canonical': '"zzz-dup"', 'slug': '"zzzslug"',
                       'symbol': '"zzz"', 'pic': '"ZZZ"',
                       'index': str(first['index']),
                       'palette_dir': '"front_pics"'}))
elif mode == 'dup-symbol':
    blocks.append(dup({'canonical': '"zzz-dup"', 'slug': '"zzzslug"',
                       'symbol': '"%s"' % first['symbol'], 'pic': '"ZZZ"',
                       'index': '999', 'palette_dir': '"front_pics"'}))
elif mode == 'dup-slug':
    blocks.append(dup({'canonical': '"zzz-dup"', 'symbol': '"zzz"',
                       'pic': '"ZZZ"', 'index': '999',
                       'slug': '"%s"' % first['slug'],
                       'palette_dir': '"front_pics"'}))
elif mode == 'bad-sheet-size':
    text = re.sub(r'^sheet_decoded_size = \d+', 'sheet_decoded_size = 2049', text, flags=re.M)
elif mode == 'neg-size':
    text = re.sub(r'^sheet_decoded_size = \d+', 'sheet_decoded_size = -5', text, flags=re.M)
elif mode == 'bad-back-index':
    blocks[-1] = re.sub(r'^back_palette_index = \d+', 'back_palette_index = 8',
                        blocks[-1], flags=re.M)
elif mode == 'missing-artifact':
    blocks[0] = re.sub(r'^slug = ".*"', 'slug = "no-such-file"', blocks[0], flags=re.M)
elif mode == 'bad-encoding':
    # "raw" is a supported encoding since R8; use a genuinely unknown one.
    text = re.sub(r'^sheet_encoding = ".*"', 'sheet_encoding = "bogus"', text, flags=re.M)
elif mode == 'missing-field':
    blocks[-1] = re.sub(r'^index = -?\d+\s*$', '', blocks[-1], flags=re.M)
else:
    sys.exit(2)

out_text = text if mode in ('bad-sheet-size', 'neg-size', 'bad-encoding') \
                 else header + ''.join('[[trainers]]' + b for b in blocks)
open(out, 'w').write(out_text)
PY
    cd "$root"
    if "$gen_dir/gen-trainer-family" --descriptor "$tmp/fail_$mode.toml" \
            --catalog-out "$tmp/failout/catalog.toml" \
            --bindings-out "$tmp/failout/bindings.generated.toml" \
            --ownership-out "$tmp/failout/ownership.generated.toml" \
            --consumers-out "$tmp/failout/trainer_front_consumers.generated.toml" \
            >"$log" 2>&1; then
        fail "Part D: $label (generator unexpectedly succeeded)"
    elif grep -qF "$diag" "$log"; then
        pass "Part D: $label"
    else
        fail "Part D: $label (wrong diagnostic: $(grep -m1 -oE '^[^:]+: .*' "$log" 2>/dev/null | head -1))"
    fi
}

expect_fail "duplicate canonical id"                    dup-canonical "duplicate canonical id"
expect_fail "duplicate table index"                     dup-index     "duplicate table index"
expect_fail "duplicate GBA symbol"                      dup-symbol    "duplicate GBA symbol suffix"
expect_fail "duplicate source artifact (not shared)"    dup-slug      "duplicate source artifact"
expect_fail "conflicting decoded sizes"                 bad-sheet-size "decoded size conflict"
expect_fail "invalid expected decoded size"             neg-size      "invalid expected decoded size"
expect_fail "unknown trainer table ref"                 bad-back-index "unknown trainer table ref"
expect_fail "source artifact missing"                   missing-artifact "source artifact missing"
expect_fail "unsupported source encoding"               bad-encoding  "unsupported sheet source encoding"
expect_fail "missing required descriptor field"         missing-field "family descriptor missing"

# R8 back-family-specific validation: frames > 0 must be paired with a
# frame_array (and vice versa). The mutation is on the BACK descriptor; the
# front descriptor stays untouched. Parse fails before any artifact
# resolution, so no fixture files are needed.
python3 - "$back_descriptor" "$tmp/fail_back_frame.toml" <<'PY'
import re, sys
src, out = sys.argv[1], sys.argv[2]
text = open(src).read()
parts = text.split('[[trainers]]')
blocks = ['[[trainers]]' + p for p in parts[1:]]
# Last block is steven (frames = 4); drop its frame_array.
blocks[-1] = re.sub(r'^frame_array = .*$', '', blocks[-1], flags=re.M)
open(out, 'w').write(parts[0] + ''.join(blocks))
PY
if "$gen_dir/gen-trainer-family" --descriptor "$descriptor" \
        --back-descriptor "$tmp/fail_back_frame.toml" \
        --catalog-out "$tmp/failout/cat.toml" \
        --bindings-out "$tmp/failout/b.toml" \
        --ownership-out "$tmp/failout/o.toml" \
        --consumers-out "$tmp/failout/cn.toml" \
        --back-bindings-out "$tmp/failout/bb.toml" \
        --back-ownership-out "$tmp/failout/bo.toml" \
        --back-consumers-out "$tmp/failout/bc.toml" \
        >"$tmp/fail_back_frame.log" 2>&1; then
    fail "Part D: back frames/frame_array mismatch (generator unexpectedly succeeded)"
elif grep -qF "frames > 0 but no frame_array given" "$tmp/fail_back_frame.log"; then
    pass "Part D: back frames/frame_array mismatch"
else
    fail "Part D: back frames/frame_array mismatch (wrong diagnostic)"
fi

printf 'this is not toml [[[[\n' >"$tmp/fail_malformed.toml"
if "$gen_dir/gen-trainer-family" --descriptor "$tmp/fail_malformed.toml" \
        --catalog-out "$tmp/failout/c.toml" --bindings-out "$tmp/failout/b.toml" \
        --ownership-out "$tmp/failout/o.toml" --consumers-out "$tmp/failout/cn.toml" \
        >"$tmp/fail_malformed.log" 2>&1; then
    fail "Part D: malformed descriptor (generator unexpectedly succeeded)"
elif grep -qF "malformed family descriptor" "$tmp/fail_malformed.log"; then
    pass "Part D: malformed descriptor"
else
    fail "Part D: malformed descriptor (wrong diagnostic)"
fi

rm -f "$root/graphics/trainers/front_pics/zzzslug.4bpp.lz" \
      "$root/graphics/trainers/front_pics/zzzslug.gbapal.lz"

echo "== Part E: completeness + cross-reference (§19) =="
if python3 "$here/trainer_family_completeness.py" "$root" >"$tmp/completeness.log" 2>&1; then
    cat "$tmp/completeness.log"
    pass "Part E: completeness cross-check"
else
    cat "$tmp/completeness.log" >&2
    fail "Part E: completeness cross-check"
fi

echo "== Part F: R1A scale test (§21) =="
python3 - "$descriptor" "$bpee01/bindings.generated.toml" \
        "$tmp/multi.list" "$tmp/multi_bindings.toml" <<'PY'
import re, sys
descriptor, bindings_path, list_out, bindings_out = sys.argv[1:5]
pick = {"brendan", "may", "wally", "aqua-admin-f", "hiker", "champion-wallace"}

text = open(descriptor).read()
trainers = []
cur = None
for line in text.splitlines():
    line = line.strip()
    if line == "[[trainers]]":
        if cur is not None:
            trainers.append(cur)
        cur = {}
    elif cur is not None and not line.startswith("#"):
        m = re.match(r'^([A-Za-z0-9_]+)\s*=\s*("([^"]*)"|(-?\d+))$', line)
        if m:
            cur[m.group(1)] = m.group(3) if m.group(2).startswith('"') else int(m.group(4))
if cur is not None:
    trainers.append(cur)

chosen = [t for t in trainers if t["canonical"] in pick]
assert len(chosen) == 6, "representative set not found"
with open(list_out, "w") as fh:
    for t in sorted(chosen, key=lambda t: t["canonical"]):
        fh.write("F gTrainerFrontPic_%s graphics/trainers/front_pics/%s.4bpp.lz\n"
                 % (t["symbol"], t["slug"]))
        if t["palette_dir"] == "palettes":
            fh.write("P gTrainerPalette_%s graphics/trainers/palettes/%s.gbapal.lz\n"
                     % (t["symbol"], t["slug"]))
        else:
            fh.write("P gTrainerPalette_%s graphics/trainers/front_pics/%s.gbapal.lz\n"
                     % (t["symbol"], t["slug"]))

want = set()
for t in chosen:
    want.add("emerald:trainer/%s/battle/front/sheet" % t["canonical"])
    want.add("emerald:trainer/%s/battle/front/normal-palette" % t["canonical"])
keep = []
for block in open(bindings_path).read().split("[[bindings]]")[1:]:
    m = re.search(r'^id = "([^"]+)"', block, re.M)
    if m and m.group(1) in want:
        keep.append("[[bindings]]" + block)
with open(bindings_out, "w") as fh:
    fh.write("".join(keep))
PY
cd "$root"
"$elf_dir/fixture_build" --elf "$tmp/multi.elf" --rom "$tmp/multi.gba" \
    --multi "$tmp/multi.list" >"$tmp/multi.sha"
multi_sha1="$(awk '/^rom_sha1/{print $2}' "$tmp/multi.sha")"
if ! "$elf_dir/gen3-elf-manifest" \
        --catalog "$catalog" \
        --bindings "$tmp/multi_bindings.toml" \
        --elf "$tmp/multi.elf" \
        --rom "$tmp/multi.gba" \
        --rom-sha1 "$multi_sha1" \
        --qualification fixture \
        --provenance "R7A §21 scale fixture: 6 representative trainers" \
        --output "$tmp/multi_manifest.toml" >"$tmp/f.scale.log" 2>&1; then
    fail "Part F: gen3-elf-manifest over the multi fixture (see $tmp/f.scale.log)"
else
    pass "Part F: gen3-elf-manifest generates a manifest over 12 resources"
fi
python3 - "$tmp/multi.list" "$tmp/multi_manifest.toml" <<'PY'
import hashlib, re, sys
list_path, manifest_path = sys.argv[1], sys.argv[2]
slot = {}
fi = pi = 0
for line in open(list_path):
    kind, sym, path = line.split()
    if kind == "F":
        slot[sym] = 0x00300000 + fi * 0x1000
        fi += 1
    else:
        slot[sym] = 0x00310000 + pi * 0x100
        pi += 1

records = []
for block in open(manifest_path).read().split("[[records]]")[1:]:
    d = {}
    for m in re.finditer(r'^([A-Za-z0-9_]+)\s*=\s*("([^"]*)"|(\d+))\s*$', block, re.M):
        d[m.group(1)] = m.group(3) if m.group(2).startswith('"') else int(m.group(4))
    if "id" in d:
        records.append(d)

assert len(records) == 12, "expected 12 manifest records, got %d" % len(records)
ids = [r["id"] for r in records]
assert ids == sorted(ids), "manifest records not bytewise sorted"
ok = 0
for r in records:
    assert r["rom_offset"] == slot[r["symbol"]], \
        "%s offset %d != slot %d" % (r["id"], r["rom_offset"], slot[r["symbol"]])
    if r["id"].endswith("/sheet"):
        assert r["decoded_length"] == 2048
        assert r["type"] == "tile-graphics"
    else:
        assert r["decoded_length"] == 32
        assert r["type"] == "palette"
    key = hashlib.sha256(b"gen3-resource-id-v1\x00" + r["id"].encode()).hexdigest()
    assert r["key"] == key, "key mismatch for %s" % r["id"]
    ok += 1
print("  PASS Part F: 12 records, sorted, slot offsets, sizes, M0/M1 keys (%d/12 verified)" % ok)
PY
if [ $? -eq 0 ]; then
    pass "Part F: manifest contents verified"
else
    fail "Part F: manifest contents"
fi

echo "== Part G: R2 full-family pack (§22) =="
core_dir="$root/src/gen3/resources"
gcc -std=c99 -Wall -Wextra -Werror \
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
    -o "$tmp/trainer_family_pack_unit"
if "$tmp/trainer_family_pack_unit" >"$tmp/pack.log" 2>&1; then
    cat "$tmp/pack.log"
    if grep -q "0 failures" "$tmp/pack.log"; then
        pass "Part G: full-family pack builds deterministically (196 entries)"
    else
        fail "Part G: full-family pack checks failed (see $tmp/pack.log)"
    fi
else
    cat "$tmp/pack.log" >&2
    fail "Part G: full-family pack unit"
fi

echo ""
echo "== $passes passes, $failures failures =="
[ "$failures" -eq 0 ]

#!/usr/bin/env bash
#
# R9 Stage 2 Pokémon battle-graphics family test runner.
#
#   A  generator --check: the five committed outputs regenerate byte-for-byte;
#      the inventory itself regenerates with a no-op diff.
#   B  regeneration to a temp dir is byte-identical to the committed files.
#   C  order-independence: a shuffled [[resources]]/[[slots]] inventory (the
#      only free orderings in the input) produces identical output; two
#      independent runs are byte-identical.
#   D  failure matrix: malformed inventories fail closed with the expected
#      structured diagnostics.
#   E  completeness + cross-reference: the five generated files against the C
#      tree (species.h, table headers, INCBIN artifacts) via
#      pokemon_family_completeness.py, including an independent strict LZ77
#      decode of all 1608 payloads and M0/M1 key verification.
#   F  R1A production-extraction pipeline (§20): a representative set of 10
#      battle resources (ordinary sheets, Castform's nonstandard 8192/128
#      payloads, the unown/a form slot, the shared unown shiny palette, the
#      upstream MrMime spelling, a multi-level slug) runs through
#      fixture_build + gen3-elf-manifest; the manifest records are verified
#      against the fixture slot layout.
#
# The production manifest (manifest.production.toml, 1608 records) is
# generated from the qualified pret reference build ELF + retail-matching ROM,
# which do not exist in this environment; the fixture in Part F exercises the
# exact same pipeline deterministically.
#
# Build-isolated like the other Gen3 runners: -std=c99 -Wall -Wextra -Werror
# plus the tool include roots and the shared Gen3 core. No Emerald defines, no
# global.h/gba/SDL. Tests never require a user ROM and never launch the game.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cd "$root"
gen_dir="$root/tools/gen3_resources/pokemon_family"
elf_dir="$root/tools/gen3_resources/elf_manifest"
battle="$root/resources/extraction/emerald/bpee01/pokemon_battle"
inventory="$battle/inventory.generated.toml"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

failures=0
passes=0
pass() { passes=$((passes+1)); echo "  PASS $1"; }
fail() { failures=$((failures+1)); echo "  FAIL $1" >&2; }

gen5() { # gen5 <inventory> <outdir>
    "$gen_dir/gen-pokemon-family" --inventory "$1" \
        --catalog-out "$2/catalog.generated.toml" \
        --bindings-out "$2/bindings.generated.toml" \
        --ownership-out "$2/ownership.generated.toml" \
        --consumers-out "$2/consumers.generated.toml" \
        --species-map-out "$2/species_mapping.generated.toml"
}

compare_outputs() { # compare_outputs <outdir> <label>
    local dir="$1" label="$2" f
    for f in catalog.generated.toml bindings.generated.toml \
             ownership.generated.toml consumers.generated.toml \
             species_mapping.generated.toml; do
        if cmp -s "$battle/$f" "$dir/$f"; then
            pass "$label: $f byte-identical"
        else
            fail "$label: $f differs from committed"
        fi
    done
}

echo "== building the family generator =="
make -C "$gen_dir" all
pass "gen-pokemon-family builds"

echo "== Part A: --check on the committed outputs =="
if "$gen_dir/gen-pokemon-family" --inventory "$inventory" \
        --catalog-out "$battle/catalog.generated.toml" \
        --bindings-out "$battle/bindings.generated.toml" \
        --ownership-out "$battle/ownership.generated.toml" \
        --consumers-out "$battle/consumers.generated.toml" \
        --species-map-out "$battle/species_mapping.generated.toml" \
        --check >"$tmp/check.log" 2>&1; then
    pass "generator --check passes on all five committed outputs"
    if grep -q "1608 payload resources" "$tmp/check.log"; then
        pass "--check reports 1608 payload resources"
    else
        fail "--check count message missing (see $tmp/check.log)"
    fi
else
    fail "generator --check failed"
fi
python3 "$gen_dir/gen_pokemon_inventory.py" "$root" "$tmp/inv_regen.toml" >/dev/null 2>&1
if cmp -s "$tmp/inv_regen.toml" "$inventory"; then
    pass "inventory regeneration is a no-op diff"
else
    fail "inventory regeneration differs from committed"
fi

echo "== Part B: regeneration to temp is byte-identical =="
mkdir -p "$tmp/genb"
gen5 "$inventory" "$tmp/genb" >/dev/null 2>&1
compare_outputs "$tmp/genb" "Part B"

echo "== Part C: order-independence (§17) =="
# Shuffle the [[resources]] and [[slots]] array orderings (the only free
# orderings in the input; per-resource slot ascending order is enforced by the
# generator itself and must NOT be shuffled).
mkdir -p "$tmp/shuffle"
python3 - "$inventory" "$tmp/shuffle/inventory.generated.toml" <<'PY'
import random, sys
src, out = sys.argv[1], sys.argv[2]
text = open(src).read()
chunks = text.split('\n[[')
header, blocks = chunks[0], ['[[' + c for c in chunks[1:]]
res = [b for b in blocks if b.startswith('[[resources]]')]
slots = [b for b in blocks if b.startswith('[[slots]]')]
other = [b for b in blocks if not b.startswith('[[resources]]') and not b.startswith('[[slots]]')]
# Each resource is emitted together with its slot rows, in the same relative
# order (per-resource slot ascending order is a generator invariant and must
# be preserved); only the (resource, slots) unit ORDER is shuffled.
# Pairing key is (kind, symbol): gMonPalette_Egg exists in BOTH palette
# families (normal slot 412 + shiny alias slot 412).
def key_of(b):
    re_ = __import__('re')
    kind = next((m.group(1) for m in re_.finditer(r'^kind = "([^"]+)"', b, re_.M)), '')
    sym = next((m.group(1) for m in re_.finditer(r'^symbol = "([^"]+)"', b, re_.M)), '')
    return (kind, sym)
pairs = [(b, [s for s in slots if key_of(s) == key_of(b)]) for b in res]
random.Random(0).shuffle(pairs)
out_blocks = other
for b, ss in pairs:
    out_blocks.append(b); out_blocks.extend(ss)
# header must be IN the join: '\n'.join of a single-element list emits no
# separator, which would weld the header to the first block.
open(out, 'w').write('\n'.join([header] + out_blocks))
PY
mkdir -p "$tmp/genc"
gen5 "$tmp/shuffle/inventory.generated.toml" "$tmp/genc" >/dev/null 2>&1
compare_outputs "$tmp/genc" "Part C shuffled"

mkdir -p "$tmp/genc2"
gen5 "$inventory" "$tmp/genc2" >/dev/null 2>&1
if diff -rq "$tmp/genb" "$tmp/genc2" >/dev/null 2>&1; then
    pass "Part C: two independent runs are byte-identical"
else
    fail "Part C: regeneration is not deterministic"
fi

echo "== Part D: failure matrix (§18) =="
expect_fail() { # expect_fail <label> <mode> <diagnostic>
    local label="$1" mode="$2" diag="$3"
    local log="$tmp/fail_$mode.log"
    mkdir -p "$tmp/failout"
    rm -f "$tmp/failout/"*.toml
    python3 - "$inventory" "$mode" "$tmp/fail_$mode.toml" <<'PY'
import re, sys
src, mode, out = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(src).read()
chunks = text.split('\n[[')
header, blocks = chunks[0], ['[[' + c for c in chunks[1:]]
# Mutations happen IN PLACE on `blocks` (strings are immutable: rebinding a
# filtered view would silently drop the change from the written file).
res_idx = [i for i, b in enumerate(blocks) if b.startswith('[[resources]]')]
slot_idx = [i for i, b in enumerate(blocks) if b.startswith('[[slots]]')]

def fields(body):
    d = {}
    for m in re.finditer(r'^([A-Za-z0-9_]+)\s*=\s*("([^"]*)"|(-?\d+))\s*$', body, re.M):
        d[m.group(1)] = m.group(3) if m.group(2).startswith('"') else int(m.group(4))
    return d

def sub(body, key, val):
    return re.sub(r'^%s = .*$' % key, '%s = %s' % (key, val), body, flags=re.M)

def sort_blocks(bs):
    return sorted(bs, key=lambda b: (fields(b).get('symbol', ''), fields(b).get('index', -1)))

if mode == 'dup-canonical':
    # Point the first resource's artifact at the second's: identical slug ->
    # identical canonical id. Slots stay untouched, so the join and alias
    # passes still resolve and ValidateFamily's duplicate-id check fires.
    ordered = sort_blocks([blocks[i] for i in res_idx])
    s = fields(ordered[1])
    blocks[res_idx[0]] = sub(blocks[res_idx[0]], 'source_artifact',
                             '"%s"' % s['source_artifact'])
elif mode == 'dup-index':
    # Give the first slot row the second's index: the second resource's index
    # is then covered twice in its family -> duplicate slot index.
    ordered = sort_blocks([blocks[i] for i in slot_idx])
    a, b = ordered[0], ordered[1]
    blocks[blocks.index(a)] = sub(a, 'index', str(fields(b)['index']))
elif mode == 'non-ascending':
    # Swap two slots of ONE multi-slot resource (the 25-slot
    # DoubleQuestionMark group): the ascending-order determinism invariant
    # must fail. Group by symbol first -- every family has its own DQM
    # resource, so a global index sort would pair two index-252 slots from
    # different families and the swap would be a no-op.
    groups = {}
    for i in slot_idx:
        b = blocks[i]
        sym = fields(b).get('symbol', '')
        if sym.endswith('DoubleQuestionMark'):
            groups.setdefault(sym, []).append(b)
    sym = max(groups, key=lambda s: len(groups[s]))
    dqm = sorted(groups[sym], key=lambda s: fields(s)['index'])
    if len(dqm) < 2:
        sys.exit(3)
    blocks[blocks.index(dqm[0])] = sub(dqm[0], 'index', str(fields(dqm[1])['index']))
    blocks[blocks.index(dqm[1])] = sub(dqm[1], 'index', str(fields(dqm[0])['index']))
elif mode == 'unknown-symbol':
    blocks[slot_idx[0]] = sub(blocks[slot_idx[0]], 'symbol', '"gMonFrontPic_NoSuchSymbol"')
elif mode == 'join-mismatch':
    blocks[res_idx[0]] = sub(blocks[res_idx[0]], 'slot_count', '7')
elif mode == 'missing-artifact':
    blocks[res_idx[0]] = sub(blocks[res_idx[0]], 'source_artifact',
                             '"graphics/pokemon/no_such_dir/anim_front.4bpp.lz"')
elif mode == 'external-not-allowlisted':
    # A NEW external alias (prefix gMonStillFrontPic_, not allowlisted) with
    # a joinable slot; ResolveAliases fires before any decode.
    blocks.append('[[resources]]\nsymbol = "gMonStillFrontPic_NotAllowlisted"\nkind = "front_sheet"\nsource_artifact = "graphics/pokemon/bulbasaur/anim_front.4bpp.lz"\nslot_count = 1')
    blocks.append('[[slots]]\nsymbol = "gMonStillFrontPic_NotAllowlisted"\nkind = "front_sheet"\nindex = 0\nspecies = "NONE"')
else:
    sys.exit(2)

# header must be IN the join (single-element join emits no separator).
open(out, 'w').write('\n'.join([header] + blocks))
PY
    if "$gen_dir/gen-pokemon-family" --inventory "$tmp/fail_$mode.toml" \
            --catalog-out "$tmp/failout/catalog.generated.toml" \
            --bindings-out "$tmp/failout/bindings.generated.toml" \
            --ownership-out "$tmp/failout/ownership.generated.toml" \
            --consumers-out "$tmp/failout/consumers.generated.toml" \
            --species-map-out "$tmp/failout/species_mapping.generated.toml" \
            >"$log" 2>&1; then
        fail "Part D: $label (generator unexpectedly succeeded)"
    elif grep -qF "$diag" "$log"; then
        pass "Part D: $label"
    else
        fail "Part D: $label (wrong diagnostic: $(grep -m1 -oE '^[^:]+: .*' "$log" 2>/dev/null | head -1))"
    fi
}

expect_fail "duplicate canonical id"         dup-canonical         "duplicate canonical id"
expect_fail "duplicate table index"          dup-index             "duplicate slot index"
expect_fail "slots not strictly ascending"   non-ascending         "slots not strictly index-ascending"
expect_fail "unknown resource symbol"        unknown-symbol        "slot references unknown resource symbol"
expect_fail "slot join mismatch"             join-mismatch         "slot join mismatch"
expect_fail "source artifact missing"        missing-artifact      "source artifact missing"
expect_fail "external alias not allowlisted" external-not-allowlisted "not in the allowlist"

printf 'this is not toml [[[[\n' >"$tmp/fail_malformed.toml"
if "$gen_dir/gen-pokemon-family" --inventory "$tmp/fail_malformed.toml" \
        --catalog-out "$tmp/failout/c.toml" --bindings-out "$tmp/failout/b.toml" \
        --ownership-out "$tmp/failout/o.toml" --consumers-out "$tmp/failout/cn.toml" \
        --species-map-out "$tmp/failout/sm.toml" \
        >"$tmp/fail_malformed.log" 2>&1; then
    fail "Part D: malformed inventory (generator unexpectedly succeeded)"
elif grep -qF "malformed inventory" "$tmp/fail_malformed.log"; then
    pass "Part D: malformed inventory"
else
    fail "Part D: malformed inventory (wrong diagnostic)"
fi

echo "== Part E: completeness + cross-reference (§19) =="
if python3 "$here/pokemon_family_completeness.py" "$root" >"$tmp/completeness.log" 2>&1; then
    cat "$tmp/completeness.log"
    pass "Part E: completeness cross-check"
else
    cat "$tmp/completeness.log" >&2
    fail "Part E: completeness cross-check"
fi

echo "== Part F: R1A production-extraction pipeline (§20) =="
# Representative set covering every family shape: ordinary 2048/4096 sheets,
# Castform's nonstandard 8192 front + 128 palette, the unown/a form slot, the
# shared unown shiny palette, the upstream MrMime spelling, and a multi-level
# slug (question_mark/circled). All derived from the committed bindings, so
# nothing is hand-maintained.
make -C "$elf_dir" all >/dev/null 2>&1
mkdir -p "$tmp/genf"
python3 - "$battle/bindings.generated.toml" "$tmp/f.list" "$tmp/f_bindings.toml" <<'PY'
import re, sys
bindings_path, list_out, bindings_out = sys.argv[1], sys.argv[2], sys.argv[3]
pick = [
    "emerald:pokemon/bulbasaur/battle/front/sheet",
    "emerald:pokemon/bulbasaur/battle/back/sheet",
    "emerald:pokemon/bulbasaur/battle/normal-palette",
    "emerald:pokemon/bulbasaur/battle/shiny-palette",
    "emerald:pokemon/castform/battle/front/sheet",
    "emerald:pokemon/castform/battle/normal-palette",
    "emerald:pokemon/unown/a/battle/front/sheet",
    "emerald:pokemon/unown/battle/shiny-palette",
    "emerald:pokemon/mr_mime/battle/front/sheet",
    "emerald:pokemon/question_mark/circled/battle/back/sheet",
]
blocks = open(bindings_path).read().split("[[bindings]]")[1:]
kept = []
lines = []
for block in blocks:
    m = re.search(r'^id = "([^"]+)"', block, re.M)
    if m and m.group(1) in pick:
        kept.append("[[bindings]]" + block)
        sym = re.search(r'^symbol = "([^"]+)"', block, re.M).group(1)
        art = re.search(r'^source_artifact = "([^"]+)"', block, re.M).group(1)
        kind = "F" if "/sheet" in m.group(1) else "P"
        lines.append("%s %s %s" % (kind, sym, art))
assert len(kept) == len(pick), "not all representative ids found in bindings"
open(list_out, "w").write("\n".join(sorted(lines)) + "\n")
open(bindings_out, "w").write("\n".join(kept))
PY
"$elf_dir/fixture_build" --elf "$tmp/f.elf" --rom "$tmp/f.gba" \
    --multi "$tmp/f.list" >"$tmp/f.sha"
f_sha1="$(awk '/^rom_sha1/{print $2}' "$tmp/f.sha")"
if ! "$elf_dir/gen3-elf-manifest" \
        --catalog "$battle/catalog.generated.toml" \
        --bindings "$tmp/f_bindings.toml" \
        --elf "$tmp/f.elf" \
        --rom "$tmp/f.gba" \
        --rom-sha1 "$f_sha1" \
        --qualification fixture \
        --provenance "R9 §20 production-extraction fixture: 10 representative battle resources" \
        --output "$tmp/f_manifest.toml" >"$tmp/f.log" 2>&1; then
    fail "Part F: gen3-elf-manifest over the battle fixture (see $tmp/f.log)"
else
    pass "Part F: gen3-elf-manifest validates 10 representative resources"
fi
python3 - "$tmp/f.list" "$tmp/f_manifest.toml" <<'PY'
import hashlib, re, sys
list_path, manifest_path = sys.argv[1], sys.argv[2]
slot = {}
fi = pi = 0
for line in open(list_path):
    kind, sym, _ = line.split()
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
assert len(records) == 10, "expected 10 manifest records, got %d" % len(records)
ids = [r["id"] for r in records]
assert ids == sorted(ids), "manifest records not bytewise sorted"
ok = 0
for r in records:
    assert r["rom_offset"] == slot[r["symbol"]], \
        "%s offset %d != slot %d" % (r["id"], r["rom_offset"], slot[r["symbol"]])
    if r["id"].endswith("/sheet"):
        assert r["decoded_length"] % 2048 == 0 and r["type"] == "tile-graphics"
    else:
        assert r["decoded_length"] % 32 == 0 and r["type"] == "palette"
    key = hashlib.sha256(b"gen3-resource-id-v1\x00" + r["id"].encode()).hexdigest()
    assert r["key"] == key, "key mismatch for %s" % r["id"]
    ok += 1
print("  PASS Part F: 10 records, sorted, slot offsets, sizes, M0/M1 keys (%d/10)" % ok)
PY
if [ $? -eq 0 ]; then
    pass "Part F: manifest contents verified"
else
    fail "Part F: manifest contents"
fi

echo ""
echo "== $passes passes, $failures failures =="
[ "$failures" -eq 0 ]

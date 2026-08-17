#!/usr/bin/env bash
#
# R12-A audio-family inventory test runner.
#
#   A  generator --check: both committed outputs (inventory.generated.toml,
#      canonical_sample_pins.generated.txt) are up to date; a fresh
#      regeneration is a no-op diff.
#   B  regeneration to a temp dir is byte-identical to the committed files.
#   C  determinism + structure: two independent runs are byte-identical; the
#      inventory parses as TOML with the pinned family counts
#      (530/105/51/388/25/195/2/5 = 1301), bytewise-sorted unique keys, the
#      R11-B canonical naming (no underscore keys), no dummy_song_header /
#      gSongTable / gMPlayTable inside [[resources]], the excluded alias
#      block pinned at count 80, and every resource's type/schema/
#      representation matching the approved R12 §2 contract per kind.
#   D  failure matrix: mutated source tables fail closed with the expected
#      structured messages (malformed row, duplicate symbol, missing family
#      source, count drift, duplicate artifact, unresolved cry reference,
#      missing voice_group macro, song track-count mismatch, .aif/.bin
#      bijection break).
#   E  provenance: the 544 tracked .aif contents at HEAD are byte-identical
#      as a SET to the pre-fork canonical blobs at ee1173eb9^ (the fork's
#      expansions/renames were restored to canonical content in a620dddbc),
#      both sides exactly 544 entries; the committed pins file covers the
#      544 working-tree .aif one-to-one.
#
# Build-isolated like the other Gen3 runners: pure Python, no Emerald defines,
# no SDL, no user ROM, never launches the game.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cd "$root"
gen="$root/tools/gen3_resources/audio_family/gen_audio_inventory.py"
audio="$root/resources/extraction/emerald/bpee01/audio"
inventory="$audio/inventory.generated.toml"
pins="$audio/canonical_sample_pins.generated.txt"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

failures=0
passes=0
pass() { passes=$((passes+1)); echo "  PASS $1"; }
fail() { failures=$((failures+1)); echo "  FAIL $1" >&2; }

# Rebuild a minimal mutation root: the 6 table files are copies (mutable),
# everything else is symlinked to the repo (cheap; the generator reads
# song files, voicegroups and samples read-only). With samples-copy set, the
# samples tree is copied instead so a file can be removed for the bijection
# case.
setup_root() {
    local root_dir="$1" copy_samples="$2"
    rm -rf "$root_dir"
    mkdir -p "$root_dir/sound"
    for f in song_table direct_sound_data programmable_wave_data \
             voice_groups cry_tables keysplit_tables; do
        cp "$root/sound/$f.inc" "$root_dir/sound/"
    done
    ln -s "$root/sound/songs" "$root_dir/sound/songs"
    ln -s "$root/sound/voicegroups" "$root_dir/sound/voicegroups"
    if [ "$copy_samples" = "copy" ]; then
        cp -a "$root/sound/direct_sound_samples" "$root_dir/sound/direct_sound_samples"
    else
        ln -s "$root/sound/direct_sound_samples" "$root_dir/sound/direct_sound_samples"
    fi
}

expect_fail() { # expect_fail <label> <diagnostic> <setup> <mutate>
    local label="$1" diag="$2" setup="$3" mutate="$4"
    local log="$tmp/fail_$label.log"
    local root_dir="$tmp/root_$label"
    setup_root "$root_dir" "$setup"
    eval "$mutate"
    if python3 "$gen" "$root_dir" "$tmp/failout_$label" >"$log" 2>&1; then
        fail "$label: generator unexpectedly succeeded"
    elif grep -q "$diag" "$log"; then
        pass "$label: refused with: $diag"
    else
        fail "$label: refused but wrong message (see $log)"
    fi
}

echo "== Part A: --check on the committed outputs =="
if python3 "$gen" "$root" "$audio" --check >"$tmp/check.log" 2>&1; then
    pass "generator --check passes on both committed outputs"
    if grep -q "1301 resources, 544 sample pins" "$tmp/check.log"; then
        pass "--check reports 1301 resources, 544 sample pins"
    else
        fail "--check count message missing (see $tmp/check.log)"
    fi
else
    fail "generator --check failed"
fi
mkdir -p "$tmp/regen"
python3 "$gen" "$root" "$tmp/regen" >/dev/null 2>&1
if cmp -s "$tmp/regen/inventory.generated.toml" "$inventory" \
   && cmp -s "$tmp/regen/canonical_sample_pins.generated.txt" "$pins"; then
    pass "regeneration is a no-op diff"
else
    fail "regeneration differs from committed outputs"
fi

echo "== Part B: regeneration to temp is byte-identical =="
mkdir -p "$tmp/genb"
python3 "$gen" "$root" "$tmp/genb" >/dev/null 2>&1
for f in inventory.generated.toml canonical_sample_pins.generated.txt; do
    if cmp -s "$audio/$f" "$tmp/genb/$f"; then
        pass "Part B: $f byte-identical"
    else
        fail "Part B: $f differs from committed"
    fi
done

echo "== Part C: determinism + structure =="
mkdir -p "$tmp/genc"
python3 "$gen" "$root" "$tmp/genc" >/dev/null 2>&1
if diff -rq "$tmp/genb" "$tmp/genc" >/dev/null 2>&1; then
    pass "Part C: two independent runs are byte-identical"
else
    fail "Part C: regeneration is not deterministic"
fi
python3 - "$inventory" <<'PY'
import re, sys, tomllib
path = sys.argv[1]
doc = tomllib.load(open(path, "rb"))
print(f"    toml parses; {len(doc['resources'])} resources, {len(doc['families'])} families")

def check(cond, label):
    if not cond:
        sys.exit(f"FAIL: {label}")

PINNED = {"song": 530, "sample/root": 105, "sample/phoneme": 51,
          "sample/cry": 388, "programmable-wave": 25, "voicegroup": 195,
          "cry-table": 2, "keysplit": 5}
CONTRACT = {
    "song":              ("music-sequence", 1, "gba-mp2k-song-graph"),
    "sample/root":       ("audio-sample", 1, "gba-wave-data"),
    "sample/phoneme":    ("audio-sample", 1, "gba-wave-data"),
    "sample/cry":        ("audio-sample", 1, "gba-wave-data"),
    "programmable-wave": ("audio-sample", 1, "gba-cgb-wave"),
    "voicegroup":        ("instrument-bank", 1, "gba-tone-data-12"),
    "cry-table":         ("instrument-bank", 1, "gba-tone-data-12"),
    "keysplit":          ("instrument-bank", 2, "gba-keysplit-run"),
}
fam = {f["kind"]: f["symbol_count"] for f in doc["families"]}
check(fam == PINNED, f"family symbol_count mismatch: {fam} != {PINNED}")

counts = {}
for r in doc["resources"]:
    counts[r["kind"]] = counts.get(r["kind"], 0) + 1
check(counts == PINNED, f"resource kind counts mismatch: {counts} != {PINNED}")
check(len(doc["resources"]) == 1301, "total resource count != 1301")

keys = [r["key"] for r in doc["resources"]]
check(keys == sorted(keys), "keys are not bytewise sorted")
check(len(keys) == len(set(keys)), "duplicate keys")
check(not any("_" in k for k in keys), "underscore in a canonical key (R11-B naming violated)")

PREFIX = {"song": "emerald:audio/song/", "sample/root": "emerald:audio/sample/",
          "sample/phoneme": "emerald:audio/sample/phoneme/",
          "sample/cry": "emerald:audio/sample/cry/",
          "programmable-wave": "emerald:audio/wave/programmable/",
          "voicegroup": "emerald:audio/voicegroup/",
          "cry-table": "emerald:audio/cry-table/",
          "keysplit": "emerald:audio/keysplit/"}
for r in doc["resources"]:
    check(r["key"].startswith(PREFIX[r["kind"]]),
          f"{r['key']}: key prefix wrong for kind {r['kind']}")
    t, s, rep = CONTRACT[r["kind"]]
    check(r["resource_type"] == t and r["schema"] == s
          and r["representation"] == rep,
          f"{r['key']}: contract mismatch "
          f"({r['resource_type']}/{r['schema']}/{r['representation']})")
    check("symbol" in r, f"{r['key']}: missing symbol")
    check("source_artifact" in r, f"{r['key']}: missing source_artifact")

for r in doc["resources"]:
    if r["kind"] == "song":
        check(r.get("track_count", 0) >= 0, f"{r['key']}: missing track_count")
        check("voicegroup" in r, f"{r['key']}: missing voicegroup")
    if r["kind"] == "cry-table":
        check(r.get("row_count") == 388, f"{r['key']}: row_count != 388")
    if r["kind"] == "keysplit":
        check("offset" in r, f"{r['key']}: missing offset")

check(doc["excluded"] == [{"symbol": "dummy_song_header", "count": 80,
       "reason": "80 gSongTable rows alias the one compiled dummy placeholder header"}],
      "excluded block != the single 80-row dummy_song_header entry")
check(not any(r["symbol"] == "dummy_song_header" for r in doc["resources"]),
      "dummy_song_header inside [[resources]]")
check(not any(r["symbol"] in ("gSongTable", "gMPlayTable") for r in doc["resources"]),
      "MP2K routing table inside [[resources]]")
print("    Part C structural assertions all passed")
PY

echo "== Part D: failure matrix =="
expect_fail "malformed-row" "malformed song row" symlink \
    'printf "\tbuggy_row_here\n" >> "$root_dir/sound/song_table.inc"'
expect_fail "dup-song-symbol" "duplicate song symbol" symlink \
    'sed -i "0,/song se_pc_login, 1, 1/s//song se_use_item, 1, 1/" "$root_dir/sound/song_table.inc"'
expect_fail "missing-family" "missing family source" symlink \
    'rm "$root_dir/sound/keysplit_tables.inc"'
expect_fail "count-drift" "has 387 rows, expected 388" symlink \
    'sed -i "/^[[:space:]]*cry Cry_Bulbasaur$/d" "$root_dir/sound/cry_tables.inc"'
expect_fail "dup-artifact" "duplicate artifact" symlink \
    'sed -i "s|sound/direct_sound_samples/sc88pro_organ2.bin|sound/direct_sound_samples/sc88pro_glockenspiel.bin|" "$root_dir/sound/direct_sound_data.inc"'
expect_fail "unresolved-cry" "undefined sample" symlink \
    'sed -i "0,/cry Cry_Bulbasaur/s//cry Cry_DefinitelyNotASample/" "$root_dir/sound/cry_tables.inc"'
expect_fail "voicegroup-macro" "expected exactly one" symlink \
    'rm "$root_dir/sound/voicegroups" && cp -a "$root/sound/voicegroups" "$root_dir/sound/voicegroups" && printf "@ no macro here\n" > "$root_dir/sound/voicegroups/dummy.inc"'
expect_fail "track-count" "declares 7 tracks" symlink \
    'rm "$root_dir/sound/songs" && cp -a "$root/sound/songs" "$root_dir/sound/songs" && sed -i "0,/^	.byte	8$/s//	.byte	7/" "$root_dir/sound/songs/midi/mus_abandoned_ship.s"'
expect_fail "bijection" "bijection failed" copy \
    'rm "$root_dir/sound/direct_sound_samples/cries/abra.aif"'

echo "== Part E: .aif provenance =="
if git rev-parse --verify 'ee1173eb9^' >/dev/null 2>&1 \
   && git rev-parse --verify a620dddbc >/dev/null 2>&1; then
    prev_ok=1
    for rev in HEAD ee1173eb9^; do
        git ls-tree -r "$rev" -- sound/direct_sound_samples \
            | grep '\.aif$' | awk '{print $3}' | sort \
            | while read -r blob; do git cat-file blob "$blob" | sha256sum | awk '{print $1}'; done \
            | sort > "$tmp/aif_$(echo "$rev" | tr -cd '[:alnum:]').txt"
    done
    if ! cmp -s "$tmp/aif_HEAD.txt" "$tmp/aif_ee1173eb9.txt"; then
        fail "Part E: .aif content set differs from pre-fork canonical blobs"
        prev_ok=0
    fi
    if [ "$(wc -l < "$tmp/aif_HEAD.txt")" != "544" ]; then
        fail "Part E: HEAD .aif count != 544"
        prev_ok=0
    fi
    [ "$prev_ok" = 1 ] && pass "Part E: 544/544 .aif contents match the pre-fork canonical blobs"
    # The committed pins cover exactly the working-tree .aif set.
    ws="$(find sound/direct_sound_samples -name '*.aif' | wc -l)"
    pinned="$(grep -c '  sound/direct_sound_samples/' "$pins")"
    if [ "$ws" = "544" ] && [ "$pinned" = "544" ]; then
        pass "Part E: pins file covers 544/544 working-tree .aif"
    else
        fail "Part E: pins file count mismatch (pins=$pinned working-tree=$ws)"
    fi
else
    fail "Part E: provenance commits missing from history (shallow clone?)"
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "run_audio_inventory: ALL PASS ($passes checks)"
    exit 0
else
    echo "run_audio_inventory: $failures FAILURES, $passes passes" >&2
    exit 1
fi

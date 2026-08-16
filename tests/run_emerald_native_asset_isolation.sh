#!/usr/bin/env bash
#
# Stage R8 native asset-isolation runner
# (tests/run_emerald_native_asset_isolation.sh; extends the R6/R7A runners).
#
# Mechanically proves the native target matches the ownership declaration
# (resources/extraction/emerald/bpee01/ownership.generated.toml — front family
# R7B, 186 records — and back_ownership.generated.toml — back family R8,
# 10 records) for the whole trainer-front + trainer-back graphics families.
# The runner is driven entirely by those files — it parses every record's
# legacy_symbol, source_artifact, source_encoding, source_encoded_sha256,
# canonical_decoded_sha256 and targets.native instead of hardcoding any trainer
# (R6 §21, R7A §14/§16, R7B §7, R8 §8). After R8 both families' inventories
# are fully migrated:
#
#   native = ROM_BASE_ONLY  (all 196 resources - 93 front sheets + 93 front
#       palettes + 8 back sheets + 2 back palettes; the 6 alias back-palette
#       slots consume the R7B front normal palettes and declare no payload)
#       -> payload must be ABSENT from the native link: the encoded/decoded
#          bytes must not appear in the binary, the legacy symbol must not
#          appear in the binary or any native object, the GBA-only payload TUs
#          (src/data/graphics/trainers_front_payload.c and
#          src/data/graphics/trainers_back_payload.c) must not be compiled
#          for native, and the native dependency graph (scaninc closure +
#          make dry-run) must exclude the artifacts.
#
# R9 generalizes the proof to the Pokémon battle graphics family
# (resources/extraction/emerald/bpee01/pokemon_battle/ownership.generated.toml,
# 1608 records — the four battle tables' front/back sheets + normal/shiny
# palettes): all 1608 are ROM_BASE_ONLY. The GBA-only payload TUs
# (src/anim_mon_front_pics.c: 416 front leaves; src/data/graphics/
# pokemon_battle_payload.c: 1192 back/palette/shiny leaves incl. the slot-0
# circled-question-mark trio) must not be compiled for native, and the four
# tables' native row macros (SPECIES_BATTLE_* in src/data.c) must keep their
# NULL-sentinel native branches next to the GBA compiled branches, with the
# one external back-EGG row still compiled via SPECIES_SPRITE(EGG,
# gMonStillFrontPic_Egg). The two external aliases (gMonStillFrontPic_Egg,
# gMonPalette_Egg) and the still-front family are deliberately NOT records
# here (R9 §6).
#
# Byte-scan exemption (R9 §9): an encoded payload whose bytes are
# byte-identical to a still-compiled still-front asset (graphics/pokemon/*/
# front.4bpp.lz — the battle front and the party-menu front share the same
# source art for castform, and identical PNGs compress to identical streams)
# is REPORTED, not failed: the symbol/encoded-unless-identical/TU/dep-graph
# checks remain hard failures, and a genuinely leaked battle leaf is never
# byte-identical to a menu asset. This mirrors the small-decoded-payload
# NOTE policy.
#
#   native = COMPILED_PENDING_MIGRATION  (zero records after R8)
#       -> the state check stays in the runner, driven by the ownership files:
#          any record still declaring this state MUST have its legacy symbol
#          defined in the binary. A future family stage flips states as it
#          migrates; the check keeps the metadata honest either way.
#
# Fails when:
#   a) a ROM_BASE_ONLY leaf symbol reappears in the final binary or any native
#      object, or a GBA-only payload TU is compiled for native;
#   b) the encoded/decoded ROM_BASE_ONLY payload bytes reappear in the final
#      binary (byte scan; the 2048/8192/10240-byte decoded sheets are also
#      scanned, and the 32-byte decoded palette is reported, not failed,
#      because of collision risk — unchanged from R6);
#   c) the native dependency graph (scaninc closure + native make object list)
#      still includes a ROM_BASE_ONLY artifact;
#   d) ownership declares COMPILED_PENDING_MIGRATION but the legacy symbol is
#      missing from the native binary (state vs reality contradiction);
#   e) ownership declares ROM_BASE_ONLY but the build includes them (a/c above,
#      driven by ownership).
#
# Metadata truth (artifact sha256, encoded length, decoded length + sha256) is
# verified for EVERY record, regardless of state. RAW records (the 8 back
# sheets) are the payload verbatim — encoded == decoded — so they skip the
# GBA LZ77 decode step; gba-lz77 records (all palettes, front/back sheets —
# the whole Pokémon family) decode strictly per src/platform/bios.c
# LZ77UnCompWram.
#
# Usage:
#   run_emerald_native_asset_isolation.sh [--binary PATH] [--build]
#
#   --binary PATH   use this native binary instead of pokeemerald-linux64
#   --build         run `make -f Makefile_pc linux64` first so the binary is
#                   freshly built from the current tree
#
# Without --build, a missing or stale binary (older than the newest source /
# header / Makefile_pc mtime) fails fast with a build-required message.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
cd "$root"
ownership="$root/resources/extraction/emerald/bpee01/ownership.generated.toml"
back_ownership="$root/resources/extraction/emerald/bpee01/back_ownership.generated.toml"
pokemon_ownership="$root/resources/extraction/emerald/bpee01/pokemon_battle/ownership.generated.toml"
scaninc="$root/tools/scaninc/scaninc"
default_binary="$root/pokeemerald-linux64"

binary=""
do_build=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --build) do_build=1; shift ;;
        -h|--help) sed -n '2,52p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$binary" ]] || binary="$default_binary"

pass=0
fail=0
ok()   { pass=$((pass + 1)); printf 'ok   - %s\n' "$*"; }
bad()  { fail=$((fail + 1)); printf 'FAIL - %s\n' "$*"; }

[[ -f "$ownership" ]] || { echo "FATAL: ownership file missing: $ownership" >&2; exit 2; }
[[ -f "$back_ownership" ]] || { echo "FATAL: ownership file missing: $back_ownership" >&2; exit 2; }
[[ -f "$pokemon_ownership" ]] || { echo "FATAL: ownership file missing: $pokemon_ownership" >&2; exit 2; }

# ---------------------------------------------------------------------------
# Build / freshness
# ---------------------------------------------------------------------------
if [[ "$do_build" == 1 ]]; then
    echo "== building native target =="
    ( cd "$root" && make -f Makefile_pc linux64 )
fi

if [[ ! -f "$binary" ]]; then
    if [[ "$do_build" == 1 ]]; then
        echo "FATAL: binary still missing after --build: $binary" >&2; exit 1
    fi
    echo "FATAL: no binary at $binary; run with --build or pass --binary" >&2; exit 2
fi

newest_src="$(find "$root/src" "$root/include" "$root/gflib" \
    -type f \( -name '*.c' -o -name '*.h' \) -printf '%T@\n' 2>/dev/null \
    | sort -n | tail -1 || true)"
make_mtime="$(stat -c '%Y' "$root/Makefile_pc" 2>/dev/null || echo 0)"
bin_mtime="$(stat -c '%Y' "$binary")"
newest_src="${newest_src:-0}"
if awk -v n="$newest_src" -v m="$make_mtime" 'BEGIN{exit !(m>n)}'; then
    newest_src="$make_mtime"
fi
if awk -v b="$bin_mtime" -v n="$newest_src" 'BEGIN{exit !(b<n)}'; then
    if [[ "$do_build" == 1 ]]; then
        echo "FATAL: binary still stale after --build" >&2; exit 1
    fi
    echo "FATAL: $binary is stale (binary mtime < newest source mtime)." >&2
    echo "       Rebuild with --build, or pass a fresh --binary." >&2
    exit 2
fi
ok "native binary is fresh: $binary"

# ---------------------------------------------------------------------------
# Ownership-driven scans (python): hash re-verification, per-encoding payload
# validation (strict GBA LZ77 decode for gba-lz77 records, verbatim bytes for
# raw records), encoded/decoded payload byte absence (ROM_BASE_ONLY), and
# legacy symbol presence/absence in the binary via nm (COMPILED_PENDING_MIGRATION
# must be present, ROM_BASE_ONLY must be absent). Also emits the per-state
# symbol / artifact lists that the bash sections below consume, so every check
# is driven by the ownership files, never a hardcoded trainer name.
# ---------------------------------------------------------------------------
echo "== ownership-driven payload byte + symbol scans =="
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
python3 - "$binary" "$ownership" "$back_ownership" "$pokemon_ownership" "$tmpdir" <<'PY' || exit 1
import glob, hashlib, re, subprocess, sys

# Line-buffer stdout: under 2>&1 the runner's log is the record of record,
# and block-buffered stdout interleaves with unbuffered stderr NOTE/FAIL
# lines mid-line (stdout flush lands inside the previous stderr write),
# which garbles the log and has misled greps. Line buffering keeps each
# diagnostic on its own line in write order.
sys.stdout.reconfigure(line_buffering=True)

binary_path, ownership_path, back_ownership_path, pokemon_ownership_path, tmpdir = sys.argv[1:6]

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def gba_lz77_decode(data):
    """Independent GBA LZ77 decompressor, mirroring src/platform/bios.c
    LZ77UnCompWram exactly: flag bits tested MSB-first, 2-byte compressed
    entry read as (byte1 << 8) | byte2 with byte1 first, copy length
    (entry >> 12) + 3 from dest - offset - 1 where offset = entry & 0xFFF."""
    if len(data) < 4 or data[0] != 0x10:
        raise ValueError('not a gba-lz77 stream')
    size = data[1] | (data[2] << 8) | (data[3] << 16)
    out = bytearray()
    pos = 4
    remaining = size
    while remaining > 0:
        d = data[pos]; pos += 1
        if d:
            for i in range(8):
                if remaining == 0:
                    return bytes(out)
                if d & 0x80:
                    b1 = data[pos]; b2 = data[pos + 1]; pos += 2
                    entry = (b1 << 8) | b2
                    length = (entry >> 12) + 3
                    offset = entry & 0x0FFF
                    window = len(out) - offset - 1
                    if window < 0:
                        raise ValueError('LZ77 back-reference before start')
                    for _ in range(length):
                        out.append(out[window]); window += 1
                        remaining -= 1
                        if remaining == 0:
                            return bytes(out)
                else:
                    out.append(data[pos]); pos += 1
                    remaining -= 1
                    if remaining == 0:
                        return bytes(out)
                d = (d << 1) & 0xFF
        else:
            for i in range(8):
                out.append(data[pos]); pos += 1
                remaining -= 1
                if remaining == 0:
                    return bytes(out)
    return bytes(out)

with open(binary_path, 'rb') as f:
    binary = f.read()

nm_out = subprocess.run(['nm', '-g', '--defined-only', binary_path],
                        capture_output=True, text=True)
defined_syms = set(l.split()[-1] for l in nm_out.stdout.splitlines() if l.strip())

# All three ownership files (trainer front R7B, trainer back R8, Pokémon
# battle R9) declare the same [[resources]] record grammar; concatenate so
# every downstream check is driven by the 1804-record union, never a
# hardcoded species or trainer name.
text = open(ownership_path).read() + '\n' + open(back_ownership_path).read() \
    + '\n' + open(pokemon_ownership_path).read()

# R9 §9 byte-scan exemption: the sha256 of every still-compiled still-front
# asset (graphics/pokemon/<species>/front.4bpp.lz - the party-menu fronts,
# still compiled on native). A ROM_BASE_ONLY encoded payload that is
# byte-identical to one of these is the same-species art-sharing case
# (castform's battle front IS its menu front; identical PNGs compress to
# identical streams) and is reported, not failed - see the header comment.
# The glob is recursive: form species live nested below the species dir
# (question_mark/circled, question_mark/double, unown/<form>), and the
# circled question mark's BACK sprite is byte-identical to its front - the
# back payload legitimately appears in the binary as the compiled still-front
# asset, exactly the castform case one level deeper.
still_front_shas = set()
for still in glob.glob('graphics/pokemon/**/front.4bpp.lz', recursive=True):
    still_front_shas.add(sha256_file(still))
# Split into [[resources]] blocks (the [resources.targets] sub-table belongs to
# the preceding block). Values may be quoted strings or bare integers.
blocks = re.split(r'\n\[\[resources\]\]\n', '\n' + text)[1:]
records = []
for blk in blocks:
    rec = {}
    for line in blk.splitlines():
        m = re.match(r'^([a-z0-9_]+)\s*=\s*("?[^"]*"?)', line.strip())
        if m and m.group(1) not in ('native', 'gba'):
            rec[m.group(1)] = m.group(2).strip('"')
        elif m:
            rec['targets.' + m.group(1)] = m.group(2).strip('"')
    if rec.get('id'):
        records.append(rec)

if not records:
    print('FATAL: no [[resources]] records parsed from ownership', file=sys.stderr)
    sys.exit(1)

migrated_symbols = []
migrated_artifacts = []
compiled_symbols = []
bad = 0
for rec in records:
    rid = rec['id']
    symbol = rec['legacy_symbol']
    artifact = rec['source_artifact']
    enc_sha = rec['source_encoded_sha256']
    dec_sha = rec['canonical_decoded_sha256']
    enc_len = int(rec['encoded_length'])
    dec_len = int(rec['decoded_length'])
    encoding = rec['source_encoding']
    native_mode = rec['targets.native']
    gba_mode = rec['targets.gba']

    if native_mode not in ('ROM_BASE_ONLY', 'COMPILED_PENDING_MIGRATION'):
        print(f'FAIL - {rid}: unknown ownership targets.native = {native_mode!r}',
              file=sys.stderr)
        bad += 1
        continue
    if gba_mode != 'COMPILED':
        print(f'FAIL - {rid}: ownership targets.gba = {gba_mode!r}, '
              f'expected COMPILED', file=sys.stderr)
        bad += 1

    try:
        with open(artifact, 'rb') as f:
            encoded = f.read()
    except OSError:
        print(f'FAIL - {rid}: cannot read source artifact {artifact}', file=sys.stderr)
        bad += 1
        continue

    if len(encoded) != enc_len:
        print(f'FAIL - {rid}: artifact {artifact} is {len(encoded)} bytes, '
              f'ownership says {enc_len}', file=sys.stderr)
        bad += 1
    if sha256_file(artifact) != enc_sha:
        print(f'FAIL - {rid}: artifact {artifact} sha256 does not match ownership',
              file=sys.stderr)
        bad += 1

    if encoding == 'raw':
        # RAW records (the 8 back sheets): the source artifact IS the decoded
        # payload (compat entry encoding RAW = streamed verbatim; the sheet
        # doubles as the LZ77 input for the GBA decompressor). The generator
        # pins encoded == decoded hashes for these.
        if enc_sha != dec_sha:
            print(f'FAIL - {rid}: raw record must have source_encoded_sha256 == '
                  f'canonical_decoded_sha256', file=sys.stderr)
            bad += 1
            continue
        decoded = encoded
    elif encoding == 'gba-lz77':
        try:
            decoded = gba_lz77_decode(encoded)
        except ValueError as e:
            print(f'FAIL - {rid}: GBA LZ77 decode failed: {e}', file=sys.stderr)
            bad += 1
            continue
        if len(decoded) != dec_len:
            print(f'FAIL - {rid}: decoded {len(decoded)} bytes, ownership says {dec_len}',
                  file=sys.stderr)
            bad += 1
    else:
        print(f'FAIL - {rid}: unknown source_encoding {encoding!r}', file=sys.stderr)
        bad += 1
        continue
    if hashlib.sha256(decoded).hexdigest() != dec_sha:
        print(f'FAIL - {rid}: decoded bytes do not match canonical_decoded_sha256 '
              f'(LZ77 decoder or ownership wrong)', file=sys.stderr)
        bad += 1
        continue

    if native_mode == 'ROM_BASE_ONLY':
        migrated_symbols.append(symbol)
        migrated_artifacts.append(artifact)
        if symbol in defined_syms:
            print(f'FAIL - {rid}: ROM_BASE_ONLY symbol {symbol} present in {binary_path}',
                  file=sys.stderr)
            bad += 1
        else:
            print(f'ok   - {rid}: ROM_BASE_ONLY symbol {symbol} absent from binary')
        if binary.find(encoded) != -1:
            if enc_sha in still_front_shas:
                # Art-sharing with the still-compiled menu-front family
                # (castform): the same bytes legitimately sit in the binary
                # as the still-front asset; the symbol/TU/dep-graph checks
                # are the hard isolation proof for this record.
                print(f'NOTE - {rid}: {enc_len}-byte encoded payload found in '
                      f'{binary_path} but byte-identical to a compiled '
                      f'still-front asset (R9 §9 art-sharing); reported, '
                      f'not failed', file=sys.stderr)
            else:
                print(f'FAIL - {rid}: {enc_len}-byte encoded payload found in {binary_path} '
                      f'({symbol} leaked into the native executable)', file=sys.stderr)
                bad += 1
        else:
            print(f'ok   - {rid}: encoded {enc_len}-byte payload absent from binary')
        if binary.find(decoded) != -1:
            if dec_len >= 1024:
                print(f'FAIL - {rid}: {dec_len}-byte decoded payload found in {binary_path}',
                      file=sys.stderr)
                bad += 1
            else:
                # Small decoded payloads (the 32-byte palettes) are REPORTED, not
                # failed: a 32-byte run can legitimately coincide with unrelated
                # bytes elsewhere in the binary (R7B hit this: rs-may's decoded
                # palette matched an x86 instruction run in .text). The real
                # isolation proof for palettes is the symbol/encoded/TU/dep-graph
                # checks, which remain hard failures.
                print(f'NOTE - {rid}: {dec_len}-byte decoded payload collides at '
                      f'0x{binary.find(decoded):x} (reported, not failed: '
                      f'small-payload collision risk)',
                      file=sys.stderr)
        else:
            print(f'ok   - {rid}: decoded {dec_len}-byte payload absent from binary')
    else:
        compiled_symbols.append(symbol)
        if symbol not in defined_syms:
            print(f'FAIL - {rid}: COMPILED_PENDING_MIGRATION symbol {symbol} '
                  f'missing from {binary_path} (ownership says still compiled)',
                  file=sys.stderr)
            bad += 1
        else:
            print(f'ok   - {rid}: COMPILED_PENDING_MIGRATION symbol {symbol} present in binary')
        print(f'ok   - {rid}: encoded {enc_len}-byte payload still compiled (not scanned)')

with open(f'{tmpdir}/migrated_symbols.txt', 'w') as f:
    f.write('\n'.join(migrated_symbols) + ('\n' if migrated_symbols else ''))
with open(f'{tmpdir}/migrated_artifacts.txt', 'w') as f:
    f.write('\n'.join(migrated_artifacts) + ('\n' if migrated_artifacts else ''))
with open(f'{tmpdir}/compiled_symbols.txt', 'w') as f:
    f.write('\n'.join(compiled_symbols) + ('\n' if compiled_symbols else ''))

sys.exit(1 if bad else 0)
PY
ok "payload byte + symbol scans (state-driven)"

# ---------------------------------------------------------------------------
# Symbol absence in native objects + GBA-only TU exclusion.
# ---------------------------------------------------------------------------
echo "== symbol scan in native objects =="
mapfile -t migrated_symbols < "$tmpdir/migrated_symbols.txt"
mapfile -t compiled_symbols < "$tmpdir/compiled_symbols.txt"
mapfile -t migrated_artifacts < "$tmpdir/migrated_artifacts.txt"
[[ ${#migrated_symbols[@]} -gt 0 ]] || { echo "FATAL: no ROM_BASE_ONLY records" >&2; exit 2; }
if [[ ${#compiled_symbols[@]} -eq 0 ]]; then
    ok "R8 end state: zero COMPILED_PENDING_MIGRATION records (all 196 ROM_BASE_ONLY)"
fi

payload_tu="$root/src/data/graphics/trainers_front_payload.c"
back_payload_tu="$root/src/data/graphics/trainers_back_payload.c"
mon_front_payload_tu="$root/src/anim_mon_front_pics.c"
mon_battle_payload_tu="$root/src/data/graphics/pokemon_battle_payload.c"
if [[ -f "$payload_tu" ]]; then
    ok "GBA-only payload TU exists: src/data/graphics/trainers_front_payload.c"
else
    bad "GBA-only payload TU missing: src/data/graphics/trainers_front_payload.c"
fi
if [[ -f "$back_payload_tu" ]]; then
    ok "GBA-only payload TU exists: src/data/graphics/trainers_back_payload.c"
else
    bad "GBA-only payload TU missing: src/data/graphics/trainers_back_payload.c"
fi
if [[ -f "$mon_front_payload_tu" ]]; then
    ok "GBA-only payload TU exists: src/anim_mon_front_pics.c"
else
    bad "GBA-only payload TU missing: src/anim_mon_front_pics.c"
fi
if [[ -f "$mon_battle_payload_tu" ]]; then
    ok "GBA-only payload TU exists: src/data/graphics/pokemon_battle_payload.c"
else
    bad "GBA-only payload TU missing: src/data/graphics/pokemon_battle_payload.c"
fi

# Native objects: only the object dir of the CHECKED binary is scanned. The
# other target dirs are either stale (build/linux holds a pre-R7B `linux`
# build; its objects predate the payload-TU exclusion) or legitimately
# compiled targets (windows64/pc keep the payloads like GBA and are out of
# the ROM_BASE_ONLY ownership model, which only has native/gba columns). The
# checked binary passed the freshness check above, so its objects are the
# authoritative native object graph for the absence proof. No object may
# define a ROM_BASE_ONLY (migrated) symbol, and the payload TU's object must
# not exist at all. The per-object symbol check runs one nm per object against
# the whole migrated list (comm), not a subprocess per symbol.
case "$(basename "$binary")" in
    pokeemerald-linux64) obj_dirs=("$root/build/linux64") ;;
    pokeemerald)         obj_dirs=("$root/build/linux") ;;
    *)                   obj_dirs=() ;; # unknown target: binary-level checks only
esac
found_objects=0
for dir in "${obj_dirs[@]}"; do
    [[ -d "$dir" ]] || continue
    while IFS= read -r -d '' obj; do
        found_objects=1
        if [[ "$obj" == *data/graphics/trainers_front_payload.o ]] \
           || [[ "$obj" == *data/graphics/trainers_back_payload.o ]] \
           || [[ "$obj" == *anim_mon_front_pics.o ]] \
           || [[ "$obj" == *data/graphics/pokemon_battle_payload.o ]]; then
            bad "GBA-only payload TU was compiled for native: $obj"
        fi
        nm "$obj" 2>/dev/null | awk '{print $3}' | sort -u > "$tmpdir/obj_syms.txt"
        if [[ -s "$tmpdir/obj_syms.txt" ]] && grep -Fxf "$tmpdir/obj_syms.txt" \
            "$tmpdir/migrated_symbols.txt" > "$tmpdir/obj_hits.txt" 2>/dev/null; then
            while IFS= read -r sym; do
                bad "ROM_BASE_ONLY symbol $sym defined in native object $obj"
            done < "$tmpdir/obj_hits.txt"
        fi
    done < <(find "$dir" -name '*.o' -print0)
done
if [[ "$found_objects" == 1 ]]; then
    ok "native object set scanned (no ROM_BASE_ONLY symbols, no payload TU object)"
else
    ok "no native object dir present (fresh source tree; binary-only check ran)"
fi

# ---------------------------------------------------------------------------
# Native dependency graph: scaninc closure of the TUs that previously pulled
# the ROM_BASE_ONLY leaves in, and the native make object list.
# ---------------------------------------------------------------------------
echo "== native dependency graph =="
if [[ ! -x "$scaninc" ]]; then
    echo "FATAL: scaninc not built at $scaninc (run 'make tools' first)" >&2
    exit 2
fi
for tu in src/graphics.c src/data.c; do
    closure="$("$scaninc" -I "$root/include" -I "$root/tools/agbcc/include" -I "$root/gflib" "$root/$tu" 2>/dev/null || true)"
    for artifact in "${migrated_artifacts[@]}"; do
        if grep -Fq "$artifact" <<<"$closure"; then
            bad "scaninc closure of $tu still includes ROM_BASE_ONLY artifact $artifact"
        else
            ok "scaninc closure of $tu excludes $artifact"
        fi
    done
done

# Native make object list: the payload TU and the ROM_BASE_ONLY artifacts must
# not appear anywhere in the dry-run of the native build.
dry="$(cd "$root" && make -f Makefile_pc linux64 -n 2>/dev/null || true)"
if grep -Eq "trainers_(front|back)_payload|anim_mon_front_pics|pokemon_battle_payload" <<<"$dry"; then
    bad "native make dry-run references a GBA-only payload TU"
else
    ok "native make dry-run excludes the four GBA-only payload TUs"
fi
for artifact in "${migrated_artifacts[@]}"; do
    if grep -Fq "$artifact" <<<"$dry"; then
        bad "native make dry-run includes ROM_BASE_ONLY artifact $artifact"
    else
        ok "native make dry-run excludes $artifact"
    fi
done

# ---------------------------------------------------------------------------
# GBA branch intact (source-level; no GBA toolchain in this environment).
# The GBA build must keep compiling the ORIGINAL compiled assets (R7B §3/§4,
# R8 §9): the front/back tables' #else branches carry the legacy symbol
# argument, the native branches are NULL sentinels, and the two GBA-only
# payload TUs define every leaf payload the GBA link still needs — the front
# TU all 186 INCBIN_U32 leaves, the back TU (R8) the 8 INCBIN_U8 raw sheets
# + the 2 INCBIN_U32 Red/Leaf back palettes (GBA-only since R8; the 6 alias
# back-palette slots consume the R7B front normal palettes).
# ---------------------------------------------------------------------------
echo "== GBA branch intact (source inspection) =="
front="$root/src/data/trainer_graphics/front_pic_tables.h"
back="$root/src/data/trainer_graphics/back_pic_tables.h"
if grep -Fq 'TRAINER_SPRITE(trainerPic, sprite, size) [TRAINER_PIC_##trainerPic] = {NULL, size' "$front" \
   && grep -Fq 'TRAINER_SPRITE(trainerPic, sprite, size) [TRAINER_PIC_##trainerPic] = {sprite, size' "$front" \
   && grep -Fq 'TRAINER_PAL(trainerPic, pal) [TRAINER_PIC_##trainerPic] = {NULL' "$front" \
   && grep -Fq 'TRAINER_PAL(trainerPic, pal) [TRAINER_PIC_##trainerPic] = {pal' "$front" \
   && grep -q '^struct CompressedSpriteSheet gTrainerFrontPicTable\[\]' "$front" \
   && grep -q 'TRAINER_SPRITE(HIKER, gTrainerFrontPic_Hiker, TRAINER_PIC_SIZE)' "$front" \
   && grep -q 'TRAINER_SPRITE(BRENDAN, gTrainerFrontPic_Brendan, TRAINER_PIC_SIZE)' "$front" \
   && grep -q 'TRAINER_SPRITE(RS_MAY, gTrainerFrontPic_RubySapphireMay, TRAINER_PIC_SIZE)' "$front" \
   && grep -q 'TRAINER_PAL(HIKER, gTrainerPalette_Hiker)' "$front" \
   && grep -q 'TRAINER_PAL(BRENDAN, gTrainerPalette_Brendan)' "$front" \
   && grep -q 'TRAINER_PAL(RS_MAY, gTrainerPalette_RubySapphireMay)' "$front"; then
    ok "GBA front tables keep native NULL sentinels + GBA legacy symbol branches"
else
    bad "GBA front table branch lost a legacy symbol reference"
fi
if grep -Fq 'TRAINER_BACK_PAL_SHARED(trainerPic, pal) [TRAINER_BACK_PIC_##trainerPic] = {NULL' "$back" \
   && grep -Fq 'TRAINER_BACK_PAL_SHARED(trainerPic, pal) [TRAINER_BACK_PIC_##trainerPic] = {pal' "$back" \
   && grep -Fq 'TRAINER_BACK_PAL_BACK_ONLY(trainerPic, pal) [TRAINER_BACK_PIC_##trainerPic] = {NULL' "$back" \
   && grep -Fq 'TRAINER_BACK_PAL_BACK_ONLY(trainerPic, pal) [TRAINER_BACK_PIC_##trainerPic] = {pal' "$back" \
   && grep -q 'TRAINER_BACK_PAL_SHARED(BRENDAN, gTrainerPalette_Brendan)' "$back" \
   && grep -q 'TRAINER_BACK_PAL_SHARED(STEVEN, gTrainerPalette_Steven)' "$back" \
   && grep -q 'TRAINER_BACK_PAL_BACK_ONLY(RED, gTrainerBackPicPalette_Red)' "$back" \
   && grep -q 'TRAINER_BACK_PAL_BACK_ONLY(LEAF, gTrainerBackPicPalette_Leaf)' "$back"; then
    ok "back-pic table keeps shared-slot sentinels + Red/Leaf compiled branches"
else
    bad "back-pic table lost a compiled branch reference"
fi
if [[ -f "$payload_tu" ]] \
   && [[ "$(grep -c 'INCBIN_U32(' "$payload_tu")" -eq 186 ]] \
   && grep -q 'INCBIN_U32("graphics/trainers/front_pics/hiker.4bpp.lz")' "$payload_tu" \
   && grep -q 'INCBIN_U32("graphics/trainers/front_pics/brendan.4bpp.lz")' "$payload_tu" \
   && grep -q 'INCBIN_U32("graphics/trainers/front_pics/hiker.gbapal.lz")' "$payload_tu" \
   && grep -q 'INCBIN_U32("graphics/trainers/palettes/brendan.gbapal.lz")' "$payload_tu"; then
    ok "GBA-only payload TU defines all 186 legacy leaf payloads"
else
    bad "GBA-only payload TU lost an INCBIN payload"
fi
if [[ -f "$back_payload_tu" ]] \
   && [[ "$(grep -c 'INCBIN_U8(' "$back_payload_tu")" -eq 8 ]] \
   && [[ "$(grep -c 'INCBIN_U32(' "$back_payload_tu")" -eq 2 ]] \
   && grep -q 'INCBIN_U8("graphics/trainers/back_pics/brendan.4bpp")' "$back_payload_tu" \
   && grep -q 'INCBIN_U8("graphics/trainers/back_pics/steven.4bpp")' "$back_payload_tu" \
   && grep -q 'INCBIN_U32("graphics/trainers/back_pics/red.gbapal.lz")' "$back_payload_tu" \
   && grep -q 'INCBIN_U32("graphics/trainers/back_pics/leaf.gbapal.lz")' "$back_payload_tu"; then
    ok "GBA-only back payload TU defines all 10 legacy leaf payloads"
else
    bad "GBA-only back payload TU lost an INCBIN payload"
fi

# R9 §9: Pokémon battle family GBA-branch intact. The four battle tables'
# row macros live in src/data.c (SPECIES_BATTLE_SPRITE / SPECIES_BATTLE_PAL /
# SPECIES_BATTLE_SHINY_PAL): each must keep BOTH the native NULL-sentinel
# branch and the GBA compiled branch, and the one external back-EGG row must
# keep the original compiled SPECIES_SPRITE macro. The two GBA-only payload
# TUs must define every leaf the GBA link still needs: anim_mon_front_pics.c
# the 416 front leaves (incl. the slot-0 circled-question-mark front),
# pokemon_battle_payload.c the 1192 back/palette/shiny leaves (incl. the
# circled-question-mark trio and the egg palette).
data_c="$root/src/data.c"
if grep -Fq 'SPECIES_BATTLE_SPRITE(species, sprite) [SPECIES_##species] = {NULL, MON_PIC_SIZE, SPECIES_##species}' "$data_c" \
   && grep -Fq 'SPECIES_BATTLE_SPRITE(species, sprite) [SPECIES_##species] = {sprite, MON_PIC_SIZE, SPECIES_##species}' "$data_c" \
   && grep -Fq 'SPECIES_BATTLE_PAL(species, pal) [SPECIES_##species] = {NULL, SPECIES_##species}' "$data_c" \
   && grep -Fq 'SPECIES_BATTLE_PAL(species, pal) [SPECIES_##species] = {pal, SPECIES_##species}' "$data_c" \
   && grep -Fq 'SPECIES_BATTLE_SHINY_PAL(species, pal) [SPECIES_##species] = {NULL, SPECIES_##species + SPECIES_SHINY_TAG}' "$data_c" \
   && grep -Fq 'SPECIES_BATTLE_SHINY_PAL(species, pal) [SPECIES_##species] = {pal, SPECIES_##species + SPECIES_SHINY_TAG}' "$data_c"; then
    ok "data.c battle row macros keep native NULL + GBA compiled branches"
else
    bad "data.c battle row macros lost a branch"
fi
if grep -q 'SPECIES_SPRITE(EGG, gMonStillFrontPic_Egg)' \
        "$root/src/data/pokemon_graphics/back_pic_table.h"; then
    ok "back table external back-EGG row keeps the compiled SPECIES_SPRITE macro"
else
    bad "back table external back-EGG row lost the compiled macro"
fi
if [[ -f "$mon_front_payload_tu" ]] \
   && [[ "$(grep -c 'INCBIN_U32(' "$mon_front_payload_tu")" -eq 416 ]] \
   && grep -q 'INCBIN_U32("graphics/pokemon/bulbasaur/anim_front.4bpp.lz")' "$mon_front_payload_tu" \
   && grep -q 'INCBIN_U32("graphics/pokemon/question_mark/circled/anim_front.4bpp.lz")' "$mon_front_payload_tu" \
   && grep -q 'INCBIN_U32("graphics/pokemon/deoxys/anim_front.4bpp.lz")' "$mon_front_payload_tu"; then
    ok "GBA-only anim TU defines all 416 front leaves"
else
    bad "GBA-only anim TU lost an INCBIN payload"
fi
if [[ -f "$mon_battle_payload_tu" ]] \
   && [[ "$(grep -c 'INCBIN_U32(' "$mon_battle_payload_tu")" -eq 1192 ]] \
   && grep -q 'INCBIN_U32("graphics/pokemon/abra/back.4bpp.lz")' "$mon_battle_payload_tu" \
   && grep -q 'INCBIN_U32("graphics/pokemon/question_mark/circled/back.4bpp.lz")' "$mon_battle_payload_tu" \
   && grep -q 'INCBIN_U32("graphics/pokemon/egg/normal.gbapal.lz")' "$mon_battle_payload_tu" \
   && grep -q 'INCBIN_U32("graphics/pokemon/abra/shiny.gbapal.lz")' "$mon_battle_payload_tu"; then
    ok "GBA-only battle payload TU defines all 1192 back/palette/shiny leaves"
else
    bad "GBA-only battle payload TU lost an INCBIN payload"
fi

echo
echo "native asset-isolation: $pass ok, $fail failed"
[[ "$fail" == 0 ]] || exit 1
echo "PASS: native target matches ownership (1804/1804 ROM_BASE_ONLY isolated - 196 trainer + 1608 pokemon battle, 0 COMPILED_PENDING_MIGRATION)"

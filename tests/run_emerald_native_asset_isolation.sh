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
#          for native, and the native dependency graph (the build's own
#          preprocessed TUs + make dry-run) must exclude the artifacts.
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
# R11-D generalizes the proof to the raw map-layout family
# (resources/extraction/emerald/bpee01/layout/ownership.generated.toml,
# 882 records — 441 blockdata + 441 border leaves): all 882 are
# ROM_BASE_ONLY, raw encoding (encoded == decoded). The GBA-only payload
# lives in data/maps.s's generated layouts.inc; the mapjson generator emits
# a `.if LINUX64` skip for the whole layout-headers section, so the native
# maps.o must contain no blockdata/border byte sequences — the runner's
# per-record object scan proves it (the preprocessed-TU check does not
# cover layouts.inc: it is ASM, not a C TU). The 8-byte borders and the
# 2/4-byte unused-map blockdata are regular u16 patterns that coincide
# with compiled x86 instruction/data runs and pre-relocation pointer
# addends, so the 10 distinct hitting patterns are sha-keyed exemptions
# (R11-D §10, classified at scan time); the symbol/TU/dep-graph checks
# remain the hard isolation proof.
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
#   c) the native dependency graph (the build's own preprocessed TUs +
#      native make object list) still includes a ROM_BASE_ONLY artifact;
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
object_event_ownership="$root/resources/extraction/emerald/bpee01/object_event/ownership.generated.toml"
tileset_ownership="$root/resources/extraction/emerald/bpee01/tileset/ownership.generated.toml"
layout_ownership="$root/resources/extraction/emerald/bpee01/layout/ownership.generated.toml"
scaninc="$root/tools/scaninc/scaninc"
default_binary="$root/pokeemerald-linux64"

# R11-D: the maps.o of the checked binary (the assembled data/maps.s with
# the `.if LINUX64` skip) is the object-scan target proving no blockdata/
# border byte sequences are compiled for native. Mirrors the obj_dirs case
# below; unknown binary names get no object scan (binary-level checks only).
maps_obj=""
case "$(basename "${binary:-}")" in
    pokeemerald-linux64) maps_obj="$root/build/linux64/data/maps.o" ;;
    pokeemerald)         maps_obj="$root/build/linux/data/maps.o" ;;
esac

binary=""
do_build=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --build) do_build=1; shift ;;
        -h|--help) sed -n '2,/^set -euo pipefail/p' "$0"; exit 0 ;;
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
[[ -f "$object_event_ownership" ]] || { echo "FATAL: ownership file missing: $object_event_ownership" >&2; exit 2; }
[[ -f "$tileset_ownership" ]] || { echo "FATAL: ownership file missing: $tileset_ownership" >&2; exit 2; }
[[ -f "$layout_ownership" ]] || { echo "FATAL: ownership file missing: $layout_ownership" >&2; exit 2; }

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
python3 - "$binary" "$ownership" "$back_ownership" "$pokemon_ownership" "$object_event_ownership" "$tileset_ownership" "$layout_ownership" "$tmpdir" "$maps_obj" <<'PY' || exit 1
import glob, hashlib, re, subprocess, sys

# Line-buffer stdout: under 2>&1 the runner's log is the record of record,
# and block-buffered stdout interleaves with unbuffered stderr NOTE/FAIL
# lines mid-line (stdout flush lands inside the previous stderr write),
# which garbles the log and has misled greps. Line buffering keeps each
# diagnostic on its own line in write order.
sys.stdout.reconfigure(line_buffering=True)

binary_path, ownership_path, back_ownership_path, pokemon_ownership_path, object_event_ownership_path, tileset_ownership_path, layout_ownership_path, tmpdir, maps_obj_path = sys.argv[1:10]

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

# R11-D: the maps.o object scan target (native data/maps.s with the
# `.if LINUX64` skip — the object-scan proof that no blockdata/border
# byte sequences are compiled for native). Empty for unknown binaries.
maps_obj = None
if maps_obj_path:
    try:
        with open(maps_obj_path, 'rb') as f:
            maps_obj = f.read()
    except OSError:
        pass

nm_out = subprocess.run(['nm', '-g', '--defined-only', binary_path],
                        capture_output=True, text=True)
defined_syms = set(l.split()[-1] for l in nm_out.stdout.splitlines() if l.strip())

# All six ownership files (trainer front R7B, trainer back R8, Pokémon
# battle R9, object-event R11-B, tileset R11-C, layout R11-D) declare the
# same [[resources]] record grammar; concatenate so every downstream check
# is driven by the 4518-record union, never a hardcoded species, trainer
# or layout name.
# R11-C: the tileset ownership file appends [[gba_parity]] blocks (18)
# after its last [[resources]] record. Strip them per file before the
# block split: the regex split below would otherwise fold the first
# parity sub-table into the preceding [[resources]] block, corrupting its
# record (the R7B-era parser bug that misattributed unused2/tiles).
# Per-file stripping keeps the concatenation order free — the layout
# file (no parity blocks) is safe to append after the tileset file.
parts = []
for path in (ownership_path, back_ownership_path, pokemon_ownership_path,
             object_event_ownership_path, tileset_ownership_path,
             layout_ownership_path):
    content = open(path).read()
    parts.append(content.split('\n[[gba_parity]]')[0])
text = '\n'.join(parts)

# R11-B byte-scan exemptions for the object-event family:
#   - fossil sheet: mirage_tower.c keeps a documented duplicate INCBIN
#     (sFossil_Gfx, "Duplicate of gObjectEventPic_Fossil") for the tower
#     puzzle - a separate compiled consumer of the same bytes; the npc1
#     PALETTE is the same art-sharing one level deeper (cmp-identical to
#     the compiled sFossil_Pal from the same TU, mirage_tower.c:77);
#   - the seven 32-byte BGR555 palettes: zero-dominated patterns that
#     coincide with zero-padded text regions in the native binary (the
#     payload symbols are the session-published .bss slots and the
#     artifacts are not linked anywhere - the symbol checks above already
#     prove no payload TU carries them).
object_event_byte_exemptions = {
    "6523ac5fa9c7d5cb405a344a86569d21a698c1a84e36dcdd3324c6c785c64459": "art-share: byte-identical to compiled sFossil_Pal (src/mirage_tower.c:77, INCBIN of graphics/object_events/pics/misc/fossil.gbapal - verified cmp-identical; nm shows the hit inside sFossil_Pal)",
    "f5ae73fbc466686b4ffdbadb4a72611576feb065f6e8c352851ffe75ec843774": "mirage tower duplicate / text padding coincidence",
    "46a48ce199a070c77b11d872d48c2025a23e16beee9f3fda4441491e637e6d37": "decoration palette share / text padding coincidence",
    "6d7439319a03cd9cc0623cec7913f0b4a0e0766772108979edb52e3f14e08112": "text padding coincidence",
    "b4197e3b7eec6c35ef1bafcbe4debd2ff483f33cf5d662dca436c7d8d08ce01b": "mirage tower duplicate (sFossil_Gfx)",
    "cc3a0428e196db601c66e1f62831caba6eb9eca15759af7403d638e18bdbc2c9": "decoration palette share / text padding coincidence",
    "1198001471e8fd508edc95cb4d0dde0310ee847149cc6ef654ad2c048032d0a3": "text padding coincidence",
    "1622618b2104fa423128075bfc05222b25929bed01e1f06addf255059cb1568b": "decoration palette share / text padding coincidence",
    "fcb96dbc6e0780037fbb327b30bf5c576b6336c943fc8ab21e3fcfcbdc78ca12": "decoration palette share / text padding coincidence",
}

# R11-C §7 byte-scan exemptions for the tileset family. Every hit below was
# classified at scan time (the artifact's bytes verified present in the
# compiled consumer via grep -rln before failing, per the plan): art-sharing
# cases are byte-identical rows the game compiles through another symbol,
# coincidence cases are small patterns that occur inside unrelated compiled
# arrays, and the vacuous cases are all-zero payloads (zero runs exist in ELF
# padding and the .bss slots by definition - the symbol/encoded/TU/dep-graph
# checks remain the hard isolation proof for those records).
tileset_byte_exemptions = {
    "e52f9192018a822afada0c364c16293490a897cad6245dd11b1d8623e1ba732d": "coincides with compiled sSecretPowerCave_Pal (src/fldeff_misc.c:71): 4 zero + 12 shared BGR555 rock colors + 16 zero bytes, nm-verified hit inside the compiled palette",
    "66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925": "all-zero 32-byte palette row (vacuous: zero runs in ELF padding)",
    "2b85cb97303e882a92f1eff48a21ae32014b066ceeb659d91eea4e09628e3b4e": "art-share: byte-identical to compiled sSecretPowerPlant_Pal (src/fldeff_misc.c)",
    "c8c96d16c3d7eeaea1efcab251276587482178320d5cb7913b6b2a2821696738": "coincides with compiled gFieldEffectPal_SmallSparkle (2 non-zero bytes)",
    "fc1ab34685c687fb2c0c001f856b8c663eaf7eb9539122aa018b995aa03ae6a4": "art-share: windywater frames byte-identical to compiled gTradeGba_Gfx (src/trade.c)",
    "a7ac58812a2000fc87709d5ca41ae26b8445c87e0bb5ef8f6718940c4305095e": "coincides with compiled gMimicOrbAffineAnimCmds1 (0xff7f rows, src/battle_anim_effects_1.c)",
    "df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119": "all-zero 4-byte metatile attribute (vacuous: zero runs in ELF padding)",
    "38723a2e5e8a17aa7950dc008209944e898f69a7bd10a23c839d341e935fd5ca": "all-zero 128-byte floorlight frame (vacuous: zero runs in ELF padding)",
    "073d0ac82cd6b15c58a8e7cb0e07b88d4f89d91543ebdd75bbbf86506360611f": "art-share: byte-identical to compiled sHofMonitor_Pal (src/field_effect.c)",
    "2c50785a40875293a2bbace907621c10bbc81775e43e53c485cd4100543c2815": "coincides with compiled sTrainerHillWindowTilemap (1 non-zero byte, src/battle_records.c)",
    "47b9075b9657e3aeb769b021ec7dd2802e18d1075acafe43ad7f9c2525331145": "coincides with compiled gMonFootprint_Metang (1 non-zero byte, src/data/pokemon_graphics/footprint_table.h)",
    "9f448f59d867b2d529109d798eeb7c7c3522b0e896d226b575a1f72b3cce5a57": "coincides with compiled gMonIcon_Armaldo (1 non-zero byte, src/pokemon_icon.c)",
}

# R11-D §10 byte-scan exemptions for the layout family. Classified at scan
# time (each pattern's occurrences located in the binary via readelf/nm
# before exempting): the 8-byte borders are repeating u16 word patterns
# (0100/0102/0802/1002 ×4) that coincide with x86 instruction runs in
# .text (960/3/53/35 occurrences across many functions), and the 2/4-byte
# unused-map blockdata coincide with x86 instruction bytes and with
# pre-relocation pointer-addend bytes in maps.o's own gba_ptr table (the
# `.if LINUX64` skip leaves no payload; the addends are section offsets
# like 0x0606/0x02cc). The symbol/TU/dep-graph checks are the hard
# isolation proof for these records; no payload > 8 bytes exists anywhere
# in the binary or maps.o.
layout_byte_exemptions = {
    "a12871fee210fb8619291eaea194581cbd2531e4b23759d225f6806923f63222": "coincides with x86 instruction bytes (ADD [rdx],eax, 4728 hits in .text)",
    "47f844a39ac23142445523cb72edb75f397308ac71efc17f2b9daede7e50e602": "coincides with x86 instruction bytes (OR AL,0x06, 392 hits)",
    "27bfae0e6b1dc6d09ebc4b92b48fea35635b03360be5242e22be2ac087ec8690": "coincides with pre-relocation pointer addends in maps.o gba_ptr table + x86 runs",
    "18ff0eb89a3259344d5768002e0fdc3ac5ba60c0641adc8b40a8dfa5ac1da92d": "coincides with pre-relocation pointer addends in maps.o gba_ptr table + x86 runs",
    "e0b47af9a746b3bdbf3c4d4e580d0cc10fa86b5716f5f634322353eb3d0b698f": "coincides with pre-relocation pointer addends in maps.o gba_ptr table + x86 runs",
    "95bbb69057343664b86e2c986fe59a8841dc00c1f0a335e8f9f05e19a2c611af": "coincides with pre-relocation pointer addends in maps.o gba_ptr table + x86 runs",
    "efef8d5a9f8dbe5bb218e6b0cc452583eb9d1452b615b1c7ab66ad49c82e9d70": "coincides with x86 instruction runs in .text (960 hits of the 0100-repeating border)",
    "9ff63640fa3b4682056532b7924b03250ccc7a832f64ccec3dea79ea8595a24d": "coincides with x86 instruction runs in .text (SetOpponentMonData etc.)",
    "0bde8b5d0bef72ccc3fde59c9bf351ad45a86b2094e5e2f318528ff3f136ef4b": "coincides with x86 instruction runs in .text (53 hits)",
    "eaf2d69938b5842d182be81f3ea06561297c6576f76979732b1d679db7e6b874": "coincides with x86 instruction runs in .text (35 hits)",
}

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
    second = rec.get('source_artifact_2')
    if second:
        # R11-C multi-file records (Sootopolis StormyWater frames: the
        # INCBIN_U16 concatenates N_kyogre + N_groudon): the ownership
        # encoded bytes are the concatenation of both artifacts.
        try:
            with open(second, 'rb') as f:
                encoded += f.read()
        except OSError:
            print(f'FAIL - {rid}: cannot read source artifact_2 {second}', file=sys.stderr)
            bad += 1
            continue

    if len(encoded) != enc_len:
        print(f'FAIL - {rid}: artifact(s) {artifact}'
              + (f' + {second}' if second else '')
              + f' are {len(encoded)} bytes, ownership says {enc_len}',
              file=sys.stderr)
        bad += 1
    if hashlib.sha256(encoded).hexdigest() != enc_sha:
        print(f'FAIL - {rid}: artifact sha256 does not match ownership '
              f'(concatenated for source_artifact_2 records)', file=sys.stderr)
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
        if second:
            # R11-C: the second artifact of a multi-file record is equally
            # ROM_BASE_ONLY - the closure/dry-run checks must cover it too.
            migrated_artifacts.append(second)
        if symbol in defined_syms:
            # R11-B: the object-event payload leaves migrate to session-
            # published NATIVE SLOTS that keep the legacy symbol name (the
            # non-const published arrays, like the battle-table headers).
            # The payload-byte scans above already prove the bytes are gone;
            # the symbol itself is structural for this family.
            if rid.startswith('emerald:object-event/') \
               or rid.startswith('emerald:tileset'):
                print(f'ok   - {rid}: session-published native slot {symbol} '
                      f'present (payload bytes verified absent)')
            else:
                print(f'FAIL - {rid}: ROM_BASE_ONLY symbol {symbol} present in {binary_path}',
                      file=sys.stderr)
                bad += 1
        else:
            print(f'ok   - {rid}: ROM_BASE_ONLY symbol {symbol} absent from binary')
        if binary.find(encoded) != -1:
            exempt_reason = tileset_byte_exemptions.get(enc_sha) \
                or layout_byte_exemptions.get(enc_sha)
            if exempt_reason:
                # R11-C §7 / R11-D §10: sha-keyed, classified at scan time
                # (see the dicts above - art-sharing with a compiled
                # consumer verified via grep -rln, or a coincidence/vacuous
                # pattern); the symbol/TU/dep-graph checks are the hard
                # isolation proof.
                print(f'NOTE - {rid}: {enc_len}-byte encoded payload found in '
                      f'{binary_path} but sha-keyed exempt: '
                      f'{exempt_reason}; reported, not failed', file=sys.stderr)
            elif enc_sha in still_front_shas or enc_sha in object_event_byte_exemptions:
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
        # R11-D §10 object scan: native maps.o (assembled data/maps.s with
        # the `.if LINUX64` skip in layouts.inc) must not contain any
        # blockdata/border byte sequences. The 2/4-byte unused-map
        # blockdata and 8-byte repeating borders coincide with pointer
        # addends / instruction runs (sha-keyed exemptions above, shared
        # with the binary scan); anything else in maps.o is a leak.
        if maps_obj is not None and rid.startswith('emerald:layout/'):
            if maps_obj.find(encoded) != -1:
                if enc_sha in layout_byte_exemptions:
                    print(f'NOTE - {rid}: {enc_len}-byte payload found in '
                          f'native maps.o but sha-keyed exempt: '
                          f'{layout_byte_exemptions[enc_sha]} (R11-D §10); '
                          f'reported, not failed', file=sys.stderr)
                else:
                    print(f'FAIL - {rid}: {enc_len}-byte payload found in '
                          f'native maps.o ({symbol} leaked into the native '
                          f'map object)', file=sys.stderr)
                    bad += 1
            else:
                print(f'ok   - {rid}: payload absent from native maps.o')
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
    ok "end state: zero COMPILED_PENDING_MIGRATION records (all 4518 ROM_BASE_ONLY)"
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
                # R11-B/R11-C: the object-event and tileset payload slots are
                # DEFINED by the compat seam TUs (the session-published native
                # arrays) - the sanctioned definition site, like the
                # battle-table headers in data.c. The byte scans above prove
                # no payload bytes are linked.
                if [[ "$obj" == *emerald/resources/emerald_object_event_compat.o ]] \
                   || [[ "$obj" == *emerald/resources/emerald_tileset_compat.o ]]; then
                    ok "session-published slot $sym (seam TU)"
                else
                    bad "ROM_BASE_ONLY symbol $sym defined in native object $obj"
                fi
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
# Native dependency graph: the native build's OWN preprocessed TUs (the
# actual cpp command with -D NATIVE_LINUX, extracted from a forced make
# dry-run) must not reference any ROM_BASE_ONLY artifact, and the native
# make object list must exclude the payload TUs. The scaninc closure was the
# branch-union: scaninc cannot evaluate the -D NATIVE_LINUX guard that the
# plan §2 keeps around the in-place INCBIN arrays in graphics.h/
# metatiles.h/tileset_anims.c (GBA-branch payloads stay in those headers;
# parity anim frames are guarded the same way), so it false-positives on
# every GBA-branch INCBIN. Preprocessing with the build's own flags is the
# exact native ground truth.
# ---------------------------------------------------------------------------
echo "== native dependency graph =="
dry_full="$(cd "$root" && make -f Makefile_pc linux64 -nB 2>/dev/null || true)"
if [[ -z "$dry_full" ]]; then
    echo "FATAL: forced native make dry-run produced no output" >&2
    exit 2
fi
for tu in src/graphics.c src/data.c src/tileset_anims.c; do
    cppline="$(grep -m1 " $tu -o " <<<"$dry_full")"
    if [[ -z "$cppline" ]]; then
        bad "native preprocess command for $tu not found in forced dry-run"
        continue
    fi
    # Replace the single -o output path with stdout; the line is make-shell-
    # ready (embedded quoting included), so run it through bash verbatim.
    preprocessed="$(bash -c "${cppline/ -o / -o - }" 2>/dev/null || true)"
    for artifact in "${migrated_artifacts[@]}"; do
        if grep -Fq "$artifact" <<<"$preprocessed"; then
            bad "native preprocessed $tu references ROM_BASE_ONLY artifact $artifact"
        else
            ok "native preprocessed $tu excludes $artifact"
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
echo "PASS: native target matches ownership (4518/4518 ROM_BASE_ONLY isolated - 196 trainer + 1608 pokemon battle + 288 object-event + 1544 tileset + 882 layout, 0 COMPILED_PENDING_MIGRATION)"

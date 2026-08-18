#!/usr/bin/env python3
"""R12-A: machine-readable Emerald audio inventory.

Parses the checked-in MP2K audio sources of truth and emits
resources/extraction/emerald/bpee01/audio/inventory.generated.toml:

  * sound/song_table.inc            -> the 610 gSongTable rows. The 80
     `song dummy_song_header` alias rows are NOT resources (compiled dummy
     placeholder); mus_dummy IS a real song -> 530 song resources. gSongTable
     itself and gMPlayTable are routing tables, not payload resources.
  * sound/songs/*.s + midi/*.s      -> per-song header labels, track counts
     (the header's first `.byte` == NumTrks, cross-checked against the
     `.int`/`.4byte` <song>_<n> track-pointer rows) and the voicegroup
     reference (`.equ <song>_grp, voicegroup_<name>`).
  * sound/direct_sound_data.inc     -> 544 sample leaves: 105 root
     DirectSoundWaveData_*, 51 DirectSoundWaveData_Phoneme_<n>, 388 Cry_*.
     The 544 `.incbin "sound/direct_sound_samples/<rel>.bin"` artifacts must
     form a 1:1 bijection with the 544 tracked `<rel>.aif` authoring sources.
  * sound/programmable_wave_data.inc -> 25 ProgrammableWaveData_<n> waves
     (16-byte 32x4-bit CGB waveforms).
  * sound/voice_groups.inc          -> 196 include rows: 195 voicegroup .inc
     + 1 cry_tables.inc (which compiles the cry-table family, not a
     voicegroup). All 195 `voice_group <name>` macros are resources — the
     audit pins 195 tables (180 root + 10 drumsets + 5 keysplits) and
     voicegroup_dummy is one of them; its compiled default state is the
     R12 publication seam, not an inventory exclusion. Names come from the
     `voice_group <name>` macro INSIDE each file (drumsets/rs.inc ->
     voice_group rs_drumset: never from the path).
  * sound/cry_tables.inc            -> gCryTable + gCryTable_Reverse (388
     rows each; every Cry_* reference must resolve to a defined sample).
  * sound/keysplit_tables.inc       -> 5 keysplit runs.

And resources/extraction/emerald/bpee01/audio/canonical_sample_pins.generated.txt:
SHA-256 pins for all 544 tracked direct_sound_samples/**/*.aif authoring
sources. R12-A prerequisite: the 76 fork-expanded .aif were restored to the
canonical pret blobs (commit a620dddbc); --check fails if any sample source
drifts from the pinned canonical state.

Pinned counts (the R12 audit): 530 songs / 544 samples (105 root + 51 phoneme
+ 388 cry) / 25 waves / 195 voicegroups / 2 cry tables / 5 keysplits =
1301 resources. MP2K engine tables/constants (gSongTable, gMPlayTable,
dummy_song_header, MPlayDef equates) are NOT resources.

Key taxonomy (approved architecture, R12 §2):
  emerald:audio/song/<canonical>              music-sequence/1/gba-mp2k-song-graph
  emerald:audio/sample/<canonical>            audio-sample/1/gba-wave-data
  emerald:audio/sample/cry/<canonical>        audio-sample/1/gba-wave-data
  emerald:audio/sample/phoneme/<n>            audio-sample/1/gba-wave-data
  emerald:audio/wave/programmable/<n>         audio-sample/1/gba-cgb-wave
  emerald:audio/voicegroup/<canonical>        instrument-bank/1/gba-tone-data-12
  emerald:audio/cry-table/forward|reverse     instrument-bank/1/gba-tone-data-12
  emerald:audio/keysplit/<canonical>          instrument-bank/2/gba-keysplit-run

Canonical component = lowercase symbol suffix with "_" -> "-" (the R11-B
rule); phonemes/waves use their number.

Deterministic: all rows bytewise sorted by key; regeneration must be a no-op
diff; --check enforces that. Duplicate symbols, duplicate keys, malformed
source lines, missing family sources, count deviations, unresolved
references and the .aif/.bin bijection are all hard failures.

Usage: python3 gen_audio_inventory.py [repo-root] [output-path] [--check]
"""
import hashlib
import os
import re
import sys

# ---------------------------------------------------------------- pinned counts

PINNED = {
    "song": 530,
    "sample/root": 105,
    "sample/phoneme": 51,
    "sample/cry": 388,
    "programmable-wave": 25,
    "voicegroup": 195,
    "cry-table": 2,
    "keysplit": 5,
}

PINNED_TABLE_SONG_ROWS = 610          # gSongTable rows (530 real + 80 dummy)
PINNED_DUMMY_SONG_ROWS = 80           # `song dummy_song_header, 0, 0` aliases
PINNED_VOICEGROUP_INCLUDES = 195      # voicegroups/ includes (all 195, dummy.inc too)
PINNED_CRY_ROWS = 388                 # rows in EACH of the two cry tables
PINNED_VOICEGROUP_ROWS = 20594        # rows across the 195 voicegroup files
PINNED_KEYSPLIT_RUN_BYTES = 372       # 5 runs: 72+72+72+84+72 (tuba is 84)
PINNED_KEYS = sum(PINNED.values())    # 1301

# Back-shift pins (R12-C). The `voice_group name, N` / `keysplit name, N`
# second argument is the label back-shift: drumsets back-shift N ROWS, keysplit
# labels N BYTES (the starting note, per the mks4agb convention). route110 is
# 40 rows, all other drumsets 36; tuba starts at note 24, all other keysplits
# at 36.
PINNED_DRUMSET_BACKS = {
    "rs_drumset": 36, "frlg_drumset": 36, "emerald_drumset_1": 36,
    "emerald_drumset_2": 36, "petalburg_drumset": 36, "route101_drumset": 36,
    "route110_drumset": 40, "frlg_fanfare_drumset_1": 36,
    "frlg_fanfare_drumset_2": 36, "rg_credits_drumset": 36,
}
PINNED_KEYSPLIT_BACKS = {"piano": 36, "strings": 36, "trumpet": 36,
                         "tuba": 24, "french_horn": 36}
PINNED_KEYSPLIT_RUNS = {"piano": 72, "strings": 72, "trumpet": 72,
                        "tuba": 84, "french_horn": 72}

# ---------------------------------------------------------------- kind -> contract

# kind -> (resource_type, schema, representation, key template)
CONTRACT = {
    "song":              ("music-sequence", 1, "gba-mp2k-song-graph", "emerald:audio/song/{}"),
    "sample/root":       ("audio-sample", 1, "gba-wave-data", "emerald:audio/sample/{}"),
    "sample/phoneme":    ("audio-sample", 1, "gba-wave-data", "emerald:audio/sample/phoneme/{}"),
    "sample/cry":        ("audio-sample", 1, "gba-wave-data", "emerald:audio/sample/cry/{}"),
    "programmable-wave": ("audio-sample", 1, "gba-cgb-wave", "emerald:audio/wave/programmable/{}"),
    "voicegroup":        ("instrument-bank", 1, "gba-tone-data-12", "emerald:audio/voicegroup/{}"),
    "cry-table":         ("instrument-bank", 1, "gba-tone-data-12", "emerald:audio/cry-table/{}"),
    "keysplit":          ("instrument-bank", 2, "gba-keysplit-run", "emerald:audio/keysplit/{}"),
}

SONG_TABLE = "sound/song_table.inc"
DIRECT_SOUND_DATA = "sound/direct_sound_data.inc"
PROGRAMMABLE_WAVES = "sound/programmable_wave_data.inc"
VOICE_GROUPS = "sound/voice_groups.inc"
CRY_TABLES = "sound/cry_tables.inc"
KEYSPLIT_TABLES = "sound/keysplit_tables.inc"
SONGS_DIR = "sound/songs"
SONGS_MIDI_DIR = "sound/songs/midi"
VOICEGROUPS_DIR = "sound/voicegroups"
SAMPLES_DIR = "sound/direct_sound_samples"

# ---------------------------------------------------------------- helpers

def die(msg):
    sys.exit("FAIL: " + msg)


def canonical(symbol):
    # R11-B canonical naming: lowercase symbol suffix, "_" -> "-" (the pack's
    # canonical-name validator only accepts [a-z0-9._-]; Cry_Abra -> "abra").
    return symbol.replace("_", "-").lower()


def read_lines(root, path):
    try:
        with open(os.path.join(root, path), encoding="utf-8") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        die(f"missing family source: {path}")
    except OSError as e:
        die(f"cannot read {path}: {e}")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------- parsers

def parse_song_table(lines):
    """Returns (songs, dummy_rows) where songs is an OrderedDict of
    symbol -> (ms, me) for the 530 real rows."""
    songs = {}
    dummy = 0
    row = 0
    for i, line in enumerate(lines):
        m = re.match(r"\s*song\s+(\w+),\s*(\d+),\s*(\d+)", line)
        if not m:
            s = line.strip()
            # The trailing `dummy_song_header:` + `.byte` rows are the compiled
            # dummy placeholder definition, not table rows.
            if s == "" or s.startswith(".align") or s.startswith("gSongTable") \
               or s.startswith("@") or s.startswith("dummy_song_header") \
               or s.startswith(".byte"):
                continue
            die(f"{SONG_TABLE}:{i + 1}: malformed song row: {s!r}")
        row += 1
        name, ms, me = m.group(1), int(m.group(2)), int(m.group(3))
        if name == "dummy_song_header":
            dummy += 1
            continue
        if name in songs:
            die(f"{SONG_TABLE}:{i + 1}: duplicate song symbol {name}")
        songs[name] = (ms, me)
    if row != PINNED_TABLE_SONG_ROWS:
        die(f"{SONG_TABLE}: expected {PINNED_TABLE_SONG_ROWS} song rows, found {row}")
    if dummy != PINNED_DUMMY_SONG_ROWS:
        die(f"{SONG_TABLE}: expected {PINNED_DUMMY_SONG_ROWS} dummy_song_header rows, found {dummy}")
    if len(songs) != PINNED["song"]:
        die(f"song family: expected {PINNED['song']} songs, found {len(songs)}")
    return songs, dummy


def parse_song_file(root, name):
    """Parses one song .s file. Returns (track_count, voicegroup_name) or
    dies. The header label is `^<name>:$`; the first `.byte` after it is
    NumTrks; the `.int`/`.4byte <name>_<n>` rows below it are the track
    pointers (hand-written songs use .int, mid2agb output uses .4byte)."""
    path = os.path.join(SONGS_DIR, name + ".s")
    if not os.path.isfile(os.path.join(root, path)):
        path = os.path.join(SONGS_MIDI_DIR, name + ".s")
    if not os.path.isfile(os.path.join(root, path)):
        die(f"song {name}: no source file in {SONGS_DIR}/ or {SONGS_MIDI_DIR}/")
    lines = read_lines(root, path)
    label = re.compile(r"^" + re.escape(name) + r":+$")
    in_header = False
    declared = None
    track_refs = []
    vg = None
    for i, line in enumerate(lines):
        stripped = line.strip()
        m = re.match(r"\.equ\s+" + re.escape(name) + r"_grp,\s*(voicegroup_\w+)", stripped)
        if m:
            vg = m.group(1)
        if not in_header:
            if label.match(stripped):
                in_header = True
            continue
        if stripped.startswith(".end"):
            break
        m = re.match(r"\.byte\s+(\d+)", stripped)
        if m and declared is None:
            declared = int(m.group(1))
            continue
        m = re.match(r"\.(?:int|4byte)\s+" + re.escape(name) + r"_(\d+)\b", stripped)
        if m:
            track_refs.append(int(m.group(1)))
    if declared is None:
        die(f"song {name} ({path}): header has no NumTrks `.byte` row")
    if len(track_refs) != declared:
        die(f"song {name} ({path}): header declares {declared} tracks, "
            f"found {len(track_refs)} track pointers")
    if vg is None:
        die(f"song {name} ({path}): no `.equ {name}_grp, voicegroup_*` row")
    return declared, vg


def parse_direct_sound_data(lines):
    """Returns (samples, incbin_artifacts): samples maps symbol -> kind,
    incbin_artifacts maps artifact relative path -> symbol."""
    samples = {}
    artifacts = {}
    current = None
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped == "" or stripped.startswith("@") or stripped.startswith(".align"):
            continue
        m = re.match(r"(\w+)::", stripped)
        if m:
            current = m.group(1)
            if current in samples:
                die(f"{DIRECT_SOUND_DATA}:{i + 1}: duplicate sample symbol {current}")
            samples[current] = None
            continue
        m = re.match(r'\.incbin\s+"([^"]+)"', stripped)
        if m:
            if current is None:
                die(f"{DIRECT_SOUND_DATA}:{i + 1}: .incbin before any sample symbol")
            if samples[current] is not None:
                die(f"{DIRECT_SOUND_DATA}:{i + 1}: {current} has more than one .incbin")
            artifact = m.group(1)
            rel = artifact.replace("sound/direct_sound_samples/", "").removesuffix(".bin")
            if rel in artifacts:
                die(f"{DIRECT_SOUND_DATA}:{i + 1}: duplicate artifact {artifact}")
            artifacts[rel] = current
            samples[current] = artifact
    for sym, artifact in samples.items():
        if artifact is None:
            die(f"{DIRECT_SOUND_DATA}: sample {sym} has no .incbin")
    return samples, artifacts


def classify_samples(samples):
    kinds = {}
    for sym in samples:
        if sym.startswith("Cry_"):
            kinds[sym] = "sample/cry"
        elif re.match(r"DirectSoundWaveData_Phoneme_\d+$", sym):
            kinds[sym] = "sample/phoneme"
        elif sym.startswith("DirectSoundWaveData_"):
            kinds[sym] = "sample/root"
        else:
            die(f"unclassifiable sample symbol {sym}")
    return kinds


def parse_waves(lines):
    waves = {}
    current = None
    for i, line in enumerate(lines):
        stripped = line.strip()
        m = re.match(r"(ProgrammableWaveData_\d+)::", stripped)
        if m:
            current = m.group(1)
            if current in waves:
                die(f"{PROGRAMMABLE_WAVES}:{i + 1}: duplicate wave symbol {current}")
            waves[current] = None
            continue
        m = re.match(r'\.incbin\s+"([^"]+)"', stripped)
        if m:
            if current is None or waves[current] is not None:
                die(f"{PROGRAMMABLE_WAVES}:{i + 1}: misplaced .incbin")
            waves[current] = m.group(1)
    for sym, artifact in waves.items():
        if artifact is None:
            die(f"{PROGRAMMABLE_WAVES}: wave {sym} has no .incbin")
    return waves


def parse_voicegroups(root):
    """Returns OrderedDict symbol (voicegroup_<name>) -> (include path, back)
    for the 195 voice_group macros, where `back` is the optional second
    argument of `voice_group <name>, <rows>` (drumset label back-shift in
    rows; 0 for every non-drumset). voicegroup_dummy is inventoried too: the
    audit pins 195 tables (180 root + 10 drumsets + 5 keysplits) and dummy.inc
    is one of them; the compiled default state is the R12 publication seam."""
    lines = read_lines(root, VOICE_GROUPS)
    includes = 0
    cry_includes = 0
    groups = {}
    for i, line in enumerate(lines):
        stripped = line.strip()
        if re.match(r'\.include\s+"sound/cry_tables\.inc"', stripped):
            # The cry tables compile into the voicegroup section; they are
            # their own family (emerald:audio/cry-table/*), not a voicegroup.
            cry_includes += 1
            continue
        m = re.match(r'\.include\s+"sound/voicegroups/([^"]+)"', stripped)
        if not m:
            if stripped == "" or stripped.startswith(".align") \
               or stripped.startswith("@"):
                continue
            die(f"{VOICE_GROUPS}:{i + 1}: malformed include row: {stripped!r}")
        includes += 1
        inc_path = "sound/voicegroups/" + m.group(1)
        inc_lines = read_lines(root, inc_path)
        names = [(mm.group(1), int(mm.group(2) or 0)) for mm in
                 (re.match(r"voice_group\s+(\w+)(?:\s*,\s*(\d+))?", l.strip())
                  for l in inc_lines) if mm]
        if len(names) != 1:
            die(f"{inc_path}: expected exactly one `voice_group` row, found {len(names)}")
        name, back = names[0]
        symbol = "voicegroup_" + name
        if symbol in groups:
            die(f"{inc_path}: duplicate voicegroup symbol {symbol}")
        groups[symbol] = (inc_path, back)
    if includes != PINNED_VOICEGROUP_INCLUDES:
        die(f"{VOICE_GROUPS}: expected {PINNED_VOICEGROUP_INCLUDES} voicegroup includes, "
            f"found {includes}")
    if cry_includes != 1:
        die(f"{VOICE_GROUPS}: expected exactly 1 cry_tables.inc include, found {cry_includes}")
    if len(groups) != PINNED["voicegroup"]:
        die(f"voicegroup family: expected {PINNED['voicegroup']} voicegroups, "
            f"found {len(groups)}")
    return groups


# Row macro -> symbol operands (0-based arg indexes after the macro name).
# roles: sample/wave resolve in the leaf families, group in the voicegroup
# family, keysplit in the keysplit family. Macros not listed here carry only
# numeric operands.
ROW_OPERANDS = {
    "voice_directsound": ((2, "sample"),),
    "voice_directsound_no_resample": ((2, "sample"),),
    "voice_directsound_alt": ((2, "sample"),),
    "voice_programmable_wave": ((2, "wave"),),
    "voice_programmable_wave_alt": ((2, "wave"),),
    "voice_keysplit": ((0, "group"), (1, "keysplit")),
    "voice_keysplit_all": ((0, "group"),),
}
ROW_PLAIN = {"voice_square_1", "voice_square_1_alt", "voice_square_2",
             "voice_square_2_alt", "voice_noise", "voice_noise_alt"}
ROW_ARITY = {  # exact required argument count per macro (all req:)
    "voice_directsound": 7, "voice_directsound_no_resample": 7,
    "voice_directsound_alt": 7, "voice_programmable_wave": 7,
    "voice_programmable_wave_alt": 7, "voice_keysplit": 2,
    "voice_keysplit_all": 1, "voice_square_1": 8, "voice_square_1_alt": 8,
    "voice_square_2": 7, "voice_square_2_alt": 7, "voice_noise": 7,
    "voice_noise_alt": 7,
}


def parse_voicegroup_rows(root, groups, samples, waves, keysplits):
    """Row-counts every voicegroup file (R12-C row pin: 20,594 rows across
    the 195 files) and resolves every symbol operand of every row against
    the sample/wave/voicegroup/keysplit families. Returns symbol ->
    row_count. The row rule is the assembly reality: a line whose first
    token is a row macro name followed by whitespace (labels end with ':',
    directives start with '.', `voice_group` emits a label, not a row)."""
    ks_symbols = {"keysplit_" + name for name in keysplits}
    counts = {}
    all_macros = set(ROW_OPERANDS) | ROW_PLAIN
    for symbol, (inc_path, _back) in groups.items():
        lines = read_lines(root, inc_path)
        n = 0
        for i, line in enumerate(lines):
            m = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)(\s|$)", line)
            if not m:
                continue
            tok = m.group(1)
            if tok.endswith(":") or tok == "voice_group":
                continue
            if tok not in all_macros:
                die(f"{inc_path}:{i + 1}: unknown row token {tok!r}")
            n += 1
            if tok in ROW_PLAIN:
                continue
            args = [a.strip() for a in line[m.end():].split(",")]
            if len(args) != ROW_ARITY[tok]:
                die(f"{inc_path}:{i + 1}: {tok} has {len(args)} arguments, "
                    f"expected {ROW_ARITY[tok]}")
            for idx, role in ROW_OPERANDS[tok]:
                operand = args[idx]
                if operand.isdigit():
                    if role in ("sample", "wave"):
                        die(f"{inc_path}:{i + 1}: {tok} {role} operand is the "
                            f"number {operand}, not a symbol")
                    continue
                family = {"sample": samples, "wave": waves,
                          "group": set(groups), "keysplit": ks_symbols}[role]
                if operand not in family:
                    die(f"{inc_path}:{i + 1}: {tok} references undefined "
                        f"{role} {operand}")
        counts[symbol] = n
    if sum(counts.values()) != PINNED_VOICEGROUP_ROWS:
        die(f"voicegroup rows: expected {PINNED_VOICEGROUP_ROWS} rows across "
            f"{len(counts)} files, found {sum(counts.values())}")
    return counts


def parse_cry_tables(lines, samples):
    """Returns OrderedDict table_symbol -> (kind, row_count) for the two
    tables. Every Cry_* reference must be a defined sample."""
    tables = {}
    current = None
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped == "" or stripped.startswith("@") or stripped.startswith(".align"):
            continue
        m = re.match(r"(gCryTable(?:_Reverse)?)::", stripped)
        if m:
            current = m.group(1)
            if current in tables:
                die(f"{CRY_TABLES}:{i + 1}: duplicate cry table {current}")
            tables[current] = 0
            continue
        m = re.match(r"cry_reverse\s+(Cry_\w+)", stripped) \
            or re.match(r"cry\s+(Cry_\w+)", stripped)
        if m:
            if current is None:
                die(f"{CRY_TABLES}:{i + 1}: cry row before any table label")
            if m.group(1) not in samples:
                die(f"{CRY_TABLES}:{i + 1}: cry row references undefined sample {m.group(1)}")
            tables[current] += 1
    for table, count in tables.items():
        if count != PINNED_CRY_ROWS:
            die(f"{CRY_TABLES}: {table} has {count} rows, expected {PINNED_CRY_ROWS}")
    return tables


def parse_keysplits(lines):
    """Returns OrderedDict name -> (offset, run_bytes): the label back-shift
    (the `keysplit name, N` starting note, in bytes) and the run length
    derived from the `split note, end` spans (each span is `end - prev`,
    starting from the back-shift; a 3-byte table entry per note)."""
    keysplits = {}
    current = None
    prev = None
    run = 0
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped == "" or stripped.startswith("@") \
           or stripped.startswith(".align"):
            continue
        m = re.match(r"keysplit\s+(\w+),\s*(\d+)", stripped)
        if m:
            if current is not None:
                keysplits[current] = (offset, run)
            name = m.group(1)
            if name in keysplits:
                die(f"{KEYSPLIT_TABLES}:{i + 1}: duplicate keysplit {name}")
            current, offset = name, int(m.group(2))
            prev, run = offset, 0
            continue
        m = re.match(r"split\s+(\d+),\s*(\d+)", stripped)
        if not m:
            die(f"{KEYSPLIT_TABLES}:{i + 1}: malformed keysplit row: {stripped!r}")
        if current is None:
            die(f"{KEYSPLIT_TABLES}:{i + 1}: split row before any keysplit")
        end = int(m.group(2))
        if end < prev:
            die(f"{KEYSPLIT_TABLES}:{i + 1}: split ends below the previous note")
        run += end - prev
        prev = end
    if current is not None:
        keysplits[current] = (offset, run)
    if len(keysplits) != PINNED["keysplit"]:
        die(f"keysplit family: expected {PINNED['keysplit']} keysplits, found {len(keysplits)}")
    for name, (offset, run) in keysplits.items():
        if offset != PINNED_KEYSPLIT_BACKS[name]:
            die(f"{KEYSPLIT_TABLES}: {name} back-shift {offset} != pinned {PINNED_KEYSPLIT_BACKS[name]}")
        if run != PINNED_KEYSPLIT_RUNS[name]:
            die(f"{KEYSPLIT_TABLES}: {name} run {run} B != pinned {PINNED_KEYSPLIT_RUNS[name]}")
    total = sum(run for _o, run in keysplits.values())
    if total != PINNED_KEYSPLIT_RUN_BYTES:
        die(f"{KEYSPLIT_TABLES}: runs total {total} B != pinned {PINNED_KEYSPLIT_RUN_BYTES}")
    return keysplits


def check_sample_bijection(root, artifacts):
    """The 544 .incbin artifacts (rel paths sans .bin) must equal the 544
    tracked .aif authoring sources exactly (R12-A sample provenance)."""
    aifs = set()
    for dirpath, _dirnames, filenames in os.walk(os.path.join(root, SAMPLES_DIR)):
        for fn in filenames:
            if fn.endswith(".aif"):
                rel = os.path.relpath(os.path.join(dirpath, fn), os.path.join(root, SAMPLES_DIR))
                aifs.add(rel[:-len(".aif")])
    if set(artifacts) != aifs:
        missing = sorted(set(artifacts) - aifs)
        extra = sorted(aifs - set(artifacts))
        die(f".aif/.bin bijection failed: {len(missing)} artifact(s) without .aif, "
            f"{len(extra)} .aif without artifact"
            + (f" (e.g. {missing[0]})" if missing else "")
            + (f" (e.g. {extra[0]})" if extra else ""))


def collect_sample_pins(root):
    """path (repo-relative) -> sha256 for every tracked .aif, sorted."""
    pins = {}
    base = os.path.join(root, SAMPLES_DIR)
    for dirpath, _dirnames, filenames in os.walk(base):
        for fn in sorted(filenames):
            if fn.endswith(".aif"):
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, root)
                pins[rel] = sha256_file(full)
    return {k: pins[k] for k in sorted(pins)}


# ---------------------------------------------------------------- emission

def emit(rows, pins):
    """rows: list of (kind, key, symbol, artifact, extra_fields dict).
    Deterministic: sorted by key."""
    lines = []
    lines.append("# Generated by tools/gen3_resources/audio_family/gen_audio_inventory.py")
    lines.append("# Do not edit by hand; edit the checked-in sound/ sources and re-run")
    lines.append("# the generator (regeneration must be a no-op diff).")
    lines.append("inventory_version = 1")
    lines.append('family = "audio"')
    lines.append('game = "emerald"')
    lines.append('rom_profile = "bpee01"')
    lines.append("")

    lines.append(f"# Pinned counts (R12 audit): {sum(PINNED.values())} resources "
                 f"= {PINNED['song']} songs + {PINNED['sample/root']} root + "
                 f"{PINNED['sample/phoneme']} phoneme + {PINNED['sample/cry']} cry samples + "
                 f"{PINNED['programmable-wave']} waves + {PINNED['voicegroup']} voicegroups + "
                 f"{PINNED['cry-table']} cry tables + {PINNED['keysplit']} keysplits")
    lines.append(f"# gSongTable rows: {PINNED_TABLE_SONG_ROWS} "
                 f"({PINNED_DUMMY_SONG_ROWS} dummy_song_header aliases are NOT resources; "
                 f"gSongTable/gMPlayTable/dummy_song_header are compiled, not resources)")
    lines.append("")

    for kind in ("song", "sample/root", "sample/phoneme", "sample/cry",
                 "programmable-wave", "voicegroup", "cry-table", "keysplit"):
        lines.append("[[families]]")
        lines.append(f'kind = "{kind}"')
        lines.append(f"symbol_count = {PINNED[kind]}")
        lines.append(f'resource_type = "{CONTRACT[kind][0]}"')
        lines.append(f"schema = {CONTRACT[kind][1]}")
        lines.append(f'representation = "{CONTRACT[kind][2]}"')
        lines.append("")

    for kind, key, symbol, artifact, extra in sorted(rows, key=lambda r: r[1]):
        lines.append("[[resources]]")
        lines.append(f'key = "{key}"')
        lines.append(f'kind = "{kind}"')
        if symbol is not None:
            lines.append(f'symbol = "{symbol}"')
        lines.append(f'source_artifact = "{artifact}"')
        lines.append(f'resource_type = "{CONTRACT[kind][0]}"')
        lines.append(f"schema = {CONTRACT[kind][1]}")
        lines.append(f'representation = "{CONTRACT[kind][2]}"')
        for k, v in sorted(extra.items()):
            if isinstance(v, bool):
                lines.append(f"{k} = {'true' if v else 'false'}")
            elif isinstance(v, int):
                lines.append(f"{k} = {v}")
            else:
                lines.append(f'{k} = "{v}"')
        lines.append("")

    lines.append("[[excluded]]")
    lines.append('symbol = "dummy_song_header"')
    lines.append(f"count = {PINNED_DUMMY_SONG_ROWS}")
    lines.append('reason = "80 gSongTable rows alias the one compiled dummy placeholder header"')
    lines.append("")

    text = "\n".join(lines)

    pin_lines = ["# Generated by tools/gen3_resources/audio_family/gen_audio_inventory.py",
                 "# R12-A canonical .aif provenance pins (the 76 fork-expanded samples were",
                 "# restored to canonical pret blobs in a620dddbc; --check fails on drift).",
                 "# Format: <sha256>  <repo-relative .aif path>", ""]
    pin_lines += [f"{h}  {p}" for p, h in sorted(pins.items())]
    pin_lines.append("")
    pins_text = "\n".join(pin_lines)
    return text, pins_text


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check = "--check" in sys.argv
    root = args[0] if len(args) > 0 else "."
    out_dir = args[1] if len(args) > 1 else \
        "resources/extraction/emerald/bpee01/audio"
    out_path = os.path.join(out_dir, "inventory.generated.toml")
    pins_path = os.path.join(out_dir, "canonical_sample_pins.generated.txt")

    songs, dummy_rows = parse_song_table(read_lines(root, SONG_TABLE))
    samples, artifacts = parse_direct_sound_data(read_lines(root, DIRECT_SOUND_DATA))
    kinds = classify_samples(samples)
    waves = parse_waves(read_lines(root, PROGRAMMABLE_WAVES))
    voicegroups = parse_voicegroups(root)
    cry_tables = parse_cry_tables(read_lines(root, CRY_TABLES), samples)
    keysplits = parse_keysplits(read_lines(root, KEYSPLIT_TABLES))
    check_sample_bijection(root, artifacts)
    voicegroup_rows = parse_voicegroup_rows(root, voicegroups, samples, waves,
                                            keysplits)

    # R12-C back-shift pins: exactly the 10 drumsets back-shift, with the
    # pinned per-name values; every other voicegroup must be unshifted.
    drumset_backs = {}
    for symbol, (_inc_path, back) in voicegroups.items():
        if back == 0:
            continue
        name = symbol[len("voicegroup_"):]
        drumset_backs[name] = back
    if drumset_backs != PINNED_DRUMSET_BACKS:
        die(f"drumset back-shifts {drumset_backs} != pinned {PINNED_DRUMSET_BACKS}")

    # Per-song structure (header track count + voicegroup reference).
    song_files = {}
    song_voicegroups = {}
    for name in songs:
        tracks, vg = parse_song_file(root, name)
        song_files[name] = tracks
        song_voicegroups[name] = vg
        if vg != "voicegroup_dummy" and vg not in voicegroups:
            die(f"song {name}: references undefined voicegroup {vg}")

    # Sample subtype counts.
    for kind in ("sample/root", "sample/phoneme", "sample/cry"):
        count = sum(1 for k in kinds.values() if k == kind)
        if count != PINNED[kind]:
            die(f"{kind}: expected {PINNED[kind]}, found {count}")

    rows = []
    keys = {}
    for name, (ms, me) in songs.items():
        key = CONTRACT["song"][3].format(canonical(name))
        rows.append(("song", key, name,
                     song_file_path(root, name),
                     {"track_count": song_files[name],
                      "voicegroup": song_voicegroups[name].replace("voicegroup_", "", 1),
                      "ms": ms, "me": me}))
    for sym, kind in kinds.items():
        if kind == "sample/root":
            canon = canonical(sym[len("DirectSoundWaveData_"):])
            key = CONTRACT[kind][3].format(canon)
        elif kind == "sample/phoneme":
            canon = sym[len("DirectSoundWaveData_Phoneme_"):]
            key = CONTRACT[kind][3].format(canon)
        else:  # sample/cry
            canon = canonical(sym[len("Cry_"):])
            key = CONTRACT[kind][3].format(canon)
        rows.append((kind, key, sym, samples[sym], {}))
    for sym in waves:
        num = sym[len("ProgrammableWaveData_"):]
        key = CONTRACT["programmable-wave"][3].format(num)
        rows.append(("programmable-wave", key, sym, waves[sym], {"number": int(num)}))
    for sym in voicegroups:
        name = sym[len("voicegroup_"):]
        inc_path, back = voicegroups[sym]
        key = CONTRACT["voicegroup"][3].format(canonical(name))
        rows.append(("voicegroup", key, sym, inc_path,
                     {"row_count": voicegroup_rows[sym], "back": back}))
    rows.append(("cry-table", "emerald:audio/cry-table/forward", "gCryTable",
                 CRY_TABLES, {"row_count": PINNED_CRY_ROWS}))
    rows.append(("cry-table", "emerald:audio/cry-table/reverse", "gCryTable_Reverse",
                 CRY_TABLES, {"row_count": PINNED_CRY_ROWS}))
    for name, (offset, run) in keysplits.items():
        key = CONTRACT["keysplit"][3].format(canonical(name))
        rows.append(("keysplit", key, "keysplit_" + name, KEYSPLIT_TABLES,
                     {"offset": offset, "run_bytes": run}))

    # Duplicate-key detection (one canonical identity per audio object).
    for _kind, key, _sym, _art, _extra in rows:
        if key in keys:
            die(f"duplicate resource key {key}")
        keys[key] = True
    if len(keys) != PINNED_KEYS:
        die(f"expected {PINNED_KEYS} resource keys, found {len(keys)}")

    pins = collect_sample_pins(root)
    if len(pins) != PINNED["sample/root"] + PINNED["sample/phoneme"] + PINNED["sample/cry"]:
        die(f"expected {PINNED_KEYS - PINNED['song'] - PINNED['programmable-wave'] - PINNED['voicegroup'] - PINNED['cry-table'] - PINNED['keysplit']} .aif pins, found {len(pins)}")
    text, pins_text = emit(rows, pins)

    if check:
        for path, expected in ((out_path, text), (pins_path, pins_text)):
            try:
                existing = open(path).read()
            except FileNotFoundError:
                die(f"--check: {path} does not exist")
            if existing != expected:
                die(f"--check: {path} is not up to date with the sources")
        print(f"{out_path}: up to date ({len(keys)} resources, {len(pins)} sample pins)")
        return 0
    os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "w") as f:
        f.write(text)
    with open(pins_path, "w") as f:
        f.write(pins_text)
    print(f"{out_path}: wrote {len(keys)} resources, {len(pins)} sample pins")
    return 0


def song_file_path(root, name):
    if os.path.isfile(os.path.join(root, SONGS_DIR, name + ".s")):
        return SONGS_DIR + "/" + name + ".s"
    return SONGS_MIDI_DIR + "/" + name + ".s"


if __name__ == "__main__":
    sys.exit(main())

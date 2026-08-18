#!/usr/bin/env python3
"""R12-B/C/D: Emerald audio extraction bindings generator.

Reads the authoritative R12-A inventory
(resources/extraction/emerald/bpee01/audio/inventory.generated.toml) and
emits the full audio family as a production catalog and semantic extraction
bindings for the R1A manifest pipeline:

  * audio/catalog.generated.toml   - [[resources]] id/type/schema
  * audio/bindings.generated.toml  - [[bindings]] id/symbol/artifact/raw/
                                     representation/exact size

Three resource classes:

  * 569 leaves (R12-B): 105 root + 51 phoneme + 388 cry samples, 25
    programmable waves - the artifact IS the canonical payload (the checked
    in `.bin` the ROM incbins).
  * 202 structural (R12-C): 195 voicegroups (20,594 GBA 12-byte ToneData
    rows) + 2 cry tables (388 rows each) + 5 keysplit runs (372 B). The
    canonical payload is the GBA-form ROM slice; since the native (LINUX64)
    row width differs (24 B), the GBA-form artifact is synthesized into
    audio/structural/*.bin (gitignored; consumed only by the manifest's
    Guardrail 10, the pack slices the ROM directly).
  * 530 songs (R12-D): MP2K song graphs, 684,052 B. Slice derivation is
    the packed-object rule (8+4*trackCount per object, previous header end
    == next object start, 0 mismatches over 7,740 track labels); every
    graph is validated by the engine-faithful stream walker
    (song_walker.py: running status, command widths, pattern stack, XCMD
    subs, pinned totals). The GBA-form payload is synthesized into
    audio/songs/<canonical>.bin (gitignored; consumed only by the
    manifest's Guardrail 10, the pack slices the ROM directly).

This stage performs the structural size derivation and the three-way
provenance proof BEFORE the manifest tooling (which re-proves Guardrail 10
artifact == ELF == ROM):

  * Assembly symbols have st_size == 0 (audit 5) and are NOT trusted.
  * The exact sample/wave size is derived from the object layout: the gap
    from the symbol to the NEXT symbol in the same section. Every leaf in
    the .inc sources is preceded by `.align 2` (4-byte label alignment), so
    gap == align4(artifact_size) must hold exactly (padding 0..3); the
    derived size is the artifact size, never the padded gap.
  * WaveData2 header (16 B: 3 compression flags, loopFlags, u32 freq,
    u32 loopStart, u32 size) is validated structurally:
      - uncompressed (root/phoneme): file == 16 + size + 1 (size stores
        sampleCount - 1; aif2pcm main.c:613-621)
      - compressed (cries): size stores decoded sampleCount - 1; the file
        is the delta-compressed extent; loopStart must lie within
        [0, size + 1)
      - freq must be nonzero
  * Programmable waves: exactly 16 B each (32 x 4-bit CGB wave RAM).
  * Structural (R12-C): the stream model (sound/voice_groups.inc include
    order; 12 B/row) reproduces the R12-C verified model - 21,370 rows =
    20,594 voicegroup + 776 cry, every label GBA address resolved against
    the pret ELF (exactly one symbol at each of 193/197 labels; the 4
    no-symbol labels bind via their pinned containing symbol + offset),
    drumset back-shifts (10 x 36 rows, route110 40) via `symbol_offset`,
    keysplit runs (5, 372 B total) verbatim, duplicate logical GBA starts
    refused, and every pointer field of every row resolved against the
    leaf/label address sets.

Any mismatch (gap, header field, ELF slice, ROM slice, stream model) is a
hard failure - the bindings are never emitted with an unproven size.

R12-E additionally emits the committed `sound/song_table_native.generated.inc`
(no-op-diff --check): the native gSongTable rows re-emitted with ROM logical
addresses (row 0 == 0x088FC03C, last real row + 8+4*trackCount == 0x089A3050,
530 real / 80 dummy, dummy rows keep `.int dummy_song_header`) so the live
MP2K consumers resolve into the audio arena instead of direct-casting to
compiled bytes.

Usage:
  python3 gen_audio_bindings.py [repo-root] [elf-path] [rom-path] [out-dir]
                                [--check]

The inventory source-of-truth requirement: this generator adds NO discovery
of its own - every resource id/symbol/artifact/type/schema/representation
comes from the R12-A inventory; only sizes and slices are derived here.
"""
import argparse
import re
import struct
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import song_walker

GEN3_GBA_ROM_BASE = 0x08000000

LEAF_KINDS = ("sample/root", "sample/phoneme", "sample/cry", "programmable-wave")
PINNED = {"sample/root": 105, "sample/phoneme": 51,
          "sample/cry": 388, "programmable-wave": 25}
PINNED_TOTAL = 569

STRUCTURAL_KINDS = ("voicegroup", "cry-table", "keysplit")
PINNED_STRUCT = {"voicegroup": 195, "cry-table": 2, "keysplit": 5}
PINNED_STRUCT_TOTAL = 202
PINNED_VOICEGROUP_ROWS = 20594
PINNED_CRY_ROWS = 388
PINNED_STREAM_ROWS = PINNED_VOICEGROUP_ROWS + 2 * PINNED_CRY_ROWS  # 21370
PINNED_KEYSPLIT_RUN_BYTES = 372

# The 4 voicegroup labels with NO coinciding pret symbol (R12-C stream
# model, verified against the native .o and the pret ELF): the binding is
# the containing pret symbol plus the byte offset of the label inside it.
NO_SYMBOL_GROUPS = {
    "unused": ("voicegroup007", 36),
    "unused_2": ("voicegroup130", 612),
    "rg_unused": ("voicegroup174", 1536),
    "rg_unused_2": ("voicegroup174", 1836),
}

# ------------------------------------------------------------ R12-D songs

# The 530-song MP2K block: one contiguous ELF section
# [0x088FC03C, 0x089A3050) = 684,052 B, anchored by the R12-B leaf tail
# (last leaf ends 0x088FC03A, `.align 2`) and the Sio32 data that begins
# at 0x089A3050. Verified against the qualified reference ELF.
SONG_BLOCK_START = 0x088FC03C
SONG_BLOCK_END = 0x089A3050
SONG_TABLE = "sound/song_table.inc"


def parse_song_table(root):
    """gSongTable rows in .inc order. Returns (real_rows, dummy_rows):
    (symbol, ms, me, table_index) for the 530 real rows and the table
    indices of the 80 `dummy_song_header` alias rows. The compiled
    `dummy_song_header:` definition (4 zero bytes after the table) is
    not a row. Table index counts every `song` row (the dummy block
    sits mid-table: indices 270..349), so it must be tracked explicitly,
    not derived from the real-row position."""
    rows = []
    dummy = []
    idx = 0
    for line in open(root / SONG_TABLE, encoding="utf-8"):
        m = re.match(r"\s*song\s+(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*$", line)
        if not m:
            continue
        name, ms, me = m.group(1), int(m.group(2)), int(m.group(3))
        if name == "dummy_song_header":
            dummy.append(idx)
        else:
            rows.append((name, ms, me, idx))
        idx += 1
    if len(rows) != 530:
        fail(f"{SONG_TABLE}: {len(rows)} real rows != 530")
    if len(dummy) != 80:
        fail(f"{SONG_TABLE}: {len(dummy)} dummy rows != 80")
    return rows, dummy


def resolve_songs(root, elf, rom, inventory, vg_addrs, outdir, args):
    """Derives and validates the 530 song graphs. Slice derivation is the
    R12-D packed-object rule: size = 8 + 4*trackCount (trackCount = ROM
    byte 0 of the header), object start = previous object's header end,
    the block [0x088FC03C, 0x089A3050) partitions exactly. Validates:
    the label gate (530 global headers + 7,740 local track labels, every
    local label inside its song's stream region, exactly one label at
    every object start), the header fields (tone in the 197-label set,
    parts interior/ascending/first == slice start), the engine-faithful
    sweep + control-flow walk of every track (song_walker), the pinned
    aggregate counts, and the gSongTable rows against the ROM table.
    Returns [(key, kind, symbol, artifact, type, schema, rep, size,
    rom_off, symbol_offset)] sorted by key, with the canonical payload
    synthesized into outdir/songs/<canonical>.bin (gitignored; the
    manifest's Guardrail 10 re-proves artifact == ELF == ROM)."""
    songs = [r for r in inventory.values() if r["kind"] == "song"]
    if len(songs) != 530:
        fail(f"song count {len(songs)} != 530")
    kinds = {}
    for r in songs:
        kinds[r["symbol"][:3]] = kinds.get(r["symbol"][:3], 0) + 1
    if kinds != {"mus": 210, "se_": 269, "ph_": 51}:
        fail(f"song family counts {kinds} != mus 210 / se 269 / ph 51")

    by_addr = []
    for r in songs:
        sym = elf.find(r["symbol"])
        if sym is None:
            fail(f"song symbol {r['symbol']} not in the ELF")
        by_addr.append((sym[1], r, sym))
    by_addr.sort(key=lambda t: t[0])

    # gSongTable row cross-check (the plan's row-by-row verification,
    # re-proven against the retail ROM table here)
    gs = elf.find("gSongTable")
    if gs is None:
        fail("gSongTable not in the ELF")
    table_base = gs[1]
    table_rows, dummy_rows = parse_song_table(root)
    if len(table_rows) != len(by_addr):
        fail(f"song table rows {len(table_rows)} != song symbols {len(by_addr)}")
    dummy_addr = elf.find("dummy_song_header")
    if dummy_addr is None:
        fail("dummy_song_header not in the ELF")
    dummy_addr = dummy_addr[1]
    for (addr, r, _s), (name, ms, me, table_idx) in zip(by_addr, table_rows):
        if r["symbol"] != name:
            fail(f"gSongTable row {table_idx}: table order {name} != inventory "
                 f"{r['symbol']}")
        # table_idx is the row's position in the full table (the 80 dummy
        # rows sit mid-table, so the real-row position is NOT the offset)
        row_off = table_base - GEN3_GBA_ROM_BASE + table_idx * 8
        hdr, rms, rme = struct.unpack_from("<IHH", rom, row_off)
        if hdr != addr:
            fail(f"gSongTable row {table_idx} ({name}): header {hdr:#x} != "
                 f"symbol {addr:#x}")
        if rms != ms or rme != me:
            fail(f"gSongTable row {table_idx} ({name}): ms/me {rms}/{rme} != "
                 f".inc {ms}/{me}")
        if r.get("ms") != ms or r.get("me") != me:
            fail(f"inventory {r['key']}: ms/me ({r.get('ms')}/{r.get('me')}) "
                 f"!= gSongTable ({ms}/{me})")
    # the 80 dummy rows hold dummy_song_header, all four zero bytes
    for idx in dummy_rows:
        hdr = struct.unpack_from("<I", rom,
                                 table_base - GEN3_GBA_ROM_BASE + idx * 8)[0]
        if hdr != dummy_addr:
            fail(f"gSongTable dummy row {idx}: {hdr:#x} != dummy_song_header "
                 f"{dummy_addr:#x}")

    # --- packed-object slice derivation ---
    slices = []  # (start, header_addr, end, size, tc, r, sym)
    for i, (addr, r, sym) in enumerate(by_addr):
        if addr < SONG_BLOCK_START or addr >= SONG_BLOCK_END:
            fail(f"{r['key']}: header {addr:#x} outside the song block")
        if i > 0 and addr <= by_addr[i - 1][0]:
            fail(f"{r['key']}: headers not strictly increasing")
        tc = rom[addr - GEN3_GBA_ROM_BASE]
        if r.get("track_count") != tc:
            fail(f"inventory {r['key']}: track_count {r.get('track_count')} "
                 f"!= header byte 0 ({tc})")
        if r.get("resource_type") != "music-sequence" or r.get("schema") != 1 \
                or r.get("representation") != "gba-mp2k-song-graph":
            fail(f"inventory {r['key']}: unexpected type/schema/representation")
        size = 8 + 4 * tc
        slices.append((None, addr, addr + size, size, tc, r, sym))
    for i in range(len(slices)):
        start = SONG_BLOCK_START if i == 0 else slices[i - 1][2]
        slices[i] = (start,) + slices[i][1:]
    if slices[0][0] != slices[0][1]:
        fail(f"first song {slices[0][5]['key']}: slice start "
             f"{slices[0][0]:#x} != header {slices[0][1]:#x}")
    if slices[-1][2] != SONG_BLOCK_END:
        fail(f"last song end {slices[-1][2]:#x} != block end "
             f"{SONG_BLOCK_END:#x}")
    # the packed-object rule makes the slices contiguous by construction,
    # so the block byte count is the span: first start to last end
    block_bytes = slices[-1][2] - slices[0][0]
    if block_bytes != song_walker.PIN_TOTAL_BYTES:
        fail(f"song block bytes {block_bytes} != pinned "
             f"{song_walker.PIN_TOTAL_BYTES}")

    # --- label gate: 530 global headers + 7,740 local track labels ---
    # STT_NOTYPE only: the section's STT_SECTION symbol (empty name,
    # value = section start) sits at 0x088FC03C and is not a label.
    block_syms = [s for s in elf.symbols
                  if not s[0].startswith("$") and s[4] != 0
                  and (s[3] & 0xF) == 0  # STT_NOTYPE
                  and SONG_BLOCK_START <= s[1] < SONG_BLOCK_END]
    globals_ = [s for s in block_syms if (s[3] >> 4) & 0xF == 1]
    locals_ = [s for s in block_syms if (s[3] >> 4) & 0xF == 0]
    if len(globals_) != 530:
        fail(f"song block: {len(globals_)} global symbols != 530")
    if len(locals_) != song_walker.PIN_LOCAL_LABELS:
        fail(f"song block: {len(locals_)} local labels != "
             f"{song_walker.PIN_LOCAL_LABELS}")
    for s in globals_:
        if not any(s[1] == sl[1] for sl in slices):
            fail(f"song block: unexpected global symbol {s[0]} at {s[1]:#x}")
    first_labels = {}
    for start, hdr, _end, _size, tc, r, _sym in slices:
        at = [s for s in locals_ if s[1] == start]
        if tc == 0:
            if at:
                fail(f"{r['key']}: zero-track song has local labels at its "
                     f"start {start:#x}")
            first_labels[r["key"]] = r["symbol"]
        else:
            if len(at) != 1:
                fail(f"{r['key']}: {len(at)} local labels at object start "
                     f"{start:#x} != 1 (packed-object invariant)")
            first_labels[r["key"]] = at[0][0]
    # membership: assign every local label to exactly one song stream
    # region [start, header_addr); a label inside a header region or a
    # header gap would resolve to zero regions
    for s in locals_:
        hit = [sl for sl in slices if sl[0] <= s[1] < sl[1]]
        if len(hit) != 1:
            fail(f"local label {s[0]} at {s[1]:#x} resolves to {len(hit)} "
                 f"song stream regions")

    # --- header fields + walker per song ---
    wave_addrs = set()
    for r in inventory.values():
        if r["kind"] == "programmable-wave":
            s = elf.find(r["symbol"])
            if s is None:
                fail(f"wave symbol {r['symbol']} not in the ELF")
            wave_addrs.add(s[1])
    if len(wave_addrs) != 25:
        fail(f"programmable wave addresses {len(wave_addrs)} != 25")

    rows = []
    agg_sweep = {k: 0 for k in ("wait", "note", "fine", "goto", "patt",
                                "pend", "rept", "memacc", "memacc_jump",
                                "xcmd", "xwave", "onebyte", "port", "endtie")}
    agg_xcmd_sub = {}
    all_targets = set()
    all_tones = set()
    for start, hdr, end_, size, tc, r, sym in slices:
        key = r["key"]
        tone = struct.unpack_from("<I", rom, hdr - GEN3_GBA_ROM_BASE + 4)[0]
        parts = struct.unpack_from("<" + "I" * tc, rom,
                                   hdr - GEN3_GBA_ROM_BASE + 8)
        sweep, walk, targets = song_walker.validate_song(
            rom, key, start, end_, hdr, list(parts), tone, vg_addrs,
            wave_addrs)
        # tone == the song's declared voicegroup label (the .equ value
        # the header `.4byte` row holds)
        vg_label = "voicegroup_" + r["voicegroup"]
        if tone not in vg_addrs:
            fail(f"{key}: tone {tone:#x} not in the voicegroup label set")
        for k in agg_sweep:
            agg_sweep[k] += sweep[k]
        for sub, n in sweep["xcmd_sub"].items():
            agg_xcmd_sub[sub] = agg_xcmd_sub.get(sub, 0) + n
        all_targets |= targets
        all_tones.add(tone)
        if len(targets) and any(not (start <= t < hdr) for t in targets):
            fail(f"{key}: an address operand resolved outside the song")
        # three-way proof: ELF slice == ROM slice (the artifact below is
        # synthesized from the ROM; the manifest re-proves it)
        first = first_labels[key]
        # the object is the WHOLE packed slice [start, end_): tracks +
        # header + parts array. size (8+4N) is just the header region.
        obj_size = end_ - start
        if elf.slice(first, obj_size) != rom[start - GEN3_GBA_ROM_BASE:
                                             start - GEN3_GBA_ROM_BASE
                                             + obj_size]:
            fail(f"{key}: ELF slice at '{first}' differs from the ROM slice")
        # canonical payload artifact (gitignored, consumed by Guardrail 10)
        data = rom[start - GEN3_GBA_ROM_BASE:start - GEN3_GBA_ROM_BASE
                   + obj_size]
        art = outdir / "songs" / (key[len("emerald:audio/song/"):] + ".bin")
        if args.check:
            if not art.exists() or art.read_bytes() != data:
                fail(f"--check: {art} differs from deterministic ROM-slice "
                     f"regeneration")
        else:
            art.parent.mkdir(parents=True, exist_ok=True)
            art.write_bytes(data)
        rows.append((key, "song", first, str(art), "music-sequence", 1,
                     "gba-mp2k-song-graph", obj_size,
                     start - GEN3_GBA_ROM_BASE, None))
    rows.sort(key=lambda r: r[0])

    # --- pinned aggregate gates (the R12-D stage gate) ---
    def pin(v, name):
        if v != getattr(song_walker, name):
            fail(f"song walker: {name} {v} != pinned "
                 f"{getattr(song_walker, name)}")

    pin(agg_sweep["goto"], "PIN_GOTO")
    pin(agg_sweep["patt"], "PIN_PATT")
    pin(agg_sweep["rept"], "PIN_REPT")
    pin(agg_sweep["memacc_jump"], "PIN_MEMACC_JUMP")
    pin(agg_sweep["xwave"], "PIN_XWAVE")
    operands = (agg_sweep["goto"] + agg_sweep["patt"] + agg_sweep["rept"]
                + agg_sweep["memacc_jump"] + agg_sweep["xwave"])
    pin(operands, "PIN_OPERANDS")
    pin(agg_sweep["memacc"], "PIN_MEMACC")
    pin(agg_sweep["xcmd"], "PIN_XCMD")
    if agg_xcmd_sub != song_walker.PIN_XCMD_SUBS:
        fail(f"song walker: XCMD sub histogram {agg_xcmd_sub} != pinned "
             f"{song_walker.PIN_XCMD_SUBS}")
    pin(agg_sweep["note"], "PIN_NOTES")
    pin(sum(s[4] for s in slices), "PIN_PARTS")
    pin(len(all_tones), "PIN_DISTINCT_TONES")
    pin(len(all_targets), "PIN_DISTINCT_TARGETS")
    return rows, agg_sweep, agg_xcmd_sub, all_targets, all_tones


def emit_native_song_table(root, elf, rom, args):
    """R12-E: the native gSongTable rows must hold ROM logical addresses,
    not native link addresses (the `song` macro's `.int \label` emits the
    link address, so every HostResolveGbaAddr lookup misses the ROM-identity
    tables and direct-casts to compiled bytes). Re-emits the 610-row table
    in song_table.inc order: 530 real rows `.int <retail ROM SongHeader
    GbaAddr>` (row-by-row against the qualified ROM gSongTable, pinned row 0
    == SONG_BLOCK_START and last real row + 8+4*trackCount == SONG_BLOCK_END,
    headers strictly increasing), 80 dummy rows `.int dummy_song_header`
    (pinned == the ELF dummy_song_header address, ms/me 0/0), `.short ms, me`
    unchanged, the trailing `dummy_song_header:` definition the references
    need, and an assembly-time row-count pin. Returns the file lines; the
    caller writes them via write_if (byte-for-byte --check)."""
    gs = elf.find("gSongTable")
    if gs is None:
        fail("gSongTable not in the ELF")
    table_base = gs[1]
    dummy_addr = elf.find("dummy_song_header")
    if dummy_addr is None:
        fail("dummy_song_header not in the ELF")
    dummy_addr = dummy_addr[1]
    table_rows, dummy_rows = parse_song_table(root)
    if len(table_rows) != 530 or len(dummy_rows) != 80 \
            or len(table_rows) + len(dummy_rows) != 610:
        fail(f"gSongTable composition {len(table_rows)} real + "
             f"{len(dummy_rows)} dummy != 530 + 80")

    rows = [None] * 610  # (name, ms, me, hdr) per table index
    for name, ms, me, idx in table_rows:
        rows[idx] = (name, ms, me, None)
    for idx in dummy_rows:
        rows[idx] = ("dummy_song_header", 0, 0, None)
    if any(r is None for r in rows):
        fail("gSongTable: a table index is neither real nor dummy")

    rom_base = table_base - GEN3_GBA_ROM_BASE
    prev_hdr = None
    for idx, (name, ms, me, _) in enumerate(rows):
        hdr, rms, rme = struct.unpack_from("<IHH", rom, rom_base + idx * 8)
        if name == "dummy_song_header":
            if hdr != dummy_addr:
                fail(f"gSongTable dummy row {idx}: {hdr:#x} != "
                     f"dummy_song_header {dummy_addr:#x}")
            if rms != 0 or rme != 0:
                fail(f"gSongTable dummy row {idx}: ms/me {rms}/{rme} != 0/0")
        else:
            if not SONG_BLOCK_START <= hdr < SONG_BLOCK_END:
                fail(f"gSongTable row {idx} ({name}): header {hdr:#x} "
                     f"outside the song block")
            if rms != ms or rme != me:
                fail(f"gSongTable row {idx} ({name}): ms/me {rms}/{rme} != "
                     f".inc {ms}/{me}")
            if prev_hdr is not None and hdr <= prev_hdr:
                fail(f"gSongTable row {idx} ({name}): header {hdr:#x} not "
                     f"strictly after {prev_hdr:#x}")
            prev_hdr = hdr
        rows[idx] = (name, ms, me, hdr)
    if rows[0][3] != SONG_BLOCK_START:
        fail(f"gSongTable row 0 header {rows[0][3]:#x} != block start "
             f"{SONG_BLOCK_START:#x}")
    if rows[-1][0] == "dummy_song_header":
        fail("gSongTable: the last row must be a real song")
    last_hdr = rows[-1][3]
    tc = rom[last_hdr - GEN3_GBA_ROM_BASE]
    if last_hdr + 8 + 4 * tc != SONG_BLOCK_END:
        fail(f"gSongTable last row {rows[-1][0]}: header + 8+4*{tc} = "
             f"{last_hdr + 8 + 4 * tc:#x} != block end {SONG_BLOCK_END:#x}")

    lines = [
        "/* Generated by tools/gen3_resources/audio_family/gen_audio_bindings.py.",
        "   Do not edit by hand; re-run the generator (regeneration must be a",
        "   no-op diff). R12-E: the native gSongTable rows hold ROM logical",
        "   addresses (the retail gSongTable, verified row-by-row), NOT native",
        "   link addresses, so every HostResolveGbaAddr lookup resolves into",
        "   the audio arena. 610 rows: 530 real + 80 dummy. */",
        "\t.align 2",
        "",
        "gSongTable::",
    ]
    for name, ms, me, hdr in rows:
        if name == "dummy_song_header":
            lines.append("\t.int dummy_song_header")
        else:
            lines.append(f"\t.int 0x{hdr:08X} /* {name} */")
        lines.append(f"\t.short {ms}")
        lines.append(f"\t.short {me}")
    lines += [
        "gSongTable_end:",
        ".if (gSongTable_end - gSongTable) != 610 * 8",
        "\t.error \"gSongTable must have exactly 610 rows of 8 bytes\"",
        ".endif",
        "",
        "\t.align 2",
        "dummy_song_header:",
        "\t.byte 0, 0, 0, 0",
        "",
    ]
    return lines


# GBA-form ToneData row (12 B, asm/macros/music_voice.inc) pointer layout.
# Type byte 0: sample/wave/group pointers at bytes 4-8 (.int); keysplit rows
# additionally hold the keysplit table at bytes 8-12; squares/noise hold
# duty/period in bytes 4-8 (no pointer).
PTR_SAMPLE = {0x00, 0x08, 0x10, 0x20, 0x30}
PTR_WAVE = {0x03, 0x0B}
PTR_GROUP = {0x40, 0x80}
PTR_KEYSPLIT = {0x40}
NO_PTR = {0x01, 0x09, 0x02, 0x0A, 0x04, 0x0C}
ALL_TYPES = PTR_SAMPLE | PTR_WAVE | PTR_GROUP | NO_PTR


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


class Elf32:
    """Minimal ELF32 symtab reader (ARM little-endian linker output)."""

    def __init__(self, data):
        if len(data) < 52 or data[0:4] != b"\x7fELF":
            fail("not an ELF file")
        if data[4] != 1 or data[5] != 1:
            fail("not ELFCLASS32 little-endian")
        self.data = data
        shoff = struct.unpack_from("<I", data, 0x20)[0]
        shentsize = struct.unpack_from("<H", data, 0x2E)[0]
        shnum = struct.unpack_from("<H", data, 0x30)[0]
        self.sections = []
        for i in range(shnum):
            off = shoff + i * shentsize
            (sname, stype, sflags, saddr, soff, ssize, slink, sinfo,
             salign, sent) = struct.unpack_from("<IIIIIIIIII", data, off)
            self.sections.append(dict(type=stype, flags=sflags, addr=saddr,
                                      offset=soff, size=ssize, link=slink,
                                      align=salign))
        # The symtab's link field points at its strtab section.
        self.symtab = None
        for i, s in enumerate(self.sections):
            if s["type"] == 2:  # SHT_SYMTAB
                self.symtab = (i, s)
                break
        if self.symtab is None:
            fail("ELF has no SHT_SYMTAB")
        _, stab = self.symtab
        strtab = self.sections[stab["link"]]
        self.strtab = data[strtab["offset"]:strtab["offset"] + strtab["size"]]
        self.symbols = []  # (name, value, size, info, shndx)
        entsize = 16
        for off in range(stab["offset"], stab["offset"] + stab["size"], entsize):
            (name_off, value, size, info, other, shndx) = struct.unpack_from(
                "<IIIBBH", data, off)
            end = self.strtab.find(b"\0", name_off)
            name = self.strtab[name_off:end].decode("ascii", "replace")
            self.symbols.append((name, value, size, info, shndx))

    def find(self, name):
        for sym in self.symbols:
            if sym[0] == name:
                return sym
        return None

    def next_gap(self, name):
        """Gap from `name` to the next higher-value symbol in the same
        section; for the section's last symbol, gap to the section end.
        Returns (gap, section) or fails when the symbol is absent or not
        allocated."""
        sym = self.find(name)
        if sym is None:
            fail(f"symbol '{name}' not found in the ELF")
        _, value, size, info, shndx = sym
        if shndx == 0 or shndx >= len(self.sections):
            fail(f"symbol '{name}' has no containing section")
        if size != 0:
            fail(f"symbol '{name}' unexpectedly has st_size {size} (asm labels must be 0)")
        sect = self.sections[shndx]
        if not (sect["flags"] & 0x2):  # SHF_ALLOC
            fail(f"symbol '{name}' section is not SHF_ALLOC")
        nxt = sect["addr"] + sect["size"]  # section end by default
        for s in self.symbols:
            if s[4] == shndx and s[1] > value and s[1] < nxt:
                nxt = s[1]
        return nxt - value, sect

    def slice(self, name, size):
        """File bytes backing [value, value+size)."""
        sym = self.find(name)
        _, value, _, _, shndx = sym
        sect = self.sections[shndx]
        off = sect["offset"] + (value - sect["addr"])
        return self.data[off:off + size]

    def slice_at(self, name, offset, size):
        """File bytes backing [value+offset, value+offset+size) (the
        manifest's `symbol_offset` slice path)."""
        sym = self.find(name)
        _, value, _, _, shndx = sym
        sect = self.sections[shndx]
        off = sect["offset"] + (value - sect["addr"]) + offset
        return self.data[off:off + size]


def parse_inventory(path):
    with open(path, "rb") as f:
        doc = tomllib.load(f)
    out = {}
    for r in doc.get("resources", []):
        key = r["key"]
        if key in out:
            fail(f"duplicate inventory key {key}")
        out[key] = r
    return out


def validate_wavedata(b, name, kind, size):
    """Structural WaveData2 header validation; fails on any violation."""
    if len(b) != size:
        fail(f"{name}: artifact length {len(b)} != derived size {size}")
    if size < 16:
        fail(f"{name}: payload shorter than the 16-byte WaveData2 header")
    cf = b[0] | b[1] | b[2]
    freq, loop_start, hdr_size = struct.unpack_from("<III", b, 4)
    if freq == 0:
        fail(f"{name}: WaveData freq field is zero")
    sample_count = hdr_size + 1  # size field stores sampleCount - 1
    if loop_start > sample_count:
        fail(f"{name}: loopStart {loop_start} out of bounds (sampleCount {sample_count})")
    if cf == 0:
        # uncompressed PCM: the artifact is 16 + sampleCount bytes
        if size != 16 + sample_count:
            fail(f"{name}: uncompressed artifact {size} != 16 + size-field+1 ({16 + sample_count})")
    else:
        if kind != "sample/cry":
            fail(f"{name}: compression flags set on a non-cry sample")
        # compressed cry: header size is the DECODED count - 1; the file is
        # the delta-compressed extent (cannot be derived from the header).
        if size <= 16:
            fail(f"{name}: compressed cry payload must exceed the header")


# ------------------------------------------------------------ R12-C stream model

ROW_TOKEN_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)(\s|$)")
NONROW = {"voice_group", "keysplit"}   # label emitters, not rows
VOICE_GROUPS_INC = "sound/voice_groups.inc"
CRY_TABLES_INC = "sound/cry_tables.inc"
KEYSPLIT_TABLES_INC = "sound/keysplit_tables.inc"


def count_rows(path):
    """Rows in a structural .inc: a line whose first token is an identifier
    followed by whitespace, minus label lines (token ends with ':'), minus
    the `voice_group`/`keysplit` label emitters. Verified against the native
    .o: the recomp sound_data.o byte counts match this rule exactly."""
    n = 0
    for line in open(path, encoding="utf-8"):
        m = ROW_TOKEN_RE.match(line)
        if not m:
            continue
        tok = m.group(1)
        if tok.endswith(":") or tok in NONROW:
            continue
        n += 1
    return n


def parse_stream(root):
    """(label, back_rows, rows, inc_path) entries in voice_groups.inc stream
    order; the cry_tables.inc include splits into its two 388-row tables.
    Returns (entries, total_rows)."""
    entries = []
    pos = 0
    for line in open(root / VOICE_GROUPS_INC, encoding="utf-8"):
        m = re.match(r'\s*\.include\s+"sound/([^"]+)"', line)
        if not m:
            continue
        inc = m.group(1)  # relative to sound/
        if inc == CRY_TABLES_INC[len("sound/"):]:
            entries.append(("gCryTable", 0, PINNED_CRY_ROWS, inc))
            pos += PINNED_CRY_ROWS
            entries.append(("gCryTable_Reverse", 0, PINNED_CRY_ROWS, inc))
            pos += PINNED_CRY_ROWS
            continue
        path = root / "sound" / inc
        vg = None
        for l in open(path, encoding="utf-8"):
            mm = re.match(r"\s*voice_group\s+([A-Za-z0-9_]+)(?:\s*,\s*(\d+))?\s*$", l)
            if mm:
                vg = (mm.group(1), int(mm.group(2) or 0))
                break
        if vg is None:
            fail(f"stream model: no `voice_group` macro in {inc}")
        n = count_rows(path)
        entries.append((vg[0], vg[1], n, inc))
        pos += n
    return entries, pos


def parse_keysplits_file(root):
    """(name, back_bytes, run_bytes) in keysplit_tables.inc order: `keysplit
    name, N` back-shifts the label N bytes (the starting note), `split
    note, end` spans sum to the run length (3-byte table entries)."""
    out = []
    cur = None
    prev = run = 0
    for line in open(root / KEYSPLIT_TABLES_INC, encoding="utf-8"):
        m = re.match(r"\s*keysplit\s+([A-Za-z0-9_]+)\s*,\s*(\d+)", line)
        if m:
            if cur is not None:
                out.append((cur, offset, run))
            cur, offset = m.group(1), int(m.group(2))
            prev, run = offset, 0
            continue
        m = re.match(r"\s*split\s+\d+\s*,\s*(\d+)", line)
        if m:
            end = int(m.group(1))
            run += end - prev
            prev = end
    if cur is not None:
        out.append((cur, offset, run))
    return out


def canonical_name(label):
    return label.replace("_", "-").lower()


def resolve_label(elf, label, label_addr, back_bytes):
    """Binding symbol + symbol_offset for a label: the pret symbol exactly at
    label_addr (offset back_bytes), or the pinned containing-symbol slice
    (label_addr - offset) for the 4 no-symbol voicegroup labels. Returns
    (symbol, symbol_offset) where symbol_offset is the total offset from the
    binding symbol's value to the payload start. ARM mapping symbols ($a/$d/
    $t/$b) are excluded - they sit at region boundaries and can share an
    address with the real label."""
    at = [s for s in elf.symbols if s[1] == label_addr and not s[0].startswith("$")]
    if len(at) == 1:
        return at[0][0], back_bytes
    if len(at) > 1:
        fail(f"duplicate pret symbols at label address {label_addr:#x}: "
             f"{[s[0] for s in at]}")
    pin = NO_SYMBOL_GROUPS.get(label)
    if pin is None:
        fail(f"no pret symbol at {label_addr:#x} for label '{label}' and it is "
             f"not in the pinned no-symbol table")
    containing, coff = pin
    csym = elf.find(containing)
    if csym is None:
        fail(f"containing symbol {containing} for '{label}' not in the pret ELF")
    if csym[1] + coff != label_addr:
        fail(f"pinned containing {containing}@{csym[1]:#x} + {coff} != label "
             f"{label_addr:#x} for '{label}'")
    return containing, coff + back_bytes


def resolve_structural(root, elf, rom, inventory):
    """Derives and validates the 202 structural resources (195 voicegroups
    + 2 cry tables + 5 keysplit runs). Returns a list of (key, kind,
    symbol, artifact, type, schema, rep, size, rom_off, symbol_offset)
    sorted by key. Every step is a hard failure on mismatch: stream row
    total, inventory extras agreement, duplicate logical GBA starts, label
    order == stream order, exactly-one pret symbol per label (or the pinned
    containing slice), and per-row pointer resolution."""
    entries, total = parse_stream(root)
    if total != PINNED_STREAM_ROWS:
        fail(f"stream model: {total} rows != pinned {PINNED_STREAM_ROWS} "
             f"({PINNED_VOICEGROUP_ROWS} voicegroup + 2x{PINNED_CRY_ROWS} cry)")
    ks_entries = parse_keysplits_file(root)
    ks_total = sum(r for _n, _b, r in ks_entries)
    if ks_total != PINNED_KEYSPLIT_RUN_BYTES:
        fail(f"keysplit runs: {ks_total} B != pinned {PINNED_KEYSPLIT_RUN_BYTES}")

    base = elf.find("voicegroup000")
    if base is None:
        fail("pret ELF has no voicegroup000 (the first stream row)")
    base = base[1]

    # inventory cross-check: per-resource extras must agree with the model
    inv = {r["symbol"]: r for r in inventory.values()}
    for label, back, rows, _inc in entries:
        r = inv[label] if label in ("gCryTable", "gCryTable_Reverse") \
            else inv.get("voicegroup_" + label)
        if r is None:
            fail(f"inventory has no resource for stream label '{label}'")
        if r.get("row_count") != rows or (label not in (
                "gCryTable", "gCryTable_Reverse") and r.get("back") != back):
            fail(f"inventory {r['key']}: row_count/back "
                 f"({r.get('row_count')}/{r.get('back')}) != stream model "
                 f"({rows}/{back})")
    for name, back_b, run in ks_entries:
        r = inv.get("keysplit_" + name)
        if r is None:
            fail(f"inventory has no keysplit_{name}")
        if r.get("offset") != back_b or r.get("run_bytes") != run:
            fail(f"inventory keysplit_{name}: offset/run_bytes "
                 f"({r.get('offset')}/{r.get('run_bytes')}) != stream model "
                 f"({back_b}/{run})")

    # leaf symbol addresses (for pointer resolution)
    leaf_addrs = set()
    for r in inventory.values():
        if r["kind"] in LEAF_KINDS:
            s = elf.find(r["symbol"])
            if s is None:
                fail(f"leaf symbol {r['symbol']} not found in the pret ELF")
            leaf_addrs.add(s[1])

    # --- voicegroups + cry tables: label addr = BASE + (pos - back) * 12 ---
    out = []
    vg_addrs = []
    pos = 0
    for label, back, rows, _inc in entries:
        if pos - back < 0:
            fail(f"{label}: label before the stream base (back {back} rows at "
                 f"position {pos})")
        vg_addrs.append((label, base + (pos - back) * 12))
        pos += rows
    if len({a for _l, a in vg_addrs}) != len(vg_addrs):
        fail(f"duplicate logical GBA starts among the {len(vg_addrs)} "
             f"voicegroup/cry labels")
    for (a, b), (c, d) in zip(vg_addrs, vg_addrs[1:]):
        if not b < d:
            fail(f"label order violates stream order: {a} @ {b:#x} not below "
                 f"{c} @ {d:#x}")
    for (label, back, rows, _inc), (_l, label_addr) in zip(entries, vg_addrs):
        size = rows * 12
        back_bytes = back * 12
        symbol, symbol_offset = resolve_label(elf, label, label_addr, back_bytes)
        if symbol_offset != back_bytes:
            # the 4 no-symbol labels: offset includes the containing offset
            containing, coff = NO_SYMBOL_GROUPS[label]
            if symbol != containing or symbol_offset != coff + back_bytes:
                fail(f"{label}: unexpected containing-slice binding "
                     f"{symbol}+{symbol_offset}")
        key = "emerald:audio/cry-table/" + (
            "forward" if label == "gCryTable" else "reverse") \
            if label in ("gCryTable", "gCryTable_Reverse") \
            else "emerald:audio/voicegroup/" + canonical_name(label)
        kind = "cry-table" if label in ("gCryTable", "gCryTable_Reverse") \
            else "voicegroup"
        rom_off = label_addr + back_bytes - GEN3_GBA_ROM_BASE
        out.append((key, kind, symbol, None, "instrument-bank", 1,
                    "gba-tone-data-12", size, rom_off, symbol_offset))

    # --- keysplits: run start = stream end + cum runs; label = start - back
    stream_end = base + total * 12
    ks_addrs = []
    cum = 0
    for name, back_b, run in ks_entries:
        start = stream_end + cum
        ks_addrs.append((name, start - back_b))
        cum += run
    if len({a for _n, a in ks_addrs}) != len(ks_addrs):
        fail("duplicate keysplit label starts")
    for i, (name, back_b, run) in enumerate(ks_entries):
        start = stream_end + sum(e[2] for e in ks_entries[:i])
        label_addr = start - back_b
        symbol, symbol_offset = resolve_label(elf, name, label_addr, back_b)
        if symbol_offset != back_b:
            fail(f"keysplit_{name}: resolved offset {symbol_offset} != back "
                 f"{back_b}")
        if not symbol.startswith("KeySplitTable"):
            fail(f"keysplit_{name}: pret symbol at label is {symbol}, "
                 f"expected KeySplitTableN")
        out.append(("emerald:audio/keysplit/" + canonical_name(name), "keysplit",
                    symbol, None, "instrument-bank", 2, "gba-keysplit-run",
                    run, start - GEN3_GBA_ROM_BASE, symbol_offset))

    # --- pointer resolution over the GBA-form rows (from the ROM slices) ---
    ks_addrs = {a for _n, a in ks_addrs}
    vg_addrs = {a for _l, a in vg_addrs}
    for key, kind, symbol, _art, _t, _s, _rep, size, rom_off, symbol_offset in out:
        if kind == "keysplit":
            continue  # 3-byte entries, no pointer fields
        data = rom[rom_off:rom_off + size]
        if len(data) != size:
            fail(f"{key}: ROM slice out of bounds")
        for i in range(size // 12):
            row = data[i * 12:(i + 1) * 12]
            t = row[0]
            if t not in ALL_TYPES:
                fail(f"{key} row {i}: unknown ToneData type 0x{t:02x}")
            if t in PTR_SAMPLE | PTR_WAVE:
                ptr = struct.unpack_from("<I", row, 4)[0]
                if ptr not in leaf_addrs:
                    fail(f"{key} row {i} (type 0x{t:02x}): sample pointer "
                         f"{ptr:#x} not a leaf address")
            elif t in PTR_GROUP:
                ptr = struct.unpack_from("<I", row, 4)[0]
                if ptr not in vg_addrs:
                    fail(f"{key} row {i} (type 0x{t:02x}): voicegroup pointer "
                         f"{ptr:#x} not a voicegroup/cry label address")
                if t in PTR_KEYSPLIT:
                    ptr2 = struct.unpack_from("<I", row, 8)[0]
                    if ptr2 not in ks_addrs:
                        fail(f"{key} row {i}: keysplit pointer {ptr2:#x} not a "
                             f"keysplit label address")

    out.sort(key=lambda r: r[0])
    return out, vg_addrs  # vg_addrs was narrowed to the 197 label addresses


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", default=".")
    ap.add_argument("elf", nargs="?", default="../pokeemerald-reference/pokeemerald.elf")
    ap.add_argument("rom", nargs="?", default="../pokeemerald-reference/pokeemerald.gba")
    ap.add_argument("outdir", nargs="?", default="resources/extraction/emerald/bpee01/audio")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    root = Path(args.root)
    outdir = Path(args.outdir)
    inv_path = root / "resources/extraction/emerald/bpee01/audio/inventory.generated.toml"
    if not inv_path.exists():
        fail(f"R12-A inventory not found at {inv_path} (run gen_audio_inventory.py first)")
    inventory = parse_inventory(inv_path)

    elf = Elf32(open(args.elf, "rb").read())
    rom = open(args.rom, "rb").read()
    if len(rom) != 0x1000000:
        fail(f"ROM size {len(rom)} != 16 MiB")

    # select the leaf subset, sorted by key (deterministic emission order)
    leaves = [r for r in inventory.values() if r["kind"] in LEAF_KINDS]
    leaves.sort(key=lambda r: r["key"])
    counts = {}
    for r in leaves:
        counts[r["kind"]] = counts.get(r["kind"], 0) + 1
    if counts != PINNED:
        fail(f"leaf kind counts {counts} != pinned {PINNED}")
    if len(leaves) != PINNED_TOTAL:
        fail(f"leaf count {len(leaves)} != {PINNED_TOTAL}")

    structural, vg_addrs = resolve_structural(root, elf, rom, inventory)
    struct_counts = {}
    for r in structural:
        struct_counts[r[1]] = struct_counts.get(r[1], 0) + 1
    if struct_counts != PINNED_STRUCT:
        fail(f"structural kind counts {struct_counts} != pinned {PINNED_STRUCT}")
    if len(structural) != PINNED_STRUCT_TOTAL:
        fail(f"structural count {len(structural)} != {PINNED_STRUCT_TOTAL}")

    songs, agg_sweep, agg_xcmd_sub, all_targets, all_tones = resolve_songs(
        root, elf, rom, inventory, vg_addrs, outdir, args)
    if len(songs) != 530:
        fail(f"song count {len(songs)} != 530")

    native_table_lines = emit_native_song_table(root, elf, rom, args)

    rows = []  # (key, kind, symbol, artifact, type, schema, rep, size,
    #           rom_off, symbol_offset)
    for r in leaves:
        key = r["key"]
        kind = r["kind"]
        symbol = r["symbol"]
        artifact = r["source_artifact"]
        rtype = r["resource_type"]
        schema = r["schema"]
        rep = r["representation"]
        if rtype != "audio-sample":
            fail(f"{key}: unexpected resource_type {rtype}")
        if schema != 1:
            fail(f"{key}: unexpected schema {schema}")
        art_path = root / artifact
        if not art_path.exists():
            fail(f"{key}: artifact {artifact} missing")
        art = art_path.read_bytes()

        gap, sect = elf.next_gap(symbol)
        if gap != ((len(art) + 3) & ~3):
            fail(f"{key}: symbol '{symbol}' end-gap {gap} != align4(artifact {len(art)}) "
                 f"(next-symbol/object-layout size derivation failed)")
        size = len(art)

        if kind == "programmable-wave":
            if size != 16:
                fail(f"{key}: programmable wave must be exactly 16 bytes, got {size}")
        else:
            validate_wavedata(art, key, kind, size)

        # three-way proof: ELF slice == artifact == ROM slice
        elf_slice = elf.slice(symbol, size)
        if elf_slice != art:
            fail(f"{key}: ELF slice at '{symbol}' differs from the artifact")
        rom_off = elf.find(symbol)[1] - GEN3_GBA_ROM_BASE
        if rom_off + size > len(rom):
            fail(f"{key}: ROM slice out of bounds")
        if rom[rom_off:rom_off + size] != art:
            fail(f"{key}: ROM slice at 0x{rom_off:x} differs from the artifact")
        rows.append((key, kind, symbol, artifact, rtype, schema, rep, size,
                     rom_off, None))

    # Structural artifacts: the GBA-form canonical payload is synthesized
    # from the ROM slice into outdir/structural/ (gitignored; consumed only
    # by the manifest Guardrail 10 - the pack slices the ROM directly).
    structural_rows = []
    for key, kind, symbol, _art, rtype, schema, rep, size, rom_off, sym_off in structural:
        data = rom[rom_off:rom_off + size]
        if len(data) != size:
            fail(f"{key}: structural ROM slice out of bounds")
        art = outdir / "structural" / (key[len("emerald:audio/"):].replace("/", "-")
                                       + ".bin")
        if args.check:
            if not art.exists() or art.read_bytes() != data:
                fail(f"--check: {art} differs from deterministic ROM-slice "
                     f"regeneration")
        else:
            art.parent.mkdir(parents=True, exist_ok=True)
            art.write_bytes(data)
        # three-way proof: ELF slice == artifact == ROM slice (the manifest
        # re-proves this; proving it here keeps generation honest too)
        if elf.slice_at(symbol, sym_off, size) != data:
            fail(f"{key}: ELF slice at '{symbol}'+{sym_off} differs from the "
                 f"ROM slice")
        structural_rows.append((key, kind, symbol, str(art), rtype, schema,
                                rep, size, rom_off, sym_off))

    rows += structural_rows
    rows += songs
    rows.sort(key=lambda r: r[0])

    # --- emit the catalog ---
    cat_lines = [
        "# Generated by tools/gen3_resources/audio_family/gen_audio_bindings.py.",
        "# Do not edit by hand; edit the R12-A inventory and re-run the",
        "# generator (regeneration must be a no-op diff).",
        "",
        "catalog_version = 1",
        'namespace = "emerald"',
        'resource_api = "1.0.0"',
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    for key, kind, symbol, artifact, rtype, schema, rep, size, rom_off, sym_off in rows:
        cat_lines.append("[[resources]]")
        cat_lines.append(f'id = "{key}"')
        cat_lines.append(f'type = "{rtype}"')
        cat_lines.append(f"schema = {schema}")
        cat_lines.append("required_for_base = true")
        cat_lines.append("")

    # --- emit the bindings ---
    bin_lines = [
        "# Generated by tools/gen3_resources/audio_family/gen_audio_bindings.py.",
        "# Do not edit by hand; edit the R12-A inventory and re-run the",
        "# generator (regeneration must be a no-op diff).",
        "",
        "# Semantic extraction bindings for the 1,301 audio resources:",
        "# 569 R12-B leaves (105 root + 51 phoneme + 388 cry samples, 25",
        "# programmable waves) + 202 R12-C structural (195 voicegroups at",
        "# 20,594 GBA 12-byte rows, 2 cry tables at 388 rows each, 5",
        "# keysplit runs at 372 B) + 530 R12-D song graphs (684,052 B of",
        "# MP2K streams; the packed-object slice rule + the engine-faithful",
        "# stream walker in song_walker.py prove every object boundary and",
        "# every address operand). NO ROM offsets: they are derived from",
        "# the matching GBA ELF symbol table via the R1A generator",
        "# (gen3-elf-manifest), never hand-maintained. The artifact IS the",
        "# canonical payload (raw encoding); sizes are derived and proven",
        "# by gen_audio_bindings.py (end-symbol/object-layout method +",
        "# WaveData2 structural validation + the R12-C stream model + the",
        "# R12-D walker) before the manifest pipeline re-proves them.",
        "# Structural bindings may carry `symbol_offset` (drumset",
        "# back-shifts, the 4 no-symbol containing slices, keysplit run",
        "# starts) - the manifest's slice path, raw encoding only. Song",
        "# bindings bind their first track label (mus_dummy binds its own",
        "# header symbol) with expected_decoded_size = object size.",
        "",
        "bindings_version = 1",
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    for key, kind, symbol, artifact, rtype, schema, rep, size, rom_off, sym_off in rows:
        bin_lines.append("[[bindings]]")
        bin_lines.append(f'id = "{key}"')
        bin_lines.append(f'symbol = "{symbol}"')
        if sym_off:
            bin_lines.append(f"symbol_offset = {sym_off}")
        bin_lines.append(f'source_artifact = "{artifact}"')
        bin_lines.append('source_encoding = "raw"')
        bin_lines.append(f'canonical_representation = "{rep}"')
        bin_lines.append(f"expected_decoded_size = {size}")
        bin_lines.append("")

    def write_if(path, lines):
        text = "\n".join(lines)
        if args.check:
            if not path.exists():
                fail(f"--check: {path} does not exist")
            if path.read_text() != text:
                fail(f"--check: {path} differs from deterministic regeneration")
            print(f"check passed: {path}")
        else:
            path.write_text(text)
            print(f"wrote {path}")

    write_if(outdir / "catalog.generated.toml", cat_lines)
    write_if(outdir / "bindings.generated.toml", bin_lines)
    write_if(root / "sound" / "song_table_native.generated.inc",
             native_table_lines)

    total = sum(counts.values())
    print(f"leaf resources: {total} "
          f"(root {counts['sample/root']}, phoneme {counts['sample/phoneme']}, "
          f"cry {counts['sample/cry']}, wave {counts['programmable-wave']})")
    print(f"structural resources: {sum(struct_counts.values())} "
          f"(voicegroup {struct_counts['voicegroup']}, "
          f"cry-table {struct_counts['cry-table']}, "
          f"keysplit {struct_counts['keysplit']}) "
          f"= {PINNED_STREAM_ROWS} rows, {PINNED_STREAM_ROWS * 12} GBA-form "
          f"bytes + {PINNED_KEYSPLIT_RUN_BYTES} keysplit bytes")
    print(f"song resources: {len(songs)} = {song_walker.PIN_TOTAL_BYTES} bytes")
    print(f"song walker: GOTO {agg_sweep['goto']} / PATT {agg_sweep['patt']} "
          f"/ REPT {agg_sweep['rept']} / memacc-jump {agg_sweep['memacc_jump']} "
          f"/ xwave {agg_sweep['xwave']} = "
          f"{agg_sweep['goto'] + agg_sweep['patt'] + agg_sweep['rept'] + agg_sweep['memacc_jump'] + agg_sweep['xwave']} "
          f"operands; XCMD {agg_sweep['xcmd']} {dict(agg_xcmd_sub)}; "
          f"notes {agg_sweep['note']}; MEMACC {agg_sweep['memacc']}; "
          f"targets {len(all_targets)} distinct; tones {len(all_tones)} "
          f"distinct")
    print("native gSongTable: 610 rows (530 real ROM addresses + 80 dummy)")


if __name__ == "__main__":
    main()

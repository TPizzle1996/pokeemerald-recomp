#!/usr/bin/env python3
"""R13-B: leaf payload generator for the movement-script + multiboot families.

Derives both leaf families from the qualified pret reference ELF + retail ROM
and emits resources/extraction/emerald/bpee01/{movement,multiboot}/ (six
generated files per family: inventory, catalog, bindings, ownership,
consumers + the leaf artifacts) plus the native seam slot table
include/emerald/resources/leaf_native.generated.h.

Movement family (schema 1, gba-bytes):
  * 1,047 `*_Movement_*` NOTYPE size-0 labels in script_data (section 4).
    Size = gap to the next symbol in the section (the end-symbol method).
    Independently proven against the checked-in pret data sources: every
    script's macro sequence (asm/macros/movement.inc, 159 single-byte
    movement actions) is expanded and compared byte-for-byte with the ELF
    slice - the label set matches the ELF exactly (0 diff) and every byte
    matches (0/1047 mismatches), with no alignment padding anywhere
    (source total 7404 B == extent total 7404 B).
  * 8 sMovement_* objects in .rodata (section 6), st_size 2-4 B. The pret
    ELF has 10; sMovement_UnionPlayerEnter/Exit are excluded - they are
    compiled only in the traditional build (union_room_player_avatar.c is
    not linked natively; the fresh recomp binary contains exactly 8).
  * Total: 1055 resources, 7428 B (7404 labels + 24 objects).

Multiboot family (schema 2, gba-bytes), section 9 (other_data):
  * emerald:multiboot/ereader           gEReaderLinkData_Start   12512 B
    (pret data/ereader_link_data.bin; == recomp data/mb_ereader.gba)
  * emerald:multiboot/pokemon-colosseum gMultiBootProgram_PokemonColosseum_Start
    163840 B (pret data/pokemon_colosseum.mb; == recomp data/mb_colosseum.gba)
  * Total: 2 resources, 176352 B.
  gMultiBootProgram_BerryGlitchFix_Start (15348 B) is NOT a resource: the
  recomp's multiboot_berry_glitch_fix.s comments the .incbin out, so the
  blob is absent from the native binary (title-screen chain is dead).

Ownership: every record is COMPILED_PENDING_MIGRATION on native (payload
still compiled; R13-B is additive only - no consumer is redirected, see
consumers.generated.toml) and COMPILED on GBA. All artifacts are raw: the
bytes ARE the canonical payload, so encoded == decoded and both hashes are
the artifact sha256.

Pinned counts (the R13 audit + the R13-B re-verification) are enforced as
hard gates; --check must be a no-op diff.

Usage:
  gen_leaf_family.py [root] [elf] [rom] [outdir] [--check]
    root   repo root (default .)
    elf    qualified pret ELF (default ../pokeemerald-reference/pokeemerald.elf)
    rom    retail-matching ROM (default ../pokeemerald-reference/pokeemerald.gba)
    outdir resource extraction root (default resources/extraction/emerald/bpee01)
"""

import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path

GEN3_GBA_ROM_BASE = 0x08000000
ROM_SIZE = 0x1000000
ROM_SHA1 = "f3ae088181bf583e55daf962a92bb46f4f1d07b7"

# ------------------------------------------------------------- movement pins
MOVEMENT_LABEL_SECTION = 4        # script_data in the qualified pret ELF
MOVEMENT_OBJECT_SECTION = 6       # .rodata
MOVEMENT_PINNED_LABELS = 1047
MOVEMENT_PINNED_OBJECTS_RAW = 10
MOVEMENT_OBJECT_EXCLUDED = frozenset(
    {"sMovement_UnionPlayerEnter", "sMovement_UnionPlayerExit"})
MOVEMENT_PINNED_OBJECTS = 8
MOVEMENT_PINNED_LABEL_BYTES = 7404
MOVEMENT_PINNED_OBJECT_BYTES = 24
MOVEMENT_PINNED_TOTAL = 1055
MOVEMENT_PINNED_BYTES = 7428

# ------------------------------------------------------------ multiboot pins
MULTIBOOT_PINNED_TOTAL = 2
MULTIBOOT_PINNED_BYTES = 176352
# (name, pret start sym, pret end sym, pinned bytes,
#  [pret data artifacts, ...], [recomp data artifacts, ...],
#  recomp compiled symbol)
# The ereader RECOMP symbol is an upstream rename of the pret symbol
# (b89722500): the native binary defines gMultiBootProgram_EReader_Start,
# never gEReaderLinkData_Start. Ownership + the seam table must name the
# COMPILED symbol (what nm sees in the binary), while the ELF-derived
# manifest keeps the pret name (the symbol the qualified ELF defines).
MULTIBOOT_PROGRAMS = [
    ("ereader", "gEReaderLinkData_Start", "gEReaderLinkData_End", 12512,
     ["data/ereader_link_data.bin"], ["data/mb_ereader.gba"],
     "gMultiBootProgram_EReader_Start"),
    ("pokemon-colosseum", "gMultiBootProgram_PokemonColosseum_Start",
     "gMultiBootProgram_PokemonColosseum_End", 163840,
     ["data/pokemon_colosseum.mb"], ["data/mb_colosseum.gba"],
     "gMultiBootProgram_PokemonColosseum_Start"),
]
MULTIBOOT_NOT_A_RESOURCE = (
    "gMultiBootProgram_BerryGlitchFix_Start",
    "gMultiBootProgram_BerryGlitchFix_End", 15348)


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
            self.sections.append(dict(name=sname, type=stype, flags=sflags,
                                      addr=saddr, offset=soff, size=ssize,
                                      link=slink, align=salign))
        shstr = self.sections[struct.unpack_from("<H", data, 0x32)[0]]
        self.shstr = data[shstr["offset"]:shstr["offset"] + shstr["size"]]
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
        for off in range(stab["offset"], stab["offset"] + stab["size"],
                         entsize):
            (name_off, value, size, info, other, shndx) = struct.unpack_from(
                "<IIIBBH", data, off)
            end = self.strtab.find(b"\0", name_off)
            name = self.strtab[name_off:end].decode("ascii", "replace")
            self.symbols.append((name, value, size, info, shndx))

    def sec_name(self, idx):
        s = self.sections[idx]
        end = self.shstr.find(b"\0", s["name"])
        return self.shstr[s["name"]:end].decode("ascii", "replace")

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
            fail(f"symbol '{name}' unexpectedly has st_size {size} "
                 f"(asm labels must be 0)")
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
        """File bytes backing [value+offset, value+offset+size)."""
        sym = self.find(name)
        _, value, _, _, shndx = sym
        sect = self.sections[shndx]
        off = sect["offset"] + (value - sect["addr"]) + offset
        return self.data[off:off + size]


# --------------------------------------------- movement source cross-check
MOVEMENT_MACRO_RE = re.compile(
    r"create_movement_action\s+(\w+)\s*,\s*(MOVEMENT_ACTION_\w+)")
MOVEMENT_CONST_RE = re.compile(
    r"#define\s+(MOVEMENT_ACTION_\w+)\s+(0x[0-9a-fA-F]+|\d+)")
LABEL_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*):\s*:?\s*(?:@.*)?$")


def parse_movement_sources(pret):
    """Expand every movement script in the pret data sources to its byte
    sequence: 159 single-byte movement macros (asm/macros/movement.inc ->
    MOVEMENT_ACTION_* in include/constants/event_object_movement.h).
    Returns {label: [byte, ...]} for every label containing 'Movement_'.
    Non-macro lines inside a movement block are a hard failure."""
    mactext = (pret / "asm/macros/movement.inc").read_text()
    macro2const = dict(MOVEMENT_MACRO_RE.findall(mactext))
    if len(macro2const) != 159:
        fail(f"movement macro table: {len(macro2const)} entries != 159")
    consttext = (pret / "include/constants/event_object_movement.h").read_text()
    const2val = {}
    for m in MOVEMENT_CONST_RE.finditer(consttext):
        v = int(m.group(2), 16) if m.group(2).startswith("0x") else int(m.group(2))
        const2val[m.group(1)] = v
    if len(const2val) < 159:
        fail(f"MOVEMENT_ACTION constants: {len(const2val)} < 159")

    files = [pret / "data/event_scripts.s"] + sorted(pret.glob("data/**/*.inc"))
    per_label = {}
    for f in files:
        if not f.exists():
            continue
        text = f.read_text()
        cur_label, cur_bytes = None, []
        for ln in text.splitlines():
            m = LABEL_RE.match(ln)
            if m:
                if cur_label is not None and "Movement_" in cur_label:
                    per_label.setdefault(cur_label, []).extend(cur_bytes)
                cur_label, cur_bytes = m.group(1), []
                continue
            if cur_label is None:
                continue
            s = ln.strip()
            if s.startswith("@") or not s:
                continue
            for name in macro2const:
                if s == name or s.startswith(name + " "):
                    cur_bytes.append(const2val[macro2const[name]])
                    break
            else:
                if "Movement_" in cur_label:
                    fail(f"{f}: non-macro line inside movement block "
                         f"'{cur_label}': {s!r}")
        if cur_label is not None and "Movement_" in cur_label:
            per_label.setdefault(cur_label, []).extend(cur_bytes)
    return per_label


def movement_canonical(symbol):
    """Canonical id component: lowercase symbol with '_' -> '-' (R11-B rule)."""
    return symbol.lower().replace("_", "-")


def resolve_movement(elf, rom, pret, args):
    labels = [s for s in elf.symbols
              if "Movement_" in s[0] and (s[3] & 0xf) == 0  # NOTYPE
              and s[4] == MOVEMENT_LABEL_SECTION and s[2] == 0]
    objects = [s for s in elf.symbols
               if s[0].startswith("sMovement_") and (s[3] & 0xf) == 1
               and s[4] == MOVEMENT_OBJECT_SECTION]
    if len(labels) != MOVEMENT_PINNED_LABELS:
        fail(f"movement labels {len(labels)} != pinned {MOVEMENT_PINNED_LABELS}")
    if len(objects) != MOVEMENT_PINNED_OBJECTS_RAW:
        fail(f"sMovement_ objects {len(objects)} != pinned "
             f"{MOVEMENT_PINNED_OBJECTS_RAW}")
    obj_names = [s[0] for s in objects]
    for name in MOVEMENT_OBJECT_EXCLUDED:
        if name not in obj_names:
            fail(f"excluded object '{name}' not in the pret ELF")
    fam_objects = [s for s in objects if s[0] not in MOVEMENT_OBJECT_EXCLUDED]
    if len(fam_objects) != MOVEMENT_PINNED_OBJECTS:
        fail(f"movement objects {len(fam_objects)} != pinned "
             f"{MOVEMENT_PINNED_OBJECTS}")

    src = parse_movement_sources(pret)

    rows = []  # (key, symbol, size, rom_off)
    label_bytes = 0
    seen_keys = set()
    for sym in sorted(labels, key=lambda s: s[1]):  # address order
        name, value, size, info, shndx = sym
        gap, sect = elf.next_gap(name)
        if gap <= 0:
            fail(f"movement label '{name}': extent {gap} <= 0")
        if name not in src:
            fail(f"movement label '{name}' not found in the pret data sources")
        if gap != len(src[name]):
            fail(f"movement label '{name}': ELF extent {gap} != source bytes "
                 f"{len(src[name])} (source == ELF slice proof failed)")
        elf_slice = elf.slice(name, gap)
        if list(elf_slice) != src[name]:
            fail(f"movement label '{name}': ELF slice != source expansion")
        rom_off = value - GEN3_GBA_ROM_BASE
        if rom_off + gap > len(rom):
            fail(f"movement label '{name}': ROM slice out of bounds")
        if rom[rom_off:rom_off + gap] != elf_slice:
            fail(f"movement label '{name}': ROM slice != ELF slice")
        key = f"emerald:movement/{movement_canonical(name)}"
        if key in seen_keys:
            fail(f"duplicate movement key {key} (case collision)")
        seen_keys.add(key)
        # Movement compiled symbols are unchanged between pret and the
        # recomp tree (same labels), so rec symbol == pret symbol.
        rows.append((key, name, name, gap, rom_off))
        label_bytes += gap
    if label_bytes != MOVEMENT_PINNED_LABEL_BYTES:
        fail(f"movement label bytes {label_bytes} != pinned "
             f"{MOVEMENT_PINNED_LABEL_BYTES}")

    object_bytes = 0
    for sym in sorted(fam_objects, key=lambda s: s[1]):
        name, value, size, info, shndx = sym
        if size <= 0 or size > 4:
            fail(f"sMovement_ object '{name}': st_size {size} out of 1..4")
        elf_slice = elf.slice(name, size)
        rom_off = value - GEN3_GBA_ROM_BASE
        if rom_off + size > len(rom):
            fail(f"sMovement_ object '{name}': ROM slice out of bounds")
        if rom[rom_off:rom_off + size] != elf_slice:
            fail(f"sMovement_ object '{name}': ROM slice != ELF slice")
        key = f"emerald:movement/{movement_canonical(name)}"
        if key in seen_keys:
            fail(f"duplicate movement key {key}")
        seen_keys.add(key)
        rows.append((key, name, name, size, rom_off))
        object_bytes += size
    if object_bytes != MOVEMENT_PINNED_OBJECT_BYTES:
        fail(f"movement object bytes {object_bytes} != pinned "
             f"{MOVEMENT_PINNED_OBJECT_BYTES}")

    rows.sort(key=lambda r: r[0])
    total = len(rows)
    if total != MOVEMENT_PINNED_TOTAL:
        fail(f"movement resources {total} != pinned {MOVEMENT_PINNED_TOTAL}")
    total_bytes = sum(r[3] for r in rows)
    if total_bytes != MOVEMENT_PINNED_BYTES:
        fail(f"movement bytes {total_bytes} != pinned {MOVEMENT_PINNED_BYTES}")
    return rows


def resolve_multiboot(elf, rom, root, pret):
    rows = []
    total = 0
    for name, start, end, pinned, pret_arts, rec_arts, rec_sym in \
            MULTIBOOT_PROGRAMS:
        s_sym = elf.find(start)
        e_sym = elf.find(end)
        if s_sym is None or e_sym is None:
            fail(f"multiboot '{name}': start/end symbols not in the ELF")
        size = e_sym[1] - s_sym[1]
        if size != pinned:
            fail(f"multiboot '{name}': {start}..{end} = {size} B != pinned "
                 f"{pinned}")
        if s_sym[4] != e_sym[4]:
            fail(f"multiboot '{name}': start/end in different sections")
        elf_slice = elf.slice(start, size)
        rom_off = s_sym[1] - GEN3_GBA_ROM_BASE
        if rom_off + size > len(rom):
            fail(f"multiboot '{name}': ROM slice out of bounds")
        if rom[rom_off:rom_off + size] != elf_slice:
            fail(f"multiboot '{name}': ROM slice != ELF slice")
        for rel in pret_arts:  # pret data artifacts == the slice
            p = pret / rel
            if not p.exists():
                fail(f"multiboot '{name}': pret artifact {rel} missing")
            if p.read_bytes() != elf_slice:
                fail(f"multiboot '{name}': pret artifact {rel} != ELF slice")
        for rel in rec_arts:  # recomp data artifacts == the slice
            p = root / rel
            if not p.exists():
                fail(f"multiboot '{name}': recomp artifact {rel} missing")
            if p.read_bytes() != elf_slice:
                fail(f"multiboot '{name}': recomp artifact {rel} != ELF slice")
        key = f"emerald:multiboot/{name}"
        rows.append((key, start, rec_sym, size, rom_off))
        total += size
    rows.sort(key=lambda r: r[0])
    if len(rows) != MULTIBOOT_PINNED_TOTAL:
        fail(f"multiboot resources {len(rows)} != pinned {MULTIBOOT_PINNED_TOTAL}")
    if total != MULTIBOOT_PINNED_BYTES:
        fail(f"multiboot bytes {total} != pinned {MULTIBOOT_PINNED_BYTES}")
    return rows


# --------------------------------------------------------------- emission
OWNERSHIP_REASON = (
    "# R13-B ownership: every payload is COMPILED_PENDING_MIGRATION on\n"
    "# native - the legacy symbol stays compiled and no consumer is\n"
    "# redirected (R13-B is additive-only). Movement consumers address the\n"
    "# scripts from field-script bytecode operands (applymovement/\n"
    "# applymovementat, src/scrcmd.c) and C immediates (trainer_see.c,\n"
    "# field_special_scene.c) - redirect needs the map/script pointer graph\n"
    "# (R13-G). The ereader multiboot program is LIVE (main_menu.c ->\n"
    "# ereader_screen.c:405 passes the compiled pointer to\n"
    "# EReaderHandleTransfer, which may write it); the colosseum program is\n"
    "# dead data natively (intro.c:1131 is #ifndef PORTABLE) but its\n"
    "# physical removal needs the R13-J symbol-absence proof. A future\n"
    "# stage flips these to ROM_BASE_ONLY after the runtime seam lands.")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_if(path, lines, args):
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


def emit_family(rows, family, outdir, args, artifact_dir):
    """Emit catalog/bindings/ownership/consumers + artifacts for one family.
    `rows` are (key, pret symbol, rec symbol, size, rom_off) sorted by key;
    `artifact_dir` is the subdirectory under the family dir holding the leaf
    .bin artifacts."""
    fam = outdir / family
    artdir = fam / artifact_dir
    art_by_key = {}
    for key, symbol, rec_symbol, size, rom_off in rows:
        rel = f"resources/extraction/emerald/bpee01/{family}/{artifact_dir}/" \
              f"{key[len(f'emerald:{family}/'):]}.bin"
        art_by_key[key] = rel
        art = root / rel
        data = rom[rom_off:rom_off + size]
        if args.check:
            if not art.exists() or art.read_bytes() != data:
                fail(f"--check: {art} differs from deterministic ROM-slice "
                     f"regeneration")
        else:
            art.parent.mkdir(parents=True, exist_ok=True)
            art.write_bytes(data)

    # inventory
    inv_lines = [
        f"# Generated by tools/gen3_resources/leaf_family/gen_leaf_family.py",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "inventory_version = 1",
        f'family = "{family}"',
        'game = "emerald"',
        'rom_profile = "bpee01"',
        "",
        "# Pinned counts (R13 audit + R13-B re-verification):",
    ]
    if family == "movement":
        inv_lines += [
            f"# {MOVEMENT_PINNED_TOTAL} resources = {MOVEMENT_PINNED_LABELS}",
            f"# script_data labels ({MOVEMENT_PINNED_LABEL_BYTES} B) + "
            f"{MOVEMENT_PINNED_OBJECTS} .rodata sMovement_* objects "
            f"({MOVEMENT_PINNED_OBJECT_BYTES} B); "
            f"{MOVEMENT_PINNED_BYTES} B total. The pret ELF's 10 objects "
            f"include sMovement_UnionPlayerEnter/Exit (excluded: not linked "
            f"natively).",
        ]
    else:
        inv_lines += [
            f"# {MULTIBOOT_PINNED_TOTAL} programs, {MULTIBOOT_PINNED_BYTES} B "
            f"(other_data section 9).",
            "# gMultiBootProgram_BerryGlitchFix_Start (15348 B) is NOT a",
            "# resource: absent from the native binary (the recomp's",
            "# multiboot_berry_glitch_fix.s comments its .incbin out).",
        ]
    inv_lines += ["", "[[families]]"]
    inv_lines.append(f'kind = "{family}"')
    inv_lines.append(f"symbol_count = {len(rows)}")
    inv_lines.append('resource_type = "binary"')
    inv_lines.append(f"schema = {1 if family == 'movement' else 2}")
    inv_lines.append('representation = "gba-bytes"')
    write_if(fam / "inventory.generated.toml", inv_lines, args)

    # catalog
    cat_lines = [
        "# Generated by tools/gen3_resources/leaf_family/gen_leaf_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "catalog_version = 1",
        'namespace = "emerald"',
        'resource_api = "1.0.0"',
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    for key, symbol, rec_symbol, size, rom_off in rows:
        cat_lines.append("[[resources]]")
        cat_lines.append(f'id = "{key}"')
        cat_lines.append('type = "binary"')
        cat_lines.append(f"schema = {1 if family == 'movement' else 2}")
        cat_lines.append("required_for_base = true")
        cat_lines.append("")
    write_if(fam / "catalog.generated.toml", cat_lines, args)

    # bindings
    bin_lines = [
        "# Generated by tools/gen3_resources/leaf_family/gen_leaf_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        f"# Semantic extraction bindings for the {family} family.",
        "# NO ROM offsets: they are derived from the matching GBA ELF symbol",
        "# table via the R1A generator (gen3-elf-manifest), never",
        "# hand-maintained. The artifact IS the canonical payload (raw",
        "# encoding); sizes are derived and proven by gen_leaf_family.py",
        "# before the manifest pipeline re-proves them.",
        "",
    ]
    if family == "movement":
        bin_lines += [
            "# Movement sizes use the end-symbol method (gap to the next",
            "# symbol in script_data) and are cross-proven against the",
            "# checked-in pret data sources: every script's movement-macro",
            "# sequence expands to exactly the ELF slice bytes, label set",
            "# and byte values both matching 1:1 with zero padding.",
        ]
    else:
        bin_lines += [
            "# Multiboot sizes come from the Start/End symbol pairs and are",
            "# cross-proven against the pret data artifacts and the recomp",
            "# data/*.gba blobs.",
        ]
    bin_lines += [
        "",
        "bindings_version = 1",
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    for key, symbol, rec_symbol, size, rom_off in rows:
        bin_lines.append("[[bindings]]")
        bin_lines.append(f'id = "{key}"')
        # The pret ELF symbol: gen3-elf-manifest derives the ROM offset from
        # the qualified ELF's symbol table, which keeps the pret names.
        bin_lines.append(f'symbol = "{symbol}"')
        bin_lines.append(f'source_artifact = "{art_by_key[key]}"')
        bin_lines.append('source_encoding = "raw"')
        bin_lines.append('canonical_representation = "gba-bytes"')
        bin_lines.append(f"expected_decoded_size = {size}")
        bin_lines.append("")
    write_if(fam / "bindings.generated.toml", bin_lines, args)

    # ownership
    own_lines = [
        "# Generated by tools/gen3_resources/leaf_family/gen_leaf_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Native asset-ownership declaration. Each [[resources]] block",
        "# records the canonical id, M0/M1 key (artifact sha256 - raw",
        "# encoding, so encoded == decoded), legacy compiled symbol, source",
        "# artifact, per-target ownership state and source hashes.",
        OWNERSHIP_REASON,
        "",
        "ownership_version = 1",
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    for key, symbol, rec_symbol, size, rom_off in rows:
        data = rom[rom_off:rom_off + size]
        h = sha256(data)
        own_lines.append("[[resources]]")
        own_lines.append(f'id = "{key}"')
        own_lines.append(f'key = "{h}"')
        # The RECOMP compiled symbol: what nm must find in the native
        # binary. Differs from the pret symbol only for the ereader
        # multiboot program (upstream rename, see MULTIBOOT_PROGRAMS).
        own_lines.append(f'legacy_symbol = "{rec_symbol}"')
        own_lines.append('type = "binary"')
        own_lines.append(f'source_artifact = "{art_by_key[key]}"')
        own_lines.append(f"encoded_length = {size}")
        own_lines.append(f"decoded_length = {size}")
        own_lines.append('source_encoding = "raw"')
        own_lines.append(f'source_encoded_sha256 = "{h}"')
        own_lines.append(f'canonical_decoded_sha256 = "{h}"')
        own_lines.append('ownership_state = "COMPILED_PENDING_MIGRATION"')
        own_lines.append("")
        own_lines.append("[resources.targets]")
        own_lines.append('native = "COMPILED_PENDING_MIGRATION"')
        own_lines.append('gba = "COMPILED"')
        own_lines.append("")
    write_if(fam / "ownership.generated.toml", own_lines, args)

    # consumers
    con_lines = [
        "# Generated by tools/gen3_resources/leaf_family/gen_leaf_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Machine-readable consumer map for the leaf family.",
    ]
    if family == "movement":
        con_lines += [
            "# Movement scripts are consumed by ADDRESS from two places:",
            "#  - applymovement/applymovementat operands inside field-script",
            "#    bytecode (script_data) - src/scrcmd.c:1015,1025",
            "#  - C immediates: the sMovement_* puzzle objects",
            "#    (src/rotating_tile_puzzle.c:152-164,250-262),",
            "#    gPostBattleMovementScript (src/trainer_see.c:805) and the",
            "#    sSSTidalSail* scripts (src/field_special_scene.c:337,342).",
            "# R13-B redirects nothing: the bytecode operand path is part of",
            "# the map/script pointer graph (R13-G) and the C immediates are",
            "# direct native pointers into the compiled arrays.",
        ]
        sites = [
            ("bytecode-operand", "src/scrcmd.c:1015",
             "applymovement", "deferred",
             "operand addresses live inside field-script bytecode in "
             "script_data; redirect requires the map/script pointer graph"),
            ("bytecode-operand", "src/scrcmd.c:1025",
             "applymovementat", "deferred",
             "operand addresses live inside field-script bytecode in "
             "script_data; redirect requires the map/script pointer graph"),
            ("c-immediate", "src/rotating_tile_puzzle.c:152-164",
             "sMovement_Shift*", "deferred",
             "compiled array pointers passed to the rotating tile puzzle"),
            ("c-immediate", "src/rotating_tile_puzzle.c:250-262",
             "sMovement_Face*", "deferred",
             "compiled array pointers passed to the rotating tile puzzle"),
            ("c-immediate", "src/trainer_see.c:805",
             "gPostBattleMovementScript", "deferred",
             "compiled array pointer handed to ScriptMovement_StartObjectMovementScript"),
            ("c-immediate", "src/field_special_scene.c:337,342",
             "sSSTidalSail*MovementScript", "deferred",
             "compiled array pointers handed to ScriptMovement_StartObjectMovementScript"),
        ]
    else:
        con_lines += [
            "# Multiboot programs are consumed by pointer:",
            "#  - ereader LIVE: main_menu.c:1034 ACTION_EREADER ->",
            "#    SetMainCallback2(CB2_InitEReader) (1083-1084) ->",
            "#    ereader_screen.c:405 EReader_Load(&gEReaderData, size,",
            "#    (u32 *)gMultiBootProgram_EReader_Start) -> :87",
            "#    EReaderHandleTransfer(TRUE, size, data, NULL). The pointer",
            "#    is stored in gEReaderData.data and MAY be written by the",
            "#    transfer - not trivially redirectable.",
            "#  - colosseum DEAD DATA natively: the only reference is",
            "#    src/intro.c:1131 (CpuCopy16, #ifndef PORTABLE - compiled",
            "#    out of the native build). The blob stays linked via",
            "#    data/multiboot_pokemon_colosseum.s; physical removal",
            "#    deferred to R13-J (needs the symbol-absence proof).",
        ]
        sites = [
            ("live-pointer", "src/main_menu.c:1034,1083-1084",
             "CB2_InitEReader dispatch", "deferred",
             "action dispatch chain into the ereader screen"),
            ("live-pointer", "src/ereader_screen.c:405",
             "EReader_Load(&gEReaderData, ..., gMultiBootProgram_EReader_Start)",
             "deferred",
             "pointer passed into gEReaderData.data; EReaderHandleTransfer "
             "may write the buffer"),
            ("live-pointer", "src/ereader_screen.c:87",
             "EReaderHandleTransfer(TRUE, size, data, NULL)", "deferred",
             "transfer may write the multiboot buffer"),
            ("dead-data", "src/intro.c:1131",
             "CpuCopy16(&gMultiBootProgram_PokemonColosseum_Start, ...)",
             "deferred",
             "#ifndef PORTABLE - compiled out of the native build; removal "
             "needs the R13-J link-list proof"),
        ]
    con_lines += [
        "",
        "consumers_version = 1",
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    for kind, site, consumer, decision, reason in sites:
        con_lines.append("[[consumer_sites]]")
        con_lines.append(f'kind = "{kind}"')
        con_lines.append(f'site = "{site}"')
        con_lines.append(f'consumer = "{consumer}"')
        con_lines.append(f'decision = "{decision}"')
        con_lines.append(f'reason = "{reason}"')
        con_lines.append("")
    write_if(fam / "consumers.generated.toml", con_lines, args)


def emit_seam_header(rows, root, args):
    lines = [
        "/* Generated by tools/gen3_resources/leaf_family/gen_leaf_family.py.",
        " * Do not edit by hand; re-run the generator and --check it",
        " * (tests/gen3_resources/run_r13b_leaf.sh).",
        " *",
        " * R13-B seam slot table for the leaf payload families. The table",
        " * enumerates every canonical resource the pack publishes (the",
        " * shared movement set + the two multiboot programs) with its",
        " * canonical id, the compiled native symbol, canonical payload size",
        " * and schema (1 = movement, 2 = multiboot). R13-B is additive-",
        " * only: every entry stays COMPILED_PENDING_MIGRATION and no",
        " * consumer is redirected; the seam (EmeraldLeafCompat) publishes",
        " * the arena after validating the session against this exact",
        " * inventory. The 3 recomp-local movement labels",
        " * (Ferry_DepartIsland*, Route103_RivalExitFacingNorth2, 12 B) and",
        " * the pret-only union objects are not resources and are not",
        " * listed. The table DEFINITION is emitted separately as",
        " * src/emerald/resources/leaf_native_table.generated.c.",
        " */",
        "#ifndef EMERALD_RESOURCES_LEAF_NATIVE_GENERATED_H",
        "#define EMERALD_RESOURCES_LEAF_NATIVE_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define LEAF_NATIVE_MOVEMENT_COUNT {MOVEMENT_PINNED_TOTAL}u",
        f"#define LEAF_NATIVE_MULTIBOOT_COUNT {MULTIBOOT_PINNED_TOTAL}u",
        f"#define LEAF_NATIVE_RESOURCE_COUNT {len(rows)}u",
        "",
        "struct LeafNativeResource",
        "{",
        "    const char *name;         /* canonical resource id, e.g. ",
        "                                 emerald:movement/<canonical> */",
        "    const char *legacySymbol; /* compiled native symbol (recomp ",
        "                                 naming; differs from the pret name",
        "                                 only for the ereader multiboot ",
        "                                 program) */",
        "    uint32_t size;            /* canonical payload bytes */",
        "    uint8_t schema;           /* 1 = movement, 2 = multiboot */",
        "};",
        "",
        "extern const struct LeafNativeResource "
        "kLeafNativeResources[LEAF_NATIVE_RESOURCE_COUNT];",
        "",
        "#endif /* EMERALD_RESOURCES_LEAF_NATIVE_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/leaf_native.generated.h",
             lines, args)

    # The table definition: one const row per canonical resource, sorted by
    # key. The seam validates the session's pack against this exact
    # inventory (name/size/schema set equality), so drift is a publish
    # failure, never a partial publication.
    c_lines = [
        "/* Generated by tools/gen3_resources/leaf_family/gen_leaf_family.py.",
        " * Do not edit by hand; re-run the generator and --check it",
        " * (tests/gen3_resources/run_r13b_leaf.sh).",
        " *",
        " * R13-B seam slot table DEFINITION: every canonical leaf payload",
        " * the production pack publishes, in sorted key order. The seam",
        " * (EmeraldLeafCompat) cross-checks the session's pack against this",
        " * table before publishing the arena - an entry missing from the",
        " * pack, a size/schema drift, or an unexpected extra entry is a",
        " * failed publication (fail-soft: the game is unaffected because no",
        " * consumer reads the arena).",
        " */",
        '#include "emerald/resources/leaf_native.generated.h"',
        "",
        "const struct LeafNativeResource "
        "kLeafNativeResources[LEAF_NATIVE_RESOURCE_COUNT] =",
        "{",
    ]
    for key, symbol, rec_symbol, size, rom_off in rows:
        schema = 2 if key.startswith("emerald:multiboot/") else 1
        c_lines.append(
            f'    {{"{key}", "{rec_symbol}", {size}u, {schema}u}},')
    c_lines += [
        "};",
        "",
    ]
    write_if(root / "src/emerald/resources/leaf_native_table.generated.c",
             c_lines, args)


def emit_movement_native_include(rows, root, args):
    """R13-G6: re-carve the compiled movement tables for the LINUX64 link.

    The per-map and common script .inc files that define the R13-B
    movement family are excluded wholesale on LINUX64 (their G
    field-script payload is pack-loaded arena modules), so this
    generated include restores the compiled tables: the R13-B ownership
    contract (COMPILED_PENDING_MIGRATION => symbol in the link) holds
    until the movement cutover. The tables are byte-directive copies of
    the qualified-ROM slices - byte-for-byte the same bytes the GBA
    build assembles from the source movement macros. The live runtime
    never references them (MOVEMENT_TARGET operands resolve through the
    leaf seam to the pack payloads), so these are contract-preserving
    shadows. Included only under the LINUX64 gate in event_scripts.s;
    the GBA build gets the tables from the original .inc files.
    """
    lines = [
        "/* Generated by tools/gen3_resources/leaf_family/gen_leaf_family.py.",
        " * Do not edit by hand; re-run the generator and --check it",
        " * (tests/gen3_resources/run_r13b_leaf.sh).",
        " *",
        " * R13-G6: compiled movement-table retention for the LINUX64",
        " * link (see the generator docstring for the rationale).",
        " */",
        "",
    ]
    retained = []
    for row in rows:
        if row[2].startswith("sMovement_"):
            # The 8 sMovement_* objects (24 B) live in .rodata from a
            # non-gated source - they never left the LINUX64 link, so
            # re-carving them would duplicate the definitions.
            continue
        retained.append(row)
    if len(retained) != MOVEMENT_PINNED_TOTAL - MOVEMENT_PINNED_OBJECTS:
        fail(f"movement native include rows {len(retained)} != "
             f"{MOVEMENT_PINNED_TOTAL - MOVEMENT_PINNED_OBJECTS} "
             f"(skipping the {MOVEMENT_PINNED_OBJECTS} sMovement_ objects)")
    lines.append(
        f"/* {len(retained)} movement tables, {sum(r[3] for r in retained)} B"
        f" (the {MOVEMENT_PINNED_OBJECTS} sMovement_* objects stay "
        f"compiled from their non-gated source) */")
    lines.append("")
    for key, symbol, rec_symbol, size, rom_off in retained:
        data = rom[rom_off:rom_off + size]
        lines.append(f"{rec_symbol}:")
        for i in range(0, len(data), 8):
            chunk = data[i:i + 8]
            lines.append("\t.byte " + ", ".join(f"0x{b:02X}" for b in chunk))
        lines.append("")
    write_if(root / "data/movement_tables_native.inc", lines, args)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", default=".")
    ap.add_argument("elf", nargs="?",
                    default="../pokeemerald-reference/pokeemerald.elf")
    ap.add_argument("rom", nargs="?",
                    default="../pokeemerald-reference/pokeemerald.gba")
    ap.add_argument("outdir", nargs="?",
                    default="resources/extraction/emerald/bpee01")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    global root, rom
    root = Path(args.root)
    outdir = Path(args.outdir)
    pret = Path(args.elf).parent
    rom = open(args.rom, "rb").read()
    if len(rom) != ROM_SIZE:
        fail(f"ROM size {len(rom)} != 16 MiB")
    if hashlib.sha1(rom).hexdigest() != ROM_SHA1:
        fail(f"ROM SHA-1 != {ROM_SHA1} (retail-matching ROM required)")

    elf = Elf32(open(args.elf, "rb").read())
    if elf.sec_name(MOVEMENT_LABEL_SECTION) != "script_data":
        fail(f"ELF section {MOVEMENT_LABEL_SECTION} is "
             f"'{elf.sec_name(MOVEMENT_LABEL_SECTION)}', expected script_data")
    if elf.sec_name(9) != "other_data":
        fail(f"ELF section 9 is '{elf.sec_name(9)}', expected other_data")

    movement = resolve_movement(elf, rom, pret, args)
    multiboot = resolve_multiboot(elf, rom, root, pret)

    emit_family(movement, "movement", outdir, args, "leaf")
    emit_family(multiboot, "multiboot", outdir, args, "program")
    emit_seam_header(movement + multiboot, root, args)
    emit_movement_native_include(movement, root, args)

    lb = sum(r[3] for r in movement)
    mb = sum(r[3] for r in multiboot)
    print(f"movement resources: {len(movement)} "
          f"({MOVEMENT_PINNED_LABELS} labels + {MOVEMENT_PINNED_OBJECTS} "
          f"objects), {lb} B")
    print(f"multiboot programs: {len(multiboot)}, {mb} B "
          f"(ereader {MULTIBOOT_PROGRAMS[0][3]} B, colosseum "
          f"{MULTIBOOT_PROGRAMS[1][3]} B)")
    print(f"leaf seam slots: {len(movement) + len(multiboot)} "
          f"(header {root / 'include/emerald/resources/leaf_native.generated.h'})")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""R13-D1: gameplay-data generator for the species/move/shared/font families
from the qualified pret reference ELF + retail ROM.

Derives every D1 resource from the reference ELF symbol table + the retail
ROM and emits resources/extraction/emerald/bpee01/gameplay/ (six generated
TOML files + row artifacts) plus the native seam tables/inventory.

Canonical payloads are EXACT padded GBA wire slices (the vanilla ROM row
layouts: species base 28 B, battle move 12 B, evolution 8 B, contest move
8 B, contest effect 4 B, item 44 B (D2)). The native packed-struct transform
is owned entirely by the publication seam (EmeraldGameplayCompat), never the
pack. Schema codes (approved plan §4):
   1 species-base | 2 species-name | 3 species-evolutions | 4 species-levelup
   5 species-tmhm  | 6 species-tutor | 7 species-egg-moves | 8 move-battle
   9 move-name    | 10 move-contest | 11 item (D2)          | 12 growth-rate
  13 tutor-moves  | 14 contest-effects | 15 contest-combo-starters
    font schema 1.

Keys follow the approved plan verbatim:
   emerald:data/species/<sp>[/name|/evolutions|/levelup|/tmhm|/tutor|/egg-moves]
   emerald:data/move/<mv>[/name|/contest]
   emerald:data/growth-rate/<rate>
   emerald:data/tutor/moves | emerald:data/contest/effects |
   emerald:data/contest/combo-starters
   emerald:font/<font>

Pinned D1 counts (reference-verified, corrected by the parity gate):
species subtotal 2,224 resources / 32,056 B (base + name + learnset + tmhm +
tutor + egg ; evolutions excluded -- diverged fork family, stays compiled);
moves 1,065 / 11,715 B; shared 11 / 3,547 B; fonts 10 / 294,912 B;
total 3,310 resources / 342,230 B. Enforced as hard gates; --check must be a
no-op diff.

Usage:
  gen_gameplay_family.py [root] [elf] [rom] [outdir] [--check]
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

# ------------------------------------------------------------------ pins
# D1 = 3310 resources / 342230 B. NOTE: the evolution family (412 x 40 B =
# 16480 B) is EXCLUDED from D1: the compile-vs-vanilla parity gate proved the
# recomp deliberately replaced the 12 vanilla trade evolutions (Kadabra/
# Machoke/Graveler/Haunter -> level 40; Poliwhirl et al. -> item) with
# level/item evolutions (src/data/pokemon/evolution.h). Publishing vanilla
# evolution data would silently overwrite that fork behaviour, which the R13-D
# stage brief forbids (STOP for that family; keep compiled). D1 therefore
# ships every parity-passing family and leaves gEvolutionTable compiled.
PINNED_TOTAL = 3310
PINNED_BYTES = 342230
PINNED_BY_FAMILY = {
    "species-base": (412, 11536),
    "species-name": (412, 4532),
    "species-levelup": (411, 8766),
    "species-tmhm": (412, 3296),
    "species-tutor": (412, 1648),
    "species-egg-moves": (165, 2278),
    "move-battle": (355, 4260),
    "move-name": (355, 4615),
    "move-contest": (355, 2840),
    "growth-rate": (8, 3232),
    "tutor-moves": (1, 60),
    "contest-effects": (1, 192),
    "contest-combo-starters": (1, 63),
    "font": (10, 294912),
}

# (font key, reference ELF symbol)
FONTS = [
    ("small-narrow-latin", "gFontSmallNarrowLatinGlyphs"),
    ("small-latin", "gFontSmallLatinGlyphs"),
    ("narrow-latin", "gFontNarrowLatinGlyphs"),
    ("short-latin", "gFontShortLatinGlyphs"),
    ("normal-latin", "gFontNormalLatinGlyphs"),
    ("small-japanese", "gFontSmallJapaneseGlyphs"),
    ("normal-japanese", "gFontNormalJapaneseGlyphs"),
    ("frlg-male-japanese", "gFontFRLGMaleJapaneseGlyphs"),
    ("frlg-female-japanese", "gFontFRLGFemaleJapaneseGlyphs"),
    ("short-japanese", "gFontShortJapaneseGlyphs"),
]

GROWTH_NAMES = ["medium-fast", "erratic", "fluctuating", "medium-slow",
                "fast", "slow", "unused-6", "unused-7"]


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
        self.symbols = []  # (name, value, size, info, shndx) sorted by value
        entsize = 16
        for off in range(stab["offset"], stab["offset"] + stab["size"],
                         entsize):
            (name_off, value, size, info, other, shndx) = struct.unpack_from(
                "<IIIBBH", data, off)
            end = self.strtab.find(b"\0", name_off)
            name = self.strtab[name_off:end].decode("ascii", "replace")
            self.symbols.append((name, value, size, info, shndx))
        self.symbols.sort(key=lambda s: s[1])

    def sec_name(self, idx):
        s = self.sections[idx]
        end = self.shstr.find(b"\0", s["name"])
        return self.shstr[s["name"]:end].decode("ascii", "replace")

    def find(self, name):
        for sym in self.symbols:
            if sym[0] == name:
                return sym
        return None

    def object_syms(self, shndx):
        return [s for s in self.symbols if (s[3] & 0xf) == 1 and s[4] == shndx]

    def slice(self, sym, size):
        """File bytes backing [value, value+size). `sym` = (name,value,...)."""
        _, value, _, _, shndx = sym
        sect = self.sections[shndx]
        off = sect["offset"] + (value - sect["addr"])
        return self.data[off:off + size]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def derive_key(canonical_id):
    """M0/M1 resource identity key: SHA-256(\"gen3-resource-id-v1\\0\" + id),
    matching Gen3ResourceId_DeriveKey (src/gen3/resources/resource_id.c)."""
    return hashlib.sha256(
        b"gen3-resource-id-v1\x00" + canonical_id.encode("ascii")).hexdigest()


def semantic_name(prefix, name):
    """'SPECIES_BULBASAUR' -> 'bulbasaur'; 'MOVE_VICE_GRIP' -> 'vice-grip'."""
    return name[len(prefix):].lower().replace("_", "-")


def parse_enum(regex_prefix, path):
    """Parse '#define PREFIX_NAME value' into {value: name}. Later defines
    override earlier ones so the semantics match the compiled array index
    (the ROM's index enumeration is the ground truth)."""
    text = path.read_text()
    out = {}
    for m in re.finditer(r"#define\s+(" + regex_prefix +
                         r"[A-Z0-9_]+)\s+(\d+)", text):
        out[int(m.group(2))] = m.group(1)
    return out


def derive(elf, rom, pret):
    """Return a sorted list of (key, symbol, legacy_symbol, size, rom_off)
    rows plus a dict family -> resource count. legacy_symbol is the native
    fill-target array symbol (what the isolation nm must see as HOST_DATA),
    equals the reference symbol for these families (names unchanged)."""
    species_names = parse_enum("SPECIES_", pret / "include/constants/species.h")
    move_names = parse_enum("MOVE_", pret / "include/constants/moves.h")
    # Indices 0..411 must each carry a species define (SPECIES_EGG=412 is the
    # array-count sentinel and is ignored here).
    if any(i not in species_names for i in range(412)):
        fail("species enum must map indices 0..411 exactly")
    if any(i not in move_names for i in range(355)):
        fail("move enum must map indices 0..354 exactly")

    sp_key = lambda i: "emerald:data/species/" + semantic_name("SPECIES_", species_names[i])  # noqa: E731
    mv_key = lambda i: "emerald:data/move/" + semantic_name("MOVE_", move_names[i])  # noqa: E731

    rows = []
    fam = {}

    def add(family, key, symbol, size, rom_off, legacy_symbol=None):
        rows.append((key, symbol, legacy_symbol or symbol, size, rom_off))
        fam[family] = fam.get(family, 0) + 1

    def sym_bytes(name):
        sym = elf.find(name)
        if sym is None:
            fail(f"symbol '{name}' not in the reference ELF")
        return sym, elf.slice(sym, sym[2]), sym[2]

    def rows_from(symbol_name, row_size, family, keyfn, count):
        sym, data, size = sym_bytes(symbol_name)
        if size != row_size * count:
            fail(f"{symbol_name}: {size} != {row_size}x{count}")
        for i in range(count):
            off = i * row_size
            slice_ = data[off:off + row_size]
            rom_off = sym[1] - GEN3_GBA_ROM_BASE + off
            if rom[rom_off:rom_off + row_size] != slice_:
                fail(f"{symbol_name}[{i}]: ROM slice != ELF slice")
            add(family, keyfn(i), symbol_name, row_size, rom_off)

    # --- species base (gBaseStats, 28 B padded rows) --------------------
    rows_from("gBaseStats", 28, "species-base", sp_key, 412)
    rows_from("gSpeciesNames", 11, "species-name",
              lambda i: sp_key(i) + "/name", 412)
    # gEvolutionTable is NOT generated: the parity gate proxes a deliberate
    # recomp trade-evolution divergence (see the module header + pins). The
    # family stays compiled and is not part of the D1 pack.
    rows_from("gTMHMLearnsets", 8, "species-tmhm",
              lambda i: sp_key(i) + "/tmhm", 412)
    rows_from("sTutorLearnsets", 4, "species-tutor",
              lambda i: sp_key(i) + "/tutor", 412)

    # --- level-up learnsets: per-species leaves --------------------------
    # 412 pointer entries, 411 DISTINCT leaves: SPECIES_NONE (index 0) shares
    # sBulbasaurLevelUpLearnset with SPECIES_BULBASAUR (index 1) -- never a
    # NULL entry. Emit ONE resource per distinct leaf (411), keyed by the
    # owning species index (the minimum index > 0 that references it, so the
    # shared leaf keys to bulbasaur, never "none"). The generated
    # species-index -> resource map lets the seam rebuild the pointer table
    # preserving index 0 == bulbasaur's leaf.
    ptr_sym, ptr_data, _ = sym_bytes("gLevelUpLearnsets")
    ptrs = list(struct.unpack_from("<%dI" % (len(ptr_data) // 4), ptr_data, 0))
    if len(ptrs) != 412:
        fail(f"gLevelUpLearnsets entries {len(ptrs)} != 412")
    leaf_by_addr = {s[1]: s for s in elf.object_syms(6)
                    if s[0].startswith("s") and s[0].endswith("LevelUpLearnset")}
    if len(leaf_by_addr) != 411:
        fail(f"distinct level-up leaves {len(leaf_by_addr)} != 411")
    addr_indices = {}
    for i, p in enumerate(ptrs):
        addr_indices.setdefault(p, []).append(i)
    if any(p != 0 and p not in leaf_by_addr for p in ptrs):
        fail("a gLevelUpLearnsets pointer has no matching leaf symbol")
    levelup_idx_map = {}  # species index -> canonical levelup resource key
    for addr, idxs in addr_indices.items():
        if addr == 0:
            fail("unexpected NULL level-up pointer (vanilla has none)")
        owner = min([i for i in idxs if i > 0] or [0])
        leaf = leaf_by_addr[addr]
        size = leaf[2]
        slice_ = elf.slice(leaf, size)
        rom_off = leaf[1] - GEN3_GBA_ROM_BASE
        if rom[rom_off:rom_off + size] != slice_:
            fail(f"levelup leaf '{leaf[0]}': ROM slice != ELF slice")
        key = sp_key(owner) + "/levelup"
        add("species-levelup", key, leaf[0], size, rom_off)
        for i in idxs:
            levelup_idx_map[i] = key
    if (_cnt(fam, "species-levelup") != 411
            or sorted(levelup_idx_map) != list(range(412))):
        fail("level-up leaf composition must be 411 resources covering "
             "indices 0..411")

    # --- egg moves: split the flat u16 stream into per-species blocks -----
    # Blocks: header u16 = 0x4000|species, then moves until the next block
    # header or the global 0xFFFF terminator. The terminator rides the last
    # block, so the concatenation of every block slice reproduces the full
    # 2,278-byte stream exactly (exact provenance + byte parity).
    egg_sym, egg_data, egg_size = sym_bytes("gEggMoves")
    words = struct.unpack_from("<%dH" % (egg_size // 2), egg_data, 0)
    # EGG_MOVES_SPECIES_OFFSET = 20000 = 0x4E20; header = offset + species.
    mask = [0x4E20 <= w < 0x4E20 + 412 for w in words]
    idxs = [i for i in range(len(words)) if mask[i]]
    if not idxs:
        fail("gEggMoves has no species block headers")
    boundaries = []
    for k, hdr in enumerate(idxs):
        start = hdr
        end = idxs[k + 1] if k + 1 < len(idxs) else len(words)
        boundaries.append((start, end))
    # Last block extends through the global terminator (if present).
    if words and words[-1] == 0xFFFF:
        last_start, _ = boundaries[-1]
        boundaries[-1] = (last_start, len(words))
    if len(boundaries) != 165:
        fail(f"egg-move blocks {len(boundaries)} != 165")
    for idx, end in boundaries:
        species = words[idx] - 0x4E20
        if species < 0 or species not in species_names:
            fail(f"egg-move block species {species} not in species enum")
        blk_off = idx * 2
        blk_size = (end - idx) * 2
        slice_ = egg_data[blk_off:blk_off + blk_size]
        rom_off = egg_sym[1] - GEN3_GBA_ROM_BASE + blk_off
        if rom[rom_off:rom_off + blk_size] != slice_:
            fail(f"egg-move block {species}: ROM slice != ELF slice")
        add("species-egg-moves", sp_key(species) + "/egg-moves",
            "gEggMoves", blk_size, rom_off)

    # --- moves -------------------------------------------------------------
    rows_from("gBattleMoves", 12, "move-battle", mv_key, 355)
    rows_from("gMoveNames", 13, "move-name", lambda i: mv_key(i) + "/name", 355)
    rows_from("gContestMoves", 8, "move-contest",
              lambda i: mv_key(i) + "/contest", 355)

    # --- shared tables ------------------------------------------------------
    ex_sym, ex_data, ex_size = sym_bytes("gExperienceTables")
    if ex_size != 8 * 404:
        fail(f"gExperienceTables size {ex_size} != 3232")
    # verify row 0 = medium fast (n^3)
    row0 = list(struct.unpack_from("<11I", ex_data, 0))
    if row0 != [i ** 3 for i in range(11)]:
        fail("gExperienceTables row 0 is not the cubic medium-fast curve")
    for r in range(8):
        off = r * 404
        slice_ = ex_data[off:off + 404]
        rom_off = ex_sym[1] - GEN3_GBA_ROM_BASE + off
        if rom[rom_off:rom_off + 404] != slice_:
            fail(f"gExperienceTables row {r}: ROM slice != ELF slice")
        add("growth-rate", "emerald:data/growth-rate/" + GROWTH_NAMES[r],
            "gExperienceTables", 404, rom_off)
    rows_from("gTutorMoves", 60, "tutor-moves",
              lambda i: "emerald:data/tutor/moves", 1)
    rows_from("gContestEffects", 192, "contest-effects",
              lambda i: "emerald:data/contest/effects", 1)
    rows_from("gComboStarterLookupTable", 63, "contest-combo-starters",
              lambda i: "emerald:data/contest/combo-starters", 1)

    # --- fonts ---------------------------------------------------------------
    for key, symbol in FONTS:
        sym, data, size = sym_bytes(symbol)
        rom_off = sym[1] - GEN3_GBA_ROM_BASE
        if rom[rom_off:rom_off + size] != data:
            fail(f"{symbol}: ROM slice != ELF slice")
        add("font", "emerald:font/" + key, symbol, size, rom_off)

    rows.sort(key=lambda r: r[0])
    total = len(rows)
    if total != PINNED_TOTAL:
        fail(f"total resources {total} != pinned {PINNED_TOTAL}")
    if sum(r[3] for r in rows) != PINNED_BYTES:
        fail(f"total bytes {sum(r[3] for r in rows)} != pinned {PINNED_BYTES}")
    for family, (count, bcount) in PINNED_BY_FAMILY.items():
        got_c = fam.get(family, 0)
        got_b = sum(r[3] for r in rows if _f(r[0], family) == family)
        if got_c != count or got_b != bcount:
            fail(f"family {family}: {got_c} res / {got_b} B != pinned "
                 f"{count} / {bcount}")
    species_keys = [sp_key(i) for i in range(412)]
    move_keys = [mv_key(i) for i in range(355)]
    return rows, fam, levelup_idx_map, species_keys, move_keys


def _f(key, family):
    # classify a key by its family tag (used only for the pinned family
    # subtotal cross-check above)
    if key.startswith("emerald:data/species/"):
        sub = key.rsplit("/", 1)[-1]
        return {"name": "species-name", "evolutions": "species-evolutions",
                "levelup": "species-levelup", "tmhm": "species-tmhm",
                "tutor": "species-tutor",
                "egg-moves": "species-egg-moves"}.get(sub, "species-base")
    if key.startswith("emerald:data/move/"):
        sub = key.rsplit("/", 1)[-1]
        return {"name": "move-name", "contest": "move-contest"}.get(
            sub, "move-battle")
    if key.startswith("emerald:data/growth-rate/"):
        return "growth-rate"
    if key == "emerald:data/tutor/moves":
        return "tutor-moves"
    if key == "emerald:data/contest/effects":
        return "contest-effects"
    if key == "emerald:data/contest/combo-starters":
        return "contest-combo-starters"
    if key.startswith("emerald:font/"):
        return "font"
    return "?"


def _cnt(fam, family):
    return fam.get(family, 0)


# ------------------------------------------------------------------ emission
def write_if(path, lines, args):
    text = "\n".join(lines) + "\n"
    if args.check:
        if not path.exists():
            fail(f"--check: {path} does not exist")
        if path.read_text() != text:
            fail(f"--check: {path} differs from deterministic regeneration")
        print(f"check passed: {path}")
    else:
        path.write_text(text)
        print(f"wrote {path}")


SCHEMA = {
    "species-base": 1, "species-name": 2, "species-evolutions": 3,
    "species-levelup": 4, "species-tmhm": 5, "species-tutor": 6,
    "species-egg-moves": 7, "move-battle": 8, "move-name": 9,
    "move-contest": 10, "growth-rate": 12, "tutor-moves": 13,
    "contest-effects": 14, "contest-combo-starters": 15, "font": 1,
}


def resource_type(family):
    return "font" if family == "font" else "structured-data"


def key_sanitize(key):
    """Artifact filename: canonical key with '/' and ':' replaced."""
    return key.replace(":", "%3A").replace("/", "_")


def emit_gameplay(rows, fam, levelup_idx_map, species_keys, move_keys,
                  outdir, root, args):
    famdir = outdir / "gameplay"
    artdir = famdir / "artifacts"
    art_by_key = {}
    for key, symbol, legacy, size, rom_off in rows:
        sub = _artifact_sub(key)
        rel = (f"resources/extraction/emerald/bpee01/gameplay/artifacts/"
               f"{sub}/{key_sanitize(key)}.bin")
        art_by_key[key] = rel
        art = root / rel
        data = rom[rom_off:rom_off + size]
        if args.check:
            if not art.exists() or art.read_bytes() != data:
                fail(f"--check: {art} differs from ROM-slice regeneration")
        else:
            art.parent.mkdir(parents=True, exist_ok=True)
            art.write_bytes(data)

    # ---- inventory -----------------------------------------------------
    inv_lines = [
        "# Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "inventory_version = 1",
        'family = "gameplay"',
        'game = "emerald"',
        'rom_profile = "bpee01"',
        "",
        f"# D1 gameplay-data family. {len(rows)} resources, "
        f"{sum(r[3] for r in rows)} B canonical (exact padded GBA wire).",
        "",
        "[[families]]",
        'kind = "gameplay"',
        f"symbol_count = {len(rows)}",
        'resource_type = "structured-data"',
        'representation = "gba-bytes"',
        "",
    ]
    write_if(famdir / "inventory.generated.toml", inv_lines, args)

    # ---- catalog -------------------------------------------------------
    cat_lines = [
        "# Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py",
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
    for key, symbol, legacy, size, rom_off in rows:
        fam1 = _f(key, "x")
        cat_lines.append("[[resources]]")
        cat_lines.append(f'id = "{key}"')
        cat_lines.append(f'type = "{resource_type(fam1)}"')
        cat_lines.append(f"schema = {SCHEMA[fam1]}")
        cat_lines.append("required_for_base = true")
        cat_lines.append("")
    write_if(famdir / "catalog.generated.toml", cat_lines, args)

    # ---- bindings --------------------------------------------------------
    bin_lines = [
        "# Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Semantic extraction bindings for the gameplay family. Every",
        "# resource is a ROM slice at (symbol value + row offset); the",
        "# production manifest is emitted directly by this generator",
        "# (gen3-elf-manifest cannot express a per-row sub-slice of a table",
        "# symbol). Offsets are never hand-maintained.",
        "",
        "bindings_version = 1",
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    for key, symbol, legacy, size, rom_off in rows:
        bin_lines.append("[[bindings]]")
        bin_lines.append(f'id = "{key}"')
        bin_lines.append(f'symbol = "{symbol}"')
        bin_lines.append(f'source_artifact = "{art_by_key[key]}"')
        bin_lines.append('source_encoding = "raw"')
        bin_lines.append('canonical_representation = "gba-bytes"')
        bin_lines.append(f"expected_decoded_size = {size}")
        bin_lines.append("")
    write_if(famdir / "bindings.generated.toml", bin_lines, args)

    # ---- ownership --------------------------------------------------------
    own_lines = [
        "# Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Native ownership for the D1 live families: all ROM_BASE_ONLY. The",
        "# canonical payload lives in the pack; EmeraldGameplayCompat fills",
        "# the native HOST_DATA fill-target arrays. legacy_symbol is the",
        "# fill-target array symbol (present as a HOST_DATA array, never a",
        "# compiled const payload on native; the const original is guarded",
        "# out). cro isolation is proven by the D1 battery: transform oracle",
        "# + section-scoped nm + font/payload scans + content.pak removal.",
        "",
        "ownership_version = 1",
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    for key, symbol, legacy, size, rom_off in rows:
        fam1 = _f(key, "x")
        data = rom[rom_off:rom_off + size]
        h = sha256(data)
        own_lines.append("[[resources]]")
        own_lines.append(f'id = "{key}"')
        own_lines.append(f'key = "{derive_key(key)}"')
        own_lines.append(f'legacy_symbol = "{legacy}"')
        own_lines.append(f'type = "{resource_type(fam1)}"')
        own_lines.append(f"schema = {SCHEMA[fam1]}")
        own_lines.append(f'source_artifact = "{art_by_key[key]}"')
        own_lines.append(f"encoded_length = {size}")
        own_lines.append(f"decoded_length = {size}")
        own_lines.append('source_encoding = "raw"')
        own_lines.append(f'source_encoded_sha256 = "{h}"')
        own_lines.append(f'canonical_decoded_sha256 = "{h}"')
        own_lines.append('ownership_state = "ROM_BASE_ONLY"')
        own_lines.append("")
        own_lines.append("[resources.targets]")
        own_lines.append('native = "ROM_BASE_ONLY"')
        own_lines.append('gba = "COMPILED"')
        own_lines.append("")
    write_if(famdir / "ownership.generated.toml", own_lines, args)

    # ---- consumers ---------------------------------------------------------
    con_lines = [
        "# Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Machine-readable consumer map (per-family summary; see the D1",
        "# report consumer-seams section for the full tracing).",
        'family = "gameplay"',
        "",
    ]
    for family in sorted(PINNED_BY_FAMILY):
        con_lines.append("[[consumer_sites]]")
        con_lines.append(f'id = "{family}"')
        con_lines.append(f"count = {fam.get(family, 0)}")
        con_lines.append("")
    write_if(famdir / "consumers.generated.toml", con_lines, args)

    # ---- production manifest (direct emission, per-row rom_offset) --------
    man_lines = [
        "# Deterministic extraction manifest - generated by "
        "tools/gen3_resources/gameplay_family/gen_gameplay_family.py.",
        "# Do not edit by hand; regenerate with --check.",
        "# Records are sorted bytewise by canonical resource id.",
        f"# provenance: qualified pret reference build ELF + retail "
        f"BPEE01 Rev 0 (SHA-1 {ROM_SHA1})",
        "manifest_version = 1",
        'namespace = "emerald"',
        'resource_api = "1.0.0"',
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        'qualification = "production"',
        f"rom_size = {ROM_SIZE}",
        f'rom_sha1 = "{ROM_SHA1}"',
        f'rom_sha256 = "{sha256(rom)}"',
        "",
    ]
    for key, symbol, legacy, size, rom_off in rows:
        fam1 = _f(key, "x")
        data = rom[rom_off:rom_off + size]
        h = sha256(data)
        man_lines.append("[[records]]")
        man_lines.append(f'id = "{key}"')
        man_lines.append(f'key = "{derive_key(key)}"')
        man_lines.append(f'type = "{resource_type(fam1)}"')
        man_lines.append(f"schema = {SCHEMA[fam1]}")
        man_lines.append(f'symbol = "{symbol}"')
        man_lines.append(f"rom_offset = {rom_off}")
        man_lines.append(f"encoded_length = {size}")
        man_lines.append(f"decoded_length = {size}")
        man_lines.append('source_encoding = "raw"')
        man_lines.append(f'source_encoded_sha256 = "{h}"')
        man_lines.append(f'canonical_decoded_sha256 = "{h}"')
        man_lines.append("")
    write_if(famdir / "manifest.production.toml", man_lines, args)

    # ---- native seam artifacts ---------------------------------------------
    header = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-D1 seam inventory for the gameplay-data families. Enumerates",
        " * every canonical resource the production pack publishes for the",
        " * D1 species/move/shared/font families, with the canonical id, the",
        " * native fill-target symbol, canonical size and the family schema.",
        " * The gameplay seam (EmeraldGameplayCompat) validates the session's",
        " * pack against this exact table (name/size/schema set equality)",
        " * before computing any fill -- a missing entry, a size/schema",
        " * drift, or an unexpected extra is a REFUSED publication.",
        " */",
        "#ifndef EMERALD_RESOURCES_GAMEPLAY_NATIVE_GENERATED_H",
        "#define EMERALD_RESOURCES_GAMEPLAY_NATIVE_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define GAMEPLAY_NATIVE_RESOURCE_COUNT {len(rows)}u",
        f"#define GAMEPLAY_NATIVE_SPECIES_COUNT 412u",
        "",
        "struct GameplayNativeResource",
        "{",
        "    const char *name;         /* canonical resource id */",
        "    const char *legacySymbol; /* native fill-target symbol */",
        "    uint32_t size;            /* canonical payload bytes */",
        "    uint8_t schema;           /* D1 family schema code */",
        "};",
        "",
        "extern const struct GameplayNativeResource "
        "kGameplayNativeResources[GAMEPLAY_NATIVE_RESOURCE_COUNT];",
        "",
        "/* species index -> levelup resource canonical key (shared leaf:",
        " * SPECIES_NONE points at bulbasaur's leaf). The seam rebuilds the",
        " * native gLevelUpLearnsets pointer table from this map. */",
        "extern const char *const "
        "kGameplayLevelupSpeciesKeys[GAMEPLAY_NATIVE_SPECIES_COUNT];",
        "",
        "/* species index -> base species resource key (emerald:data/species/",
        " * <name>); sub-keys add /name, /tmhm, /tutor, /egg-moves. */",
        "extern const char *const "
        "kGameplaySpeciesKeys[GAMEPLAY_NATIVE_SPECIES_COUNT];",
        "/* move index -> base move resource key (emerald:data/move/<name>);",
        " * sub-keys add /name, /contest. */",
        "#ifndef GAMEPLAY_NATIVE_MOVE_COUNT",
        "#define GAMEPLAY_NATIVE_MOVE_COUNT 355u",
        "#endif",
        "extern const char *const "
        "kGameplayMoveKeys[GAMEPLAY_NATIVE_MOVE_COUNT];",
        "",
        "#endif /* EMERALD_RESOURCES_GAMEPLAY_NATIVE_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/gameplay_native.generated.h",
             header, args)

    c_lines = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/gameplay_native.generated.h"',
        "",
        "const struct GameplayNativeResource "
        "kGameplayNativeResources[GAMEPLAY_NATIVE_RESOURCE_COUNT] =",
        "{",
    ]
    for key, symbol, legacy, size, rom_off in rows:
        fam1 = _f(key, "x")
        c_lines.append(f'    {{"{key}", "{legacy}", {size}u, '
                       f'{SCHEMA[fam1]}u}},')
    c_lines += [
        "};",
        "",
    ]
    write_if(root / "src/emerald/resources/gameplay_native_table.generated.c",
             c_lines, args)

    lvl_c_lines = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/gameplay_native.generated.h"',
        "",
        "/* species index -> levelup resource key. SPECIES_NONE (index 0)",
        " * shares bulbasaur's leaf, so it maps to the same key the seam",
        " * uses to reconstruct the native gLevelUpLearnsets pointer table,",
        " * preserving vanilla sharing exactly. */",
        "const char *const "
        "kGameplayLevelupSpeciesKeys[GAMEPLAY_NATIVE_SPECIES_COUNT] =",
        "{",
    ]
    for i in range(412):
        lvl_c_lines.append(f'    "{levelup_idx_map[i]}",')
    lvl_c_lines += [
        "};",
        "",
    ]
    lvl_c_lines += [
        "const char *const "
        "kGameplaySpeciesKeys[GAMEPLAY_NATIVE_SPECIES_COUNT] =",
        "{",
    ]
    for k in species_keys:
        lvl_c_lines.append(f'    "{k}",')
    lvl_c_lines += [
        "};",
        "",
        "const char *const kGameplayMoveKeys[GAMEPLAY_NATIVE_MOVE_COUNT] =",
        "{",
    ]
    for k in move_keys:
        lvl_c_lines.append(f'    "{k}",')
    lvl_c_lines += [
        "};",
        "",
    ]
    write_if(root / "src/emerald/resources/gameplay_levelup.generated.c",
             lvl_c_lines, args)


def _artifact_sub(key):
    if key.startswith("emerald:data/species/"):
        sub = key.rsplit("/", 1)[-1]
        return {"name": "species-name", "evolutions": "evolutions",
                "levelup": "levelup", "tmhm": "tmhm", "tutor": "tutor",
                "egg-moves": "egg-moves"}.get(sub, "species-base")
    if key.startswith("emerald:data/move/"):
        return "move-name" if key.endswith("/name") else (
            "move-contest" if key.endswith("/contest") else "move-battle")
    if key.startswith("emerald:data/growth-rate/"):
        return "growth-rate"
    if key == "emerald:data/tutor/moves":
        return "tutor-moves"
    if key == "emerald:data/contest/effects":
        return "contest-effects"
    if key == "emerald:data/contest/combo-starters":
        return "contest-combo-starters"
    return "font"


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
    rows, fam, levelup_idx_map, species_keys, move_keys = derive(
        elf, rom, pret)
    emit_gameplay(rows, fam, levelup_idx_map, species_keys, move_keys,
                  outdir, root, args)

    print(f"gameplay resources: {len(rows)}, "
          f"{sum(r[3] for r in rows)} B (D1 subtotal; D2 items excluded)")
    for family in sorted(PINNED_BY_FAMILY):
        print(f"  {family}: {fam.get(family, 0)} resources")


if __name__ == "__main__":
    main()

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
import unicodedata
from pathlib import Path

GEN3_GBA_ROM_BASE = 0x08000000
ROM_SIZE = 0x1000000
ROM_SHA1 = "f3ae088181bf583e55daf962a92bb46f4f1d07b7"

# ------------------------------------------------------------------ pins
# R13-D1 = 3310 resources / 342230 B (the diverged evolution family is
# excluded -- see note below). R13-D2 adds the gItems family (377 x 44 B GBA
# wire rows = 16588 B, schema 11=item). R13-E1 adds the three trainer
# families (schema 16 metadata 855 x 40 B = 34200 B; schema 17 party leaves
# 854 = 18088 B; schema 18 class names 66 x 13 B = 858 B). R13-E2 adds the
# wild-encounter families (schema 19 headers block = 1 x 2500 B; schema 20
# per-(map,type) slot tables = 209 resources / 7,900 B). Combined gameplay +
# trainer + encounter total:
#    5672 resources / 422364 B.
#
# NOTE (R13-D1): the evolution family (412 x 40 B = 16480 B) is EXCLUDED: the
# compile-vs-vanilla parity gate proved the recomp deliberately replaced the
# 12 vanilla trade evolutions (Kadabra/Machoke/Graveler/Haunter -> level 40;
# Poliwhirl et al. -> item) with level/item evolutions
# (src/data/pokemon/evolution.h). Publishing vanilla evolution data would
# silently overwrite that fork behaviour, which the R13-D stage brief forbids
# (STOP for that family; keep compiled).
#
# R13-E3a-1 adds the frontier families (schema 21 trainer metadata 300 x 52 B
# = 15600 B; schema 22 mon-set index-stream leaves 300 = 28060 B; schema 23
# shared 882-mons pool 1 x 14112 B; schema 24 held-items 126 B; schema 25
# banned-species 22 B) plus the three Battle Tent families (tent-trainer 90 x
# 52 B = 4680 B; tent-mon-set 90 leaves = 1558 B; tent-mon 3 pools =
# 2560 B). Mon index-stream model confirmed from the ROM: every frontier
# trainer's monSet GBA ptr resolves to a distinct 0xFFFF-terminated leaf and
# every leaf index is 0..881; every tent trainer's monSet ptr resolves to a
# leaf whose indices are within that tent's own pool (slateport 70 /
# verdanturf 45 / fallarbor 45). The 300 frontier leaves tile
# [0x5ced2e, 0x5d5aca) exactly (the 2 trailing pad bytes before
# gBattleFrontierTrainers at 0x5d5acc are not part of any leaf).
PINNED_TOTAL = 6908
PINNED_BYTES = 509975
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
    "item": (377, 16588),
    "trainer": (855, 34200),
    "trainer-party": (854, 18088),
    "trainer-class-name": (66, 858),
    "encounter-headers": (1, 2500),
    "encounter": (209, 7900),
    "frontier-trainer": (300, 15600),
    "frontier-mon-set": (300, 28060),
    "frontier-mon": (1, 14112),
    "frontier-held-items": (1, 126),
    "frontier-banned-species": (1, 22),
    "tent-trainer": (90, 4680),
    "tent-mon-set": (90, 1558),
    "tent-mon": (3, 2560),
    "font": (10, 294912),
    "frontier-factory": (7, 314),
    "frontier-palace": (2, 30),
    "frontier-arena": (2, 30),
    "frontier-pike-npc": (1, 200),
    "frontier-pike-speech": (11, 819),
    "frontier-pyramid-floor": (2, 324),
    "frontier-pyramid-item": (1, 400),
    "frontier-pyramid-slots": (1, 126),
    "frontier-brain": (3, 882),
    "frontier-apprentice": (16, 1408),
    "frontier-wild-headers": (2, 260),
    "frontier-wild": (11, 528),
    # R13-E3b Pokédex: 387 rows x 32 B GBA wire + the ordering/routing u16
    # tables (alphabetical 411 / height 386 / weight 386 / species-to-national
    # 411). The description text stays an R13-C text identity (never a
    # structured resource here).
    "pokedex-row": (387, 12384),
    "pokedex-order": (3, 2366),
    "pokedex-species-to-national": (1, 822),
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


def callback_kind_suffix(symbol):
    """Split a reference item-use symbol into (kind, suffix), e.g.
    ItemUseOutOfBattle_Medicine -> ('OUTOFBATTLE', 'Medicine')."""
    for kind, prefix in (("OUTOFBATTLE", "ItemUseOutOfBattle_"),
                         ("INBATTLE", "ItemUseInBattle_")):
        if symbol.startswith(prefix):
            return kind, symbol[len(prefix):]
    fail(f"reference item-use symbol '{symbol}' has an unexpected prefix")


def callback_token(symbol):
    """Stable C enum token for a reference item-use symbol."""
    kind, suffix = callback_kind_suffix(symbol)
    return ("GAMEPLAY_ITEM_USE_ACTION_" + kind + "_" + suffix).upper()


def action_id(symbol):
    """Stable semantic id for a reference item-use symbol, e.g.
    item-use/out-of-battle/medicine."""
    kind, suffix = callback_kind_suffix(symbol)
    return "item-use/" + kind.lower().replace("outofbattle", "out-of-battle") \
        .replace("inbattle", "in-battle") + "/" + suffix.lower().replace("_", "-")


def semantic_name(prefix, name):
    """'SPECIES_BULBASAUR' -> 'bulbasaur'; 'MOVE_VICE_GRIP' -> 'vice-grip'."""
    return name[len(prefix):].lower().replace("_", "-")


def class_resource_suffix(display):
    """Display string -> ASCII resource segment satisfying the M0/M1 name
    grammar ([a-z0-9._-] per IsSegmentCharacter in src/gen3/resources/
    resource_id.c). The charmap display strings carry non-ASCII glyphs (the
    gender signs in SWIMMER♂/♀ and the accented é in POKéMANIAC/POKéFAN)
    and the animated-text brace tokens ({PKMN}); NFKD-transliterate é -> e,
    map the sex glyphs (♂ -> m, ♀ -> f), spaces -> '-', and drop every
    character the grammar forbids. The result stays readable and unique per
    class; duplicate display strings are disambiguated separately by
    appending the class index. """
    s = unicodedata.normalize("NFKD", display)
    s = "".join(ch for ch in s if not unicodedata.combining(ch))
    s = s.replace("\u2642", "m").replace("\u2640", "f")
    s = s.replace(" ", "-").lower()
    allowed = "._-"
    return "".join(ch for ch in s
                   if ch.isascii() and (ch in allowed or ch.isalnum()))


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

    # --- items (gItems, 44 B GBA wire rows) -- R13-D2 ------------------------
    item_names = parse_enum("ITEM_", pret / "include/constants/items.h")
    if any(i not in item_names for i in range(377)):
        fail("item enum must map indices 0..376 exactly")
    item_key = (lambda i: "emerald:data/item/" +
                semantic_name("ITEM_", item_names[i]))  # noqa: E731
    ref_addr_symbol = {}
    for s in elf.symbols:
        ref_addr_symbol.setdefault(s[1], s[0])
    it_sym, it_data, it_size = sym_bytes("gItems")
    if it_size != 44 * 377:
        fail(f"gItems size {it_size} != 44 x 377")
    item_callbacks = {}   # GBA addr -> reference symbol (the 27-use census)
    for i in range(377):
        off = i * 44
        row = it_data[off:off + 44]
        rom_off = it_sym[1] - GEN3_GBA_ROM_BASE + off
        if rom[rom_off:rom_off + 44] != row:
            fail(f"gItems[{i}]: ROM slice != ELF slice")
        add("item", item_key(i), "gItems", 44, rom_off)
        for foff in (28, 36):  # fieldUseFunc, battleUseFunc GBA addresses
            addr = struct.unpack_from("<I", row, foff)[0]
            if addr == 0:
                continue  # null battle-use (non-battle items)
            if addr not in ref_addr_symbol:
                fail(f"gItems[{i}]: callback addr 0x{addr:x} "
                     "has no reference ELF symbol")
            item_callbacks[addr] = ref_addr_symbol[addr]
    if len(item_callbacks) != 27:
        fail(f"item callback census {len(item_callbacks)} != 27")
    callback_rows = sorted(item_callbacks.items())  # (addr, symbol) by addr
    item_keys = [item_key(i) for i in range(377)]

    # --- trainers (R13-E1) ------------------------------------------------
    # Parse the recomp source for the canonical order. trainer_names[i] is
    # the semantic suffix of the i-th `[TRAINER_<suffix>]` designator in file
    # order (trainer id i == designator index i, no gaps), lowercased with
    # '_' -> '-': TRAINER_GRUNT_AQUA_HIDEOUT_1 -> "grunt-aqua-hideout-1".
    TRAINER_ROW_OFF = 0x310030   # gTrainers ROM-relative (+ 0x08000000 GBA)
    TRAINER_CLASS_OFF = 0x30fcd4 # gTrainerClassNames
    TRAINER_PBLOCK_S = 0x30b62c  # party leaves block start
    TRAINER_PBLOCK_E = 0x30fcd4  # party leaves block end (touches classes)
    TRAINER_ROW_WIRE = 40
    TRAINER_CLASS_WIRE = 13
    TRAINER_ROM_STRIDE = {0: 8, 1: 16, 2: 8, 3: 16}  # by partyFlags & 3

    trainers_h = (root / "src/data/trainers.h").read_text()
    designators = re.findall(r"^\s*\[(TRAINER_[A-Z0-9_]+)\]",
                             trainers_h, re.M)
    if len(designators) != 855:
        fail(f"trainers.h designators {len(designators)} != 855")
    trainer_names = [d[len("TRAINER_"):].lower().replace("_", "-")
                     for d in designators]

    # Class names: parse the `[TRAINER_CLASS_X] = _("display")` rows in file
    # order. The display-string-derived key is unique per class: when two
    # classes share a display string (e.g. the four "{PKMN} TRAINER" rows),
    # the later one disambiguates with its class index appended. The
    # generated kGameplayTrainerClassKeys[66] maps each class index to its
    # exact key, so the seam is never ambiguous.
    class_h = (root / "src/data/text/trainer_class_names.h").read_text()
    class_rows = re.findall(
        r"^\s*\[TRAINER_CLASS_[A-Z0-9_]+\]\s*=\s*_\(([^)]*)\)\s*[,]?\s*$",
        class_h, re.M)
    if len(class_rows) != 66:
        fail(f"trainer_class_names.h rows {len(class_rows)} != 66")
    seen_class = set()
    class_keys = []
    for cidx, display in enumerate(class_rows):
        if len(display) >= 2 and display[0] == '"' and display[-1] == '"':
            display = display[1:-1]  # drop the charmap string quotes
        base = class_resource_suffix(display)
        key = base if base not in seen_class else base + "-" + str(cidx)
        seen_class.add(key)
        class_keys.append("emerald:data/trainer-class/" + key)
    if len(set(class_keys)) != 66:
        fail("trainer-class keys must be distinct (66)")

    tr_sym, tr_data, tr_size = sym_bytes("gTrainers")
    if tr_size != TRAINER_ROW_WIRE * 855:
        fail(f"gTrainers size {tr_size} != 40 x 855")
    cl_sym, cl_data, cl_size = sym_bytes("gTrainerClassNames")
    if cl_size != TRAINER_CLASS_WIRE * 66:
        fail(f"gTrainerClassNames size {cl_size} != 13 x 66")
    if tr_sym[1] - GEN3_GBA_ROM_BASE != TRAINER_ROW_OFF:
        fail(f"gTrainers not at expected 0x{TRAINER_ROW_OFF:x}")
    if cl_sym[1] - GEN3_GBA_ROM_BASE != TRAINER_CLASS_OFF:
        fail(f"gTrainerClassNames not at expected 0x{TRAINER_CLASS_OFF:x}")

    # Per-trainer generated maps (index-aligned with gTrainers[855]).
    trainer_keys = []          # trainer id -> metadata resource key
    party_keys = []            # trainer id -> party resource key ("" if NULL)
    party_meta = []            # (partySize, variant) per trainer
    party_leaves = []          # (trainer_idx, rom_off, size, variant)

    for i in range(855):
        row_rom = TRAINER_ROW_OFF + i * TRAINER_ROW_WIRE
        if rom[row_rom:row_rom + TRAINER_ROW_WIRE] != tr_data[
                i * TRAINER_ROW_WIRE:i * TRAINER_ROW_WIRE + TRAINER_ROW_WIRE]:
            fail(f"gTrainers[{i}]: ROM slice != ELF slice")
        add("trainer", "emerald:data/trainer/" + trainer_names[i],
            "gTrainers", TRAINER_ROW_WIRE, row_rom)
        trainer_keys.append("emerald:data/trainer/" + trainer_names[i])

        flags = tr_data[i * TRAINER_ROW_WIRE]
        psz = tr_data[i * TRAINER_ROW_WIRE + 0x20]
        variant = flags & 3
        pptr = struct.unpack_from("<I", tr_data, i * TRAINER_ROW_WIRE + 0x24)[0]
        if pptr:
            leaf_off = pptr - GEN3_GBA_ROM_BASE
            leaf_size = psz * TRAINER_ROM_STRIDE[variant]
            if leaf_off < TRAINER_PBLOCK_S or leaf_off + leaf_size > TRAINER_PBLOCK_E:
                fail(f"gTrainers[{i}] party leaf outside the party block")
            add("trainer-party",
                "emerald:data/trainer/" + trainer_names[i] + "/party",
                "gTrainersParty", leaf_size, leaf_off)
            party_keys.append(
                "emerald:data/trainer/" + trainer_names[i] + "/party")
            party_leaves.append((i, leaf_off, leaf_size, variant))
        else:
            if psz != 0 or i != 0:
                fail(f"gTrainers[{i}] NULL party but partySize {psz}")
            party_keys.append("")
        party_meta.append((psz, variant))

    # The 854 leaves must tile [TRAINER_PBLOCK_S, TRAINER_PBLOCK_E) exactly
    # in trainer-index order (no overlap, no gap) - the sum is the 18,088 B
    # canonical party block.
    party_leaves.sort(key=lambda t: t[0])
    if len(party_leaves) != 854:
        fail(f"party leaves {len(party_leaves)} != 854")
    cursor = TRAINER_PBLOCK_S
    for (i, leaf_off, leaf_size, variant) in party_leaves:
        if leaf_off != cursor:
            fail(f"party leaves do not tile contiguously at trainer {i}; "
                 f"leaf @0x{leaf_off:x} but block cursor 0x{cursor:x}")
        cursor += leaf_size
    if cursor != TRAINER_PBLOCK_E:
        fail("party leaves do not cover the full party block")

    for c in range(66):
        cl_rom = TRAINER_CLASS_OFF + c * TRAINER_CLASS_WIRE
        if rom[cl_rom:cl_rom + TRAINER_CLASS_WIRE] != cl_data[
                c * TRAINER_CLASS_WIRE:c * TRAINER_CLASS_WIRE + TRAINER_CLASS_WIRE]:
            fail(f"gTrainerClassNames[{c}]: ROM slice != ELF slice")
        add("trainer-class-name", class_keys[c], "gTrainerClassNames",
            TRAINER_CLASS_WIRE, cl_rom)

    # --- wild encounters (R13-E2) ---------------------------------------------
    # ONE `emerald:data/encounter/headers` block resource (the exact 2,500 B
    # ROM gWildMonHeaders block: 124 real + 1 MAP_UNDEFINED sentinel at row
    # 124) + 209 `emerald:data/encounter/<map>/<type>` slot-table resources
    # (type in land/water/rock/fishing; payload = the exact ROM WildPokemon
    # slot slice). encounterRate + the info/slot GBA pointers ride as
    # generated metadata for the publication seam's header->info->slot graph
    # validation. See docs/R13E2_WILD_ENCOUNTER_MIGRATION_PLAN.md.
    ENCOUNTER_HEADERS_ROM = 0x552d48   # gWildMonHeaders ROM-relative
    ENCOUNTER_HEADER_WIRE = 20
    ENCOUNTER_SLOT_WIRE = 4
    ENCOUNTER_TYPE_SLOTS = {"land": 12, "water": 5,
                            "rock-smash": 5, "fishing": 10}
    # (type tag, GBA-pointer field offset within the 20 B header row); order
    # matches the native struct field order (rock-smash before fishing).
    ENCOUNTER_FIELDS = (("land", 4), ("water", 8),
                        ("rock-smash", 12), ("fishing", 16))

    hdr_sym = elf.find("gWildMonHeaders")
    if hdr_sym is None or hdr_sym[2] != ENCOUNTER_HEADER_WIRE * 125:
        fail("gWildMonHeaders must be exactly 125 x 20 B")
    if hdr_sym[1] - GEN3_GBA_ROM_BASE != ENCOUNTER_HEADERS_ROM:
        fail(f"gWildMonHeaders not at expected 0x{ENCOUNTER_HEADERS_ROM:x}")
    hdr_block = rom[ENCOUNTER_HEADERS_ROM:ENCOUNTER_HEADERS_ROM + 2500]
    if elf.slice(hdr_sym, 2500) != hdr_block:
        fail("gWildMonHeaders ROM slice != ELF slice")

    # (mapGroup,mapNum) -> lowercase semantic map name from map_groups.h
    # (the R13-F map identity): MAP_FOO_BAR -> foo-bar.
    map_groups_h = (root / "include/constants/map_groups.h").read_text()
    map_value = {}
    for m in re.finditer(
            r"#define\s+(MAP_[A-Z0-9_]+)\s*\((\d+)\s*\|\s*\((\d+)\s*<<\s*8\)\)",
            map_groups_h):
        val = (int(m.group(3)) << 8) | int(m.group(2))
        if val in map_value and map_value[val] != m.group(1):
            fail(f"map_groups.h collision for map value {val:#x}")
        map_value[val] = m.group(1)[len("MAP_"):].lower().replace("_", "-")

    add("encounter-headers", "emerald:data/encounter/headers",
        "gWildMonHeaders", 2500, ENCOUNTER_HEADERS_ROM)

    # Object-symbol address map (slot-table provenance): STT_OBJECT only, so
    # the compiler's "$d"/"$a" alignment labels never shadow a real table.
    obj_addr = {}
    for s in elf.symbols:
        if (s[3] & 0xf) == 1 and s[4] >= 0:
            obj_addr.setdefault(s[1], s[0])

    head_keys = []   # 125 x 4 slot-resource keys ("" = null header field)
    infos = {}       # slot resource key -> {rate, infoPtr, slotPtr, slotRows}
    ac_variant = 0   # Altering Cave header index (1..9 in header order)
    for hi in range(125):
        coff = ENCOUNTER_HEADERS_ROM + hi * ENCOUNTER_HEADER_WIRE
        mg, mn, pad = struct.unpack_from("<BBH", rom, coff)
        if hi == 124:
            if mg != 0xFF or mn != 0xFF:
                fail("gWildMonHeaders row 124 is not the MAP_UNDEFINED sentinel")
            if any(struct.unpack_from("<I", rom, coff + f)[0] != 0
                   for _, f in ENCOUNTER_FIELDS):
                fail("sentinel header must carry NULL info pointers")
            head_keys.append(["", "", "", ""])
            continue
        alter = (mg == 24 and mn == 106)
        if alter:
            ac_variant += 1
            map_key = "altering-cave-%d" % ac_variant
        else:
            v = (mg << 8) | mn
            if v not in map_value:
                fail(f"gWildMonHeaders map ({mg},{mn}) has no MAP_ constant")
            map_key = map_value[v]
        field_keys = []
        for (tname, foff) in ENCOUNTER_FIELDS:
            p = struct.unpack_from("<I", rom, coff + foff)[0]
            if p == 0:
                field_keys.append("")
                continue
            ioff = p - GEN3_GBA_ROM_BASE
            rate = rom[ioff]
            slotptr = struct.unpack_from("<I", rom, ioff + 4)[0]
            if slotptr == 0:
                fail(f"info @0x{p:x} ({map_key}/{tname}) has a NULL slotPtr")
            slot_rom = slotptr - GEN3_GBA_ROM_BASE
            slot_rows = ENCOUNTER_TYPE_SLOTS[tname]
            slot_bytes = slot_rows * ENCOUNTER_SLOT_WIRE
            slot_slice = rom[slot_rom:slot_rom + slot_bytes]
            rkey = "emerald:data/encounter/%s/%s" % (map_key, tname)

            slot_sym = obj_addr.get(slotptr)
            if slot_sym is None:
                fail(f"{rkey}: no object symbol for slot table @0x{slotptr:x}")
            stbl = elf.find(slot_sym)
            if stbl[1] - GEN3_GBA_ROM_BASE != slot_rom:
                fail(f"{rkey}: slot symbol addr != info slotPtr")
            if elf.slice(stbl, slot_bytes) != slot_slice:
                fail(f"{rkey}: ROM slot slice != ELF slice")

            add("encounter", rkey, slot_sym, slot_bytes, slot_rom)
            field_keys.append(rkey)
            if rkey in infos:
                fail(f"duplicate encounter resource {rkey}")
            infos[rkey] = {"rate": rate, "infoPtr": p, "slotPtr": slotptr,
                           "slotRows": slot_rows}
        head_keys.append(field_keys)

    if ac_variant != 9:
        fail(f"Altering Cave variants {ac_variant} != 9")
    if sum(1 for h in head_keys for f in h if f) != 209:
        fail("map-based encounter slot resources != 209")
    if len(infos) != 209:
        fail(f"distinct infos {len(infos)} != 209")

    # --- Battle Frontier trainer/mon graph + Battle Tents (R13-E3a-1) -------
    # Frontier trainer metadata rows are the exact 52-byte GBA wire slices of
    # gBattleFrontierTrainers; each row's GBA monSet pointer (u32 @ wire offset
    # 48) resolves to the trainer's own 0xFFFF-terminated u16 index-stream leaf
    # (indices 0..881 into the shared 882-row gBattleFrontierMons pool). The 300
    # leaves tile [0x5ced2e, 0x5d5aca) contiguously (2 pad bytes precede
    # gBattleFrontierTrainers). Tents (slateport/verdanturf/fallarbor) share the
    # same trainer schema; each tent trainer's monSet pointer resolves to that
    # tent's leaf, whose indices are within the tent's OWN mons pool.
    FRONTIER_TRAINER_ROM = 0x5d5acc
    FRONTIER_TRAINER_WIRE = 52
    FRONTIER_TRAINER_COUNT = 300
    FRONTIER_MONS_ROM = 0x5d97bc
    FRONTIER_MONS_WIRE = 16
    FRONTIER_MONS_COUNT = 882
    FRONTIER_MONSET_BLOCK_S = 0x5ced2e
    HELD_ITEMS_ROM = 0x5cecb0
    HELD_ITEMS_BYTES = 126
    BANNED_ROM = 0x611c9a
    BANNED_BYTES = 22
    TENTS = [("slateport", 0x5dda14, 70),
             ("verdanturf", 0x5de610, 45),
             ("fallarbor", 0x5df084, 45)]
    TENT_TRAINER_COUNT = 30

    for sym, rom_off, label in [
            ("gBattleFrontierTrainers", FRONTIER_TRAINER_ROM, "gBattleFrontierTrainers"),
            ("gBattleFrontierMons", FRONTIER_MONS_ROM, "gBattleFrontierMons"),
            ("gBattleFrontierHeldItems", HELD_ITEMS_ROM, "gBattleFrontierHeldItems"),
            ("gFrontierBannedSpecies", BANNED_ROM, "gFrontierBannedSpecies")]:
        s = elf.find(sym)
        if s is None or s[1] - GEN3_GBA_ROM_BASE != rom_off:
            fail(f"{sym} not at expected ROM 0x{rom_off:x}")

    def frontier_leaf(addr):
        """(rom_offset, size, words) of the 0xFFFF-terminated u16 stream at a
        GBA address. indices must be in [0, 882)."""
        leaf_off = addr - GEN3_GBA_ROM_BASE
        words = []
        while True:
            w = struct.unpack_from("<H", rom, leaf_off)[0]
            if w == 0xFFFF:
                break
            words.append(w)
            leaf_off += 2
        end = leaf_off + 2  # after the terminator
        if any(w >= FRONTIER_MONS_COUNT for w in words):
            fail(f"frontier mon-set leaf @0x{addr:x} has out-of-range index")
        return leaf_off - 2 * len(words), end, words

    frontier_map = {"trainer_keys": [], "monset_keys": [], "mons_key": "",
                    "held_key": "", "banned_key": "", "tents": {}}

    # Shared 882-row pool, held items, banned species.
    fpool_key = "emerald:data/frontier/mons"
    fm_sz = FRONTIER_MONS_WIRE * FRONTIER_MONS_COUNT
    if elf.find("gBattleFrontierMons")[2] != fm_sz:
        fail(f"gBattleFrontierMons size != {FRONTIER_MONS_WIRE} x {FRONTIER_MONS_COUNT}")
    if rom[FRONTIER_MONS_ROM:FRONTIER_MONS_ROM + fm_sz] != elf.slice(
            elf.find("gBattleFrontierMons"), fm_sz):
        fail("gBattleFrontierMons ROM slice != ELF slice")
    add("frontier-mon", fpool_key, "gBattleFrontierMons", fm_sz, FRONTIER_MONS_ROM)
    add("frontier-held-items", "emerald:data/frontier/held-items",
        "gBattleFrontierHeldItems", HELD_ITEMS_BYTES, HELD_ITEMS_ROM)
    add("frontier-banned-species", "emerald:data/frontier/banned-species",
        "gFrontierBannedSpecies", BANNED_BYTES, BANNED_ROM)

    # 300 frontier trainer rows + 300 mon-set leaves + end-to-end tiling.
    ft_sz = FRONTIER_TRAINER_WIRE * FRONTIER_TRAINER_COUNT
    if elf.find("gBattleFrontierTrainers")[2] != ft_sz:
        fail(f"gBattleFrontierTrainers size != 52 x 300")
    if rom[FRONTIER_TRAINER_ROM:FRONTIER_TRAINER_ROM + ft_sz] != elf.slice(
            elf.find("gBattleFrontierTrainers"), ft_sz):
        fail("gBattleFrontierTrainers ROM slice != ELF slice")
    f_leaf_spans = []
    for i in range(FRONTIER_TRAINER_COUNT):
        off = FRONTIER_TRAINER_ROM + i * FRONTIER_TRAINER_WIRE
        tkey = "emerald:data/frontier/trainer/%d" % i
        mkey = tkey + "/mons"
        if rom[off:off + FRONTIER_TRAINER_WIRE] != elf.slice(elf.find("gBattleFrontierTrainers"), ft_sz)[i*FRONTIER_TRAINER_WIRE:(i+1)*FRONTIER_TRAINER_WIRE]:
            fail(f"gBattleFrontierTrainers[{i}] ROM slice != ELF slice")
        add("frontier-trainer", tkey, "gBattleFrontierTrainers",
            FRONTIER_TRAINER_WIRE, off)
        monset = struct.unpack_from("<I", rom, off + 48)[0]
        leaf_off, leaf_end, words = frontier_leaf(monset)
        leaf_sym = obj_addr.get(monset, "gBattleFrontierTrainerMons")
        add("frontier-mon-set", mkey, leaf_sym, leaf_end - leaf_off, leaf_off)
        f_leaf_spans.append((leaf_off, leaf_end))
        frontier_map["trainer_keys"].append(tkey)
        frontier_map["monset_keys"].append(mkey)
    # The 300 leaves tile the block contiguously in address order.
    f_leaf_spans.sort()
    cursor = FRONTIER_MONSET_BLOCK_S
    for (s_, e_) in f_leaf_spans:
        if s_ != cursor:
            fail(f"frontier mon-set leaf tiling gap at {s_:x} (cursor {cursor:x})")
        cursor = e_
    if cursor != 0x5d5aca:
        fail(f"frontier mon-set block end {cursor:x} != expected 0x5d5aca")
    frontier_map["mons_key"] = fpool_key
    frontier_map["held_key"] = "emerald:data/frontier/held-items"
    frontier_map["banned_key"] = "emerald:data/frontier/banned-species"

    # Three Battle Tent families (same trainer schema, per-tent mons pools).
    for (tent, troff, pool_size) in TENTS:
        tent_base = "emerald:data/frontier/tent/%s" % tent
        tmons_key = tent_base + "/mons"
        pool_sz = pool_size * FRONTIER_MONS_WIRE
        tmons_sym = elf.find("g%sBattleTentMons" % tent.capitalize())
        if tmons_sym is None or tmons_sym[2] != pool_sz:
            fail("no exact-size tent mons symbol for %s" % tent)
        tmons_entry_rom = tmons_sym[1] - GEN3_GBA_ROM_BASE
        if rom[tmons_entry_rom:tmons_entry_rom + pool_sz] != elf.slice(
                tmons_sym, pool_sz):
            fail("tent mons ROM slice != ELF slice")
        add("tent-mon", tmons_key, "g%sBattleTentMons" % tent.capitalize(),
            pool_sz, tmons_entry_rom)
        tk = []; mk = []
        for i in range(TENT_TRAINER_COUNT):
            off = troff + i * FRONTIER_TRAINER_WIRE
            tkey = tent_base + "/trainer/%d" % i
            mkey = tkey + "/mons"
            row_rom = rom[off:off + FRONTIER_TRAINER_WIRE]
            add("tent-trainer", tkey, "g%sBattleTentTrainers" % tent.capitalize(),
                FRONTIER_TRAINER_WIRE, off)
            monset = struct.unpack_from("<I", row_rom, 48)[0]
            leaf_off, leaf_end, words = frontier_leaf(monset)
            if any(w >= pool_size for w in words):
                fail(f"{tent} mon-set leaf@{hex(monset)} index out of pool")
            leaf_sym = obj_addr.get(monset,
                                    "g%sBattleTentTrainerMons" % tent.capitalize())
            add("tent-mon-set", mkey, leaf_sym, leaf_end - leaf_off, leaf_off)
            tk.append(tkey); mk.append(mkey)
        frontier_map["tents"][tent] = {"trainer_keys": tk, "monset_keys": mk,
                                        "mons_key": tmons_key}

    # --- Battle Frontier facility AUX content (R13-E3a-2) --------------------
    # Exact-GBA-slice content resources for the factory/palace/arena/pike/
    # pyramid/brain/apprentice facility families plus the pike/pyramid wild-
    # encounter handoff. Wire shapes (from the reference ELF/ROM) drive the
    # publication transforms in emerald_frontier_compat.c; each transformed
    # family records (count, wire_stride, native_stride) so the seam rebuilds
    # the packed native table (every native row is a prefix of its GBA row:
    # the GBA build pads to 4-byte/88-byte boundaries, native drops the pad).
    def get_slice(symbol, size, expected_rom, label):
        s = elf.find(symbol)
        if s is None or s[2] != size or s[1] - GEN3_GBA_ROM_BASE != expected_rom:
            fail(f"{label}: symbol {symbol} size/addr mismatch"
                 f" (got {s[2] if s else 0} B @0x{s[1] - GEN3_GBA_ROM_BASE if s else 0:x},"
                 f" want {size} B @0x{expected_rom:x})")
        sl = elf.slice(s, size)
        ro = rom[expected_rom:expected_rom + size]
        if sl != ro:
            fail(f"{label}: {symbol} ROM slice != ELF slice")
        return ro

    aux = {
        "factory_moves": [],     # 7 keys (schema 26)
        "palace_early": "",      # schema 27
        "palace_late": "",
        "arena_short": "",       # schema 28
        "arena_long": "",
        "pike_npc": "",          # schema 29 (25 x 8 -> 25 x 6)
        "pike_speeches": [],     # schema 30 keys [speeches, hints, heals]
        "pike_wild_mons": [],    # 8 keys (lvl50 1..4, lvlopen 1..4)
        "pyramid_floor": [],     # schema 31 keys [templates, options]
        "pyramid_item": "",      # schema 32 (shared lvl50/lvlopen payload)
        "pyramid_slots": "",     # schema 33
        "brain_ids": "",         # schema 34
        "brain_mons": "",
        "brain_streak": "",
        "apprentice_keys": [],   # 16 keys (schema 35)
        "wild_headers": {},      # schema 36: {"pike":key,"pyramid":key}
        "wild_sets": {},         # schema 37: key -> {rate, infoPtr, slotPtr, slotRows}
        # native transform descriptors for padded->packed families
        "pike_npc_slots": 25,
        "pyramid_floor_slots": 16,
        "apprentice_rows": 16,
    }

    # Factory: 7 strategy move lists (u16 move-ID arrays; LEAF).
    FACTORY_MOVES = [
        ("total-preparation", "sMoves_TotalPreparation", 0x611fc8, 0x38),
        ("impossible-to-predict", "sMoves_ImpossibleToPredict", 0x612000, 0x1e),
        ("weakening-the-foe", "sMoves_WeakeningTheFoe", 0x61201e, 0x28),
        ("high-risk-high-return", "sMoves_HighRiskHighReturn", 0x612046, 0x36),
        ("endurance", "sMoves_Endurance", 0x61207c, 0x38),
        ("slow-and-steady", "sMoves_SlowAndSteady", 0x6120b4, 0x42),
        ("depends-on-the-battles-flow", "sMoves_DependsOnTheBattlesFlow",
         0x6120f6, 0x0c),
    ]
    for seg, sym, ro, sz in FACTORY_MOVES:
        get_slice(sym, sz, ro, "factory-moves")
        key = "emerald:data/frontier/factory/moves/%s" % seg
        add("frontier-factory", key, sym, sz, ro)
        aux["factory_moves"].append(key)

    # Palace / Arena prize item arrays (LEAF).
    aux["palace_early"] = "emerald:data/frontier/palace/prizes/early"
    get_slice("sBattlePalaceEarlyPrizes", 0x0c, 0x60de78, "palace-early")
    add("frontier-palace", aux["palace_early"], "sBattlePalaceEarlyPrizes",
        0x0c, 0x60de78)
    aux["palace_late"] = "emerald:data/frontier/palace/prizes/late"
    get_slice("sBattlePalaceLatePrizes", 0x12, 0x60de84, "palace-late")
    add("frontier-palace", aux["palace_late"], "sBattlePalaceLatePrizes",
        0x12, 0x60de84)
    aux["arena_short"] = "emerald:data/frontier/arena/prizes/short"
    get_slice("sShortStreakPrizeItems", 0x0c, 0x611fa0, "arena-short")
    add("frontier-arena", aux["arena_short"], "sShortStreakPrizeItems",
        0x0c, 0x611fa0)
    aux["arena_long"] = "emerald:data/frontier/arena/prizes/long"
    get_slice("sLongStreakPrizeItems", 0x12, 0x611fac, "arena-long")
    add("frontier-arena", aux["arena_long"], "sLongStreakPrizeItems",
        0x12, 0x611fac)

    # Pike NPC table (25 rows; GBA 8 B/row -> native 6 B/row).
    aux["pike_npc"] = "emerald:data/frontier/pike/npc"
    npc = get_slice("sNPCTable", 0xc8, 0x61231c, "pike-npc")
    if len(npc) != 25 * 8:
        fail("sNPCTable must be exactly 25 x 8 B")
    add("frontier-pike-npc", aux["pike_npc"], "sNPCTable", 0xc8, 0x61231c)

    # Pike speeches + room hints + pre-queen heals (LEAF u8/u16 content).
    aux["pike_speeches"] = ["emerald:data/frontier/pike/speeches",
                            "emerald:data/frontier/pike/room-hints",
                            "emerald:data/frontier/pike/heals"]
    get_slice("sNPCSpeeches", 0x1f8, 0x6123e4, "pike-speeches")
    add("frontier-pike-speech", aux["pike_speeches"][0], "sNPCSpeeches",
        0x1f8, 0x6123e4)
    get_slice("sRoomTypeHints", 0x09, 0x61266c, "pike-room-hints")
    add("frontier-pike-speech", aux["pike_speeches"][1], "sRoomTypeHints",
        0x09, 0x61266c)
    get_slice("sNumMonsToHealBeforePikeQueen", 0x12, 0x612675, "pike-heals")
    add("frontier-pike-speech", aux["pike_speeches"][2],
        "sNumMonsToHealBeforePikeQueen", 0x12, 0x612675)

    # PikeWildMon tables (8 x 36 B; native == GBA stride, LEAF content).
    for (which, base_rom) in (("lvl50", 0x6121d4), ("lvlopen", 0x612274)):
        for n in range(4):
            ro = base_rom + n * 0x24
            sym = "sLvl%s_Mons%d" % ("50" if which == "lvl50" else "Open", n + 1)
            key = "emerald:data/frontier/pike/wild-mons/%s/%d" % (which, n + 1)
            get_slice(sym, 0x24, ro, "pike-wild-mon")
            add("frontier-pike-speech", key, sym, 0x24, ro)
            aux["pike_wild_mons"].append(key)

    # Pyramid floor templates (16 x 16 -> 13) + floor-template options (68 B).
    aux["pyramid_floor"] = ["emerald:data/frontier/pyramid/floor-templates",
                            "emerald:data/frontier/pyramid/floor-options"]
    pf = get_slice("sPyramidFloorTemplates", 0x100, 0x613650, "pyramid-floor")
    if len(pf) != 16 * 16:
        fail("sPyramidFloorTemplates must be exactly 16 x 16 B")
    add("frontier-pyramid-floor", aux["pyramid_floor"][0],
        "sPyramidFloorTemplates", 0x100, 0x613650)
    get_slice("sPyramidFloorTemplateOptions", 0x44, 0x613750,
              "pyramid-floor-options")
    add("frontier-pyramid-floor", aux["pyramid_floor"][1],
        "sPyramidFloorTemplateOptions", 0x44, 0x613750)

    # Pyramid pickup items: lvl50 + lvlopen are byte-identical twins; store
    # ONE payload bound to both native fill targets (dedupe).
    l50 = get_slice("sPickupItemsLvl50", 0x190, 0x61379c, "pyramid-items-lvl50")
    lopen = get_slice("sPickupItemsLvlOpen", 0x190, 0x61392c, "pyramid-items-lvlopen")
    if l50 != lopen:
        fail("sPickupItemsLvl50 != sPickupItemsLvlOpen (expected identical twins)")
    aux["pyramid_item"] = "emerald:data/frontier/pyramid/items/lvl50"
    add("frontier-pyramid-item", aux["pyramid_item"], "sPickupItemsLvl50",
        0x190, 0x61379c)

    # Pyramid pickup item slots (126 B).
    aux["pyramid_slots"] = "emerald:data/frontier/pyramid/item-slots"
    get_slice("sPickupItemSlots", 0x7e, 0x613abc, "pyramid-item-slots")
    add("frontier-pyramid-slots", aux["pyramid_slots"], "sPickupItemSlots",
        0x7e, 0x613abc)

    # Brain: trainer ids (14 B), braves mons (20 B/row x 42 = 840 B) and the
    # shared streak-appearances table (28 B).
    aux["brain_ids"] = "emerald:data/frontier/brain/ids"
    get_slice("sFrontierBrainTrainerIds", 0x0e, 0x611d30, "brain-ids")
    add("frontier-brain", aux["brain_ids"], "sFrontierBrainTrainerIds",
        0x0e, 0x611d30)
    aux["brain_mons"] = "emerald:data/frontier/brain/mons"
    bm = get_slice("sFrontierBrainsMons", 0x348, 0x61156c, "brain-mons")
    if len(bm) != 42 * 20:
        fail("sFrontierBrainsMons must be 42 mons x 20 B")
    add("frontier-brain", aux["brain_mons"], "sFrontierBrainsMons",
        0x348, 0x61156c)
    aux["brain_streak"] = "emerald:data/frontier/brain/streak-appearances"
    get_slice("sFrontierBrainStreakAppearances", 0x1c, 0x611550, "brain-streak")
    add("frontier-brain", aux["brain_streak"],
        "sFrontierBrainStreakAppearances", 0x1c, 0x611550)

    # Apprentice: 16 entries x 88 B (-> native 86 B/row).
    ap = get_slice("gApprentices", 0x580, 0x610970, "apprentice")
    if len(ap) != 16 * 88:
        fail("gApprentices must be 16 x 88 B")
    for i in range(16):
        ro = 0x610970 + i * 88
        key = "emerald:data/frontier/apprentice/%d" % i
        add("frontier-apprentice", key, "gApprentices", 88, ro)
        aux["apprentice_keys"].append(key)

    # ---- Pike/Pyramid wild-encounter handoff (reuse E2 wire format). ----
    # gBattlePikeWildMonHeaders (5 x 20 = 100 B, 4 real + sentinel) and
    # gBattlePyramidWildMonHeaders (8 x 20 = 160 B, 7 real + sentinel), each
    # row's landMonsInfo pointer -> WildPokemonInfo (rate u8 @0, slotPtr u32
    # @4) -> 12-row WildPokemon slot table (4 B/row). Uses the exact E2
    # encounter slot schema/wire format, emitted under new schema codes 36/37
    # so the E2 schema-count gate (19/20 == 1/209) is unaffected.
    FWD = (("pike", "gBattlePikeWildMonHeaders", 0x553a14, 5,
            "pike-%d", "emerald:data/frontier/pike/wild/headers"),
           ("pyramid", "gBattlePyramidWildMonHeaders", 0x553894, 8,
            "pyramid-round-%d", "emerald:data/frontier/pyramid/wild/headers"))
    fw_infos = {}
    for (tag, sym, hb_rom, hb_rows, setkey_fmt, hdr_key) in FWD:
        hb = get_slice(sym, hb_rows * 20, hb_rom, "facility-wild-headers")
        add("frontier-wild-headers", hdr_key, sym, hb_rows * 20, hb_rom)
        aux["wild_headers"][tag] = hdr_key
        real = 0
        for hi in range(hb_rows):
            coff = hb_rom + hi * 20
            mg, mn, pad = struct.unpack_from("<BBH", rom, coff)
            if mg == 0xFF and mn == 0xFF:
                if any(struct.unpack_from("<I", rom, coff + f)[0] != 0
                       for f in (4, 8, 12, 16)):
                    fail(f"{sym} row {hi} sentinel must carry NULL info ptrs")
                continue
            real += 1
            p = struct.unpack_from("<I", rom, coff + 4)[0]
            if p == 0:
                fail(f"{sym} row {hi} has a NULL landMonsInfo")
            ioff = p - GEN3_GBA_ROM_BASE
            rate = rom[ioff]
            slotptr = struct.unpack_from("<I", rom, ioff + 4)[0]
            if slotptr == 0:
                fail(f"{sym} row {hi}: NULL slotPtr")
            slot_rom = slotptr - GEN3_GBA_ROM_BASE
            slot_slice = rom[slot_rom:slot_rom + 48]
            rkey = "emerald:data/frontier/%s/wild/%s" % (
                tag, setkey_fmt % (hi + 1))
            slot_sym = obj_addr.get(slotptr)
            if slot_sym is None:
                fail(f"{rkey}: no object symbol for slot table @0x{slotptr:x}")
            stbl = elf.find(slot_sym)
            if stbl[1] - GEN3_GBA_ROM_BASE != slot_rom:
                fail(f"{rkey}: slot symbol addr != info slotPtr")
            if elf.slice(stbl, 48) != slot_slice:
                fail(f"{rkey}: ROM slot slice != ELF slice")
            add("frontier-wild", rkey, slot_sym, 48, slot_rom)
            aux["wild_sets"][rkey] = {"rate": rate, "infoPtr": p,
                                      "slotPtr": slotptr, "slotRows": 12}
            fw_infos.setdefault(tag, []).append((rkey, p, rate))
        if real != hb_rows - 1:
            fail(f"{sym} must have {hb_rows - 1} real headers + sentinel")
    # Rate census: pike all 10; pyramid rounds 1..6 (hi 0..5) rate 4, round
    # 7 (hi 6) rate 8.
    pike_rates = [aux["wild_sets"][k]["rate"] for k in aux["wild_sets"]
                  if k.startswith("emerald:data/frontier/pike/wild/")]
    if any(r != 10 for r in pike_rates) or len(pike_rates) != 4:
        fail("pike wild sets must be exactly 4 sets at rate 10")
    pyr = sorted((int(k.rsplit("-", 1)[1]) - 1,
                  aux["wild_sets"][k]["rate"]) for k in aux["wild_sets"]
                 if k.startswith("emerald:data/frontier/pyramid/wild/"))
    if len(pyr) != 7 or [r for _, r in pyr] != [4, 4, 4, 4, 4, 4, 8]:
        fail("pyramid wild sets must be 7 sets at rates [4]*6,8")

    # --- Pokédex rows + ordering/routing tables (R13-E3b) ------------------
    # gPokedexEntries: 387 rows x 32 B GBA wire @0x56b5b0. Row layout (native
    # struct in include/pokedex.h): categoryName[12] INLINE Gen-3 charmap
    # string @0 (NOT a pointer), height u16 @12, weight u16 @14, description
    # u32 GBA text ptr @16 (the ONLY text pointer), unused u16 @20,
    # pokemonScale u16 @22, pokemonOffset u16 @24, trainerScale u16 @26,
    # trainerOffset u16 @28 (+2 pad -> 32). The description GBA address
    # resolves to the reference `g<Species>PokedexText` symbol -> its R13-C
    # text label (emerald:text/pokedex/g<species>pokedextext). Category stays
    # inline (copied byte-identically into the native 40 B row). The ordering/
    # routing tables are u16 arrays, byte-identical GBA/native.
    POKEDEX_ROWS_ROM = 0x56b5b0
    POKEDEX_ROW_WIRE = 32
    POKEDEX_ROWS = 387
    POKEDEX_ORDERS = [  # (reference symbol, ROM offset, u16 count, suffix)
        ("gPokedexOrder_Alphabetical", 0x55c6a4, 411, "alphabetical"),
        ("gPokedexOrder_Weight", 0x55c9da, 386, "weight"),
        ("gPokedexOrder_Height", 0x55ccde, 386, "height"),
    ]

    pe_sym, pe_data, pe_size = sym_bytes("gPokedexEntries")
    if pe_size != POKEDEX_ROW_WIRE * POKEDEX_ROWS:
        fail(f"gPokedexEntries {pe_size} != {POKEDEX_ROW_WIRE} x {POKEDEX_ROWS}")
    if pe_sym[1] - GEN3_GBA_ROM_BASE != POKEDEX_ROWS_ROM:
        fail(f"gPokedexEntries not at expected 0x{POKEDEX_ROWS_ROM:x}")

    # R13-C pokedex text base label -> rom_offset (from the emitted text
    # manifest). The seam re-points description into these live identities.
    text_manifest = (root / "resources/extraction/emerald/bpee01/text"
                     / "manifest.production.toml")
    if not text_manifest.exists():
        fail("text manifest missing: run gen_text_family.py before the "
             "gameplay generator")
    txt = text_manifest.read_text()
    text_base_off = {}
    for m in re.finditer(
            r'id = "([^"]+)"\nkey = "[0-9a-f]+"\ntype = "text"\nschema = '
            r'\d+\nsymbol = "[^"]*"\nrom_offset = (\d+)', txt):
        text_base_off[int(m.group(2))] = m.group(1)
    pokedex_text_ids = {v for v in text_base_off.values()
                        if v.startswith("emerald:text/pokedex/")}
    if len(pokedex_text_ids) != POKEDEX_ROWS:
        fail(f"R13-C pokedex text resource set {len(pokedex_text_ids)} != 387")

    pokedex_row_keys = []
    pokedex_desc_labels = []
    for i in range(POKEDEX_ROWS):
        rom_off = POKEDEX_ROWS_ROM + i * POKEDEX_ROW_WIRE
        row = rom[rom_off:rom_off + POKEDEX_ROW_WIRE]
        if row != pe_data[i * POKEDEX_ROW_WIRE:(i + 1) * POKEDEX_ROW_WIRE]:
            fail(f"gPokedexEntries[{i}]: ROM slice != ELF slice")
        desc = struct.unpack_from("<I", row, 16)[0]
        sym = ref_addr_symbol.get(desc)
        if sym is None or not sym.endswith("PokedexText") \
                or sym[:1] not in ("g", "s"):
            fail(f"gPokedexEntries[{i}]: description addr 0x{desc:x} has no "
                 "reference PokedexText symbol")
        species_sem = sym[1:-len("PokedexText")].lower().replace("_", "-")
        rkey = "emerald:data/pokedex/" + species_sem
        label = "emerald:text/pokedex/g" + species_sem + "pokedextext"
        if label not in pokedex_text_ids:
            fail(f"gPokedexEntries[{i}] ({rkey}): description label {label} "
                 "has no R13-C pokedex text resource")
        add("pokedex-row", rkey, "gPokedexEntries", POKEDEX_ROW_WIRE, rom_off)
        pokedex_row_keys.append(rkey)
        pokedex_desc_labels.append(label)
    if (len(pokedex_row_keys) != POKEDEX_ROWS
            or len(set(pokedex_row_keys)) != POKEDEX_ROWS):
        fail("pokedex row resource keys must be 387 distinct")

    for (osym, orom, orows, suffix) in POKEDEX_ORDERS:
        s = elf.find(osym)
        if s is None or s[2] != orows * 2:
            fail(f"{osym} must be {orows} x 2 B")
        if s[1] - GEN3_GBA_ROM_BASE != orom:
            fail(f"{osym} not at expected 0x{orom:x}")
        if rom[orom:orom + orows * 2] != elf.slice(s, orows * 2):
            fail(f"{osym}: ROM slice != ELF slice")
        add("pokedex-order", "emerald:data/pokedex/order/" + suffix,
            osym, orows * 2, orom)
    s2n_sym, s2n_data, s2n_size = sym_bytes("sSpeciesToNationalPokedexNum")
    if (s2n_size != 411 * 2
            or s2n_sym[1] - GEN3_GBA_ROM_BASE != 0x31dc82
            or rom[0x31dc82:0x31dc82 + 822] != s2n_data):
        fail("sSpeciesToNationalPokedexNum must be 411 x 2 B @0x31dc82")
    add("pokedex-species-to-national",
        "emerald:data/pokedex/species-to-national",
        "sSpeciesToNationalPokedexNum", 822, 0x31dc82)

    pokedex_map = {
        "row_keys": pokedex_row_keys,
        "desc_labels": pokedex_desc_labels,
    }

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
    return rows, fam, levelup_idx_map, species_keys, move_keys, item_keys, \
        callback_rows, trainer_keys, party_keys, party_meta, class_keys, \
        head_keys, infos, frontier_map, aux, pokedex_map


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
    if key.startswith("emerald:data/item/"):
        return "item"
    if key.startswith("emerald:data/trainer/"):
        return "trainer-party" if key.endswith("/party") else "trainer"
    if key.startswith("emerald:data/trainer-class/"):
        return "trainer-class-name"
    if key == "emerald:data/encounter/headers":
        return "encounter-headers"
    if key.startswith("emerald:data/encounter/"):
        return "encounter"
    if key.startswith("emerald:data/frontier/trainer/"):
        return "frontier-mon-set" if key.endswith("/mons") else "frontier-trainer"
    if key == "emerald:data/frontier/mons":
        return "frontier-mon"
    if key == "emerald:data/frontier/held-items":
        return "frontier-held-items"
    if key == "emerald:data/frontier/banned-species":
        return "frontier-banned-species"
    if key.startswith("emerald:data/frontier/factory/"):
        return "frontier-factory"
    if key.startswith("emerald:data/frontier/palace/"):
        return "frontier-palace"
    if key.startswith("emerald:data/frontier/arena/"):
        return "frontier-arena"
    if key.startswith("emerald:data/frontier/") and "/wild/" in key:
        return "frontier-wild-headers" if key.endswith("/wild/headers") else "frontier-wild"
    if key == "emerald:data/frontier/pike/npc":
        return "frontier-pike-npc"
    if key.startswith("emerald:data/frontier/pike/"):
        # speeches / room-hints / heals / pike wild-mons tables (schema 30)
        return "frontier-pike-speech"
    if key.startswith("emerald:data/frontier/pyramid/items/"):
        return "frontier-pyramid-item"
    if key == "emerald:data/frontier/pyramid/item-slots":
        return "frontier-pyramid-slots"
    if key.startswith("emerald:data/frontier/pyramid/"):
        # floor-templates + floor-options (schema 31)
        return "frontier-pyramid-floor"
    if key.startswith("emerald:data/frontier/brain/"):
        return "frontier-brain"
    if key.startswith("emerald:data/frontier/apprentice/"):
        return "frontier-apprentice"
    if key.startswith("emerald:data/frontier/tent/"):
        parts = key[len("emerald:data/frontier/tent/"):].split("/")
        # tent/<tent>/mons (len 2) -> pool; tent/<tent>/trainer/<n>/mons
        # (last /mons) -> mon-set; tent/<tent>/trainer/<n> -> trainer.
        if len(parts) == 2:
            return "tent-mon"
        if parts[-1] == "mons":
            return "tent-mon-set"
        return "tent-trainer"
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
    if key.startswith("emerald:data/pokedex/order/"):
        return "pokedex-order"
    if key == "emerald:data/pokedex/species-to-national":
        return "pokedex-species-to-national"
    if key.startswith("emerald:data/pokedex/"):
        return "pokedex-row"
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
    "move-contest": 10, "item": 11, "growth-rate": 12, "tutor-moves": 13,
    "contest-effects": 14, "contest-combo-starters": 15,
    "trainer": 16, "trainer-party": 17, "trainer-class-name": 18,
    "encounter-headers": 19, "encounter": 20,
    "frontier-trainer": 21, "frontier-mon-set": 22, "frontier-mon": 23,
    "frontier-held-items": 24, "frontier-banned-species": 25,
    "tent-trainer": 21, "tent-mon-set": 22, "tent-mon": 23,
    "frontier-factory": 26, "frontier-palace": 27, "frontier-arena": 28,
    "frontier-pike-npc": 29, "frontier-pike-speech": 30,
    "frontier-pyramid-floor": 31, "frontier-pyramid-item": 32,
    "frontier-pyramid-slots": 33, "frontier-brain": 34,
    "frontier-apprentice": 35,
    "frontier-wild-headers": 36, "frontier-wild": 37,
    "pokedex-row": 38, "pokedex-order": 39,
    "pokedex-species-to-national": 40,
    "font": 1,
}


def resource_type(family):
    return "font" if family == "font" else "structured-data"


def key_sanitize(key):
    """Artifact filename: canonical key with '/' and ':' replaced."""
    return key.replace(":", "%3A").replace("/", "_")


def emit_gameplay(rows, fam, levelup_idx_map, species_keys, move_keys,
                  item_keys, callback_rows, trainer_keys, party_keys,
                  party_meta, class_keys, enc_head_keys, enc_infos,
                  frontier_map, aux, pokedex_map, outdir, root, args):
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
        f"#define GAMEPLAY_NATIVE_TRAINER_COUNT 855u",
        f"#define GAMEPLAY_NATIVE_CLASS_COUNT 66u",
        f"#define GAMEPLAY_NATIVE_TRAINER_PARTY_COUNT 854u",
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
        "/* item index -> item resource key (emerald:data/item/<name>). */",
        "#ifndef GAMEPLAY_NATIVE_ITEM_COUNT",
        "#define GAMEPLAY_NATIVE_ITEM_COUNT 377u",
        "#endif",
        "extern const char *const "
        "kGameplayItemKeys[GAMEPLAY_NATIVE_ITEM_COUNT];",
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
        "const char *const kGameplayItemKeys[GAMEPLAY_NATIVE_ITEM_COUNT] =",
        "{",
    ]
    for k in item_keys:
        lvl_c_lines.append(f'    "{k}",')
    lvl_c_lines += [
        "};",
        "",
    ]
    write_if(root / "src/emerald/resources/gameplay_levelup.generated.c",
             lvl_c_lines, args)

    # ---- item callback registry (R13-D2) ---------------------------------
    # Maps each distinct GBA use-function address to a stable semantic action
    # token. The seam reads an item row's fieldUseFunc/battleUseFunc GBA
    # addresses, looks them up here, and maps the action token to the native
    # ItemUseFunc (the native mapping is a compiled engine table, never the
    # pack). Actions are keyed by the reference pret symbol name.
    cb_h = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-D2 item-use callback registry. Every distinct GBA use-function",
        " * address across the 377 gItems rows, mapped to a stable semantic",
        " * action token. The gameplay seam resolves an item row's two GBA",
        " * callback addresses (fieldUseFunc/battleUseFunc) through this table",
        " * and maps the action to a native ItemUseFunc via the compiled seam",
        " * table (gameplay_item_callbacks_native.c). The pack resource NEVER",
        " * stores a native function pointer: it holds the exact 44-byte ROM",
        " * row (GBA addresses included) and the seam does the validation +",
        " * resolution.",
        " */",
        "#ifndef EMERALD_RESOURCES_GAMEPLAY_CALLBACKS_GENERATED_H",
        "#define EMERALD_RESOURCES_GAMEPLAY_CALLBACKS_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define GAMEPLAY_ITEM_CALLBACK_COUNT {len(callback_rows)}u",
        "",
        "enum GameplayItemUseAction",
        "{",
    ]
    tokens = []
    for addr, symbol in callback_rows:
        tok = callback_token(symbol)
        tokens.append((addr, symbol, tok))
    for idx, (addr, symbol, tok) in enumerate(tokens):
        cb_h.append(f"    {tok} = {idx},")
    cb_h += [
        "    GAMEPLAY_ITEM_USE_ACTION_COUNT,",
        "};",
        "",
        "struct GameplayItemCallback",
        "{",
        "    uint32_t gbaAddr;       /* reference GBA function address */",
        "    uint32_t action;        /* enum GameplayItemUseAction */",
        "    const char *symbol;     /* reference pret symbol name */",
        "};",
        "",
        "extern const struct GameplayItemCallback "
        "kGameplayItemCallbacks[GAMEPLAY_ITEM_CALLBACK_COUNT];",
        "",
        "#endif /* EMERALD_RESOURCES_GAMEPLAY_CALLBACKS_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/gameplay_callbacks.generated.h",
             cb_h, args)

    cb_c = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/gameplay_callbacks.generated.h"',
        "",
        "const struct GameplayItemCallback "
        "kGameplayItemCallbacks[GAMEPLAY_ITEM_CALLBACK_COUNT] =",
        "{",
    ]
    for addr, symbol, tok in tokens:
        cb_c.append(f'    {{0x{addr:08x}u, {tok}, "{symbol}"}},')
    cb_c += [
        "};",
        "",
    ]
    write_if(root / "src/emerald/resources/gameplay_callbacks.generated.c",
             cb_c, args)

    # callback metadata TO ML
    cbm = [
        "# Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py",
        "# Do not edit by hand; re-run the generator (regeneration must be a",
        "# no-op diff).",
        "",
        "# R13-D2 item-use callback census (27 distinct GBA addresses).",
        'family = "gameplay"',
        "callback_version = 1",
        "",
    ]
    for addr, symbol, tok in tokens:
        cbm.append("[[callbacks]]")
        cbm.append(f'gba_address = {addr}')
        cbm.append(f'symbol = "{symbol}"')
        cbm.append(f'action = "{action_id(symbol)}"')
        cbm.append("")
    write_if(famdir / "item_callbacks.generated.toml", cbm, args)

    # ---- trainer seam generated maps (R13-E1) ----------------------------
    # Index-aligned maps for the trainer families so the seam can rebuild the
    # native gTrainers[855] / gTrainerClassNames[66][13] tables and the party
    # pointer graph without parsing pack names. The variant enum is aligned
    # with the two partyFlags bits (bit0 = custom moveset, bit1 = held item)
    # so `variant == (partyFlags & 3)`; the seam REFUSEs any disagreement
    # between a row's partyFlags and the generated metadata.
    tr_hdr = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-E1 trainer publication maps. Index-aligned with the native",
        " * gTrainers table (855 trainers). kGameplayTrainerKeys[i] is the",
        " * metadata resource key (emerald:data/trainer/<name>);",
        " * kGameplayTrainerPartyKeys[i] is that trainer's party leaf key",
        " * (emerald:data/trainer/<name>/party) or \"\" for the NULL-party",
        " * TRAINER_NONE row; kGameplayTrainerPartyMeta[i] carries the",
        " * canonical (partySize, variant) such that the variant equals the",
        " * row's (partyFlags & 3). kGameplayTrainerClassKeys[c] maps each",
        " * class index (0..65) to its class-name resource key.",
        " */",
        "#ifndef EMERALD_RESOURCES_TRAINER_NATIVE_GENERATED_H",
        "#define EMERALD_RESOURCES_TRAINER_NATIVE_GENERATED_H",
        "",
        "#include <stdint.h>",
        '#include "emerald/resources/gameplay_native.generated.h"',
        "",
        "enum GameplayTrainerPartyVariant",
        "{",
        "    GAMEPLAY_TRAINER_PARTY_NO_ITEM_DEFAULT_MOVES = 0,",
        "    GAMEPLAY_TRAINER_PARTY_NO_ITEM_CUSTOM_MOVES = 1,",
        "    GAMEPLAY_TRAINER_PARTY_ITEM_DEFAULT_MOVES = 2,",
        "    GAMEPLAY_TRAINER_PARTY_ITEM_CUSTOM_MOVES = 3,",
        "};",
        "",
        "struct GameplayTrainerPartyMeta",
        "{",
        "    uint8_t partySize;  /* canonical party size (wire partySize) */",
        "    uint8_t variant;    /* enum GameplayTrainerPartyVariant */",
        "};",
        "",
        "/* trainer id -> metadata resource key */",
        "extern const char *const "
        "kGameplayTrainerKeys[GAMEPLAY_NATIVE_TRAINER_COUNT];",
        "/* trainer id -> party leaf resource key (\"\" for NULL party) */",
        "extern const char *const "
        "kGameplayTrainerPartyKeys[GAMEPLAY_NATIVE_TRAINER_COUNT];",
        "/* trainer id -> canonical (partySize, variant) */",
        "extern const struct GameplayTrainerPartyMeta "
        "kGameplayTrainerPartyMeta[GAMEPLAY_NATIVE_TRAINER_COUNT];",
        "/* class index -> class-name resource key */",
        "extern const char *const "
        "kGameplayTrainerClassKeys[GAMEPLAY_NATIVE_CLASS_COUNT];",
        "",
        "#endif /* EMERALD_RESOURCES_TRAINER_NATIVE_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/trainer_native.generated.h",
             tr_hdr, args)

    tr_c = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/trainer_native.generated.h"',
        "",
        "const char *const "
        "kGameplayTrainerKeys[GAMEPLAY_NATIVE_TRAINER_COUNT] =",
        "{",
    ]
    for k in trainer_keys:
        tr_c.append(f'    "{k}",')
    tr_c += [
        "};",
        "",
        "const char *const "
        "kGameplayTrainerPartyKeys[GAMEPLAY_NATIVE_TRAINER_COUNT] =",
        "{",
    ]
    for k in party_keys:
        tr_c.append(f'    "{k}",')
    tr_c += [
        "};",
        "",
        "const struct GameplayTrainerPartyMeta "
        "kGameplayTrainerPartyMeta[GAMEPLAY_NATIVE_TRAINER_COUNT] =",
        "{",
    ]
    for (psz, variant) in party_meta:
        tr_c.append(f"    {{{psz}u, {variant}u}},")
    tr_c += [
        "};",
        "",
        "const char *const "
        "kGameplayTrainerClassKeys[GAMEPLAY_NATIVE_CLASS_COUNT] =",
        "{",
    ]
    for k in class_keys:
        tr_c.append(f'    "{k}",')
    tr_c += [
        "};",
        "",
    ]
    write_if(root / "src/emerald/resources/trainer_native.generated.c",
             tr_c, args)

    # ---- encounter seam generated maps (R13-E2) ----------------------------
    # The encounter publication seam (EmeraldEncounterCompat) rebuilds the
    # native gWildMonHeaders[125] / WildPokemonInfo[209] / slot arena from the
    # 210 encounter resources. kEncounterHeaderKeys[h][t] (header-index x
    # land/water/rock-smash/fishing) carries the slot resource key for each
    # non-null header field ("" = null family); kEncounterInfos (sorted by
    # canonical key) carries the per-(map,type) encounterRate and the two GBA
    # pointers (info ptr, slot ptr) so the seam can validate the full
    # header->info->slot pointer graph against the pack (every resource byte-
    # identical, every pointer edge exact) before publishing.
    ECN = "EMERALD_ENCOUNTER_"
    enc_hdr = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-E2 encounter publication maps. kEncounterHeaderKeys[h][t] is",
        " * the slot resource key (emerald:data/encounter/<map>/<type>) for",
        " * header row h / field t (0=land,1=water,2=rock-smash,3=fishing),",
        " * or \"\" for a NULL header field. Header row 124 is the MAP_UNDEFINED",
        " * sentinel. kEncounterInfos (sorted by canonical key) carries each",
        " * published slot table's encounterRate and the two GBA pointers the",
        " * seam validates against the pack (info ptr at instantiation, slot",
        " * ptr over the slot-table ROM address). The seam publishes the",
        " * native gWildMonHeaders array with info pointers rebuilt to its own",
        " * 209 native WildPokemonInfo objects, slotPtr rebuilt into the slot",
        " * arena.",
        " */",
        "#ifndef EMERALD_RESOURCES_ENCOUNTER_NATIVE_GENERATED_H",
        "#define EMERALD_RESOURCES_ENCOUNTER_NATIVE_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define {ECN}SCHEMA_HEADERS 19u",
        f"#define {ECN}SCHEMA_SLOT    20u",
        f"#define {ECN}HEADER_COUNT   125u",
        f"#define {ECN}INFO_COUNT     209u",
        f"#define {ECN}HEADERS_BYTES  2500u",
        "",
        "/* header row x field (0 land, 1 water, 2 rock-smash, 3 fishing). */",
        "extern const char *const "
        "kEncounterHeaderKeys[EMERALD_ENCOUNTER_HEADER_COUNT][4];",
        "",
        "struct EmeraldEncounterInfoRecord",
        "{",
        "    const char *name;    /* canonical slot resource key */",
        "    uint8_t rate;        /* wire encounterRate (info @0) */",
        "    uint32_t gbaInfoPtr; /* info struct GBA address (header field) */",
        "    uint32_t gbaSlotPtr; /* slot table GBA address (info @4) */",
        "    uint32_t slotRows;   /* slot-table row count (payload/4) */",
        "};",
        "",
        "extern const struct EmeraldEncounterInfoRecord "
        "kEncounterInfos[EMERALD_ENCOUNTER_INFO_COUNT];",
        "",
        "#endif /* EMERALD_RESOURCES_ENCOUNTER_NATIVE_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/encounter_native.generated.h",
             enc_hdr, args)

    enc_c = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/encounter_native.generated.h"',
        "",
        "const char *const "
        "kEncounterHeaderKeys[EMERALD_ENCOUNTER_HEADER_COUNT][4] =",
        "{",
    ]
    for fields in enc_head_keys:
        enc_c.append("    {" + ", ".join('"%s"' % f for f in fields) + "},")
    enc_c += [
        "};",
        "",
        "const struct EmeraldEncounterInfoRecord "
        "kEncounterInfos[EMERALD_ENCOUNTER_INFO_COUNT] =",
        "{",
    ]
    for rkey in sorted(enc_infos):
        e = enc_infos[rkey]
        enc_c.append(f'    {{"{rkey}", {e["rate"]}u, 0x{e["infoPtr"]:08x}u, '
                     f'0x{e["slotPtr"]:08x}u, {e["slotRows"]}u}},')
    enc_c += [
        "};",
        "",
    ]
    write_if(root / "src/emerald/resources/encounter_native.generated.c",
             enc_c, args)

    # ---- frontier seam generated maps (R13-E3a-1) ---------------------------
    # The frontier publication seam (EmeraldFrontierCompat) rebuilds the native
    # gBattleFrontierTrainers[300] / gBattleFrontierMons[882] / held-items /
    # banned-species / tent tables from the frontier resources.
    # kFrontierTrainerKeys[i] / kFrontierMonSetKeys[i] are the trainer metadata
    # row and its 0xFFFF-terminated mon-set index-stream leaf; the seam
    # validates the row's GBA monSet pointer == leaf source ROM address for all
    # 300 edges and every leaf index 0..881, then rebuilds each native row's
    # monSet to point into its mon-set arena. kFrontierTent*Keys carry the same
    # per-trainer maps for the three tents (index 0=slateport 1=verdanturf
    # 2=fallarbor, each 30 trainers + a per-tent mons pool key).
    FRN = "EMERALD_FRONTIER_"
    fr_hdr = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-E3a-1 frontier publication maps and constants. The frontier seam",
        " * rebuilds the native Battle Frontier trainer/mon tables into their",
        " * HOST_DATA fill targets (frontier_data_native.c) from the pack.",
        " * kFrontierTrainerKeys[i] is the 52-byte GBA trainer metadata row;",
        " * kFrontierMonSetKeys[i] its 0xFFFF-terminated u16 index stream into",
        " * the shared 882-row gBattleFrontierMons pool (the seam validates each",
        " * row's GBA monSet pointer resolves to its leaf and every index is in",
        " * range, then rebuilds the native monSet into its mon-set arena).",
        " */",
        "#ifndef EMERALD_RESOURCES_FRONTIER_NATIVE_GENERATED_H",
        "#define EMERALD_RESOURCES_FRONTIER_NATIVE_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define {FRN}SCHEMA_TRAINER     21u",
        f"#define {FRN}SCHEMA_MON_SET     22u",
        f"#define {FRN}SCHEMA_MON         23u",
        f"#define {FRN}SCHEMA_HELD_ITEMS 24u",
        f"#define {FRN}SCHEMA_BANNED     25u",
        f"#define {FRN}TRAINER_COUNT      300u",
        f"#define {FRN}MONS_COUNT        882u",
        f"#define {FRN}HELDITEMS_COUNT     63u",
        f"#define {FRN}BANNED_COUNT        11u",
        f"#define {FRN}TENT_TRAINER_COUNT 30u",
        f"#define {FRN}TENT_SLATEPORT_MONS    70u",
        f"#define {FRN}TENT_VERDANTURF_MONS   45u",
        f"#define {FRN}TENT_FALLARBOR_MONS    45u",
        f"#define {FRN}MON_WIRE 16u",
        f"#define {FRN}MON_NATIVE 14u",
        f"#define {FRN}TRAINER_WIRE 52u",
        f"#define {FRN}TRAINER_NATIVE 56u",
        "",
        "enum FrontierTentTag",
        "{",
        "    FRONTIER_TENT_SLATEPORT = 0,",
        "    FRONTIER_TENT_VERDANTURF = 1,",
        "    FRONTIER_TENT_FALLARBOR = 2,",
        f"    FRONTIER_TENT_COUNT_ = 3,",
        "};",
        "",
        "/* frontier trainer index -> metadata resource key */",
        "extern const char *const "
        "kFrontierTrainerKeys[EMERALD_FRONTIER_TRAINER_COUNT];",
        "/* frontier trainer index -> mon-set leaf resource key */",
        "extern const char *const "
        "kFrontierMonSetKeys[EMERALD_FRONTIER_TRAINER_COUNT];",
        "extern const char *const kFrontierMonsKey;",
        "extern const char *const kFrontierHeldItemsKey;",
        "extern const char *const kFrontierBannedSpeciesKey;",
        "/* tent (slateport/verdanturf/fallarbor) trainer index -> keys */",
        "extern const char *const "
        "kFrontierTentTrainerKeys[3][EMERALD_FRONTIER_TENT_TRAINER_COUNT];",
        "extern const char *const "
        "kFrontierTentMonSetKeys[3][EMERALD_FRONTIER_TENT_TRAINER_COUNT];",
        "extern const char *const kFrontierTentMonsKeys[3];",
        "",
        "#endif /* EMERALD_RESOURCES_FRONTIER_NATIVE_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/frontier_native.generated.h",
             fr_hdr, args)

    fr_c = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/frontier_native.generated.h"',
        "",
        "const char *const "
        "kFrontierTrainerKeys[EMERALD_FRONTIER_TRAINER_COUNT] =",
        "{",
    ]
    for k in frontier_map["trainer_keys"]:
        fr_c.append(f'    "{k}",')
    fr_c += [
        "};",
        "",
        "const char *const "
        "kFrontierMonSetKeys[EMERALD_FRONTIER_TRAINER_COUNT] =",
        "{",
    ]
    for k in frontier_map["monset_keys"]:
        fr_c.append(f'    "{k}",')
    fr_c += [
        "};",
        "",
        f'const char *const kFrontierMonsKey = '
        f'"{frontier_map["mons_key"]}";',
        f'const char *const kFrontierHeldItemsKey = '
        f'"{frontier_map["held_key"]}";',
        f'const char *const kFrontierBannedSpeciesKey = '
        f'"{frontier_map["banned_key"]}";',
        "",
        "const char *const "
        "kFrontierTentTrainerKeys[3][EMERALD_FRONTIER_TENT_TRAINER_COUNT] =",
        "{",
    ]
    for tent in ("slateport", "verdanturf", "fallarbor"):
        fr_c.append("    {" + ", ".join(
            '"%s"' % k for k in frontier_map["tents"][tent]["trainer_keys"]) + "},")
    fr_c += [
        "};",
        "",
        "const char *const "
        "kFrontierTentMonSetKeys[3][EMERALD_FRONTIER_TENT_TRAINER_COUNT] =",
        "{",
    ]
    for tent in ("slateport", "verdanturf", "fallarbor"):
        fr_c.append("    {" + ", ".join(
            '"%s"' % k for k in frontier_map["tents"][tent]["monset_keys"]) + "},")
    fr_c += [
        "};",
        "",
        "const char *const kFrontierTentMonsKeys[3] =",
        "{",
    ]
    for tent in ("slateport", "verdanturf", "fallarbor"):
        fr_c.append(f'    "{frontier_map["tents"][tent]["mons_key"]}",')
    fr_c += [
        "};",
        "",
    ]
    write_if(root / "src/emerald/resources/frontier_native.generated.c",
             fr_c, args)

    # frontier metadata TO ML (index maps / graph edges for the report)
    frm = [
        "# Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py",
        "# Do not edit by hand; re-run the generator (regeneration must be a",
        "# no-op diff).",
        "",
        "# R13-E3a-1 frontier/tent index maps: trainer->mon-set leaf edges,",
        "# mon-index bounds, held-item/banned resources.",
        'family = "gameplay"',
        "frontier_version = 1",
        "",
    ]
    for i in range(300):
        frm.append("[[frontier_trainers]]")
        frm.append(f"index = {i}")
        frm.append(f'trainer = "{frontier_map["trainer_keys"][i]}"')
        frm.append(f'mon_set = "{frontier_map["monset_keys"][i]}"')
        frm.append("")
    frm.append(f'frontier_mons = "{frontier_map["mons_key"]}"')
    frm.append(f'frontier_held_items = "{frontier_map["held_key"]}"')
    frm.append(f'frontier_banned_species = "{frontier_map["banned_key"]}"')
    frm.append("mon_index_max = 881")
    frm.append("")
    write_if(famdir / "frontier_index_maps.generated.toml", frm, args)

    # --- R13-E3a-2 frontier AUX publication maps + constants. ----------------
    pike_wild = sorted((int(k.rsplit("-", 1)[1]), k)
                       for k in aux["wild_sets"]
                       if k.startswith("emerald:data/frontier/pike/wild/"))
    pyramid_wild = sorted((int(k.rsplit("-", 1)[1]), k)
                          for k in aux["wild_sets"]
                          if k.startswith("emerald:data/frontier/pyramid/wild/"))
    fx = "EMERALD_FRONTIER_AUX_"
    hdr = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-E3a-2 frontier facility AUX publication maps. The frontier seam",
        " * rebuilds the Battle Factory/Palace/Arena/Pike/Pyramid/Brain/"
        "Apprentice native tables + the pike/pyramid wild-encounter handoff"
        " from",
        " * the pack. kFrontierAux* arrays pair each facility table with its",
        " * resource key; the wild info table carries the header->info->slot"
        " edge",
        " * metadata (rates + GBA address linkage) the seam validates then"
        " rebuilds",
        " * into its own packed wild slot/info arena.",
        " */",
        "#ifndef EMERALD_RESOURCES_FRONTIER_AUX_NATIVE_GENERATED_H",
        "#define EMERALD_RESOURCES_FRONTIER_AUX_NATIVE_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define {fx}SCHEMA_FACTORY        26u",
        f"#define {fx}SCHEMA_PALACE         27u",
        f"#define {fx}SCHEMA_ARENA          28u",
        f"#define {fx}SCHEMA_PIKE_NPC       29u",
        f"#define {fx}SCHEMA_PIKE_SPEECH    30u",
        f"#define {fx}SCHEMA_PYRAMID_FLOOR 31u",
        f"#define {fx}SCHEMA_PYRAMID_ITEM  32u",
        f"#define {fx}SCHEMA_PYRAMID_SLOTS 33u",
        f"#define {fx}SCHEMA_BRAIN         34u",
        f"#define {fx}SCHEMA_APPRENTICE    35u",
        f"#define {fx}SCHEMA_WILD_HEADERS  36u",
        f"#define {fx}SCHEMA_WILD          37u",
        "",
        f"#define {fx}PIKE_NPC_SLOTS     25u   /* 25 rows GBA 8 B -> native 6 B */",
        f"#define {fx}PIKE_NPC_WIRE      8u",
        f"#define {fx}PIKE_NPC_NATIVE    6u",
        f"#define {fx}PYRAMID_FLOOR_SLOTS 16u /* 16 rows GBA 16 B -> native 13 B */",
        f"#define {fx}PYRAMID_FLOOR_WIRE 16u",
        f"#define {fx}PYRAMID_FLOOR_NATIVE 13u",
        f"#define {fx}APPRENTICE_ROWS    16u  /* 16 rows GBA 88 B -> native 86 B */",
        f"#define {fx}APPRENTICE_WIRE    88u",
        f"#define {fx}APPRENTICE_NATIVE  86u",
        f"#define {fx}BRAIN_MONS        42u  /* FrontierBrainMon 20 B, GBA==native */",
        f"#define {fx}BRAIN_MONS_WIRE    20u",
        f"#define {fx}WILD_SLOT_ROWS     12u  /* WildPokemon 4 B/row */",
        f"#define {fx}WILD_SLOT_WIRE      4u",
        f"#define {fx}WILD_INFO_WIRE      8u",
        f"#define {fx}PIKE_WILD_SETS      4u",
        f"#define {fx}PYRAMID_WILD_SETS   7u",
        f"#define {fx}WILD_INFO_COUNT   11u",
        "",
        "extern const char *const "
        "kFrontierAuxFactoryMoves[7];",
        "extern const char *const kFrontierAuxPalaceEarlyKey;",
        "extern const char *const kFrontierAuxPalaceLateKey;",
        "extern const char *const kFrontierAuxArenaShortKey;",
        "extern const char *const kFrontierAuxArenaLongKey;",
        "extern const char *const kFrontierAuxPikeNpcKey;",
        "extern const char *const "
        "kFrontierAuxPikeSpeeches[3];",
        "extern const char *const "
        "kFrontierAuxPikeWildMons[8];",
        "extern const char *const "
        "kFrontierAuxPyramidFloor[2];",
        "extern const char *const kFrontierAuxPyramidItemKey;",
        "extern const char *const kFrontierAuxPyramidSlotsKey;",
        "extern const char *const kFrontierAuxBrainIdsKey;",
        "extern const char *const kFrontierAuxBrainMonsKey;",
        "extern const char *const kFrontierAuxBrainStreakKey;",
        "extern const char *const "
        "kFrontierAuxApprenticeKeys[16];",
        "extern const char *const kFrontierAuxPikeWildHeadersKey;",
        "extern const char *const kFrontierAuxPyramidWildHeadersKey;",
        "",
        "/* wild slot-set metadata: rate + header->info->slot GBA address"
        " linkage */",
        "struct FrontierWildInfoMeta",
        "{",
        "    const char *key;",
        "    uint16_t rate;",
        "    uint16_t slotRows;",
        "    uint32_t gbaInfoAddr;",
        "    uint32_t gbaSlotAddr;",
        "};",
        "extern const struct FrontierWildInfoMeta "
        "kFrontierAuxPikeWildInfos[4];",
        "extern const struct FrontierWildInfoMeta "
        "kFrontierAuxPyramidWildInfos[7];",
        "",
        "#endif /* EMERALD_RESOURCES_FRONTIER_AUX_NATIVE_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/frontier_aux_native.generated.h",
             hdr, args)

    cb = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/frontier_aux_native.generated.h"',
        "",
    ]
    cb.append(
        "const char *const kFrontierAuxFactoryMoves[7] =")
    cb.append("{")
    for k in aux["factory_moves"]:
        cb.append(f'    "{k}",')
    cb.append("};")
    cb += [f'const char *const kFrontierAuxPalaceEarlyKey = '
           f'"{aux["palace_early"]}";',
           f'const char *const kFrontierAuxPalaceLateKey = '
           f'"{aux["palace_late"]}";',
           f'const char *const kFrontierAuxArenaShortKey = '
           f'"{aux["arena_short"]}";',
           f'const char *const kFrontierAuxArenaLongKey = '
           f'"{aux["arena_long"]}";',
           f'const char *const kFrontierAuxPikeNpcKey = '
           f'"{aux["pike_npc"]}";']
    cb.append(
        "const char *const kFrontierAuxPikeSpeeches[3] =")
    cb.append("{")
    for k in aux["pike_speeches"]:
        cb.append(f'    "{k}",')
    cb.append("};")
    cb.append(
        "const char *const kFrontierAuxPikeWildMons[8] =")
    cb.append("{")
    for k in aux["pike_wild_mons"]:
        cb.append(f'    "{k}",')
    cb.append("};")
    cb.append(
        "const char *const kFrontierAuxPyramidFloor[2] =")
    cb.append("{")
    for k in aux["pyramid_floor"]:
        cb.append(f'    "{k}",')
    cb.append("};")
    cb += [f'const char *const kFrontierAuxPyramidItemKey = '
           f'"{aux["pyramid_item"]}";',
           f'const char *const kFrontierAuxPyramidSlotsKey = '
           f'"{aux["pyramid_slots"]}";',
           f'const char *const kFrontierAuxBrainIdsKey = '
           f'"{aux["brain_ids"]}";',
           f'const char *const kFrontierAuxBrainMonsKey = '
           f'"{aux["brain_mons"]}";',
           f'const char *const kFrontierAuxBrainStreakKey = '
           f'"{aux["brain_streak"]}";']
    cb.append(
        "const char *const kFrontierAuxApprenticeKeys[16] =")
    cb.append("{")
    for k in aux["apprentice_keys"]:
        cb.append(f'    "{k}",')
    cb.append("};")
    cb += [f'const char *const kFrontierAuxPikeWildHeadersKey = '
           f'"{aux["wild_headers"]["pike"]}";',
           f'const char *const kFrontierAuxPyramidWildHeadersKey = '
           f'"{aux["wild_headers"]["pyramid"]}";', ""]
    for (arrname, lst) in (("kFrontierAuxPikeWildInfos", pike_wild),
                           ("kFrontierAuxPyramidWildInfos", pyramid_wild)):
        cb.append(
            "const struct FrontierWildInfoMeta %s[%d] =" % (
                arrname, len(lst)))
        cb.append("{")
        for (_n, rkey) in lst:
            v = aux["wild_sets"][rkey]
            cb.append(f'    {{"{rkey}", {v["rate"]}u, {v["slotRows"]}u, '
                      f'0x{v["infoPtr"]:08x}u, 0x{v["slotPtr"]:08x}u}},')
        cb.append("};")
        cb.append("")
    write_if(root / "src/emerald/resources/frontier_aux_native.generated.c",
             cb, args)

    # frontier AUX metadata TO ML.
    fam2 = [
        "# Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py",
        "# Do not edit by hand; re-run the generator (regeneration must be a",
        "# no-op diff).",
        "",
        "# R13-E3a-2 frontier facility AUX + pike/pyramid wild handoff.",
        'family = "gameplay"',
        "frontier_aux_version = 1",
        "",
        "[[wild_sets]]",
    ]
    for (_n, rkey) in pike_wild + pyramid_wild:
        v = aux["wild_sets"][rkey]
        fam2.append(f'set = "{rkey}"')
        fam2.append(f"rate = {v['rate']}")
        fam2.append(f"slot_rows = {v['slotRows']}")
        fam2.append("")
    write_if(famdir / "frontier_aux_maps.generated.toml", fam2, args)

    # ---- Pokédex seam generated maps + constants (R13-E3b) ----------------
    # The pokedex publication seam (EmeraldPokedexCompat) rebuilds the native
    # gPokedexEntries[387] + the four ordering/routing arrays from the pack.
    # kPokedexRowKeys[i] is the row resource key
    # (emerald:data/pokedex/<species>) for gPokedexEntries row i (national-dex
    # index); kPokedexDescLabels[i] is the R13-C description text identity
    # (emerald:text/pokedex/g<species>pokedextext) each row's GBA description
    # pointer must bind to. The seam validates every row's desc address
    # resolves to its label, then re-points description into the live R13-C
    # arena.
    PKD = "EMERALD_POKEDEX_"
    pk_hdr = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-E3b Pokédex publication maps. gPokedexEntries is indexed by",
        " * national-dex value (index 0 = the UNKNOWN/dummy row).",
        " * kPokedexRowKeys[i] is the row resource key for index i;",
        " * kPokedexDescLabels[i] the R13-C description text label that row",
        " * i's GBA description pointer must resolve to (387/387). The four",
        " * ordering/routing resources are exposed as single canonical keys.",
        " */",
        "#ifndef EMERALD_RESOURCES_POKEDEX_NATIVE_GENERATED_H",
        "#define EMERALD_RESOURCES_POKEDEX_NATIVE_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define {PKD}SCHEMA_ROW       38u",
        f"#define {PKD}SCHEMA_ORDER     39u",
        f"#define {PKD}SCHEMA_S2N       40u",
        f"#define {PKD}ROW_COUNT        387u",
        f"#define {PKD}ROW_WIRE         32u",
        f"#define {PKD}ROW_NATIVE       40u",
        f"#define {PKD}ALPHABETICAL_COUNT 411u",
        f"#define {PKD}HEIGHT_COUNT      386u",
        f"#define {PKD}WEIGHT_COUNT      386u",
        f"#define {PKD}S2N_COUNT         411u",
        "",
        "/* national-dex index -> row resource key (emerald:data/pokedex/"
        "<species>) */",
        "extern const char *const "
        "kPokedexRowKeys[EMERALD_POKEDEX_ROW_COUNT];",
        "/* national-dex index -> R13-C description text label */",
        "extern const char *const "
        "kPokedexDescLabels[EMERALD_POKEDEX_ROW_COUNT];",
        "",
        "extern const char *const kPokedexOrderAlphabeticalKey;",
        "extern const char *const kPokedexOrderHeightKey;",
        "extern const char *const kPokedexOrderWeightKey;",
        "extern const char *const kPokedexSpeciesToNationalKey;",
        "",
        "#endif /* EMERALD_RESOURCES_POKEDEX_NATIVE_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/pokedex_native.generated.h",
             pk_hdr, args)

    pk_c = [
        "/* Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/pokedex_native.generated.h"',
        "",
        "const char *const "
        "kPokedexRowKeys[EMERALD_POKEDEX_ROW_COUNT] =",
        "{",
    ]
    for k in pokedex_map["row_keys"]:
        pk_c.append(f'    "{k}",')
    pk_c += [
        "};",
        "",
        "const char *const "
        "kPokedexDescLabels[EMERALD_POKEDEX_ROW_COUNT] =",
        "{",
    ]
    for k in pokedex_map["desc_labels"]:
        pk_c.append(f'    "{k}",')
    pk_c += [
        "};",
        "",
        'const char *const kPokedexOrderAlphabeticalKey = '
        '"emerald:data/pokedex/order/alphabetical";',
        'const char *const kPokedexOrderHeightKey = '
        '"emerald:data/pokedex/order/height";',
        'const char *const kPokedexOrderWeightKey = '
        '"emerald:data/pokedex/order/weight";',
        'const char *const kPokedexSpeciesToNationalKey = '
        '"emerald:data/pokedex/species-to-national";',
        "",
    ]
    write_if(root / "src/emerald/resources/pokedex_native.generated.c",
             pk_c, args)

    # pokedex metadata TO ML (row keys + description bindings for the report
    # and the text-binding proof).
    pkm = [
        "# Generated by tools/gen3_resources/gameplay_family/"
        "gen_gameplay_family.py",
        "# Do not edit by hand; re-run the generator (regeneration must be a",
        "# no-op diff).",
        "",
        "# R13-E3b Pokédex row keys + description bindings.",
        'family = "gameplay"',
        "pokedex_version = 1",
        "",
    ]
    for i in range(387):
        pkm.append("[[rows]]")
        pkm.append(f"index = {i}")
        pkm.append(f'row_key = "{pokedex_map["row_keys"][i]}"')
        pkm.append(f'description_label = "{pokedex_map["desc_labels"][i]}"')
        pkm.append("")
    write_if(famdir / "pokedex_maps.generated.toml", pkm, args)


def _artifact_sub(key):
    if key.startswith("emerald:data/species/"):
        sub = key.rsplit("/", 1)[-1]
        return {"name": "species-name", "evolutions": "evolutions",
                "levelup": "levelup", "tmhm": "tmhm", "tutor": "tutor",
                "egg-moves": "egg-moves"}.get(sub, "species-base")
    if key.startswith("emerald:data/move/"):
        return "move-name" if key.endswith("/name") else (
            "move-contest" if key.endswith("/contest") else "move-battle")
    if key.startswith("emerald:data/item/"):
        return "items"
    if key.startswith("emerald:data/trainer/"):
        return "trainer-party" if key.endswith("/party") else "trainer"
    if key.startswith("emerald:data/trainer-class/"):
        return "trainer-class-name"
    if key == "emerald:data/encounter/headers":
        return "encounter-headers"
    if key.startswith("emerald:data/encounter/"):
        return "encounter"
    if key.startswith("emerald:data/frontier/trainer/"):
        return "frontier-mon-set" if key.endswith("/mons") else "frontier-trainer"
    if key == "emerald:data/frontier/mons":
        return "frontier-mons"
    if key == "emerald:data/frontier/held-items":
        return "frontier-held-items"
    if key == "emerald:data/frontier/banned-species":
        return "frontier-banned-species"
    if key.startswith("emerald:data/frontier/factory/"):
        return "frontier-factory"
    if key.startswith("emerald:data/frontier/palace/"):
        return "frontier-palace"
    if key.startswith("emerald:data/frontier/arena/"):
        return "frontier-arena"
    if key.startswith("emerald:data/frontier/") and "/wild/" in key:
        return "frontier-wild-headers" if key.endswith("/wild/headers") else "frontier-wild"
    if key == "emerald:data/frontier/pike/npc":
        return "frontier-pike-npc"
    if key.startswith("emerald:data/frontier/pike/"):
        # speeches / room-hints / heals / pike wild-mons tables (schema 30)
        return "frontier-pike-speech"
    if key.startswith("emerald:data/frontier/pyramid/items/"):
        return "frontier-pyramid-item"
    if key == "emerald:data/frontier/pyramid/item-slots":
        return "frontier-pyramid-slots"
    if key.startswith("emerald:data/frontier/pyramid/"):
        # floor-templates + floor-options (schema 31)
        return "frontier-pyramid-floor"
    if key.startswith("emerald:data/frontier/brain/"):
        return "frontier-brain"
    if key.startswith("emerald:data/frontier/apprentice/"):
        return "frontier-apprentice"
    if key.startswith("emerald:data/frontier/tent/"):
        parts = key[len("emerald:data/frontier/tent/"):].split("/")
        if len(parts) == 2:
            return "tent-mons"
        if parts[-1] == "mons":
            return "tent-mon-set"
        return "tent-trainer"
    if key.startswith("emerald:data/growth-rate/"):
        return "growth-rate"
    if key == "emerald:data/tutor/moves":
        return "tutor-moves"
    if key == "emerald:data/contest/effects":
        return "contest-effects"
    if key == "emerald:data/contest/combo-starters":
        return "contest-combo-starters"
    if key.startswith("emerald:data/pokedex/order/"):
        return "pokedex-order"
    if key == "emerald:data/pokedex/species-to-national":
        return "pokedex-species-to-national"
    if key.startswith("emerald:data/pokedex/"):
        return "pokedex-row"
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
    rows, fam, levelup_idx_map, species_keys, move_keys, item_keys, \
        callback_rows, trainer_keys, party_keys, party_meta, class_keys, \
        enc_head_keys, enc_infos, frontier_map, aux, pokedex_map = \
        derive(elf, rom, pret)
    emit_gameplay(rows, fam, levelup_idx_map, species_keys, move_keys,
                  item_keys, callback_rows, trainer_keys, party_keys,
                  party_meta, class_keys, enc_head_keys, enc_infos,
                  frontier_map, aux, pokedex_map, outdir, root, args)

    print(f"gameplay resources: {len(rows)}, "
          f"{sum(r[3] for r in rows)} B (D1+D2+E1+E2; evolutions excluded)")
    print(f"item callback census: {len(callback_rows)} distinct")
    for family in sorted(PINNED_BY_FAMILY):
        print(f"  {family}: {fam.get(family, 0)} resources")
    print(f"trainer metadata: {len(trainer_keys)} / party: "
          f"{sum(1 for k in party_keys if k)} / class: {len(class_keys)}")
    print(f"encounter headers block: 1 / slot tables: {len(enc_infos)}")
    print(f"pokedex rows: {len(pokedex_map['row_keys'])} / ordering+routing: "
          f"4 resources (row bytes 12384; order 2366; s2n 822)")


if __name__ == "__main__":
    main()

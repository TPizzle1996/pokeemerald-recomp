#!/usr/bin/env python3
"""R13-G2: Emerald field-script module resource generator (additive-only).

Usage:
  gen_script_family.py [root] [elf] [rom] [outdir] [--check]

Scope (R13-G2 brief, sections 1-3): emit exactly 523 semantic script
module resources from the qualified build:
  - 468 map modules      emerald:script/map/<map>            (schema 45)
  -  47 common modules   emerald:script/common/<module>      (schema 45)
  -   8 gift modules     emerald:script/mystery-gift/<gift>  (schema 46)
preserving exact qualified-ROM owned byte segments (206,638 main +
692 mystery-gift = 207,330 B), typed sparse segments, exports with
aliases and GBA provenance, interior bindings, boundary metadata, and
all 16,704 relocation sidecars with the G1 graph class partition
(8,208/6,207/2,009/262/18) and the 95 root / 8,113 interior split.

The byte model closes the 972,520-byte main object as class sums:
  engine routing 3,152 + map routing 5,749 + field bytecode 206,638
  + movement 7,416 + R13-C text 749,565 = 972,520.
The mystery-gift object closes separately: 0xD2E = 3,374 B = 692 gift
  bytecode + 2,682 gift text (spans of the 20 sText_ labels; C handoff).
Text and movement holes are external bindings (C/B); routing holes are
the map dispatch/conditional tables (typed G routing segments, emitted
as their own resources by the G2 routing sub-stage). Relocation graph,
classes, and interior proof are re-derived from the G1 oracle
(field_script_extractor.collect_qualified_graph) so G2 metadata
reproduces G1's graph exactly.

Byte-total reconciliation (2026-08-20): the authoritative qualified G
pin is 207,330 B (206,638 main + 692 gift). The former 206,283 figure
was recomp-era bookkeeping derived from stale/pre-correction R13-C
text accounting; see docs/R13G_FIELD_SCRIPT_MIGRATION_PLAN.md §18.1.

Pins (hard gates; any mismatch stops generation):
  523 modules, 207,330 bytecode bytes, 16,704 operands,
  8,208/6,207/2,009/262/18 classes, 95 root / 8,113 interior,
  5,749 routing bytes, ROM SHA-1 gate, zero unresolved/ambiguous.
"""

import argparse
import bisect
import hashlib
import re
import resource
import struct
import sys
import tomllib
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import field_script_extractor as fe
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "text_family"))
from gen_text_family import RENAME_MAP as RECOMP_TO_QUALIFIED_TEXT_RENAME

# Qualified-ELF label -> recomp-catalog label (reverse of the C family's
# authoritative RENAME_MAP). C's bundle catalog was generated from the
# recomp ELF, so AC-3-renamed text labels resolve only through the alias.
QUALIFIED_TO_RECOMP_TEXT = {v: k for k, v in RECOMP_TO_QUALIFIED_TEXT_RENAME.items()}

GEN3_GBA_ROM_BASE = fe.GEN3_GBA_ROM_BASE
ROM_SHA1 = fe.ROM_SHA1

MAIN_OBJECT_BYTES = 972520
ENGINE_PREFIX_BYTES = 3152
ROUTING_BYTES_PIN = 5749
MOVEMENT_BYTES_PIN = 7416
TEXT_BYTES_PIN = 749565
MAIN_BYTECODE_PIN = 206638
GIFT_BYTECODE_PIN = 692
BYTECODE_PIN = MAIN_BYTECODE_PIN + GIFT_BYTECODE_PIN
GIFT_OBJECT_BYTES = 0xD2E            # full .rodata mystery-gift object (3,374 B)
GIFT_TEXT_BYTES_PIN = 2682           # spans of the 20 gift sText_ labels (C handoff)
GIFT_TEXT_LABELS_PIN = 20            # unique gift text labels
GIFT_TEXT_EDGES_PIN = 20             # TEXT_TARGET operands into gift text (plan §7.3)
MAP_MODULES_PIN = 468
COMMON_MODULES_PIN = 47
GIFT_MODULES_PIN = 8
MODULES_PIN = 523
# Payload-bearing modules embedded in the pack manifest. The 56 routing-only
# modules (terminator-only `.byte 0` map-script tables, plan §5) have empty
# payloads and stay catalog/ownership/meta surface only.
MANIFEST_RECORDS_PIN = 467
OPERANDS_PIN = 16704
CLASS_PINS = {
    "SCRIPT_TARGET": 8208,
    "TEXT_TARGET": 6207,
    "MOVEMENT_TARGET": 2009,
    "MART_TABLE_TARGET": 262,
    "RAM_DATA_TARGET": 18,
}
ROOT_TARGETS_PIN = 95
INTERIOR_TARGETS_PIN = 8113
RAM_SPLIT_PIN = (18, 0)                 # all 18 RAM_DATA_TARGETs are the
                                        # gStringVar4 buffer (EWRAM 0x02021fc4,
                                        # 1,000 B); no IWRAM targets in this build
TEXT_FILES_PIN = 36
TEXT_ALIAS_EDGES_PIN = 2             # AC-3-renamed text targets via RENAME_MAP
TEXT_ALIAS_LABELS = {
    "OldaleTown_Text_CitySign",          # catalog: OldaleTown_Text_TownSign
    "MauvilleCity_PokemonCenter_1F_Text_HaveYouHeardOfPhrase",  # ..._Word
}
BRAILLE_TEXT_LABELS_PIN = 22         # braille.inc is text-class bytes with no
BRAILLE_TEXT_EDGES_PIN = 26          # C catalog record (plan §7.5: text stays C)
BRAILLE_TEXT_BYTES_PIN = 552         # spans [braille label, next symbol)
SCHEMA_MAIN = 45
SCHEMA_GIFT = 46

# Recomp-local movement bridges: 12 B of movement data whose symbols are
# NOT in the R13-B binding catalog (they were renamed in the recomp
# checkout; B documents them in its leaf seam header). Qualified names
# and module-relative offsets:
RECOMP_LOCAL_MOVEMENT = [
    ("Route103_EventScript_RivalExitFacingNorth2", 0x10F45, 7),
    ("Ferry_EventScript_DepartIslandBoardSouth", 0x673BB, 2),
    ("Ferry_EventScript_DepartIslandBoardWest", 0x673BD, 3),
]

# R13-G2 §7: gStdScripts shadow binding records.  The 11 slots occupy the
# 44-byte span [gScriptCmdTable+0xC24, +0xC50); records are metadata only
# (the 44 bytes remain provenance in the field bytecode region; live
# publication is phase-2/R13-H).
STDSCRIPTS_PIN = 11
STD_SLOT_OFFSET = 0xC24
# Boundary split (offset-zero / interior / routing): the 11 Std_ scripts
# are G bytecode; none is a routing-class boundary.
STD_BOUNDARY_SPLIT = (3, 8, 0)

# R13-G2 §8: F inbound binding metadata (MapHeader.mapScripts,
# ObjectEventTemplate.script, CoordEvent.script, BgEvent.script -> script
# module key/export/interior offset/valid ENTRYPOINT boundary).
# Boundary kinds: "offset-zero" (G export at module root), "interior" (G
# bytecode instruction boundary), "routing" (map-script dispatch table
# byte: every MapHeader.mapScripts pointer targets the map's dispatch
# table start, whose tag byte is routing-owned per the G1 qualified
# model; terminator-only `.byte 0` tables are routing bytes too).
F_INBOUND_PINS = {
    "map-scripts": 518,
    "object-event": 2163,
    "coord-event": 289,
    "bg-event": 531,
}
F_INBOUND_BOUNDARY_SPLIT = {
    "map-scripts": (0, 0, 518),
    "object-event": (129, 2034, 0),
    "coord-event": (0, 289, 0),
    "bg-event": (6, 525, 0),
}
# Per-kind same_map counts (reference lands in its own map's module):
# 832 of 3,501 refs are legitimate cross-map/common-module targets
# (16 Secret Base maps -> shared-secret-base, BattlePyramidSquare02-06
# -> battlepyramidsquare01, object/coord/bg refs -> common modules
# like item-ball-scripts / pc / berry-tree / field-poison).
F_INBOUND_SAME_MAP_SPLIT = {
    "map-scripts": 468,
    "object-event": 1496,
    "coord-event": 254,
    "bg-event": 451,
}

# R13-G2 §5: all 2,009 MOVEMENT_TARGET edges resolve to R13-B ownership
# (movement/bindings.generated.toml keys); the three recomp-local
# 12-byte movement bridges (7+2+3) stay explicit named compiled bridges.
MOVEMENT_EDGES_PIN = 2009
MOVEMENT_BRIDGE_BYTES_PIN = 12

# R13-G2 §7.5: mart/decor tables are typed G module data segments, each
# a u16 item list ending in the ITEM_NONE sentinel (0x0000). The 38
# tables (742 B) are exactly the MART_TABLE_TARGET operand targets that
# land in module G bytes; a name heuristic cannot identify them
# (Common_EventScript_ShowPokemartSign contains "Pokemart" but is a
# script, not a table).
MART_TABLES_PIN = 38
MART_TABLE_BYTES_PIN = 742

LABEL_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)::?$")
INCLUDE_RE = re.compile(
    r'^\s*\.include "data/(maps/([A-Za-z0-9_]+)/scripts\.inc|scripts/([A-Za-z0-9_]+)\.inc)"')

JUNK_PREFIXES = ("$d", "$a", "$t", "$u")


def fail(msg):
    fe.fail(msg)


def sha256hex(b):
    return hashlib.sha256(b).hexdigest()


def rss_mb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024


def derive_key(resource_id):
    return hashlib.sha256(b"gen3-resource-id-v1\0" + resource_id.encode()).hexdigest()


def slugify(name):
    s = name.lower().replace("_", "-")
    if not re.fullmatch(r"[a-z0-9.\-]+", s):
        fail(f"slugify produced invalid resource segment: {name!r} -> {s!r}")
    return s


def write_if(path, text, check):
    if check:
        if not path.exists():
            fail(f"--check: {path} does not exist")
        if path.read_text() != text:
            fail(f"--check: {path} differs from deterministic regeneration")
        print(f"check passed: {path}")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        print(f"wrote {path}")


def write_bin_if(path, data, check):
    if check:
        if not path.exists():
            fail(f"--check: {path} does not exist")
        if path.read_bytes() != data:
            fail(f"--check: {path} differs from deterministic regeneration")
        print(f"check passed: {path}")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(f"wrote {path}")


class Module:
    def __init__(self, kind, name, file_label_names, region_start, region_end, schema):
        self.kind = kind                 # "map" | "common" | "mystery-gift"
        self.name = name                 # raw name (map dir / file basename)
        self.file_label_names = file_label_names
        self.region_start = region_start
        self.region_end = region_end
        self.schema = schema
        self.id = f"emerald:script/{kind}/{slugify(name)}"
        self.key = derive_key(self.id)
        self.segments = []               # {kind, original_gba_start, byte_count, payload_offset}
        self.exports = []                # {name, original_gba_address, provenance_offset, payload_offset, boundary_kind, aliases}
        self.relocs = []                 # {operand_payload_offset, operand_width, original_encoded_gba, target_class, target_resource_key, target_export, target_offset, runtime_resolution_required}
        self.payload = b""
        self.gbytes = set()
        self.g_label_addrs = []
        self.primary_symbol = ""
        self.bytecode_bytes = 0
        self.static_data_bytes = 0

    @property
    def payload_bytes(self):
        return len(self.payload)


def parse_file_labels(path, val_by_name):
    names = set()
    for ln in path.read_text().splitlines():
        m = LABEL_RE.match(ln)
        if m:
            names.add(m.group(1))
    return names, sorted({val_by_name[n] for n in names if n in val_by_name})


def build_routing_set(rom, elf, base):
    """Replicate G1's routing byte model exactly: map tables rows [p, p+5)
    plus one terminator byte per table, conditional rows [q, q+8) plus a
    two-byte terminator per conditional table (total 5,749 B).

    Returns (routing, source_tables): source_tables maps every reloc
    source address inside the routing class to the enclosing table label
    (map table symbol, or "cond_0x<addr>" for a conditional table)."""
    routing = set()
    source_tables = {}
    map_syms = elf.syms_by_pattern(r"_MapScripts$", shndx=4)
    cond_refs = set()
    for ts in sorted(map_syms, key=lambda s: s["value"]):
        p = ts["value"]
        for _ in range(32):
            tag = rom[p - GEN3_GBA_ROM_BASE]
            if tag == 0:
                routing.add(p)
                break
            routing.update(range(p, p + 5))
            source_tables[p + 1] = ts["name"]
            if tag in (2, 4):
                cond_refs.add(struct.unpack_from("<I", rom, p + 1 - GEN3_GBA_ROM_BASE)[0])
            p += 5
    for addr in sorted(cond_refs):
        q = addr
        for _ in range(256):
            if struct.unpack_from("<H", rom, q - GEN3_GBA_ROM_BASE)[0] == 0:
                routing.update(range(q, q + 2))
                break
            routing.update(range(q, q + 8))
            source_tables[q + 4] = f"cond_0x{addr:X}"
            q += 8
    if len(routing) != ROUTING_BYTES_PIN:
        fail(f"routing byte set {len(routing)} != {ROUTING_BYTES_PIN}")
    return routing, source_tables


def build_gift_text_class(elf, gift_first, gift_end):
    """The 2,682-byte gift text class: spans [sText_ label, next symbol)
    inside the 0xD2E-byte mystery-gift object. These bytes are handed to
    C (Task #3); they are excluded from every G payload, including the
    692-byte gift bytecode complement (object 3,374 = 692 G + 2,682 C)."""
    objsyms = sorted({s["value"] for s in elf.symbols
                      if gift_first <= s["value"] < gift_end})
    labels = sorted(s["value"] for s in elf.symbols
                    if gift_first <= s["value"] < gift_end
                    and s["name"].startswith("sText_"))
    if len(labels) != GIFT_TEXT_LABELS_PIN:
        fail(f"gift text labels {len(labels)} != {GIFT_TEXT_LABELS_PIN}")
    tset = set()
    j = 0
    n = len(objsyms)
    for a in labels:
        while j < n and objsyms[j] <= a:
            j += 1
        nxt = objsyms[j] if j < n else gift_end
        tset.update(range(a, nxt))
    if len(tset) != GIFT_TEXT_BYTES_PIN:
        fail(f"gift text class bytes {len(tset)} != {GIFT_TEXT_BYTES_PIN}")
    return tset


def build_class_spans(elf, base, main_end, starts, extra_spans=()):
    """Map a label-start set to byte spans via the next-symbol gap rule.

    Matches the qualified partition rule exactly: labels are resolved
    against section-4 symbols only, and the engine prefix [base,
    base+ENGINE_PREFIX_BYTES) is excluded from every class (it is the
    engine's 3,152-byte provenance table, counted separately)."""
    all_addrs = sorted({s["value"] for s in elf.symbols
                        if s["shndx"] == 4 and s["value"] >= GEN3_GBA_ROM_BASE})
    window_start = base + ENGINE_PREFIX_BYTES
    starts = sorted(a for a in starts
                    if window_start <= a < main_end)
    byteset = set()
    j = bisect.bisect_left(all_addrs, window_start)
    n = len(all_addrs)
    for a in starts:
        while j < n and all_addrs[j] <= a:
            j += 1
        nxt = all_addrs[j] if j < n else main_end
        if nxt > main_end:
            nxt = main_end
        byteset.update(range(a, nxt))
    for a, size in extra_spans:
        byteset.update(range(a, a + size))
    return byteset


def build_regions(elf, reference_root, base, main_end, val_by_name):
    """Module regions from the event_scripts.s include order.

    One module per included file (map or common); a module's region is
    [first surviving label of its file, first surviving label of the next
    included file); the last region closes at the main-object end.
    Modules whose region contains no G-owned bytecode are dropped (the
    movement-only include is R13-B, not a G module)."""
    order = []          # (kind, file_path, label_names)
    src = (reference_root / "data/event_scripts.s").read_text()
    for ln in src.splitlines():
        m = INCLUDE_RE.match(ln)
        if not m:
            continue
        if m.group(1).startswith("maps/"):
            kind, fname = "map", reference_root / ("data/maps/" + m.group(2) + "/scripts.inc")
        else:
            kind, fname = "common", reference_root / ("data/scripts/" + m.group(3) + ".inc")
        names, addrs = parse_file_labels(fname, val_by_name)
        if not addrs:
            fail(f"include {fname} has no surviving labels in the qualified ELF")
        order.append((kind, fname, names, addrs[0]))
    # Regions: [min_i, min_{i+1}), last closes at main_end.
    modules = []
    for i, (kind, fname, names, min_addr) in enumerate(order):
        nxt = order[i + 1][3] if i + 1 < len(order) else main_end
        if nxt > main_end:
            nxt = main_end
        base_name = fname.stem
        if kind == "map":
            base_name = fname.parent.name
        m = Module(kind, base_name, names, min_addr, nxt, SCHEMA_MAIN)
        # primary symbol: the file label at the region start (the map
        # table or first script label; survives even for terminator-only
        # maps whose region holds no bytecode).
        m.primary_symbol = next((n for n in sorted(names)
                                 if val_by_name.get(n) == min_addr), base_name)
        modules.append(m)
    return modules


def build_gift_modules(elf, reference_root, val_by_name, gift_base, gift_end):
    order = []
    src = (reference_root / "data/mystery_gift.s").read_text()
    for ln in src.splitlines():
        m = re.match(r'^\s*\.include "data/scripts/([A-Za-z0-9_]+)\.inc"', ln)
        if not m:
            continue
        fname = reference_root / f"data/scripts/{m.group(1)}.inc"
        names, addrs = parse_file_labels(fname, val_by_name)
        if not addrs:
            fail(f"gift include {fname} has no surviving labels in the qualified ELF")
        order.append((m.group(1), names, addrs[0]))
    modules = []
    for i, (name, names, min_addr) in enumerate(order):
        nxt = order[i + 1][2] if i + 1 < len(order) else gift_end
        if nxt > gift_end:
            nxt = gift_end
        m = Module("mystery-gift", name, names, min_addr, nxt, SCHEMA_GIFT)
        m.primary_symbol = next((n for n in sorted(names)
                                 if val_by_name.get(n) == min_addr), name)
        modules.append(m)
    return modules


def names_at(addr, names_by_addr):
    return names_by_addr.get(addr, [])


def find_module(modules, addr):
    """Binary search: the module whose region contains addr, or None."""
    lo, hi = 0, len(modules)
    while lo < hi:
        mid = (lo + hi) // 2
        if modules[mid].region_start <= addr:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None
    m = modules[lo - 1]
    return m if addr < m.region_end else None


def emit_all(outdir, args, rom, modules, text_class_bytes, move_class_bytes,
             routing, operands, routing_relocs, gift_text_edges,
             braille_text_edges, bundle_of, move_id_of, base, std,
             object_refs, coord_refs, bg_refs, map_script_refs,
             names_by_addr):
    out = outdir / "script" / "modules"
    check = args.check
    by_start = sorted(modules, key=lambda m: m.region_start)

    main_modules = [m for m in modules if m.kind != "mystery-gift"]
    gift_modules = [m for m in modules if m.kind == "mystery-gift"]
    n_map = sum(1 for m in modules if m.kind == "map")
    n_common = sum(1 for m in modules if m.kind == "common")
    n_gift = sum(1 for m in modules if m.kind == "mystery-gift")

    # --- gates over relocations -------------------------------------
    all_relocs = [r for m in modules for r in m.relocs] + routing_relocs
    if len(all_relocs) != OPERANDS_PIN:
        fail(f"module+routing relocs {len(all_relocs)} != {OPERANDS_PIN}")
    if len(gift_text_edges) != GIFT_TEXT_EDGES_PIN:
        fail(f"gift text edges {len(gift_text_edges)} != {GIFT_TEXT_EDGES_PIN}")
    if len({e["target_label"] for e in gift_text_edges}) != GIFT_TEXT_LABELS_PIN:
        fail(f"gift text edge labels != {GIFT_TEXT_LABELS_PIN} unique")
    # R13-G2 §7.3 prerequisite gate (Task #3 handoff): every gift-text
    # edge's key must exist in C's published catalog
    # (emerald:text/mystery-gift/<canonical> from gen_text_family.py's
    # mystery-gift family). "Missing identities after that prerequisite
    # must be zero" - fail-closed here, not at pack build time.
    cat = outdir / "text" / "catalog.generated.toml"
    try:
        catalog_ids = {ln.split('"')[1] for ln in cat.read_text().splitlines()
                       if ln.startswith('id = "emerald:')}
    except OSError:
        fail(f"text catalog missing at {cat}: run gen_text_family.py "
             f"(mystery-gift family) before the script family")
    missing = sorted({e["target_resource_key"] for e in gift_text_edges}
                     - catalog_ids)
    if missing:
        fail(f"{len(missing)} gift-text edges resolve to missing C "
             f"catalog ids: {missing}")
    cls = Counter(r["target_class"] for r in all_relocs)
    for k, v in CLASS_PINS.items():
        if cls[k] != v:
            fail(f"class {k}: {cls[k]} != {v}")
    roots = sum(1 for r in all_relocs
                if r["target_class"] == "SCRIPT_TARGET" and r["target_offset"] == 0)
    interior = sum(1 for r in all_relocs
                   if r["target_class"] == "SCRIPT_TARGET" and r["target_offset"] != 0)
    if roots != ROOT_TARGETS_PIN or interior != INTERIOR_TARGETS_PIN:
        fail(f"script-target split {roots}/{interior} != {ROOT_TARGETS_PIN}/{INTERIOR_TARGETS_PIN}")

    # --- artifact + meta emission -----------------------------------
    kind_dir = {"map": "map", "common": "common", "mystery-gift": "mystery-gift"}
    for m in modules:
        d = out / "data" / kind_dir[m.kind]
        p = out / "meta" / kind_dir[m.kind]
        write_bin_if(d / f"{m.name.lower()}.bin", m.payload, check)
        write_if(p / f"{m.name.lower()}.toml", render_meta(m), check)

    # --- family tomls ------------------------------------------------
    ram_ops = [op for op in operands
               if op["target_class"] == "RAM_DATA_TARGET"]
    write_if(out / "inventory.generated.toml",
             render_inventory(main_modules, gift_modules, gift_text_edges,
                              braille_text_edges, ram_ops), check)
    write_if(out / "bindings.generated.toml", render_bindings(modules), check)
    write_if(out / "catalog.generated.toml", render_catalog(modules), check)
    write_if(out / "ownership.generated.toml", render_ownership(modules), check)
    write_if(out / "consumers.generated.toml", render_consumers(), check)
    write_if(out / "manifest.production.toml", render_manifest(rom, modules), check)
    write_if(out / "routing_relocations.generated.toml", render_routing(routing_relocs), check)
    write_if(out / "movement_edges.generated.toml",
             render_movement_edges(modules, routing_relocs, base), check)
    write_if(out / "stdscripts.generated.toml",
             render_stdscripts(std, rom, base, by_start, routing,
                               move_class_bytes, names_by_addr), check)
    write_if(out / "f_inbound_bindings.generated.toml",
             render_f_inbound(map_script_refs, object_refs, coord_refs,
                              bg_refs, by_start, routing, move_class_bytes,
                              names_by_addr), check)
    write_if(out / "mart_tables.generated.toml",
             render_mart_tables(modules), check)

    print(f"\n=== R13-G2 RESULTS ===")
    print(f"Modules: {len(modules)} (pinned: {MODULES_PIN})  [{n_map} map, {n_common} common, {n_gift} gift]")
    print(f"Payload bytes: {sum(m.payload_bytes for m in modules)} (pinned: {BYTECODE_PIN})")
    print(f"  main {sum(m.payload_bytes for m in main_modules)} (pinned: {MAIN_BYTECODE_PIN}), "
          f"gift {sum(m.payload_bytes for m in gift_modules)} (pinned: {GIFT_BYTECODE_PIN})")
    print(f"Class partition: {dict(cls)}")
    print(f"Script-target split: {roots}/{interior} (pinned: {ROOT_TARGETS_PIN}/{INTERIOR_TARGETS_PIN})")
    print(f"Text class bytes: {len(text_class_bytes)} (pinned: {TEXT_BYTES_PIN})")
    print(f"Movement class bytes: {len(move_class_bytes)} (pinned: {MOVEMENT_BYTES_PIN})")
    print(f"Routing relocs: {len(routing_relocs)}; gift-text edges: {len(gift_text_edges)}"
          f" ({len({e['target_label'] for e in gift_text_edges})} labels, {GIFT_TEXT_BYTES_PIN} B, C handoff)")
    movement_edges = [r for r in all_relocs if r["target_class"] == "MOVEMENT_TARGET"]
    print(f"Movement edges: {len(movement_edges)} (pinned: {MOVEMENT_EDGES_PIN}) "
          f"+ {len(RECOMP_LOCAL_MOVEMENT)} bridges "
          f"({MOVEMENT_BRIDGE_BYTES_PIN} B, B leaf seam)")
    print(f"gStdScripts: {len(std)} shadow records "
          f"(slot span {base + STD_SLOT_OFFSET:#x}.."
          f"{base + STD_SLOT_OFFSET + 4 * STDSCRIPTS_PIN:#x})")
    print(f"F inbound: {len(map_script_refs)} map-scripts + "
          f"{len(object_refs)} object + {len(coord_refs)} coord + "
          f"{len(bg_refs)} bg = {len(map_script_refs) + len(object_refs) + len(coord_refs) + len(bg_refs)} "
          f"entrypoints, zero unresolved")
    mart_segs = [s for m in modules for s in m.segments
                 if s.get("kind") == "static-data"]
    print(f"Mart/decor tables: {len(mart_segs)} "
          f"({sum(s['byte_count'] for s in mart_segs)} B, ITEM_NONE sentinel "
          f"validated; 198 routing dispatch-table targets, "
          f"{BRAILLE_TEXT_LABELS_PIN} braille labels -> C handoff)")
    ew = sum(1 for o in ram_ops if 0x02000000 <= o["target_gba"] < 0x02040000)
    if (ew, len(ram_ops) - ew) != RAM_SPLIT_PIN:
        fail(f"RAM target split {(ew, len(ram_ops) - ew)} != {RAM_SPLIT_PIN}")
    print(f"RAM targets: {len(ram_ops)} allowlisted ({ew} EWRAM, "
          f"{len(ram_ops) - ew} IWRAM; no resource, never arena data)")
    embedded = sum(1 for m in modules if m.payload)
    if embedded != MANIFEST_RECORDS_PIN:
        fail(f"manifest payload-bearing modules {embedded} "
             f"!= {MANIFEST_RECORDS_PIN}")
    print(f"Pack manifest: {embedded} embedded module records "
          f"({len(modules) - embedded} routing-only catalog identities)")
    print(f"\nDone. Files in {out}/")


def render_meta(m):
    lines = [
        "# Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; regeneration must be a no-op diff.",
        "",
        "# R13-G2 canonical pack record (plan §4.4): exact qualified-ROM",
        "# bytes in deterministic segment order, never rewritten to native",
        "# pointers. Segments/exports/relocs carry GBA provenance only.",
        "",
        f"module_key = \"{m.id}\"",
        f"key = \"{m.key}\"",
        f"schema = {m.schema}",
        f"primary_symbol = \"{m.primary_symbol}\"",
        f"module_digest = \"{sha256hex(m.payload)}\"",
        f"region_gba_start = {m.region_start:#x}",
        f"region_byte_count = {m.region_end - m.region_start}",
        f"bytecode_bytes = {m.bytecode_bytes}",
        f"static_data_bytes = {m.static_data_bytes}",
        f"segment_count = {len(m.segments)}",
        f"export_count = {len(m.exports)}",
        f"reloc_count = {len(m.relocs)}",
        "",
    ]
    for s in m.segments:
        lines += [
            "[[segments]]",
            f"kind = \"{s['kind']}\"",
        ]
        if s.get("data_kind"):
            lines.append(f"data_kind = \"{s['data_kind']}\"")
        lines += [
            f"original_gba_start = {s['original_gba_start']:#x}",
            f"byte_count = {s['byte_count']}",
            f"payload_offset = {s['payload_offset']}",
            "",
        ]
    for e in m.exports:
        lines += [
            "[[exports]]",
            f"name = \"{e['name']}\"",
            f"original_gba_address = {e['original_gba_address']:#x}",
            f"provenance_offset = {e['provenance_offset']}",
            f"payload_offset = {e['payload_offset']}",
            f"boundary_kind = \"{e['boundary_kind']}\"",
            f"aliases = [{', '.join('\"%s\"' % a for a in e['aliases'])}]",
            "",
        ]
    for r in m.relocs:
        lines += [
            "[[relocs]]",
            f"operand_payload_offset = {r['operand_payload_offset']}",
            f"operand_width = {r['operand_width']}",
            f"original_encoded_gba = {r['original_encoded_gba']:#x}",
            f"target_class = \"{r['target_class']}\"",
            f"target_resource_key = \"{r['target_resource_key']}\"",
        ]
        if r.get("target_export"):
            lines.append(f"target_export = \"{r['target_export']}\"")
        if r.get("target_offset") is not None:
            lines.append(f"target_offset = {r['target_offset']}")
        lines += [
            f"runtime_resolution_required = {str(r['runtime_resolution_required']).lower()}",
            "",
        ]
    return "\n".join(lines)


def render_inventory(main_modules, gift_modules, gift_text_edges=(),
                     braille_text_edges=(), ram_ops=()):
    # Payload totals (bytecode + typed static-data segments) match the
    # authoritative pins; bytecode_bytes is the segment-kind split.
    main_bc = sum(m.payload_bytes for m in main_modules)
    gift_bc = sum(m.payload_bytes for m in gift_modules)
    lines = [
        "# Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# R13-G2 script-module family inventory. 523 modules = 468 map +",
        "# 47 common + 8 mystery-gift; bytecode 207,330 B = 206,638 main +",
        "# 692 gift (authoritative qualified pin, plan §18.1). Relocation",
        "# graph 16,704 with G1 class partition; 95 root / 8,113 interior.",
        "",
        "inventory_version = 1",
        "family = \"script\"",
        "game = \"emerald\"",
        "rom_profile = \"bpee01\"",
        "",
        "[[families]]",
        "kind = \"script-module\"",
        f"module_count = {len(main_modules) + len(gift_modules)}",
        f"map_modules = {sum(1 for m in main_modules if m.kind == 'map')}",
        f"common_modules = {sum(1 for m in main_modules if m.kind == 'common')}",
        f"mystery_gift_modules = {len(gift_modules)}",
        "resource_type = \"structured-data\"",
        "schema = 45",
        "gift_schema = 46",
        f"bytecode_bytes = {main_bc + gift_bc}",
        f"main_bytecode_bytes = {main_bc}",
        f"mystery_gift_bytecode_bytes = {gift_bc}",
        f"mystery_gift_text_edges = {len(gift_text_edges)}",
        f"mystery_gift_text_labels = {len({e['target_label'] for e in gift_text_edges})}",
        f"mystery_gift_text_bytes = {GIFT_TEXT_BYTES_PIN}",
        f"braille_text_edges = {len(braille_text_edges)}",
        f"braille_text_labels = {len({e['target_label'] for e in braille_text_edges})}",
        f"braille_text_bytes = {BRAILLE_TEXT_BYTES_PIN}",
        "",
    ]
    for op in sorted(ram_ops, key=lambda o: o["target_gba"]):
        region = ("ewram" if 0x02000000 <= op["target_gba"] < 0x02040000
                  else "iwram")
        lines += [
            "[[ram_targets]]",
            f"gba_address = {op['target_gba']:#x}",
            f"region = \"{region}\"",
            f"source_gba_offset = {op['source_gba_offset']:#x}",
            "",
        ]
    return "\n".join(lines)


def render_bindings(modules):
    lines = [
        "# Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Semantic extraction bindings for the script family. The artifact",
        "# IS the canonical payload (raw encoding); sizes are derived and",
        "# proven by gen_script_family.py against the qualified ROM before",
        "# the manifest pipeline re-proves them. NO ROM offsets here: the",
        "# meta sidecars carry the segment provenance.",
        "",
        "bindings_version = 1",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        "",
    ]
    for m in sorted(modules, key=lambda m: m.id):
        lines += [
            "[[bindings]]",
            f"id = \"{m.id}\"",
            f"symbol = \"{m.primary_symbol}\"",
            f"source_artifact = \"resources/extraction/emerald/bpee01/script/modules/data/{m.kind}/{m.name.lower()}.bin\"",
            "source_encoding = \"raw\"",
            "canonical_representation = \"gba-bytes\"",
            f"expected_decoded_size = {m.payload_bytes}",
            "",
        ]
    return "\n".join(lines)


def render_catalog(modules):
    lines = [
        "# Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "catalog_version = 1",
        "namespace = \"emerald\"",
        "resource_api = \"1.0.0\"",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        "",
    ]
    for m in sorted(modules, key=lambda m: m.id):
        lines += [
            "[[resources]]",
            f"id = \"{m.id}\"",
            "type = \"structured-data\"",
            f"schema = {m.schema}",
            "required_for_base = true",
            "",
        ]
    return "\n".join(lines)


def render_ownership(modules):
    lines = [
        "# Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Native asset-ownership declaration. Each [[resources]] block",
        "# records the canonical id, key (artifact sha256 - raw encoding,",
        "# so encoded == decoded), legacy compiled symbol, source artifact,",
        "# per-target ownership state and source hashes.",
        "# R13-G2 ownership: every payload is COMPILED_PENDING_MIGRATION on",
        "# native - the legacy compiled field scripts stay in place and no",
        "# consumer is redirected (additive-only; the G3 resolver seam",
        "# flips these later). The gba target keeps the qualified ROM bytes",
        "# as the canonical representation.",
        "",
        "ownership_version = 1",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        "",
    ]
    for m in sorted(modules, key=lambda m: m.id):
        digest = sha256hex(m.payload)
        lines += [
            "[[resources]]",
            f"id = \"{m.id}\"",
            f"key = \"{digest}\"",
            f"legacy_symbol = \"{m.primary_symbol}\"",
            "type = \"structured-data\"",
            f"source_artifact = \"resources/extraction/emerald/bpee01/script/modules/data/{m.kind}/{m.name.lower()}.bin\"",
            f"encoded_length = {m.payload_bytes}",
            f"decoded_length = {m.payload_bytes}",
            "source_encoding = \"raw\"",
            f"source_encoded_sha256 = \"{digest}\"",
            f"canonical_decoded_sha256 = \"{digest}\"",
            "ownership_state = \"COMPILED_PENDING_MIGRATION\"",
            "",
            "[resources.targets]",
            "native = \"COMPILED_PENDING_MIGRATION\"",
            "gba = \"COMPILED\"",
            "",
        ]
    return "\n".join(lines)


def render_consumers():
    sites = [
        ("map-header", "MapHeader.mapScripts", "map dispatch entry",
         "R13-F repoints map script ownership in the host build; the module"
         " keeps the semantic identity until the G3 resolver seam"),
        ("object-event", "ObjectEventTemplate.script", "per-object script pointer",
         "operand address lives inside map event tables; redirect requires the map/script pointer graph"),
        ("coord-event", "CoordEvent.script", "coord event script pointer",
         "operand address lives inside map event tables; redirect requires the map/script pointer graph"),
        ("bg-event", "BgEvent.script", "bg event script pointer",
         "operand address lives inside map event tables; redirect requires the map/script pointer graph"),
        ("gba-constant", "gStdScripts", "11 engine routing bindings",
         "G routing table entries consumed by script commands; shadow-only in G2"),
        ("bytecode-operand", "script_data R_ARM_ABS32 operands", "16,704 address operands",
         "operands stay byte-exact; the G3 resolver reads them through the typed relocation index"),
        ("c-text", "deferred-message sites (message/messageautoscroll)",
         "TEXT_TARGET operands into the R13-C catalog",
         "text bundles are C-owned; G2 relocation metadata names the bundle keys"),
        ("movement", "applymovement/applymovementat operands", "MOVEMENT_TARGET operands",
         "movement leaves are R13-B resources; G2 metadata names their ids"),
    ]
    lines = [
        "# Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Machine-readable consumer map for the script family. Every",
        "# consumer of script-module bytes is an address operand inside the",
        "# qualified graph or a host-build pointer into the compiled",
        "# legacy arrays. R13-G2 redirects nothing: decision = deferred for",
        "# every site until the G3 resolver seam publishes typed targets.",
        "",
        "consumers_version = 1",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        "",
    ]
    for kind, site, consumer, reason in sites:
        lines += [
            "[[consumer_sites]]",
            f"kind = \"{kind}\"",
            f"site = \"{site}\"",
            f"consumer = \"{consumer}\"",
            "decision = \"deferred\"",
            f"reason = \"{reason}\"",
            "",
        ]
    return "\n".join(lines)


def render_manifest(rom, modules):
    rom_sha1 = hashlib.sha1(rom).hexdigest()
    rom_sha256 = hashlib.sha256(rom).hexdigest()
    lines = [
        "# Deterministic extraction manifest - generated by",
        "# tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; regenerate with gen_script_family.py --check.",
        "# Records are sorted bytewise by canonical resource id.",
        "# provenance: qualified pret reference build ELF + retail-matching",
        "# ROM; BPEE01 Rev 0 (SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7)",
        f"rom_sha1 = \"{rom_sha1}\"",
        f"rom_sha256 = \"{rom_sha256}\"",
        "manifest_version = 1",
        "namespace = \"emerald\"",
        "resource_api = \"1.0.0\"",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        "qualification = \"production\"",
        f"rom_size = {len(rom)}",
        "",
    ]
    for m in sorted(modules, key=lambda m: m.id):
        if not m.payload:
            # Routing-only modules (plan §5: terminator-only `.byte 0`
            # map-script tables) carry no G payload: their region holds
            # routing-class bytes only, which stay ROM-resident. The pack
            # model rejects encoded_length == 0 records, so the manifest
            # embeds the payload-bearing modules only; the routing-only
            # identities stay catalog/ownership/meta surface (f_inbound
            # bindings reference them with boundary_kind = "routing").
            continue
        digest = sha256hex(m.payload)
        lines += [
            "[[records]]",
            f"id = \"{m.id}\"",
            f"key = \"{m.key}\"",
            "type = \"structured-data\"",
            f"schema = {m.schema}",
            "bundle = true",
            f"source_artifact = \"resources/extraction/emerald/bpee01/script/modules/data/{m.kind}/{m.name.lower()}.bin\"",
            f"symbol = \"{m.primary_symbol}\"",
            "rom_offset = 0",
            f"encoded_length = {m.payload_bytes}",
            f"decoded_length = {m.payload_bytes}",
            "source_encoding = \"raw\"",
            f"source_encoded_sha256 = \"{digest}\"",
            f"canonical_decoded_sha256 = \"{digest}\"",
            "",
        ]
    return "\n".join(lines)


def render_routing(routing_relocs):
    lines = [
        "# Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Relocations whose sources live inside the 5,749-byte map",
        "# dispatch/conditional routing class. These become the relocation",
        "# sidecars of the typed G routing segment resources (G2 routing",
        "# sub-stage); they are part of the 16,704 canonical operand pin",
        "# but never module payload.",
        "",
        "routing_version = 1",
        f"total = {len(routing_relocs)}",
        "",
    ]
    for r in sorted(routing_relocs, key=lambda r: r["source_gba_offset"]):
        lines += [
            "[[relocs]]",
            f"source_gba_offset = {r['source_gba_offset']:#x}",
            f"source_table = \"{r['source_table']}\"",
            "operand_width = 4",
            f"target_class = \"{r['target_class']}\"",
            f"target_gba = {r['target_gba']:#x}",
            f"target_resource_key = \"{r['target_resource_key']}\"",
        ]
        if r.get("target_export"):
            lines.append(f"target_export = \"{r['target_export']}\"")
        if r.get("target_offset") is not None:
            lines.append(f"target_offset = {r['target_offset']}")
        lines.append("")
    return "\n".join(lines)


def resolve_entrypoint(kind, ref, by_start, routing, move_class_bytes,
                       names_by_addr):
    """Resolve one F-inbound/gStdScripts script pointer to its module
    entrypoint.  Fail-closed (brief §7/§8): every target must be a
    shipped module's G-byte export or a routing-class boundary; the
    boundary kind is part of the binding record."""
    target = ref["gba_target"]
    tmod = find_module(by_start, target)
    if tmod is None:
        fail(f"{kind} target {target:#x} ({ref.get('map_symbol', '?')}) "
             f"has no shipped module span")
    if target in tmod.gbytes:
        ex = next((e for e in tmod.exports
                   if e["original_gba_address"] == target), None)
        if ex is None:
            fail(f"{kind} target {target:#x}: G byte in {tmod.id} "
                 f"with no export")
        off = target - tmod.region_start
        # Region-relative boundary, matching G1's interior proof
        # (offset 0 = module root export; the module's payload boundary
        # kind is a separate model in the meta records).
        return tmod, ex, off, "offset-zero" if off == 0 else "interior"
    if target in routing:
        # Map-script dispatch table bytes (including terminator-only
        # `.byte 0` tables): routing-owned, valid entrypoints per G1's
        # target-class rule (every MapHeader.mapScripts pointer lands
        # here). The label is the module's own *_MapScripts symbol.
        label = next((n for n in names_at(target, names_by_addr)), None)
        if label is None:
            fail(f"{kind} target {target:#x} in {tmod.id}: routing byte "
                 f"with no symbol")
        return tmod, dict(name=label), target - tmod.region_start, "routing"
    if target in move_class_bytes:
        # The three named movement bridges (plan §5): B-owned bytes, not
        # G resources; recorded as valid boundaries only if labeled.
        label = next((n for n in names_at(target, names_by_addr)), None)
        if label is None:
            fail(f"{kind} target {target:#x} in {tmod.id}: movement byte "
                 f"with no symbol")
        return tmod, dict(name=label), target - tmod.region_start, "movement"
    fail(f"{kind} target {target:#x} in {tmod.id}: not G bytecode, "
         f"routing, or movement")


def _reloc_source_gba(m, r):
    """GBA address of a module reloc's operand source, mapped back from
    its payload offset through the module's deterministic segments."""
    seg = next(s for s in m.segments
               if s["payload_offset"] <= r["operand_payload_offset"]
               < s["payload_offset"] + s["byte_count"])
    return seg["original_gba_start"] + (r["operand_payload_offset"]
                                        - seg["payload_offset"])


def render_movement_edges(modules, routing_relocs, base):
    lines = [
        "# Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# R13-G2 (plan §5): every MOVEMENT_TARGET operand edge resolves to",
        "# R13-B ownership (movement/bindings.generated.toml resource keys).",
        "# The three recomp-local movement cases are explicit named compiled",
        "# bridges: 12 B total (7+2+3), B leaf-seam identity, not resources.",
        "",
        f"movement_edges = {MOVEMENT_EDGES_PIN}",
        f"bridges = {len(RECOMP_LOCAL_MOVEMENT)}",
        f"bridge_bytes = {MOVEMENT_BRIDGE_BYTES_PIN}",
        "",
    ]
    by_start = sorted(modules, key=lambda m: m.region_start)
    edges = []
    for m in modules:
        for r in m.relocs:
            if r["target_class"] != "MOVEMENT_TARGET":
                continue
            edges.append(dict(src_gba=_reloc_source_gba(m, r), reloc=r))
    for r in routing_relocs:
        if r["target_class"] == "MOVEMENT_TARGET":
            edges.append(dict(src_gba=r["source_gba_offset"], reloc=r))
    if len(edges) != MOVEMENT_EDGES_PIN:
        fail(f"MOVEMENT_TARGET edges {len(edges)} != {MOVEMENT_EDGES_PIN}")
    if any(not e["reloc"]["target_resource_key"] for e in edges):
        fail("a MOVEMENT_TARGET edge lacks a B resource key")
    for e in sorted(edges, key=lambda e: e["src_gba"]):
        r = e["reloc"]
        lines += [
            "[[edges]]",
            f"source_gba_offset = {e['src_gba']:#x}",
            f"target_gba = {r['original_encoded_gba']:#x}",
            f"target_resource_key = \"{r['target_resource_key']}\"",
            f"target_export = \"{r['target_export']}\"",
            "",
        ]
    for name, rel, size in RECOMP_LOCAL_MOVEMENT:
        gba = base + rel
        hits = [(m, r) for m in modules for r in m.relocs
                if r["original_encoded_gba"] == gba
                and r["target_class"] == "SCRIPT_TARGET"
                and r["target_resource_key"]
                == f"emerald:movement/bridge/{slugify(name)}"]
        if len(hits) != 1:
            fail(f"movement bridge {name}@{gba:#x}: {len(hits)} operands, "
                 f"expected 1")
        m, r = hits[0]
        tmod = find_module(by_start, gba)
        if tmod is None:
            fail(f"movement bridge {name}@{gba:#x} has no module span")
        lines += [
            "[[bridges]]",
            f"name = \"{name}\"",
            f"gba_address = {gba:#x}",
            f"byte_count = {size}",
            f"module_key = \"{tmod.id}\"",
            f"module_offset = {gba - tmod.region_start}",
            f"source_gba_offset = {_reloc_source_gba(m, r):#x}",
            f"target_resource_key = \"{r['target_resource_key']}\"",
            "",
        ]
    return "\n".join(lines)


def render_stdscripts(std, rom, base, by_start, routing, move_class_bytes,
                      names_by_addr):
    lines = [
        "# Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# R13-G2 (plan §7): gStdScripts shadow binding records.  The 11",
        "# slot pointers occupy the 44-byte span [gScriptCmdTable+0xC24,",
        "# +0xC50) and remain provenance in the field bytecode region; these",
        "# records are metadata only (status = \"shadow\" - no live",
        "# publication; that is phase-2/R13-H).",
        "",
        f"gstdscripts = {STDSCRIPTS_PIN}",
        f"slot_span_start = {base + STD_SLOT_OFFSET:#x}",
        f"slot_span_end = {base + STD_SLOT_OFFSET + 4 * STDSCRIPTS_PIN:#x}",
        "",
    ]
    rows = sorted(std, key=lambda r: r[0])
    got = [r[0] for r in rows]
    expected = [base + STD_SLOT_OFFSET + 4 * i
                for i in range(STDSCRIPTS_PIN)]
    if got != expected:
        fail(f"gStdScripts slot span {[hex(a) for a in got]} "
             f"!= {[hex(a) for a in expected]}")
    split = {"offset-zero": 0, "interior": 0, "routing": 0}
    for i, (off, typ, sym) in enumerate(rows):
        target = struct.unpack_from("<I", rom, off - GEN3_GBA_ROM_BASE)[0]
        tmod, ex, moff, boundary = resolve_entrypoint(
            "gStdScripts", dict(gba_target=target), by_start, routing,
            move_class_bytes, names_by_addr)
        split[boundary] += 1
        lines += [
            "[[records]]",
            f"slot = {i}",
            f"rom_offset = {off:#x}",
            f"encoded_gba = {target:#x}",
            f"module_key = \"{tmod.id}\"",
            f"export = \"{ex['name']}\"",
            f"module_offset = {moff}",
            f"boundary_kind = \"{boundary}\"",
            "status = \"shadow\"",
            "",
        ]
    got = (split["offset-zero"], split["interior"], split["routing"])
    if got != STD_BOUNDARY_SPLIT:
        fail(f"gStdScripts boundary split {got} != {STD_BOUNDARY_SPLIT}")
    return "\n".join(lines)


def resolve_same_map(kind, r, tmod):
    """True when the F reference lands in its own map's script module.

    Cross-map refs are legitimate and the resolved module key is the
    binding; same_map is informational (documented in the header).
    """
    if kind == "map-scripts":
        mid = r["map_key"]
        if not (mid.startswith("emerald:data/map/")
                and mid.endswith("/header")):
            fail(f"unexpected map-scripts manifest id {mid}")
        key = mid[len("emerald:data/map/"):-len("/header")]
        return tmod.id == f"emerald:script/map/{key}"
    key = r["map_name"].lower().replace("_", "-")
    return tmod.id == f"emerald:script/map/{key}"


def render_f_inbound(map_script_refs, object_refs, coord_refs, bg_refs,
                     by_start, routing, move_class_bytes, names_by_addr):
    lines = [
        "# Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# R13-G2 (plan §8): F inbound binding metadata.  Every",
        "# MapHeader.mapScripts / ObjectEventTemplate.script /",
        "# CoordEvent.script / BgEvent.script pointer resolves to a shipped",
        "# script module key, its exported label, the module-relative",
        "# offset, and a valid ENTRYPOINT boundary (offset-zero = module",
        "# root export; interior = G bytecode instruction boundary;",
        "# routing = map-script dispatch table byte, the kind every",
        "# MapHeader.mapScripts pointer lands on).",
        "",
        "f_inbound_version = 1",
        "",
    ]
    total = 0
    same_map_maps = 0
    for kind, refs in (("map-scripts", map_script_refs),
                       ("object-event", object_refs),
                       ("coord-event", coord_refs),
                       ("bg-event", bg_refs)):
        n = len(refs)
        if n != F_INBOUND_PINS[kind]:
            fail(f"F inbound {kind} refs {n} != {F_INBOUND_PINS[kind]}")
        rows = [resolve_entrypoint(kind, r, by_start, routing,
                                   move_class_bytes, names_by_addr) + (r,)
                for r in refs]
        split = {"offset-zero": 0, "interior": 0, "routing": 0}
        for tmod, ex, off, boundary, r in rows:
            split[boundary] += 1
        got = (split["offset-zero"], split["interior"], split["routing"])
        if got != F_INBOUND_BOUNDARY_SPLIT[kind]:
            fail(f"F inbound {kind} boundary split {got} "
                 f"!= {F_INBOUND_BOUNDARY_SPLIT[kind]}")
        if kind == "map-scripts" and any(off != 0
                                         for tmod, ex, off, boundary, r
                                         in rows):
            fail("a map-scripts entrypoint is not at module offset 0 "
                 "(dispatch table start)")
        kind_same = 0
        for tmod, ex, off, boundary, r in rows:
            if resolve_same_map(kind, r, tmod):
                kind_same += 1
        if kind_same != F_INBOUND_SAME_MAP_SPLIT[kind]:
            fail(f"F inbound {kind} same_map {kind_same} "
                 f"!= {F_INBOUND_SAME_MAP_SPLIT[kind]}")
        for tmod, ex, off, boundary, r in sorted(
                rows, key=lambda t: (t[0].id, t[4]["gba_target"])):
            key = (r["map_key"][len("emerald:data/map/"):-len("/header")]
                   if kind == "map-scripts"
                   else r["map_name"].lower().replace("_", "-"))
            same_map = resolve_same_map(kind, r, tmod)
            if same_map:
                same_map_maps += 1
            map_symbol = (r["map_symbol"] if kind == "map-scripts"
                          else r["map_name"])
            lines += [
                "[[references]]",
                f"kind = \"{kind}\"",
                f"map_symbol = \"{map_symbol}\"",
                f"map_key = \"{key}\"",
                f"gba_target = {r['gba_target']:#x}",
                f"module_key = \"{tmod.id}\"",
                f"export = \"{ex['name']}\"",
                f"module_offset = {off}",
                f"boundary_kind = \"{boundary}\"",
                f"same_map = {'true' if same_map else 'false'}",
                "",
            ]
        total += n
    # The 3,501 F-table pointers, one entrypoint each, zero unresolved.
    lines[12:12] = [
        f"references = {total}",
        "unresolved = 0",
        f"same_map = {same_map_maps}",
        "",
    ]
    return "\n".join(lines)


def render_mart_tables(modules):
    tables = []
    for m in sorted(modules, key=lambda m: m.region_start):
        for s in m.segments:
            if s.get("kind") != "static-data":
                continue
            ex = next((e for e in m.exports
                       if e["original_gba_address"] == s["original_gba_start"]),
                      None)
            label = ex["name"] if ex else f"table_0x{s['original_gba_start']:X}"
            tables.append(dict(
                label=label,
                gba_address=s["original_gba_start"],
                module_key=m.id,
                module_offset=s["payload_offset"],
                byte_count=s["byte_count"],
                item_count=(s["byte_count"] - 2) // 2,
            ))
    lines = [
        "# Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# R13-G2 (plan §7.5): mart / decoration shop tables are typed",
        "# G module data segments (data_kind = \"mart-table\"), each a u16",
        "# item list validated to end in the ITEM_NONE sentinel (0x0000).",
        "",
        f"mart_tables = {len(tables)}",
        f"mart_table_bytes = {sum(t['byte_count'] for t in tables)}",
        "sentinel = \"ITEM_NONE (0x0000)\"",
        "",
    ]
    for t in sorted(tables, key=lambda t: t["gba_address"]):
        lines += [
            "[[tables]]",
            f"label = \"{t['label']}\"",
            f"gba_address = {t['gba_address']:#x}",
            f"module_key = \"{t['module_key']}\"",
            f"module_offset = {t['module_offset']}",
            f"byte_count = {t['byte_count']}",
            f"item_count = {t['item_count']}",
            "sentinel = \"valid\"",
            "",
        ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", default=".")
    ap.add_argument("elf", nargs="?", default="../pokeemerald-reference/pokeemerald.elf")
    ap.add_argument("rom", nargs="?", default="../pokeemerald-reference/pokeemerald.gba")
    ap.add_argument("outdir", nargs="?", default="resources/extraction/emerald/bpee01")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    root = Path(args.root)
    outdir = root / Path(args.outdir)
    reference_root = root.resolve().parent / "pokeemerald-reference"

    print("Loading ROM...")
    rom = open(args.rom, "rb").read()
    if len(rom) != fe.ROM_SIZE:
        fail(f"ROM size {len(rom)} != 16 MiB")
    if hashlib.sha1(rom).hexdigest() != ROM_SHA1:
        fail(f"ROM SHA-1 != {ROM_SHA1}")

    print("Loading ELF...")
    elf = fe.Elf32(open(args.elf, "rb").read())
    main_sym = elf.find("gScriptCmdTable")
    if not main_sym:
        fail("missing gScriptCmdTable")
    base = main_sym["value"]
    main_end = base + MAIN_OBJECT_BYTES

    print("Loading grammar + qualified graph (G1 oracle)...")
    opcode_table, trainer_types = fe.load_grammar()
    operands, walked, *graph_state = fe.collect_qualified_graph(root, rom, elf, opcode_table, trainer_types)
    if len(operands) != OPERANDS_PIN:
        fail(f"graph operands {len(operands)} != {OPERANDS_PIN}")
    # Task #4 keeps the F-inbound and gStdScripts provenance sets from the
    # G1 oracle; the walk artifacts are not needed by the resource emission.
    (map_tables, conditional_tables, dispatch_rows, cond_rows, total_rows,
     map_leaf_targets, object_refs, coord_refs, bg_refs, engine_refs,
     std, mg_excluded) = graph_state
    del graph_state, map_tables, conditional_tables, dispatch_rows, cond_rows
    del total_rows, map_leaf_targets, engine_refs, mg_excluded
    print(f"  rss {rss_mb()} MB")

    # R13-G2 (plan §8): MapHeader.mapScripts provenance.  The gameplay
    # family owns the 518 28-byte per-map header resources; the script
    # pointer sits at wire offset 8 of each row.
    game_manifest = outdir / "gameplay" / "manifest.production.toml"
    try:
        gd = tomllib.loads(game_manifest.read_text())
    except OSError:
        fail(f"gameplay manifest missing at {game_manifest}: run "
             f"gen_gameplay_family.py before the script family")
    map_script_refs = []
    for rec in gd.get("records", []):
        rid = rec["id"]
        if not (rid.startswith("emerald:data/map/") and rid.endswith("/header")):
            continue
        row = rom[rec["rom_offset"]:rec["rom_offset"] + 28]
        scptr = struct.unpack_from("<I", row, 8)[0]
        if scptr == 0:
            fail(f"map header {rid} has a null mapScripts pointer")
        map_script_refs.append(dict(map_key=rid, map_symbol=rec["symbol"],
                                    gba_target=scptr))
    if len(map_script_refs) != F_INBOUND_PINS["map-scripts"]:
        fail(f"map-scripts refs {len(map_script_refs)} "
             f"!= {F_INBOUND_PINS['map-scripts']} map headers")

    val_by_name = {}
    for s in elf.symbols:
        if s["value"] >= GEN3_GBA_ROM_BASE:
            val_by_name.setdefault(s["name"], s["value"])

    names_by_addr = {}
    for s in elf.symbols:
        if s["value"] >= GEN3_GBA_ROM_BASE and not s["name"].startswith(JUNK_PREFIXES):
            names_by_addr.setdefault(s["value"], []).append(s["name"])
    for a in names_by_addr:
        names_by_addr[a].sort()

    print("Building text/movement/routing class sets...")
    phase_rss = rss_mb()
    text_starts = fe.generated_target_starts(root, elf, "text")
    print(f"    text_starts {len(text_starts)} labels, rss {rss_mb()} MB")
    for s in elf.symbols:
        n = s["name"]
        if "_Text_" in n or n.startswith(("Text_", "gText_", "sText_")):
            text_starts.add(s["value"])
    text_files = sorted((reference_root / "data/text").glob("*.inc"))
    if len(text_files) != TEXT_FILES_PIN:
        fail(f"data/text files {len(text_files)} != {TEXT_FILES_PIN}")
    for t in text_files:
        _, addrs = parse_file_labels(t, val_by_name)
        text_starts.update(addrs)
    print(f"    full text_starts {len(text_starts)} labels, rss {rss_mb()} MB")
    text_class_bytes = build_class_spans(elf, base, main_end, text_starts)
    print(f"    text_class_bytes {len(text_class_bytes)} bytes, rss {rss_mb()} MB")
    if len(text_class_bytes) != TEXT_BYTES_PIN:
        fail(f"text class bytes {len(text_class_bytes)} != {TEXT_BYTES_PIN}")

    move_starts = fe.generated_target_starts(root, elf, "movement")
    extra_movement = []
    for name, rel, size in RECOMP_LOCAL_MOVEMENT:
        a = val_by_name.get(name)
        if a is None:
            fail(f"recomp-local movement {name} missing from qualified ELF")
        if a != base + rel:
            fail(f"recomp-local movement {name} at {a:#x}, expected {base + rel:#x}")
        extra_movement.append((a, size))
    move_class_bytes = build_class_spans(elf, base, main_end, move_starts, extra_movement)
    if len(move_class_bytes) != MOVEMENT_BYTES_PIN:
        fail(f"movement class bytes {len(move_class_bytes)} != {MOVEMENT_BYTES_PIN}")

    routing, routing_source_tables = build_routing_set(rom, elf, base)
    engine = set(range(base, base + ENGINE_PREFIX_BYTES))
    print(f"  rss {rss_mb()} MB (class sets: +{rss_mb() - phase_rss} MB)")

    print("Building module regions...")
    phase_rss = rss_mb()
    modules = build_regions(elf, reference_root, base, main_end, val_by_name)

    # Gift modules: 8 roots over [gift_first, gift_first + 0xD2E). The
    # object is 3,374 B = 692 B gift bytecode + 2,682 B gift text (the
    # 20 sText_ label spans, handed to C in Task #3); bytecode interleaves
    # with the window texts and includes the five later modules' scripts.
    gift_first = min(s["value"] for s in elf.symbols
                     if s["name"].startswith("MysteryGiftScript_"))
    gift_text_class = build_gift_text_class(elf, gift_first,
                                            gift_first + GIFT_OBJECT_BYTES)
    gift_modules = build_gift_modules(elf, reference_root, val_by_name,
                                      gift_first, gift_first + GIFT_OBJECT_BYTES)
    all_modules = modules + gift_modules

    print("Classifying per-module G bytes / segments / exports...")
    phase_rss = rss_mb()
    # Mart/decor table starts are the MART_TABLE_TARGET operand targets
    # that land in G bytes (plan §7.5: "typed G module data segment;
    # sentinel validated"). Routing-class targets are conditional
    # dispatch table starts; text-class targets are the braille strings
    # (C handoff). A name heuristic cannot be trusted: Common_EventScript_
    # ShowPokemartSign contains "Pokemart" but is a script, not a table.
    mart_targets = sorted({
        op["target_gba"]
        for op in operands
        if op["target_class"] == "MART_TABLE_TARGET"
        and op["target_gba"] not in routing
        and op["target_gba"] not in text_class_bytes
        and op["target_gba"] not in gift_text_class
    })
    # Braille strings are text-class bytes without a C catalog record
    # (plan §7.5: text portion stays C). Their spans [label, next
    # symbol) are the C-handoff extent, pinned like the gift texts.
    braille_labels = sorted({
        op["target_gba"]
        for op in operands
        if op["target_class"] == "MART_TABLE_TARGET"
        and op["target_gba"] in text_class_bytes
    })
    if len(braille_labels) != BRAILLE_TEXT_LABELS_PIN:
        fail(f"braille labels {len(braille_labels)} "
             f"!= {BRAILLE_TEXT_LABELS_PIN}")
    all_sym_addrs = sorted(names_by_addr)
    braille_span = 0
    for a in braille_labels:
        idx = bisect.bisect_right(all_sym_addrs, a)
        nxt = all_sym_addrs[idx] if idx < len(all_sym_addrs) else a
        braille_span += nxt - a
    if braille_span != BRAILLE_TEXT_BYTES_PIN:
        fail(f"braille text bytes {braille_span} != {BRAILLE_TEXT_BYTES_PIN}")
    del all_sym_addrs, braille_labels

    kept = []
    for m in all_modules:
        gb = {a for a in range(m.region_start, m.region_end)
              if a not in text_class_bytes and a not in move_class_bytes
              and a not in routing and a not in engine
              and a not in gift_text_class}
        routing_in_region = {a for a in range(m.region_start, m.region_end)
                             if a in routing}
        if not gb and not routing_in_region and m.kind != "mystery-gift":
            # Module region holds no G or routing bytes: the movement-only
            # include (R13-B) is not a script module; everything else with a
            # surviving label keeps its identity even with an empty payload
            # (terminator-only map tables are routing-only modules). Gift
            # modules are kept unconditionally (schema-46 identity is part
            # of the 523-module pin even when a region holds only text).
            continue
        # runs of G bytes, cut at mart/decor table starts; each table is
        # a u16 item list ending in the ITEM_NONE sentinel (0x0000), and
        # its extent is validated here (fail if no sentinel before the
        # run end). Everything after the sentinel is field bytecode.
        runs = []
        start = None
        for a in range(m.region_start, m.region_end):
            in_g = a in gb
            if in_g and start is None:
                start = a
            elif not in_g and start is not None:
                runs.append([start, a, "bytecode"]); start = None
        if start is not None:
            runs.append([start, m.region_end, "bytecode"])
        out_runs = []
        for s0, e0, k in runs:
            cuts = sorted(x for x in mart_targets if s0 <= x < e0)
            prev = s0
            for c in cuts:
                if c > prev:
                    out_runs.append((prev, c, "bytecode"))
                t = c
                while t + 2 <= e0:
                    item = struct.unpack_from("<H", rom,
                                              t - GEN3_GBA_ROM_BASE)[0]
                    if item == 0:
                        break
                    t += 2
                end = t + 2
                if end > e0:
                    fail(f"mart table at {c:#x} (module {m.name}) has no "
                         f"ITEM_NONE sentinel before run end {e0:#x}")
                out_runs.append((c, end, "static-data"))
                prev = end
            if prev < e0:
                out_runs.append((prev, e0, "bytecode"))
        m.gbytes = gb
        m.g_label_addrs = sorted(a for a in gb if a in names_by_addr)
        off = 0
        for s0, e0, k in out_runs:
            seg = dict(kind=k, original_gba_start=s0, byte_count=e0 - s0,
                       payload_offset=off)
            if k == "static-data":
                seg["data_kind"] = "mart-table"
            m.segments.append(seg)
            if k == "static-data":
                m.static_data_bytes += e0 - s0
            else:
                m.bytecode_bytes += e0 - s0
            off += e0 - s0
        m.payload = b"".join(rom[s0 - GEN3_GBA_ROM_BASE:e0 - GEN3_GBA_ROM_BASE]
                             for s0, e0, _ in out_runs)
        # exports: every G label in the region
        for a in m.g_label_addrs:
            names = names_at(a, names_by_addr)
            if not names:
                continue
            primary = next((n for n in names if n in m.file_label_names), None)
            if primary is None:
                primary = sorted(names)[0]
            aliases = [n for n in sorted(names) if n != primary]
            seg = next(s for s in m.segments
                       if s["original_gba_start"] <= a < s["original_gba_start"] + s["byte_count"])
            poff = seg["payload_offset"] + (a - seg["original_gba_start"])
            m.exports.append(dict(
                name=primary, original_gba_address=a,
                provenance_offset=a - GEN3_GBA_ROM_BASE, payload_offset=poff,
                boundary_kind="offset-zero" if poff == 0 else "interior",
                aliases=aliases))
        if m.exports:
            m.primary_symbol = m.exports[0]["name"]
            m.exports.sort(key=lambda e: e["payload_offset"])
        kept.append(m)
    modules = kept

    if len(modules) != MODULES_PIN:
        fail(f"modules {len(modules)} != {MODULES_PIN}")
    n_map = sum(1 for m in modules if m.kind == "map")
    n_common = sum(1 for m in modules if m.kind == "common")
    n_gift = sum(1 for m in modules if m.kind == "mystery-gift")
    if (n_map, n_common, n_gift) != (MAP_MODULES_PIN, COMMON_MODULES_PIN, GIFT_MODULES_PIN):
        fail(f"module split {n_map}/{n_common}/{n_gift} != "
             f"{MAP_MODULES_PIN}/{COMMON_MODULES_PIN}/{GIFT_MODULES_PIN}")
    main_bc = sum(m.bytecode_bytes + m.static_data_bytes for m in modules if m.kind != "mystery-gift")
    gift_bc = sum(m.bytecode_bytes + m.static_data_bytes for m in modules if m.kind == "mystery-gift")
    if (main_bc, gift_bc) != (MAIN_BYTECODE_PIN, GIFT_BYTECODE_PIN):
        fail(f"bytecode {main_bc}/{gift_bc} != {MAIN_BYTECODE_PIN}/{GIFT_BYTECODE_PIN}")
    mart_segs = [s for m in modules for s in m.segments
                 if s.get("kind") == "static-data"]
    if (len(mart_segs), sum(s["byte_count"] for s in mart_segs)) != \
            (MART_TABLES_PIN, MART_TABLE_BYTES_PIN):
        fail(f"mart tables {len(mart_segs)} / "
             f"{sum(s['byte_count'] for s in mart_segs)} B != "
             f"{MART_TABLES_PIN} / {MART_TABLE_BYTES_PIN} B")
    print(f"  rss {rss_mb()} MB (modules: +{rss_mb() - phase_rss} MB)")
    phase_rss = rss_mb()

    # ---- target resolution maps ------------------------------------
    print("Loading C text bundles + B movement bindings for target resolution...")
    bundle_of = {}
    bundles_path = root / "resources/extraction/emerald/bpee01/text/bundles.generated.toml"
    doc = tomllib.loads(bundles_path.read_text())
    for b in doc.get("bundles", []):
        for lab in b.get("labels", []):
            bundle_of[lab["symbol"]] = b["id"]
    move_id_of = {}
    mov_path = root / "resources/extraction/emerald/bpee01/movement/bindings.generated.toml"
    for b in tomllib.loads(mov_path.read_text()).get("bindings", []):
        move_id_of[b["symbol"]] = b["id"]

    # name -> set of ids per address (for reverse resolution)
    text_alias_hits = []
    def bundle_key_for_target(target):
        for n in names_at(target, names_by_addr):
            if n in bundle_of:
                return bundle_of[n], n
            # AC-3 renames: qualified label is newer than the recomp-built
            # C catalog; resolve through the text family's RENAME_MAP.
            alias = QUALIFIED_TO_RECOMP_TEXT.get(n)
            if alias and alias in bundle_of:
                text_alias_hits.append(n)
                return bundle_of[alias], n
        return None, None

    def movement_key_for_target(target):
        for n in names_at(target, names_by_addr):
            if n in move_id_of:
                return move_id_of[n], n
        return None, None

    def payload_offset_of(m, a):
        seg = next(s for s in m.segments
                   if s["original_gba_start"] <= a < s["original_gba_start"] + s["byte_count"])
        return seg["payload_offset"] + (a - seg["original_gba_start"])

    def resolve_target(cls, target):
        """Target side of a relocation: (resource_key, export, module offset).
        Returns ("", None, None) for RAM data (allowlisted, no resource)."""
        if target in gift_text_class:
            # Gift text handoff (plan §7.3, Task #3): the 20 sText_ labels
            # are not yet in C's catalog; G2 names the pending handoff key
            # emerald:text/mystery-gift/<slug> and census-counts the edges.
            label = next((n for n in names_at(target, names_by_addr)
                          if n.startswith("sText_")), None)
            if label is None:
                fail(f"gift text target {target:#x} has no sText_ label")
            return f"emerald:text/mystery-gift/{slugify(label)}", label, None
        if cls == "RAM_DATA_TARGET":
            return "", None, None
        if cls == "SCRIPT_TARGET":
            # G1 interior-proof convention (report §Interior proof): every
            # script target resolves to exactly one module span; the
            # binding offset is MODULE-RELATIVE (offset 0 = module root
            # export, nonzero = interior boundary). This holds for targets
            # whose bytes are G bytecode, routing class (conditional On*
            # tables), or the 3 recomp-local movement bridges alike.
            tmod = find_module(by_start, target)
            if tmod is None:
                fail(f"SCRIPT_TARGET {target:#x} has no module span")
            off = target - tmod.region_start
            if target in tmod.gbytes:
                ex = next((e for e in tmod.exports
                           if e["original_gba_address"] == target), None)
                return tmod.id, (ex["name"] if ex else None), off
            if target in routing:
                # Conditional dispatch table start (tag 2/4 row): bytes
                # belong to the typed routing segment (plan §6); the module
                # span still owns the script boundary per G1's proof.
                label = next((n for n in names_at(target, names_by_addr)), None)
                if label is None:
                    fail(f"routing-class SCRIPT_TARGET {target:#x} has no symbol")
                return tmod.id, label, off
            if target in movement_bridges:
                # The 3 recomp-local movement bridges (plan §5): B-owned
                # movement bytes with no resource identity (B's seam names
                # them explicitly, "not resources and are not listed").
                label = next((n for n in names_at(target, names_by_addr)), None)
                if label is None:
                    fail(f"movement bridge {target:#x} has no symbol")
                return f"emerald:movement/bridge/{slugify(label)}", label, off
            fail(f"SCRIPT_TARGET {target:#x} unresolved (no G byte, routing, or bridge)")
        if cls == "TEXT_TARGET":
            key, name = bundle_key_for_target(target)
            if key is None:
                fail(f"TEXT_TARGET {target:#x} unresolved to a C bundle")
            return key, name, None
        if cls == "MOVEMENT_TARGET":
            key, name = movement_key_for_target(target)
            if key is None:
                fail(f"MOVEMENT_TARGET {target:#x} unresolved to a B resource")
            return key, name, None
        if cls == "MART_TABLE_TARGET":
            tmod = find_module(by_start, target)
            if tmod is not None and target in tmod.gbytes:
                ex = next((e for e in tmod.exports
                           if e["original_gba_address"] == target), None)
                return tmod.id, (ex["name"] if ex else None), payload_offset_of(tmod, target)
            if target in routing:
                # 198 of the 262: the target is a conditional dispatch
                # table start (tag 2/4 row) inside the routing class; the
                # typed routing segment owns it (plan §6), not a module.
                tname = next((n for n in names_at(target, names_by_addr)), None)
                if tname is None:
                    tname = f"cond_0x{target:X}"
                return (f"emerald:script/routing/{slugify(tname)}", tname, None)
            if target in text_class_bytes:
                # Braille format text (16 labels; plan §7.5 "text portion
                # remains C"): C-owned text-class bytes with no braille.inc
                # record in C's catalog; G2 names the pending per-label key
                # (the same handoff category as the gift texts, Task #3).
                label = next((n for n in names_at(target, names_by_addr)), None)
                if label is None:
                    fail(f"text-class MART_TABLE_TARGET {target:#x} has no symbol")
                return f"emerald:text/braille/{slugify(label)}", label, None
            key, name = bundle_key_for_target(target)
            if key is None:
                fail(f"MART_TABLE_TARGET {target:#x} unresolved "
                     f"(no module G byte, no routing table, no C bundle)")
            return key, name, None
        fail(f"unhandled target class {cls}")

    print("Attributing and resolving 16,704 relocations...")
    by_start = sorted(all_modules, key=lambda m: m.region_start)
    movement_bridges = {base + rel for _, rel, _ in RECOMP_LOCAL_MOVEMENT}
    routing_relocs = []
    gift_text_edges = []
    movement_bridge_hits = []
    braille_text_edges = []
    for op in operands:
        src = op["source_gba_offset"]
        cls = op["target_class"]
        target = op["target_gba"]
        width = op["width"]
        key, exp, off = resolve_target(cls, target)
        if src in gift_text_class:
            # No operand source lives in the gift text class (the extractor
            # calls the tail sources "mystery_gift_text" by its window
            # model, but they are the five later modules' script bytes).
            fail(f"gift text class holds an operand source: {src:#x}")
        if cls == "TEXT_TARGET" and target in gift_text_class:
            # Task #3 handoff census: 20 edges into the 20 gift text labels.
            gift_text_edges.append(dict(source_gba_offset=src,
                                        target_label=exp,
                                        target_resource_key=key))
        if target in movement_bridges:
            # One operand per bridge (pin 3); recorded for the report.
            movement_bridge_hits.append(target)
        if cls == "MART_TABLE_TARGET" and target in text_class_bytes:
            # Braille text edges (26 into 16 labels); pending C keys.
            braille_text_edges.append(dict(source_gba_offset=src,
                                           target_label=exp,
                                           target_resource_key=key))
        # attribute the source: routing class or a module
        if base <= src < main_end and src in routing:
            routing_relocs.append(dict(
                source_gba_offset=src,
                source_table=routing_source_tables.get(src, "unknown"),
                operand_width=width,
                target_class=cls, target_gba=target,
                target_resource_key=key, target_export=exp, target_offset=off))
            continue
        if base <= src < main_end and src in engine:
            fail(f"engine source in operand set: {src:#x}")
        src_mod = find_module(by_start, src)
        if src_mod is None or src not in src_mod.gbytes:
            fail(f"source {src:#x} not attributed to any module or routing class")
        src_mod.relocs.append(dict(
            operand_payload_offset=payload_offset_of(src_mod, src),
            operand_width=width, original_encoded_gba=target,
            target_class=cls, target_resource_key=key,
            target_export=exp, target_offset=off,
            runtime_resolution_required=True))

    for m in all_modules:
        m.relocs.sort(key=lambda r: (r["operand_payload_offset"],
                                     r["original_encoded_gba"]))
    if (len(text_alias_hits) != TEXT_ALIAS_EDGES_PIN
            or set(text_alias_hits) != TEXT_ALIAS_LABELS):
        fail(f"text alias edges {len(text_alias_hits)} {sorted(set(text_alias_hits))} "
             f"!= {TEXT_ALIAS_EDGES_PIN} {sorted(TEXT_ALIAS_LABELS)}")
    if len(movement_bridge_hits) != len(RECOMP_LOCAL_MOVEMENT):
        fail(f"movement bridge operands {len(movement_bridge_hits)} "
             f"!= {len(RECOMP_LOCAL_MOVEMENT)}")
    if (len(braille_text_edges) != BRAILLE_TEXT_EDGES_PIN
            or len({e["target_label"] for e in braille_text_edges}) != BRAILLE_TEXT_LABELS_PIN):
        fail(f"braille text edges {len(braille_text_edges)} / "
             f"{len({e['target_label'] for e in braille_text_edges})} labels "
             f"!= {BRAILLE_TEXT_EDGES_PIN} / {BRAILLE_TEXT_LABELS_PIN}")
    print(f"  rss {rss_mb()} MB (relocs: +{rss_mb() - phase_rss} MB)")

    # Emit the KEPT module set only (all_modules includes the movement-only
    # include that the retention rule drops; it is not a G module).
    #
    # Release the heavyweight classification structures first: the
    # documents are built from modules/relocs alone, and the host runs
    # near its memory ceiling (an earlier emission pass was OOM-killed).
    del text_starts, move_starts, gift_text_class, mart_targets
    del val_by_name, opcode_table, trainer_types
    del routing_source_tables, extra_movement, engine, elf
    # The attribution closures hold the same structures via their cells;
    # drop them so the memory is actually reclaimable before emission.
    # names_by_addr survives: the F-inbound/std renderers still need the
    # label map for routing-class entrypoints.
    del bundle_key_for_target, movement_key_for_target, payload_offset_of
    del resolve_target
    emit_all(outdir, args, rom, modules, text_class_bytes,
             move_class_bytes, routing, operands, routing_relocs,
             gift_text_edges, braille_text_edges, bundle_of, move_id_of,
             base, std, object_refs, coord_refs, bg_refs,
             map_script_refs, names_by_addr)


if __name__ == "__main__":
    main()

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
    # (qualified name, recomp-compiled name, region-relative offset, size)
    ("Route103_EventScript_RivalExitFacingNorth2",
     "Route103_Movement_RivalExitFacingNorth2", 0x10F45, 7),
    ("Ferry_EventScript_DepartIslandBoardSouth",
     "Ferry_Movement_DepartIslandBoardSouth", 0x673BB, 2),
    ("Ferry_EventScript_DepartIslandBoardWest",
     "Ferry_Movement_DepartIslandBoardWest", 0x673BD, 3),
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

# R13-G3 (shadow resolver seam): target sub-kind pins measured against
# the qualified graph. SCRIPT_TARGET rows split into payload (module
# bytecode, export boundary) / routing (region-gap dispatch table
# starts) / bridge (the 3 recomp-local movement cases); TEXT_TARGET
# rows are bundle members (canonical label inside a C bundle blob) or
# per-label catalog ids (the 20 gift handoff labels); MART_TABLE_TARGET
# rows are the 38 typed mart tables, the 198 dispatch-table targets, or
# the 26 braille text-class edges. DISPATCH and BRAILLE stay deferred
# through G3 (routing bytes are not module payload; the 22 braille
# labels await their C handoff); everything else resolves to a live
# arena/sibling-seam pointer in the shadow generation.
TARGET_SUBKIND_PINS = {
    # Measured 2026-08-21 against the qualified graph: 8,198 payload
    # script targets + 7 routing-class script targets (dispatch rows
    # into conditional tables) + 3 movement bridges close the 8,208
    # SCRIPT_TARGET class; 6,187 bundle members + 20 per-label gift
    # ids close 6,207 TEXT_TARGET; all 2,009 MOVEMENT_TARGET edges
    # resolve to B resources (the 3 bridges are SCRIPT-class operands).
    "SCRIPT_PAYLOAD": 8198,
    "SCRIPT_ROUTING": 7,
    "SCRIPT_BRIDGE": 3,
    "TEXT_BUNDLE_MEMBER": 6187,
    "TEXT_LABEL": 20,
    "MOVEMENT_RESOURCE": 2009,
    "MOVEMENT_BRIDGE": 0,
    "MART_TABLE": 38,
    "DISPATCH": 198,
    "BRAILLE": 26,
    "RAM_HOST": 18,
}

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
             names_by_addr, dyn):
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

    # ---- R13-G3 seam tables (plan sec 1): generated C inventory ----
    # The std/F/mart rows resolve with the SAME entrypoint logic as the
    # TOML renderers (the f_inbound TOML itself is not tomllib-parseable:
    # its top-level `references` count key collides with the
    # [[references]] array-of-tables name), so the C rows are built
    # directly from the resolved data.
    std_raw = []
    for i, (off, typ, sym) in enumerate(sorted(std, key=lambda r: r[0])):
        target = struct.unpack_from("<I", rom, off - GEN3_GBA_ROM_BASE)[0]
        tmod, ex, moff, boundary = resolve_entrypoint(
            "gStdScripts", dict(gba_target=target), by_start, routing,
            move_class_bytes, names_by_addr)
        std_raw.append(dict(slot=i, rom_offset=off, encoded_gba=target,
                            module_key=tmod.id, export=ex["name"],
                            module_offset=moff, boundary_kind=boundary))
    f_raw = []
    for kind, refs in (("map-scripts", map_script_refs),
                       ("object-event", object_refs),
                       ("coord-event", coord_refs),
                       ("bg-event", bg_refs)):
        for r in refs:
            tmod, ex, moff, boundary = resolve_entrypoint(
                kind, r, by_start, routing, move_class_bytes,
                names_by_addr)
            key = (r["map_key"][len("emerald:data/map/"):-len("/header")]
                   if kind == "map-scripts"
                   else r["map_name"].lower().replace("_", "-"))
            same_map = resolve_same_map(kind, r, tmod)
            map_symbol = (r["map_symbol"] if kind == "map-scripts"
                          else r["map_name"])
            f_raw.append(dict(kind=kind, map_symbol=map_symbol, map_key=key,
                              gba_target=r["gba_target"],
                              module_key=tmod.id, export=ex["name"],
                              module_offset=moff, boundary_kind=boundary,
                              same_map=same_map))
    mart_raw = []
    for m in sorted(modules, key=lambda m: m.region_start):
        for s in m.segments:
            if s.get("kind") != "static-data":
                continue
            ex = next((e for e in m.exports
                       if e["original_gba_address"] == s["original_gba_start"]),
                      None)
            mart_raw.append(dict(
                label=ex["name"] if ex else
                f"table_0x{s['original_gba_start']:X}",
                gba_address=s["original_gba_start"], module_key=m.id,
                module_offset=s["payload_offset"], byte_count=s["byte_count"],
                item_count=(s["byte_count"] - 2) // 2))
    ram_rows = [dict(gba_address=op["target_gba"],
                     source_gba_offset=op["source_gba_offset"])
                for op in operands
                if op["target_class"] == "RAM_DATA_TARGET"]
    emit_script_native_tables(Path(args.root), args, rom, base, modules,
                              routing_relocs, routing, std_raw, f_raw,
                              mart_raw, ram_rows, dyn)

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


def payload_offset_of_opt(m, a):
    """Payload offset of GBA address `a` in module `m`, or None when the
    address is not inside one of `m`'s emitted segments (region gaps:
    routing tables, movement bridges, etc.)."""
    for s in m.segments:
        if s["original_gba_start"] <= a < s["original_gba_start"] + s["byte_count"]:
            return s["payload_offset"] + (a - s["original_gba_start"])
    return None


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
        f"instruction_count = {len(m.boundaries)}",
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
            f"target_kind = \"{r['target_kind']}\"",
            f"target_payload_offset = {r['target_payload_offset']}",
            f"runtime_resolution_required = {str(r['runtime_resolution_required']).lower()}",
            "",
        ]
    for off, length in m.boundaries:
        lines += [
            "[[boundaries]]",
            f"payload_offset = {off}",
            f"length = {length}",
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
    for name, compiled, rel, size in RECOMP_LOCAL_MOVEMENT:
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




def render_script_native_h(n_mod, n_seg, n_exp, n_rel, n_rrt, n_bnd, n_dyn,
                            n_rseg):
    pins = [
        ("EMERALD_SCRIPT_MODULE_COUNT", n_mod, 523),
        ("EMERALD_SCRIPT_SEGMENT_COUNT", n_seg, 812),
        ("EMERALD_SCRIPT_EXPORT_COUNT", n_exp, 7683),
        ("EMERALD_SCRIPT_RELOC_COUNT", n_rel, 15874),
        ("EMERALD_SCRIPT_ROUTING_RELOC_COUNT", n_rrt, 830),
        ("EMERALD_SCRIPT_TOTAL_RELOC_COUNT", n_rel + n_rrt, 16704),
        ("EMERALD_SCRIPT_STD_SCRIPT_COUNT", 11, 11),
        ("EMERALD_SCRIPT_F_BINDING_COUNT", 3501, 3501),
        ("EMERALD_SCRIPT_MART_COUNT", 38, 38),
        ("EMERALD_SCRIPT_RAM_TARGET_COUNT", 18, 18),
        ("EMERALD_SCRIPT_RAM_ALLOWLIST_COUNT", 1, 1),
        ("EMERALD_SCRIPT_BRIDGE_COUNT", 3, 3),
        ("EMERALD_SCRIPT_ROUTING_SEGMENT_COUNT", n_rseg, n_rseg),
        ("EMERALD_SCRIPT_ARENA_PAYLOAD_BYTES", 207330, 207330),
        ("EMERALD_SCRIPT_ARENA_ALIGNMENT", 16, 16),
    ]
    lines = [
        "/* Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        " * Do not edit by hand; regeneration must be a no-op diff.",
        " *",
        " * R13-G3 shadow-seam native inventory (plan sec 1): the G2 script",
        " * family metadata projected into C - 523 module rows, 812 segments,",
        " * 7,683 exports, 15,874 module relocations + 830 routing",
        " * relocations (16,704 operands), the instruction-boundary walk,",
        " * the dynamic encoded-GBA target index (parity oracle input), the",
        " * 11 gStdScripts shadow records, the 3,501 F inbound bindings,",
        " * the 38 mart tables, the 18 RAM targets + 1 allowlist row, and",
        " * the 3 recomp-local movement bridges (bytes embedded from the",
        " * qualified ROM). EmeraldScriptCompat validates the session's pack",
        " * against this table and stages its shadow generation from it.",
        " */",
        "#ifndef EMERALD_RESOURCES_SCRIPT_NATIVE_GENERATED_H",
        "#define EMERALD_RESOURCES_SCRIPT_NATIVE_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
    ]
    for name, actual, wanted in pins:
        if actual != wanted:
            fail(f"script native pin {name}: {actual} != {wanted}")
        lines.append(f"#define {name} {actual}u")
    lines += [
        "",
        "enum EmeraldScriptNativeTargetClass",
        "{",
        "    EMERALD_SCRIPT_NATIVE_CLASS_SCRIPT = 0,",
        "    EMERALD_SCRIPT_NATIVE_CLASS_TEXT = 1,",
        "    EMERALD_SCRIPT_NATIVE_CLASS_MOVEMENT = 2,",
        "    EMERALD_SCRIPT_NATIVE_CLASS_STATIC_DATA = 3,",
        "    EMERALD_SCRIPT_NATIVE_CLASS_RAM_DATA = 4,",
        "};",
        "",
        "enum EmeraldScriptNativeTargetKind",
        "{",
        "    EMERALD_SCRIPT_NATIVE_KIND_SCRIPT_PAYLOAD = 0,",
        "    EMERALD_SCRIPT_NATIVE_KIND_SCRIPT_ROUTING = 1,",
        "    EMERALD_SCRIPT_NATIVE_KIND_SCRIPT_BRIDGE = 2,",
        "    EMERALD_SCRIPT_NATIVE_KIND_TEXT_BUNDLE_MEMBER = 3,",
        "    EMERALD_SCRIPT_NATIVE_KIND_TEXT_LABEL = 4,",
        "    EMERALD_SCRIPT_NATIVE_KIND_MOVEMENT_RESOURCE = 5,",
        "    EMERALD_SCRIPT_NATIVE_KIND_MOVEMENT_BRIDGE = 6,",
        "    EMERALD_SCRIPT_NATIVE_KIND_MART_TABLE = 7,",
        "    EMERALD_SCRIPT_NATIVE_KIND_DISPATCH = 8,",
        "    EMERALD_SCRIPT_NATIVE_KIND_BRAILLE = 9,",
        "    EMERALD_SCRIPT_NATIVE_KIND_RAM_HOST = 10,",
        "};",
        "",
        "enum EmeraldScriptNativeBoundaryKind",
        "{",
        "    EMERALD_SCRIPT_NATIVE_BOUNDARY_OFFSET_ZERO = 0,",
        "    EMERALD_SCRIPT_NATIVE_BOUNDARY_INTERIOR = 1,",
        "    EMERALD_SCRIPT_NATIVE_BOUNDARY_ROUTING = 2,",
        "    EMERALD_SCRIPT_NATIVE_BOUNDARY_NONE = 0xFF,",
        "};",
        "",
        "enum EmeraldScriptNativeModuleKind",
        "{",
        "    EMERALD_SCRIPT_NATIVE_MODULE_MAP = 0,",
        "    EMERALD_SCRIPT_NATIVE_MODULE_COMMON = 1,",
        "    EMERALD_SCRIPT_NATIVE_MODULE_GIFT = 2,",
        "};",
        "",
        "enum EmeraldScriptNativeSegmentKind",
        "{",
        "    EMERALD_SCRIPT_NATIVE_SEGMENT_BYTECODE = 0,",
        "    EMERALD_SCRIPT_NATIVE_SEGMENT_STATIC_DATA = 1,",
        "};",
        "",
        "enum EmeraldScriptNativeExportKind",
        "{",
        "    EMERALD_SCRIPT_NATIVE_EXPORT_SCRIPT = 0,",
        "    EMERALD_SCRIPT_NATIVE_EXPORT_TYPED_DATA = 1,",
        "    EMERALD_SCRIPT_NATIVE_EXPORT_OPAQUE = 2,",
        "};",
        "",
        "enum EmeraldScriptNativeFKind",
        "{",
        "    EMERALD_SCRIPT_NATIVE_F_MAP_SCRIPTS = 0,",
        "    EMERALD_SCRIPT_NATIVE_F_OBJECT_EVENT = 1,",
        "    EMERALD_SCRIPT_NATIVE_F_COORD_EVENT = 2,",
        "    EMERALD_SCRIPT_NATIVE_F_BG_EVENT = 3,",
        "};",
        "",
        "#define EMERALD_SCRIPT_NATIVE_OFFSET_NONE 0xFFFFFFFFu",
        "",
        "struct EmeraldScriptNativeModule",
        "{",
        "    const char *id;",
        "    const char *primarySymbol;",
        "    uint8_t digest[32];",
        "    uint32_t schema;",
        "    uint32_t regionGbaStart;",
        "    uint32_t payloadSize;",
        "    uint32_t arenaOffset;",
        "    uint32_t segmentFirst;",
        "    uint32_t segmentCount;",
        "    uint32_t exportFirst;",
        "    uint32_t exportCount;",
        "    uint32_t relocFirst;",
        "    uint32_t relocCount;",
        "    uint32_t boundaryFirst;",
        "    uint32_t boundaryCount;",
        "    uint32_t routingFirst;",
        "    uint32_t routingCount;",
        "    uint8_t kind;",
        "    uint8_t embedded;",
        "};",
        "",
        "struct EmeraldScriptNativeSegment",
        "{",
        "    uint32_t originalGbaStart;",
        "    uint32_t byteCount;",
        "    uint32_t payloadOffset;",
        "    uint8_t kind;",
        "};",
        "",
        "/* R13-G5 (plan sec 4): a module's routing-class tables (map",
        " * dispatch + conditional tables) staged as a span suffix after",
        " * the payload. spanOffset is payload-relative; the live address",
        " * of table byte X is arena + module.arenaOffset + spanOffset +",
        " * (X - originalGbaStart). */",
        "struct EmeraldScriptNativeRoutingSegment",
        "{",
        "    uint32_t moduleIndex;",
        "    uint32_t originalGbaStart;",
        "    uint32_t byteCount;",
        "    uint32_t spanOffset;",
        "};",
        "",
        "struct EmeraldScriptNativeExport",
        "{",
        "    uint32_t moduleIndex;",
        "    uint32_t payloadOffset;",
        "    uint32_t originalGbaAddress;",
        "    uint8_t boundaryKind;",
        "    uint8_t kind;",
        "    uint32_t name;",
        "};",
        "",
        "struct EmeraldScriptNativeReloc",
        "{",
        "    uint32_t moduleIndex;",
        "    uint32_t operandPayloadOffset;",
        "    uint32_t originalEncodedGba;",
        "    uint8_t targetClass;",
        "    uint8_t targetKind;",
        "    uint32_t targetKey;",
        "    uint32_t targetLabel;",
        "    uint32_t targetOffset;",
        "    uint32_t targetPayloadOffset;",
        "};",
        "",
        "struct EmeraldScriptNativeRoutingReloc",
        "{",
        "    uint32_t sourceGbaOffset;",
        "    uint32_t sourceTable;",
        "    uint8_t targetClass;",
        "    uint8_t targetKind;",
        "    uint32_t targetGba;",
        "    uint32_t targetKey;",
        "    uint32_t targetLabel;",
        "    uint32_t targetOffset;",
        "    uint32_t targetPayloadOffset;",
        "};",
        "",
        "struct EmeraldScriptNativeDynamicTarget",
        "{",
        "    uint32_t gbaAddress;",
        "    uint8_t targetClass;",
        "    uint8_t targetKind;",
        "    uint32_t targetKey;",
        "    uint32_t targetLabel;",
        "    uint32_t targetOffset;",
        "    uint32_t targetPayloadOffset;",
        "    uint8_t boundaryKind;",
        "};",
        "",
        "struct EmeraldScriptNativeBridge",
        "{",
        "    const char *key;",
        "    const char *symbol;",
        "    uint32_t gbaAddress;",
        "    uint8_t byteCount;",
        "    uint8_t bytes[8];",
        "};",
        "",
        "struct EmeraldScriptNativeStdScript",
        "{",
        "    uint32_t moduleIndex;",
        "    uint32_t exportIndex;",
        "    uint32_t payloadOffset;",
        "    uint32_t encodedGba;",
        "    uint32_t romOffset;",
        "    uint8_t boundaryKind;",
        "    uint8_t slot;",
        "};",
        "",
        "struct EmeraldScriptNativeFBinding",
        "{",
        "    uint8_t kind;",
        "    uint8_t boundaryKind;",
        "    uint8_t sameMap;",
        "    uint32_t mapSymbol;",
        "    uint32_t mapKey;",
        "    uint32_t gbaTarget;",
        "    uint32_t moduleIndex;",
        "    uint32_t exportIndex;",
        "    uint32_t payloadOffset;",
        "};",
        "",
        "struct EmeraldScriptNativeMart",
        "{",
        "    uint32_t moduleIndex;",
        "    uint32_t payloadOffset;",
        "    uint32_t gbaAddress;",
        "    uint32_t label;",
        "    uint16_t byteCount;",
        "    uint16_t itemCount;",
        "};",
        "",
        "struct EmeraldScriptNativeRamTarget",
        "{",
        "    uint32_t gbaAddress;",
        "    uint8_t region;",
        "    uint32_t sourceGbaOffset;",
        "};",
        "",
        "struct EmeraldScriptNativeRamAllowlist",
        "{",
        "    uint32_t gbaAddress;",
        "    uint32_t size;",
        "};",
        "",
        "struct EmeraldScriptNativeBoundary",
        "{",
        "    uint32_t payloadOffset;",
        "    uint16_t length;",
        "    uint16_t pad;",
        "};",
        "",
        "struct EmeraldScriptCompatNativeTable",
        "{",
        "    const struct EmeraldScriptNativeModule *modules;",
        "    uint32_t moduleCount;",
        "    const struct EmeraldScriptNativeSegment *segments;",
        "    uint32_t segmentCount;",
        "    const struct EmeraldScriptNativeExport *exports;",
        "    uint32_t exportCount;",
        "    const struct EmeraldScriptNativeReloc *relocs;",
        "    uint32_t relocCount;",
        "    const struct EmeraldScriptNativeRoutingReloc *routingRelocs;",
        "    uint32_t routingRelocCount;",
        "    const struct EmeraldScriptNativeDynamicTarget *dynamicTargets;",
        "    uint32_t dynamicTargetCount;",
        "    const struct EmeraldScriptNativeStdScript *stdScripts;",
        "    uint32_t stdScriptCount;",
        "    const struct EmeraldScriptNativeFBinding *fBindings;",
        "    uint32_t fBindingCount;",
        "    const struct EmeraldScriptNativeMart *marts;",
        "    uint32_t martCount;",
        "    const struct EmeraldScriptNativeRamTarget *ramTargets;",
        "    uint32_t ramTargetCount;",
        "    const struct EmeraldScriptNativeRamAllowlist *ramAllowlist;",
        "    uint32_t ramAllowlistCount;",
        "    const struct EmeraldScriptNativeBridge *bridges;",
        "    uint32_t bridgeCount;",
        "    const struct EmeraldScriptNativeBoundary *boundaries;",
        "    uint32_t boundaryCount;",
        "    const struct EmeraldScriptNativeRoutingSegment *routingSegments;",
        "    uint32_t routingSegmentCount;",
        "    const uint8_t *routingBytes;",
        "    uint32_t routingByteCount;",
        "    const char *const *pool;",
        "    uint32_t poolCount;",
        "};",
        "",
        "extern const struct EmeraldScriptCompatNativeTable kEmeraldScriptCompatTable;",
        "",
        "#endif /* EMERALD_RESOURCES_SCRIPT_NATIVE_GENERATED_H */",
        "",
    ]
    return "\n".join(lines)


def _cstr(s):
    return '"%s"' % s


def render_script_native_c(mod_rows, seg_rows, exp_rows, reloc_rows, rrt_rows,
                           dyn_rows, bridge_rows, std_rows, f_rows, mart_rows,
                           ram_rows, bound_rows, rseg_rows, pool, arena_bytes):
    cls_map = {"SCRIPT_TARGET": 0, "TEXT_TARGET": 1, "MOVEMENT_TARGET": 2,
               "MART_TABLE_TARGET": 3, "RAM_DATA_TARGET": 4}
    kind_map = {
        "SCRIPT_PAYLOAD": 0, "SCRIPT_ROUTING": 1, "SCRIPT_BRIDGE": 2,
        "TEXT_BUNDLE_MEMBER": 3, "TEXT_LABEL": 4, "MOVEMENT_RESOURCE": 5,
        "MOVEMENT_BRIDGE": 6, "MART_TABLE": 7, "DISPATCH": 8,
        "BRAILLE": 9, "RAM_HOST": 10,
    }
    bnd_map = {"offset-zero": 0, "interior": 1, "routing": 2}
    fkind_map = {"map-scripts": 0, "object-event": 1, "coord-event": 2,
                 "bg-event": 3}
    out = [
        "/* Generated by tools/gen3_resources/script_family/gen_script_family.py.",
        " * Do not edit by hand; regeneration must be a no-op diff.",
        " * See include/emerald/resources/script_native.generated.h for the",
        " * row contracts. One initializer per line (the G3 fault-injection",
        " * harness edits rows textually).",
        " */",
        "#include \"emerald/resources/script_native.generated.h\"",
        "",
        "/* The 3 recomp-local movement bridges (plan sec 5): explicit named",
        " * compiled bridges, 12 B total. The table embeds the qualified-ROM",
        " * bytes so EmeraldScriptCompat can prove byte identity against the",
        " * compiled symbols; the live pointers themselves come from the",
        " * symbols below (never written into any arena). */",
        "extern const uint8_t Route103_Movement_RivalExitFacingNorth2[7];",
        "extern const uint8_t Ferry_Movement_DepartIslandBoardSouth[2];",
        "extern const uint8_t Ferry_Movement_DepartIslandBoardWest[3];",
        "const uint8_t *const kEmeraldScriptNativeBridgeSymbols["
        + str(len(bridge_rows)) + "] = {",
    ]
    for b in bridge_rows:
        out.append(f"    {b['sym']},")
    out += [
        "};",
        "",
        "/* The one approved writable RAM target (plan sec 9): EWRAM",
        " * 0x02021fc4 = gStringVar4, the 1,000-byte string-variable",
        " * buffer. RAM operands resolve to this host symbol only -",
        " * permission comes from the allowlist, never from address",
        " * arithmetic. */",
        "extern uint8_t gStringVar4[1000];",
        "",
    ]
    out.append(f"static const char *const kScriptPool[{len(pool)}] = {{")
    for s in pool:
        out.append(f"    {_cstr(s)},")
    out += [
        "};",
        "",
    ]

    def emit_array(cname, ctype, rows, fmt):
        out.append(f"static const {ctype} {cname}[{len(rows)}] = {{")
        for r in rows:
            out.append("    " + fmt(r) + ",")
        out.append("};")
        out.append("")

    emit_array("kScriptModules", "struct EmeraldScriptNativeModule",
               mod_rows,
               lambda r: "{%s, %s, {%s}, %uu, 0x%xu, %uu, %uu, %uu, %uu, "
                         "%uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu}" % (
                   _cstr(r["id"]), _cstr(r["sym"]),
                   ", ".join("0x%02x" % b for b in r["digest"]),
                   r["schema"], r["rstart"], r["size"], r["arena"],
                   r["sfirst"], r["sn"], r["efirst"], r["en"],
                   r["rfirst"], r["rn"], r["bfirst"], r["bn"],
                   r["rsegfirst"], r["rsegn"],
                   r["kind"], r["embedded"]))
    emit_array("kScriptSegments", "struct EmeraldScriptNativeSegment",
               seg_rows,
               lambda r: "{0x%xu, %uu, %uu, %uu}" % (r["gba"], r["n"],
                                                     r["off"], r["kind"]))
    emit_array("kScriptExports", "struct EmeraldScriptNativeExport",
               exp_rows,
               lambda r: "{%uu, %uu, 0x%xu, %uu, %uu, %uu}" % (
                   r["mi"], r["off"], r["gba"], r["bk"], r["kind"],
                   r["name"]))
    emit_array("kScriptRelocs", "struct EmeraldScriptNativeReloc",
               reloc_rows,
               lambda r: "{%uu, %uu, 0x%xu, %uu, %uu, %uu, %uu, %uu, %uu}" % (
                   r["mi"], r["off"], r["enc"],
                   cls_map[r["cls"]], kind_map[r["kind"]],
                   r["key"], r["label"], r["to"], r["tpo"]))
    emit_array("kScriptRoutingRelocs",
               "struct EmeraldScriptNativeRoutingReloc", rrt_rows,
               lambda r: "{0x%xu, %uu, %uu, %uu, 0x%xu, %uu, %uu, %uu, %uu}" % (
                   r["gba"], r["table"], cls_map[r["cls"]],
                   kind_map[r["kind"]], r["tg"], r["key"], r["label"],
                   r["to"], r["tpo"]))
    emit_array("kScriptDynamicTargets",
               "struct EmeraldScriptNativeDynamicTarget", dyn_rows,
               lambda r: "{0x%xu, %uu, %uu, %uu, %uu, %uu, %uu, %uu}" % (
                   r["gba"], cls_map[r["cls"]], kind_map[r["kind"]],
                   r["key"], r["label"], r["to"], r["tpo"], r["bk"]))
    # R13-G5: the routing-class bytes come straight from the qualified
    # ROM (the generator's provenance) - they are not pack resources.
    out.append("/* R13-G5: the 5,749 B routing class (map dispatch + "
               "conditional tables), exact qualified-ROM bytes. The seam "
               "copies each module's routing suffix from this blob. */")
    routing_blob = b"".join(r["rom"] for r in rseg_rows)
    out.append(f"const uint8_t kScriptRoutingBytes[{len(routing_blob)}] = {{")
    for i in range(0, len(routing_blob), 16):
        chunk = routing_blob[i:i + 16]
        out.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
    out.append("};")
    out.append("")

    emit_array("kScriptBridges", "struct EmeraldScriptNativeBridge",
               bridge_rows,
               lambda r: "{%s, %s, 0x%xu, %uu, {%s}}" % (
                   _cstr(r["key"]), _cstr(r["sym"]), r["gba"], r["n"],
                   ", ".join("0x%02x" % b for b in r["bytes"])))
    emit_array("kScriptStdScripts", "struct EmeraldScriptNativeStdScript",
               std_rows,
               lambda r: "{%uu, %uu, %uu, 0x%xu, 0x%xu, %uu, %uu}" % (
                   r["mi"], r["ei"], r["po"], r["enc"], r["rom"],
                   r["bk"], r["slot"]))
    emit_array("kScriptFBindings", "struct EmeraldScriptNativeFBinding",
               f_rows,
               lambda r: "{%uu, %uu, %uu, %uu, %uu, 0x%xu, %uu, %uu, %uu}" % (
                   fkind_map[r["kind"]], r["bk"], r["sm"], r["ms"],
                   r["mk"], r["gt"], r["mi"], r["ei"], r["po"]))
    emit_array("kScriptMarts", "struct EmeraldScriptNativeMart", mart_rows,
               lambda r: "{%uu, %uu, 0x%xu, %uu, %uu, %uu}" % (
                   r["mi"], r["po"], r["gba"], r["label"], r["n"],
                   r["items"]))
    emit_array("kScriptRamTargets", "struct EmeraldScriptNativeRamTarget",
               ram_rows,
               lambda r: "{0x%xu, %uu, 0x%xu}" % (r["gba"], 0,
                                                  r["src"]))
    emit_array("kScriptRamAllowlist", "struct EmeraldScriptNativeRamAllowlist",
               [dict(gba=0x2021FC4, size=1000)],
               lambda r: "{0x%xu, %uu}" % (r["gba"], r["size"]))
    emit_array("kScriptBoundaries", "struct EmeraldScriptNativeBoundary",
               bound_rows,
               lambda r: "{%uu, %uu, 0u}" % (r["off"], r["n"]))
    emit_array("kScriptRoutingSegments",
               "struct EmeraldScriptNativeRoutingSegment", rseg_rows,
               lambda r: "{%uu, 0x%xu, %uu, %uu}" % (r["mi"], r["gba"],
                                                     r["n"], r["off"]))

    out += [
        "const struct EmeraldScriptCompatNativeTable kEmeraldScriptCompatTable = {",
        "    kScriptModules, EMERALD_SCRIPT_MODULE_COUNT,",
        "    kScriptSegments, EMERALD_SCRIPT_SEGMENT_COUNT,",
        "    kScriptExports, EMERALD_SCRIPT_EXPORT_COUNT,",
        "    kScriptRelocs, EMERALD_SCRIPT_RELOC_COUNT,",
        "    kScriptRoutingRelocs, EMERALD_SCRIPT_ROUTING_RELOC_COUNT,",
        "    kScriptDynamicTargets, (uint32_t)" + str(len(dyn_rows)) + "u,",
        "    kScriptStdScripts, EMERALD_SCRIPT_STD_SCRIPT_COUNT,",
        "    kScriptFBindings, EMERALD_SCRIPT_F_BINDING_COUNT,",
        "    kScriptMarts, EMERALD_SCRIPT_MART_COUNT,",
        "    kScriptRamTargets, EMERALD_SCRIPT_RAM_TARGET_COUNT,",
        "    kScriptRamAllowlist, EMERALD_SCRIPT_RAM_ALLOWLIST_COUNT,",
        "    kScriptBridges, EMERALD_SCRIPT_BRIDGE_COUNT,",
        "    kScriptBoundaries, (uint32_t)" + str(len(bound_rows)) + "u,",
        "    kScriptRoutingSegments, (uint32_t)" + str(len(rseg_rows)) + "u,",
        "    kScriptRoutingBytes, (uint32_t)" + str(len(routing_blob)) + "u,",
        "    kScriptPool, (uint32_t)" + str(len(pool)) + "u,",
        "};",
        "",
    ]
    return "\n".join(out)


def emit_script_native_tables(root, args, rom, base, modules, routing_relocs,
                              routing, std_raw, f_raw, mart_raw,
                              ram_rows, dyn):
    """R13-G3 (plan sec 1/10/18): emit the shadow seam's generated C
    inventory - include/emerald/resources/script_native.generated.h +
    src/emerald/resources/script_native_table.generated.c. Every row is
    a projection of the already-generated G2 metadata plus the boundary
    walk and the dynamic encoded-GBA target index derived in main();
    the G1 graph itself is not reinterpreted here.

    R13-G5 (plan sec 4): the module spans additionally stage each
    module's routing-class tables (map dispatch + conditional tables,
    5,749 B total - the region-gap bytes G2 typed as routing segments)
    as a routing suffix after the payload. That materializes the
    MapHeader.mapScripts provenance and every dispatch-entry operand as
    live arena bytes without touching any payload offset."""
    mods = sorted(modules, key=lambda m: m.id)
    mod_idx = {m.id: i for i, m in enumerate(mods)}
    # Deterministic arena layout: modules in bytewise id order (the
    # manifest order), 16-aligned spans; each span = payload + the
    # module's routing suffix.
    routing_runs = {}
    routing_total = 0
    for m in mods:
        runs = []
        cur = None
        for a in range(m.region_start, m.region_end):
            if a in routing:
                if cur is None:
                    cur = a
            elif cur is not None:
                runs.append((cur, a))
                cur = None
        if cur is not None:
            runs.append((cur, m.region_end))
        routing_runs[m.id] = runs
        routing_total += sum(b - a for a, b in runs)
    if routing_total != ROUTING_BYTES_PIN:
        fail(f"staged routing bytes {routing_total} != {ROUTING_BYTES_PIN}")
    span_sizes = {
        m.id: m.payload_bytes + sum(b - a for a, b in routing_runs[m.id])
        for m in mods}
    arena_offsets = {}
    cursor = 0
    for m in mods:
        arena_offsets[m.id] = cursor
        cursor += (span_sizes[m.id] + 15) & ~15
    arena_bytes = cursor

    # String pool (index 0 = "").
    pool_idx = {"": 0}
    pool = [""]

    def P(s):
        if s is None:
            return 0xFFFFFFFF
        i = pool_idx.get(s)
        if i is None:
            i = len(pool)
            pool_idx[s] = i
            pool.append(s)
        return i

    # Braille export rows: the MART_TABLE_TARGET rows with BRAILLE kind
    # name (module, payload offset); those exports are typed braille
    # data positions, not instruction boundaries.
    braille_offsets = set()
    for m in mods:
        for r in m.relocs:
            if r["target_kind"] == "BRAILLE":
                braille_offsets.add((m.id, r["target_payload_offset"]))
    mart_spans = {}
    for m in mods:
        mart_spans[m.id] = [
            (s["payload_offset"], s["payload_offset"] + s["byte_count"])
            for s in m.segments if s.get("kind") == "static-data"]

    mod_rows, seg_rows, exp_rows, reloc_rows, bound_rows = [], [], [], [], []
    rseg_rows = []
    exp_index_of = {}  # module id -> {payload_offset: global export row}
    seg_first = exp_first = rel_first = bound_first = rseg_first = 0
    for mi, m in enumerate(mods):
        mod_rows.append(dict(
            id=m.id, sym=m.primary_symbol,
            digest=bytes.fromhex(sha256hex(m.payload)),
            schema=m.schema, rstart=m.region_start, size=m.payload_bytes,
            arena=arena_offsets[m.id],
            kind={"map": 0, "common": 1, "mystery-gift": 2}[m.kind],
            embedded=1 if m.payload else 0,
            sfirst=seg_first, sn=len(m.segments),
            efirst=exp_first, en=len(m.exports),
            rfirst=rel_first, rn=len(m.relocs),
            bfirst=bound_first, bn=len(m.boundaries),
            rsegfirst=rseg_first, rsegn=len(routing_runs[m.id])))
        for gba0, gba1 in routing_runs[m.id]:
            rseg_rows.append(dict(
                mi=mi, gba=gba0, n=gba1 - gba0,
                off=m.payload_bytes + sum(b - a for a, b in routing_runs[m.id]
                                          if b <= gba0),
                rom=rom[(gba0 - GEN3_GBA_ROM_BASE):(gba1 - GEN3_GBA_ROM_BASE)]))
            rseg_first += 1
        exp_index_of[m.id] = {}
        for s in m.segments:
            seg_rows.append(dict(
                gba=s["original_gba_start"], n=s["byte_count"],
                off=s["payload_offset"],
                kind=1 if s.get("kind") == "static-data" else 0))
        for e in sorted(m.exports, key=lambda e: e["payload_offset"]):
            exp_index_of[m.id][e["payload_offset"]] = exp_first
            if (m.id, e["payload_offset"]) in braille_offsets \
               or any(s <= e["payload_offset"] < t
                      for s, t in mart_spans[m.id]):
                ekind = 1
            elif e.get("opaque"):
                ekind = 2
            else:
                ekind = 0
            exp_rows.append(dict(
                mi=mi, off=e["payload_offset"], gba=e["original_gba_address"],
                bk=0 if e["boundary_kind"] == "offset-zero" else 1,
                kind=ekind,
                name=P(e["name"])))
            exp_first += 1
        for r in m.relocs:
            reloc_rows.append(dict(
                mi=mi, off=r["operand_payload_offset"],
                enc=r["original_encoded_gba"],
                cls=r["target_class"], kind=r["target_kind"],
                key=P(r["target_resource_key"]),
                label=P(r.get("canonical_label") or r.get("target_export")),
                to=r["target_offset"] if r.get("target_offset") is not None
                   else 0xFFFFFFFF,
                tpo=r.get("target_payload_offset", 0xFFFFFFFF)))
        for off, length in m.boundaries:
            bound_rows.append(dict(off=off, n=length))
        seg_first += len(m.segments)
        rel_first += len(m.relocs)
        bound_first += len(m.boundaries)
    if exp_first != len(exp_rows):
        fail(f"export row count drift {exp_first} != {len(exp_rows)}")

    rrt_rows = []
    for r in sorted(routing_relocs, key=lambda r: r["source_gba_offset"]):
        rrt_rows.append(dict(
            gba=r["source_gba_offset"], table=P(r["source_table"]),
            cls=r["target_class"], kind=r["target_kind"],
            tg=r["target_gba"], key=P(r["target_resource_key"]),
            label=P(r.get("canonical_label") or r.get("target_export")),
            to=r["target_offset"] if r.get("target_offset") is not None
               else 0xFFFFFFFF,
            tpo=r.get("target_payload_offset", 0xFFFFFFFF)))

    # Dynamic target index: unique (encoded GBA, class) -> canonical
    # identity, sorted ascending; the seam's encoded-target resolution
    # and the 16,704 parity oracle both read it.
    dyn_rows = []
    for (gba, cls), (kind, key, label, to, tpo, canon) in sorted(dyn.items()):
        bk = 0xFF
        if kind == "SCRIPT_PAYLOAD":
            tmi = mod_idx.get(key)
            if tmi is not None:
                ex = next((e for e in mods[tmi].exports
                           if e["payload_offset"] == tpo), None)
                if ex is not None:
                    bk = 0 if ex["boundary_kind"] == "offset-zero" else 1
        dyn_rows.append(dict(
            gba=gba, cls=cls, kind=kind, key=P(key),
            label=P(canon or label),
            to=to if to is not None else 0xFFFFFFFF,
            tpo=tpo if tpo is not None else 0xFFFFFFFF, bk=bk))

    # The 3 recomp-local movement bridges: gba/size/bytes come straight
    # from the qualified ROM (the generator's provenance, plan sec 5).
    bridge_rows = []
    for name, compiled, rel, size in RECOMP_LOCAL_MOVEMENT:
        gba = base + rel
        data = rom[(gba - GEN3_GBA_ROM_BASE):(gba - GEN3_GBA_ROM_BASE) + size]
        bridge_rows.append(dict(
            key=f"emerald:movement/bridge/{slugify(name)}", sym=compiled,
            gba=gba, n=size, bytes=list(data)))

    # gStdScripts + F inbound rows: payload offsets are derived from the
    # encoded GBA target through segment containment (the TOML rows
    # carry region-relative offsets per the G1 convention).
    std_rows = []
    for rec in std_raw:
        mi = mod_idx[rec["module_key"]]
        po = payload_offset_of_opt(mods[mi], rec["encoded_gba"])
        if po is None:
            fail(f"gStdScripts slot {rec['slot']}: {rec['export']} not in "
                 f"module payload")
        ei = exp_index_of[rec["module_key"]].get(po)
        if ei is None:
            fail(f"gStdScripts slot {rec['slot']}: no export at payload {po}")
        std_rows.append(dict(
            mi=mi, ei=ei, po=po, enc=rec["encoded_gba"],
            rom=rec["rom_offset"],
            bk={"offset-zero": 0, "interior": 1, "routing": 2}[rec["boundary_kind"]],
            slot=rec["slot"]))
    f_rows = []
    fkind_order = {"map-scripts": 0, "object-event": 1, "coord-event": 2,
                   "bg-event": 3}
    for rec in sorted(f_raw, key=lambda r: (fkind_order[r["kind"]],
                                            r["gba_target"])):
        mi = mod_idx[rec["module_key"]]
        is_routing = rec["boundary_kind"] == "routing"
        po = 0xFFFFFFFF
        ei = 0xFFFFFFFF
        if is_routing:
            if rec["kind"] != "map-scripts":
                fail(f"non-map-scripts F ref with routing boundary: "
                     f"{rec['map_symbol']}")
        else:
            po = payload_offset_of_opt(mods[mi], rec["gba_target"])
            if po is None:
                fail(f"F ref {rec['kind']} {rec['map_symbol']} target "
                     f"{rec['gba_target']:#x} not in module payload")
            ei = exp_index_of[rec["module_key"]].get(po)
            if ei is None:
                fail(f"F ref {rec['kind']} {rec['map_symbol']}: no export "
                     f"at payload {po}")
        f_rows.append(dict(
            kind=rec["kind"],
            bk={"offset-zero": 0, "interior": 1, "routing": 2}[rec["boundary_kind"]],
            sm=1 if rec["same_map"] else 0,
            ms=P(rec["map_symbol"]), mk=P(rec["map_key"]),
            gt=rec["gba_target"], mi=mi, ei=ei, po=po))
    if len(f_rows) != F_INBOUND_PINS["map-scripts"] + F_INBOUND_PINS["object-event"] \
            + F_INBOUND_PINS["coord-event"] + F_INBOUND_PINS["bg-event"]:
        fail(f"F rows {len(f_rows)} != 3501")
    mart_rows = []
    for t in mart_raw:
        mi = mod_idx[t["module_key"]]
        mart_rows.append(dict(
            mi=mi, po=t["module_offset"], gba=t["gba_address"],
            label=P(t["label"]), n=t["byte_count"], items=t["item_count"]))
    if len(mart_rows) != MART_TABLES_PIN:
        fail(f"mart rows {len(mart_rows)} != {MART_TABLES_PIN}")
    ram_row_list = []
    for r in ram_rows:
        ram_row_list.append(dict(gba=r["gba_address"], src=r["source_gba_offset"]))
    if len(ram_row_list) != 18:
        fail(f"ram rows {len(ram_row_list)} != 18")
    if len(bridge_rows) != 3 or len(std_rows) != 11:
        fail(f"bridge/std rows {len(bridge_rows)}/{len(std_rows)} != 3/11")

    # Routing segments sort globally by GBA start (the seam
    # binary-searches them; the bytes blob follows the same order).
    # Each module's runs stay contiguous (its region is a contiguous
    # GBA interval), so the per-module windows are re-derived over the
    # sorted rows.
    rseg_rows.sort(key=lambda r: r["gba"])
    rseg_first_by_module = {}
    rseg_count_by_module = {}
    for idx, r in enumerate(rseg_rows):
        if r["mi"] not in rseg_first_by_module:
            rseg_first_by_module[r["mi"]] = idx
            rseg_count_by_module[r["mi"]] = 1
        else:
            rseg_count_by_module[r["mi"]] += 1
    for mi, row in enumerate(mod_rows):
        row["rsegfirst"] = rseg_first_by_module.get(mi, 0)
        row["rsegn"] = rseg_count_by_module.get(mi, 0)
    header = render_script_native_h(
        len(mod_rows), len(seg_rows), len(exp_rows), len(reloc_rows),
        len(rrt_rows), len(bound_rows), len(dyn_rows), len(rseg_rows))
    body = render_script_native_c(
        mod_rows, seg_rows, exp_rows, reloc_rows, rrt_rows, dyn_rows,
        bridge_rows, std_rows, f_rows, mart_rows, ram_row_list,
        bound_rows, rseg_rows, pool, arena_bytes)
    hpath = root / "include/emerald/resources/script_native.generated.h"
    cpath = root / "src/emerald/resources/script_native_table.generated.c"
    write_if(hpath, header, args.check)
    write_if(cpath, body, args.check)
    print(f"    seam tables: {hpath.name} + {cpath.name} "
          f"({len(pool)} pool strings, {len(dyn_rows)} dynamic targets, "
          f"{arena_bytes} B deterministic arena, "
          f"{len(bound_rows)} boundaries)")




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
     std, mg_excluded, bfs_starts, bfs_sizes) = graph_state
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
    for name, compiled, rel, size in RECOMP_LOCAL_MOVEMENT:
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
    movement_bridges = {base + rel for _, _, rel, _ in RECOMP_LOCAL_MOVEMENT}
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

    # ---- R13-G3 shadow-seam enrichment -----------------------------------
    # (plan sec 10/11/18): instruction boundaries, target sub-kinds, payload
    # offsets, and the dynamic encoded-GBA target index all derive from the
    # already-attributed graph; nothing is re-extracted from ROM/ELF.
    phase_rss = rss_mb()
    mods_by_id = {m.id: m for m in all_modules}
    # Text bundles (symbol -> (bundle id, canonical label)) for the
    # bundle-member edges; the per-label ids (incl. the 20 gift handoff
    # labels) resolve by key directly.
    bundles_toml_path = outdir / "text" / "bundles.generated.toml"
    try:
        bundles_doc = tomllib.loads(bundles_toml_path.read_text())
    except OSError:
        fail(f"text bundles missing at {bundles_toml_path}: run "
             f"gen_text_family.py before the script family")
    text_symbol_info = {}
    bundle_id_set = set()
    for b in bundles_doc.get("bundles", []):
        bundle_id_set.add(b["id"])
        for lab in b.get("labels", []):
            text_symbol_info[lab["symbol"]] = (b["id"], lab["canonical"])
    del bundles_doc
    catalog_toml_path = outdir / "text" / "catalog.generated.toml"
    try:
        catalog_ids = {ln.split('"')[1]
                       for ln in catalog_toml_path.read_text().splitlines()
                       if ln.startswith('id = "emerald:')}
    except OSError:
        fail(f"text catalog missing at {catalog_toml_path}: run "
             f"gen_text_family.py before the script family")

    # Instruction boundaries (plan sec 10): the AUTHORITATIVE walk is the
    # G1 BFS (proven roots, break on unknown opcode / terminal) that the
    # operand census used - never a per-segment re-walk (segments can cut
    # through a trainerbattle instruction whose inline metadata was typed
    # static-data by the G2 cutter). The BFS root set does not include
    # map-script dispatch targets (OnTransition/OnFrame/etc. are entered
    # only through routing tables the BFS never walks), so the remaining
    # bytecode-class bytes get a supplementary walk seeded from every
    # SCRIPT_TARGET operand target + F entry + gStdScripts target, with
    # the same grammar/break/terminal rules. The supplementary rows mark
    # instruction boundaries ONLY: the G1 classification, operand census
    # and pins are untouched (no graph re-derivation).
    sup_starts = {}
    seeds = {op["target_gba"] for op in operands
             if op["target_class"] == "SCRIPT_TARGET"}
    seeds.update(x["gba_target"] for x in object_refs)
    seeds.update(x["gba_target"] for x in coord_refs)
    seeds.update(x["gba_target"] for x in bg_refs)
    # Every non-data export is an entry point too (labels the census
    # never references still mark valid code); exports at typed-data
    # positions (mart spans) are data labels and are not seeded.
    for m in all_modules:
        for s in m.segments:
            if s.get("kind") != "static-data":
                continue
            for e in m.exports:
                if (s["payload_offset"] <= e["payload_offset"]
                        < s["payload_offset"] + s["byte_count"]):
                    seeds.discard(e["original_gba_address"])
        for e in m.exports:
            seeds.add(e["original_gba_address"])
    seeds.update(struct.unpack_from("<I", rom, a - GEN3_GBA_ROM_BASE)[0]
                 for a, _, _ in std)
    # G4 state-readiness boundary closure: the eight ordinary-field
    # mystery-gift modules live in data/mystery_gift.o rather than the main
    # script_data section.  The opcode grammar already includes the complete
    # vaddress family (B8-BF); the G3 supplementary walk accidentally gated
    # every seed on the main section's address interval, leaving all 692 gift
    # bytes opaque.  Walk the exact sparse G-owned byte set instead.  This is
    # also stricter than a broad ROM interval: an instruction may not cross a
    # C-text or B-movement hole.
    bytecode_addresses = {gba for m in all_modules for gba in m.gbytes}
    terminal_set = {0x02, 0x03, 0x05, 0x08, 0x0C, 0x0D, 0x24, 0xB9}

    def rom_u8(a):
        return rom[a - GEN3_GBA_ROM_BASE]

    def rom_u32(a):
        return struct.unpack_from("<I", rom, a - GEN3_GBA_ROM_BASE)[0]

    bfs_starts_sorted = sorted(bfs_starts)
    queue = sorted(seeds)
    seen = set(queue)
    while queue:
        p = queue.pop(0)
        if p not in bytecode_addresses or p in text_starts \
           or p in move_starts or p in bfs_starts or p in sup_starts:
            continue
        for _ in range(100000):
            if p in bfs_starts or p in sup_starts:
                break
            spec = opcode_table.get(rom_u8(p))
            if not spec:
                break
            size, types, operand = spec["encoded_size"], \
                list(spec.get("operand_types", [])), 1
            if rom_u8(p) == 0x5C:
                tt = trainer_types.get(rom_u8(p + 1))
                if not tt:
                    break
                size, types, operand = tt["encoded_size"], \
                    list(tt.get("operand_types", [])), 2
            if size <= 0 or any(p + i not in bytecode_addresses
                                for i in range(size)):
                break
            # Never decode across a BFS-decoded start: the census walk
            # owns those bytes and the boundary model must not overlap
            # (a chain may reach a region the BFS entered from another
            # root; the BFS interpretation wins).
            j = bisect.bisect_left(bfs_starts_sorted, p + 1)
            if j < len(bfs_starts_sorted) and bfs_starts_sorted[j] < p + size:
                break
            sup_starts[p] = size
            for typ in types:
                if typ.startswith("ADDR32"):
                    val = rom_u32(p + operand)
                    if typ == "ADDR32_SCRIPT" and val in bytecode_addresses \
                       and val not in seen:
                        queue.append(val)
                        seen.add(val)
                    operand += 4
                elif typ == "U8":
                    operand += 1
                elif typ in ("U16_LE", "VAR16", "SPECIAL_ID"):
                    operand += 2
                elif typ == "U32_LE":
                    operand += 4
            if rom_u8(p) in terminal_set:
                break
            p += size

    start_by_module = {}
    boundary_total = 0
    all_starts = {}
    all_starts.update(bfs_sizes)
    all_starts.update(sup_starts)
    for m in all_modules:
        rows = []
        for gba in m.gbytes:
            if gba in all_starts:
                rows.append((payload_offset_of_opt(m, gba), all_starts[gba]))
        rows.sort()
        m.boundaries = rows
        start_by_module[m.id] = {off for off, _ in rows}
        boundary_total += len(rows)
    print(f"  supplementary walk: {len(sup_starts)} starts "
          f"({sum(sup_starts.values())} B in non-BFS bytecode)")
    # Opaque-byte accounting: the G1 BFS decodes 204,350 B of the
    # 207,330-byte bytecode class; the remaining bytes are data or
    # dynamically-reached code the census never walks (its own
    # routing_or_typed_data sources). Those bytes are legitimately
    # opaque in the boundary model: instruction queries refuse there,
    # operand rows index there structurally (G1 census authority).
    opaque_bytes = 0
    for m in all_modules:
        spans = [(off, off + length) for off, length in m.boundaries]
        for s in m.segments:
            if s.get("kind") != "bytecode":
                continue
            pos = s["payload_offset"]
            end = pos + s["byte_count"]
            while pos < end:
                i = bisect.bisect_right(spans, (pos, 1 << 62)) - 1
                if i >= 0 and spans[i][0] <= pos < spans[i][1]:
                    pos = spans[i][1]
                else:
                    j = bisect.bisect_right(spans, (pos, 1 << 62))
                    nxt = spans[j][0] if j < len(spans) else end
                    opaque_bytes += min(nxt, end) - pos
                    pos = min(nxt, end)
    n_mart_exports = 0
    n_opaque_exports = 0
    n_offset_zero = 0
    static_data_spans = {}
    for m in all_modules:
        static_data_spans[m.id] = [
            (s["payload_offset"], s["payload_offset"] + s["byte_count"])
            for s in m.segments if s.get("kind") == "static-data"]
    for m in all_modules:
        for e in m.exports:
            e["opaque"] = False
            if e["boundary_kind"] == "offset-zero":
                if e["payload_offset"] != 0:
                    fail(f"{m.id}: offset-zero export {e['name']} at "
                         f"payload {e['payload_offset']} != 0")
                n_offset_zero += 1
            elif any(s <= e["payload_offset"] < t
                     for s, t in static_data_spans[m.id]):
                # Inside a mart/decor table: a typed data position (the
                # table start export or a trainerbattle inline slot that
                # the G1 symbol model labels), not an instruction
                # boundary (plan sec 7.5).
                n_mart_exports += 1
            elif e["payload_offset"] not in start_by_module[m.id]:
                # An unreferenced label on bytes the G1 walk never
                # decoded (unknown opcode / dead data after a
                # terminator). A valid export identity whose instruction
                # boundary is unproven - the G4 differential oracle
                # exercises these.
                e["opaque"] = True
                n_opaque_exports += 1
    del static_data_spans
    # Operand containment: inside one BFS instruction, or in a
    # static-data segment (typed-data operands), or in an opaque zone
    # (G1 census sources the BFS never decoded). Overlap between
    # operands stays forbidden structurally (sorted rows, below).
    spans_by_module = {}
    for m in all_modules:
        spans_by_module[m.id] = [(off, off + length)
                                 for off, length in m.boundaries]
    opaque_operands = 0
    for m in all_modules:
        spans = spans_by_module[m.id]
        sd = [(s["payload_offset"], s["payload_offset"] + s["byte_count"])
              for s in m.segments if s.get("kind") == "static-data"]
        for r in m.relocs:
            off = r["operand_payload_offset"]
            i = bisect.bisect_right(spans, (off, 1 << 62)) - 1
            if i >= 0 and spans[i][0] <= off \
               and off + r["operand_width"] <= spans[i][1]:
                continue
            if any(s <= off and off + r["operand_width"] <= t
                   for s, t in sd):
                continue
            opaque_operands += 1

    # Target sub-kinds + payload offsets + dynamic index (plan sec 18).
    move_id_set = set(move_id_of.values())
    subkinds = Counter()
    dyn = {}

    def enrich_row(r, enc, is_routing):
        cls = r["target_class"]
        key = r["target_resource_key"]
        tmod = mods_by_id.get(key)
        kind = None
        po = None
        label = None
        if cls == "SCRIPT_TARGET":
            if tmod is not None:
                po = payload_offset_of_opt(tmod, enc)
                kind = "SCRIPT_PAYLOAD" if po is not None else "SCRIPT_ROUTING"
            elif key.startswith("emerald:movement/bridge/"):
                kind = "SCRIPT_BRIDGE"
            else:
                fail(f"SCRIPT_TARGET key {key} is neither a module nor a bridge")
        elif cls == "TEXT_TARGET":
            sym = r["target_export"]
            if key in catalog_ids and key not in bundle_id_set:
                # A per-label catalog id (the 20 gift handoff labels and
                # any direct C-side label): resolves by key alone.
                kind, label = "TEXT_LABEL", key[len("emerald:text/"):]
            elif sym in text_symbol_info or \
                    QUALIFIED_TO_RECOMP_TEXT.get(sym) in text_symbol_info:
                # AC-3 renames (plan sec 7): the qualified symbol
                # resolves through the text family's RENAME_MAP to the
                # recomp-built bundle catalog name.
                lookup = sym if sym in text_symbol_info \
                    else QUALIFIED_TO_RECOMP_TEXT[sym]
                bid, canonical = text_symbol_info[lookup]
                if bid != key:
                    fail(f"TEXT_TARGET {sym}: bundle {bid} != reloc key {key}")
                kind, label = "TEXT_BUNDLE_MEMBER", canonical
            else:
                fail(f"TEXT_TARGET {key} ({sym}) is not a catalog label "
                     f"nor a bundle member")
        elif cls == "MOVEMENT_TARGET":
            if key.startswith("emerald:movement/bridge/"):
                kind = "MOVEMENT_BRIDGE"
            elif key in move_id_set:
                kind = "MOVEMENT_RESOURCE"
            else:
                fail(f"MOVEMENT_TARGET key {key} is neither a B resource "
                     f"nor a bridge")
        elif cls == "MART_TABLE_TARGET":
            if tmod is not None:
                po = payload_offset_of_opt(tmod, enc)
                if po is None:
                    fail(f"MART_TABLE_TARGET {key} {enc:#x} not in payload")
                kind = "MART_TABLE"
            elif key.startswith("emerald:script/routing/"):
                kind = "DISPATCH"
            elif key.startswith("emerald:text/braille/"):
                kind = "BRAILLE"
            else:
                fail(f"MART_TABLE_TARGET key {key} unresolved")
        elif cls == "RAM_DATA_TARGET":
            kind = "RAM_HOST"
        else:
            fail(f"unhandled target class {cls}")
        r["target_kind"] = kind
        r["target_payload_offset"] = po if po is not None else 0xFFFFFFFF
        if label is not None:
            r["canonical_label"] = label
        subkinds[kind] += 1
        dkey = (enc, cls)
        ident = (kind, key, r.get("target_export"), r.get("target_offset"),
                 r.get("target_payload_offset"), r.get("canonical_label"))
        if dkey in dyn and dyn[dkey] != ident:
            fail(f"dynamic target {dkey} ambiguous: {dyn[dkey]} vs {ident}")
        dyn[dkey] = ident

    for m in all_modules:
        for r in m.relocs:
            enrich_row(r, r["original_encoded_gba"], False)
    for r in routing_relocs:
        enrich_row(r, r["target_gba"], True)
        # Dispatch rows target module payload script labels OR the three
        # recomp-local movement bridges (a Route103 OnFrame row points
        # at the named bridge, plan sec 5).
        if r["target_class"] == "SCRIPT_TARGET" \
           and r["target_kind"] not in ("SCRIPT_PAYLOAD", "SCRIPT_ROUTING",
                                        "SCRIPT_BRIDGE"):
            fail(f"routing SCRIPT_TARGET {r['target_gba']:#x} "
                 f"kind {r['target_kind']} (not payload/routing/bridge)")
        if r["target_class"] == "MART_TABLE_TARGET" and r["target_kind"] != "DISPATCH":
            fail(f"routing MART_TABLE_TARGET {r['target_gba']:#x} not DISPATCH")
    for k, v in sorted(subkinds.items()):
        pin = TARGET_SUBKIND_PINS.get(k)
        if pin is not None and v != pin:
            fail(f"target sub-kind {k}: {v} != pin {pin}")
    print(f"  target sub-kinds: {dict(sorted(subkinds.items()))}")
    print(f"  boundaries: {boundary_total} instructions, "
          f"{n_offset_zero} offset-zero exports, "
          f"{n_mart_exports} mart-span export rows, "
          f"{n_opaque_exports} opaque export rows, "
          f"{opaque_bytes} opaque bytes, {opaque_operands} opaque operands")
    print(f"  dynamic targets: {len(dyn)} unique (gba, class) identities")
    del start_by_module, spans_by_module, text_symbol_info, move_id_set
    del bundle_id_set
    del mods_by_id, catalog_ids
    del enrich_row, bfs_starts, bfs_sizes, sup_starts, all_starts
    del rom_u8, rom_u32
    print(f"  rss {rss_mb()} MB (G3 enrichment: +{rss_mb() - phase_rss} MB)")

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
             map_script_refs, names_by_addr, dyn)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""R13-H2: additive ROM-backed battle resource emission.

Emits the 2,089 battle-family module resources from the qualified ROM and
the R13-H1 graph artifacts (resources/extraction/emerald/bpee01/battle/):

  - 2,081 payload modules as pack records; the 8 zero-width alias
    identities stay catalog/ownership/meta surface only (the pack model
    rejects encoded_length == 0 records -- G precedent, plan §26
    reconciliation: 20,988 + 2,081 = 23,069 pack entries).
  - manifest/catalog/ownership/inventory/bindings/relocations/exports/
    boundary_maps/arenas sidecars, meta/<family>/<module>.toml and
    data/<family>/<module>.bin payloads.

Every payload byte is an exact qualified-ROM slice -- no widening, no
patching, no rewriting to native pointers (R13-H2 §4). Ownership is
COMPILED_PENDING_MIGRATION (§3/§27): compiled battle/animation/AI/field-
effect content remains the live runtime source through H2 (§28). Schemas
47-51 are free (G occupies 1-46) and are assigned per family.

Regeneration must be a no-op diff; run with --check to verify.
"""

import argparse
import hashlib
import json
import pathlib
import struct
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[3]
H1_DIR = ROOT / "resources" / "extraction" / "emerald" / "bpee01" / "battle"
OUT = H1_DIR / "modules"
REFERENCE_ROOT = ROOT.parent / "pokeemerald-reference"
ROM_PATH = REFERENCE_ROOT / "pokeemerald.gba"
ELF_PATH = REFERENCE_ROOT / "pokeemerald.elf"

ROM_SHA1_PIN = "f3ae088181bf583e55daf962a92bb46f4f1d07b7"

# ---- R13-H1 qualified pins (docs/R13H1_BATTLE_SCRIPT_GRAPH_REPORT.md) ----
MODULE_COUNT_PIN = 2089
PAYLOAD_MODULE_COUNT_PIN = 2081          # 2,089 - 8 zero-width alias identities
BYTES_PIN = 90047
RELOCS_PIN = 7549
EXPORT_NAMES_PIN = 2109
ALIAS_NAMES_PIN = 20
NULL_SENTINELS_PIN = 65
ADDENDS_PIN = 3058                        # nonzero addends (H1 addend audit)
BINDINGS_PIN = 718
EWRAM_OCCURRENCES_PIN = 488
EWRAM_UNIQUE_BASES_PIN = 23
PACK_ENTRIES_PRE_H = 20988
PACK_ENTRIES_POST_H = PACK_ENTRIES_PRE_H + PAYLOAD_MODULE_COUNT_PIN  # 23069
# per-family byte/reloc pins (§1): battle 645/13592/1562; anim 658/63811/4231;
# battle-ai 553/9303/1222; contest-ai 165/2524/364; field-effect 68/817/170
FAMILY_PINS = {
    "battle":      dict(modules=645, bytes=13592, relocs=1562, schema=47),
    "battle_anim": dict(modules=658, bytes=63811, relocs=4231, schema=48),
    "battle_ai":   dict(modules=553, bytes=9303,  relocs=1222, schema=49),
    "contest_ai":  dict(modules=165, bytes=2524,  relocs=364,  schema=50),
    "field_effect": dict(modules=68, bytes=817,   relocs=170,  schema=51),
}
ROM_BASE = 0x08000000

# H1 arena hulls (h1_family_inventory arenas table), GBA space.
ARENAS = [
    dict(name="battle", start=0x82d86a8, end=0x82dbef5,
         sub=["battle_scripts_1", "field_effect_scripts", "battle_scripts_2"],
         holes=[(0x82db9d3, 1), (0x82dbd05, 3)]),
    dict(name="battle_anim", start=0x82c8d64, end=0x82d86a7,
         sub=["battle_anim_scripts"], holes=[]),
    dict(name="battle_ai", start=0x82dbef8, end=0x82de34f,
         sub=["battle_ai_scripts"], holes=[]),
    dict(name="contest_ai", start=0x82de350, end=0x82ded2c,
         sub=["contest_ai_scripts"], holes=[]),
    dict(name="field_effect", start=0x82db9d4, end=0x82dbd05,
         sub=["field_effect_scripts"], holes=[]),
]
GEN3_RESOURCE_ID_PREFIX = b"gen3-resource-id-v1\x00"


def fail(msg):
    print(f"H2 ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def sha256hex(b):
    return hashlib.sha256(b).hexdigest()


def derive_key(resource_id):
    """Gen3ResourceId_DeriveKey (src/gen3/resources/resource_id.c):
    SHA-256("gen3-resource-id-v1\\0" + canonical name)."""
    return hashlib.sha256(GEN3_RESOURCE_ID_PREFIX + resource_id.encode()).hexdigest()


def toml_escape(s):
    """Basic-string escape: backslashes (macro-arg references like
    `\\failPtr` appear in H1 operand strings) must be doubled."""
    return s.replace("\\", "\\\\")


def write_if(path, text, check):
    path = pathlib.Path(path)
    if check:
        if not path.exists():
            fail(f"--check: {path} missing")
        if path.read_text() != text:
            fail(f"--check: {path} differs on regeneration")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def write_bin_if(path, data, check):
    path = pathlib.Path(path)
    if check:
        if not path.exists():
            fail(f"--check: {path} missing")
        if path.read_bytes() != data:
            fail(f"--check: {path} differs on regeneration")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class Module:
    """One H1 module record, enriched with payload/exports/relocs/boundaries."""

    def __init__(self, rec, rom, elf, debug_ins, routing_rows):
        self.id = rec["key"]
        self.family = rec["family"]
        self.object = rec["object"]
        self.span_label = rec["span_label"]
        self.start = rec["start"]
        self.end = rec["end"]
        self.byte_count = rec["byte_count"]
        self.kind = rec["kind"]
        self.gba_base = rec["gba_base"]
        self.exports = list(rec["exports"])
        self.aliases = list(rec.get("aliases", []))
        self.schema = FAMILY_PINS[self.family]["schema"]
        # id_prefix = the id segment after "emerald:" (battle-script,
        # battle-anim-script, battle-ai, contest-ai, field-effect) — the
        # artifact directories mirror the H1-finalized ids verbatim.
        self.id_prefix = self.id[len("emerald:"):].split("/", 1)[0]
        # Filename = the id suffix VERBATIM: H1 ids keep internal
        # underscores (AI_PreferBatonPass_End -> prefer-baton-pass_end),
        # so hyphen-replacement would collide with prefer-baton-pass-end.
        self.name = self.id.split("/", 1)[1]
        self.payload = rom[self.gba_base - ROM_BASE:
                           self.gba_base - ROM_BASE + self.byte_count]
        self.key = derive_key(self.id)
        self.digest = sha256hex(self.payload)
        self.primary_symbol = self.span_label
        self.export_rows = []        # [{name, gba, provenance, payload_offset, boundary_kind}]
        self.reloc_rows = []         # dicts, operand_payload_offset sorted
        self.boundaries = []         # [(payload_offset, length)]
        self.boundary_kind = "bytecode"  # "bytecode" | "routing" | "data" | "empty"
        self._build_exports(elf)
        self._build_boundaries(debug_ins, routing_rows)

    # -- exports ---------------------------------------------------------
    def _build_exports(self, elf):
        for name in self.exports:
            # The ELF may hold several symbols with the same name (local
            # labels reused across objects); resolve by address containment
            # within this module's own span.
            cands = elf.by_name.get(name, [])
            if self.byte_count:
                addr = next((s["value"] for s in cands
                             if self.gba_base <= s["value"]
                             < self.gba_base + self.byte_count), None)
            else:
                addr = next((s["value"] for s in cands
                             if s["value"] == self.gba_base), None)
            if addr is None:
                fail(f"{self.id}: export {name} has no ELF symbol value in "
                     f"[{self.gba_base:#x},{self.gba_base + self.byte_count:#x})")
            if self.byte_count == 0:
                if addr != self.gba_base:
                    fail(f"{self.id}: zero-width export {name} at {addr:#x}, "
                         f"module base {self.gba_base:#x}")
                poff, bk = 0, "alias"
            else:
                if not (self.gba_base <= addr < self.gba_base + self.byte_count):
                    fail(f"{self.id}: export {name} at {addr:#x} outside "
                         f"[{self.gba_base:#x},{self.gba_base + self.byte_count:#x})")
                poff = addr - self.gba_base
                bk = "offset-zero" if poff == 0 else "interior"
            self.export_rows.append(dict(
                name=name, gba=addr, provenance=addr - ROM_BASE,
                payload_offset=poff, boundary_kind=bk))

    # -- instruction boundaries ------------------------------------------
    def _build_boundaries(self, debug_ins, routing_rows):
        if self.byte_count == 0:
            self.boundary_kind = "empty"
            return
        if self.kind == "script":
            ins = [i for i in debug_ins
                   if self.start <= i["offset"] < self.end]
            p = self.start
            for i in ins:
                if i["offset"] != p:
                    fail(f"{self.id}: instruction gap at object offset "
                         f"{p:#x} (next {i['offset']:#x})")
                p += i["size"]
            if p != self.end:
                fail(f"{self.id}: script instruction walk ends at {p:#x}, "
                     f"expected {self.end:#x}")
            self.boundaries = [(i["offset"] - self.start, i["size"])
                               for i in ins]
            self.boundary_kind = "bytecode"
        elif self.kind == "routing":
            rows = [r for r in routing_rows if r["table"] == self.span_label]
            p = self.start
            for r in rows:
                if r["offset"] != p:
                    fail(f"{self.id}: routing row gap at {p:#x} "
                         f"(next {r['offset']:#x})")
                p += r["width"]
            if p != self.end:
                fail(f"{self.id}: routing rows cover {p:#x} of {self.end:#x}")
            self.boundaries = [(r["offset"] - self.start, r["width"])
                               for r in rows]
            self.boundary_kind = "routing"
        elif self.kind == "data":
            # AI type lists etc.: deterministic boundary map = reloc operand
            # offsets + export offsets (no bytecode walk).
            self.boundary_kind = "data"
        else:
            fail(f"{self.id}: unknown module kind {self.kind!r}")


def load_inputs():
    rom = ROM_PATH.read_bytes()
    if hashlib.sha1(rom).hexdigest() != ROM_SHA1_PIN:
        fail(f"ROM SHA-1 does not match qualified pin {ROM_SHA1_PIN}")
    elf = None
    sys.path.insert(0, str(pathlib.Path(__file__).parent))
    from h1_elf import LinkedElf
    elf = LinkedElf(str(ELF_PATH))

    def tload(name):
        with open(H1_DIR / f"{name}.generated.toml", "rb") as f:
            return tomllib.load(f)

    mods = tload("h1_modules")["modules"]
    census = tload("h1_relocation_census")
    bindings = tload("h1_engine_bindings")["bindings"]
    addends = tload("h1_addend_audit")["addends"]
    routing_rows = tload("h1_routing_rows")["rows"]
    debug = json.load(open(H1_DIR / "_h1_debug.json", "r"))
    return rom, elf, mods, census, bindings, addends, routing_rows, debug


def build_modules(rom, elf, mods, debug, routing_rows):
    modules = []
    by_name = {}
    for rec in mods:
        m = Module(rec, rom, elf, debug[rec["object"]]["instructions"],
                   routing_rows)
        modules.append(m)
        for e in m.export_rows:
            by_name.setdefault(e["name"], m)
    modules.sort(key=lambda m: m.id)
    paths = [(m.id_prefix, m.name) for m in modules]
    if len(set(paths)) != len(paths):
        fail("module file path collision under id-suffix naming")
    return modules, by_name


def build_relocs(modules, census):
    """Attach census reloc rows to their owning module (source containment
    by object + object-relative offset); resolve targets by ADDRESS
    containment in the module table (names repeat across objects — e.g.
    AI_CheckBadMove exists in battle_ai AND contest_ai — so the byte-owning
    module is found from final_gba; alias labels resolve to the owning
    module's base; zero-width identities never own bytes)."""
    by_object = {}
    for o in census["object"]:
        by_object[o["name"]] = o
    payload_mods = [m for m in modules if m.byte_count]
    total = 0
    for m in modules:
        if m.byte_count == 0:
            continue
        obj = by_object[m.object]
        rows = [r for r in obj["relocs"] if m.start <= r["offset"] < m.end]
        for r in rows:
            poff = r["offset"] - m.start
            row = dict(
                module_key=m.id, operand_offset=poff,
                operand_width=r["width"], raw_encoded_value=r["stored"],
                target_class=r["target_class"], addend=r["addend"],
                target_symbol=r["target_symbol"], reloc_symbol=r["reloc_symbol"],
                root=r["root"], command=r["command"], operand=r["operand"],
                final_gba=r["final_gba"], span=r["span"])
            tm = next((x for x in payload_mods
                       if x.gba_base <= r["final_gba"]
                       < x.gba_base + x.byte_count), None)
            if tm is not None:
                row["target_resource_key"] = tm.id
                row["target_export"] = r["target_symbol"]
                row["target_offset"] = r["final_gba"] - tm.gba_base
                row["target_kind"] = tm.kind
                row["target_gba"] = r["final_gba"]
            else:
                row["target_resource_key"] = ""
                row["target_export"] = ""
                row["target_offset"] = None
                row["target_kind"] = "engine-" + r["target_class"]
                row["target_gba"] = r["final_gba"]
            m.reloc_rows.append(row)
            total += 1
    if total != RELOCS_PIN:
        fail(f"reloc rows {total} != pin {RELOCS_PIN}")
    return total


def verify_relocs(modules, census, rom):
    """H2-side re-proof of the four-way oracle (ROM == ELF == H1 == H2):
    for every one of the 7,549 sites the ROM word equals the record's
    final_gba (the H1 oracle relation); SCRIPT_TARGET finals land on a
    root export (interior == 0, §13); section-relative sites (stored != 0)
    satisfy source_base + stored == final_gba (the 2,778 addend rows);
    EWRAM sites stay in EWRAM and never resolve through the binding index
    as pointers (§17 keeps NULLs and IDs unresolved)."""
    rel_offs = {}
    for o in census["object"]:
        rel_offs[o["name"]] = set(r["offset"] for r in o["relocs"])
    bad = 0
    script_targets = 0
    section_relative = 0
    for m in modules:
        for r in m.reloc_rows:
            romv = struct.unpack_from(
                "<I", rom, (m.gba_base + r["operand_offset"]) - ROM_BASE)[0]
            if romv != r["final_gba"]:
                bad += 1
            if r["target_class"] == "SCRIPT_TARGET":
                script_targets += 1
                tm = next((x for x in modules
                           if x.id == r["target_resource_key"]), None)
                if tm is None:
                    fail(f"{r['module_key']}: SCRIPT_TARGET with no target module")
                # final address == module base + module-relative target offset
                if r["final_gba"] != tm.gba_base + r["target_offset"]:
                    fail(f"{r['module_key']}: target {r['target_symbol']} final "
                         f"{r['final_gba']:#x} != {tm.gba_base + r['target_offset']:#x}")
                # every target lands on a root export (interior == 0, §13)
                offs = {e["payload_offset"] for e in tm.export_rows}
                if r["target_offset"] not in offs:
                    fail(f"{r['module_key']}: interior target {r['target_symbol']} "
                         f"at module offset {r['target_offset']:#x} in {tm.id}")
                if r["addend"] != 0 or r["raw_encoded_value"] != 0:
                    section_relative += 1
                    base = next(o["base"] for o in census["object"]
                                if o["name"] == m.object)
                    if base + r["raw_encoded_value"] != r["final_gba"]:
                        fail(f"{r['module_key']}: section-relative site "
                             f"{r['raw_encoded_value']:#x} + base {base:#x} != "
                             f"final {r['final_gba']:#x}")
            elif r["target_class"] == "ENGINE_EWRAM_TARGET":
                if not (0x02000000 <= r["final_gba"] < 0x02040000):
                    fail(f"{r['module_key']}: EWRAM target outside EWRAM")
    if bad != 0:
        fail(f"oracle mismatches {bad} (must be 0)")
    if script_targets != 3798:
        fail(f"SCRIPT_TARGET rows {script_targets} != 3798")
    if section_relative != 2778:
        fail(f"section-relative rows {section_relative} != 2778")
    return bad


def scan_null_sentinels(modules, census, rom, debug, grammars):
    """§17: u32 pointer operand slots whose stored value is 0 and that carry
    no relocation (literal NULL constants, e.g. tryfaintmon NULL or
    playanimation arg=NULL slots). H1 counted 65; H2 re-derives the same
    deterministic scan and refuses to resolve any of them."""
    rel_offs = {}
    for o in census["object"]:
        rel_offs[o["name"]] = set(r["offset"] for r in o["relocs"])
    obj_fam = {}
    for m in modules:
        obj_fam[m.object] = m.family
    found = []
    for obj, dbg in debug.items():
        fam = obj_fam[obj]
        gram = grammars[fam]
        rels = rel_offs[obj]
        for ins in dbg["instructions"]:
            g = gram.get(ins["command"])
            if g is None:
                fail(f"{obj}: instruction {ins['command']} has no grammar entry")
            off = 1
            for o in g.get("operands", []):
                if o["width"] == 4 and o["name"].strip() != "argv":
                    aoff = ins["offset"] + off
                    if aoff not in rels:
                        v = struct.unpack_from(
                            "<I", rom, (dbg["base"] + aoff) - ROM_BASE)[0]
                        if v == 0:
                            m = next((x for x in modules
                                      if x.object == obj
                                      and x.start <= aoff < x.end), None)
                            if m is None:
                                fail(f"{obj}: NULL slot at {aoff:#x} outside "
                                     "all modules")
                            found.append(dict(module_key=m.id,
                                              operand_offset=aoff - m.start,
                                              command=ins["command"]))
                off += o["width"]
    if len(found) != NULL_SENTINELS_PIN:
        fail(f"NULL sentinels {len(found)} != pin {NULL_SENTINELS_PIN}")
    return found


def verify_addends(modules, addends):
    """Every one of the 3,058 nonzero addends in the H1 audit must appear on
    exactly one reloc row with the same (family, object offset, class); every
    reloc row with a nonzero addend must be in the audit."""
    audited = {}
    for a in addends:
        audited.setdefault((a["family"], a["offset"], a["class"]), []).append(a)
    seen = set()
    for m in modules:
        for r in m.reloc_rows:
            if r["addend"] == 0:
                continue
            key = (m.family, m.start + r["operand_offset"], r["target_class"])
            if key in audited:
                seen.add(key)
            else:
                fail(f"{r['module_key']}: nonzero addend {r['addend']} at "
                     f"{key[1]:#x} not in the H1 addend audit")
    if len(seen) != ADDENDS_PIN:
        fail(f"addend-bearing rows verified {len(seen)} != pin {ADDENDS_PIN}")
    return len(seen)


def render_manifest(rom, modules):
    rom_sha1 = hashlib.sha1(rom).hexdigest()
    rom_sha256 = hashlib.sha256(rom).hexdigest()
    lines = [
        "# Deterministic extraction manifest - generated by",
        "# tools/gen3_resources/battle_family/h2_generate.py.",
        "# Do not edit by hand; regenerate with h2_generate.py --check.",
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
    n = 0
    for m in modules:
        if not m.payload:
            # zero-width alias identity: no G payload; the pack model rejects
            # encoded_length == 0 records, so it stays catalog/ownership/meta
            # surface (plan §26 reconciliation: 2,081 embedded records).
            continue
        n += 1
        lines += [
            "[[records]]",
            f"id = \"{m.id}\"",
            f"key = \"{m.key}\"",
            "type = \"structured-data\"",
            f"schema = {m.schema}",
            "bundle = true",
            f"source_artifact = \"resources/extraction/emerald/bpee01/battle/modules/data/{m.id_prefix}/{m.name}.bin\"",
            f"symbol = \"{m.primary_symbol}\"",
            "rom_offset = 0",
            f"encoded_length = {m.byte_count}",
            f"decoded_length = {m.byte_count}",
            "source_encoding = \"raw\"",
            f"source_encoded_sha256 = \"{m.digest}\"",
            f"canonical_decoded_sha256 = \"{m.digest}\"",
            "",
        ]
    if n != PAYLOAD_MODULE_COUNT_PIN:
        fail(f"manifest records {n} != pin {PAYLOAD_MODULE_COUNT_PIN}")
    return "\n".join(lines)


def render_catalog(modules):
    lines = [
        "# Generated by tools/gen3_resources/battle_family/h2_generate.py.",
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
    for m in modules:
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
        "# Generated by tools/gen3_resources/battle_family/h2_generate.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Native asset-ownership declaration. Each [[resources]] block",
        "# records the canonical id, key (artifact sha256 - raw encoding,",
        "# so encoded == decoded), legacy compiled symbol, source artifact,",
        "# per-target ownership state and source hashes.",
        "# R13-H2 ownership: every battle/animation/AI/field-effect module is",
        "# COMPILED_PENDING_MIGRATION - the compiled ROM payloads stay the",
        "# live runtime source through H2 (§28); nothing is staged live, no",
        "# State-v5 range is registered (§19/§29). The gba target keeps the",
        "# qualified ROM bytes as the canonical representation.",
        "",
        "ownership_version = 1",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        "",
    ]
    for m in modules:
        lines += [
            "[[resources]]",
            f"id = \"{m.id}\"",
            f"key = \"{m.digest}\"",
            f"legacy_symbol = \"{m.primary_symbol}\"",
            "type = \"structured-data\"",
            f"source_artifact = \"resources/extraction/emerald/bpee01/battle/modules/data/{m.id_prefix}/{m.name}.bin\"",
            f"encoded_length = {m.byte_count}",
            f"decoded_length = {m.byte_count}",
            "source_encoding = \"raw\"",
            f"source_encoded_sha256 = \"{m.digest}\"",
            f"canonical_decoded_sha256 = \"{m.digest}\"",
            "ownership_state = \"COMPILED_PENDING_MIGRATION\"",
            "",
            "[resources.targets]",
            "native = \"COMPILED_PENDING_MIGRATION\"",
            "gba = \"COMPILED\"",
            "",
        ]
    return "\n".join(lines)


def render_inventory(modules, reloc_total, nulls, addends_verified, bindings,
                     ewram_occurrences, ewram_unique_bases):
    for fam, pin in sorted(FAMILY_PINS.items()):
        nmod = sum(1 for m in modules if m.family == fam)
        nbytes = sum(m.byte_count for m in modules if m.family == fam)
        nrel = sum(len(m.reloc_rows) for m in modules if m.family == fam)
        if (nmod, nbytes, nrel) != (pin["modules"], pin["bytes"], pin["relocs"]):
            fail(f"family {fam}: ({nmod},{nbytes},{nrel}) != pin "
                 f"({pin['modules']},{pin['bytes']},{pin['relocs']})")
    # scalars FIRST (a key after [[families]] would bind to the last element)
    lines = [
        "# Generated by tools/gen3_resources/battle_family/h2_generate.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# R13-H2 battle-family resource inventory. 2,089 modules = 645",
        "# battle + 658 battle-anim + 553 battle-ai + 165 contest-ai + 68",
        "# field-effect; canonical bytes 90,047 B (qualified pin, plan",
        "# §26). Relocation graph 7,549; 8 zero-width alias identities are",
        "# catalog/ownership/meta surface only, so the pack embeds 2,081",
        "# payload records: 20,988 + 2,081 = 23,069 entries (reconciled",
        "# from the plan's 23,077 - the pack model rejects zero-length",
        "# records, G precedent for routing-only identities).",
        "",
        "inventory_version = 1",
        "family = \"battle\"",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        "module_count = 2089",
        "payload_modules = 2081",
        "canonical_bytes = 90047",
        "relocs = 7549",
        f"null_sentinels = {nulls}",
        f"addends_verified = {addends_verified}",
        f"semantic_bindings = {len(bindings)}",
        f"ewram_occurrences = {ewram_occurrences}",
        f"ewram_unique_bases = {ewram_unique_bases}",
        f"pack_entries_pre_h = {PACK_ENTRIES_PRE_H}",
        f"pack_entries_post_h = {PACK_ENTRIES_POST_H}",
        f"export_names = {sum(len(m.export_rows) for m in modules)}",
        f"alias_names = {sum(len(m.aliases) for m in modules)}",
        "",
    ]
    for fam, pin in sorted(FAMILY_PINS.items()):
        fam_mods = [m for m in modules if m.family == fam]
        payload_mods = [m for m in fam_mods if m.byte_count]
        lines += [
            "[[families]]",
            f"name = \"{fam}\"",
            f"id_prefix = \"{fam_mods[0].id_prefix}\"",
            f"module_count = {len(fam_mods)}",
            f"payload_modules = {len(payload_mods)}",
            f"empty_modules = {len(fam_mods) - len(payload_mods)}",
            f"schema = {pin['schema']}",
            f"canonical_bytes = {sum(m.byte_count for m in fam_mods)}",
            f"relocs = {sum(len(m.reloc_rows) for m in fam_mods)}",
            f"routing_modules = {sum(1 for m in fam_mods if m.kind == 'routing')}",
            f"script_modules = {sum(1 for m in fam_mods if m.kind == 'script')}",
            f"data_modules = {sum(1 for m in fam_mods if m.kind == 'data')}",
            f"export_names = {sum(len(m.export_rows) for m in fam_mods)}",
            "",
        ]
    return "\n".join(lines)


def render_bindings(modules, h1_bindings):
    lines = [
        "# Generated by tools/gen3_resources/battle_family/h2_generate.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Semantic extraction bindings for the battle family. Primary",
        "# bindings mirror the G script-family shape (artifact IS the",
        "# canonical payload; NO ROM offsets here - the meta sidecars carry",
        "# segment provenance). Semantic binding tables A-F follow the",
        "# R13-H1 engine-binding census (718 unique bindings, 0 anonymous):",
        "#   A battle EWRAM fields (50 unique / 488 occurrences)",
        "#   B battle engine tables (50 unique / 89 occurrences)",
        "#   C animation sprite templates (327 unique / 2,108 occurrences)",
        "#   D animation callbacks (213 unique / 963 occurrences)",
        "#   E field-effect graphics (11 unique / 36 occurrences)",
        "#   F field-effect callbacks (67 unique / 67 occurrences)",
        "# No host pointers appear in canonical bytes; these tables are",
        "# semantic identity only (R13-H2 §15).",
        "",
        "bindings_version = 1",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        "",
    ]
    for m in modules:
        lines += [
            "[[bindings]]",
            f"id = \"{m.id}\"",
            f"symbol = \"{m.primary_symbol}\"",
            f"source_artifact = \"resources/extraction/emerald/bpee01/battle/modules/data/{m.id_prefix}/{m.name}.bin\"",
            "source_encoding = \"raw\"",
            "canonical_representation = \"gba-bytes\"",
            f"expected_decoded_size = {m.byte_count}",
            "",
        ]
    # semantic tables A-F, sorted deterministically
    letters = {"ENGINE_EWRAM_TARGET": "A", "ENGINE_TABLE_TARGET": "B",
               "ENGINE_SPRITE_TEMPLATE_TARGET": "C", "ENGINE_GFX_TARGET": "E"}
    for b in sorted(h1_bindings, key=lambda x: (x["semantic_family"],
                                                x["gba_base_symbol"])):
        if b["semantic_family"] == "ENGINE_CALLBACK":
            letter = b.get("_letter", "?")
        else:
            letter = letters[b["semantic_family"]]
        lines += [
            "[[semantic_tables]]",
            f"table = \"{letter}\"",
            f"semantic_family = \"{b['semantic_family']}\"",
            f"gba_base_symbol = \"{b['gba_base_symbol']}\"",
            f"encoded_gba_value = {b['encoded_gba_value']:#x}",
            f"addend = {b['addend']}",
            f"native_binding_symbol = \"{b['native_binding_symbol']}\"",
            f"allowed_offset = {b['allowed_offset']}",
            f"width_or_type = {str(b['width_or_type'])}",
            f"access_widths = {str(b['access_widths'])}",
            f"mutability = \"{b['mutability']}\"",
            f"occurrences = {b['occurrences']}",
            f"section = \"{b['section']}\"",
            "",
        ]
    return "\n".join(lines)


def letter_for_callbacks(bindings, census):
    """Tag ENGINE_CALLBACK bindings D (battle_anim source) / F (field_effect
    source) by the census rows that reference them."""
    src = {}
    for o in census["object"]:
        for r in o["relocs"]:
            if r["target_class"] == "ENGINE_CALLBACK":
                src.setdefault(r["target_symbol"], set()).add(o["name"])
    for b in bindings:
        if b["semantic_family"] != "ENGINE_CALLBACK":
            continue
        sym = b.get("gba_base_symbol")
        fam = src.get(sym, set())
        if fam == {"battle_anim_scripts"}:
            b["_letter"] = "D"
        elif fam == {"field_effect_scripts"}:
            b["_letter"] = "F"
        else:
            fail(f"callback binding {sym}: ambiguous source families {fam}")
    d = sum(b["occurrences"] for b in bindings if b.get("_letter") == "D")
    f = sum(b["occurrences"] for b in bindings if b.get("_letter") == "F")
    if d != 963 or f != 67:
        fail(f"callback letter split D={d} F={f} != 963/67")
    return bindings


def render_relocations(modules):
    lines = [
        "# Generated by tools/gen3_resources/battle_family/h2_generate.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# R13-H2 relocation sidecar: all 7,549 rows with source module,",
        "# operand offset (module-relative), raw encoded value, class,",
        "# addend, target identity (resource key / export / module offset),",
        "# owner and boundary kind. Row order is deterministic: sorted by",
        "# (module_key, operand_offset). NULL sentinels (65) are preserved",
        "# unresolved. Raw operand == final_gba is re-verified per row.",
        "",
        "relocations_version = 1",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        f"total_relocs = {RELOCS_PIN}",
        "",
    ]
    rows = []
    for m in modules:
        for r in m.reloc_rows:
            rows.append(r)
    rows.sort(key=lambda r: (r["module_key"], r["operand_offset"]))
    for r in rows:
        lines += [
            "[[relocs]]",
            f"module_key = \"{r['module_key']}\"",
            f"operand_offset = {r['operand_offset']}",
            f"operand_width = {r['operand_width']}",
            f"raw_encoded_value = {r['raw_encoded_value']:#x}",
            f"target_class = \"{r['target_class']}\"",
            f"addend = {r['addend']}",
            f"target_symbol = \"{toml_escape(r['target_symbol'])}\"",
            f"reloc_symbol = \"{toml_escape(r['reloc_symbol'])}\"",
            f"target_resource_key = \"{r['target_resource_key']}\"",
            f"target_export = \"{toml_escape(r['target_export'])}\"",
        ]
        if r["target_offset"] is not None:
            lines.append(f"target_offset = {r['target_offset']}")
        lines += [
            f"target_kind = \"{r['target_kind']}\"",
            f"final_gba = {r['final_gba']:#x}",
            f"root = {str(r['root']).lower()}",
            f"command = \"{toml_escape(r['command'])}\"",
            f"operand = \"{toml_escape(r['operand'])}\"",
            "",
        ]
    return "\n".join(lines)


def render_null_sentinels(nulls):
    lines = [
        "# Generated by tools/gen3_resources/battle_family/h2_generate.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# R13-H2 NULL sentinel preservation (§17): u32 pointer operand",
        "# slots whose stored value is 0 and that carry no relocation",
        "# (literal NULL constants, e.g. tryfaintmon NULL or playanimation",
        "# arg=NULL slots). All 65 are preserved unresolved; nothing",
        "# resolves a NULL through the binding index, and an unexpected",
        "# NULL refusals staging.",
        "",
        "null_sentinels_version = 1",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        f"total_null_sentinels = {len(nulls)}",
        "",
    ]
    for n in sorted(nulls, key=lambda x: (x["module_key"], x["operand_offset"])):
        lines += [
            "[[null_sentinels]]",
            f"module_key = \"{n['module_key']}\"",
            f"operand_offset = {n['operand_offset']}",
            f"command = \"{n['command']}\"",
            "",
        ]
    return "\n".join(lines)


def render_exports(modules):
    lines = [
        "# Generated by tools/gen3_resources/battle_family/h2_generate.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# R13-H2 export/alias table: all 2,109 export names across the",
        "# 2,089 modules (20 alias names, 14 modules with aliases). Every",
        "# target lands on a root export; interior targets = 0 (§13).",
        "",
        "exports_version = 1",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        f"export_names = {sum(len(m.export_rows) for m in modules)}",
        f"alias_names = {sum(len(m.aliases) for m in modules)}",
        "",
    ]
    rows = []
    for m in modules:
        for e in m.export_rows:
            rows.append((e["name"], m, e))
    rows.sort(key=lambda x: x[0])
    for name, m, e in rows:
        lines += [
            "[[exports]]",
            f"name = \"{name}\"",
            f"module_key = \"{m.id}\"",
            f"original_gba_address = {e['gba']:#x}",
            f"provenance_offset = {e['provenance']}",
            f"payload_offset = {e['payload_offset']}",
            f"boundary_kind = \"{e['boundary_kind']}\"",
            "root = true",
            "",
        ]
    return "\n".join(lines)


def render_boundary_maps(modules):
    lines = [
        "# Generated by tools/gen3_resources/battle_family/h2_generate.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# R13-H2 deterministic instruction-boundary maps (§14): per module,",
        "# the exact (payload_offset, length) walk. bytecode = source-level",
        "# command walk (H1 debug instruction list); routing = table row",
        "# strides; data = reloc/export offsets (no bytecode walk); empty =",
        "# zero-width alias identity. Malformed/out-of-range target fails",
        "# the generator, which the staging fault matrix exercises.",
        "",
        "boundary_maps_version = 1",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        "",
    ]
    for m in modules:
        lines += [
            "[[maps]]",
            f"module_key = \"{m.id}\"",
            f"kind = \"{m.boundary_kind}\"",
            f"boundary_count = {len(m.boundaries)}",
            "",
        ]
        for off, length in m.boundaries:
            lines += [
                "[[maps.boundaries]]",
                f"payload_offset = {off}",
                f"length = {length}",
                "",
            ]
    return "\n".join(lines)


def render_arenas():
    lines = [
        "# Generated by tools/gen3_resources/battle_family/h2_generate.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# R13-H2 family arenas (§19/§20): the five H1 hull ranges in GBA",
        "# space. The battle hull (14,413 B) contains battle_scripts_1 + a",
        "# 1-byte hole + field_effect + a 3-byte hole + battle_scripts_2;",
        "# field_effect is also its own arena. Reverse containment is",
        "# module-exact: hull holes refuse, module spans are unique and",
        "# non-overlapping; different host bases are allowed.",
        "",
        "arenas_version = 1",
        "game = \"emerald\"",
        "rom_profile = \"bpee01-rev0\"",
        "",
    ]
    for a in ARENAS:
        lines += [
            "[[arenas]]",
            f"name = \"{a['name']}\"",
            f"gba_start = {a['start']:#x}",
            f"gba_end = {a['end']:#x}",
            f"hull_bytes = {a['end'] - a['start']}",
            "",
        ]
        for sub in a["sub"]:
            lines += [
                "[[arenas.sub_arenas]]",
                f"object = \"{sub}\"",
                "",
            ]
        for start, length in a["holes"]:
            lines += [
                "[[arenas.holes]]",
                f"gba_start = {start:#x}",
                f"length = {length}",
                "",
            ]
    return "\n".join(lines)


def render_meta(m):
    lines = [
        "# Generated by tools/gen3_resources/battle_family/h2_generate.py.",
        "# Do not edit by hand; regeneration must be a no-op diff.",
        "",
        "# R13-H2 canonical pack record: exact qualified-ROM bytes in",
        "# deterministic segment order, never rewritten to native pointers.",
        "# Segments/exports/relocs carry GBA provenance only.",
        "",
        f"module_key = \"{m.id}\"",
        f"key = \"{m.key}\"",
        f"schema = {m.schema}",
        f"primary_symbol = \"{m.primary_symbol}\"",
        f"module_digest = \"{m.digest}\"",
        f"region_gba_start = {m.gba_base:#x}",
        f"region_byte_count = {m.byte_count}",
        f"bytecode_bytes = {m.byte_count if m.kind in ('script', 'routing') else 0}",
        f"static_data_bytes = {m.byte_count if m.kind == 'data' else 0}",
        "segment_count = 1" if m.byte_count else "segment_count = 0",
        f"export_count = {len(m.export_rows)}",
        f"reloc_count = {len(m.reloc_rows)}",
        f"instruction_count = {len(m.boundaries)}",
        "",
    ]
    if m.byte_count:
        lines += [
            "[[segments]]",
            f"kind = \"{m.kind}\"",
            f"original_gba_start = {m.gba_base:#x}",
            f"byte_count = {m.byte_count}",
            "payload_offset = 0",
            "",
        ]
    for e in m.export_rows:
        lines += [
            "[[exports]]",
            f"name = \"{e['name']}\"",
            f"original_gba_address = {e['gba']:#x}",
            f"provenance_offset = {e['provenance']}",
            f"payload_offset = {e['payload_offset']}",
            f"boundary_kind = \"{e['boundary_kind']}\"",
            "aliases = []",
            "",
        ]
    for r in sorted(m.reloc_rows, key=lambda r: r["operand_offset"]):
        lines += [
            "[[relocs]]",
            f"operand_payload_offset = {r['operand_offset']}",
            f"operand_width = {r['operand_width']}",
            f"original_encoded_gba = {r['raw_encoded_value']:#x}",
            f"target_class = \"{r['target_class']}\"",
            f"target_resource_key = \"{r['target_resource_key']}\"",
        ]
        if r["target_export"]:
            lines.append(f"target_export = \"{r['target_export']}\"")
        if r["target_offset"] is not None:
            lines.append(f"target_offset = {r['target_offset']}")
        lines += [
            f"target_kind = \"{r['target_kind']}\"",
            f"target_payload_offset = {r['target_offset'] if r['target_offset'] is not None else 0}",
            "runtime_resolution_required = true",
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


def emit_all(args, rom, modules, h1_bindings, reloc_total, nulls,
             addends_verified, ewram_occurrences, ewram_unique_bases):
    write_if(OUT / "manifest.production.toml",
             render_manifest(rom, modules), args.check)
    write_if(OUT / "catalog.generated.toml",
             render_catalog(modules), args.check)
    write_if(OUT / "ownership.generated.toml",
             render_ownership(modules), args.check)
    write_if(OUT / "inventory.generated.toml",
             render_inventory(modules, reloc_total, len(nulls),
                              addends_verified, h1_bindings,
                              ewram_occurrences, ewram_unique_bases),
             args.check)
    write_if(OUT / "bindings.generated.toml",
             render_bindings(modules, h1_bindings), args.check)
    write_if(OUT / "relocations.generated.toml",
             render_relocations(modules), args.check)
    write_if(OUT / "null_sentinels.generated.toml",
             render_null_sentinels(nulls), args.check)
    write_if(OUT / "exports.generated.toml",
             render_exports(modules), args.check)
    write_if(OUT / "boundary_maps.generated.toml",
             render_boundary_maps(modules), args.check)
    write_if(OUT / "arenas.generated.toml",
             render_arenas(), args.check)
    for m in modules:
        write_if(OUT / "meta" / m.id_prefix / f"{m.name}.toml",
                 render_meta(m), args.check)
        if m.byte_count:
            write_bin_if(OUT / "data" / m.id_prefix / f"{m.name}.bin",
                         m.payload, args.check)


def ewram_stats(modules, bindings):
    """§7: 488 occurrences across the census; 23 unique EWRAM bases = the
    unique gba_base_symbol of the ENGINE_EWRAM_TARGET semantic bindings."""
    occ = sum(1 for m in modules
              for r in m.reloc_rows
              if r["target_class"] == "ENGINE_EWRAM_TARGET")
    bases = {b["gba_base_symbol"] for b in bindings
             if b["semantic_family"] == "ENGINE_EWRAM_TARGET"}
    return occ, len(bases)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="verify regeneration is a no-op diff")
    args = ap.parse_args()

    rom, elf, mods, census, bindings, addends, routing_rows, debug = load_inputs()
    modules, by_name = build_modules(rom, elf, mods, debug, routing_rows)

    if len(modules) != MODULE_COUNT_PIN:
        fail(f"module count {len(modules)} != pin {MODULE_COUNT_PIN}")
    total_bytes = sum(m.byte_count for m in modules)
    if total_bytes != BYTES_PIN:
        fail(f"canonical bytes {total_bytes} != pin {BYTES_PIN}")
    payload_mods = sum(1 for m in modules if m.byte_count)
    if payload_mods != PAYLOAD_MODULE_COUNT_PIN:
        fail(f"payload modules {payload_mods} != pin {PAYLOAD_MODULE_COUNT_PIN}")
    total_exports = sum(len(m.export_rows) for m in modules)
    if total_exports != EXPORT_NAMES_PIN:
        fail(f"export names {total_exports} != pin {EXPORT_NAMES_PIN}")
    alias_names = sum(len(m.aliases) for m in modules)
    if alias_names != ALIAS_NAMES_PIN:
        fail(f"alias names {alias_names} != pin {ALIAS_NAMES_PIN}")

    reloc_total = build_relocs(modules, census)
    verify_relocs(modules, census, rom)
    bindings = letter_for_callbacks(bindings, census)
    if len(bindings) != BINDINGS_PIN:
        fail(f"semantic bindings {len(bindings)} != pin {BINDINGS_PIN}")
    addends_verified = verify_addends(modules, addends)
    occ, bases = ewram_stats(modules, bindings)
    if (occ, bases) != (EWRAM_OCCURRENCES_PIN, EWRAM_UNIQUE_BASES_PIN):
        fail(f"EWRAM ({occ} occurrences, {bases} unique bases) != "
             f"({EWRAM_OCCURRENCES_PIN}, {EWRAM_UNIQUE_BASES_PIN})")
    grammars = {}
    for fam, fn in [("battle", "h1_grammar_battle"),
                    ("battle_anim", "h1_grammar_battle_anim"),
                    ("battle_ai", "h1_grammar_battle_ai"),
                    ("contest_ai", "h1_grammar_contest_ai"),
                    ("field_effect", "h1_grammar_field_effect")]:
        g = tomllib.load(open(H1_DIR / f"{fn}.generated.toml", "rb"))
        grammars[fam] = {op["command"]: op for op in g["opcodes"]}
    nulls = scan_null_sentinels(modules, census, rom, debug, grammars)

    emit_all(args, rom, modules, bindings, reloc_total, nulls,
             addends_verified, occ, bases)

    print(f"H2 OK: {len(modules)} modules / {payload_mods} payload / "
          f"{total_bytes} B / {reloc_total} relocs / {len(nulls)} NULLs / "
          f"{addends_verified} addends / {total_exports} exports / "
          f"{len(bindings)} bindings / pack {PACK_ENTRIES_POST_H} entries")


if __name__ == "__main__":
    main()

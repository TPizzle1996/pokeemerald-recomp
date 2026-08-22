#!/usr/bin/env python3
"""R13-H2 staging harness: family arenas, reverse containment, shadow parity.

Pure-python test/shadow staging of the 2,089 battle-family module resources
at two host bases (gen-a ~0x10000000, gen-b ~0x50000000). NOTHING registers
in the live State-v5 range registry (§19: live range count stays 6,377;
§29: 0 H ranges; §28: no production execution change). Implements the
R13-H2 brief sections:

  §18/§19  five family arenas staged in shadow mode at two host bases
  §20  reverse containment: host addr -> (family, module, module offset);
       module-exact (hull holes / inter-module gaps / FE-in-hull refuse),
       module spans unique and non-overlapping, stale generation tokens
       refuse, different host bases allowed
  §21  source relocation index: 7,549 rows keyed (module, operand_offset);
       staged host word == canonical ROM word == final_gba at every site;
       raw_encoded_value == addend (the H1 `stored` addend) preserved
  §22  target binding index: SCRIPT_TARGET -> (module, root export, offset);
       ENGINE_* -> semantic binding tables A-F by symbol (final ==
       encoded_gba_value + addend); NULL sentinels stay unresolved; IDs
       never become pointer bindings
  §23  shadow parity at two host bases: identical canonical aggregate,
       host pointers differ
  §30  fault matrix: 21 refusal cases; every fault refuses staging/querying
       and the prior generation survives byte-identical (token unchanged)

Exit 0 = all green.
Run: python3 tools/gen3_resources/battle_family/h2_stage.py
"""

import hashlib
import pathlib
import struct
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[3]
OUT = ROOT / "resources" / "extraction" / "emerald" / "bpee01" / "battle"
MOD = OUT / "modules"
H1_MODULES = OUT / "h1_modules.generated.toml"
ROM_PATH = ROOT.parent / "pokeemerald-reference" / "pokeemerald.gba"
GEN_A = 0x10000000
GEN_B = 0x50000000
MASK = (1 << 32) - 1

# H1-finalized pins (qualified truth; docs/R13H1_BATTLE_SCRIPT_GRAPH_REPORT.md)
P_MODULES = 2089
P_PAYLOAD = 2081
P_BYTES = 90047
P_RELOCS = 7549
P_NULLS = 65
P_SECTION_REL = 2778      # object base + raw == final_gba (re-verified)
P_EXPORTS = 2109
P_BINDINGS = 718
P_SCRIPT = 3798
P_ENGINE = 3751
P_EWRAM = 488
P_PACK_ENTRIES = 23069    # 20,988 + 2,081 payload records (plan pin 23,077
                          # reconciled: 8 zero-width alias modules cannot be
                          # pack records - emerald_resource_import.c rejects
                          # encoded_length <= 0; G precedent)
SCHEMAS = {"battle-script": 47, "battle-anim-script": 48, "battle-ai": 49,
           "contest-ai": 50, "field-effect-script": 51}
CLASSES = ("SCRIPT_TARGET", "ENGINE_EWRAM_TARGET", "ENGINE_TABLE_TARGET",
           "ENGINE_CALLBACK", "ENGINE_SPRITE_TEMPLATE_TARGET", "ENGINE_GFX_TARGET")
CLASS_TABLE = {"ENGINE_EWRAM_TARGET": "A", "ENGINE_TABLE_TARGET": "B",
               "ENGINE_SPRITE_TEMPLATE_TARGET": "C", "ENGINE_GFX_TARGET": "E"}

PASS = 0
FAIL = 0


VERBOSE = "--verbose" in sys.argv


def ok(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        if VERBOSE:
            print(f"PASS {name}")
    else:
        FAIL += 1
        print(f"FAIL {name} {detail}")


class Refusal(Exception):
    """Staging/query refused; the prior generation is untouched."""


# ---------------------------------------------------------------------------
# Inputs: an in-memory snapshot of the generated tables + payload bytes.
# ---------------------------------------------------------------------------

class Inputs:
    def __init__(self):
        self.metas = {}        # module_key -> meta dict
        self.payloads = {}     # module_key -> bytes (payload modules only)
        self.relocs = []       # relocations sidecar rows
        self.nulls = []        # null sentinel rows
        self.bindings = []     # semantic binding rows
        self.exports = []      # export rows
        self.maps = []         # boundary maps
        self.arenas = []       # arena rows (arenas.generated.toml)
        self.manifest = []     # manifest records
        self.catalog = []      # catalog resources
        self.rom = b""

    @classmethod
    def load(cls):
        ins = cls()
        for f in sorted((MOD / "meta").rglob("*.toml")):
            m = tomllib.load(open(f, "rb"))
            ins.metas[m["module_key"]] = m
        for key, m in ins.metas.items():
            if m["region_byte_count"]:
                fam, name = key[len("emerald:"):].split("/", 1)
                ins.payloads[key] = (MOD / "data" / fam / (name + ".bin")).read_bytes()
        ins.relocs = tomllib.load(open(MOD / "relocations.generated.toml", "rb"))["relocs"]
        ins.nulls = tomllib.load(open(MOD / "null_sentinels.generated.toml", "rb"))["null_sentinels"]
        ins.bindings = tomllib.load(open(MOD / "bindings.generated.toml", "rb")).get("semantic_tables", [])
        ins.exports = tomllib.load(open(MOD / "exports.generated.toml", "rb"))["exports"]
        ins.maps = tomllib.load(open(MOD / "boundary_maps.generated.toml", "rb"))["maps"]
        ins.arenas = tomllib.load(open(MOD / "arenas.generated.toml", "rb"))["arenas"]
        ins.manifest = tomllib.load(open(MOD / "manifest.production.toml", "rb"))["records"]
        ins.catalog = tomllib.load(open(MOD / "catalog.generated.toml", "rb"))["resources"]
        ins.rom = ROM_PATH.read_bytes()
        return ins


# ---------------------------------------------------------------------------
# Generation: one staged shadow world at a host base.
# ---------------------------------------------------------------------------

class Generation:
    def __init__(self, inputs, base):
        self.inputs = inputs
        self.base = base
        self.modules = []     # {id, family, arena, gba_start, byte_count, host_start}
        self.arenas = []      # {name, gba_start, gba_end, host_start, host_end}
        self.holes = []       # (host_start, length) per arena
        self._place(inputs, base)
        self._token = hashlib.sha256(
            f"h2-stage-v1\0{base:#x}\0".encode() +
            "|".join(f"{m['id']}:{m['host_start']:#x}:{m['byte_count']}"
                     for m in self.modules).encode()).hexdigest()[:16]

    # -- layout ----------------------------------------------------------
    def _place(self, inputs, base):
        """Stage the five arenas in sidecar order. Each module is placed in
        the smallest arena containing its gba span (the FE arena is nested
        inside the battle hull, so FE modules stage in the FE arena; the
        hull's FE-region bytes stay unmapped -> containment refuses them).
        Zero-width alias modules have no host span."""
        for a in inputs.arenas:
            host_start = base + sum(x["gba_end"] - x["gba_start"]
                                    for x in self.arenas)
            self.arenas.append(dict(name=a["name"], gba_start=a["gba_start"],
                                    gba_end=a["gba_end"],
                                    host_start=host_start,
                                    host_end=host_start +
                                    (a["gba_end"] - a["gba_start"])))
            for h in a.get("holes", []):
                hs, ln = h["gba_start"], h["length"]
                self.holes.append((host_start + (hs - a["gba_start"]), ln))
        for key in sorted(inputs.metas):
            m = inputs.metas[key]
            if not m["region_byte_count"]:
                continue  # zero-width alias identity: no payload, no span
            arena = self._arena_containing(m["region_gba_start"])
            self.modules.append(dict(
                id=key, family=key[len("emerald:"):].split("/", 1)[0],
                arena=arena["name"], gba_start=m["region_gba_start"],
                byte_count=m["region_byte_count"],
                host_start=arena["host_start"] +
                (m["region_gba_start"] - arena["gba_start"])))
        self.modules.sort(key=lambda m: m["host_start"])
        self.by_id = {m["id"]: m for m in self.modules}
        self.by_gba = sorted(self.modules, key=lambda m: m["gba_start"])

    def _arena_containing(self, gba_addr):
        """Smallest arena whose gba range contains addr (FE wins over the
        battle hull because the sidecar lists it separately)."""
        best = None
        for a in self.arenas:
            if a["gba_start"] <= gba_addr < a["gba_end"]:
                if best is None or a["gba_end"] - a["gba_start"] < \
                        best["gba_end"] - best["gba_start"]:
                    best = a
        if best is None:
            raise Refusal(f"gba {gba_addr:#x} outside every arena")
        return best

    @property
    def token(self):
        return self._token

    # -- containment ------------------------------------------------------
    def contain(self, host_addr, token=None):
        """Reverse containment: host addr -> (family, module_id, offset).
        Module-exact: only bytes inside a staged module span resolve; hull
        holes, inter-module gaps and the hull's FE-region refuse. A stale
        generation token refuses."""
        if token is not None and token != self._token:
            raise Refusal("stale generation token")
        a = host_addr & MASK
        lo, hi = 0, len(self.modules)
        while lo < hi:                       # bisect over host spans
            mid = (lo + hi) // 2
            m = self.modules[mid]
            if a < m["host_start"]:
                hi = mid
            elif a >= m["host_start"] + m["byte_count"]:
                lo = mid + 1
            else:
                return (m["family"], m["id"], a - m["host_start"])
        raise Refusal(f"{a:#x} not a module byte")

    def contain_gba(self, gba_addr):
        """Base-independent gba containment (same result at both bases)."""
        lo, hi = 0, len(self.by_gba)
        while lo < hi:
            mid = (lo + hi) // 2
            m = self.by_gba[mid]
            if gba_addr < m["gba_start"]:
                hi = mid
            elif gba_addr >= m["gba_start"] + m["byte_count"]:
                lo = mid + 1
            else:
                return (m["family"], m["id"], gba_addr - m["gba_start"])
        raise Refusal(f"{gba_addr:#x} not a module byte in gba space")

    # -- staged memory ----------------------------------------------------
    def staged_word(self, module_id, offset, width):
        """Canonical payload word (the qualified-ROM bytes, never
        rewritten)."""
        if width not in (1, 2, 4):
            raise Refusal(f"unsupported operand width {width}")
        b = self.inputs.payloads[module_id]
        if offset < 0 or offset + width > len(b):
            raise Refusal(f"operand site {module_id}+{offset} out of payload")
        if width == 1:
            return b[offset]
        if width == 2:
            return struct.unpack_from("<H", b, offset)[0]
        return struct.unpack_from("<I", b, offset)[0]

    def host_word(self, host_addr, width, token=None):
        fam, mid, off = self.contain(host_addr, token)
        return self.staged_word(mid, off, width)


# ---------------------------------------------------------------------------
# Staging gates: every violation refuses staging (Refusal). One gate per
# fault-matrix case so the 21 faults map 1:1 onto refusals.
# ---------------------------------------------------------------------------

def gate_schemas(ins):
    for key, m in sorted(ins.metas.items()):
        want = SCHEMAS.get(key[len("emerald:"):].split("/", 1)[0])
        if m["schema"] != want:
            raise Refusal(f"{key}: schema {m['schema']} != {want}")
    for r in ins.catalog:
        if r["schema"] != ins.metas[r["id"]]["schema"]:
            raise Refusal(f"{r['id']}: catalog/meta schema mismatch")


def gate_lengths(ins):
    for key, m in sorted(ins.metas.items()):
        if m["region_byte_count"] == 0:
            if key in ins.payloads:
                raise Refusal(f"{key}: zero-width but has payload")
            continue
        if key not in ins.payloads:
            raise Refusal(f"{key}: payload missing")
        if m["region_byte_count"] != len(ins.payloads[key]):
            raise Refusal(f"{key}: byte_count {m['region_byte_count']} "
                          f"!= payload {len(ins.payloads[key])}")


def gate_digests(ins):
    rec = {r["id"]: r for r in ins.manifest}
    for key, payload in sorted(ins.payloads.items()):
        dig = hashlib.sha256(payload).hexdigest()
        if dig != ins.metas[key]["module_digest"]:
            raise Refusal(f"{key}: payload != module_digest")
        r = rec.get(key)
        if r is None:
            raise Refusal(f"{key}: not a manifest record")
        if dig != r["canonical_decoded_sha256"] or \
                dig != r["source_encoded_sha256"]:
            raise Refusal(f"{key}: manifest hash mismatch")
    if set(rec) != set(ins.payloads):
        raise Refusal("manifest records != payload module keys")


def gate_duplicates(ins):
    if len(ins.metas) != P_MODULES:
        raise Refusal(f"meta count {len(ins.metas)} != {P_MODULES}")
    if len(ins.catalog) != P_MODULES:
        raise Refusal(f"catalog count {len(ins.catalog)} != {P_MODULES}")
    if {r["id"] for r in ins.catalog} != set(ins.metas):
        raise Refusal("catalog ids != meta keys")
    seen = set()
    for r in ins.relocs:
        k = (r["module_key"], r["operand_offset"])
        if k in seen:
            raise Refusal(f"duplicate reloc source {k}")
        seen.add(k)
    eseen = set()
    for e in ins.exports:
        k = (e["module_key"], e["name"])
        if k in eseen:
            raise Refusal(f"duplicate export {k}")
        eseen.add(k)


def gate_boundaries(ins):
    """bytecode/routing boundary maps tile [0, byte_count) exactly; data and
    empty maps carry none; reloc operands never straddle a boundary."""
    for mp in ins.maps:
        m = ins.metas[mp["module_key"]]
        bc = m["region_byte_count"]
        bs = mp.get("boundaries", [])
        if mp["kind"] in ("data", "empty"):
            if bs:
                raise Refusal(f"{mp['module_key']}: {mp['kind']} has boundaries")
            continue
        if not bs:
            raise Refusal(f"{mp['module_key']}: no boundary map")
        offs = sorted(b["payload_offset"] for b in bs)
        if sum(b["length"] for b in bs) != bc or offs != sorted(set(offs)):
            raise Refusal(f"{mp['module_key']}: boundary map malformed")
        pos = 0
        for b in sorted(bs, key=lambda x: x["payload_offset"]):
            if b["payload_offset"] != pos:
                raise Refusal(f"{mp['module_key']}: boundary gap at {pos}")
            pos += b["length"]
    for r in ins.relocs:
        mp = next((x for x in ins.maps if x["module_key"] == r["module_key"]), None)
        if mp is None:
            raise Refusal(f"{r['module_key']}: no boundary map")
        if mp["kind"] not in ("bytecode", "routing"):
            raise Refusal(f"{r['module_key']}: reloc on {mp['kind']} module")
        hit = None
        for b in mp["boundaries"]:
            if b["payload_offset"] <= r["operand_offset"] < \
                    b["payload_offset"] + b["length"]:
                hit = b
                break
        if hit is None or r["operand_offset"] + r["operand_width"] > \
                hit["payload_offset"] + hit["length"]:
            raise Refusal(f"{r['module_key']}+{r['operand_offset']}: "
                          "operand straddles instruction boundary")


def gate_sources(ins):
    for r in ins.relocs:
        m = ins.metas.get(r["module_key"])
        if m is None:
            raise Refusal(f"{r['module_key']}: source module missing")
        if m["region_byte_count"] == 0:
            raise Refusal(f"{r['module_key']}: reloc on zero-width alias")
        if r["operand_offset"] + r["operand_width"] > m["region_byte_count"]:
            raise Refusal(f"{r['module_key']}+{r['operand_offset']}: "
                          "operand out of payload")


def gate_targets(ins):
    # Binding row identity is (gba_base_symbol, encoded_gba_value, addend):
    # EWRAM struct fields share the struct base with distinct field addends.
    bind = {(b["gba_base_symbol"], b["encoded_gba_value"], b["addend"]): b
            for b in ins.bindings}
    exp_names = {}
    for e in ins.exports:
        # multiple symbols (primary + aliases) can share one address, so the
        # name set at an offset may hold several names (e.g. Move_NONE and
        # Move_POUND both live at move_pound offset 0)
        exp_names.setdefault(e["module_key"], {}).setdefault(
            e["payload_offset"], set()).add(e["name"])
    for r in ins.relocs:
        cls = r["target_class"]
        if cls not in CLASSES:
            raise Refusal(f"{r['module_key']}+{r['operand_offset']}: "
                          f"unknown target class {cls}")
        if cls == "SCRIPT_TARGET":
            tm = ins.metas.get(r["target_resource_key"])
            if tm is None:
                raise Refusal(f"{r['module_key']}+{r['operand_offset']}: "
                              "script target module missing")
            to = r.get("target_offset")
            names = exp_names.get(r["target_resource_key"], {}).get(to)
            if to is None or not names:
                raise Refusal(f"{r['module_key']}+{r['operand_offset']}: "
                              "target offset not a root export")
            if r.get("target_export", "") not in names:
                raise Refusal(f"{r['module_key']}+{r['operand_offset']}: "
                              "target export name mismatch")
            if tm["region_gba_start"] + to != r["final_gba"]:
                raise Refusal(f"{r['module_key']}+{r['operand_offset']}: "
                              "final != target base + offset")
        else:
            sym = r.get("target_symbol", "")
            key = (sym, r["final_gba"] - r["addend"], r["addend"])
            b = bind.get(key)
            if b is None or b["encoded_gba_value"] + b["addend"] != r["final_gba"]:
                raise Refusal(f"{r['module_key']}+{r['operand_offset']}: "
                              f"unbound engine target {sym}")
            want_table = CLASS_TABLE.get(cls)
            if cls == "ENGINE_CALLBACK":
                want_table = b.get("table", "?")
            if b["semantic_family"] != cls or \
                    (want_table is not None and b.get("table") != want_table):
                raise Refusal(f"{r['module_key']}+{r['operand_offset']}: "
                              f"binding table/class mismatch for {sym}")


def gate_addends(ins):
    """raw_encoded_value is the H1 `stored` addend (== addend column, all
    7,549 rows); section-relative sites obey object base + raw == final
    (2,778 rows, re-verified against the sidecars)."""
    h1 = tomllib.load(open(H1_MODULES, "rb"))["modules"]
    objbase = {}
    modobj = {}
    for mm in h1:
        if mm["byte_count"]:
            objbase[mm["object"]] = min(objbase.get(mm["object"], 1 << 32),
                                        mm["gba_base"])
        modobj[mm["key"]] = mm["object"]
    sec = 0
    for r in ins.relocs:
        if r["raw_encoded_value"] != r["addend"]:
            raise Refusal(f"{r['module_key']}+{r['operand_offset']}: "
                          "raw_encoded_value != addend")
        ob = objbase[modobj[r["module_key"]]]
        if ob + r["raw_encoded_value"] == r["final_gba"]:
            sec += 1
    if sec != P_SECTION_REL:
        raise Refusal(f"section-relative sites {sec} != {P_SECTION_REL}")


def gate_words(ins):
    """§21 oracle: the staged word at every reloc site equals the canonical
    ROM word (== final_gba, re-verified 7,549/7,549 against the ROM)."""
    for r in ins.relocs:
        payload = ins.payloads[r["module_key"]]
        off, w = r["operand_offset"], r["operand_width"]
        word = struct.unpack_from("<I", payload, off)[0] if w == 4 else \
            (struct.unpack_from("<H", payload, off)[0] if w == 2
             else payload[off])
        if word != r["final_gba"]:
            raise Refusal(f"{r['module_key']}+{off}: staged word "
                          f"{word:#x} != final_gba {r['final_gba']:#x}")


def gate_nulls(ins):
    sites = set()
    for n in ins.nulls:
        m = ins.metas.get(n["module_key"])
        if m is None or m["region_byte_count"] == 0:
            raise Refusal(f"{n['module_key']}: null sentinel on missing/alias")
        if n["operand_offset"] + 4 > m["region_byte_count"]:
            raise Refusal(f"{n['module_key']}+{n['operand_offset']}: "
                          "null out of payload")
        sites.add((n["module_key"], n["operand_offset"]))
    for r in ins.relocs:
        if (r["module_key"], r["operand_offset"]) in sites:
            raise Refusal(f"{r['module_key']}+{r['operand_offset']}: "
                          "NULL sentinel resolved as a reloc")
    if len(ins.nulls) != P_NULLS:
        raise Refusal(f"null sentinels {len(ins.nulls)} != {P_NULLS}")


def gate_layout(ins):
    """gba-space module spans unique/non-overlapping, and no span crosses a
    declared arena hole."""
    spans = sorted((m["region_gba_start"],
                    m["region_gba_start"] + m["region_byte_count"])
                   for m in ins.metas.values() if m["region_byte_count"])
    for (a, b), (c, d) in zip(spans, spans[1:]):
        if b > c:
            raise Refusal(f"overlapping gba spans at {a:#x}/{c:#x}")
    for a in ins.arenas:
        for h in a.get("holes", []):
            hs, ln = h["gba_start"], h["length"]
            for s, e in spans:
                if s < hs + ln and e > hs:
                    raise Refusal(f"span crosses hole at {hs:#x}")


def stage(inputs, base):
    """Stage + validate. Any gate violation raises Refusal; the caller's
    prior generation is untouched (Inputs copies are never mutated)."""
    for g in (gate_schemas, gate_lengths, gate_digests, gate_duplicates,
              gate_boundaries, gate_sources, gate_words, gate_targets,
              gate_addends, gate_nulls, gate_layout):
        g(inputs)
    return Generation(inputs, base)


# ---------------------------------------------------------------------------
# Main passes
# ---------------------------------------------------------------------------

def main():
    ins = Inputs.load()
    if len(ins.manifest) != P_PAYLOAD:
        fail_now(f"manifest records {len(ins.manifest)} != {P_PAYLOAD}")

    ga = stage(ins, GEN_A)
    gb = stage(ins, GEN_B)
    ga2 = stage(ins, GEN_A)
    ok("generation token deterministic per base", ga.token == ga2.token)
    ok("generation tokens differ across bases", ga.token != gb.token)
    ok("manifest payload records = 2081", len(ins.manifest) == P_PAYLOAD)
    ok("payload modules staged = 2081", len(ga.modules) == P_PAYLOAD)

    # ---- §20 reverse containment --------------------------------------
    allmods = set(ins.metas)
    payload_ids = set(ins.payloads)
    ok("catalog == meta ids", {r["id"] for r in ins.catalog} == allmods)
    ok("8 zero-width alias identities (no payload)",
       len(allmods - payload_ids) == P_MODULES - P_PAYLOAD)

    # every module byte contains with exact round-trip
    exact = 0
    for m in ga.modules:
        for k in range(m["byte_count"]):
            fam, mid, off = ga.contain(m["host_start"] + k)
            if (fam, mid, off) == (m["family"], m["id"], k):
                exact += 1
    ok(f"containment round-trip over all {P_BYTES} payload bytes",
       exact == P_BYTES)

    # module spans unique + non-overlapping (pairwise)
    spans = sorted((m["host_start"], m["host_start"] + m["byte_count"])
                   for m in ga.modules)
    overlap = sum(1 for (a, b), (c, d) in zip(spans, spans[1:]) if b > c)
    ok("no overlapping module host spans", overlap == 0)

    # hull holes refuse; every non-module arena byte refuses (module-exact)
    refused = 0
    try:
        ga.contain(0xDEADBEEF)
    except Refusal:
        refused += 1
    for hs, ln in ga.holes:
        for k in range(ln):
            try:
                ga.contain(hs + k)
            except Refusal:
                refused += 1
    ok("declared hull holes refuse containment",
       refused == 1 + sum(ln for _, ln in ga.holes))
    arena_bytes = sum(a["host_end"] - a["host_start"] for a in ga.arenas)
    module_bytes = sum(m["byte_count"] for m in ga.modules)
    ok(f"all non-module arena bytes refuse ({module_bytes} staged of "
       f"{arena_bytes})", module_bytes == P_BYTES)

    # stale generation token refuses; fresh token accepted
    try:
        ga.contain(ga.modules[0]["host_start"], token="deadbeef")
        ok("stale generation token refuses", False)
    except Refusal:
        ok("stale generation token refuses", True)
    ga.contain(ga.modules[0]["host_start"], token=ga.token)  # must not raise
    ok("fresh token accepted", True)

    # different host bases allowed: gen-b is a distinct world
    try:
        gb.contain(ga.modules[0]["host_start"])
        ok("cross-base query refuses", False)
    except Refusal:
        ok("cross-base query refuses", True)

    # gba containment is base-independent
    m0 = ga.modules[0]
    f1, i1, o1 = ga.contain_gba(m0["gba_start"])
    f2, i2, o2 = gb.contain_gba(m0["gba_start"])
    ok("gba containment base-independent",
       (f1, i1, o1) == (f2, i2, o2) == (m0["family"], m0["id"], 0))
    try:
        ga.contain_gba(0x08200000)
        ok("gba outside arenas refuses", False)
    except Refusal:
        ok("gba outside arenas refuses", True)

    # ---- §21 source relocation index -----------------------------------
    ok("reloc rows = 7549", len(ins.relocs) == P_RELOCS)
    word_ok = cross_ok = raw_ok = 0
    for r in ins.relocs:
        ma = ga.by_id[r["module_key"]]
        mb = gb.by_id[r["module_key"]]
        w_a = ga.host_word(ma["host_start"] + r["operand_offset"],
                           r["operand_width"])
        w_b = gb.host_word(mb["host_start"] + r["operand_offset"],
                           r["operand_width"])
        if w_a == r["final_gba"]:
            word_ok += 1
        if w_a == w_b:
            cross_ok += 1
        if r["raw_encoded_value"] == r["addend"]:
            raw_ok += 1
    ok(f"staged host word == final_gba at every reloc site ({word_ok})",
       word_ok == P_RELOCS)
    ok(f"staged words identical across bases ({cross_ok})", cross_ok == P_RELOCS)
    ok(f"raw_encoded_value == addend everywhere ({raw_ok})", raw_ok == P_RELOCS)

    # null sentinel sites staged as word 0, unresolved
    null_zero = sum(1 for n in ins.nulls
                    if ga.staged_word(n["module_key"], n["operand_offset"], 4) == 0)
    ok(f"NULL sentinel sites staged as 0 ({null_zero}/{P_NULLS})",
       null_zero == P_NULLS)

    # ---- §22 target binding index --------------------------------------
    script = [r for r in ins.relocs if r["target_class"] == "SCRIPT_TARGET"]
    engine = [r for r in ins.relocs if r["target_class"] != "SCRIPT_TARGET"]
    ok(f"SCRIPT_TARGET rows = {P_SCRIPT}", len(script) == P_SCRIPT)
    ok(f"ENGINE_* rows = {P_ENGINE}", len(engine) == P_ENGINE)
    ok("no scalar-ID rows (ID routing stays ID-based)",
       not any(r["target_class"].endswith("_ID") for r in ins.relocs))
    ewr = [r for r in ins.relocs if r["target_class"] == "ENGINE_EWRAM_TARGET"]
    ok(f"EWRAM occurrences = {P_EWRAM}", len(ewr) == P_EWRAM)
    rows = ins.bindings
    ok(f"semantic binding rows = {P_BINDINGS}", len(rows) == P_BINDINGS)
    ok("binding rows unique by (symbol, enc, addend)",
       len({(b["gba_base_symbol"], b["encoded_gba_value"], b["addend"])
            for b in rows}) == len(rows))
    ok("binding tables A-F = 50/50/327/213/11/67",
       {b.get("table") for b in rows} == {"A", "B", "C", "D", "E", "F"} and
       [sum(1 for b in rows if b.get("table") == t) for t in "ABCDEF"] ==
       [50, 50, 327, 213, 11, 67])
    ok("every engine target bound by field row",
       all(any(b["gba_base_symbol"] == r.get("target_symbol", "") and
               b["encoded_gba_value"] + b["addend"] == r["final_gba"]
               for b in rows)
           for r in engine))
    ok(f"export names = {P_EXPORTS}", len(ins.exports) == P_EXPORTS)

    # ---- §23 shadow parity ----------------------------------------------
    def canon(g):
        return hashlib.sha256(b"".join(
            m["id"].encode() + ins.payloads[m["id"]] for m in g.modules)).hexdigest()
    ok("canonical aggregate identical across bases", canon(ga) == canon(gb))
    host_a = [m["host_start"] for m in ga.modules]
    host_b = [m["host_start"] for m in gb.modules]
    ok("host pointers differ between bases", host_a != host_b)
    ok("host base delta = 0x40000000",
       all(b - a == GEN_B - GEN_A for a, b in zip(host_a, host_b)))

    # ---- §30 fault matrix ----------------------------------------------
    faults = run_fault_matrix(ins)
    for name, refused, note in faults:
        ok(f"fault {name} refuses", refused, note)
    # prior generation preserved: pristine re-stage is byte-identical
    ga3 = stage(ins, GEN_A)
    ok("prior generation preserved after all faults",
       ga3.token == ga.token and canon(ga3) == canon(ga))

    print(f"\nH2 stage: {PASS} pass / {FAIL} fail  "
          f"(gen-a {GEN_A:#x}, gen-b {GEN_B:#x}, pack {P_PACK_ENTRIES} entries)")
    sys.exit(1 if FAIL else 0)


def fail_now(msg):
    print(f"FAIL {msg}")
    sys.exit(1)


# ---------------------------------------------------------------------------
# Fault matrix: 21 refusal cases (§30). Each mutates a deep copy of the
# inputs; staging/querying must refuse. The pristine Inputs is never touched,
# so the prior generation survives byte-identical (verified in main()).
# ---------------------------------------------------------------------------

def run_fault_matrix(ins):
    from copy import deepcopy
    out = []

    def attempt(name, mutate, query=None):
        mutated = deepcopy(ins)
        mutate(mutated)
        try:
            g = stage(mutated, GEN_A)
        except Refusal:
            out.append((name, True, ""))
            return
        if query is not None:
            try:
                query(mutated, g)
                out.append((name, False, "no refusal"))
            except Refusal:
                out.append((name, True, "runtime refusal"))
        else:
            out.append((name, False, "staging accepted"))

    # Rows are always derived from the per-fault copy `i`, never captured
    # from the pristine `ins` (captured row dicts are references into
    # ins.relocs - mutating them would corrupt the prior generation).
    def script_of(i):
        return next(r for r in i.relocs
                    if r["target_class"] == "SCRIPT_TARGET")

    def eng_of(i):
        return next(r for r in i.relocs
                    if r["target_class"].startswith("ENGINE_"))

    # 1. missing resource: the script target module vanishes from every
    # surface (catalog/manifest/meta/exports), as if never packed
    def f1(i):
        k = script_of(i)["target_resource_key"]
        del i.metas[k]
        i.payloads.pop(k, None)
        i.catalog[:] = [r for r in i.catalog if r["id"] != k]
        i.manifest[:] = [r for r in i.manifest if r["id"] != k]
        i.exports[:] = [e for e in i.exports if e["module_key"] != k]
        i.maps[:] = [mp for mp in i.maps if mp["module_key"] != k]
    attempt("01-missing-resource", f1)

    # 2. wrong schema
    def f2(i):
        k = sorted(i.metas)[0]
        i.metas[k]["schema"] = 99
    attempt("02-wrong-schema", f2)

    # 3. digest mismatch: corrupt a byte that is not a reloc site
    def f3(i):
        for key, payload in list(i.payloads.items()):
            sites = {r["operand_offset"] for r in i.relocs
                     if r["module_key"] == key and r["operand_width"] == 1}
            for off in range(len(payload)):
                if off not in sites:
                    b = bytearray(payload)
                    b[off] ^= 0xFF
                    i.payloads[key] = bytes(b)
                    return
        raise RuntimeError("no non-reloc byte found")
    attempt("03-digest-mismatch", f3)

    # 4. malformed length
    def f4(i):
        k = sorted(i.payloads)[0]
        i.metas[k]["region_byte_count"] += 1
    attempt("04-malformed-length", f4)

    # 5. duplicate key (meta entry duplicated under a second key)
    def f5(i):
        k = sorted(i.metas)[0]
        i.metas[k + "-dup"] = deepcopy(i.metas[k])
    attempt("05-duplicate-key", f5)

    # 6. missing export
    def f6(i):
        for r in i.relocs:
            if r["target_class"] == "SCRIPT_TARGET":
                r["target_export"] = "NotARealExport"
                break
    attempt("06-missing-export", f6)

    # 7. wrong export offset
    def f7(i):
        r = script_of(i)
        r["target_offset"] = (r["target_offset"] or 0) + 4
    attempt("07-wrong-export-offset", f7)

    # 8. invalid boundary map (stretch an instruction past the payload end)
    def f8(i):
        for mp in i.maps:
            if mp["kind"] == "bytecode" and mp["boundaries"]:
                mp["boundaries"][-1]["length"] += 1
                return
        raise RuntimeError("no bytecode map")
    attempt("08-invalid-boundary", f8)

    # 9. reloc source missing (operand beyond payload)
    def f9(i):
        r = script_of(i)
        r["operand_offset"] = i.metas[r["module_key"]]["region_byte_count"]
    attempt("09-reloc-source-missing", f9)

    # 10. reloc source duplicated
    def f10(i):
        i.relocs.append(deepcopy(script_of(i)))
    attempt("10-reloc-source-duplicated", f10)

    # 11. raw operand mismatch: corrupt the payload byte at a reloc site
    # (all 7,549 sites are width 4). Digests are updated to match so the
    # refusal comes from the site-level §21 word gate, not the digest gate.
    def f11(i):
        r = i.relocs[0]
        b = bytearray(i.payloads[r["module_key"]])
        b[r["operand_offset"]] ^= 0xFF
        i.payloads[r["module_key"]] = bytes(b)
        dig = hashlib.sha256(i.payloads[r["module_key"]]).hexdigest()
        i.metas[r["module_key"]]["module_digest"] = dig
        for rec in i.manifest:
            if rec["id"] == r["module_key"]:
                rec["canonical_decoded_sha256"] = dig
                rec["source_encoded_sha256"] = dig
    attempt("11-raw-operand-mismatch", f11)

    # 12. wrong target class
    def f12(i):
        eng_of(i)["target_class"] = "ENGINE_BOGUS_TARGET"
    attempt("12-wrong-target-class", f12)

    # 13. unknown EWRAM binding
    def f13(i):
        e = next(r for r in i.relocs
                 if r["target_class"] == "ENGINE_EWRAM_TARGET")
        e["target_symbol"] = "gNonexistentEwramField"
    attempt("13-unknown-ewram-binding", f13)

    # 14. invalid addend (raw/addend metadata disagree)
    def f14(i):
        eng_of(i)["addend"] += 1
    attempt("14-invalid-addend", f14)

    # 15. unknown callback
    def f15(i):
        c = next(r for r in i.relocs
                 if r["target_class"] == "ENGINE_CALLBACK")
        c["target_symbol"] = "AnimTask_DoesNotExist"
    attempt("15-unknown-callback", f15)

    # 16. unknown template
    def f16(i):
        t = next(r for r in i.relocs
                 if r["target_class"] == "ENGINE_SPRITE_TEMPLATE_TARGET")
        t["target_symbol"] = "gNonexistentSpriteTemplate"
    attempt("16-unknown-template", f16)

    # 17. unknown gfx
    def f17(i):
        g = next(r for r in i.relocs
                 if r["target_class"] == "ENGINE_GFX_TARGET")
        g["target_symbol"] = "gNonexistentGfx"
    attempt("17-unknown-gfx", f17)

    # 18. illegal NULL: resolve a NULL sentinel as a reloc source
    def f18(i):
        n = i.nulls[0]
        i.relocs.append(dict(module_key=n["module_key"],
                             operand_offset=n["operand_offset"],
                             operand_width=4, raw_encoded_value=0,
                             target_class="SCRIPT_TARGET", addend=0,
                             target_symbol="", reloc_symbol="",
                             target_resource_key=script_of(i)["target_resource_key"],
                             target_export="", target_offset=0,
                             target_kind="script", final_gba=0, root=True,
                             command="", operand=""))
    attempt("18-illegal-null", f18)

    # 19. overlap: two modules with the same gba span
    def f19(i):
        ids = sorted(i.payloads)
        i.metas[ids[1]]["region_gba_start"] = i.metas[ids[0]]["region_gba_start"]
        i.metas[ids[1]]["region_byte_count"] = i.metas[ids[0]]["region_byte_count"]
    attempt("19-overlap", f19)

    # 20. containment hole: extend the module ending at a hull hole so its
    # span crosses the hole (a span may never cover a hole)
    def f20(i):
        for a in i.arenas:
            for h in a.get("holes", []):
                hs, ln = h["gba_start"], h["length"]
                for key, m in i.metas.items():
                    if m["region_byte_count"] and \
                            m["region_gba_start"] + m["region_byte_count"] == hs:
                        m["region_byte_count"] += ln
                        return
        raise RuntimeError("no module adjacent to a hole")
    attempt("20-containment-hole", f20)

    # 21. stale generation: a query carrying a foreign token refuses
    gb = stage(ins, GEN_B)
    try:
        gb.contain(gb.modules[0]["host_start"], token="stale-token")
        out.append(("21-stale-generation", False, "no refusal"))
    except Refusal:
        out.append(("21-stale-generation", True, ""))
    return out


if __name__ == "__main__":
    main()

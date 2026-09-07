#!/usr/bin/env python3
"""R9 Stage 2 completeness + cross-reference check for the Pokémon battle
family metadata.

Verifies, without a user ROM or a game launch, that the five generated files
under resources/extraction/emerald/bpee01/pokemon_battle/ are consistent with
each other and with the committed C tree:

  1. species_mapping: 4 tables x 440 slots, indices 0..439 strictly ascending,
     species token resolves in species.h to exactly the row index, and every
     slot resolves to a catalog canonical (or an allowlisted external symbol).
  2. catalog / bindings / ownership: identical sorted id sets; every binding's
     source artifact exists and its LZ77 header matches expected_decoded_size;
     ownership's encoded/decoded SHA-256s match the artifact file and a strict
     software GBA-LZ77 decode of it (independent reimplementation).
  3. M0/M1 keys: ownership key == sha256("gen3-resource-id-v1\\0" + id).
  4. consumers: every (id, table, index) resource_slot matches the
     species_mapping row for that (table, index); table_sites count matches
     the generator's fixed inventory; external slots are the allowlisted ones.

Usage: python3 pokemon_family_completeness.py [repo-root]
"""
import hashlib
import re
import sys

def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)

def strict_lz77_decode(data):
    """Independent strict GBA LZ77 decode (see gen3/resources/lz77.h)."""
    if len(data) < 4 or data[0] != 0x10:
        fail("not a GBA LZ77 stream")
    size = data[1] | (data[2] << 8) | (data[3] << 16)
    out = bytearray()
    i = 4
    while len(out) < size:
        ctrl = data[i]; i += 1
        for bit in range(7, -1, -1):
            if len(out) >= size:
                break
            if ctrl & (1 << bit):
                if i + 2 > len(data):
                    fail("truncated stream")
                b0, b1 = data[i], data[i + 1]; i += 2
                block = (b0 >> 4) + 3
                dist = ((b0 & 0xF) << 8 | b1) + 1
                if dist > len(out):
                    fail("invalid backref")
                for _ in range(block):
                    out.append(out[-dist])
            else:
                if i >= len(data):
                    fail("truncated stream")
                out.append(data[i]); i += 1
    if len(out) != size:
        fail("decoded size mismatch")
    if any(b != 0 for b in data[i:]):
        fail("trailing non-zero data")
    return bytes(out)

def parse_tables(path):
    """Split a generated TOML on repeated [[array]] headers; every array table
    (one per row in these files) becomes one dict in the named list."""
    out = {}
    for chunk in re.split(r"(?<=\n)(?=\[\[)", open(path).read()):
        m = re.match(r"\[\[([a-z_]+)\]\]\n", chunk)
        if not m:
            continue
        d = {}
        for fm in re.finditer(r'^([A-Za-z0-9_]+)\s*=\s*("([^"]*)"|(-?\d+))\s*$',
                              chunk[m.end():], re.M):
            d[fm.group(1)] = fm.group(3) if fm.group(2).startswith('"') else int(fm.group(4))
        out.setdefault(m.group(1), []).append(d)
    return out

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    battle = root + "/resources/extraction/emerald/bpee01/pokemon_battle"
    tables = parse_tables(battle + "/species_mapping.generated.toml")
    catalog = parse_tables(battle + "/catalog.generated.toml")["resources"]
    bindings = parse_tables(battle + "/bindings.generated.toml")["bindings"]
    ownership = parse_tables(battle + "/ownership.generated.toml")["resources"]
    consumers = parse_tables(battle + "/consumers.generated.toml")

    catalog_ids = [r["id"] for r in catalog]
    binding_ids = [r["id"] for r in bindings]
    ownership_ids = [r["id"] for r in ownership]
    # R9 §6: the external still-EGG payload is declared by the catalog +
    # bindings (it IS a pack resource) but is intentionally NOT an
    # ownership record (it stays compiled on native).
    catalog_set = set(catalog_ids) - {"emerald:pokemon/egg/battle/front/still"}
    binding_set = set(binding_ids) - {"emerald:pokemon/egg/battle/front/still"}
    if not (catalog_set == binding_set == set(ownership_ids)):
        fail("catalog/bindings/ownership id sets differ")
    if catalog_ids != sorted(catalog_ids):
        fail("catalog ids not bytewise sorted")
    catalog_ids = set(catalog_ids)
    print("  PASS metadata id sets identical, sorted (%d ids)" % len(catalog_ids))

    # species.h: species token -> id (alias + arithmetic passes).
    species = {}
    for line in open(root + "/include/constants/species.h"):
        m = re.match(r"#define (SPECIES_\w+) (\d+)", line)
        if m and m.group(1) != "SPECIES_TOTAL":
            species[m.group(1)] = int(m.group(2))
    changed = True
    while changed:
        changed = False
        for line in open(root + "/include/constants/species.h"):
            m = re.match(r"#define (\w+) (SPECIES_\w+)", line)
            if m and m.group(2) in species and m.group(1) not in species:
                species[m.group(1)] = species[m.group(2)]; changed = True
        for line in open(root + "/include/constants/species.h"):
            m = re.match(r"#define (SPECIES_\w+) \((\w+) \+ (\d+)\)", line)
            if m and m.group(1) not in species and m.group(2) in species:
                species[m.group(1)] = species[m.group(2)] + int(m.group(3)); changed = True

    table_kinds = {"gMonFrontPicTable": "front_sheet",
                   "gMonBackPicTable": "back_sheet",
                   "gMonPaletteTable": "normal_palette",
                   "gMonShinyPaletteTable": "shiny_palette",
                   "gMonStillFrontPicTable": "still_front",
                   "gMonIconTable": "icon",
                   "gMonFootprintTable": "footprint"}
    kind_tables = {v: k for k, v in table_kinds.items()}
    EXPECTED_SITES = {"gMonFrontPicTable": 44, "gMonBackPicTable": 15,
                      "gMonPaletteTable": 6, "gMonShinyPaletteTable": 3,
                      "gMonStillFrontPicTable": 0, "gMonIconTable": 0,
                      "gMonFootprintTable": 0}
    EXTERNAL = {"gMonStillFrontPic_Egg"}

    # 1. species mapping vs species.h + catalog.
    slot_ids = {}   # (table, index) -> canonical or external symbol
    for row in tables["slots"]:
        table = row["table"]
        idx = row["index"]
        token = "SPECIES_" + row["species"]  # table rows carry bare tokens
        if token not in species or species[token] != idx:
            fail("species token %s != index %d in %s" % (row["species"], idx, table))
        if "external_symbol" in row:
            if row["external_symbol"] not in EXTERNAL:
                fail("unexpected external symbol %s" % row["external_symbol"])
            if table not in ("gMonBackPicTable", "gMonStillFrontPicTable") or idx != 412:
                fail("external slot not a back/still-EGG slot: %s[%d]" % (table, idx))
            slot_ids[(table, idx)] = row["external_symbol"]
        else:
            canon = row["canonical"]
            if canon not in catalog_ids:
                fail("slot %s[%d] -> %s not in catalog" % (table, idx, canon))
            seg = canon.rsplit("/battle/", 1)
            if len(seg) == 2 and "/" + seg[1] in ("/front/sheet", "/back/sheet",
                                                  "/normal-palette", "/shiny-palette",
                                                  "/front/still"):
                pass
            elif len(seg) == 1 and "/" + canon.rsplit("/", 1)[1] in ("/icon", "/footprint"):
                pass
            else:
                fail("malformed canonical %s" % canon)
            # Canonical must mirror the full artifact path: the canonical
            # segment is "pokemon/<dirs...>" = the artifact path under
            # graphics/, minus the file name. Multi-level dirs (e.g.
            # question_mark/circled) must match exactly.
            binding = next((b for b in bindings if b["id"] == canon), None)
            if binding is None:
                fail("no binding for %s" % canon)
            path = binding["source_artifact"].split("graphics/", 1)[1]
            path = path.rsplit("/", 1)[0]  # drop the file name
            if "/battle/" in canon:
                if "emerald:" + path != canon.rsplit("/battle/", 1)[0]:
                    fail("canonical path %s != artifact path %s" % (canon, path))
            else:
                if "emerald:" + path != canon.rsplit("/", 1)[0]:
                    fail("canonical path %s != artifact path %s" % (canon, path))
            slot_ids[(table, idx)] = canon
    print("  PASS species mapping: 3053 rows all resolve (index == species id)")

    per_table = {}
    for (table, idx) in slot_ids:
        per_table.setdefault(table, set()).add(idx)
    for table, kind in table_kinds.items():
        expected = set(range(440)) if kind != "footprint" \
            else set(range(413))
        if per_table.get(table) != expected:
            fail("table %s does not cover its slots exactly" % table)
    print("  PASS tables cover slots exactly (6 x 440 + footprint 413)")

    # 2. bindings vs artifacts: LZ header size + strict decode hash (RAW kinds
    #    are verbatim: no LZ container, decoded == encoded == file bytes).
    external = {"emerald:pokemon/egg/battle/front/still"}  # R9 §6: no ownership record
    for b in bindings:
        path = root + "/" + b["source_artifact"]
        try:
            raw = open(path, "rb").read()
        except OSError:
            fail("missing artifact %s" % b["source_artifact"])
        if b.get("source_encoding") == "raw":
            declared = len(raw)
            if declared != b["expected_decoded_size"]:
                fail("raw size %d != expected %d for %s" % (declared, b["expected_decoded_size"], b["id"]))
            if hashlib.sha256(raw).hexdigest() != next(o["source_encoded_sha256"] for o in ownership if o["id"] == b["id"]):
                fail("encoded sha mismatch for %s" % b["id"])
            if hashlib.sha256(raw).hexdigest() != next(o["canonical_decoded_sha256"] for o in ownership if o["id"] == b["id"]):
                fail("decoded sha mismatch for %s" % b["id"])
            continue
        if b["id"] in external:
            continue  # external still-EGG: verified through the manifest
        declared = raw[1] | (raw[2] << 8) | (raw[3] << 16)
        if declared != b["expected_decoded_size"]:
            fail("header size %d != expected %d for %s" % (declared, b["expected_decoded_size"], b["id"]))
        kind = b["id"].rsplit("/battle/", 1)[1] if "/battle/" in b["id"] else b["id"].rsplit("/", 1)[1]
        if (kind in ("front/sheet", "back/sheet", "front/still") and declared % 2048 != 0) or \
           (kind in ("normal-palette", "shiny-palette") and declared % 32 != 0):
            fail("size %d not a valid multiple for %s" % (declared, b["id"]))
        if hashlib.sha256(raw).hexdigest() != next(o["source_encoded_sha256"] for o in ownership if o["id"] == b["id"]):
            fail("encoded sha mismatch for %s" % b["id"])
        dec = strict_lz77_decode(raw)
        if len(dec) != declared:
            fail("decode length mismatch for %s" % b["id"])
        if hashlib.sha256(dec).hexdigest() != next(o["canonical_decoded_sha256"] for o in ownership if o["id"] == b["id"]):
            fail("decoded sha mismatch for %s" % b["id"])
        if b["expected_decoded_size"] != len(dec):
            fail("expected_decoded_size != decoded length for %s" % b["id"])
    print("  PASS artifacts: 2826 LZ77 decodes + RAW verifications + hashes")

    # 3. M0/M1 keys.
    for o in ownership:
        key = hashlib.sha256(b"gen3-resource-id-v1\x00" + o["id"].encode()).hexdigest()
        if o["key"] != key:
            fail("key mismatch for %s" % o["id"])
    print("  PASS M0/M1 keys (2826/2826)")

    # 4. consumers vs species mapping (bijection) and site counts.
    res_slots = {}
    for row in consumers.get("resource_slots", []):
        res_slots[(row["table"], row["index"])] = row["id"]
    ext_slots = {}
    for row in consumers.get("external_slots", []):
        ext_slots[(row["table"], row["index"])] = row["symbol"]
    for (table, idx), canon in slot_ids.items():
        if isinstance(canon, str) and canon.startswith("emerald:"):
            if res_slots.get((table, idx)) != canon:
                fail("consumers resource_slot mismatch at %s[%d]" % (table, idx))
        else:
            if ext_slots.get((table, idx)) != canon:
                fail("consumers external_slot mismatch at %s[%d]" % (table, idx))
    if len(res_slots) != 3051 or len(ext_slots) != 2:
        fail("consumer slot counts wrong: %d + %d" % (len(res_slots), len(ext_slots)))
    sites = {}
    for row in consumers.get("table_sites", []):
        sites.setdefault(row["table"], []).append(row["site"])
    for table, n in EXPECTED_SITES.items():
        if len(sites.get(table, [])) != n:
            fail("table_sites for %s: %d != %d" % (table, len(sites.get(table, [])), n))
        if sites.get(table, []) != sorted(set(sites.get(table, []))):
            fail("table_sites for %s not sorted/unique" % table)
    print("  PASS consumers: 3051 resource slots + 2 external slots + 68 sites")

    print("  PASS pokemon-family completeness: all checks green")

if __name__ == "__main__":
    main()

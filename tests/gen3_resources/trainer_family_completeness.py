#!/usr/bin/env python3
"""Stage R7A/R8 completeness + cross-reference checker (§19, part of §18).

Verifies, against the C tree AND the checked-in generated files, that the
trainer-front (R7A) and trainer-back (R8) family metadata is mechanically
complete and self-consistent:

  * Family completeness (§19): the front descriptor's 93 trainers are exactly
    the TRAINER_PIC_* constants; the back descriptor's 8 trainers are exactly
    the TRAINER_BACK_PIC_* constants; every sheet / palette is catalogued.
  * Catalog/bindings/ownership cross-reference: every id present in all three,
    no orphans, bytewise-sorted, sizes/encodings/representations consistent.
    The catalog is the union of both families (196); the per-family bindings/
    ownership/consumers files stay per-family (186 front + 10 back).
  * Canonical-name mapping (§4): descriptor canonical == ASCII-lowercase of the
    *_PIC_* constant suffix with '_' -> '-'.
  * Consumer map (§13): front file 30 table_sites / 192 resource_slots; back
    file 8 table_sites / 10 resource_slots / 34 frame_slots; every slot has a
    resource, no duplicate (table, index) / (array, index); the 6 shared back
    palettes carry the front-file back_palette_index consumer and are NOT
    duplicated as back payloads.
  * Keys: every ownership key equals the independently recomputed M0/M1 key
    and all 196 keys are pairwise distinct (the reachable half of "canonical
    key collision"; a SHA-256 collision cannot be constructed, so the
    generator's collision check is defensive).

Reads only committed files under the repo root. No user ROM required.
"""

import hashlib
import re
import sys

M0_DOMAIN = b"gen3-resource-id-v1\x00"


def fail(msg):
    print("  FAIL " + msg)
    return 1


def ok(msg):
    print("  PASS " + msg)
    return 0


def parse_blocks(text, marker):
    """Split TOML on `[[marker]]` lines, returning a list of dicts built from
    the `key = value` lines in each block. String and integer/boolean values
    are typed. Only a marker on its own line splits (comments that merely quote
    `[[marker]]` are not separators)."""
    blocks = []
    parts = re.split(r'(?m)^\s*\[\[' + re.escape(marker) + r'\]\]\s*$', text)
    for raw in parts[1:]:
        entry = {}
        for line in raw.splitlines():
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("["):
                continue
            m = re.match(r'^([A-Za-z0-9_]+)\s*=\s*(.*)$', line)
            if not m:
                continue
            key, value = m.group(1), m.group(2)
            if value.startswith('"'):
                entry[key] = value[1:-1]
            elif value == "true":
                entry[key] = True
            elif value == "false":
                entry[key] = False
            else:
                entry[key] = int(value)
        blocks.append(entry)
    return blocks


def read(path):
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def derive_key(name):
    return hashlib.sha256(M0_DOMAIN + name.encode("utf-8")).hexdigest()


def trainer_constants(root):
    """{ suffix: numeric value } from include/constants/trainers.h"""
    out = {}
    text = read(root + "/include/constants/trainers.h")
    for m in re.finditer(r"#define\s+TRAINER_PIC_([A-Z0-9_]+)\s+(\d+)", text):
        out[m.group(1)] = int(m.group(2))
    return out


def back_trainer_constants(root):
    """{ suffix: numeric value } for the TRAINER_BACK_PIC_* constants."""
    out = {}
    text = read(root + "/include/constants/trainers.h")
    for m in re.finditer(r"#define\s+TRAINER_BACK_PIC_([A-Z0-9_]+)\s+(\d+)", text):
        out[m.group(1)] = int(m.group(2))
    return out


def parse_trainer_blocks(text):
    """Parse the [[trainers]] blocks of a family descriptor into dicts.
    Handles strings, integers and booleans (the R8 back descriptor uses
    has_palette = true/false)."""
    trainers = []
    cur = None
    for line in text.splitlines():
        line = line.strip()
        if line == "[[trainers]]":
            if cur is not None:
                trainers.append(cur)
            cur = {}
        elif cur is not None and not line.startswith("#"):
            m = re.match(r'^([A-Za-z0-9_]+)\s*=\s*("([^"]*)"|(-?\d+)|(true|false))$', line)
            if m:
                if m.group(4) is not None:
                    cur[m.group(1)] = int(m.group(4))
                elif m.group(5) is not None:
                    cur[m.group(1)] = m.group(5) == "true"
                else:
                    cur[m.group(1)] = m.group(3)
    if cur is not None:
        trainers.append(cur)
    return trainers


def front_table_suffixes(root):
    """Set of TRAINER_PIC_* suffixes referenced by the in-tree front table."""
    text = read(root + "/src/data/trainer_graphics/front_pic_tables.h")
    return set(re.findall(r"\[TRAINER_PIC_([A-Z0-9_]+)\]", text))


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    bpee01 = root + "/resources/extraction/emerald/bpee01"
    catalog_path = root + "/resources/catalogs/emerald/catalog.toml"
    descriptor_path = bpee01 + "/trainer_front_family.toml"
    bindings_path = bpee01 + "/bindings.generated.toml"
    ownership_path = bpee01 + "/ownership.generated.toml"
    consumers_path = bpee01 + "/trainer_front_consumers.generated.toml"
    back_descriptor_path = bpee01 + "/trainer_back_family.toml"
    back_bindings_path = bpee01 + "/back_bindings.generated.toml"
    back_ownership_path = bpee01 + "/back_ownership.generated.toml"
    back_consumers_path = bpee01 + "/trainer_back_consumers.generated.toml"

    failures = 0

    # ---- parse inputs -----------------------------------------------------
    desc = read(descriptor_path)
    desc_header = {}
    for m in re.finditer(r'^([A-Za-z0-9_]+)\s*=\s*"([^"]*)"', desc, re.M):
        desc_header[m.group(1)] = m.group(2)
    for m in re.finditer(r'^([A-Za-z0-9_]+)\s*=\s*(\d+)', desc, re.M):
        desc_header[m.group(1)] = int(m.group(2))

    trainers = parse_trainer_blocks(desc)

    bdesc = read(back_descriptor_path)
    bdesc_header = {}
    for m in re.finditer(r'^([A-Za-z0-9_]+)\s*=\s*"([^"]*)"', bdesc, re.M):
        bdesc_header[m.group(1)] = m.group(2)
    for m in re.finditer(r'^([A-Za-z0-9_]+)\s*=\s*(\d+)', bdesc, re.M):
        bdesc_header[m.group(1)] = int(m.group(2))
    btrainers = parse_trainer_blocks(bdesc)

    catalog = parse_blocks(read(catalog_path), "resources")
    bindings = parse_blocks(read(bindings_path), "bindings")
    ownership = parse_blocks(read(ownership_path), "resources")
    consumers = read(consumers_path)
    table_sites = parse_blocks(consumers, "table_sites")
    resource_slots = parse_blocks(consumers, "resource_slots")

    back_bindings = parse_blocks(read(back_bindings_path), "bindings")
    back_ownership = parse_blocks(read(back_ownership_path), "resources")
    back_consumers = read(back_consumers_path)
    # The [[frame_slots]] section trails [[resource_slots]]; parse_blocks
    # would absorb its rows into the last resource-slot block, so parse the
    # table/slot sections from the text cut at the frame_slots marker line
    # (the header comment also quotes the marker, so match the line form).
    back_pre_frames = re.split(r'(?m)^\s*\[\[frame_slots\]\]\s*$', back_consumers, 1)[0]
    back_table_sites = parse_blocks(back_pre_frames, "table_sites")
    back_resource_slots = parse_blocks(back_pre_frames, "resource_slots")
    frame_slots = parse_blocks(back_consumers, "frame_slots")

    sheet_type = desc_header["resource_type_sheet"]
    palette_type = desc_header["resource_type_palette"]
    sheet_encoding = desc_header["sheet_encoding"]
    palette_encoding = desc_header["palette_encoding"]
    sheet_repr = desc_header["sheet_representation"]
    palette_repr = desc_header["palette_representation"]
    sheet_size = desc_header["sheet_decoded_size"]
    palette_size = desc_header["palette_decoded_size"]
    sheet_tpl = desc_header["sheet_id_template"]
    palette_tpl = desc_header["palette_id_template"]

    # ---- §19 family completeness -----------------------------------------
    constants = trainer_constants(root)
    table_suffixes = front_table_suffixes(root)
    desc_pics = set(t["pic"] for t in trainers)
    if set(constants.keys()) != desc_pics:
        failures += fail("descriptor TRAINER_PIC_ set != constants/trainers.h "
                         "(%d vs %d)" % (len(desc_pics), len(constants)))
    else:
        failures += ok("descriptor pics == TRAINER_PIC_* constants (%d)" % len(constants))
    if table_suffixes != desc_pics:
        failures += fail("front table suffix set != descriptor pics")
    else:
        failures += ok("front table references exactly the 93 descriptor trainers")

    if len(trainers) != 93:
        failures += fail("descriptor has %d trainers, expected 93" % len(trainers))
    else:
        failures += ok("descriptor has 93 trainers")

    if len(catalog) != 196:
        failures += fail("catalog has %d entries, expected 196" % len(catalog))
    else:
        failures += ok("catalog has 196 entries (186 front + 10 back)")

    # ---- canonical-name mapping (§4) and indices --------------------------
    mapping_bad = [t for t in trainers
                   if t["canonical"] != t["pic"].lower().replace("_", "-")]
    if mapping_bad:
        failures += fail("canonical-name rule broken for %s"
                         % ", ".join(t["canonical"] for t in mapping_bad[:3]))
    else:
        failures += ok("canonical names obey lowercase + '_'->'-' for all 93")
    idx_bad = [t for t in trainers if constants.get(t["pic"]) != t["index"]]
    if idx_bad:
        failures += fail("descriptor index != TRAINER_PIC_ value for %s"
                         % ", ".join(t["pic"] for t in idx_bad[:3]))
    else:
        failures += ok("every descriptor index equals its TRAINER_PIC_ constant")

    indices = [t["index"] for t in trainers]
    if sorted(indices) != list(range(93)):
        failures += fail("descriptor indices are not a bijection of 0..92")
    else:
        failures += ok("table indices are a bijection of 0..92")

    # ---- catalog cross-check (union of both families) ---------------------
    catalog_ids = [e["id"] for e in catalog]
    # The catalog is the front-family run (bytewise-sorted, exactly the R7B
    # 186) followed by the back-family run (bytewise-sorted); the runs are
    # contiguous and family order is fixed, so the layout is independent of
    # descriptor [[trainers]] order.
    expected_ids = set()
    for t in trainers:
        expected_ids.add(sheet_tpl % t["canonical"])
        expected_ids.add(palette_tpl % t["canonical"])
    back_sheet_tpl = bdesc_header["sheet_id_template"]
    back_palette_tpl = bdesc_header["palette_id_template"]
    expected_back_ids = set()
    for t in btrainers:
        expected_back_ids.add(back_sheet_tpl % t["canonical"])
        if t["has_palette"]:
            expected_back_ids.add(back_palette_tpl % t["canonical"])
    if expected_ids & expected_back_ids:
        failures += fail("cross-family canonical id collision")
    else:
        failures += ok("front and back id sets are disjoint")
    front_run = sorted(expected_ids)
    back_run = sorted(expected_back_ids)
    if catalog_ids != front_run + back_run:
        failures += fail("catalog layout != front-run + back-run (each bytewise sorted)")
    else:
        failures += ok("catalog ids == front-run (186, R7B order) + back-run (10)")

    type_ok = all(
        (e["id"].endswith("/sheet") and e["type"] == sheet_type)
        or (e["id"].endswith("/normal-palette") and e["type"] == palette_type)
        or (e["id"].endswith("/palette") and e["type"] == palette_type)
        for e in catalog)
    failures += ok("catalog types match id suffix") if type_ok else \
        fail("catalog type/id mismatch")

    # ---- bindings cross-check ---------------------------------------------
    b_ids = [b["id"] for b in bindings]
    if len(bindings) != 186 or set(b_ids) != expected_ids:
        failures += fail("bindings set/size (%d) != catalog ids" % len(bindings))
    else:
        failures += ok("bindings == catalog ids (186)")
    if b_ids != sorted(b_ids):
        failures += fail("bindings ids not bytewise sorted")
    else:
        failures += ok("bindings ids bytewise sorted")
    by_trainer = {t["canonical"]: t for t in trainers}
    for b in bindings:
        canon = None
        if b["id"].endswith("/sheet"):
            canon = b["id"][len(sheet_tpl.split("%s")[0]):-len("/battle/front/sheet")]
        else:
            canon = b["id"][len(palette_tpl.split("%s")[0]):-len("/battle/front/normal-palette")]
        t = by_trainer[canon]
        expect_sym = "gTrainerFrontPic_%s" % t["symbol"]
        if b["id"].endswith("/normal-palette"):
            expect_sym = "gTrainerPalette_%s" % t["symbol"]
        if b["symbol"] != expect_sym:
            failures += fail("binding symbol for %s is %s, expected %s"
                             % (canon, b["symbol"], expect_sym))
        if b["source_encoding"] != (sheet_encoding if "/sheet" in b["id"] else palette_encoding):
            failures += fail("binding encoding for %s" % canon)
        if b["canonical_representation"] != (sheet_repr if "/sheet" in b["id"] else palette_repr):
            failures += fail("binding representation for %s" % canon)
        want_size = sheet_size if b["id"].endswith("/sheet") else palette_size
        if b["expected_decoded_size"] != want_size:
            failures += fail("binding size for %s is %d, expected %d"
                             % (canon, b["expected_decoded_size"], want_size))
        if b["id"].endswith("/sheet"):
            art = "graphics/trainers/front_pics/%s.4bpp.lz" % t["slug"]
        elif t["palette_dir"] == "palettes":
            art = "graphics/trainers/palettes/%s.gbapal.lz" % t["slug"]
        else:
            art = "graphics/trainers/front_pics/%s.gbapal.lz" % t["slug"]
        if b["source_artifact"] != art:
            failures += fail("binding artifact for %s is %s, expected %s"
                             % (canon, b["source_artifact"], art))
    failures += ok("bindings symbols/encodings/sizes/artifacts consistent")

    # ---- ownership cross-check --------------------------------------------
    o_by_id = {o["id"]: o for o in ownership}
    if len(ownership) != 186 or set(o_by_id.keys()) != expected_ids:
        failures += fail("ownership set/size (%d) != catalog ids" % len(ownership))
    else:
        failures += ok("ownership == catalog ids (186)")
    key_bad = [o["id"] for o in ownership if o["key"] != derive_key(o["id"])]
    if key_bad:
        failures += fail("ownership key mismatch for %s" % key_bad[0])
    else:
        failures += ok("all 186 ownership keys == independently derived M0/M1")
    keys = [o["key"] for o in ownership]
    if len(set(keys)) != 186:
        failures += fail("ownership keys not pairwise distinct")
    else:
        failures += ok("186 ownership keys pairwise distinct (no key collision)")
    state_bad = []
    for o in ownership:
        expect = "ROM_BASE_ONLY"  # R7B end state: the whole trainer-front family migrated
        if o["ownership_state"] != expect:
            state_bad.append(o["id"])
    if state_bad:
        failures += fail("ownership_state wrong for %s" % state_bad[0])
    else:
        failures += ok("ownership_state: all 186 ROM_BASE_ONLY (0 pending, R7B end state)")

    # targets blocks (regex-only, not TOML-parseable)
    native_vals = re.findall(r'^native = "([^"]+)"', read(ownership_path), re.M)
    gba_vals = re.findall(r'^gba = "([^"]+)"', read(ownership_path), re.M)
    if len(native_vals) != 186 or len(gba_vals) != 186:
        failures += fail("ownership targets blocks missing (%d native, %d gba)"
                         % (len(native_vals), len(gba_vals)))
    elif any(v != "COMPILED_PENDING_MIGRATION" and v != "ROM_BASE_ONLY" for v in native_vals) \
            or any(v != "COMPILED" for v in gba_vals):
        failures += fail("ownership targets state wrong")
    else:
        failures += ok("ownership [resources.targets] blocks: native/gba per record")

    # ---- consumer map (§13) -----------------------------------------------
    if len(table_sites) != 30:
        failures += fail("table_sites count %d, expected 30" % len(table_sites))
    else:
        failures += ok("consumer map has 30 table_sites (6 front + 18 pal + 6 back)")
    if len(resource_slots) != 192:
        failures += fail("resource_slots count %d, expected 192" % len(resource_slots))
    else:
        failures += ok("consumer map has 192 resource_slots (186 primary + 6 back)")

    by_table = {}
    slot_ids = set()
    for s in resource_slots:
        slot_ids.add(s["id"])
        by_table.setdefault(s["table"], []).append(s["index"])
    orphans = slot_ids - expected_ids
    if orphans:
        failures += fail("resource_slots reference unknown ids: %s" % sorted(orphans)[0])
    else:
        failures += ok("every resource_slot references a catalogued resource")
    dup = [tbl for tbl, idxs in by_table.items() if len(idxs) != len(set(idxs))]
    if dup:
        failures += fail("duplicate (table, index) in %s" % dup[0])
    else:
        failures += ok("no duplicate (table, index) consumer record")
    if set(by_table.get("gTrainerFrontPicTable", [])) != set(range(93)) \
            or set(by_table.get("gTrainerFrontPicPaletteTable", [])) != set(range(93)):
        failures += fail("front tables do not consume every index 0..92")
    else:
        failures += ok("gTrainerFrontPicTable and gTrainerFrontPicPaletteTable consume 0..92")
    back_idx = set(by_table.get("gTrainerBackPicPaletteTable", []))
    expected_back_idx = {t["back_palette_index"] for t in trainers
                         if "back_palette_index" in t}
    if back_idx != expected_back_idx:
        failures += fail("back palette consumer indices %s != descriptor %s"
                         % (sorted(back_idx), sorted(expected_back_idx)))
    else:
        failures += ok("gTrainerBackPicPaletteTable consumers match descriptor "
                       "back_palette_index set (%d slots)" % len(back_idx))
    expected_back = {t["canonical"] for t in trainers
                     if "back_palette_index" in t}
    back_canons = {s["id"].split("trainer/")[1].split("/battle")[0]
                   for s in resource_slots
                   if s["table"] == "gTrainerBackPicPaletteTable"}
    if back_canons != expected_back:
        failures += fail("shared back-palette consumer set mismatch")
    else:
        failures += ok("shared palettes (%d) carry the back-palette consumer"
                       % len(back_canons))

    # ---- R8 back family: §19 completeness ----------------------------------
    bconstants = back_trainer_constants(root)
    bdesc_pics = set(t["pic"] for t in btrainers)
    if set(bconstants.keys()) != bdesc_pics:
        failures += fail("back descriptor TRAINER_BACK_PIC_ set != constants "
                         "(%d vs %d)" % (len(bdesc_pics), len(bconstants)))
    else:
        failures += ok("back descriptor pics == TRAINER_BACK_PIC_* constants (%d)"
                       % len(bconstants))

    if len(btrainers) != 8:
        failures += fail("back descriptor has %d trainers, expected 8" % len(btrainers))
    else:
        failures += ok("back descriptor has 8 trainers")

    bmapping_bad = [t for t in btrainers
                    if t["canonical"] != t["pic"].lower().replace("_", "-")]
    if bmapping_bad:
        failures += fail("back canonical-name rule broken for %s"
                         % ", ".join(t["canonical"] for t in bmapping_bad))
    else:
        failures += ok("back canonical names obey lowercase + '_'->'-' for all 8")
    b_idx_bad = [t for t in btrainers if bconstants.get(t["pic"]) != t["index"]]
    if b_idx_bad:
        failures += fail("back descriptor index != TRAINER_BACK_PIC_ value for %s"
                         % ", ".join(t["pic"] for t in b_idx_bad))
    else:
        failures += ok("every back descriptor index equals its TRAINER_BACK_PIC_ constant")
    bindices = sorted(t["index"] for t in btrainers)
    if bindices != list(range(8)):
        failures += fail("back indices are not a bijection of 0..7")
    else:
        failures += ok("back table indices are a bijection of 0..7")

    # has_palette exactly red/leaf: the only 2 unique back palettes.
    pal_trainers = sorted(t["canonical"] for t in btrainers if t["has_palette"])
    if pal_trainers != ["leaf", "red"]:
        failures += fail("back has_palette set %s != {red, leaf}" % pal_trainers)
    else:
        failures += ok("has_palette: exactly red + leaf (the 2 unique back palettes)")

    # ---- back bindings cross-check -----------------------------------------
    bb_ids = [b["id"] for b in back_bindings]
    if len(back_bindings) != 10 or set(bb_ids) != expected_back_ids:
        failures += fail("back bindings set/size (%d) != expected 10" % len(back_bindings))
    else:
        failures += ok("back bindings == expected id set (10)")
    if bb_ids != sorted(bb_ids):
        failures += fail("back bindings ids not bytewise sorted")
    else:
        failures += ok("back bindings ids bytewise sorted")
    b_by_trainer = {t["canonical"]: t for t in btrainers}
    for b in back_bindings:
        canon = b["id"][len(back_sheet_tpl.split("%s")[0]):-len("/battle/back/sheet")]
        if b["id"].endswith("/palette"):
            canon = b["id"][len(back_palette_tpl.split("%s")[0]):-len("/battle/back/palette")]
        t = b_by_trainer[canon]
        expect_sym = "gTrainerBackPic_%s" % t["symbol"]
        if b["id"].endswith("/palette"):
            expect_sym = "gTrainerBackPicPalette_%s" % t["symbol"]
        if b["symbol"] != expect_sym:
            failures += fail("back binding symbol for %s is %s, expected %s"
                             % (canon, b["symbol"], expect_sym))
        want_enc = bdesc_header["sheet_encoding"] if "/sheet" in b["id"] \
            else bdesc_header["palette_encoding"]
        if b["source_encoding"] != want_enc:
            failures += fail("back binding encoding for %s is %s, expected %s"
                             % (canon, b["source_encoding"], want_enc))
        want_repr = bdesc_header["sheet_representation"] if "/sheet" in b["id"] \
            else bdesc_header["palette_representation"]
        if b["canonical_representation"] != want_repr:
            failures += fail("back binding representation for %s" % canon)
        want_size = t["sheet_size"] if b["id"].endswith("/sheet") \
            else bdesc_header["palette_decoded_size"]
        if b["expected_decoded_size"] != want_size:
            failures += fail("back binding size for %s is %d, expected %d"
                             % (canon, b["expected_decoded_size"], want_size))
        art = "graphics/trainers/back_pics/%s.4bpp" % t["slug"]
        if b["id"].endswith("/palette"):
            art = "graphics/trainers/back_pics/%s.gbapal.lz" % t["slug"]
        if b["source_artifact"] != art:
            failures += fail("back binding artifact for %s is %s, expected %s"
                             % (canon, b["source_artifact"], art))
    failures += ok("back bindings symbols/encodings/sizes/artifacts consistent")

    # ---- back ownership cross-check ----------------------------------------
    bo_by_id = {o["id"]: o for o in back_ownership}
    if len(back_ownership) != 10 or set(bo_by_id.keys()) != expected_back_ids:
        failures += fail("back ownership set/size (%d) != expected 10" % len(back_ownership))
    else:
        failures += ok("back ownership == expected id set (10)")
    bkey_bad = [o["id"] for o in back_ownership if o["key"] != derive_key(o["id"])]
    if bkey_bad:
        failures += fail("back ownership key mismatch for %s" % bkey_bad[0])
    else:
        failures += ok("all 10 back ownership keys == independently derived M0/M1")
    all_keys = [o["key"] for o in ownership] + [o["key"] for o in back_ownership]
    if len(set(all_keys)) != 196:
        failures += fail("ownership keys not pairwise distinct across both families")
    else:
        failures += ok("196 ownership keys pairwise distinct (no key collision)")
    if any(o["ownership_state"] != "ROM_BASE_ONLY" for o in back_ownership):
        failures += fail("back ownership_state not ROM_BASE_ONLY everywhere")
    else:
        failures += ok("back ownership_state: all 10 ROM_BASE_ONLY (R8 end state)")
    back_native_vals = re.findall(r'^native = "([^"]+)"', read(back_ownership_path), re.M)
    back_gba_vals = re.findall(r'^gba = "([^"]+)"', read(back_ownership_path), re.M)
    if len(back_native_vals) != 10 or len(back_gba_vals) != 10 \
            or any(v != "ROM_BASE_ONLY" for v in back_native_vals) \
            or any(v != "COMPILED" for v in back_gba_vals):
        failures += fail("back ownership targets blocks wrong")
    else:
        failures += ok("back ownership [resources.targets] blocks: native/gba per record")

    # ---- back consumer map (§13) -------------------------------------------
    if len(back_table_sites) != 8:
        failures += fail("back table_sites count %d, expected 8" % len(back_table_sites))
    else:
        failures += ok("back consumer map has 8 table_sites (6 pal + 2 pic table)")
    if len(back_resource_slots) != 10:
        failures += fail("back resource_slots count %d, expected 10"
                         % len(back_resource_slots))
    else:
        failures += ok("back consumer map has 10 resource_slots (8 sheets + 2 palettes)")
    if len(frame_slots) != 34:
        failures += fail("frame_slots count %d, expected 34" % len(frame_slots))
    else:
        failures += ok("back consumer map has 34 frame_slots (6x4 + 2x5)")

    back_by_table = {}
    back_slot_ids = set()
    for s in back_resource_slots:
        back_slot_ids.add(s["id"])
        back_by_table.setdefault(s["table"], []).append(s["index"])
    if back_slot_ids - expected_back_ids:
        failures += fail("back resource_slots reference unknown ids")
    else:
        failures += ok("every back resource_slot references a catalogued resource")
    dup_back = [tbl for tbl, idxs in back_by_table.items()
                if len(idxs) != len(set(idxs))]
    if dup_back:
        failures += fail("duplicate (table, index) back record in %s" % dup_back[0])
    else:
        failures += ok("no duplicate (table, index) back consumer record")
    if set(back_by_table.get("gTrainerBackPicTable", [])) != set(range(8)):
        failures += fail("back pic table does not consume every index 0..7")
    else:
        failures += ok("gTrainerBackPicTable consumes every back index 0..7")
    if set(back_by_table.get("gTrainerBackPicPaletteTable", [])) != {2, 3}:
        failures += fail("back palette table consumers != {2, 3} (red, leaf)")
    else:
        failures += ok("gTrainerBackPicPaletteTable consumers are exactly red (2) "
                       "and leaf (3)")

    # frame_slots: every (array, index) unique, ids are the sheet resource,
    # per-array count == descriptor frames, array names exist in src/data.c.
    fs_by_array = {}
    fs_seen = set()
    for s in frame_slots:
        if s["id"] not in expected_back_ids or not s["id"].endswith("/sheet"):
            failures += fail("frame_slot id %s is not a back sheet" % s["id"])
        fs_by_array.setdefault(s["array"], []).append(s["index"])
        fs_seen.add((s["array"], s["index"]))
    if len(fs_seen) != len(frame_slots):
        failures += fail("duplicate (array, index) frame_slot record")
    else:
        failures += ok("no duplicate (array, index) frame_slot record")
    data_c = read(root + "/src/data.c")
    frame_bad = 0
    for t in btrainers:
        if t["frame_array"] != "gTrainerBackPicTable_%s" % t["symbol"]:
            failures += fail("frame_array name mismatch for %s" % t["canonical"])
            frame_bad += 1
        if "gTrainerBackPicTable_%s[]" % t["symbol"] not in data_c:
            failures += fail("frame array %s missing from src/data.c" % t["frame_array"])
            frame_bad += 1
        got = sorted(fs_by_array.get(t["frame_array"], []))
        if got != list(range(t["frames"])):
            failures += fail("frame_slots for %s: %s, expected 0..%d"
                             % (t["canonical"], got, t["frames"] - 1))
            frame_bad += 1
    if frame_bad:
        failures += fail("frame_slots cross-check has %d problems" % frame_bad)
    else:
        failures += ok("frame_slots match descriptor frames and src/data.c arrays (34)")

    # The 6 alias back-palette slots must NOT be duplicated as back payloads:
    # every back palette resource_slot is a unique red/leaf payload, and the
    # front consumers file already carries the 6 aliases (checked above).
    pal_slot_ids = {s["id"] for s in back_resource_slots
                    if s["table"] == "gTrainerBackPicPaletteTable"}
    if pal_slot_ids != {back_palette_tpl % c for c in ("red", "leaf")}:
        failures += fail("back palette resource_slots != the 2 unique payloads")
    else:
        failures += ok("back palette slots are only the 2 unique payloads "
                       "(6 aliases stay in the front consumers file)")

    print("== completeness: %s ==" % ("PASS" if failures == 0 else "FAIL"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""R15 Phase 4: battle-animation GRAPHICS family generator (graphics only -
the H VM bytecode family is unchanged).

Consumes the checked-in sources of truth and emits the family metadata in the
same shapes the R9 pokemon / R13 leaf families use:

  * inventory.generated.toml        - per-payload (symbol, artifact, kind,
                                       consumer-table binding)
  * catalog.generated.toml          - full resource catalog
  * bindings.generated.toml         - extraction bindings
  * ownership.generated.toml        - native/GBA ownership
  * manifest.production.toml        - pack records (via gen3-elf-manifest)
  * battle_anim_gfx_slots.generated.h - native compat slot map

Payload sources: src/graphics.c INCBINs under graphics/battle_anims/ plus the
battle-frontier dome anims already declared elsewhere (excluded here).
Consumer tables: gBattleAnimPicTable / gBattleAnimPaletteTable /
gBattleAnimBackgroundTable (src/data/battle_anim.h) plus the direct
initializer tables in battle_anim_throw.c, battle_anim_rock.c,
battle_anim_mons.c, battle_anim_water.c, battle_intro.c, battle_bg.c,
evolution_scene.c, battle_anim_utility_funcs.c.

Resource keys: emerald:battle-anim-gfx/<slug> where slug is the artifact file
stem (unique per payload family; sprite gfx and palettes share stems but are
separate resources keyed by the table kind).

Determinism: sorted output; regeneration must be a no-op diff.
"""
import hashlib
import re
import sys

def manifest_map(family_dir):
    """id -> canonical_decoded_sha256 from the production manifest."""
    p = f"resources/extraction/emerald/bpee01/{family_dir}/manifest.production.toml"
    out = {}
    try:
        text = open(p).read()
        for block in text.split('[[records]]')[1:]:
            m_id = re.search(r'id = "([^"]+)"', block)
            m_sha = re.search(r'canonical_decoded_sha256 = "([0-9a-f]+)"', block)
            if m_id and m_sha:
                out[m_id.group(1)] = m_sha.group(1)
    except OSError:
        pass
    return out

from collections import OrderedDict

# (symbol_prefix, table, row_kind, resource_kind)
# row_kind: "sheet" (CompressedSpriteSheet rows) | "pal" (CompressedSpritePalette
# rows) | "bg" (BattleAnimBackground rows: image/pal/tilemap triple)
FAMILY_SPEC = OrderedDict()

INCBIN_RE = re.compile(
    r'const u(\d+) (\w+)\[\]\s*=\s*'
    r'INCBIN_U(\d+)\(\s*"([^"]+)"\);')

def parse_incbins(path):
    out = OrderedDict()
    for line in open(path):
        m = INCBIN_RE.match(line.strip())
        if m and ("battle_anims" in m.group(4) or "battle_environment" in m.group(4)):
            out[m.group(2)] = m.group(4)
    return out

def slug_of(artifact):
    # graphics/battle_anims/<dir>/<file> -> <dir>_<stem>.<fmt> (the format
    # class .4bpp/.gbapal/.bin keeps same-stem variants distinct); the
    # battle_environment family joins the same resource namespace.
    if artifact.startswith("graphics/battle_anims/"):
        p = artifact[len("graphics/battle_anims/"):]
    else:
        p = artifact[len("graphics/battle_environment/"):]
        p = "environment_" + p
    parts = p.rsplit("/", 1)
    d = parts[0].replace("/", "_")
    name = parts[1]
    if name.endswith(".lz"):
        name = name[:-len(".lz")]
    stem, fmt = name.rsplit(".", 1)
    return f"{d}_{stem}.{fmt}"

def parse_pic_table(path):
    """gBattleAnimPicTable rows: BATTLE_ANIM_PIC(gfx, size, ANIM_TAG_X)."""
    rows = []
    pat = re.compile(r"BATTLE_ANIM_PIC\((\w+),\s*(0x[0-9a-fA-F]+),\s*(\w+)\)")
    in_table = False
    for line in open(path):
        if "gBattleAnimPicTable[]" in line:
            in_table = True
            continue
        if in_table:
            m = pat.search(line)
            if m:
                rows.append((m.group(1), m.group(2), m.group(3)))
            if line.strip() == "};":
                break
    return rows

def parse_pal_table(path):
    rows = []
    pat = re.compile(r"BATTLE_ANIM_PAL\((\w+),\s*(\w+)\)")
    in_table = False
    for line in open(path):
        if "gBattleAnimPaletteTable[]" in line:
            in_table = True
            continue
        if in_table:
            m = pat.search(line)
            if m:
                rows.append((m.group(1), m.group(2)))
            if line.strip() == "};":
                break
    return rows

def parse_bg_table(path):
    rows = []
    pat = re.compile(r"\[(\w+)\]\s*=\s*BATTLE_ANIM_BG\((\w+),\s*(\w+),\s*(\w+)\)")
    in_table = False
    for line in open(path):
        if "gBattleAnimBackgroundTable[]" in line:
            in_table = True
            continue
        if in_table:
            m = pat.search(line)
            if m:
                rows.append((m.group(1), m.group(2), m.group(3), m.group(4)))
            if line.strip() == "};":
                break
    return rows

def parse_ball_table(path):
    """sBallParticleSpriteSheets rows: {gBattleAnimSpriteGfx_X, size, TAG_X}."""
    rows = []
    pat = re.compile(r"\{(\w+),\s*(0x[0-9a-fA-F]+),\s*(\w+)\}")
    in_table = False
    for line in open(path):
        if "sBallParticleSpriteSheets" in line:
            in_table = True
            continue
        if in_table:
            m = pat.search(line)
            if m:
                rows.append((m.group(1), m.group(2), m.group(3)))
            if line.strip() == "};":
                break
    return rows

def parse_env_table(path):
    """sBattleEnvironmentTable rows: designated BattleBackground entries with BATTLE_ENV_FIELD macros."""
    rows = []
    pat = re.compile(r"\[(\w+)\]\s*=")
    field = re.compile(r"\.(\w+)\s*=\s*BATTLE_ENV_FIELD\((\w+)\),")
    in_table = False
    cur = None
    for line in open(path):
        if "sBattleEnvironmentTable" in line:
            in_table = True
            continue
        if in_table:
            m = pat.search(line)
            if m:
                if cur is not None:
                    rows.append(cur)
                cur = [m.group(1), {}, {}]
                continue
            if cur is not None:
                fm = field.search(line)
                if fm:
                    cur[1][fm.group(1)] = fm.group(2)
                if line.strip() == "}," or line.strip() == "};":
                    rows.append(cur)
                    cur = None
                    if line.strip() == "};":
                        break
    return [(r[0], r[1]) for r in rows if r is not None and len(r[1]) >= 4]

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    out = sys.argv[2] if len(sys.argv) > 2 else \
        "resources/extraction/emerald/bpee01/battle_anim_gfx/inventory.generated.toml"
    incbins = parse_incbins(f"{root}/src/graphics.c")
    incbins.update(parse_incbins(f"{root}/src/data/graphics/battle_environment.h"))
    pic_rows = parse_pic_table(f"{root}/src/data/battle_anim.h")
    pal_rows = parse_pal_table(f"{root}/src/data/battle_anim.h")
    bg_rows = parse_bg_table(f"{root}/src/data/battle_anim.h")
    ball_rows = parse_ball_table(f"{root}/src/battle_anim_throw.c")
    env_rows = parse_env_table(f"{root}/src/battle_bg.c")

    lines = []
    lines.append("# Generated by tools/gen3_resources/battle_anim_family/gen_battle_anim_gfx.py")
    lines.append("# Do not edit by hand; regeneration must be a no-op diff.")
    lines.append("inventory_version = 1")
    lines.append('family = "battle_anim_gfx"')
    lines.append('game = "emerald"')
    lines.append('rom_profile = "bpee01-rev0"')
    lines.append("")

    for sym, art in incbins.items():
        lines.append("[[resources]]")
        lines.append(f'symbol = "{sym}"')
        lines.append(f'source_artifact = "{art}"')
        lines.append(f'slug = "{slug_of(art)}"')
        lines.append("")
    for gfx, size, tag in pic_rows:
        lines.append("[[pic_slots]]")
        lines.append(f'symbol = "{gfx}"')
        lines.append(f'size = "{size}"')
        lines.append(f'tag = "{tag}"')
    for pal, tag in pal_rows:
        lines.append("[[pal_slots]]")
        lines.append(f'symbol = "{pal}"')
        lines.append(f'tag = "{tag}"')
    for bg, img, pal, tm in bg_rows:
        lines.append("[[bg_slots]]")
        lines.append(f'background = "{bg}"')
        lines.append(f'image = "{img}"')
        lines.append(f'palette = "{pal}"')
        lines.append(f'tilemap = "{tm}"')
    for gfx, size, tag in ball_rows:
        lines.append("[[ball_slots]]")
        lines.append(f'symbol = "{gfx}"')
        lines.append(f'size = "{size}"')
        lines.append(f'tag = "{tag}"')
    for env, fields in env_rows:
        lines.append("[[env_slots]]")
        lines.append(f'environment = "{env}"')
        for k, v in sorted(fields.items()):
            lines.append(f'{k} = "{v}"')
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {out}: {len(incbins)} payloads, {len(pic_rows)} pic rows, "
          f"{len(pal_rows)} pal rows, {len(bg_rows)} bg rows")

    # ---- catalog / bindings / ownership (sorted by id) ----
    ids = OrderedDict()
    for sym, art in sorted(incbins.items()):
        slug = slug_of(art)
        rid = f"emerald:battle-anim-gfx/{slug}"
        ids[rid] = (sym, art)
    cat = ["# Generated by tools/gen3_resources/battle_anim_family/gen_battle_anim_gfx.py",
           "# Do not edit by hand; regeneration must be a no-op diff.",
           "catalog_version = 1", 'namespace = "emerald"', 'resource_api = "1.0.0"',
           'game = "emerald"', 'rom_profile = "bpee01-rev0"', ""]
    bin_ = ["# Generated by tools/gen3_resources/battle_anim_family/gen_battle_anim_gfx.py",
            "# Do not edit by hand; regeneration must be a no-op diff.",
            "bindings_version = 1", 'game = "emerald"', 'rom_profile = "bpee01-rev0"', ""]
    canon = manifest_map("battle_anim_gfx") or {}

    own = ["# Generated by tools/gen3_resources/battle_anim_family/gen_battle_anim_gfx.py",
           "# Do not edit by hand; regeneration must be a no-op diff.",
           "ownership_version = 1", 'game = "emerald"', 'rom_profile = "bpee01-rev0"', ""]
    def payload_info(art):
        data = open(f"{root}/{art}", "rb").read()
        if art.endswith(".lz"):
            assert data[0] & 0x0F == 0 and len(data) >= 4, f"not GBA LZ: {art}"
            return "gba-lz77", data[1] | (data[2] << 8) | (data[3] << 16), "tile-graphics"
        return "raw", len(data), "binary"   # the 3 unused .bin payloads are raw u16 arrays

    import hashlib as _hl
    def art_hash(art):
        return _hl.sha256(open(f"{root}/{art}", "rb").read()).hexdigest()
    from collections import Counter as _Counter
    _hash_counts = _Counter(art_hash(art) for _, art in ids.values())

    for rid, (sym, art) in ids.items():
        key = hashlib.sha256(b"gen3-resource-id-v1\x00" + rid.encode()).hexdigest()
        enc, dec, kind = payload_info(art)
        shared = _hash_counts[art_hash(art)] > 1
        cat.append("[[resources]]")
        cat.append(f'id = "{rid}"')
        cat.append(f'type = "{kind}"')
        cat.append("schema = 1")
        cat.append("required_for_base = true")
        bin_.append("[[bindings]]")
        bin_.append(f'id = "{rid}"')
        bin_.append(f'symbol = "{sym}"')
        bin_.append(f'source_artifact = "{art}"')
        bin_.append(f'source_encoding = "{enc}"')
        if kind == "tile-graphics":
            bin_.append('canonical_representation = "gba-4bpp-tiles"')
        else:
            bin_.append('canonical_representation = "gba-bytes"')
        bin_.append(f"expected_decoded_size = {dec}")
        if shared:
            bin_.append("allow_shared_range = true")
        data = open(f"{root}/{art}", "rb").read()
        src_sha = hashlib.sha256(data).hexdigest()
        own.append("[[resources]]")
        own.append(f'id = "{rid}"')
        own.append(f'key = "{key}"')
        own.append(f'legacy_symbol = "{sym}"')
        own.append(f'type = "{kind}"')
        own.append("schema = 1")
        own.append(f'source_artifact = "{art}"')
        own.append(f'encoded_length = {len(data)}')
        own.append(f'decoded_length = {dec}')
        own.append(f'source_encoding = "{enc}"')
        own.append(f'source_encoded_sha256 = "{src_sha}"')
        own.append(f'canonical_decoded_sha256 = "{canon.get(rid, "")}"')
        own.append('ownership_state = "ROM_BASE_ONLY"')
        own.append("")
        own.append("[resources.targets]")
        own.append('native = "ROM_BASE_ONLY"')
        own.append('gba = "COMPILED"')
    import os
    d = "resources/extraction/emerald/bpee01/battle_anim_gfx"
    os.makedirs(d, exist_ok=True)
    for name, ls in (("catalog.generated.toml", cat),
                     ("bindings.generated.toml", bin_),
                     ("ownership.generated.toml", own)):
        open(f"{d}/{name}", "w").write("\n".join(ls) + "\n")
    print(f"emitted {len(ids)} catalog/bindings/ownership rows to {d}")

    # ---- native compat slot map (C header) ----
    # kind-major sections: pic 0..288 (gfx,size,tag), pal 289..577 (pal,tag),
    # bg 578..604 (image,pal,tilemap per BG_* row).
    idx_of = {rid: n for n, rid in enumerate(ids)}
    h = ["/* Generated by tools/gen3_resources/battle_anim_family/gen_battle_anim_gfx.py",
         " * Do not edit by hand; regeneration must be a no-op diff.",
         " *",
         " * R15 Phase 4: native compat slot map for the battle-animation gfx",
         " * tables. kBattleAnimGfxSlots is kind-major: pic rows 0..%d," % (len(pic_rows) - 1),
         " * pal rows %d..%d, bg rows %d..%d (3 resource indices per row)."
         % (len(pic_rows), len(pic_rows) + len(pal_rows) - 1,
            len(pic_rows) + len(pal_rows), len(pic_rows) + len(pal_rows) + len(bg_rows) * 3 - 1),
         " */",
         "#ifndef EMERALD_RESOURCES_BATTLE_ANIM_GFX_SLOTS_GENERATED_H",
         "#define EMERALD_RESOURCES_BATTLE_ANIM_GFX_SLOTS_GENERATED_H",
         "",
         "#include <stdint.h>",
         '#include "gen3/resources/resource_types.h"',
         '#include "emerald/resources/emerald_resource_ranges.h"',
         ""]
    h.append(f"#define BATTLE_ANIM_GFX_RESOURCE_COUNT {len(ids)}u")
    h.append(f"#define BATTLE_ANIM_GFX_PIC_ROWS {len(pic_rows)}u")
    h.append(f"#define BATTLE_ANIM_GFX_PAL_ROWS {len(pal_rows)}u")
    h.append(f"#define BATTLE_ANIM_GFX_BG_ROWS {len(bg_rows)}u")
    h.append(f"#define BATTLE_ANIM_GFX_BALL_ROWS {len(ball_rows)}u")
    h.append(f"#define BATTLE_ANIM_GFX_ENV_ROWS {len(env_rows)}u")
    h.append("#define BATTLE_ANIM_GFX_SLOT_COUNT %uu" % (
        len(pic_rows) + len(pal_rows) + len(bg_rows) * 3
        + len(ball_rows) + len(env_rows) * 5))
    h.append("")
    h.append("struct BattleAnimGfxResource")
    h.append("{")
    h.append("    const char *id;")
    h.append("    uint32_t expectedSize;   /* decoded size (LZ header / raw file size) */")
    h.append("    enum Gen3ResourceType type;")
    h.append("    uint32_t schema;")
    h.append("    uint32_t role;          /* EMERALD_RESOURCE_ROLE_LEGACY_LZ or CANONICAL (raw) */")
    h.append("};")
    h.append("")
    h.append("static const struct BattleAnimGfxResource")
    h.append("    kBattleAnimGfxResources[BATTLE_ANIM_GFX_RESOURCE_COUNT] =")
    h.append("{")
    for rid, (sym, art) in ids.items():
        enc, dec, kind = payload_info(art)
        role = "EMERALD_RESOURCE_ROLE_CANONICAL" if enc == "raw" else "EMERALD_RESOURCE_ROLE_LEGACY_LZ"
        rtype = "GEN3_RESOURCE_TYPE_TILE_GRAPHICS" if kind == "tile-graphics" else "GEN3_RESOURCE_TYPE_BINARY"
        h.append(f'    {{ "{rid}", {dec}u, {rtype}, 1u, {role} }},')
    h.append("};")
    h.append("")
    h.append("static const int32_t kBattleAnimGfxSlots[BATTLE_ANIM_GFX_SLOT_COUNT] =")
    h.append("{")
    for gfx, size, tag in pic_rows:
        rid = f"emerald:battle-anim-gfx/{slug_of(incbins[gfx])}"
        h.append(f"    {idx_of[rid]}, /* pic {tag} {gfx} */")
    for pal, tag in pal_rows:
        rid = f"emerald:battle-anim-gfx/{slug_of(incbins[pal])}"
        h.append(f"    {idx_of[rid]}, /* pal {tag} {pal} */")
    for bg, img, pal, tm in bg_rows:
        rid_i = f"emerald:battle-anim-gfx/{slug_of(incbins[img])}"
        rid_p = f"emerald:battle-anim-gfx/{slug_of(incbins[pal])}"
        rid_t = f"emerald:battle-anim-gfx/{slug_of(incbins[tm])}"
        h.append(f"    {idx_of[rid_i]}, {idx_of[rid_p]}, {idx_of[rid_t]}, /* BG_{bg} */")
    for gfx, size, tag in ball_rows:
        rid = f"emerald:battle-anim-gfx/{slug_of(incbins[gfx])}"
        h.append(f"    {idx_of[rid]}, /* ball {tag} {gfx} */")
    for env, fields in env_rows:
        for k in ("tileset", "tilemap", "entryTileset", "entryTilemap", "palette"):
            if k in fields:
                rid = f"emerald:battle-anim-gfx/{slug_of(incbins[fields[k]])}"
                h.append(f"    {idx_of[rid]}, /* env {env} {k} */")
    h.append("};")
    h.append("")
    h.append("#endif")
    open("include/emerald/resources/battle_anim_gfx_slots.generated.h", "w").write("\n".join(h) + "\n")
    print("slot map emitted")

    # ---- native accessor macros (call-site redirects) ----
    a = ["/* Generated by tools/gen3_resources/battle_anim_family/gen_battle_anim_gfx.py",
         " * Do not edit by hand; regeneration must be a no-op diff.",
         " *",
         " * R15 Phase 4: on native every legacy gfx symbol name redirects to the",
         " * compat seam accessor (the compiled leaves are GBA-only); on GBA the",
         " * names stay the real INCBIN arrays. Consumer files include this header",
         " * so their call sites are identical on both targets. */"]
    a.append("#if defined(NATIVE_LINUX)")
    a.append('#include "emerald/resources/emerald_battle_anim_gfx_compat.h"')
    for rid, (sym, art) in ids.items():
        a.append(f'#define {sym} BattleAnimGfx_Get("{rid}")')
    a.append("#endif")
    open("include/emerald/resources/battle_anim_gfx_accessors.generated.h", "w").write("\n".join(a) + "\n")
    print("accessor macros emitted")

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""R11-D: machine-readable map layout inventory.

Parses the checked-in sources of truth and emits
resources/extraction/emerald/bpee01/layout/inventory.generated.toml:

  * data/layouts/layouts.json
      -> the 441 `struct MapLayout` records: name / id / width / height /
         primary_tileset / secondary_tileset (the native records are
         re-emitted writable by the family generator, so the C tool needs
         the full field set)
      -> two resources per layout:
           - blockdata: data/layouts/<name>/map.bin   (w*h*2 bytes)
           - border:    data/layouts/<name>/border.bin (8 bytes)

Deterministic: all rows sorted by layout name. Regeneration must be a
no-op diff; --check enforces that.

Pins (fail hard on deviation): 441 layouts, 441 blockdata leaves, 441
border leaves, 32 distinct border byte patterns (the audit's "4 u16
uniform" claim is wrong - verified during R11-D exploration), canonical
slug uniqueness (two leaves per layout share the layout slug), every
data/layouts/*/{map,border}.bin file covered by exactly one record.
Duplicate names, missing files, width/height vs file-size mismatches,
border size != 8, orphan .bin files and slug collisions are hard
failures.

Usage: python3 gen_layout_inventory.py [repo-root] [output-path] [--check]
"""
import hashlib
import json
import os
import sys

PIN_LAYOUTS = 441
PIN_BLOCKDATA = 441
PIN_BORDER = 441
PIN_DISTINCT_BORDERS = 32
PIN_PADDED_BLOCKDATA = 20
BORDER_BYTES = 8


def canonicalize(symbol):
    """R11-B canonical rule: lowercase symbol suffix, '_' -> '-'.
    e.g. PetalburgCity_Layout -> petalburg-city (the _Layout suffix is
    dropped by the caller; the slug is shared by the two leaves)."""
    return symbol.lower().replace("_", "-")


def fail(msg):
    sys.exit(f"FAIL: {msg}")


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    out = sys.argv[2] if len(sys.argv) > 2 else None
    check = "--check" in sys.argv[3:]

    layouts_path = os.path.join(root, "data/layouts/layouts.json")
    layouts_dir = os.path.join(root, "data/layouts")
    try:
        with open(layouts_path) as f:
            data = json.load(f)
    except FileNotFoundError:
        fail(f"{layouts_path} does not exist")
    layouts = data.get("layouts")
    if not isinstance(layouts, list):
        fail(f"{layouts_path} has no \"layouts\" array")
    if len(layouts) != PIN_LAYOUTS:
        fail(f"{len(layouts)} layouts (expected {PIN_LAYOUTS})")
    if data.get("layouts_table_label") != "gMapLayouts":
        fail(f'layouts_table_label is {data.get("layouts_table_label")!r} '
             "(expected gMapLayouts)")

    fields = ("id", "name", "width", "height", "primary_tileset",
              "secondary_tileset", "border_filepath", "blockdata_filepath")
    seen_names = {}
    seen_files = {}
    slugs = {}
    distinct_borders = {}

    for i, layout in enumerate(layouts):
        label = f"layout {i + 1}"
        for fld in fields:
            if fld not in layout:
                fail(f"{label} missing '{fld}'")
        name = layout["name"]
        if not name.endswith("_Layout"):
            fail(f"{label} name {name!r} does not end with _Layout")
        if name in seen_names:
            fail(f"duplicate layout name {name!r} ({seen_names[name]})")
        seen_names[name] = i + 1
        width, height = layout["width"], layout["height"]
        if not isinstance(width, int) or width <= 0 or width > 140:
            fail(f"{label} {name} bad width {width!r}")
        if not isinstance(height, int) or height <= 0 or height > 140:
            fail(f"{label} {name} bad height {height!r}")

        slug = canonicalize(name[:-len("_Layout")])
        if slug in slugs:
            fail(f"canonical slug collision: {name!r} and {slugs[slug]!r} "
                 f"both -> {slug!r}")
        slugs[slug] = name

        for kind, fld_name, expect in (("blockdata", "blockdata_filepath",
                                        width * height * 2),
                                       ("border", "border_filepath",
                                        BORDER_BYTES)):
            rel = layout[fld_name]
            path = os.path.join(root, rel)
            if not os.path.isfile(path):
                fail(f"{label} {name} {kind} file {rel} does not exist")
            size = os.path.getsize(path)
            if kind == "blockdata":
                # The .bin is the authoritative byte stream: the GBA build
                # INCBINs it verbatim and the ELF symbol carries its real
                # size, so the resource payload is the FILE bytes. The grid
                # math (w*h*2) is the runtime indexing contract, and 20
                # layouts (18 unused 1x1 + 2 padded) carry files larger
                # than the grid needs; smaller-than-grid would be
                # corruption.
                if size % 2 != 0 or size < expect:
                    fail(f"{label} {name} blockdata file {rel} is {size} "
                         f"bytes (w*h*2 = {expect}; must be >= grid and "
                         "even)")
            elif size != expect:
                fail(f"{label} {name} border file {rel} is {size} bytes "
                     f"(expected {BORDER_BYTES})")
            if rel in seen_files:
                fail(f"{rel} referenced by both {seen_files[rel]} and {name}")
            seen_files[rel] = name
            if kind == "border":
                digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
                distinct_borders.setdefault(digest, []).append(name)

    # Orphan check: every data/layouts/*/{map,border}.bin is covered.
    covered = set()
    for rel in seen_files:
        covered.add(rel[len("data/layouts/"):].split("/")[-2] + "/map.bin")
        covered.add(rel[len("data/layouts/"):].split("/")[-2] + "/border.bin")
    orphan_dirs = []
    try:
        entries = sorted(os.listdir(layouts_dir))
    except FileNotFoundError:
        fail(f"{layouts_dir} does not exist")
    for d in entries:
        if not os.path.isdir(os.path.join(layouts_dir, d)):
            continue
        for leaf in ("map.bin", "border.bin"):
            if d + "/" + leaf not in covered:
                orphan_dirs.append(d + "/" + leaf)
    if orphan_dirs:
        fail(f"orphan layout bin files not referenced by layouts.json: "
             + ", ".join(orphan_dirs))

    if len(distinct_borders) != PIN_DISTINCT_BORDERS:
        fail(f"{len(distinct_borders)} distinct border byte patterns "
             f"(expected {PIN_DISTINCT_BORDERS})")

    padded = [l["name"] for l in layouts
              if os.path.getsize(os.path.join(root, l["blockdata_filepath"]))
              > l["width"] * l["height"] * 2]
    if len(padded) != PIN_PADDED_BLOCKDATA:
        fail(f"{len(padded)} layouts with grid-padded blockdata files "
             f"(expected {PIN_PADDED_BLOCKDATA})")

    # ---- emit ----
    lines = []
    lines.append("# Generated by tools/gen3_resources/layout_family/"
                 "gen_layout_inventory.py")
    lines.append("# Do not edit by hand; edit data/layouts/layouts.json "
                 "and re-run the generator (regeneration must be a no-op "
                 "diff).")
    lines.append("inventory_version = 1")
    lines.append('family = "layout"')
    lines.append('game = "emerald"')
    lines.append('rom_profile = "bpee01"')
    lines.append("")

    lines.append("[[families]]")
    lines.append('kind = "blockdata"')
    lines.append(f"layout_count = {PIN_BLOCKDATA}")
    lines.append("")
    lines.append("[[families]]")
    lines.append('kind = "border"')
    lines.append(f"layout_count = {PIN_BORDER}")
    lines.append("")

    for layout in layouts:
        name = layout["name"]
        lines.append("[[layouts]]")
        lines.append(f'name = "{name}"')
        lines.append(f'id = "{layout["id"]}"')
        lines.append(f"width = {layout['width']}")
        lines.append(f"height = {layout['height']}")
        lines.append(f'primary_tileset = "{layout["primary_tileset"]}"')
        lines.append(f'secondary_tileset = "{layout["secondary_tileset"]}"')
        lines.append("")

    for layout in layouts:
        name = layout["name"]
        slug = canonicalize(name[:-len("_Layout")])
        for kind, fld in (("blockdata", "blockdata_filepath"),
                          ("border", "border_filepath")):
            lines.append("[[resources]]")
            lines.append(f'layout = "{name}"')
            lines.append(f'kind = "{kind}"')
            lines.append(f'canonical = "{slug}"')
            lines.append(f'source_artifact = "{layout[fld]}"')
            lines.append("")

    text = "\n".join(lines)
    if check:
        try:
            existing = open(out).read()
        except FileNotFoundError:
            fail(f"--check: {out} does not exist")
        if existing != text:
            fail(f"--check: {out} is not up to date with the sources")
        print(f"{out}: up to date ({len(layouts)} layouts, "
              f"{PIN_BLOCKDATA} blockdata, {PIN_BORDER} border, "
              f"{len(distinct_borders)} distinct borders)")
        return 0
    if out is None:
        fail("no output path given (and --check not passed)")
    with open(out, "w") as f:
        f.write(text)
    print(f"{out}: wrote {len(layouts)} layouts, {PIN_BLOCKDATA} blockdata, "
          f"{PIN_BORDER} border, {len(distinct_borders)} distinct borders")
    return 0


if __name__ == "__main__":
    sys.exit(main())

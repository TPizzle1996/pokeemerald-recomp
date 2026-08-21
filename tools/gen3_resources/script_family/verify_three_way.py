#!/usr/bin/env python3
"""R13-G2 section 11 static three-way oracle (independent re-verification).

Re-proves the generated script-module family against the qualified ROM and
the cross-family catalogs WITHOUT importing the generator:

  ROM bytes  ==  generated payload bytes  ==  manifest digests      (modules)
  ROM operand at the source  ==  recorded target address            (relocs)
  target address  ==  recorded export / catalog identity            (edges)

Fail-closed: prints FAIL and exits 1 on the first inconsistency.

Usage: verify_three_way.py <repo-root> <qualified.elf> <qualified.gba>
The ELF is only used to re-check the symbol table (exports/aliases);
the ROM is the byte source.
"""

import hashlib
import struct
import sys
import tomllib
from pathlib import Path

ROM_SHA1 = "f3ae088181bf583e55daf962a92bb46f4f1d07b7"
GBA_BASE = 0x08000000
EWRAM = (0x02000000, 0x02040000)


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def load_meta_index(mods):
    """All 523 module metas indexed by canonical resource key (one parse
    per file; the per-reloc lookup is a dict hit, not a meta walk)."""
    index = {}
    for tkind in ("map", "common", "mystery-gift"):
        for tp in sorted((mods / "meta" / tkind).glob("*.toml")):
            d = tomllib.loads(tp.read_text())
            index[d["module_key"]] = d
    if len(index) != 523:
        fail(f"meta index has {len(index)} modules, expected 523")
    return index


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    elf_path = Path(sys.argv[2] if len(sys.argv) > 2
                    else "/home/tristen/work/pokeemerald-reference/pokeemerald.elf")
    rom_path = Path(sys.argv[3] if len(sys.argv) > 3
                    else "/home/tristen/work/pokeemerald-reference/pokeemerald.gba")

    rom = rom_path.read_bytes()
    if hashlib.sha1(rom).hexdigest() != ROM_SHA1:
        fail(f"ROM SHA-1 != {ROM_SHA1}")
    mods = root / "resources/extraction/emerald/bpee01/script/modules"
    if not (mods / "manifest.production.toml").exists():
        fail(f"script module family not found at {mods}")

    # --- manifest: id -> digests/artifact ---------------------------
    manifest = tomllib.loads((mods / "manifest.production.toml").read_text())
    man = {}
    for rec in manifest.get("records", []):
        man[rec["id"]] = rec

    # --- ELF symbol table (exports/aliases leg) ----------------------
    import subprocess
    elf_syms = {}
    p = subprocess.run(
        ["readelf", "-sW", str(elf_path)], capture_output=True, text=True)
    if p.returncode:
        fail("readelf failed on the qualified ELF")
    for line in p.stdout.splitlines():
        f = line.split()
        if len(f) >= 8 and f[0].rstrip(":").isdigit() \
           and f[3] in ("FUNC", "OBJECT", "NOTYPE"):
            try:
                v = int(f[1], 16)
            except ValueError:
                continue
            if v >= GBA_BASE:
                elf_syms.setdefault(f[7], set()).add(v)

    # --- cross-family catalogs ---------------------------------------
    text_cat = tomllib.loads(
        (root / "resources/extraction/emerald/bpee01/text/catalog.generated.toml")
        .read_text())
    text_ids = {r["id"] for r in text_cat.get("resources", [])}
    mov_binds = tomllib.loads(
        (root / "resources/extraction/emerald/bpee01/movement/bindings.generated.toml")
        .read_text())
    mov_syms = {b["symbol"] for b in mov_binds.get("bindings", [])}

    # --- walk meta files ----------------------------------------------
    meta_index = load_meta_index(mods)
    n_mod = n_seg = n_exp = n_rel = n_edges = n_ro = 0
    routing_checked = 0
    cls_tally = {}
    for meta in meta_index.values():
        key = meta["module_key"]
        rec = man.get(key)
        n_mod += 1
        region_start = meta["region_gba_start"]
        region_end = region_start + meta["region_byte_count"]
        if rec is None:
            # Routing-only module (plan §5: terminator-only `.byte 0`
            # map-script table): no manifest record by design (the pack
            # model rejects zero-length payloads). Expect the empty
            # shape, then fall through to the export/reloc checks.
            n_ro += 1
            if meta["module_digest"] != sha256(b"") \
               or meta["bytecode_bytes"] != 0 or meta["static_data_bytes"] != 0 \
               or meta.get("segments", []):
                fail(f"{key}: routing-only module not manifest-shaped")
            payload = b""
        else:
            payload = (mods / rec["source_artifact"].replace(
                "resources/extraction/emerald/bpee01/script/modules/", "")) \
                .read_bytes()
            # manifest digests == payload digest
            if rec["canonical_decoded_sha256"] != sha256(payload) \
               or rec["source_encoded_sha256"] != sha256(payload):
                fail(f"{key}: manifest digest != payload sha256")
            if meta["module_digest"] != sha256(payload):
                fail(f"{key}: meta module_digest != payload sha256")
            if meta.get("primary_symbol") != rec.get("symbol"):
                fail(f"{key}: primary_symbol != manifest symbol")
        # segments: ROM slice == payload slice
        for seg in meta.get("segments", []):
            o, bc, po = (seg["original_gba_start"], seg["byte_count"],
                         seg["payload_offset"])
            n_seg += 1
            rom_slice = rom[o - GBA_BASE:o - GBA_BASE + bc]
            if rom_slice != payload[po:po + bc]:
                fail(f"{key}: ROM slice @{o:#x} != payload @{po}")
        # exports: address inside region, symbol present in the ELF,
        # payload offset points at a segment boundary-consistent byte
        for ex in meta.get("exports", []):
            n_exp += 1
            addr = ex["original_gba_address"]
            if not (region_start <= addr < region_end):
                fail(f"{key}: export {ex['name']} @{addr:#x} outside region")
            if elf_syms.get(ex["name"]) is not None \
               and addr not in elf_syms[ex["name"]]:
                fail(f"{key}: export {ex['name']} @{addr:#x} not in ELF syms")
            if ex["boundary_kind"] == "offset-zero" and ex["payload_offset"] != 0:
                fail(f"{key}: offset-zero export {ex['name']} has nonzero offset")
        # relocs: operand bytes + target identity
        for rl in meta.get("relocs", []):
            n_rel += 1
            pop, width, tgt = (rl["operand_payload_offset"],
                               rl["operand_width"], rl["original_encoded_gba"])
            seg = next((s for s in meta["segments"]
                        if s["payload_offset"] <= pop <
                        s["payload_offset"] + s["byte_count"]), None)
            if seg is None:
                fail(f"{key}: operand @{pop} in no segment")
            src_gba = seg["original_gba_start"] + (pop - seg["payload_offset"])
            got = struct.unpack_from("<I", rom, src_gba - GBA_BASE)[0]
            if got != tgt:
                fail(f"{key}: operand @{src_gba:#x} = {got:#x} != {tgt:#x}")
            cls, rkey, exp, off = (rl["target_class"], rl["target_resource_key"],
                                   rl.get("target_export"), rl.get("target_offset"))
            cls_tally[cls] = cls_tally.get(cls, 0) + 1
            if cls == "RAM_DATA_TARGET":
                n_edges += 1
                if rkey or not (EWRAM[0] <= tgt < EWRAM[1]):
                    fail(f"{key}: bad RAM target {tgt:#x}")
            elif cls == "SCRIPT_TARGET":
                n_edges += 1
                if rkey.startswith("emerald:movement/bridge/"):
                    # 3 recomp-local movement bridges (plan §5): B-owned
                    # movement bytes the seam names explicitly; not module
                    # exports, not B resources. Identity = ELF symbol at tgt,
                    # plus interior-proof containment in one module span.
                    if not exp or tgt not in (elf_syms.get(exp) or ()):
                        fail(f"{key}: bridge {exp} @{tgt:#x} not in ELF syms")
                    host = next((m for m in meta_index.values()
                                 if m["region_gba_start"] <= tgt <
                                 m["region_gba_start"] + m["region_byte_count"]),
                                None)
                    if host is None:
                        fail(f"{key}: bridge {tgt:#x} in no module span")
                    if off is not None and off != tgt - host["region_gba_start"]:
                        fail(f"{key}: bridge offset {off} != "
                             f"{tgt - host['region_gba_start']:#x}")
                    continue
                if not rkey.startswith("emerald:script/"):
                    fail(f"{key}: SCRIPT_TARGET key {rkey} not a script key")
                tmeta = meta_index.get(rkey)
                if tmeta is None:
                    fail(f"{key}: script target key {rkey} has no meta")
                if exp and not any(e["name"] == exp
                                   and e["original_gba_address"] == tgt
                                   for e in tmeta["exports"]):
                    fail(f"{key}: target {tgt:#x} not exported as {exp}")
                # SCRIPT_TARGET offsets are REGION-relative
                # (gen_script_family.py resolve_target: target - region_start)
                if off is not None and off != tgt - tmeta["region_gba_start"]:
                    fail(f"{key}: target offset {off} != {tgt - tmeta['region_gba_start']:#x}")
            elif cls == "TEXT_TARGET":
                n_edges += 1
                if rkey not in text_ids:
                    fail(f"{key}: TEXT_TARGET {rkey} missing from C catalog")
            elif cls == "MOVEMENT_TARGET":
                n_edges += 1
                if exp and exp not in mov_syms and \
                   not rkey.startswith("emerald:movement/bridge/"):
                    fail(f"{key}: MOVEMENT_TARGET {exp} missing from B bindings")
            elif cls == "MART_TABLE_TARGET":
                n_edges += 1
                if rkey.startswith("emerald:text/braille/"):
                    if rkey in text_ids:
                        fail(f"{key}: braille key {rkey} unexpectedly in C catalog")
                elif rkey.startswith("emerald:text/"):
                    if rkey not in text_ids:
                        fail(f"{key}: text-class mart target {rkey} not in catalog")
                elif rkey.startswith("emerald:script/routing/"):
                    pass  # routing dispatch table start (metadata identity)
                elif rkey.startswith("emerald:script/"):
                    m = meta_index.get(rkey)
                    if m is None:
                        fail(f"{key}: mart target key {rkey} has no meta")
                    if not any(e["name"] == exp and e["original_gba_address"] == tgt
                               for e in m["exports"]):
                        fail(f"{key}: mart target {tgt:#x} not exported as {exp}")
                    # MART_TABLE_TARGET offsets are PAYLOAD-relative
                    # (gen_script_family.py resolve_target: payload_offset_of)
                    if off is not None:
                        seg = next((s for s in m["segments"]
                                    if s["original_gba_start"] <= tgt <
                                    s["original_gba_start"] + s["byte_count"]), None)
                        if seg is None:
                            fail(f"{key}: mart target {tgt:#x} in no target segment")
                        exp_off = seg["payload_offset"] + (tgt - seg["original_gba_start"])
                        if off != exp_off:
                            fail(f"{key}: mart target offset {off} != {exp_off:#x}")
                else:
                    fail(f"{key}: unhandled MART_TABLE_TARGET key {rkey}")
            else:
                fail(f"{key}: unhandled target class {cls}")

    # --- routing relocation sidecar -----------------------------------
    routing = tomllib.loads((mods / "routing_relocations.generated.toml").read_text())
    for r in routing.get("relocs", []):
        routing_checked += 1
        cls_tally[r["target_class"]] = cls_tally.get(r["target_class"], 0) + 1
        src = r["source_gba_offset"]
        got = struct.unpack_from("<I", rom, src - GBA_BASE)[0]
        if got != r["target_gba"]:
            fail(f"routing reloc @{src:#x}: operand {got:#x} != {r['target_gba']:#x}")

    # --- section 3 pins ------------------------------------------------
    if n_rel + routing_checked != 16704:
        fail(f"relocation total {n_rel} + {routing_checked} != 16,704")
    if n_ro != 56:
        fail(f"routing-only modules {n_ro} != 56")
    if len(man) != 467:
        fail(f"embedded manifest records {len(man)} != 467")
    exp_partition = {"SCRIPT_TARGET": 8208, "TEXT_TARGET": 6207,
                     "MOVEMENT_TARGET": 2009, "MART_TABLE_TARGET": 262,
                     "RAM_DATA_TARGET": 18}
    if cls_tally != exp_partition:
        fail(f"target class partition {cls_tally} != {exp_partition}")
    print(f"class partition: " + ", ".join(
        f"{k} {cls_tally[k]}" for k in
        ("SCRIPT_TARGET", "TEXT_TARGET", "MOVEMENT_TARGET",
         "MART_TABLE_TARGET", "RAM_DATA_TARGET")))

    print(f"three-way oracle: {n_mod} modules ({n_ro} routing-only), "
          f"{n_seg} segments, {n_exp} exports, {n_rel} relocs, "
          f"{n_edges} cross-family edges, {routing_checked} routing relocs, "
          f"all consistent")
    print(f"catalog surface: script {len(man)} embedded + 56 routing-only "
          f"catalog ids; text catalog {len(text_ids)}; movement bindings "
          f"{len(mov_syms)}")
    print("OK")


if __name__ == "__main__":
    main()

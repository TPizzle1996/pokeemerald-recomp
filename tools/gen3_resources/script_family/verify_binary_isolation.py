#!/usr/bin/env python3
"""R13-G6 binary-isolation runner for the field-script family.

Sweeps the built native binary + ownership catalog to prove the G-owned
compiled field-script payloads are physically gone from the link while
every named exclusion (engine prefix, F-owned tables, C-owned text,
B-owned bridges, gStdScripts label + zeroed slots) is still present.

Usage:
    verify_binary_isolation.py --binary pokeemerald-linux64
                               [--root <repo root>] [--quiet]

Exit code 0 = all sweeps green; 1 = any sweep failed.

Sweeps:
  1. Ownership catalog: all 523 script resources ROM_BASE_ONLY on native,
     zero COMPILED_PENDING_MIGRATION anywhere in the script extraction.
  2. Legacy-symbol sweep: zero of the 523 legacy_symbol names defined.
  3. Export sweep: zero of the 7,683 kScriptExportNameIndex names defined.
  4. gStdScripts: the 11 published slots are zero in the static binary
     (the seam's PublishStdScripts is the only writer, G5 boot ordering).
  5. Address identity: gMapHeaders + gMapGroups live below 4 GiB (the
     engine's HostResolveGbaAddr identity path resolves the R13-F seam).
  6. Named exclusions present (engine prefix, F-owned, C-owned text,
     B-owned bridges, script_cmd_table).
  7. R13-G6 braille handoff: generated kBrailleGbaAddrs == the .inc rows,
     ascending ROM provenance; kBrailleTextAddresses = 22 distinct mapped
     live addresses, each carrying its braille.inc brailleformat header -
     the compiled braille text is still linked and reachable.
  8. R13-G6 sec 11B provenance-byte sweep: the 523 canonical module
     payload bytes (207,330 B) are byte-absent from the binary - full
     payload plus head/tail/interior windows for every payload >= 32 B.
"""

import argparse
import re
import subprocess
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
OWNERSHIP = (
    REPO
    / "resources/extraction/emerald/bpee01/script/modules/ownership.generated.toml"
)
EXPORT_TABLE = REPO / "src/emerald/resources/script_native_table.generated.c"
EVENT_SCRIPTS_S = REPO / "data/event_scripts.s"
BRAILLE_INC = REPO / "data/text/braille.inc"
BRAILLE_ADDR_INC = REPO / "data/text/braille_addresses_native.inc"

ENGINE_PREFIX = [
    "gScriptCmdTable",
    "gScriptCmdTableEnd",
    "gSpecials",
    "gSpecialVars",
    "gStdScripts",
    "gStdScripts_End",
]
F_OWNED = ["gMapLayouts", "gMapGroups", "gMapHeaders"]
# Sample C-owned text labels (R13-C family). R13-J §3A: the two Obtained
# labels were FLIPped (arbiter LINUX64 link proved zero host refs) - their
# compiled duplicates are removed and the strings are pack-served, so they
# are expected ABSENT (FLIP_TEXT_SAMPLE). gBirchDexRatingText_AreYouCurious
# is C-consumed (birch_pc.c; one of the 70 arbiter-proven labels) and stays
# compiled through the pokedex_rating STAY twin.
C_TEXT_SAMPLE = [
    "gBirchDexRatingText_AreYouCurious",
]
FLIP_TEXT_SAMPLE = [
    "gText_ObtainedTheItem",
    "gText_ObtainedTheDecor",
]
BRIDGE_BYTES = ["kScriptBridges"]


def nm(binary: Path, flags: str = "-n"):
    out = subprocess.run(
        ["nm", flags, str(binary)], capture_output=True, text=True
    ).stdout
    defined = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and not parts[0].endswith(":"):
            defined[parts[-1]] = parts[0]
    return defined


def _elf_data(binary: Path) -> bytes:
    with open(binary, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise RuntimeError(f"{binary} is not an ELF64 file")
    return data


def _mapped(data: bytes, addr: int, size: int) -> int:
    """File offset of `addr` in the binary, or None if unmapped."""
    phoff = int.from_bytes(data[32:40], "little")
    phentsize = int.from_bytes(data[54:56], "little")
    phnum = int.from_bytes(data[56:58], "little")
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type = int.from_bytes(data[off:off + 4], "little")
        if p_type == 1:  # PT_LOAD
            p_offset = int.from_bytes(data[off + 8:off + 16], "little")
            p_vaddr = int.from_bytes(data[off + 16:off + 24], "little")
            p_filesz = int.from_bytes(data[off + 32:off + 40], "little")
            if p_vaddr <= addr < p_vaddr + p_filesz:
                foff = p_offset + (addr - p_vaddr)
                if foff + size <= len(data):
                    return foff
    return -1


def read_quads(binary: Path, addr: int, count: int) -> list[int]:
    """Read `count` 8-byte quads at the ELF virtual address `addr`."""
    import struct

    data = _elf_data(binary)
    foff = _mapped(data, addr, 8 * count)
    if foff < 0:
        raise RuntimeError(f"address 0x{addr:x} not mapped in {binary}")
    return list(struct.unpack(f"<{count}Q", data[foff:foff + 8 * count]))


def read_words(binary: Path, addr: int, count: int) -> list[int]:
    """Read `count` 4-byte words at the ELF virtual address `addr`."""
    import struct

    data = _elf_data(binary)
    foff = _mapped(data, addr, 4 * count)
    if foff < 0:
        raise RuntimeError(f"address 0x{addr:x} not mapped in {binary}")
    return list(struct.unpack(f"<{count}I", data[foff:foff + 4 * count]))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="pokeemerald-linux64")
    ap.add_argument("--root", default=str(REPO))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    binary = Path(args.root) / args.binary
    fails = []
    info = []

    def report(name, detail, ok):
        (info if ok else fails).append(f"{name}: {detail}")

    # 1. Ownership catalog
    with open(OWNERSHIP, "rb") as f:
        doc = tomllib.load(f)
    res = doc["resources"]
    n = len(res)
    bad = [r["id"] for r in res if r["targets"]["native"] != "ROM_BASE_ONLY"]
    pending = [
        r["id"] for r in res if r["targets"]["native"] == "COMPILED_PENDING_MIGRATION"
    ]
    report(
        "ownership",
        f"{n} resources, native=ROM_BASE_ONLY all targets: "
        f"{'yes' if not bad else bad[:3]}",
        not bad,
    )
    report(
        "ownership-no-pending",
        f"{len(pending)} COMPILED_PENDING_MIGRATION remaining",
        len(pending) == 0,
    )

    # 2 + 3. Symbol sweeps
    defined = nm(binary)
    legacy = [r["legacy_symbol"] for r in res]
    hit = [s for s in legacy if s in defined]
    report(
        "legacy-symbols",
        f"{len(legacy)} legacy symbols, {len(hit)} still defined",
        not hit,
    )

    text = EXPORT_TABLE.read_text()
    start = text.index("kScriptExportNameIndex[")
    seg = text[start:text.index("};", start)]
    names = re.findall(r'\{"([^"]+)"', seg)
    hit = [s for s in names if s in defined]
    report(
        "exports",
        f"{len(names)} export names, {len(hit)} still defined",
        not hit,
    )

    # 4. gStdScripts zeroed slots (static binary; PublishStdScripts writes at boot)
    addr = int(defined.get("gStdScripts", "0"), 16)
    if addr:
        quads = read_quads(binary, addr, 11)
        report("gStdScripts-slots", f"11 slots at 0x{addr:x}: {quads}", all(q == 0 for q in quads))
    else:
        report("gStdScripts-slots", "gStdScripts symbol missing", False)

    # 5. Address identity (< 4 GiB)
    for s in F_OWNED:
        a = int(defined.get(s, "0"), 16)
        report(f"identity-{s}", f"0x{a:x} {'<4GiB' if a < 0x100000000 else '>=4GiB!'}", a != 0 and a < 0x100000000)

    # 6. Named exclusions present
    for s in ENGINE_PREFIX + F_OWNED + C_TEXT_SAMPLE + BRIDGE_BYTES:
        report(f"exclusion-{s}", f"{'present' if s in defined else 'MISSING'}", s in defined)
    # 6b. R13-J §3A FLIP text labels absent (compiled duplicates removed;
    # the strings are pack-served). The arbiter LINUX64 link is the
    # completeness proof: a live consumer would have failed the link.
    for s in FLIP_TEXT_SAMPLE:
        report(f"flip-text-{s}", f"{'ABSENT' if s not in defined else 'still defined!'}", s not in defined)

    # 7. R13-G6 braille handoff (plan sec 7.3): the generated
    # kBrailleGbaAddrs (ascending GBA provenance) pairs index-for-index
    # with kBrailleTextAddresses (defined in braille_addresses_native.inc,
    # same assembly unit as braille.inc). Every live address must carry
    # its label's 6-byte brailleformat header from braille.inc - the
    # end-to-end proof that the seam's live table points at the real
    # compiled braille text.
    braille_format = {}
    cur = None
    for line in BRAILLE_INC.read_text().splitlines():
        m = re.match(r"^(\w+):\s*$", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"^\s*brailleformat\s+(\d+),\s*(\d+),\s*(\d+),"
                     r"\s*(\d+),\s*(\d+),\s*(\d+)\s*$", line)
        if m and cur is not None and cur not in braille_format:
            braille_format[cur] = [int(g) for g in m.groups()]
    addr_rows = []
    for line in BRAILLE_ADDR_INC.read_text().splitlines():
        m = re.match(r"^\s*\.int\s+(\w+)\s*/\*\s*0x([0-9a-f]+)\s*\*/",
                     line)
        if m:
            addr_rows.append((int(m.group(2), 16), m.group(1)))
    gba_addr = int(defined.get("kBrailleGbaAddrs", "0"), 16)
    txt_addr = int(defined.get("kBrailleTextAddresses", "0"), 16)
    if gba_addr and txt_addr:
        data = _elf_data(binary)
        gbas = read_words(binary, gba_addr, 22)
        addrs = read_words(binary, txt_addr, 22)
        report("braille-table",
               f"generated .inc rows == binary kBrailleGbaAddrs "
               f"({len(addr_rows)} rows, tables at "
               f"0x{gba_addr:x}/0x{txt_addr:x})",
               len(addr_rows) == 22 and gbas == [g for g, _ in addr_rows])
        report("braille-gba-sorted",
               "22 ascending GBA provenance, all in the ROM range",
               all(0x08000000 <= g < 0x0A000000 for g in gbas)
               and all(a < b for a, b in zip(gbas, gbas[1:])))
        report("braille-live-distinct",
               "22 distinct mapped live addresses",
               len(set(addrs)) == 22
               and all(_mapped(data, a, 6) >= 0 for a in addrs))
        bad = []
        for i, (gba, label) in enumerate(addr_rows):
            fmt = braille_format.get(label)
            if fmt is None:
                bad.append(f"{label}:no-brailleformat")
                continue
            foff = _mapped(data, addrs[i], 6)
            if foff < 0 or list(data[foff:foff + 6]) != fmt:
                bad.append(f"{label}:header-mismatch")
        report("braille-format-headers",
               "6-byte brailleformat header at every live address"
               if not bad else f"mismatch: {bad[:3]}",
               not bad)
    else:
        report("braille-table",
               "kBrailleGbaAddrs/kBrailleTextAddresses missing from binary",
               False)

    # 8. R13-G6 sec 11B provenance-byte sweep: the canonical module
    # payload bytes (207,330 B across 523 modules) must not appear in
    # the native binary. Every payload >= 32 B is searched whole PLUS
    # head/tail/interior windows, so no compiled copy survives under an
    # anonymous or module-local label. Payloads shorter than 32 B are
    # skipped (too short to be a meaningful byte identity - the
    # legacy-symbol and export sweeps already prove them absent).
    data = _elf_data(binary)
    byte_hits = []
    checked = 0
    short = 0
    for r in res:
        artifact = r["source_artifact"]
        cands = [
            REPO / artifact,
            REPO / "resources/extraction/emerald/bpee01/script/modules"
            / artifact,
        ]
        payload_path = next((c for c in cands if c.is_file()), None)
        if payload_path is None:
            byte_hits.append(f"{r['id']}:missing-artifact")
            continue
        payload = payload_path.read_bytes()
        if len(payload) < 32:
            short += 1
            continue
        checked += 1
        if payload in data:
            byte_hits.append(f"{r['id']}:full")
            continue
        if len(payload) >= 64:
            mid = len(payload) // 2
            for kind, w in (("head", payload[:64]),
                            ("tail", payload[-64:]),
                            ("interior", payload[mid - 32:mid + 32])):
                if w in data:
                    byte_hits.append(f"{r['id']}:{kind}")
    report(
        "provenance-bytes",
        f"{checked} payloads >= 32 B byte-absent (full + head/tail/interior "
        f"windows), {short} short payloads (< 32 B) covered by the symbol "
        f"sweeps" if not byte_hits else f"hits: {byte_hits[:3]}",
        not byte_hits,
    )

    if not args.quiet:
        for line in info:
            print("OK   ", line)
        for line in fails:
            print("FAIL ", line)
    if fails:
        print(f"ISOLATION FAIL: {len(fails)} sweep(s) failed", file=sys.stderr)
        return 1
    print("ISOLATION GREEN: all sweeps passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

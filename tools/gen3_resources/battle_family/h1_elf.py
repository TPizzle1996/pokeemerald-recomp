"""h1_elf.py — minimal ELF32 (ARM, little-endian) reader for the R13-H1
battle-family graph analysis.

Two readers:

  ObjectFile   — the qualified per-family .o files under
                 ../pokeemerald-reference/build/emerald/data/. Section data,
                 symbol table, and relocation sections with exact addends
                 (the addend of an R_ARM_ABS32 in a relocatable object is the
                 stored 32-bit value at the relocation source).

  LinkedElf    — the qualified full ELF (pokeemerald.elf): final GBA
                 addresses of every defined symbol, used to compute per-object
                 script_data bases and for the three-way static oracle
                 (ROM raw == ELF relocation result == generated record).

Analysis-only module; nothing here is linked into production code.
"""

import struct
import sys


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


class ObjectFile:
    """ELF32 relocatable object reader (the qualified .o files)."""

    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        if len(data) < 52 or data[0:4] != b"\x7fELF":
            fail(f"{path}: not an ELF file")
        if data[4] != 1 or data[5] != 1:
            fail(f"{path}: not ELFCLASS32 little-endian")
        self.path = path
        self.data = data
        shoff = struct.unpack_from("<I", data, 0x20)[0]
        shentsize = struct.unpack_from("<H", data, 0x2E)[0]
        shnum = struct.unpack_from("<H", data, 0x30)[0]
        shstrndx = struct.unpack_from("<H", data, 0x32)[0]
        self.sections = []
        for i in range(shnum):
            off = shoff + i * shentsize
            vals = struct.unpack_from("<IIIIIIIIII", data, off)
            self.sections.append(dict(
                sh_name=vals[0], sh_type=vals[1], sh_flags=vals[2],
                sh_addr=vals[3], sh_offset=vals[4], sh_size=vals[5],
                sh_link=vals[6], sh_info=vals[7], sh_addralign=vals[8]))
        shstr = self.sections[shstrndx]
        self.shstr = data[shstr["sh_offset"]:shstr["sh_offset"] + shstr["sh_size"]]
        for i, s in enumerate(self.sections):
            end = self.shstr.find(b"\0", s["sh_name"])
            s["name"] = self.shstr[s["sh_name"]:end].decode("ascii", "replace")
        # symbol table
        self.symtab_idx = None
        for i, s in enumerate(self.sections):
            if s["sh_type"] == 2:  # SHT_SYMTAB
                self.symtab_idx = i
                break
        if self.symtab_idx is None:
            fail(f"{path}: no SHT_SYMTAB")
        stab = self.sections[self.symtab_idx]
        strtab = self.sections[stab["sh_link"]]
        self.strtab = data[strtab["sh_offset"]:strtab["sh_offset"] + strtab["sh_size"]]
        self.symbols = []
        for off in range(stab["sh_offset"], stab["sh_offset"] + stab["sh_size"], 16):
            name_off, value, size, info, other, shndx = \
                struct.unpack_from("<IIIBBH", data, off)
            end = self.strtab.find(b"\0", name_off)
            name = self.strtab[name_off:end].decode("ascii", "replace")
            binding = info >> 4
            typ = info & 0xF
            self.symbols.append(dict(name=name, value=value, size=size,
                                     binding=binding, typ=typ, shndx=shndx))
        self.sym_by_name = {}
        for s in self.symbols:
            self.sym_by_name.setdefault(s["name"], []).append(s)
        # relocation sections
        self.relocs = {}  # section_name -> list of dicts
        for sec in self.sections:
            if sec["sh_type"] == 9:  # SHT_REL
                entries = []
                for off in range(sec["sh_offset"], sec["sh_offset"] + sec["sh_size"], 8):
                    source, info = struct.unpack_from("<II", data, off)
                    symidx = info >> 8
                    entries.append(dict(
                        offset=source,
                        type=info & 0xFF,
                        sym=self.symbols[symidx]["name"],
                        symidx=symidx,
                    ))
                self.relocs[sec["name"]] = entries

    def section(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s
        return None

    def section_bytes(self, name):
        s = self.section(name)
        if s is None:
            return None
        return self.data[s["sh_offset"]:s["sh_offset"] + s["sh_size"]]

    def relocs_for(self, target_section):
        """Relocations applying to `target_section` (e.g. 'script_data')."""
        for sec in self.sections:
            if sec["sh_type"] == 9 and sec["sh_info"] == self.section_index(target_section):
                return self.relocs[sec["name"]]
        return []

    def section_index(self, name):
        for i, s in enumerate(self.sections):
            if s["name"] == name:
                return i
        return None

    def symbols_in(self, shndx):
        return [s for s in self.symbols if s["shndx"] == shndx]


class LinkedElf:
    """Qualified linked ELF: final GBA addresses of defined symbols."""

    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        if data[0:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            fail(f"{path}: not ELFCLASS32 little-endian")
        self.data = data
        self.path = path
        shoff = struct.unpack_from("<I", data, 0x20)[0]
        shentsize = struct.unpack_from("<H", data, 0x2E)[0]
        shnum = struct.unpack_from("<H", data, 0x30)[0]
        shstrndx = struct.unpack_from("<H", data, 0x32)[0]
        self.sections = []
        for i in range(shnum):
            off = shoff + i * shentsize
            vals = struct.unpack_from("<IIIIIIIIII", data, off)
            self.sections.append(dict(
                sh_name=vals[0], sh_type=vals[1], sh_flags=vals[2],
                sh_addr=vals[3], sh_offset=vals[4], sh_size=vals[5],
                sh_link=vals[6], sh_info=vals[7], sh_addralign=vals[8]))
        shstr = self.sections[shstrndx]
        self.shstr = data[shstr["sh_offset"]:shstr["sh_offset"] + shstr["sh_size"]]
        for i, s in enumerate(self.sections):
            end = self.shstr.find(b"\0", s["sh_name"])
            s["name"] = self.shstr[s["sh_name"]:end].decode("ascii", "replace")
        self.symtab_idx = None
        for i, s in enumerate(self.sections):
            if s["sh_type"] == 2:
                self.symtab_idx = i
                break
        stab = self.sections[self.symtab_idx]
        strtab = self.sections[stab["sh_link"]]
        self.strtab = data[strtab["sh_offset"]:strtab["sh_offset"] + strtab["sh_size"]]
        self.symbols = []
        for off in range(stab["sh_offset"], stab["sh_offset"] + stab["sh_size"], 16):
            name_off, value, size, info, other, shndx = \
                struct.unpack_from("<IIIBBH", data, off)
            end = self.strtab.find(b"\0", name_off)
            name = self.strtab[name_off:end].decode("ascii", "replace")
            self.symbols.append(dict(name=name, value=value, size=size,
                                     binding=info >> 4, typ=info & 0xF, shndx=shndx))
        self.by_name = {}
        for s in self.symbols:
            self.by_name.setdefault(s["name"], []).append(s)
        self.by_addr = {}
        for s in self.symbols:
            if s["shndx"] != 0 and s["binding"] == 1:  # defined GLOBAL
                self.by_addr.setdefault(s["value"], []).append(s)

    def global_addr(self, name):
        for s in self.symbols:
            if s["name"] == name and s["shndx"] != 0:
                return s["value"]
        return None

    def sym_at(self, addr):
        return self.by_addr.get(addr, [])

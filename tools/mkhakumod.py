#!/usr/bin/env python3
"""mkhakumod.py - pack relocatable objects into a Haku module.

Usage: mkhakumod.py INPUT... OUTPUT.hkm --name NAME

The kernel does not parse object files, so the host does it once: allocatable
sections are laid out back to back, uninitialized sections become the BSS
size, and the relocations are reduced to records the kernel loader can apply
against its export table. The resulting layout is documented in
kernel/module/haku_module.h.

Two host formats are accepted:

  * ELF32 (`nasm -f elf32`): exactly one input, R_386_32 / R_386_PC32.
  * COFF/i386 (`cl.exe` or `clang-cl`) and COFF/amd64 (the x64 tools of the
    same compilers): one or more inputs, linked here. On i386
    IMAGE_REL_I386_DIR32 becomes R_386_32 and IMAGE_REL_I386_REL32 becomes
    R_386_PC32 (subtracting the 4-byte field the PC-relative form already
    accounted for); on amd64 IMAGE_REL_AMD64_ADDR32 and DIR64 become the
    32-bit absolute form and IMAGE_REL_AMD64_REL32 the PC-relative one.
    Merging several objects is what lets a C module link the module library:
    internal symbols resolve to their section offset, undefined symbols
    become kernel imports, duplicate COMDAT definitions are folded, and code
    the entry point cannot reach is dropped, so a module only ships the
    helpers it actually calls.

The amd64 container is version 2; version 1 is the i386 one. Kernel, heap and
export addresses all sit below 4 GiB, so an amd64 64-bit pointer is packed as
its low 32 bits with an absolute relocation and a zeroed upper half. The
metadata sections a 64-bit compiler emits for unwinding (`.pdata`, `.xdata`)
are dropped: the loader never unwinds a module frame.

Nothing larger than the kernel's limits is accepted (64 KiB image and BSS,
256 relocations, 4096 bytes of import names), and the only relocation types
understood are the four-byte absolute and PC-relative forms above.
"""

import argparse
import struct
import sys

MAGIC = b"HAKUMOD\0"
VERSION_386 = 1
VERSION_X64 = 2
HEADER_SIZE = 72
NAME_MAX = 24
SYMBOL_MAX = 48
MAX_IMAGE = 64 * 1024
MAX_BSS = 64 * 1024
MAX_RELOCS = 256
MAX_STRINGS = 4096

SYM_BASE = 0xFFFFFFFE
NO_OFFSET = 0xFFFFFFFF

R_386_32 = 1
R_386_PC32 = 2
R_64_ABS = 3
R_64_PC32 = 4

# ELF32 relocatable objects.
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHF_ALLOC = 0x2
SHN_UNDEF = 0
SHN_ABS = 0xFFF1

# COFF objects. The packer links both machines; the machine also picks the
# container version and the relocation vocabulary.
COFF_MACHINE_I386 = 0x14C
COFF_MACHINE_AMD64 = 0x8664
COFF_SYMBOL_SIZE = 18
COFF_SECTION_SIZE = 40
COFF_SECTION_ALIGN_SHIFT = 20
COFF_SECTION_ALIGN_MASK = 0xF
COFF_ALIGNMENTS = (1, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048,
                   4096, 8192)
SCN_CNT_CODE = 0x00000020
SCN_CNT_INITIALIZED_DATA = 0x00000040
SCN_CNT_UNINITIALIZED_DATA = 0x00000080
SCN_LNK_INFO = 0x00000200
SCN_LNK_REMOVE = 0x00000800
SCN_LNK_COMDAT = 0x00001000
SCN_MEM_DISCARDABLE = 0x02000000
SYM_CLASS_EXTERNAL = 2
SYM_CLASS_STATIC = 3
SYM_UNDEFINED = 0
SYM_ABSOLUTE = -1
REL_I386_DIR32 = 0x6
REL_I386_REL32 = 0x14
REL_AMD64_ADDR64 = 0x1
REL_AMD64_ADDR32 = 0x2
REL_AMD64_ADDR32NB = 0x3
REL_AMD64_REL32 = 0x4
REL_AMD64_DIR64 = 0xA
COMDAT_SELECT_ANY = 2
# Offset of the "Selection" byte inside a section-definition aux record.
COMDAT_SELECTION_OFFSET = 14
# Bookkeeping a 64-bit compiler emits for exceptions and debug info. Haku
# never unwinds a module frame, so these are not part of the image.
AMD64_DROP_SECTIONS = (".pdata", ".xdata")

ENTRY_NAMES = ("haku_module_init", "_haku_module_init")
EXIT_NAMES = ("haku_module_exit", "_haku_module_exit")


def fail(message):
    print(f"mkhakumod: error: {message}", file=sys.stderr)
    raise SystemExit(1)


class Module:
    """Sections laid out back to back, plus the container built from them."""

    def __init__(self):
        self.sections = []
        self.offsets = []
        self.image = bytearray()
        self.bss = 0
        self.relocs = []
        self.strings = bytearray()
        self.imports = {}
        self.placed = False
        # Container format: 1 = i386, 2 = x86-64.
        self.version = VERSION_386

    def add_section(self, name, size, align, data):
        """Append one section; `data` is None for uninitialized storage."""
        self.sections.append({"name": name, "size": size,
                              "align": max(1, align), "data": data})
        return len(self.sections) - 1

    def place(self):
        """Assign every section an offset: initialized first, then BSS."""
        self.placed = True
        self.offsets = [0] * len(self.sections)
        for index, section in enumerate(self.sections):
            if section["data"] is None:
                continue
            if len(section["data"]) != section["size"]:
                fail(f"internal error: section {section['name']} size")
            self.image.extend(b"\0" * (-len(self.image) % section["align"]))
            self.offsets[index] = len(self.image)
            self.image.extend(section["data"])
        if len(self.image) > MAX_IMAGE:
            fail(f"image is {len(self.image)} bytes, limit is {MAX_IMAGE}")
        bss = 0
        for index, section in enumerate(self.sections):
            if section["data"] is not None:
                continue
            bss += -bss % section["align"]
            self.offsets[index] = len(self.image) + bss
            bss += section["size"]
        if bss > MAX_BSS:
            fail(f"BSS is {bss} bytes, limit is {MAX_BSS}")
        self.bss = bss

    def import_offset(self, text):
        known = self.imports.get(text)
        if known is not None:
            return known
        if len(text) > SYMBOL_MAX:
            fail(f"import name {text!r} exceeds {SYMBOL_MAX} bytes")
        offset = len(self.strings)
        self.strings.extend(text.encode("ascii", errors="replace") + b"\0")
        if len(self.strings) > MAX_STRINGS:
            fail(f"import names exceed {MAX_STRINGS} bytes")
        self.imports[text] = offset
        return offset

    def relocate(self, section, offset, target, kind, pc_bias=0, width=4):
        """Record one relocation.

        `target` is ("base", section, value) for code or data inside the
        module, or ("import", name) for a kernel export. The addend starts as
        the word already in the image, which is what both object formats
        store there. `pc_bias` is the PC-relative convention's difference:
        ELF already stores S + A - P with A field-relative, while COFF stores
        the target offset for a field that the linker subtracts P + 4 from,
        so a COFF REL32 addend loses those 4 bytes.

        `width` is 8 for an amd64 pointer. The value is still below 4 GiB, so
        the loader writes its low half and the upper half stays zero; the
        addend is checked to fit before anything is written.
        """
        location = self.offsets[section] + offset
        total = len(self.image) + self.bss
        if location > total or total - location < width:
            fail(f"relocation at {location:#x} is outside the module image")
        if width == 8:
            if location < len(self.image):
                addend, = struct.unpack_from("<q", self.image, location)
                struct.pack_into("<Q", self.image, location, 0)
            else:
                addend = 0
            if not -(1 << 31) <= addend < (1 << 32):
                fail(f"a 64-bit addend at {location:#x} does not fit in "
                     "the 32-bit container")
        else:
            if location < len(self.image):
                addend, = struct.unpack_from("<i", self.image, location)
                struct.pack_into("<I", self.image, location, 0)
            else:
                addend = 0
        if target[0] == "base":
            symbol = SYM_BASE
            addend += self.offsets[target[1]] + target[2]
        else:
            symbol = self.import_offset(target[1])
        if kind in (R_386_PC32, R_64_PC32):
            addend -= pc_bias
        if len(self.relocs) >= MAX_RELOCS:
            fail(f"more than {MAX_RELOCS} relocations")
        self.relocs.append((location, symbol, kind, addend))

    def write(self, path, name, entry, exit_offset):
        if not self.placed:
            fail("internal error: sections were never placed")
        reloc_offset = HEADER_SIZE + len(self.image)
        string_offset = reloc_offset + 16 * len(self.relocs)
        header = MAGIC + struct.pack(
            "<IIIIIIIIII", self.version, len(self.image), self.bss, entry,
            exit_offset, len(self.relocs), reloc_offset, len(self.strings),
            string_offset, len(name) + 1)
        header += name.ljust(NAME_MAX, b"\0")
        if len(header) != HEADER_SIZE:
            fail("internal error: header size")
        payload = bytearray(header)
        payload += self.image
        for location, symbol, kind, addend in self.relocs:
            payload += struct.pack("<IIIi", location, symbol, kind, addend)
        payload += self.strings
        try:
            with open(path, "wb") as handle:
                handle.write(payload)
        except OSError as error:
            fail(f"cannot write {path}: {error}")
        print(f"mkhakumod: {path} ready ({len(payload)} bytes, "
              f"name={name.decode('ascii', errors='replace')}, "
              f"image={len(self.image)}, bss={self.bss}, "
              f"relocs={len(self.relocs)}, imports={len(self.imports)})")


def c_string(blob, offset, what):
    if offset >= len(blob):
        fail(f"{what} name offset {offset} is outside its string table")
    end = blob.find(b"\0", offset)
    if end < 0:
        fail(f"{what} name at {offset} is not NUL-terminated")
    return blob[offset:end].decode("ascii", errors="replace")


# ---------------------------------------------------------------- ELF32 ----


def read_elf(path, data):
    if len(data) < 52:
        fail(f"{path}: not an ELF file (truncated header)")
    if data[:4] != b"\x7fELF":
        fail(f"{path}: not an ELF file")
    if data[4] == 2:
        fail(f"{path}: ELF64 modules are not supported; build x64 modules "
             "from COFF/amd64 objects (cl.exe or clang-cl)")
    if data[4] != 1 or data[5] != 1:
        fail(f"{path}: expected a 32-bit little-endian object")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_type != 1:
        fail(f"{path}: expected an ET_REL object (nasm -f elf32)")
    if e_machine != 3:
        fail(f"{path}: expected EM_386")
    e_shoff, = struct.unpack_from("<I", data, 32)
    e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = \
        struct.unpack_from("<HHHHHH", data, 40)
    del e_ehsize, e_phentsize, e_phnum

    if e_shoff == 0 or e_shnum == 0:
        fail(f"{path}: the object has no section table")
    if e_shentsize < 40:
        fail(f"{path}: unsupported section header size")
    if e_shoff > len(data) or e_shnum > (len(data) - e_shoff) // e_shentsize:
        fail(f"{path}: section table is outside the file")

    raw = []
    for index in range(e_shnum):
        offset = e_shoff + index * e_shentsize
        (name, kind, flags, addr, file_offset, size, link, info, align,
         entsize) = struct.unpack_from("<IIIIIIIIII", data, offset)
        del addr, entsize
        raw.append({"name": name, "type": kind, "flags": flags,
                    "offset": file_offset, "size": size, "link": link,
                    "info": info, "align": align})
    if e_shstrndx >= len(raw):
        fail(f"{path}: section-name string table index is invalid")
    names = raw[e_shstrndx]
    if names["offset"] > len(data) or names["size"] > len(data) - names["offset"]:
        fail(f"{path}: section-name string table is outside the file")
    names_blob = data[names["offset"]:names["offset"] + names["size"]]
    for section in raw:
        section["name"] = c_string(names_blob, section["name"], "section")
    for section in raw:
        if section["type"] in (SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB,
                               SHT_REL, SHT_RELA):
            if section["offset"] > len(data) or \
                    section["size"] > len(data) - section["offset"]:
                fail(f"{path}: section {section['name']} is outside the file")
        if section["type"] == SHT_RELA:
            fail(f"{path}: RELA relocations are not supported; "
                 "nasm emits REL")
    return raw


def elf_symbols(data, sections, symtab):
    """Return [(name, value, shndx)] for one ELF symbol table."""
    strtab = sections[symtab["link"]] if symtab["link"] < len(sections) else None
    if not strtab or strtab["type"] != SHT_STRTAB:
        fail(f"symbol table {symtab['name']} has no string table")
    blob = data[strtab["offset"]:strtab["offset"] + strtab["size"]]
    symbols = []
    for index in range(symtab["size"] // 16):
        offset = symtab["offset"] + index * 16
        st_name, st_value, st_size, st_info, st_other, st_shndx = \
            struct.unpack_from("<IIIBBH", data, offset)
        del st_size, st_info, st_other
        name = c_string(blob, st_name, "symbol") if st_name else ""
        symbols.append((name, st_value, st_shndx))
    return symbols


def pack_elf(path, data, module):
    sections = read_elf(path, data)
    mapping = {}
    for index, section in enumerate(sections):
        if not (section["flags"] & SHF_ALLOC):
            continue
        if section["type"] == SHT_PROGBITS:
            content = data[section["offset"]:section["offset"] + section["size"]]
            mapping[index] = module.add_section(
                section["name"], section["size"], section["align"], content)
        elif section["type"] == SHT_NOBITS:
            mapping[index] = module.add_section(
                section["name"], section["size"], section["align"], None)
    module.place()

    symbols = {}
    for symtab in sections:
        if symtab["type"] != SHT_SYMTAB:
            continue
        for sym_name, value, shndx in elf_symbols(data, sections, symtab):
            if sym_name:
                symbols[sym_name] = (shndx, value)

    for reloc_section in sections:
        if reloc_section["type"] != SHT_REL:
            continue
        target = reloc_section["info"]
        if target not in mapping:
            continue  # a relocation against a discarded section
        symtab = sections[reloc_section["link"]] \
            if reloc_section["link"] < len(sections) else None
        if not symtab or symtab["type"] != SHT_SYMTAB:
            fail(f"relocation section {reloc_section['name']} "
                 "has no symbol table")
        table = elf_symbols(data, sections, symtab)
        for index in range(reloc_section["size"] // 8):
            offset = reloc_section["offset"] + index * 8
            r_offset, r_info = struct.unpack_from("<II", data, offset)
            kind = r_info & 0xFF
            sym_index = r_info >> 8
            if kind not in (R_386_32, R_386_PC32):
                fail(f"{path}: unsupported relocation type {kind} in "
                     f"{reloc_section['name']}")
            if sym_index >= len(table):
                fail(f"{path}: relocation names a symbol past the symbol table")
            sym_name, sym_value, sym_shndx = table[sym_index]
            if sym_shndx == SHN_UNDEF:
                if not sym_name:
                    fail(f"{path}: relocation names an empty undefined symbol")
                resolved = ("import", sym_name)
            elif sym_shndx in mapping:
                resolved = ("base", mapping[sym_shndx], sym_value)
            elif sym_shndx == SHN_ABS:
                fail(f"{path}: absolute symbol {sym_name} is not supported")
            else:
                fail(f"{path}: symbol {sym_name} is not in an allocatable "
                     "section")
            module.relocate(mapping[target], r_offset, resolved, kind)

    entry = NO_OFFSET
    exit_offset = NO_OFFSET
    for sym_name, (shndx, value) in symbols.items():
        if shndx not in mapping:
            continue
        if sym_name == "haku_module_init":
            entry = module.offsets[mapping[shndx]] + value
        elif sym_name == "haku_module_exit":
            exit_offset = module.offsets[mapping[shndx]] + value
    return entry, exit_offset


# ----------------------------------------------------------------- COFF ----


class CoffObject:
    """One parsed COFF/i386 object: sections, symbols, relocations."""

    def __init__(self, path, data):
        self.path = path
        self.sections = []
        self.symbols = []
        self.by_index = {}

        if len(data) < 20:
            fail(f"{path}: not a COFF object (truncated header)")
        machine, count, _, symptr, nsyms, optsize, _ = \
            struct.unpack_from("<HHIIIHH", data, 0)
        if machine not in (COFF_MACHINE_I386, COFF_MACHINE_AMD64):
            fail(f"{path}: COFF machine {machine:#x} is not i386 or amd64")
        self.machine = machine
        self.is_x64 = machine == COFF_MACHINE_AMD64
        if optsize:
            fail(f"{path}: unexpected optional header in an object file")
        if symptr + nsyms * COFF_SYMBOL_SIZE > len(data):
            fail(f"{path}: symbol table is outside the file")
        string_base = symptr + nsyms * COFF_SYMBOL_SIZE
        if string_base + 4 > len(data):
            fail(f"{path}: string table is outside the file")
        string_size, = struct.unpack_from("<I", data, string_base)
        if string_size < 4 or string_base + string_size > len(data):
            fail(f"{path}: string table is outside the file")
        strings = data[string_base:string_base + string_size]

        def name_of(raw):
            if raw[:4] == b"\0\0\0\0":
                offset, = struct.unpack_from("<I", raw, 4)
                return c_string(strings, offset, "symbol")
            return raw.rstrip(b"\0").decode("ascii", errors="replace")

        offset = 20
        for index in range(count):
            if offset + COFF_SECTION_SIZE > len(data):
                fail(f"{path}: section table is outside the file")
            raw = data[offset:offset + COFF_SECTION_SIZE]
            offset += COFF_SECTION_SIZE
            name = name_of(raw[:8])
            (_, _, raw_size, raw_ptr, rel_ptr, _, rel_count, _,
             chars) = struct.unpack_from("<IIIIIIHHI", raw, 8)
            section = {"name": name, "raw_ptr": raw_ptr,
                       "rel_ptr": rel_ptr, "rel_count": rel_count,
                       "relocations": [], "comdat": False,
                       "selection": 0, "kept": False, "drop": False}
            if chars & SCN_LNK_COMDAT:
                section["comdat"] = True
            kept = bool(chars & (SCN_CNT_CODE | SCN_CNT_INITIALIZED_DATA |
                                 SCN_CNT_UNINITIALIZED_DATA))
            if chars & (SCN_LNK_INFO | SCN_LNK_REMOVE | SCN_MEM_DISCARDABLE):
                kept = False
            # Unwind and debug metadata: the kernel never unwinds a module.
            if self.is_x64 and name in AMD64_DROP_SECTIONS:
                kept = False
            if kept:
                if chars & SCN_CNT_UNINITIALIZED_DATA and raw_ptr == 0:
                    section["data"] = None
                    section["size"] = raw_size
                else:
                    if raw_ptr + raw_size > len(data):
                        fail(f"{path}: section {name} is outside the file")
                    section["data"] = data[raw_ptr:raw_ptr + raw_size]
                    section["size"] = raw_size
                section["align"] = COFF_ALIGNMENTS[
                    (chars >> COFF_SECTION_ALIGN_SHIFT) &
                    COFF_SECTION_ALIGN_MASK]
            section["kept"] = kept
            self.sections.append(section)

        index = 0
        while index < nsyms:
            raw = data[symptr + index * COFF_SYMBOL_SIZE:
                       symptr + (index + 1) * COFF_SYMBOL_SIZE]
            value, number, _, storage, naux = struct.unpack_from("<IhHBB", raw, 8)
            selection = 0
            if naux and 0 < number <= len(self.sections):
                # A COMDAT section's definition symbol carries a
                # section-definition aux record; byte 14 is its Selection.
                aux = symptr + (index + 1) * COFF_SYMBOL_SIZE
                if aux + 16 <= len(data):
                    selection = data[aux + COMDAT_SELECTION_OFFSET]
            record = {"name": name_of(raw[:8]), "value": value,
                      "section": number, "storage": storage, "aux": naux,
                      "selection": selection}
            self.symbols.append(record)
            self.by_index[index] = record
            index += 1 + naux

        for section_index, section in enumerate(self.sections):
            if not section["kept"] or not section["rel_count"]:
                continue
            base = section["rel_ptr"]
            if base + section["rel_count"] * 10 > len(data):
                fail(f"{path}: relocations for {section['name']} are "
                     "outside the file")
            for index in range(section["rel_count"]):
                at = base + index * 10
                target, symbol, kind = struct.unpack_from("<IIH", data, at)
                section["relocations"].append((target, symbol, kind))

    def define_sections(self):
        """Attach each COMDAT section's selection byte to the section."""
        for record in self.symbols:
            number = record["section"]
            if not (0 < number <= len(self.sections) and not record["value"]
                    and record["aux"]
                    and record["storage"] == SYM_CLASS_STATIC
                    and record["name"] == self.sections[number - 1]["name"]):
                continue
            self.sections[number - 1]["selection"] = record["selection"]


def link_coff(objects, module):
    """Merge COFF objects: fold COMDAT duplicates, drop unreachable code."""
    if not objects:
        fail("no COFF objects")
    is_x64 = objects[0].is_x64
    if any(obj.is_x64 != is_x64 for obj in objects):
        fail("cannot mix i386 and amd64 objects in one module")
    module.version = VERSION_X64 if is_x64 else VERSION_386
    located = {}        # (object, section) -> section index in `module`
    external = {}       # external name -> (object, section, value, selection)

    for obj in objects:
        obj.define_sections()

    # Pass 1: collect the global symbols. A duplicate COMDAT "select any"
    # definition folds into the first one, which is how the compiler emits
    # inline functions; anything else defined twice is a real link error.
    for object_index, obj in enumerate(objects):
        for record in obj.symbols:
            section_index = record["section"] - 1
            if record["storage"] != SYM_CLASS_EXTERNAL or \
                    not 0 <= section_index < len(obj.sections):
                continue
            section = obj.sections[section_index]
            if not section["kept"]:
                fail(f"{obj.path}: external {record['name']} is defined in "
                     f"non-allocatable section {section['name']}")
            first = external.get(record["name"])
            if first is None:
                external[record["name"]] = (object_index, section_index,
                                            record["value"])
                continue
            other = objects[first[0]].sections[first[1]]
            if (record["value"] == 0 and first[2] == 0 and
                    section["selection"] == COMDAT_SELECT_ANY and
                    other["selection"] == COMDAT_SELECT_ANY):
                section["drop"] = True
                continue
            fail(f"{obj.path}: symbol {record['name']} is already defined in "
                 f"{objects[first[0]].path}")

    entry_key = next((name for name in ENTRY_NAMES if name in external), None)
    if entry_key is None:
        fail("the objects do not define haku_module_init")
    exit_key = next((name for name in EXIT_NAMES if name in external), None)
    entry_node = external[entry_key][:2]
    exit_node = external[exit_key][:2] if exit_key else None

    def base_target(object_index, section_index, value, name):
        key = (object_index, section_index)
        if key not in located:
            fail(f"symbol {name} lives in a section that the entry point "
                 "does not reach")
        return ("base", located[key], value)

    def target_of(object_index, record):
        """Map a relocation's target symbol to ("base"|"import", ...)."""
        obj = objects[object_index]
        number = record["section"]
        if number == SYM_UNDEFINED:
            if record["storage"] != SYM_CLASS_EXTERNAL or not record["name"]:
                fail(f"{obj.path}: relocation names an undefined symbol "
                     "without a name")
            known = external.get(record["name"])
            if known is not None:
                return base_target(known[0], known[1], known[2],
                                   record["name"])
            name = record["name"]
            if not is_x64:
                if name.startswith("_"):
                    name = name[1:]
            if not name or "@" in name:
                fail(f"{obj.path}: symbol {record['name']} is decorated for "
                     "stdcall/fastcall; modules use the cdecl convention")
            return ("import", name)
        if number == SYM_ABSOLUTE:
            fail(f"{obj.path}: absolute symbol {record['name']} cannot be "
                 "relocated")
        if not 0 < number <= len(obj.sections):
            fail(f"{obj.path}: symbol {record['name']} names section "
                 f"{number}, which does not exist")
        value = record["value"]
        if record["storage"] == SYM_CLASS_EXTERNAL:
            known = external.get(record["name"])
            if known is None:
                fail(f"{obj.path}: external {record['name']} is undefined")
            if objects[known[0]].sections[known[1]]["drop"]:
                fail(f"{obj.path}: {record['name']} was folded away")
            object_index, number, base = known[0], known[1] + 1, known[2]
            value += base
        return base_target(object_index, number - 1, value, record["name"])

    # Pass 2: reachability from the entry and exit hooks. Only reachable
    # sections are laid out, so a module that calls memset ships memset and
    # a module that never does not.
    live = set()
    pending = [entry_node] if entry_node else []
    if exit_node is not None:
        pending.append(exit_node)
    while pending:
        node = pending.pop()
        if node in live:
            continue
        live.add(node)
        obj = objects[node[0]]
        for _, symbol_index, _ in obj.sections[node[1]]["relocations"]:
            record = obj.by_index.get(symbol_index)
            if record is None:
                fail(f"{obj.path}: relocation names a symbol table index "
                     "that is not a symbol")
            number = record["section"]
            if number == SYM_UNDEFINED:
                known = external.get(record["name"])
                if known is not None:
                    pending.append(known[:2])
                continue
            if not 0 < number <= len(obj.sections):
                fail(f"{obj.path}: symbol {record['name']} names section "
                     f"{number}, which does not exist")
            if record["storage"] == SYM_CLASS_EXTERNAL:
                known = external.get(record["name"])
                if known is not None:
                    pending.append(known[:2])
                continue
            pending.append((node[0], number - 1))

    # Pass 3: layout in object order, then apply the relocations.
    for object_index, obj in enumerate(objects):
        for section_index, section in enumerate(obj.sections):
            key = (object_index, section_index)
            if key not in live or section["drop"]:
                continue
            located[key] = module.add_section(
                section["name"], section["size"], section["align"],
                section["data"])
    module.place()

    for object_index, obj in enumerate(objects):
        for section_index, section in enumerate(obj.sections):
            key = (object_index, section_index)
            if key not in located:
                continue
            for target, symbol_index, kind in section["relocations"]:
                width = 4
                if is_x64:
                    if kind == REL_AMD64_REL32:
                        reloc_kind, pc_bias = R_64_PC32, 4
                    elif kind in (REL_AMD64_ADDR64, REL_AMD64_DIR64):
                        reloc_kind, pc_bias, width = R_64_ABS, 0, 8
                    elif kind == REL_AMD64_ADDR32:
                        reloc_kind, pc_bias = R_64_ABS, 0
                    else:
                        fail(f"{obj.path}: amd64 relocation type {kind:#x} "
                             f"in {section['name']} is not supported")
                elif kind == REL_I386_DIR32:
                    reloc_kind, pc_bias = R_386_32, 0
                elif kind == REL_I386_REL32:
                    reloc_kind, pc_bias = R_386_PC32, 4
                else:
                    fail(f"{obj.path}: relocation type {kind:#x} in "
                         f"{section['name']} is not supported")
                record = obj.by_index.get(symbol_index)
                if record is None:
                    fail(f"{obj.path}: relocation names a symbol table "
                         "index that is not a symbol")
                module.relocate(located[key], target,
                                target_of(object_index, record), reloc_kind,
                                pc_bias=pc_bias, width=width)

    entry = module.offsets[located[entry_node]] + external[entry_key][2]
    exit_offset = NO_OFFSET
    if exit_node is not None:
        exit_offset = module.offsets[located[exit_node]] + \
            external[exit_key][2]
    return entry, exit_offset


def main(argv):
    parser = argparse.ArgumentParser(
        description="Pack relocatable objects into a Haku module.")
    parser.add_argument("inputs", nargs="+", help="ELF32 or COFF/i386 objects")
    parser.add_argument("output", help="output .hkm file")
    parser.add_argument("--name", required=True,
                        help="module name used by delete_module (max 23 chars)")
    args = parser.parse_args(argv)

    name = args.name.encode("ascii", errors="strict")
    if not name or len(name) > NAME_MAX - 1:
        fail(f"module name must be 1..{NAME_MAX - 1} ASCII bytes")
    if b"\0" in name:
        fail("module name must not contain NUL")

    blobs = []
    for path in args.inputs:
        try:
            with open(path, "rb") as handle:
                blobs.append((path, handle.read()))
        except OSError as error:
            fail(f"cannot read {path}: {error}")
    is_elf = [data[:4] == b"\x7fELF" for _, data in blobs]
    if any(is_elf) and not all(is_elf):
        fail("cannot mix ELF and COFF inputs")
    module = Module()
    if all(is_elf):
        if len(blobs) != 1:
            fail("an ELF module takes exactly one object")
        entry, exit_offset = pack_elf(blobs[0][0], blobs[0][1], module)
    else:
        objects = [CoffObject(path, data) for path, data in blobs]
        entry, exit_offset = link_coff(objects, module)

    if entry == NO_OFFSET:
        fail("the objects do not define haku_module_init")
    if entry >= len(module.image):
        fail("haku_module_init is outside the copied image")
    if exit_offset != NO_OFFSET and exit_offset >= len(module.image):
        fail("haku_module_exit is outside the copied image")
    module.write(args.output, name, entry, exit_offset)


if __name__ == "__main__":
    main(sys.argv[1:])

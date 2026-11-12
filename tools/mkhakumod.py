#!/usr/bin/env python3
"""mkhakumod.py - pack an ELF32 relocatable object into a Haku module.

Usage: mkhakumod.py input.o output.hkm --name NAME

The kernel does not parse ELF, so the host does it once: allocatable
sections are laid out back to back, NOBITS sections become the BSS size,
and the R_386_32 / R_386_PC32 relocations NASM emits are reduced to records
the kernel loader can apply against its export table. The resulting layout
is documented in kernel/module/haku_module.h.

Only the subset of ELF a NASM `-f elf32` object uses is accepted: no RELA
records, no COMMON symbols, no absolute relocations, and nothing larger
than the kernel's fixed limits.
"""

import argparse
import struct
import sys

MAGIC = b"HAKUMOD\0"
VERSION = 1
HEADER_SIZE = 72
IMAGE_OFFSET = 72
NAME_MAX = 24
MAX_IMAGE = 64 * 1024
MAX_BSS = 64 * 1024
MAX_RELOCS = 256
MAX_STRINGS = 4096

SYM_BASE = 0xFFFFFFFE
NO_OFFSET = 0xFFFFFFFF

R_386_32 = 1
R_386_PC32 = 2

SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9

SHF_ALLOC = 0x2
SHN_UNDEF = 0
SHN_ABS = 0xFFF1


def fail(message):
    print(f"mkhakumod: error: {message}", file=sys.stderr)
    raise SystemExit(1)


def c_string(blob, offset, what):
    if offset >= len(blob):
        fail(f"{what} name offset {offset} is outside its string table")
    end = blob.find(b"\0", offset)
    if end < 0:
        fail(f"{what} name at {offset} is not NUL-terminated")
    return blob[offset:end].decode("ascii", errors="replace")


def read_sections(data):
    if len(data) < 52:
        fail("not an ELF file (truncated header)")
    if data[:4] != b"\x7fELF":
        fail("not an ELF file")
    if data[4] != 1 or data[5] != 1:
        fail("expected a 32-bit little-endian object")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_type != 1:
        fail("expected an ET_REL object (NASM -f elf32)")
    if e_machine != 3:
        fail("expected EM_386")
    e_shoff, = struct.unpack_from("<I", data, 32)
    e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = \
        struct.unpack_from("<HHHHHH", data, 40)

    if e_shoff == 0 or e_shnum == 0:
        fail("the object has no section table")
    if e_shentsize < 40:
        fail("unsupported section header size")
    if e_shoff > len(data) or e_shnum > (len(data) - e_shoff) // e_shentsize:
        fail("section table is outside the file")

    raw = []
    for index in range(e_shnum):
        offset = e_shoff + index * e_shentsize
        (name, kind, flags, addr, file_offset, size, link, info, align,
         entsize) = struct.unpack_from("<IIIIIIIIII", data, offset)
        raw.append({
            "name": name,
            "type": kind,
            "flags": flags,
            "addr": addr,
            "offset": file_offset,
            "size": size,
            "link": link,
            "info": info,
            "align": align,
            "entsize": entsize,
        })

    if e_shstrndx >= len(raw):
        fail("section-name string table index is invalid")
    names = raw[e_shstrndx]
    if names["offset"] > len(data) or names["size"] > len(data) - names["offset"]:
        fail("section-name string table is outside the file")
    names_blob = data[names["offset"]:names["offset"] + names["size"]]
    for section in raw:
        section["name"] = c_string(names_blob, section["name"], "section")

    for index, section in enumerate(raw):
        if section["type"] in (SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB,
                               SHT_REL, SHT_RELA):
            if section["offset"] > len(data) or \
                    section["size"] > len(data) - section["offset"]:
                fail(f"section {section['name']} is outside the file")
        if section["type"] == SHT_RELA:
            fail("RELA relocations are not supported; NASM emits REL")
    return raw, data


def section_symbols(data, sections, symtab):
    """Return [(name, value, shndx)] for one symbol table."""
    strtab = sections[symtab["link"]] if symtab["link"] < len(sections) else None
    if not strtab or strtab["type"] != SHT_STRTAB:
        fail(f"symbol table {symtab['name']} has no string table")
    blob = data[strtab["offset"]:strtab["offset"] + strtab["size"]]
    count = symtab["size"] // 16
    symbols = []
    for index in range(count):
        offset = symtab["offset"] + index * 16
        st_name, st_value, st_size, st_info, st_other, st_shndx = \
            struct.unpack_from("<IIIBBH", data, offset)
        name = c_string(blob, st_name, "symbol") if st_name else ""
        symbols.append((name, st_value, st_shndx))
    return symbols


def main(argv):
    parser = argparse.ArgumentParser(
        description="Pack an ELF32 relocatable object into a Haku module.")
    parser.add_argument("input", help="NASM -f elf32 object")
    parser.add_argument("output", help="output .hkm file")
    parser.add_argument("--name", required=True,
                        help="module name used by delete_module (max 23 chars)")
    args = parser.parse_args(argv)

    name = args.name.encode("ascii", errors="strict")
    if not name or len(name) > NAME_MAX - 1:
        fail(f"module name must be 1..{NAME_MAX - 1} ASCII bytes")
    if b"\0" in name:
        fail("module name must not contain NUL")

    try:
        data = open(args.input, "rb").read()
    except OSError as error:
        fail(f"cannot read {args.input}: {error}")
    sections, data = read_sections(data)

    # Lay out every allocatable section back to back. The order is the
    # object's section order, which is what NASM writes; each section keeps
    # its own alignment. NOBITS sections are placed after the copied image
    # and only contribute to the zeroed BSS size.
    image = bytearray()
    section_offset = {}
    for index, section in enumerate(sections):
        if section["type"] != SHT_PROGBITS or not (section["flags"] & SHF_ALLOC):
            continue
        align = max(1, section["align"])
        image += b"\0" * ((-len(image)) % align)
        section_offset[index] = len(image)
        content = data[section["offset"]:section["offset"] + section["size"]]
        image += content

    image_size = len(image)
    if image_size > MAX_IMAGE:
        fail(f"image is {image_size} bytes, limit is {MAX_IMAGE}")

    bss_end = 0
    for index, section in enumerate(sections):
        if section["type"] != SHT_NOBITS or not (section["flags"] & SHF_ALLOC):
            continue
        align = max(1, section["align"])
        aligned = bss_end + ((-bss_end) % align)
        section_offset[index] = image_size + aligned
        bss_end = aligned + section["size"]
    bss_size = bss_end
    if bss_size > MAX_BSS:
        fail(f"BSS is {bss_size} bytes, limit is {MAX_BSS}")

    def symbol_offset(wanted):
        for symtab in sections:
            if symtab["type"] != SHT_SYMTAB:
                continue
            for sym_name, value, shndx in section_symbols(data, sections, symtab):
                if sym_name != wanted:
                    continue
                if shndx not in section_offset:
                    fail(f"symbol {wanted} is not in an allocatable section")
                return section_offset[shndx] + value
        return NO_OFFSET

    entry = symbol_offset("haku_module_init")
    if entry == NO_OFFSET:
        fail("the object does not define haku_module_init")
    if entry >= image_size:
        fail("haku_module_init is outside the copied image")
    exit_offset = symbol_offset("haku_module_exit")

    relocations = []
    strings = bytearray()
    string_names = {}

    def string_offset(text):
        if text in string_names:
            return string_names[text]
        offset = len(strings)
        strings.extend(text.encode("ascii", errors="replace") + b"\0")
        if len(strings) > MAX_STRINGS:
            fail(f"import names exceed {MAX_STRINGS} bytes")
        string_names[text] = offset
        return offset

    total = image_size + bss_size
    for reloc_section in sections:
        if reloc_section["type"] != SHT_REL:
            continue
        target = reloc_section["info"]
        if target not in section_offset:
            continue  # a relocation against a discarded section
        symtab = sections[reloc_section["link"]] \
            if reloc_section["link"] < len(sections) else None
        if not symtab or symtab["type"] != SHT_SYMTAB:
            fail(f"relocation section {reloc_section['name']} has no symbol table")
        symbols = section_symbols(data, sections, symtab)
        count = reloc_section["size"] // 8
        for index in range(count):
            offset = reloc_section["offset"] + index * 8
            r_offset, r_info = struct.unpack_from("<II", data, offset)
            kind = r_info & 0xFF
            sym_index = r_info >> 8
            if kind not in (R_386_32, R_386_PC32):
                fail(f"unsupported relocation type {kind} in "
                     f"{reloc_section['name']}")
            if sym_index >= len(symbols):
                fail("relocation names a symbol past the symbol table")
            location = section_offset[target] + r_offset
            if location > total or total - location < 4:
                fail("relocation target is outside the module image")
            if location < image_size:
                addend, = struct.unpack_from("<i", image, location)
                struct.pack_into("<I", image, location, 0)
            else:
                addend = 0
            sym_name, sym_value, sym_shndx = symbols[sym_index]
            if sym_shndx == SHN_UNDEF:
                if not sym_name:
                    fail("relocation names an empty undefined symbol")
                symbol = string_offset(sym_name)
            elif sym_shndx in section_offset:
                symbol = SYM_BASE
                addend += section_offset[sym_shndx] + sym_value
            elif sym_shndx == SHN_ABS:
                fail(f"absolute symbol {sym_name} is not supported")
            else:
                fail(f"symbol {sym_name} is not in an allocatable section")
            relocations.append((location, symbol, kind, addend))

    if len(relocations) > MAX_RELOCS:
        fail(f"{len(relocations)} relocations, limit is {MAX_RELOCS}")

    reloc_offset = HEADER_SIZE + image_size
    string_offset_base = reloc_offset + 16 * len(relocations)
    header = MAGIC + struct.pack(
        "<IIIIIIIIII", VERSION, image_size, bss_size, entry, exit_offset,
        len(relocations), reloc_offset, len(strings), string_offset_base,
        len(name) + 1)
    header += name.ljust(NAME_MAX, b"\0")
    if len(header) != HEADER_SIZE:
        fail("internal error: header size")

    payload = bytearray(header)
    payload += image
    for location, symbol, kind, addend in relocations:
        payload += struct.pack("<IIIi", location, symbol, kind, addend)
    payload += strings

    try:
        with open(args.output, "wb") as handle:
            handle.write(payload)
    except OSError as error:
        fail(f"cannot write {args.output}: {error}")

    print(f"mkhakumod: {args.output} ready ({len(payload)} bytes, "
          f"name={args.name}, image={image_size}, bss={bss_size}, "
          f"relocs={len(relocations)}, imports={len(string_names)})")


if __name__ == "__main__":
    main(sys.argv[1:])

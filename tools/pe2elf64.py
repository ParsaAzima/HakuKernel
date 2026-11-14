#!/usr/bin/env python3
"""Convert a fixed-base PE32+ image into the ELF64 layout Haku loads.

Usage:
    pe2elf64.py input.exe output.elf

The x86-64 counterpart of pe2elf.py. MSVC (or clang-cl) emits a PE32+ file
with page-aligned sections; this tool keeps the loadable ones, drops the
discardable sections, and writes an ET_EXEC / EM_X86_64 executable whose
program headers describe the sections.

Rules, matching kernel/arch/x86_64/elf/elf64.c:

  * only readable sections become PT_LOAD segments;
  * every segment is page aligned, and its file offset is congruent with its
    virtual address modulo the page size;
  * a section that is both writable and executable is refused, because the
    loader refuses that too;
  * one executable segment must contain the entry point;
  * a PE TLS directory becomes a PT_TLS segment pointing into the section
    that already holds the template, so a loader can find it through the
    program headers instead of a fixed address.

The first program header is a read-only PT_LOAD for the ELF header and the
program header table at the image base. That gives the kernel a mapped copy
of the table to report as AT_PHDR, which is what the startup code walks to
find PT_TLS.

The image loads at the PE ImageBase, which the build fixes at
0x0000008000400000 with /BASE and /FIXED. Exit codes: 0 = ok, 1 = usage,
2 = input not usable.
"""

import struct
import sys

PT_LOAD = 1
PT_TLS = 7
PF_X = 1
PF_W = 2
PF_R = 4

PAGE = 0x1000
EHDR = struct.Struct("<16sHHIQQQIHHHHHH")
PHDR = struct.Struct("<IIQQQQQQ")

IMAGE_SCN_MEM_DISCARDABLE = 0x02000000
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_READ = 0x40000000
IMAGE_SCN_MEM_WRITE = 0x80000000

ET_EXEC = 2
EM_X86_64 = 62
EV_CURRENT = 1
ELFCLASS64 = 2
ELFDATA2LSB = 1


def die(message):
    sys.exit(f"pe2elf64: {message}")


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def unpack_from(fmt, data, offset):
    try:
        return struct.unpack_from(fmt, data, offset)
    except struct.error:
        die("truncated PE image")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    source_path, output_path = sys.argv[1], sys.argv[2]
    with open(source_path, "rb") as source:
        image = source.read()

    if image[:2] != b"MZ":
        die("missing MZ header")
    pe_offset = unpack_from("<I", image, 0x3C)[0]
    if image[pe_offset:pe_offset + 4] != b"PE\0\0":
        die("missing PE signature")
    coff = pe_offset + 4
    machine, section_count = unpack_from("<HH", image, coff)
    optional_size = unpack_from("<H", image, coff + 16)[0]
    optional = coff + 20
    if machine != 0x8664:
        die(f"machine 0x{machine:04X} is not x86-64")
    if unpack_from("<H", image, optional)[0] != 0x20B:
        die("not a PE32+ image")
    entry_rva = unpack_from("<I", image, optional + 16)[0]
    image_base = unpack_from("<Q", image, optional + 24)[0]

    section_table = []
    segments = []
    table = optional + optional_size
    for index in range(section_count):
        header = table + index * 40
        name = image[header:header + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size, virtual_address, raw_size, raw_pointer = \
            unpack_from("<IIII", image, header + 8)
        characteristics = unpack_from("<I", image, header + 36)[0]
        section_table.append((name, virtual_address,
                              max(virtual_size, raw_size), raw_pointer,
                              raw_size))
        if characteristics & IMAGE_SCN_MEM_DISCARDABLE:
            continue
        if not characteristics & IMAGE_SCN_MEM_READ:
            continue
        if virtual_size == 0 and raw_size == 0:
            continue
        if raw_size and raw_pointer + raw_size > len(image):
            die(f"section {name} is outside the image")
        memory_size = max(virtual_size, raw_size)
        address = virtual_address + image_base
        if address & (PAGE - 1):
            die("sections must be page aligned (/ALIGN:4096)")
        flags = PF_R
        if characteristics & IMAGE_SCN_MEM_WRITE:
            flags |= PF_W
        if characteristics & IMAGE_SCN_MEM_EXECUTE:
            flags |= PF_X
        if (flags & (PF_W | PF_X)) == (PF_W | PF_X):
            die(f"section {name} is writable and executable")
        segments.append({
            "name": name,
            "vaddr": address,
            "filesz": raw_size,
            "memsz": memory_size,
            "flags": flags,
            "data": image[raw_pointer:raw_pointer + raw_size],
        })

    def rva_to_file(rva):
        for _, address, span, raw_pointer, raw_size in section_table:
            if address <= rva < address + span and rva - address < raw_size:
                return raw_pointer + (rva - address)
        return None

    segments.sort(key=lambda segment: segment["vaddr"])
    if not segments:
        die("no loadable sections")
    for index in range(1, len(segments)):
        previous = segments[index - 1]
        previous_end = align_up(previous["vaddr"] + previous["memsz"], PAGE)
        if previous_end > segments[index]["vaddr"]:
            die("section pages overlap")

    entry = image_base + entry_rva
    if not any(segment["flags"] & PF_X and
               segment["vaddr"] <= entry < segment["vaddr"] + segment["memsz"]
               for segment in segments):
        die("entry point is not inside an executable section")

    # A PE TLS directory describes the template the linker built from
    # __declspec(thread) data: [StartAddressOfRawData, EndAddressOfRawData)
    # plus SizeOfZeroFill tail bytes. Emit it as PT_TLS; the kernel loader
    # maps the bytes as part of their normal section, and the startup code
    # copies them into the thread's block. The template has to be backed by
    # file bytes because the startup code memcpy's it.
    tls = None
    number_of_directories = unpack_from("<I", image, optional + 108)[0]
    if number_of_directories > 9:
        tls_rva, tls_size = unpack_from("<II", image, optional + 112 + 9 * 8)
        if tls_rva and tls_size >= 40:
            offset = rva_to_file(tls_rva)
            if offset is None or offset + 40 > len(image):
                die("the TLS directory is not inside a raw section")
            start, end, index_address, callbacks, zero_fill, characteristics = \
                struct.unpack_from("<QQQQII", image, offset)
            if start and end > start:
                if not any(segment["vaddr"] <= start and
                           end <= segment["vaddr"] + segment["filesz"]
                           for segment in segments):
                    die("the TLS template is not inside a loadable section")
                tls = {
                    "vaddr": start,
                    "filesz": end - start,
                    "memsz": end - start + zero_fill,
                    "align": 8,
                    "offset": 0,
                }

    # The loader's AT_PHDR has to point at a mapped copy of the phdr table,
    # so the first program header is a read-only PT_LOAD for the ELF header
    # and the table at the image base. The output file still starts the
    # sections at the next page, which is why the header segment's memory
    # size has to stay below one page.
    phnum = len(segments) + 1 + (1 if tls else 0)
    header_size = EHDR.size + phnum * PHDR.size
    header_segment = {
        "name": "<headers>",
        "vaddr": image_base,
        "filesz": header_size,
        "memsz": header_size,
        "flags": PF_R,
        "data": None,
    }
    if segments and align_up(image_base + header_size, PAGE) > segments[0]["vaddr"]:
        die("the mapped ELF headers overlap the first section")
    segments.insert(0, header_segment)

    file_offset = 0
    for index, segment in enumerate(segments):
        if index == 0:
            segment["offset"] = 0
            file_offset = header_size
        else:
            file_offset = align_up(file_offset, PAGE)
            segment["offset"] = file_offset
            file_offset += segment["filesz"]
    output_size = max(align_up(file_offset, PAGE), PAGE)
    output = bytearray(output_size)

    if tls:
        for segment in segments[1:]:
            if (segment["vaddr"] <= tls["vaddr"] and
                    tls["vaddr"] + tls["filesz"] <=
                    segment["vaddr"] + segment["filesz"]):
                tls["offset"] = segment["offset"] + \
                    (tls["vaddr"] - segment["vaddr"])
                break

    ident = bytearray(16)
    ident[0:4] = b"\x7fELF"
    ident[4] = ELFCLASS64
    ident[5] = ELFDATA2LSB
    ident[6] = EV_CURRENT
    ident[7] = 0                                  # System V ABI
    ident[8] = 0                                  # ABI version
    header = EHDR.pack(bytes(ident), ET_EXEC, EM_X86_64, EV_CURRENT, entry,
                       EHDR.size, 0, 0, EHDR.size, PHDR.size, phnum,
                       0, 0, 0)
    phdr_bytes = bytearray()
    for segment in segments:
        phdr_bytes += PHDR.pack(PT_LOAD, segment["flags"], segment["offset"],
                                segment["vaddr"], segment["vaddr"],
                                segment["filesz"], segment["memsz"], PAGE)
    if tls:
        phdr_bytes += PHDR.pack(PT_TLS, PF_R, tls["offset"], tls["vaddr"],
                                tls["vaddr"], tls["filesz"], tls["memsz"],
                                tls["align"])
    header_segment["data"] = header + bytes(phdr_bytes)
    output[:EHDR.size] = header
    start = EHDR.size
    output[start:start + len(phdr_bytes)] = phdr_bytes
    for segment in segments:
        output[segment["offset"]:segment["offset"] + segment["filesz"]] = \
            segment["data"]

    with open(output_path, "wb") as destination:
        destination.write(output)
    print(f"ELF64: entry=0x{entry:016X} segments={len(segments)} "
          f"size={len(output)}")
    for segment in segments:
        access = ("R" if segment["flags"] & PF_R else "-") + \
                 ("W" if segment["flags"] & PF_W else "-") + \
                 ("X" if segment["flags"] & PF_X else "-")
        print(f"  {segment['vaddr']:#14x} filesz {segment['filesz']:6d} "
              f"memsz {segment['memsz']:6d} {access} {segment['name']}")
    if tls:
        print(f"  PT_TLS {tls['vaddr']:#14x} filesz {tls['filesz']:6d} "
              f"memsz {tls['memsz']:6d} align {tls['align']}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""mkelf64.py — wrap a flat x86-64 binary in a minimal static ELF64 image.

Usage:
    mkelf64.py code.bin out.elf [--base 0x8000400000] [--data-size 0x1000]

The output is an ET_EXEC / EM_X86_64 executable with two PT_LOAD segments:

    0x1000  code  R+X  the whole flat binary, memsz rounded up to a page
    base+0x1000  BSS  R+W  filesz 0, memsz --data-size

Both are page aligned and congruent, which is what the kernel loader checks
before it maps anything. The image has no section table and no dynamic
linking, matching the loader's supported subset.

Because the code segment is placed at a file offset of 0x1000 and the BSS
segment one page above the load base, the flat binary has to fit in one page.
That keeps this tool small; a larger test program would use more segments.

Exit codes: 0 = ok, 1 = usage error, 2 = input not usable.
"""

import struct
import sys

PAGE = 0x1000
CODE_OFFSET = 0x1000
DEFAULT_BASE = 0x0000008000400000

ELFCLASS64 = 2
ELFDATA2LSB = 1
EV_CURRENT = 1
ET_EXEC = 2
EM_X86_64 = 62
PT_LOAD = 1
PF_X = 1
PF_W = 2
PF_R = 4

EHDR = struct.Struct("<16sHHIQQQIHHHHHH")
PHDR = struct.Struct("<IIQQQQQQ")


def die(msg, code=2):
    print(f"mkelf64: error: {msg}", file=sys.stderr)
    sys.exit(code)


def usage():
    print(__doc__)
    sys.exit(1)


def parse_int(text, name):
    try:
        value = int(text, 0)
    except ValueError:
        die(f"{name} is not an integer: {text}")
    if value < 0:
        die(f"{name} must not be negative: {text}")
    return value


def parse_args(argv):
    if len(argv) < 3:
        usage()
    code_path, out_path = argv[1], argv[2]
    base = DEFAULT_BASE
    data_size = PAGE
    for arg in argv[3:]:
        if arg.startswith("--base="):
            base = parse_int(arg.split("=", 1)[1], "--base")
        elif arg.startswith("--data-size="):
            data_size = parse_int(arg.split("=", 1)[1], "--data-size")
        else:
            die(f"unknown argument: {arg}", 1)
    return code_path, out_path, base, data_size


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def main():
    code_path, out_path, base, data_size = parse_args(sys.argv)
    with open(code_path, "rb") as f:
        code = f.read()

    if not code:
        die("empty code image")
    if base == 0 or base % PAGE:
        die(f"load base {base:#x} must be page aligned and non-zero")
    if data_size == 0 or data_size % PAGE:
        die(f"data size {data_size:#x} must be a non-zero page multiple")
    if len(code) > PAGE:
        die(f"code is {len(code)} bytes; the BSS segment starts at "
            f"base+{PAGE:#x}, so the code segment is limited to {PAGE} bytes")

    data_vaddr = base + PAGE
    code_memsz = align_up(len(code), PAGE)

    ident = bytearray(16)
    ident[0:4] = b"\x7fELF"
    ident[4] = ELFCLASS64
    ident[5] = ELFDATA2LSB
    ident[6] = EV_CURRENT
    ident[7] = 0                     # System V ABI
    ident[8] = 0                     # ABI version

    header = EHDR.pack(
        bytes(ident),
        ET_EXEC,
        EM_X86_64,
        EV_CURRENT,
        base,                        # e_entry
        EHDR.size,                   # e_phoff
        0,                           # e_shoff: no section table
        0,                           # e_flags
        EHDR.size,                   # e_ehsize
        PHDR.size,                   # e_phentsize
        2,                           # e_phnum
        0, 0, 0)                     # no section headers

    code_phdr = PHDR.pack(
        PT_LOAD,
        PF_R | PF_X,
        CODE_OFFSET,
        base,
        base,
        len(code),
        code_memsz,
        PAGE)
    data_phdr = PHDR.pack(
        PT_LOAD,
        PF_R | PF_W,
        0,                           # never read: filesz is zero
        data_vaddr,
        data_vaddr,
        0,
        data_size,
        PAGE)

    image = bytearray(CODE_OFFSET + len(code))
    image[0:EHDR.size] = header
    image[EHDR.size:EHDR.size + PHDR.size] = code_phdr
    image[EHDR.size + PHDR.size:EHDR.size + 2 * PHDR.size] = data_phdr
    image[CODE_OFFSET:CODE_OFFSET + len(code)] = code

    with open(out_path, "wb") as f:
        f.write(image)

    print(f"mkelf64: {code_path} -> {out_path} ({len(image)} bytes, "
          f"entry={base:#x})")
    print(f"  {base:#12x}  memsz {code_memsz:6d}  R+X  code")
    print(f"  {data_vaddr:#12x}  memsz {data_size:6d}  R+W  bss (filesz 0)")


if __name__ == "__main__":
    main()

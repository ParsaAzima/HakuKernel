#!/usr/bin/env python3
"""Create a minimal FAT12 superfloppy image with root-directory files.

Usage:
    mkfat12.py input.elf output.img [NAME.EXT=FILE ...]
    mkfat12.py --empty output.img NAME.EXT=FILE ...

The first form always adds the input ELF as HAKU.ELF (the 32-bit boot
volume). `--empty` skips that and writes only the named files, which is how
the x86-64 seed disk is built.
"""

import pathlib
import re
import struct
import sys

SECTOR_SIZE = 512
TOTAL_SECTORS = 2880
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 1
FAT_COUNT = 2
ROOT_ENTRIES = 224
SECTORS_PER_FAT = 9
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR_SIZE
FAT_START = RESERVED_SECTORS
ROOT_START = FAT_START + FAT_COUNT * SECTORS_PER_FAT
DATA_START = ROOT_START + ROOT_SECTORS
CLUSTER_COUNT = TOTAL_SECTORS - DATA_START


def fat12_set(fat, cluster, value):
    offset = cluster + cluster // 2
    value &= 0x0FFF
    if cluster & 1:
        fat[offset] = (fat[offset] & 0x0F) | ((value << 4) & 0xF0)
        fat[offset + 1] = (value >> 4) & 0xFF
    else:
        fat[offset] = value & 0xFF
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F)


def short_name(name):
    allowed = r"A-Za-z0-9_$~!#%&'()@^`{}-"
    match = re.fullmatch(
        rf"([{allowed}]+)(?:\.([{allowed}]+))?", name)
    if not match:
        raise ValueError(f"invalid 8.3 filename: {name}")
    base, extension = match.groups()
    if len(base) > 8 or len(extension or "") > 3:
        raise ValueError(f"filename is not 8.3: {name}")
    return (base.upper().ljust(8) + (extension or "").upper().ljust(3)).encode("ascii")


def main():
    args = sys.argv[1:]
    # --empty skips the implicit HAKU.ELF first file, which is what the
    # 64-bit port's seed volume wants: an image with only the named files.
    empty = bool(args) and args[0] == "--empty"
    if empty:
        args = args[1:]
        if not args:
            sys.exit("usage: mkfat12.py --empty output.img "
                     "NAME.EXT=FILE ...")
        image_path = pathlib.Path(args[0])
        files = []
        specifications = args[1:]
    else:
        if len(args) < 2:
            sys.exit("usage: mkfat12.py input.elf output.img "
                     "[NAME.EXT=FILE ...]")
        image_path = pathlib.Path(args[1])
        files = [("HAKU.ELF", pathlib.Path(args[0]).read_bytes())]
        specifications = args[2:]

    names = {short_name(files[0][0])} if files else set()
    for specification in specifications:
        if "=" not in specification:
            sys.exit(f"mkfat12: expected NAME.EXT=FILE, got {specification!r}")
        name, source = specification.split("=", 1)
        try:
            encoded_name = short_name(name)
        except ValueError as error:
            sys.exit(f"mkfat12: {error}")
        if encoded_name in names:
            sys.exit(f"mkfat12: duplicate FAT filename {name}")
        names.add(encoded_name)
        files.append((name, pathlib.Path(source).read_bytes()))
    if not files:
        sys.exit("mkfat12: an image needs at least one file")
    if any(not payload for _, payload in files):
        sys.exit("mkfat12: refusing to create an empty file")

    total_clusters = sum(
        (len(payload) + SECTOR_SIZE - 1) // SECTOR_SIZE
        for _, payload in files)
    if total_clusters > CLUSTER_COUNT:
        sys.exit("mkfat12: payload does not fit on the FAT12 volume")
    if len(files) > ROOT_ENTRIES:
        sys.exit("mkfat12: too many files for FAT12 root directory")

    image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)
    boot = bytearray(SECTOR_SIZE)
    boot[0:3] = b"\xEB\x3C\x90"
    boot[3:11] = b"HAKUKERN"
    struct.pack_into("<HBHBHHBHHHII", boot, 11,
                     SECTOR_SIZE, SECTORS_PER_CLUSTER, RESERVED_SECTORS,
                     FAT_COUNT, ROOT_ENTRIES, TOTAL_SECTORS, 0xF0,
                     SECTORS_PER_FAT, 18, 2, 0, 0)
    boot[36:39] = b"\x00\x00\x00"
    boot[39:43] = b"HAKU"
    boot[43:54] = b"FAT12 VOL  "
    boot[54:62] = b"FAT12   "
    boot[510:512] = b"\x55\xAA"
    image[:SECTOR_SIZE] = boot

    fat = bytearray(SECTORS_PER_FAT * SECTOR_SIZE)
    fat[0:3] = b"\xF0\xFF\xFF"
    next_cluster = 2
    root = ROOT_START * SECTOR_SIZE
    for file_index, (name, payload) in enumerate(files):
        cluster_count = (len(payload) + SECTOR_SIZE - 1) // SECTOR_SIZE
        first_cluster = next_cluster
        for index in range(cluster_count):
            current = next_cluster
            following = 0x0FFF if index == cluster_count - 1 else current + 1
            fat12_set(fat, current, following)
            source = index * SECTOR_SIZE
            destination = (DATA_START + current - 2) * SECTOR_SIZE
            image[destination:destination + SECTOR_SIZE] = \
                payload[source:source + SECTOR_SIZE].ljust(SECTOR_SIZE, b"\0")
            next_cluster += 1

        entry = root + file_index * 32
        image[entry:entry + 11] = short_name(name)
        image[entry + 11] = 0x20
        struct.pack_into("<H", image, entry + 26, first_cluster)
        struct.pack_into("<I", image, entry + 28, len(payload))

    for index in range(FAT_COUNT):
        start = (FAT_START + index * SECTORS_PER_FAT) * SECTOR_SIZE
        image[start:start + len(fat)] = fat

    image_path.write_bytes(image)
    summary = ", ".join(
        f"{name}={len(payload)} bytes"
        for name, payload in files)
    print(f"mkfat12: {image_path} ready ({summary}, {TOTAL_SECTORS} sectors)")


if __name__ == "__main__":
    main()

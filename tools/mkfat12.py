#!/usr/bin/env python3
"""Create a minimal FAT12 superfloppy image from files and directories.

Usage:
    mkfat12.py input.elf output.img [NAME.EXT=FILE ...] [--dir=NAME ...]
    mkfat12.py --empty output.img NAME.EXT=FILE ... [--dir=NAME ...]

The first form always adds the input ELF as HAKU.ELF (the 32-bit boot
volume). `--empty` skips that and writes only the named entries, which is how
the x86-64 seed disk is built.

A NAME may contain '/' to place the file in a subdirectory
(`BIN/BUSYBOX=build/busybox`); missing parents are created, and `--dir=` adds
an empty one (`--dir=/TMP`). Directories are single-cluster (16 entries
including "." and ".."), which is all a boot volume needs; a full directory
is an error rather than a silent truncation.
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

DIR_ENTRIES = SECTOR_SIZE // 32          # one cluster per directory
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
EOC = 0x0FFF
DOT = b".          "
DOTDOT = b"..         "


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


def entry_bytes(encoded, attr, cluster, size):
    entry = bytearray(32)
    entry[0:11] = encoded
    entry[11] = attr
    struct.pack_into("<H", entry, 26, cluster)
    struct.pack_into("<I", entry, 28, size)
    return entry


class Directory:
    """One directory: its parent, and the files and directories it holds."""

    def __init__(self, name, parent):
        self.name = name
        self.parent = parent
        self.entries = []            # (kind, component, payload/Directory)
        self.taken = set()           # encoded 11-byte names, for duplicates
        self.cluster = 0
        self.encoded = b""

    def add(self, kind, component, payload):
        encoded = short_name(component)
        if encoded in self.taken:
            raise ValueError(f"duplicate FAT name {component!r}")
        self.taken.add(encoded)
        self.entries.append((kind, component, payload))


def build_tree(files, directory_names):
    """Return (root, {path: Directory}) with every named path materialised."""
    root = Directory("", None)
    directories = {"": root}

    def resolve(path, create):
        current = root
        prefix = ""
        for component in path.strip("/").split("/"):
            if not component:
                raise ValueError(f"empty path component in {path!r}")
            prefix = f"{prefix}/{component}" if prefix else component
            child = directories.get(prefix)
            if child is None:
                if not create:
                    raise ValueError(f"no such directory: {prefix}")
                child = Directory(component, current)
                current.add("dir", component, child)
                directories[prefix] = child
            current = child
        return current

    for path in directory_names:
        try:
            resolve(path, True)
        except ValueError as error:
            raise ValueError(f"{path}: {error}") from None
    for name, payload in files:
        try:
            if "/" in name:
                parent_name, _, component = name.rpartition("/")
                parent = resolve(parent_name, True)
            else:
                parent, component = root, name
            parent.add("file", component, payload)
        except ValueError as error:
            raise ValueError(f"{name}: {error}") from None
    return root, directories


def walk_files(directory):
    """Depth-first (name, payload) over every file in the tree."""
    for kind, component, payload in directory.entries:
        if kind == "file":
            yield component, payload
        else:
            yield from walk_files(payload)


def main():
    args = sys.argv[1:]
    # --empty skips the implicit HAKU.ELF first file, which is what the
    # 64-bit port's seed volume wants: an image with only the named files.
    empty = bool(args) and args[0] == "--empty"
    if empty:
        args = args[1:]
        if not args:
            sys.exit("usage: mkfat12.py --empty output.img "
                     "NAME.EXT=FILE ... [--dir=NAME ...]")
        image_path = pathlib.Path(args[0])
        files = []
        specifications = args[1:]
    else:
        if len(args) < 2:
            sys.exit("usage: mkfat12.py input.elf output.img "
                     "[NAME.EXT=FILE ...] [--dir=NAME ...]")
        image_path = pathlib.Path(args[1])
        files = [("HAKU.ELF", pathlib.Path(args[0]).read_bytes())]
        specifications = args[2:]

    directory_names = []
    for specification in specifications:
        if specification.startswith("--dir="):
            directory_names.append(specification[len("--dir="):])
            continue
        if "=" not in specification:
            sys.exit(f"mkfat12: expected NAME.EXT=FILE, got {specification!r}")
        name, source = specification.split("=", 1)
        try:
            payload = pathlib.Path(source).read_bytes()
        except OSError as error:
            sys.exit(f"mkfat12: cannot read {source}: {error}")
        if not payload:
            sys.exit(f"mkfat12: refusing to create an empty file ({name})")
        files.append((name, payload))
    if not files and not directory_names:
        sys.exit("mkfat12: an image needs at least one entry")

    try:
        root, directories = build_tree(files, directory_names)
    except ValueError as error:
        sys.exit(f"mkfat12: {error}")

    # FAT12 stores a directory as a cluster chain; every directory here is one
    # cluster, so it can hold DIR_ENTRIES slots minus the "." and ".." links.
    for path, directory in directories.items():
        limits = ROOT_ENTRIES if directory is root else DIR_ENTRIES - 2
        if len(directory.entries) > limits:
            where = path or "the root directory"
            sys.exit(f"mkfat12: {where} needs {len(directory.entries)} "
                     f"entries, the format allows {limits}")

    file_list = list(walk_files(root))
    file_clusters = sum(
        (len(payload) + SECTOR_SIZE - 1) // SECTOR_SIZE
        for _, payload in file_list)
    directory_clusters = len(directories) - 1      # the root has no cluster
    if file_clusters + directory_clusters > CLUSTER_COUNT:
        sys.exit("mkfat12: payload does not fit on the FAT12 volume")

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
    first_cluster = {}

    def write_cluster(cluster, offset, payload):
        destination = (DATA_START + cluster - 2) * SECTOR_SIZE + offset
        image[destination:destination + len(payload)] = payload

    # Files first, in tree order, so cluster numbers are stable across runs
    # and a directory walk sees the same layout every build.
    for name, payload in file_list:
        count = (len(payload) + SECTOR_SIZE - 1) // SECTOR_SIZE
        # Keyed by object identity: two directories may hold a same-named
        # file, and only the payload instance identifies one entry.
        first_cluster[id(payload)] = next_cluster
        for index in range(count):
            fat12_set(fat, next_cluster,
                      EOC if index == count - 1 else next_cluster + 1)
            write_cluster(next_cluster, 0,
                          payload[index * SECTOR_SIZE:(index + 1) * SECTOR_SIZE]
                          .ljust(SECTOR_SIZE, b"\0"))
            next_cluster += 1

    # Directories get one cluster each; "." points at it, ".." at the parent.
    for path, directory in directories.items():
        if directory is root:
            continue
        directory.cluster = next_cluster
        fat12_set(fat, next_cluster, EOC)
        next_cluster += 1

    def dot_entries(directory):
        parent_cluster = 0 if directory.parent is root else directory.parent.cluster
        return [entry_bytes(DOT, ATTR_DIRECTORY, directory.cluster, 0),
                entry_bytes(DOTDOT, ATTR_DIRECTORY, parent_cluster, 0)]

    def child_entry(kind, component, payload):
        if kind == "file":
            return entry_bytes(short_name(component), ATTR_ARCHIVE,
                               first_cluster[id(payload)], len(payload))
        return entry_bytes(short_name(component), ATTR_DIRECTORY,
                           payload.cluster, 0)

    root_offset = ROOT_START * SECTOR_SIZE
    for index, (kind, component, payload) in enumerate(root.entries):
        at = root_offset + index * 32
        image[at:at + 32] = child_entry(kind, component, payload)

    for path, directory in directories.items():
        if directory is root:
            continue
        block = bytearray(SECTOR_SIZE)
        entries = dot_entries(directory) + [
            child_entry(kind, component, payload)
            for kind, component, payload in directory.entries]
        for index, entry in enumerate(entries):
            block[index * 32:(index + 1) * 32] = entry
        write_cluster(directory.cluster, 0, block)

    for index in range(FAT_COUNT):
        start = (FAT_START + index * SECTORS_PER_FAT) * SECTOR_SIZE
        image[start:start + len(fat)] = fat

    image_path.write_bytes(image)
    print(f"mkfat12: {image_path} ready ({len(file_list)} files, "
          f"{len(directories) - 1} directories, {next_cluster - 2} clusters, "
          f"{TOTAL_SECTORS} sectors)")


if __name__ == "__main__":
    main()

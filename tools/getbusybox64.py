#!/usr/bin/env python3
"""Fetch a static musl busybox for the x86-64 image.

The x86-64 port has no musl toolchain in its build: it takes the binary
Alpine ships in `busybox-static` from the x86_64 repository, which is
statically linked against musl and has no interpreter. The package is a
gzipped tar (.apk) holding the binary at bin/busybox.static.

Unlike tools/getbusybox.py (the i386 source), this checker reads the whole
ELF before walking program headers: reading only the first 64 bytes and then
unpacking every phdr would validate a truncated cache file by accident.

The binary is linked at 0x400000 and left that way. Haku gives each user
space a private copy of the kernel's slot-0 page tables so a static Linux
ET_EXEC can keep its absolute addresses and still be mapped at ring 3.

Usage:
    getbusybox64.py [OUTPUT]

Defaults to build/busybox64. The binary is not committed: it is a 1 MiB
artifact owned by Alpine, fetched on first use and cached under build/.

Exit codes: 0 = ready, 1 = could not fetch or the binary is not usable.
"""

import gzip
import io
import os
import struct
import sys
import tarfile
import urllib.request

# Pinned for the same reason as the i386 one: a newer build may start using a
# syscall this kernel does not implement, which would fail for a reason that
# has nothing to do with the tree under test.
PACKAGE_URL = (
    "https://dl-cdn.alpinelinux.org/alpine/v3.21/main/x86_64/"
    "busybox-static-1.37.0-r14.apk"
)
MEMBER = "bin/busybox.static"

EM_X86_64 = 62
PT_LOAD = 1
PT_INTERP = 3


def describe(data):
    """Return (ok, reason) after checking the ELF is a static x86-64 ET_EXEC."""
    if len(data) < 64 or data[:4] != b"\x7fELF":
        return False, "not an ELF file"
    if data[4] != 2:
        return False, "not a 64-bit ELF (e_ident[EI_CLASS] != ELFCLASS64)"
    if data[5] != 1:
        return False, "not little-endian"
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_type != 2:
        return False, f"not ET_EXEC (e_type={e_type})"
    if e_machine != EM_X86_64:
        return False, f"not x86-64 (e_machine={e_machine})"
    phoff, = struct.unpack_from("<Q", data, 32)
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    if phnum == 0 or phentsize < 56:
        return False, f"unusable program headers ({phnum} x {phentsize})"
    if phoff + phnum * phentsize > len(data):
        return False, "program header table lies outside the file"
    loads = 0
    for i in range(phnum):
        p_type, = struct.unpack_from("<I", data, phoff + i * phentsize)
        if p_type == PT_INTERP:
            return False, "needs a dynamic loader (PT_INTERP present)"
        if p_type == PT_LOAD:
            loads += 1
    if not loads:
        return False, "no loadable segments"
    return True, f"static x86-64 ELF ({loads} PT_LOAD segments)"


def fetch():
    print(f"getbusybox64: downloading {PACKAGE_URL}")
    with urllib.request.urlopen(PACKAGE_URL, timeout=120) as response:
        payload = response.read()
    if payload[:2] != b"\x1f\x8b":
        raise ValueError("package is not gzip-compressed")
    archive = tarfile.open(fileobj=io.BytesIO(gzip.decompress(payload)))
    member = archive.getmember(MEMBER)
    return archive.extractfile(member).read()


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "busybox64")
    if os.path.isfile(output) and os.path.getsize(output) > 0:
        with open(output, "rb") as f:
            ok, reason = describe(f.read())
        if ok:
            print(f"getbusybox64: using cached {output}")
            return 0
        print(f"getbusybox64: cached file is unusable ({reason}), refetching")

    try:
        data = fetch()
    except Exception as error:                      # noqa: BLE001 - report and stop
        print(f"getbusybox64: download failed: {error}", file=sys.stderr)
        print("getbusybox64: pass --busybox64=PATH to use a local binary",
              file=sys.stderr)
        return 1

    ok, reason = describe(data)
    if not ok:
        print(f"getbusybox64: unusable binary: {reason}", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "wb") as f:
        f.write(data)
    print(f"getbusybox64: wrote {output} ({len(data)} bytes, {reason})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

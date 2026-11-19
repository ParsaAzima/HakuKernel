#!/usr/bin/env python3
"""Fetch a static musl busybox for the shell image.

Haku's shell target needs an i386 Linux binary that is statically linked
against musl. Building one from source needs a musl toolchain, so this script
takes the binary Alpine ships: `busybox-static` from the x86 repository is
exactly that — musl, static, no interpreter — and the package is a gzipped
tar (an .apk) with the binary at bin/busybox.static.

Usage:
    getbusybox.py [OUTPUT]

Defaults to build/busybox. The file is not committed: it is a 1 MiB binary
that belongs to someone else, so it is fetched on first use and cached.

Exit codes: 0 = ready, 1 = could not fetch or the binary is not usable.
"""

import gzip
import io
import os
import struct
import sys
import tarfile
import urllib.request

# Alpine's x86 (32-bit) repository. The version is pinned: a new build could
# start using a syscall Haku does not implement, and the check would go red
# for a reason that has nothing to do with this tree.
PACKAGE_URL = (
    "https://dl-cdn.alpinelinux.org/alpine/v3.21/main/x86/"
    "busybox-static-1.37.0-r14.apk"
)
MEMBER = "bin/busybox.static"

EM_386 = 3
PT_LOAD = 1
PT_INTERP = 3


def describe(data):
    """Return (ok, reason) after checking the ELF is a static i386 executable."""
    if data[:4] != b"\x7fELF":
        return False, "not an ELF file"
    if data[4] != 1:
        return False, "not a 32-bit ELF (e_ident[EI_CLASS] != ELFCLASS32)"
    if data[5] != 1:
        return False, "not little-endian"
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_type != 2:
        return False, f"not ET_EXEC (e_type={e_type})"
    if e_machine != EM_386:
        return False, f"not i386 (e_machine={e_machine:#x})"
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    for i in range(phnum):
        p_type, = struct.unpack_from("<I", data, phoff + i * phentsize)
        if p_type == PT_INTERP:
            return False, "needs a dynamic loader (PT_INTERP present)"
    if not any(struct.unpack_from("<I", data, phoff + i * phentsize)[0] == PT_LOAD
               for i in range(phnum)):
        return False, "no loadable segments"
    return True, "static i386 ELF"


def fetch():
    print(f"getbusybox: downloading {PACKAGE_URL}")
    with urllib.request.urlopen(PACKAGE_URL, timeout=120) as response:
        payload = response.read()
    if payload[:2] != b"\x1f\x8b":
        raise ValueError("package is not gzip-compressed")
    archive = tarfile.open(fileobj=io.BytesIO(gzip.decompress(payload)))
    member = archive.getmember(MEMBER)
    return archive.extractfile(member).read()


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "busybox")
    if os.path.isfile(output) and os.path.getsize(output) > 0:
        with open(output, "rb") as f:
            ok, reason = describe(f.read(64))
        if ok:
            print(f"getbusybox: using cached {output}")
            return 0
        print(f"getbusybox: cached file is unusable ({reason}), refetching")

    try:
        data = fetch()
    except Exception as error:                      # noqa: BLE001 - report and stop
        print(f"getbusybox: download failed: {error}", file=sys.stderr)
        print("getbusybox: pass --busybox=PATH to use a local binary",
              file=sys.stderr)
        return 1

    ok, reason = describe(data)
    if not ok:
        print(f"getbusybox: unusable binary: {reason}", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "wb") as f:
        f.write(data)
    print(f"getbusybox: wrote {output} ({len(data)} bytes, {reason})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
